#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "NpcifyDialogueDirector.generated.h"

class ACameraActor;
class ANpcifyNpc;
class UNpcifyDialogueWidget;

/**
 * Finds the NPC near the player, shows the interaction prompt, and runs the
 * conversation: speech with typewriter text, then numbered reply choices.
 */
UCLASS()
class NPCIFYEXAMPLE_API ANpcifyDialogueDirector : public AActor
{
	GENERATED_BODY()

public:
	ANpcifyDialogueDirector();

	UPROPERTY(EditAnywhere, Category="Npcify")
	float InteractDistance = 260.f;

	const TArray<TObjectPtr<ANpcifyNpc>>& GetNpcs() const { return Npcs; }
	ANpcifyNpc* GetActiveNpc() const { return Active; }
	bool IsChoosing() const { return Phase == EPhase::Choosing; }
	const TArray<FString>& GetChoices() const { return Choices; }

	void TryOpenNearby();
	void Choose(int32 Index);
	void Close();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	enum class EPhase : uint8
	{
		Exploring,
		Waiting,
		Speaking,
		Choosing,
	};

	void UpdateLabels();
	void UpdateExploring();
	void UpdateConversation(float DeltaSeconds);
	void UpdateCamera(float DeltaSeconds, bool bSnap);
	void Open(ANpcifyNpc* Npc);
	void SendReply(const FString& Message);
	void EnterWaiting();
	void EnterChoosing();
	void ShowError(const FString& Message);
	void SetPlayerControl(bool bEnabled);
	void ConfigureShoulderCamera();
	ANpcifyNpc* FindNearbyNpc() const;
	bool Project(const FVector& World, FVector2D& OutScreen) const;
	APlayerController* GetPlayerController() const;
	APawn* GetPlayerPawn() const;

	UFUNCTION()
	void HandleSpeechStart(const FString& StagingId, const FString& Text);

	UFUNCTION()
	void HandleSpeechEnd(const FString& StagingId, const FString& Text);

	UFUNCTION()
	void HandleError(const FString& Message);

	UFUNCTION()
	void HandleConversationEnded();

	UPROPERTY(Transient)
	TArray<TObjectPtr<ANpcifyNpc>> Npcs;

	UPROPERTY(Transient)
	TObjectPtr<ANpcifyNpc> Nearby;

	UPROPERTY(Transient)
	TObjectPtr<ANpcifyNpc> Active;

	UPROPERTY(Transient)
	TObjectPtr<UNpcifyDialogueWidget> Widget;

	UPROPERTY(Transient)
	TObjectPtr<ACameraActor> DialogueCamera;

	EPhase Phase = EPhase::Exploring;
	TArray<FString> Choices;
	FString LastSpokenText;
	FString PendingRetry;
	bool bRetryOpens = false;
	bool bEnding = false;
	bool bPlayerConfigured = false;
};
