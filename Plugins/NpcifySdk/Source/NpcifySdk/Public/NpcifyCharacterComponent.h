#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/AudioComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWaveProcedural.h"
#include "Core/NpcifyCharacterRuntime.h"
#include "NpcifyCharacterComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNpcifySpeechEventSignature, const FString&, StagingId, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNpcifyCharacterRevealSignature, const FString&, RevealedText, const FString&, Character);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNpcifyAnimationRequestSignature, const FString&, AnimationId, bool, bLoop);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNpcifyErrorSignature, const FString&, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FNpcifyConversationEndedSignature);

UCLASS(ClassGroup=(Npcify), meta=(BlueprintSpawnableComponent, DisplayName="Npcify Character"))
class NPCIFYSDK_API UNpcifyCharacterComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UNpcifyCharacterComponent();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Connection")
    FString CharacterId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Connection")
    FString ApiKey;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Connection")
    FString Endpoint = TEXT("https://api.npcify.ai");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Rig & Audio")
    AActor* GazeTargetActor = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Rig & Audio")
    USceneComponent* GazeTargetComponent = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Rig & Audio")
    FName HeadBoneName = TEXT("head");

    /** The voice comes from the character: left/right panning and distance falloff. Off: a 2D voice. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Rig & Audio")
    bool bSpatialAudio = true;

    /** Attenuation asset for the voice. Empty: the SDK uses VoiceInnerRadius and VoiceFalloffDistance. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Rig & Audio", meta=(EditCondition="bSpatialAudio"))
    TObjectPtr<USoundAttenuation> VoiceAttenuation = nullptr;

    /** Full voice volume within this distance from the character, in cm. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Rig & Audio", meta=(EditCondition="bSpatialAudio", ClampMin="0"))
    float VoiceInnerRadius = 250.0f;

    /** Distance after the inner radius over which the voice fades to silence (linear), in cm. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Rig & Audio", meta=(EditCondition="bSpatialAudio", ClampMin="1"))
    float VoiceFalloffDistance = 2750.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Rig & Audio")
    bool bAutoBlink = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Offline Baked Conversation")
    TArray<uint8> BakedConversationBytes;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Offline Baked Conversation")
    bool bAutoStartBakedConversation = false;

    UPROPERTY(BlueprintAssignable, Category="Npcify|Events")
    FNpcifySpeechEventSignature OnSpeechStart;

    UPROPERTY(BlueprintAssignable, Category="Npcify|Events")
    FNpcifySpeechEventSignature OnSpeechEnd;

    UPROPERTY(BlueprintAssignable, Category="Npcify|Events")
    FNpcifyCharacterRevealSignature OnCharacterRevealed;

    UPROPERTY(BlueprintAssignable, Category="Npcify|Events")
    FNpcifyAnimationRequestSignature OnAnimationRequested;

    UPROPERTY(BlueprintAssignable, Category="Npcify|Events")
    FNpcifyErrorSignature OnError;

    /** Raised when the player picks a reply option that ends a baked conversation. */
    UPROPERTY(BlueprintAssignable, Category="Npcify|Events")
    FNpcifyConversationEndedSignature OnConversationEnded;

    UFUNCTION(BlueprintCallable, Category="Npcify|Character")
    void Configure(const FString& InBaseUrl, const FString& InCharacterId, const FString& InApiKey);

    UFUNCTION(BlueprintCallable, Category="Npcify|Character")
    bool ConfigureFromJson(const FString& ConfigJson);

    UFUNCTION(BlueprintCallable, Category="Npcify|Character")
    void RefreshAvatarBindings();

    UFUNCTION(BlueprintPure, Category="Npcify|Character")
    USkeletalMeshComponent* GetSkeletalMeshComponent() const;

    UFUNCTION(BlueprintPure, Category="Npcify|Character")
    TArray<USkeletalMeshComponent*> GetSkeletalMeshComponents() const { return SkeletalMeshes; }

    UFUNCTION(BlueprintPure, Category="Npcify|Character")
    TArray<UMaterialInterface*> GetMaterials() const { return CachedMaterials; }

    UFUNCTION(BlueprintPure, Category="Npcify|Character")
    UAudioComponent* GetAudioComponent() const { return AudioComponent; }

    /**
     * Applies bSpatialAudio, VoiceAttenuation and the voice distances to the voice audio component.
     * Call it after you change these properties at run time. An audio component of the owner that
     * already has its own attenuation keeps it, unless VoiceAttenuation is set.
     */
    UFUNCTION(BlueprintCallable, Category="Npcify|Rig & Audio")
    void ApplyVoiceSpatialization();

    UFUNCTION(BlueprintPure, Category="Npcify|Character")
    FTransform GetBoneTransform(FName BoneName) const;

    UFUNCTION(BlueprintPure, Category="Npcify|Character")
    FVector GetBoneLocation(FName BoneName) const;

    UFUNCTION(BlueprintCallable, Category="Npcify|Conversation")
    void CreateConversation();

    UFUNCTION(BlueprintCallable, Category="Npcify|Conversation")
    void Speak(const FString& Message);

    UFUNCTION(BlueprintCallable, Category="Npcify|Conversation")
    void Reply(const FString& OptionLabel);

    UFUNCTION(BlueprintCallable, Category="Npcify|Conversation")
    bool LoadBakedConversationBytes(const TArray<uint8>& NpcpBytes, bool bAutoStart = false);

    UFUNCTION(BlueprintCallable, Category="Npcify|Conversation")
    bool StartBakedConversation(const FString& StagingId = TEXT(""));

    UFUNCTION(BlueprintCallable, Category="Npcify|Conversation")
    bool SelectBakedStaging(const FString& StagingId);

    UFUNCTION(BlueprintCallable, Category="Npcify|Character")
    void StopSpeaking();

    /** Stops speech and forgets the active conversation. The next call starts a new one. */
    UFUNCTION(BlueprintCallable, Category="Npcify|Conversation")
    void ResetConversation();

    UFUNCTION(BlueprintCallable, Category="Npcify|Expressions")
    void SetEmotion(const FString& Expression, float Weight = 1.0f);

    UFUNCTION(BlueprintCallable, Category="Npcify|Expressions")
    void ClearEmotion();

    UFUNCTION(BlueprintCallable, Category="Npcify|Expressions")
    void PlayExpressionCue(
        const FString& Expression,
        float Weight = 1.0f,
        float DurationSeconds = 1.2f,
        float FadeInSeconds = 0.15f,
        float FadeOutSeconds = 0.25f
    );

    UFUNCTION(BlueprintCallable, Category="Npcify|Animation")
    void PlayAnimation(const FString& AnimationId, bool bLoop = false);

    UFUNCTION(BlueprintCallable, Category="Npcify|Gaze")
    void LookAtActor(AActor* TargetActor);

    UFUNCTION(BlueprintCallable, Category="Npcify|Gaze")
    void LookAtComponent(USceneComponent* TargetComponent);

    UFUNCTION(BlueprintCallable, Category="Npcify|Gaze")
    void LookAtLocation(FVector WorldLocation);

    UFUNCTION(BlueprintCallable, Category="Npcify|Gaze")
    void ClearLookAt();

    UFUNCTION(BlueprintPure, Category="Npcify|State")
    float GetMouthOpenness() const;

    UFUNCTION(BlueprintPure, Category="Npcify|State")
    float GetExpressionWeight(const FString& ExpressionName) const;

    UFUNCTION(BlueprintPure, Category="Npcify|State")
    bool IsSpeaking() const;

    UFUNCTION(BlueprintPure, Category="Npcify|State")
    bool IsThinking() const;

    UFUNCTION(BlueprintPure, Category="Npcify|State")
    FString GetCurrentText() const;

    UFUNCTION(BlueprintPure, Category="Npcify|State")
    FString GetRevealedText() const;

    UFUNCTION(BlueprintPure, Category="Npcify|State")
    TArray<FString> GetCurrentReplyOptions() const;

    Npcify::FNpcifyCharacterRuntime& GetRuntime();

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(
        float DeltaTime,
        ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction
    ) override;

private:
    void EnsureInitialized();
    void RebuildRuntime();
    void UpdateGazeLayer();
    void ApplyMorphTargets();
    void HandleAudioBlock(const Npcify::FStagingDialogueBlock& Block, const Npcify::FNpcifyDecodedAudio& DecodedAudio);
    void QueuePcm(const int16* Samples, int32 NumSamples, int32 SampleRate, int32 Channels);
    /** Downloads audio that the staging references by URL, then plays the staging. */
    void PrepareAndPlayStaging(const Npcify::FStagingResultPayload& Staging);
    void SendCreateConversationRequest(const FString& FollowUpMessage);
    void SendCreateStagingRequest(
        const FString& ConversationIdStr,
        const FString& Message,
        const FString& IdempotencyKey,
        int32 Attempt
    );

    UPROPERTY(Transient)
    TArray<USkeletalMeshComponent*> SkeletalMeshes;

    UPROPERTY(Transient)
    TArray<UMaterialInterface*> CachedMaterials;

    UPROPERTY(Transient)
    UAudioComponent* AudioComponent = nullptr;

    /** True when the SDK made the voice audio component (not a component of the owner). */
    bool bOwnsAudioComponent = false;

    UPROPERTY(Transient)
    USoundWaveProcedural* ProceduralSoundWave = nullptr;

    TUniquePtr<Npcify::FNpcifyCharacterRuntime> Runtime;
    bool bHasLookAtLocation = false;
    /** Increments on reset, so that late HTTP responses of an old conversation are ignored. */
    int32 ConversationGeneration = 0;
    FVector LookAtWorldLocation = FVector::ZeroVector;
};
