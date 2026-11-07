#include "NpcifyCharacterComponent.h"

#include "NpcifyConversationSocket.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/AssetUserData.h"
#include "Engine/SkeletalMesh.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/EngineVersionComparison.h"
#include "ReferenceSkeleton.h"
#include "UObject/UnrealType.h"

#include <map>
#include <string>

// Live motion of UNpcifyCharacterComponent (docs/realtime-motion.md): the conversation stream,
// the warm-up, and the pose on the skeletal mesh after its animation.

DEFINE_LOG_CATEGORY_STATIC(LogNpcifyLiveMotion, Log, All);

/** The mesh that plays live motion: its VRM humanoid bones, their T-pose, and the rig. */
struct FNpcifyLiveMotionBinding
{
    TWeakObjectPtr<USkeletalMeshComponent> Mesh;
    TWeakObjectPtr<USkeletalMesh> Asset;
    TArray<int32> Parents;
    /** Component-space rotation of each bone in the reference pose (the VRM T-pose). */
    TArray<FQuat> RefRotations;
    /** The VRM bone of each skeleton bone (empty: not a live motion bone). */
    TArray<FString> VrmBones;
    TArray<std::string> VrmBoneNames;
    /** The skeleton index of the bone's nearest present VRM parent (-1: the hips). */
    TArray<int32> VrmParents;
    int32 HipsIndex = INDEX_NONE;
    TUniquePtr<Npcify::FNpcifyLiveMotionRig> Rig;
    FDelegateHandle Handle;
    TArray<FTransform> Scratch;
    TArray<FQuat> BaseAccumulated;
    TArray<FQuat> NewAccumulated;
};

namespace
{
    UObject* ReadObjectProperty(UObject* Object, const TCHAR* Name)
    {
        const FObjectPropertyBase* Property = Object ? FindFProperty<FObjectPropertyBase>(Object->GetClass(), Name) : nullptr;
        return Property ? Property->GetObjectPropertyValue_InContainer(Object) : nullptr;
    }

    bool ReadStringMapProperty(UObject* Object, const TCHAR* Name, TMap<FString, FName>& Out)
    {
        const FMapProperty* Property = FindFProperty<FMapProperty>(Object->GetClass(), Name);
        if (!Property || !Property->KeyProp->IsA<FStrProperty>() || !Property->ValueProp->IsA<FStrProperty>())
        {
            return false;
        }
        FScriptMapHelper Helper(Property, Property->ContainerPtrToValuePtr<void>(Object));
        for (FScriptMapHelper::FIterator It = Helper.CreateIterator(); It; ++It)
        {
            const FString& Key = *reinterpret_cast<const FString*>(Helper.GetKeyPtr(It));
            const FString& Value = *reinterpret_cast<const FString*>(Helper.GetValuePtr(It));
            if (!Key.IsEmpty() && !Value.IsEmpty())
            {
                Out.Add(Key, FName(*Value));
            }
        }
        return Out.Num() > 0;
    }

    /**
     * VRM4U stores the VRM humanoid table (VRM bone to skeleton bone) with the imported mesh:
     * asset user data -> VrmAssetListObject -> VrmMetaObject -> humanoidBoneTable. Read it by
     * reflection, so the SDK needs no VRM4U module.
     */
    bool ReadVrm4uHumanoidBones(const USkeletalMesh* Asset, TMap<FString, FName>& Out)
    {
        const TArray<UAssetUserData*>* UserData = Asset ? Asset->GetAssetUserDataArray() : nullptr;
        if (!UserData)
        {
            return false;
        }
        for (UAssetUserData* Data : *UserData)
        {
            UObject* List = ReadObjectProperty(Data, TEXT("VrmAssetListObject"));
            UObject* Meta = ReadObjectProperty(List, TEXT("VrmMetaObject"));
            if (Meta && ReadStringMapProperty(Meta, TEXT("humanoidBoneTable"), Out))
            {
                return true;
            }
        }
        return false;
    }

    void SetLiveMotionAuthHeaders(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request, const std::string& Token)
    {
        if (!Token.empty())
        {
            const FString TokenStr = UTF8_TO_TCHAR(Token.c_str());
            Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *TokenStr));
            Request->SetHeader(TEXT("X-API-Token"), TokenStr);
        }
    }

    FQuat ToQuat(const Npcify::FNpcifyQuat& Q)
    {
        return FQuat(Q.X, Q.Y, Q.Z, Q.W);
    }
}

bool UNpcifyCharacterComponent::WantsLiveMotion() const
{
    return bLiveMotion && LiveMotionBinding.IsValid() && LiveMotionBinding->Rig.IsValid() && Runtime.IsValid() &&
        !Runtime->HasBakedConversation() && (!bLiveMotionInfoKnown || bLiveMotionInEffect);
}

bool UNpcifyCharacterComponent::IsLiveMotionReady() const
{
    return WantsLiveMotion();
}

float UNpcifyCharacterComponent::GetLiveMotionWeight() const
{
    return Runtime.IsValid() ? static_cast<float>(Runtime->GetLiveMotion().GetWeight()) : 0.0f;
}

bool UNpcifyCharacterComponent::GetLiveMotionStats(
    const FString& StagingId,
    int32& FramesReceived,
    int32& FramesPlayed,
    float& FirstFrameTime) const
{
    FramesReceived = 0;
    FramesPlayed = 0;
    FirstFrameTime = -1.0f;
    Npcify::FNpcifyLiveMotionStats Stats;
    if (!Runtime.IsValid() || !Runtime->GetLiveMotion().GetStats(TCHAR_TO_UTF8(*StagingId), Stats))
    {
        return false;
    }
    FramesReceived = Stats.FramesReceived;
    FramesPlayed = Stats.FramesPlayed;
    FirstFrameTime = static_cast<float>(Stats.FirstFrameTime);
    return true;
}

FString UNpcifyCharacterComponent::GetActiveStagingId() const
{
    return Runtime.IsValid() ? FString(UTF8_TO_TCHAR(Runtime->GetActiveStagingId().c_str())) : FString();
}

FName UNpcifyCharacterComponent::GetHumanoidBoneName(const FString& VrmBone) const
{
    const FNpcifyLiveMotionBinding* Binding = LiveMotionBinding.Get();
    const USkeletalMesh* Asset = Binding ? Binding->Asset.Get() : nullptr;
    if (!Asset)
    {
        return NAME_None;
    }
    const int32 Index = Binding->VrmBones.IndexOfByKey(VrmBone);
    return Index != INDEX_NONE ? Asset->GetRefSkeleton().GetBoneName(Index) : NAME_None;
}

void UNpcifyCharacterComponent::PrepareLiveMotion()
{
    EnsureInitialized();
    if (bLiveMotion)
    {
        BindLiveMotionMesh();
    }
    const FString ConversationIdStr = UTF8_TO_TCHAR(Runtime->GetConversationId().c_str());
    if (!WantsLiveMotion() || ConversationIdStr.IsEmpty() || LiveMotionPreparedConversation == ConversationIdStr)
    {
        return;
    }
    if (StreamSocket.IsValid() &&
        (StreamSocket->IsRefused(ConversationIdStr) || StreamSocket->IsFailing(ConversationIdStr)))
    {
        // The frames could not reach this client: a warm-up would cost the owner for nothing.
        return;
    }
    LiveMotionPreparedConversation = ConversationIdStr;
    // Join the stream now, so that the first reply does not wait for it.
    JoinConversationStream(ConversationIdStr, nullptr);
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(FString::Printf(
        TEXT("%s/conversations/%s/motion/warm-up"),
        UTF8_TO_TCHAR(Runtime->GetOptions().BaseUrl.c_str()),
        *ConversationIdStr
    ));
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
    SetLiveMotionAuthHeaders(Request, Runtime->GetOptions().ResolvedToken());
    Request->SetContentAsString(FString::Printf(TEXT("{\"capabilities\":[\"%s\"]}"), UTF8_TO_TCHAR(Npcify::FNpcifyMotionFrames::Capability)));
    Request->OnProcessRequestComplete().BindLambda([](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
    {
        // A refused warm-up costs nothing; the first message still opens the session.
        const bool bWarming = bConnected && Response.IsValid() && EHttpResponseCodes::IsOk(Response->GetResponseCode()) &&
            Response->GetContentAsString().Contains(TEXT("\"warming\":true"));
        UE_LOG(LogNpcifyLiveMotion, Log, TEXT("Live motion warm-up: %s"), bWarming ? TEXT("warming") : TEXT("not warming"));
    });
    Request->ProcessRequest();
}

void UNpcifyCharacterComponent::DownloadMotionTrack(const std::string& StagingId, const Npcify::FStagingMotionTrack& Track)
{
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    // Signed storage URLs must not receive the API key.
    Request->SetURL(UTF8_TO_TCHAR(Track.FramesUrl.c_str()));
    Request->SetVerb(TEXT("GET"));
    TWeakObjectPtr<UNpcifyCharacterComponent> WeakThis(this);
    Request->OnProcessRequestComplete().BindLambda(
        [WeakThis, StagingId, Track](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
        {
            if (!WeakThis.IsValid() || !WeakThis->Runtime.IsValid())
            {
                return;
            }
            if (!bConnected || !Response.IsValid() || !EHttpResponseCodes::IsOk(Response->GetResponseCode()))
            {
                WeakThis->OnError.Broadcast(TEXT("Npcify live motion frames download failed."));
                return;
            }
            const TArray<uint8>& Content = Response->GetContent();
            try
            {
                WeakThis->Runtime->GetLiveMotion().Load(
                    StagingId,
                    Track,
                    std::vector<uint8_t>(Content.GetData(), Content.GetData() + Content.Num()));
            }
            catch (const std::exception& Ex)
            {
                WeakThis->OnError.Broadcast(UTF8_TO_TCHAR(Ex.what()));
            }
        }
    );
    Request->ProcessRequest();
}

void UNpcifyCharacterComponent::UpdateLiveMotion(float)
{
    Runtime->GetLiveMotion().SetRootMode(
        LiveMotionRootMode == ENpcifyLiveMotionRootMode::World
            ? Npcify::ENpcifyLiveMotionRootMode::World
            : Npcify::ENpcifyLiveMotionRootMode::Anchored);
    if (bLiveMotion)
    {
        BindLiveMotionMesh();
    }
    else
    {
        UnbindLiveMotionMesh();
    }
}

void UNpcifyCharacterComponent::BindLiveMotionMesh()
{
    USkeletalMeshComponent* Mesh = IsValid(LiveMotionMesh) ? LiveMotionMesh.Get() : nullptr;
    for (int32 Index = 0; Mesh == nullptr && Index < SkeletalMeshes.Num(); ++Index)
    {
        if (IsValid(SkeletalMeshes[Index]) && SkeletalMeshes[Index]->GetSkeletalMeshAsset())
        {
            Mesh = SkeletalMeshes[Index];
        }
    }
    USkeletalMesh* Asset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
    if (LiveMotionBinding.IsValid() && LiveMotionBinding->Mesh.Get() == Mesh && LiveMotionBinding->Asset.Get() == Asset)
    {
        return;
    }
    UnbindLiveMotionMesh();
    if (!Mesh || !Asset)
    {
        return;
    }

    TMap<FString, FName> Names = LiveMotionBones;
    if (Names.Num() == 0 && !ReadVrm4uHumanoidBones(Asset, Names))
    {
        UE_LOG(LogNpcifyLiveMotion, Warning,
            TEXT("%s: no VRM humanoid bones for live motion (set LiveMotionBones or import the model with VRM4U). The character keeps its prepared animation clips."),
            *GetNameSafe(GetOwner()));
        // Remember the mesh, so the warning appears once.
        LiveMotionBinding = MakeShared<FNpcifyLiveMotionBinding>();
        LiveMotionBinding->Mesh = Mesh;
        LiveMotionBinding->Asset = Asset;
        return;
    }

    const FReferenceSkeleton& Reference = Asset->GetRefSkeleton();
    const TArray<FTransform>& ReferencePose = Reference.GetRefBonePose();
    const int32 Count = Reference.GetNum();
    TSharedPtr<FNpcifyLiveMotionBinding> Binding = MakeShared<FNpcifyLiveMotionBinding>();
    Binding->Mesh = Mesh;
    Binding->Asset = Asset;
    Binding->Parents.SetNum(Count);
    Binding->RefRotations.SetNum(Count);
    Binding->VrmBones.SetNum(Count);
    Binding->VrmBoneNames.SetNum(Count);
    Binding->VrmParents.Init(INDEX_NONE, Count);
    TArray<FTransform> ReferenceComponent;
    ReferenceComponent.SetNum(Count);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const int32 Parent = Reference.GetParentIndex(Index);
        Binding->Parents[Index] = Parent;
        ReferenceComponent[Index] = Parent >= 0 ? ReferencePose[Index] * ReferenceComponent[Parent] : ReferencePose[Index];
        Binding->RefRotations[Index] = ReferenceComponent[Index].GetRotation();
    }
    std::map<std::string, Npcify::FNpcifyVec3> TPose;
    TMap<FString, int32> Indices;
    for (const std::string& Bone : Npcify::FNpcifyMotionFrames::Bones())
    {
        const FString VrmBone = UTF8_TO_TCHAR(Bone.c_str());
        const FName* SkeletonBone = Names.Find(VrmBone);
        const int32 Index = SkeletonBone ? Reference.FindBoneIndex(*SkeletonBone) : INDEX_NONE;
        if (Index == INDEX_NONE)
        {
            continue;
        }
        Indices.Add(VrmBone, Index);
        Binding->VrmBones[Index] = VrmBone;
        Binding->VrmBoneNames[Index] = Bone;
        const FVector Location = ReferenceComponent[Index].GetLocation();
        TPose[Bone] = Npcify::FNpcifyVec3(Location.X, Location.Y, Location.Z);
    }
    try
    {
        // The component origin is the floor under the model.
        Binding->Rig = MakeUnique<Npcify::FNpcifyLiveMotionRig>(TPose, Npcify::FNpcifyVec3());
    }
    catch (const std::exception& Ex)
    {
        UE_LOG(LogNpcifyLiveMotion, Warning, TEXT("%s: live motion is off for this mesh: %s"), *GetNameSafe(GetOwner()), UTF8_TO_TCHAR(Ex.what()));
        Binding->Rig.Reset();
        LiveMotionBinding = Binding;
        return;
    }
    for (const TPair<FString, int32>& Pair : Indices)
    {
        const std::string Parent = Binding->Rig->ParentOf(TCHAR_TO_UTF8(*Pair.Key));
        const int32* ParentIndex = Parent.empty() ? nullptr : Indices.Find(UTF8_TO_TCHAR(Parent.c_str()));
        Binding->VrmParents[Pair.Value] = ParentIndex ? *ParentIndex : INDEX_NONE;
        if (Pair.Key == TEXT("hips"))
        {
            Binding->HipsIndex = Pair.Value;
        }
    }
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    Mesh->OnBoneTransformsFinalized.AddDynamic(this, &UNpcifyCharacterComponent::HandleBoneTransformsFinalized);
#else
    Binding->Handle = Mesh->RegisterOnBoneTransformsFinalizedDelegate(
        FOnBoneTransformsFinalizedMultiCast::FDelegate::CreateUObject(this, &UNpcifyCharacterComponent::HandleBoneTransformsFinalized));
#endif
    // The staging clock and the live pose update before the mesh evaluates its animation.
    Mesh->PrimaryComponentTick.AddPrerequisite(this, PrimaryComponentTick);
    LiveMotionBinding = Binding;
    UE_LOG(LogNpcifyLiveMotion, Log, TEXT("%s: live motion plays on %s (%d VRM bones)."),
        *GetNameSafe(GetOwner()), *GetNameSafe(Asset), Indices.Num());
}

void UNpcifyCharacterComponent::UnbindLiveMotionMesh()
{
    if (!LiveMotionBinding.IsValid())
    {
        return;
    }
    USkeletalMeshComponent* Mesh = LiveMotionBinding->Mesh.Get();
    if (Mesh && LiveMotionBinding->Rig.IsValid())
    {
#if UE_VERSION_OLDER_THAN(5, 5, 0)
        Mesh->OnBoneTransformsFinalized.RemoveDynamic(this, &UNpcifyCharacterComponent::HandleBoneTransformsFinalized);
#else
        Mesh->UnregisterOnBoneTransformsFinalizedDelegate(LiveMotionBinding->Handle);
#endif
        Mesh->PrimaryComponentTick.RemovePrerequisite(this, PrimaryComponentTick);
    }
    LiveMotionBinding.Reset();
}

void UNpcifyCharacterComponent::HandleBoneTransformsFinalized()
{
    FNpcifyLiveMotionBinding* Binding = LiveMotionBinding.Get();
    if (!Binding || !Binding->Rig.IsValid() || !Runtime.IsValid() || Binding->HipsIndex == INDEX_NONE)
    {
        return;
    }
    const Npcify::FNpcifyLiveMotionPlayer& Player = Runtime->GetLiveMotion();
    USkeletalMeshComponent* Mesh = Binding->Mesh.Get();
    if (!Player.HasPose() || !Mesh || Mesh->GetSkeletalMeshAsset() != Binding->Asset.Get())
    {
        return;
    }
    // The animation of this frame is final here; the mesh sends the bones to the renderer and
    // updates its attachments and bounds after this callback. FK only: each live bone gets
    // its blended normalized rotation over the T-pose, and every other bone keeps its local
    // transform under its parent.
    TArray<FTransform>& Pose = const_cast<TArray<FTransform>&>(Mesh->GetComponentSpaceTransforms());
    const int32 Count = Binding->Parents.Num();
    if (Pose.Num() != Count)
    {
        return;
    }
    const Npcify::FNpcifyLiveMotionRig& Rig = *Binding->Rig;
    const double Weight = Player.GetWeight();
    TArray<FTransform>& Result = Binding->Scratch;
    if (Result.Num() != Count)
    {
        Result.SetNum(Count);
        Binding->BaseAccumulated.SetNum(Count);
        Binding->NewAccumulated.SetNum(Count);
    }
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const int32 Parent = Binding->Parents[Index];
        const FTransform Local = Parent >= 0 ? Pose[Index].GetRelativeTransform(Pose[Parent]) : Pose[Index];
        const FString& VrmBone = Binding->VrmBones[Index];
        if (VrmBone.IsEmpty())
        {
            Result[Index] = Parent >= 0 ? Local * Result[Parent] : Pose[Index];
            continue;
        }
        const FQuat Reference = Binding->RefRotations[Index];
        const FQuat BaseAccumulated = Pose[Index].GetRotation() * Reference.Inverse();
        const int32 VrmParent = Binding->VrmParents[Index];
        const FQuat BaseLocal = VrmParent >= 0
            ? Binding->BaseAccumulated[VrmParent].Inverse() * BaseAccumulated
            : BaseAccumulated;
        Npcify::FNpcifyQuat Live;
        const FQuat Blended = Rig.TryGetLiveLocalRotation(Player, Binding->VrmBoneNames[Index], Live)
            ? FQuat::Slerp(BaseLocal, ToQuat(Live), Weight)
            : BaseLocal;
        const FQuat NewAccumulated = VrmParent >= 0 ? Binding->NewAccumulated[VrmParent] * Blended : Blended;
        Binding->BaseAccumulated[Index] = BaseAccumulated;
        Binding->NewAccumulated[Index] = NewAccumulated;
        FVector Location;
        if (Index == Binding->HipsIndex)
        {
            const Npcify::FNpcifyVec3 LiveHips = Rig.GetLiveHipsPosition(Player);
            Location = FMath::Lerp(Pose[Index].GetLocation(), FVector(LiveHips.X, LiveHips.Y, LiveHips.Z), Weight);
        }
        else
        {
            Location = Parent >= 0 ? Result[Parent].TransformPosition(Local.GetLocation()) : Pose[Index].GetLocation();
        }
        Result[Index] = FTransform((NewAccumulated * Reference).GetNormalized(), Location, Pose[Index].GetScale3D());
    }
    for (int32 Index = 0; Index < Count; ++Index)
    {
        Pose[Index] = Result[Index];
    }
}
