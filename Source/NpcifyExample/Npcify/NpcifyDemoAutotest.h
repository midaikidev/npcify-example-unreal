#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "NpcifyDemoClockProbe.h"
#include "NpcifyDemoAutotest.generated.h"

class ANpcifyDialogueDirector;
class ANpcifyNpc;

/**
 * Smoke test that runs only with the -NpcifyAutotest=<folder> command-line argument.
 * It walks to each NPC, talks, picks the first reply, saves screenshots, and quits. With
 * -NpcifyAutotestReply="<text>" it types that reply to an NPC that allows custom replies. For a
 * character with live motion on it reports the live frames of the reply and fails when none played.
 */
UCLASS(NotPlaceable)
class NPCIFYEXAMPLE_API ANpcifyDemoAutotest : public AActor
{
	GENERATED_BODY()

public:
	ANpcifyDemoAutotest();

	FString OutputFolder;
	FString CustomReply;

protected:
	virtual void Tick(float DeltaSeconds) override;

private:
	void Step(const FString& Label, float Delay, TFunction<bool()> Done, TFunction<void()> Action);
	void Capture(const FString& Name);
	void LogVoiceSpatialization(const FString& Tag, const class UAudioComponent* Audio);
	void MovePlayerNear(ANpcifyNpc* Npc);
	void LogLiveMotion(const FString& Tag, ANpcifyNpc* Npc);

	UFUNCTION()
	void HandleLiveMotionFrames(const FString& StagingId, int32 StartFrame, int32 FrameCount);
	ANpcifyDialogueDirector* Director() const;

	struct FStep
	{
		FString Label;
		float Delay = 0.f;
		TFunction<bool()> Done;
		TFunction<void()> Action;
	};

	TArray<FStep> Steps;
	int32 Current = -1;
	float StepTime = 0.f;
	int32 Failures = 0;
	float MaxMouthOpenness = 0.f;
	float MaxLiveWeight = 0.f;
	float MaxHandAboveShoulder = -1000.f;
	float MaxLeftHandAboveShoulder = -1000.f;
	double ReplySentAt = 0.0;
	/** The online reply: the lag of its staging clock behind its voice. */
	TUniquePtr<FNpcifyDemoClockProbe> ClockProbe;
	FString ClockProbeTag;

	UPROPERTY(Transient)
	TObjectPtr<ANpcifyNpc> FramesNpc;
	bool bPlanned = false;
};
