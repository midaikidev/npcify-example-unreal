#include "NpcifyImportCharactersCommandlet.h"

#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/RetargetOps/FKChainsOp.h"
#include "Retargeter/RetargetOps/PelvisMotionOp.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Rig/IKRigDefinition.h"
#include "UObject/Package.h"
#include "VrmMetaObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"
#include "VrmAssetListObject.h"
#include "VrmImporterBPFunctionLibrary.h"
#include "VrmUtil.h"

DEFINE_LOG_CATEGORY_STATIC(LogNpcifyImport, Log, All);

namespace
{
	const TCHAR* CharactersRoot = TEXT("/Game/Npcify/Characters");
	// VRMA models, IK rigs and retargeters that are needed only during the import.
	const TCHAR* TemporaryRoot = TEXT("/Game/NpcifyImportTemp");

	FImportOptionData MakeImportOptions()
	{
		FImportOptionData Options;
		Options.init();
		Options.bSkipPhysics = true;
		Options.bSkipRetargeter = false;
		Options.bSkipMorphTarget = false;
		// Same as the defaults of the VRM4U import dialog.
		Options.bVrm10RemoveLocalRotation = false;
		// Creates the humanoid IK rigs that the animation retargeting needs.
		Options.bGenerateRigIK = true;
		return Options;
	}

	template <typename T>
	T* FindAsset(const FString& PackageFolder, const FString& AssetName)
	{
		const FString Path = FString::Printf(TEXT("%s/%s.%s"), *PackageFolder, *AssetName, *AssetName);
		return LoadObject<T>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	UIKRigDefinition* FindHumanoidRig(const UVrmAssetListObject* AssetList)
	{
		const FString Folder = FPaths::GetPath(AssetList->GetPathName());
		return FindAsset<UIKRigDefinition>(Folder, FString::Printf(TEXT("IK_%s_VrmHumanoid"), *AssetList->BaseFileName));
	}

	void SaveObjectPackage(UObject* Object)
	{
		UPackage* Package = Object->GetOutermost();
		const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::SavePackage(Package, Object, *FileName, Args);
	}

	/** FK-only retargeter between two VRM humanoid IK rigs (same chain names). */
	UIKRetargeter* MakeDirectRetargeter(const FString& Folder, UIKRigDefinition* SourceRig, USkeletalMesh* SourceMesh,
		UIKRigDefinition* TargetRig, USkeletalMesh* TargetMesh)
	{
		const FString AssetName = TEXT("RTG_direct");
		UPackage* Package = CreatePackage(*(Folder / AssetName));
		UIKRetargeter* Retargeter = NewObject<UIKRetargeter>(Package, *AssetName, RF_Public | RF_Standalone);
		UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
		if (Controller == nullptr)
		{
			return nullptr;
		}
		Controller->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
		Controller->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);
		Controller->SetPreviewMesh(ERetargetSourceOrTarget::Source, SourceMesh);
		Controller->SetPreviewMesh(ERetargetSourceOrTarget::Target, TargetMesh);
		Controller->RemoveAllOps();
		Controller->AddRetargetOp(FIKRetargetPelvisMotionOp::StaticStruct());
		Controller->AddRetargetOp(FIKRetargetFKChainsOp::StaticStruct());
		Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Source, SourceRig);
		Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Target, TargetRig);
		Controller->AutoMapChains(EAutoMapChainType::Exact, true);
		SaveObjectPackage(Retargeter);
		return Retargeter;
	}


	/** Reads the JSON chunk of a GLB file (VRM). */
	TSharedPtr<FJsonObject> ReadGlbJson(const FString& File)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *File) || Bytes.Num() < 20)
		{
			return nullptr;
		}
		const uint32 JsonLength = *reinterpret_cast<const uint32*>(Bytes.GetData() + 12);
		if (20 + static_cast<int64>(JsonLength) > Bytes.Num())
		{
			return nullptr;
		}
		FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + 20), JsonLength);
		const FString Json(Converter.Length(), Converter.Get());
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
		return Root;
	}

	/**
	 * VRM4U leaves mtoon_Cutoff at 0 for VRM 1.0 materials, so alpha-masked parts
	 * (for example the area around a face texture) stay visible. Copy the glTF alphaCutoff.
	 */
	/**
	 * VRM4U reads no VRMC_node_constraint of a VRM 1.0 file (VrmConvertMetadata.cpp looks for it at the document's
	 * root, not in each node), and its constraint node walks the map in its order, so a constrained bone under a
	 * constrained bone needs its parent first. This fills the rotation constraints from the file's nodes, parents
	 * first, into the character's meta object. Returns the number of constraints.
	 */
	int32 FillRotationConstraints(const FString& VrmFile, UVrmAssetListObject* Character)
	{
		UVrmMetaObject* Meta = Character ? Character->VrmMetaObject : nullptr;
		const TSharedPtr<FJsonObject> Root = ReadGlbJson(VrmFile);
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (Meta == nullptr || Character->SkeletalMesh == nullptr || !Root.IsValid() || !Root->TryGetArrayField(TEXT("nodes"), Nodes))
		{
			return 0;
		}
		const FReferenceSkeleton& Reference = Character->SkeletalMesh->GetRefSkeleton();
		auto BoneName = [&Reference](const FString& Name)
		{
			const FString Plain = Name.Replace(TEXT(":"), TEXT("_"));
			return Reference.FindBoneIndex(FName(*Name)) != INDEX_NONE ? Name
				: (Reference.FindBoneIndex(FName(*Plain)) != INDEX_NONE ? Plain : FString());
		};
		TArray<int32> Parent;
		Parent.Init(INDEX_NONE, Nodes->Num());
		for (int32 Index = 0; Index < Nodes->Num(); ++Index)
		{
			const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
			if ((*Nodes)[Index]->AsObject()->TryGetArrayField(TEXT("children"), Children))
			{
				for (const TSharedPtr<FJsonValue>& Child : *Children)
				{
					Parent[static_cast<int32>(Child->AsNumber())] = Index;
				}
			}
		}
		struct FFound { int32 Depth; FString Name; FVRMConstraint Constraint; };
		TArray<FFound> Found;
		for (int32 Index = 0; Index < Nodes->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Node = (*Nodes)[Index]->AsObject();
			const TSharedPtr<FJsonObject>* Extensions = nullptr;
			const TSharedPtr<FJsonObject>* Ext = nullptr;
			const TSharedPtr<FJsonObject>* Constraint = nullptr;
			const TSharedPtr<FJsonObject>* Rotation = nullptr;
			if (!Node->TryGetObjectField(TEXT("extensions"), Extensions) ||
				!(*Extensions)->TryGetObjectField(TEXT("VRMC_node_constraint"), Ext) ||
				!(*Ext)->TryGetObjectField(TEXT("constraint"), Constraint) ||
				!(*Constraint)->TryGetObjectField(TEXT("rotation"), Rotation))
			{
				continue;
			}
			const int32 Source = static_cast<int32>((*Rotation)->GetNumberField(TEXT("source")));
			double Weight = 1.0;
			(*Rotation)->TryGetNumberField(TEXT("weight"), Weight);
			const FString Name = BoneName(Node->GetStringField(TEXT("name")));
			const FString SourceName = Nodes->IsValidIndex(Source) ? BoneName((*Nodes)[Source]->AsObject()->GetStringField(TEXT("name"))) : FString();
			if (Name.IsEmpty() || SourceName.IsEmpty())
			{
				UE_LOG(LogNpcifyImport, Warning, TEXT("Constraint of node %d: bone not found"), Index);
				continue;
			}
			int32 Depth = 0;
			for (int32 Up = Parent[Index]; Up != INDEX_NONE; Up = Parent[Up])
			{
				++Depth;
			}
			FFound Item{Depth, Name, FVRMConstraint()};
			Item.Constraint.type = EVRMConstraintType::Rotation;
			Item.Constraint.constraintRotation.source = Source;
			Item.Constraint.constraintRotation.sourceName = SourceName;
			Item.Constraint.constraintRotation.weight = static_cast<float>(Weight);
			Found.Add(Item);
		}
		Found.StableSort([](const FFound& A, const FFound& B) { return A.Depth < B.Depth; });
		Meta->VRMConstraintMeta.Empty();
		for (const FFound& Item : Found)
		{
			Meta->VRMConstraintMeta.Add(Item.Name, Item.Constraint);
		}
		Meta->MarkPackageDirty();
		SaveObjectPackage(Meta);
		return Found.Num();
	}

	void ApplyAlphaCutoff(const FString& VrmFile, const FString& CharacterFolder)
	{
		const TSharedPtr<FJsonObject> Root = ReadGlbJson(VrmFile);
		const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
		if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("materials"), Materials))
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Materials)
		{
			const TSharedPtr<FJsonObject> Material = Value->AsObject();
			FString Name;
			FString AlphaMode;
			if (!Material.IsValid() || !Material->TryGetStringField(TEXT("name"), Name) ||
				!Material->TryGetStringField(TEXT("alphaMode"), AlphaMode) || AlphaMode != TEXT("MASK"))
			{
				continue;
			}
			double Cutoff = 0.5;
			Material->TryGetNumberField(TEXT("alphaCutoff"), Cutoff);
			if (UMaterialInstanceConstant* Instance = FindAsset<UMaterialInstanceConstant>(CharacterFolder, TEXT("MI_") + Name))
			{
				Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("mtoon_Cutoff")), static_cast<float>(Cutoff));
				Instance->PostEditChange();
				SaveObjectPackage(Instance);
				UE_LOG(LogNpcifyImport, Display, TEXT("%s: alpha cutoff %.2f"), *Instance->GetName(), Cutoff);
			}
		}
	}
}

UNpcifyImportCharactersCommandlet::UNpcifyImportCharactersCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UNpcifyImportCharactersCommandlet::Main(const FString& Params)
{
	const FString RawRoot = FPaths::Combine(FPaths::ProjectDir(), TEXT("RawCharacters"));
	TArray<FString> Folders;
	FString OnlyFolder;
	if (FParse::Value(*Params, TEXT("Folder="), OnlyFolder))
	{
		Folders.Add(OnlyFolder);
	}
	else
	{
		IFileManager::Get().FindFiles(Folders, *FPaths::Combine(RawRoot, TEXT("*")), false, true);
	}

	int32 Failures = 0;
	for (const FString& Folder : Folders)
	{
		if (!ImportCharacter(Folder))
		{
			++Failures;
		}
	}

	UEditorLoadingAndSavingUtils::SaveDirtyPackages(false, true);
	UE_LOG(LogNpcifyImport, Display, TEXT("Imported %d character(s), %d failure(s). Delete Content/NpcifyImportTemp after the import."),
		Folders.Num() - Failures, Failures);
	return Failures == 0 ? 0 : 1;
}

bool UNpcifyImportCharactersCommandlet::ImportCharacter(const FString& Folder)
{
	const FString Source = FPaths::Combine(FPaths::ProjectDir(), TEXT("RawCharacters"), Folder);
	const FString VrmFile = FPaths::Combine(Source, TEXT("character.vrm"));
	const FString CharacterFolder = FString::Printf(TEXT("%s/%s"), CharactersRoot, *Folder);

	UVrmAssetListObject* Character = UVrmImporterBPFunctionLibrary::ImportVRMFileWithOptions(
		VrmFile, CharacterFolder / TEXT("character"), MakeImportOptions());
	if (Character == nullptr || Character->SkeletalMesh == nullptr)
	{
		UE_LOG(LogNpcifyImport, Error, TEXT("VRM import failed: %s"), *VrmFile);
		return false;
	}
	ApplyAlphaCutoff(VrmFile, CharacterFolder);
	UE_LOG(LogNpcifyImport, Display, TEXT("Rotation constraints: %d"), FillRotationConstraints(VrmFile, Character));
	UIKRigDefinition* CharacterRig = FindHumanoidRig(Character);
	UE_LOG(LogNpcifyImport, Display, TEXT("Imported %s -> %s (IK rig: %s)"), *VrmFile, *Character->GetPathName(),
		CharacterRig ? *CharacterRig->GetPathName() : TEXT("missing"));
	if (CharacterRig == nullptr)
	{
		return false;
	}

	TArray<FString> Animations;
	IFileManager::Get().FindFiles(Animations, *FPaths::Combine(Source, TEXT("animations"), TEXT("*.vrma")), true, false);
	bool bAllAnimations = true;
	for (const FString& Animation : Animations)
	{
		const FString Name = FPaths::GetBaseFilename(Animation);
		const FString AnimationFile = FPaths::Combine(Source, TEXT("animations"), Animation);
		const FString TemporaryFolder = FString::Printf(TEXT("%s/%s/%s"), TemporaryRoot, *Folder, *Name);

		// 1. The VRMA becomes a small model with its own humanoid skeleton and animation.
		UVrmAssetListObject* Vrma = UVrmImporterBPFunctionLibrary::ImportVRMFileWithOptions(
			AnimationFile, TemporaryFolder / Name, MakeImportOptions());
		UAnimSequence* SourceAnimation = Vrma ? FindAsset<UAnimSequence>(TemporaryFolder, TEXT("A_") + Name) : nullptr;
		if (SourceAnimation == nullptr || Vrma->SkeletalMesh == nullptr)
		{
			UE_LOG(LogNpcifyImport, Error, TEXT("VRMA import failed: %s"), *AnimationFile);
			bAllAnimations = false;
			continue;
		}

		// 2. Retarget the VRMA directly onto the character. Both rest poses are the VRM T-pose,
		//    so FK rotations alone give the same pose as the Npcify web player (three-vrm).
		//    Hand IK goals (for example through the UE5 Mannequin) move the hands away from
		//    the body on characters with other proportions.
		UIKRigDefinition* VrmaRig = FindHumanoidRig(Vrma);
		UIKRetargeter* Retargeter = VrmaRig ? MakeDirectRetargeter(TemporaryFolder, VrmaRig, Vrma->SkeletalMesh, CharacterRig, Character->SkeletalMesh) : nullptr;
		if (Retargeter == nullptr)
		{
			UE_LOG(LogNpcifyImport, Error, TEXT("Cannot make the retargeter for %s (VRMA IK rig: %d)"), *Name, VrmaRig != nullptr);
			bAllAnimations = false;
			continue;
		}

		FIKRetargetBatchOperationInputs Inputs;
		Inputs.AssetsToRetarget.Add(FAssetData(SourceAnimation));
		Inputs.SourceMesh = Vrma->SkeletalMesh;
		Inputs.TargetMesh = Character->SkeletalMesh;
		Inputs.IKRetargetAsset = Retargeter;
		Inputs.TargetPath = CharacterFolder / TEXT("Animations");
		Inputs.Search = TEXT("A_");
		Inputs.Replace = TEXT("AS_");
		Inputs.bIncludeReferencedAssets = false;
		Inputs.bOverwriteExistingFiles = true;
		const TArray<FAssetData> Created = UIKRetargetBatchOperation::RunBatchRetarget(Inputs);
		for (const FAssetData& Asset : Created)
		{
			UE_LOG(LogNpcifyImport, Display, TEXT("Animation %s -> %s"), *Name, *Asset.GetObjectPathString());
			if (UObject* Object = Asset.GetAsset())
			{
				SaveObjectPackage(Object);
			}
		}
		if (Created.Num() == 0)
		{
			UE_LOG(LogNpcifyImport, Error, TEXT("Animation retarget to the character failed: %s"), *Name);
			bAllAnimations = false;
		}
	}
	return bAllAnimations;
}
