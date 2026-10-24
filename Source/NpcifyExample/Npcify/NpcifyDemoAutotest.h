#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "NpcifyDemoAutotest.generated.h"

class ANpcifyDialogueDirector;
class ANpcifyNpc;

/**
 * Smoke test that runs only with the -NpcifyAutotest=<folder> command-line argument.
 * It walks to each NPC, talks, picks the first reply, saves screenshots, and quits.
 */
UCLASS(NotPlaceable)
class NPCIFYEXAMPLE_API ANpcifyDemoAutotest : public AActor
{
	GENERATED_BODY()

public:
	ANpcifyDemoAutotest();

	FString OutputFolder;

protected:
	virtual void Tick(float DeltaSeconds) override;

private:
	void Step(const FString& Label, float Delay, TFunction<bool()> Done, TFunction<void()> Action);
	void Capture(const FString& Name);
	void MovePlayerNear(ANpcifyNpc* Npc);
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
	bool bPlanned = false;
};
