#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UAnimInstance;
class UAnimMontage;
class USkeletalMeshComponent;

/**
 * The animation state of a skeletal mesh component across a change of its mesh (a change of
 * appearance variant). A change of variant changes the model only. The engine keeps the anim
 * instance when the next mesh has the same skeleton; a mesh with another skeleton (each VRM4U
 * import has its own) gets a new anim instance. Capture reads the state before the change:
 * the single node animation (asset, time, play rate, looping, playing) and the active montages.
 * Restore gives a new anim instance that state: the single node animation continues at its time,
 * and each montage plays on at its position.
 */
struct FNpcifyAnimationHandover
{
    struct FMontage
    {
        TWeakObjectPtr<UAnimMontage> Montage;
        float Position = 0.0f;
        float PlayRate = 1.0f;
        float Weight = 1.0f;
        float BlendTimeRemaining = 0.0f;
        bool bPlaying = true;
    };

    TWeakObjectPtr<UAnimInstance> PreviousInstance;
    bool bSingleNode = false;
    TArray<FMontage> Montages;

    static FNpcifyAnimationHandover Capture(USkeletalMeshComponent* Mesh);

    /** Returns the montages that play again on a new anim instance. */
    int32 Restore(USkeletalMeshComponent* Mesh) const;
};
