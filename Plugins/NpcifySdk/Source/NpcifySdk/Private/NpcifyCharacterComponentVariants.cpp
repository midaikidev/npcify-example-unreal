#include "NpcifyCharacterComponent.h"

#include "NpcifyAnimationHandover.h"

#include <exception>
#include <string>

void UNpcifyCharacterComponent::SwitchVariant(const FString& VariantKey)
{
    EnsureInitialized();
    const std::string KeyUtf8 = TCHAR_TO_UTF8(*VariantKey);
    const std::string Previous = Runtime->GetActiveVariant();
    try
    {
        Runtime->SwitchVariant(KeyUtf8);
        if (Previous == KeyUtf8)
        {
            ApplyVariantMesh(VariantKey);
        }
    }
    catch (const std::exception& Ex)
    {
        OnError.Broadcast(UTF8_TO_TCHAR(Ex.what()));
    }
}

FString UNpcifyCharacterComponent::GetActiveVariant() const
{
    if (!Runtime.IsValid())
    {
        return TEXT("default");
    }
    return UTF8_TO_TCHAR(Runtime->GetActiveVariant().c_str());
}

void UNpcifyCharacterComponent::ApplyVariantMesh(const FString& VariantKey)
{
    const TObjectPtr<USkeletalMesh>* FoundMesh = VariantMeshes.Find(VariantKey);
    if (!FoundMesh || !IsValid(*FoundMesh))
    {
        return;
    }
    if (SkeletalMeshes.Num() == 0)
    {
        RefreshAvatarBindings();
    }
    if (USkeletalMeshComponent* PrimaryMesh = GetSkeletalMeshComponent())
    {
        if (PrimaryMesh->GetSkeletalMeshAsset() != *FoundMesh)
        {
            // A change of variant changes the model only: the animation continues on the next mesh.
            const FNpcifyAnimationHandover Animation = FNpcifyAnimationHandover::Capture(PrimaryMesh);
            PrimaryMesh->SetSkeletalMeshAsset(*FoundMesh);
            Animation.Restore(PrimaryMesh);
        }
        RefreshAvatarBindings();
        if (bLiveMotion)
        {
            BindLiveMotionMesh();
        }
    }
}

void UNpcifyCharacterComponent::HandleVariantChanged(const FString& VariantKey)
{
    ApplyVariantMesh(VariantKey);
    OnVariantChanged.Broadcast(VariantKey);
}

void UNpcifyCharacterComponent::HandleToolCall(const Npcify::FNpcifyStagingToolCall& Call)
{
    FNpcifyToolCallEvent Event;
    Event.Id = UTF8_TO_TCHAR(Call.Id.c_str());
    Event.Name = UTF8_TO_TCHAR(Call.Name.c_str());
    Event.Timestamp = static_cast<float>(Call.Timestamp);
    Event.Variant = UTF8_TO_TCHAR(Call.Variant.c_str());
    Event.bOk = Call.Result.bOk;
    Event.Status = UTF8_TO_TCHAR(Call.Result.Status.c_str());
    Event.PreviousVariant = UTF8_TO_TCHAR(Call.Result.PreviousVariant.c_str());
    Event.ActiveVariant = UTF8_TO_TCHAR(Call.Result.ActiveVariant.c_str());
    Event.Error = UTF8_TO_TCHAR(Call.Result.Error.c_str());
    OnToolCall.Broadcast(Event);
}
