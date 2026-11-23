#include "NpcifyAnimationHandover.h"

#include "AlphaBlend.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/SkeletalMeshComponent.h"

FNpcifyAnimationHandover FNpcifyAnimationHandover::Capture(USkeletalMeshComponent* Mesh)
{
    FNpcifyAnimationHandover State;
    if (!Mesh)
    {
        return State;
    }
    State.PreviousInstance = Mesh->GetAnimInstance();
    if (Mesh->GetAnimationMode() == EAnimationMode::AnimationSingleNode)
    {
        if (UAnimSingleNodeInstance* Single = Mesh->GetSingleNodeInstance())
        {
            // A new single node instance starts from AnimationData: the time of now, not the
            // start time of the last PlayAnimation.
            Mesh->AnimationData.PopulateFrom(Single);
            State.bSingleNode = true;
        }
        return State;
    }
    UAnimInstance* Instance = State.PreviousInstance.Get();
    if (!Instance)
    {
        return State;
    }
    for (const FAnimMontageInstance* Montage : Instance->MontageInstances)
    {
        if (Montage && Montage->Montage && Montage->IsActive())
        {
            FMontage Entry;
            Entry.Montage = Montage->Montage;
            Entry.Position = Montage->GetPosition();
            Entry.PlayRate = Montage->GetPlayRate();
            Entry.Weight = Montage->GetWeight();
            Entry.BlendTimeRemaining = Montage->GetBlend().GetBlendTimeRemaining();
            Entry.bPlaying = Montage->IsPlaying();
            State.Montages.Add(Entry);
        }
    }
    return State;
}

int32 FNpcifyAnimationHandover::Restore(USkeletalMeshComponent* Mesh) const
{
    UAnimInstance* Instance = Mesh ? Mesh->GetAnimInstance() : nullptr;
    if (!Instance || bSingleNode || Instance == PreviousInstance.Get())
    {
        // The engine kept the anim instance with its state, or the single node animation
        // started again from AnimationData.
        return 0;
    }
    int32 Restored = 0;
    for (const FMontage& Entry : Montages)
    {
        UAnimMontage* Montage = Entry.Montage.Get();
        if (!Montage)
        {
            continue;
        }
        // A montage at full weight continues at full weight; one that was blending in blends in
        // for the rest of its blend time.
        const FAlphaBlendArgs BlendIn(Entry.Weight >= 0.999f ? 0.0f : Entry.BlendTimeRemaining);
        if (Instance->Montage_PlayWithBlendIn(Montage, BlendIn, Entry.PlayRate, EMontagePlayReturnType::MontageLength, Entry.Position, false) <= 0.0f)
        {
            continue;
        }
        if (!Entry.bPlaying)
        {
            Instance->Montage_Pause(Montage);
        }
        ++Restored;
    }
    return Restored;
}
