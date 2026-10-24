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
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Rig/IKRigDefinition.h"
#include "UObject/Package.h"
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

		// 2. Bake through the UE5 Mannequin with the retargeters that VRM4U generated:
		//    VRMA -> Mannequin (RTG_<animation>), then Mannequin -> character (RTG_character).
		UIKRetargeter* ToMannequin = FindAsset<UIKRetargeter>(TemporaryFolder, TEXT("RTG_") + Name);
		UIKRetargeter* ToCharacter = FindAsset<UIKRetargeter>(CharacterFolder, TEXT("RTG_character"));
		USkeletalMesh* Mannequin = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"));
		if (ToMannequin == nullptr || ToCharacter == nullptr || Mannequin == nullptr)
		{
			UE_LOG(LogNpcifyImport, Error, TEXT("Missing retargeter for %s (to mannequin: %d, to character: %d, mannequin: %d)"),
				*Name, ToMannequin != nullptr, ToCharacter != nullptr, Mannequin != nullptr);
			bAllAnimations = false;
			continue;
		}

		FIKRetargetBatchOperationInputs First;
		First.AssetsToRetarget.Add(FAssetData(SourceAnimation));
		First.SourceMesh = Vrma->SkeletalMesh;
		First.TargetMesh = Mannequin;
		First.IKRetargetAsset = ToMannequin;
		First.TargetPath = TemporaryFolder / TEXT("Mannequin");
		First.Search = TEXT("A_");
		First.Replace = TEXT("AM_");
		First.bIncludeReferencedAssets = false;
		First.bOverwriteExistingFiles = true;
		const TArray<FAssetData> OnMannequin = UIKRetargetBatchOperation::RunBatchRetarget(First);
		if (OnMannequin.Num() == 0)
		{
			UE_LOG(LogNpcifyImport, Error, TEXT("Animation retarget to the mannequin failed: %s"), *Name);
			bAllAnimations = false;
			continue;
		}

		FIKRetargetBatchOperationInputs Second;
		Second.AssetsToRetarget = OnMannequin;
		Second.SourceMesh = Mannequin;
		Second.TargetMesh = Character->SkeletalMesh;
		Second.IKRetargetAsset = ToCharacter;
		Second.TargetPath = CharacterFolder / TEXT("Animations");
		Second.Search = TEXT("AM_");
		Second.Replace = TEXT("AS_");
		Second.bIncludeReferencedAssets = false;
		Second.bOverwriteExistingFiles = true;
		const TArray<FAssetData> Created = UIKRetargetBatchOperation::RunBatchRetarget(Second);
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
