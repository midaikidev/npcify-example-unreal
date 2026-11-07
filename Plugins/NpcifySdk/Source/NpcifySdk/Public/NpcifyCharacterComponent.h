#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/AudioComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWaveProcedural.h"
#include "InputCoreTypes.h"
#include "Core/NpcifyCharacterRuntime.h"
#include "Core/NpcifySpeechInput.h"
#include "NpcifyCharacterComponent.generated.h"

/** How the player can reply (docs/voice-input.md). */
UENUM(BlueprintType)
enum class ENpcifyInputMode : uint8
{
    Options,
    Text,
    Voice,
};

/** How live motion uses the generated root (docs/realtime-motion.md). */
UENUM(BlueprintType)
enum class ENpcifyLiveMotionRootMode : uint8
{
    /** The turn starts over the character's rest position: it stays where the game placed it. */
    Anchored,
    /** The generated hips position, also its horizontal movement. */
    World,
};

UENUM(BlueprintType)
enum class ENpcifyListeningState : uint8
{
    Idle,
    Starting,
    Listening,
    Recognizing,
};

struct FNpcifyVoiceCapture;
struct FNpcifyLiveMotionBinding;
class FNpcifyConversationSocket;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNpcifySpeechEventSignature, const FString&, StagingId, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNpcifyCharacterRevealSignature, const FString&, RevealedText, const FString&, Character);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNpcifyAnimationRequestSignature, const FString&, AnimationId, bool, bLoop);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNpcifyErrorSignature, const FString&, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FNpcifyConversationEndedSignature);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNpcifyListeningStateSignature, ENpcifyListeningState, State);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNpcifyTranscriptSignature, const FString&, Text, bool, bSent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FNpcifyLiveMotionFramesSignature, const FString&, StagingId, int32, StartFrame, int32, FrameCount);

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

    /**
     * How the player can reply: Options, Text and Voice; one mode forces it. Empty: every mode
     * that the character permits. A baked conversation permits only Options, and a character
     * without custom replies permits only Options (docs/voice-input.md).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Voice Input")
    TArray<ENpcifyInputMode> InputModes;

    /** Hold this key to talk. None: call BeginPushToTalk and EndPushToTalk from your own input. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Voice Input")
    FKey PushToTalkKey;

    /** A BCP 47 language hint, for example ja, zh or en. Empty: the server detects the language. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Voice Input")
    FString SpeechLanguage;

    UPROPERTY(BlueprintAssignable, Category="Npcify|Events")
    FNpcifyListeningStateSignature OnListeningStateChanged;

    /** The recognised text of the player's speech; bSent is true when it became the player's message. */
    UPROPERTY(BlueprintAssignable, Category="Npcify|Events")
    FNpcifyTranscriptSignature OnTranscript;

    /**
     * Starts recording the default microphone. Listening ends with StopListening, after
     * MaxDurationSeconds (30 at most), or after EndOnSilenceSeconds of silence that follows
     * speech (0: off). At the end the recording is recognised and, with bSend, its text is sent
     * as the player's message. Needs an active live conversation.
     */
    UFUNCTION(BlueprintCallable, Category="Npcify|Voice Input")
    bool StartListening(float MaxDurationSeconds = 30.0f, float EndOnSilenceSeconds = 0.0f, bool bSend = true);

    /** Ends the recording and sends it for recognition. */
    UFUNCTION(BlueprintCallable, Category="Npcify|Voice Input")
    void StopListening();

    /** Discards the recording. Nothing is recognised or charged. */
    UFUNCTION(BlueprintCallable, Category="Npcify|Voice Input")
    void CancelListening();

    /** Press of a push-to-talk control: the key, a UI button or an Enhanced Input action. */
    UFUNCTION(BlueprintCallable, Category="Npcify|Voice Input")
    void BeginPushToTalk();

    /** Release of a push-to-talk control. A press shorter than 0.3 s sends nothing. */
    UFUNCTION(BlueprintCallable, Category="Npcify|Voice Input")
    void EndPushToTalk();

    /** Sends a recognised text that StartListening did not send (bSend false). Needs Voice. */
    UFUNCTION(BlueprintCallable, Category="Npcify|Voice Input")
    void SendTranscript(const FString& Text);

    UFUNCTION(BlueprintPure, Category="Npcify|Voice Input")
    ENpcifyListeningState GetListeningState() const { return ListeningState; }

    /** The input modes in use. */
    UFUNCTION(BlueprintPure, Category="Npcify|Voice Input")
    TArray<ENpcifyInputMode> GetInputModes() const;

    /**
     * Play the generated body motion of each reply when the character has Live motion on
     * (docs/realtime-motion.md). Off: the character keeps its prepared animation clips.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Live Motion")
    bool bLiveMotion = true;

    /** Anchored keeps the character where the game placed it; World applies the generated hips movement. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Live Motion")
    ENpcifyLiveMotionRootMode LiveMotionRootMode = ENpcifyLiveMotionRootMode::Anchored;

    /**
     * VRM humanoid bone (hips, leftUpperArm, ...) to skeleton bone. Empty: the SDK reads the
     * humanoid table that VRM4U stores with the imported mesh.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Live Motion")
    TMap<FString, FName> LiveMotionBones;

    /** The mesh that plays live motion. Empty: the first skeletal mesh of the owner. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Npcify|Live Motion")
    TObjectPtr<USkeletalMeshComponent> LiveMotionMesh = nullptr;

    /**
     * Warms up live motion for the current conversation on the player's intent: call it when the
     * player starts to type a reply or presses to talk (push-to-talk calls it itself). It runs once
     * per conversation, does nothing without live motion, and never delays the reply.
     */
    UFUNCTION(BlueprintCallable, Category="Npcify|Live Motion")
    void PrepareLiveMotion();

    /** True while turns ask for live motion: it is on, the character has it, and the mesh can play it. */
    UFUNCTION(BlueprintPure, Category="Npcify|Live Motion")
    bool IsLiveMotionReady() const;

    /** The live motion weight now: 0 (the animation) to 1 (live frames). */
    UFUNCTION(BlueprintPure, Category="Npcify|Live Motion")
    float GetLiveMotionWeight() const;

    /** Frames of a staging: received, played, and the staging time of the first played frame (-1: none). */
    UFUNCTION(BlueprintPure, Category="Npcify|Live Motion")
    bool GetLiveMotionStats(const FString& StagingId, int32& FramesReceived, int32& FramesPlayed, float& FirstFrameTime) const;

    /** Live motion frames arrived for a staging: frame StartFrame plays at StartFrame / 30 s of its clock. */
    UPROPERTY(BlueprintAssignable, Category="Npcify|Events")
    FNpcifyLiveMotionFramesSignature OnLiveMotionFrames;

    /** The staging that plays or played last. */
    UFUNCTION(BlueprintPure, Category="Npcify|Conversation")
    FString GetActiveStagingId() const;

    /** The skeleton bone of a VRM humanoid bone (hips, rightHand, ...) of the live motion mesh, or None. */
    UFUNCTION(BlueprintPure, Category="Npcify|Live Motion")
    FName GetHumanoidBoneName(const FString& VrmBone) const;

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
    /** Requests a turn; with live motion it joins the conversation stream first. */
    void SendTurnRequest(const FString& ConversationIdStr, const FString& Message);
    void SendCreateStagingRequest(
        const FString& ConversationIdStr,
        const FString& Message,
        const FString& IdempotencyKey,
        int32 Attempt,
        bool bWithLiveMotion
    );
    /** Live motion: NpcifyCharacterComponentLiveMotion.cpp. */
    bool WantsLiveMotion() const;
    void JoinLiveMotionStream(const FString& ConversationIdStr, TFunction<void(bool)> OnDone);
    void HandleLiveMotionEvent(const Npcify::FNpcifyMsgPackValue& Event);
    void DownloadMotionTrack(const std::string& StagingId, const Npcify::FStagingMotionTrack& Track);
    void UpdateLiveMotion(float DeltaTime);
    void BindLiveMotionMesh();
    void UnbindLiveMotionMesh();
    UFUNCTION()
    void HandleBoneTransformsFinalized();
    /** Sends a reply after the input-mode checks of Speak, Reply or SendTranscript. */
    void SendReply(const FString& Message);
    /** Voice input: NpcifyCharacterComponentVoice.cpp. */
    static std::vector<Npcify::EInputMode> ToCoreInputModes(const TArray<ENpcifyInputMode>& Modes);
    std::vector<Npcify::EInputMode> CurrentInputModes() const;
    bool RequireInputMode(Npcify::EInputMode Mode, const TCHAR* Reason);
    void CheckInputModesWithCharacter(TFunction<void()> OnPermitted);
    void TickVoiceInput();
    void SetListeningState(ENpcifyListeningState State);
    void CloseMicrophone();
    void FinishListening();
    void SendSpeechRequest(TArray<uint8> Wav, bool bSend);

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
    TSharedPtr<FNpcifyVoiceCapture> VoiceCapture;
    ENpcifyListeningState ListeningState = ENpcifyListeningState::Idle;
    double ListeningStartedAt = 0.0;
    float ListeningMaxSeconds = 30.0f;
    bool bSendTranscript = true;
    bool bPushToTalkActive = false;
    double PushToTalkPressedAt = 0.0;
    /** InputModes after the check with the character's settings. */
    bool bInputModesChecked = false;
    std::vector<Npcify::EInputMode> CheckedInputModes;
    /** The owner's voice input (speechInputEnabled), after the character's info was read. */
    bool bSpeechInputKnown = false;
    bool bSpeechInputEnabled = false;
    bool bHasLookAtLocation = false;
    /** Increments on reset, so that late HTTP responses of an old conversation are ignored. */
    int32 ConversationGeneration = 0;
    TSharedPtr<FNpcifyConversationSocket> LiveMotionSocket;
    TSharedPtr<FNpcifyLiveMotionBinding> LiveMotionBinding;
    FString LiveMotionPreparedConversation;
    /** A turn stopped waiting for the stream of this conversation; later turns do not wait for it. */
    FString LiveMotionJoinGivenUp;
    /** The character's live motion switch from its info, once read. */
    bool bLiveMotionInfoKnown = false;
    bool bLiveMotionInEffect = false;
    FVector LookAtWorldLocation = FVector::ZeroVector;
};
