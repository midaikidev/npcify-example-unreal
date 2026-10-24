#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "NpcifyNpc.generated.h"

class UAnimSequence;
class UCapsuleComponent;
class UNpcifyCharacterComponent;
class USkeletalMesh;
class USkeletalMeshComponent;

UENUM(BlueprintType)
enum class ENpcifyConnectionMode : uint8
{
	/** The baked conversation ships with the game. No network. */
	OfflineBaked,
	/** Every reply comes from the Npcify API. */
	OnlineApi,
};

/**
 * An Npcify character in the level: a VRM model (imported with VRM4U), its baked
 * animations, and an Npcify Character component for speech, lip-sync and replies.
 */
UCLASS()
class NPCIFYEXAMPLE_API ANpcifyNpc : public AActor
{
	GENERATED_BODY()

public:
	ANpcifyNpc();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Npcify")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Npcify")
	ENpcifyConnectionMode Mode = ENpcifyConnectionMode::OfflineBaked;

	/** Used when the .env file does not set the character ID. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Npcify")
	FString DefaultCharacterId;

	/** The imported VRM model, for example /Game/Npcify/Characters/hinata/SK_character. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Npcify")
	TSoftObjectPtr<USkeletalMesh> Model;

	/** Folder with the baked animations AS_<animation ID>. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Npcify", meta=(ContentDir))
	FDirectoryPath AnimationFolder;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Npcify")
	FString IdleAnimationId = TEXT("idle");

	/** Baked conversation file, relative to the Content folder. Offline mode only. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Npcify")
	FString OfflineConversationFile;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Npcify")
	TObjectPtr<UCapsuleComponent> Capsule;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Npcify")
	TObjectPtr<USkeletalMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Npcify")
	TObjectPtr<UNpcifyCharacterComponent> Npcify;

	bool IsReady() const { return bReady; }
	const FString& GetStatusMessage() const { return StatusMessage; }
	bool AllowsCustomReply() const { return Mode == ENpcifyConnectionMode::OnlineApi; }
	FText GetModeLabel() const;

	/** A point above the head for the name label and the interaction prompt. */
	FVector GetLabelLocation() const;

	/** The point that the conversation camera looks at. */
	FVector GetFocusLocation() const;

	void BeginConversation();
	void Reply(const FString& Message);
	void EndConversation();

	/** Turns towards an actor, or back to the start rotation for null. */
	void FaceTowards(AActor* Target);

	void PlayNpcAnimation(const FString& AnimationId, bool bLoop);

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	UFUNCTION()
	void HandleAnimationRequested(const FString& AnimationId, bool bLoop);

	UAnimSequence* FindAnimation(const FString& AnimationId) const;

	bool bReady = false;
	FString StatusMessage;
	FRotator RestRotation;
	TWeakObjectPtr<AActor> FaceTarget;
	FTimerHandle ReturnToIdleTimer;
};
