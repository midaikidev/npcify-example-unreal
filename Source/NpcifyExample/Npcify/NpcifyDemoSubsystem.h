#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "NpcifyDemoSubsystem.generated.h"

/**
 * Adds the demo to the default level when play starts: the two Npcify NPCs in
 * front of the player start, and the dialogue director. NPCs that a level already
 * contains are kept, so you can also place ANpcifyNpc actors by hand.
 */
UCLASS()
class NPCIFYEXAMPLE_API UNpcifyDemoSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& World) override;
};
