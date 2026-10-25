#include "NpcifyCharacterComponent.h"

#include "NpcifyMp3Decoder.h"

#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

UNpcifyCharacterComponent::UNpcifyCharacterComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UNpcifyCharacterComponent::BeginPlay()
{
    Super::BeginPlay();
    RefreshAvatarBindings();
    EnsureInitialized();
    if (BakedConversationBytes.Num() > 0)
    {
        LoadBakedConversationBytes(BakedConversationBytes, bAutoStartBakedConversation);
    }
}

void UNpcifyCharacterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (AudioComponent && AudioComponent->IsPlaying()) AudioComponent->Stop();
    Runtime.Reset();
    Super::EndPlay(EndPlayReason);
}

void UNpcifyCharacterComponent::TickComponent(
    float DeltaTime,
    ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction
)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    EnsureInitialized();
    Runtime->GetExpressions().bAutoBlinkEnabled = bAutoBlink;
    UpdateGazeLayer();
    Runtime->Tick(static_cast<double>(DeltaTime), 0.0f);
    ApplyMorphTargets();
}

void UNpcifyCharacterComponent::Configure(
    const FString& InBaseUrl,
    const FString& InCharacterId,
    const FString& InApiKey
)
{
    Endpoint = InBaseUrl.IsEmpty() ? TEXT("https://api.npcify.ai") : InBaseUrl;
    CharacterId = InCharacterId;
    ApiKey = InApiKey;
    RebuildRuntime();
}

bool UNpcifyCharacterComponent::ConfigureFromJson(const FString& ConfigJson)
{
    TSharedPtr<FJsonObject> JsonObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ConfigJson);
    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        OnError.Broadcast(TEXT("Failed to parse Npcify Unreal SDK JSON config."));
        return false;
    }
    FString ParsedBaseUrl;
    if (JsonObject->TryGetStringField(TEXT("baseUrl"), ParsedBaseUrl) && !ParsedBaseUrl.IsEmpty())
    {
        Endpoint = ParsedBaseUrl;
    }
    JsonObject->TryGetStringField(TEXT("characterId"), CharacterId);
    JsonObject->TryGetStringField(TEXT("apiKey"), ApiKey);
    RebuildRuntime();
    return true;
}

void UNpcifyCharacterComponent::RefreshAvatarBindings()
{
    SkeletalMeshes.Reset();
    CachedMaterials.Reset();
    AActor* Owner = GetOwner();
    if (!Owner) return;

    TInlineComponentArray<USkeletalMeshComponent*> FoundMeshes(Owner);
    for (USkeletalMeshComponent* MeshComp : FoundMeshes)
    {
        if (!IsValid(MeshComp)) continue;
        SkeletalMeshes.Add(MeshComp);
        const int32 NumMats = MeshComp->GetNumMaterials();
        for (int32 Idx = 0; Idx < NumMats; ++Idx)
        {
            if (UMaterialInterface* Mat = MeshComp->GetMaterial(Idx)) CachedMaterials.AddUnique(Mat);
        }
    }

    if (!IsValid(AudioComponent))
    {
        AudioComponent = Owner->FindComponentByClass<UAudioComponent>();
        bOwnsAudioComponent = false;
        if (!IsValid(AudioComponent))
        {
            AudioComponent = NewObject<UAudioComponent>(Owner, TEXT("NpcifyVoiceAudio"));
            if (AudioComponent)
            {
                AudioComponent->SetupAttachment(Owner->GetRootComponent());
                AudioComponent->bAutoActivate = false;
                AudioComponent->RegisterComponent();
                bOwnsAudioComponent = true;
            }
        }
    }
    ApplyVoiceSpatialization();
}

void UNpcifyCharacterComponent::ApplyVoiceSpatialization()
{
    if (!IsValid(AudioComponent)) return;

    AudioComponent->bAllowSpatialization = bSpatialAudio;
    if (!bSpatialAudio)
    {
        AudioComponent->AttenuationSettings = nullptr;
        AudioComponent->bOverrideAttenuation = false;
        return;
    }
    if (VoiceAttenuation)
    {
        AudioComponent->AttenuationSettings = VoiceAttenuation;
        AudioComponent->bOverrideAttenuation = false;
        return;
    }
    const bool bHasOwnAttenuation = AudioComponent->AttenuationSettings != nullptr || AudioComponent->bOverrideAttenuation;
    if (!bOwnsAudioComponent && bHasOwnAttenuation) return;

    // Without attenuation settings Unreal plays a sound in 2D, whatever bAllowSpatialization is.
    FSoundAttenuationSettings& Settings = AudioComponent->AttenuationOverrides;
    Settings.bAttenuate = true;
    Settings.bSpatialize = true;
    Settings.DistanceAlgorithm = EAttenuationDistanceModel::Linear;
    Settings.AttenuationShape = EAttenuationShape::Sphere;
    Settings.AttenuationShapeExtents = FVector(FMath::Max(VoiceInnerRadius, 0.0f), 0.0f, 0.0f);
    Settings.FalloffDistance = FMath::Max(VoiceFalloffDistance, 1.0f);
    AudioComponent->AttenuationSettings = nullptr;
    AudioComponent->bOverrideAttenuation = true;
}

USkeletalMeshComponent* UNpcifyCharacterComponent::GetSkeletalMeshComponent() const
{
    return SkeletalMeshes.Num() > 0 ? SkeletalMeshes[0] : nullptr;
}

FTransform UNpcifyCharacterComponent::GetBoneTransform(FName BoneName) const
{
    if (USkeletalMeshComponent* PrimaryMesh = GetSkeletalMeshComponent())
    {
        const int32 BoneIdx = PrimaryMesh->GetBoneIndex(BoneName);
        if (BoneIdx != INDEX_NONE) return PrimaryMesh->GetBoneTransform(BoneIdx);
    }
    return GetOwner() ? GetOwner()->GetActorTransform() : FTransform::Identity;
}

FVector UNpcifyCharacterComponent::GetBoneLocation(FName BoneName) const
{
    if (USkeletalMeshComponent* PrimaryMesh = GetSkeletalMeshComponent())
    {
        if (PrimaryMesh->GetBoneIndex(BoneName) != INDEX_NONE) return PrimaryMesh->GetBoneLocation(BoneName);
    }
    return GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
}

void UNpcifyCharacterComponent::CreateConversation()
{
    EnsureInitialized();
    if (Runtime->HasBakedConversation()) { StartBakedConversation(TEXT("")); return; }
    SendCreateConversationRequest(TEXT(""));
}

void UNpcifyCharacterComponent::Speak(const FString& Message)
{
    EnsureInitialized();
    if (Message.TrimStartAndEnd().IsEmpty()) { OnError.Broadcast(TEXT("Message must not be empty.")); return; }
    if (Runtime->HasBakedConversation())
    {
        try
        {
            if (Runtime->GetActiveStagingId().empty())
            {
                Runtime->StartBakedConversation();
                Runtime->StopSpeaking();
            }
            const Npcify::FBakedReplyResult Result = Runtime->ReplyBaked(TCHAR_TO_UTF8(*Message));
            if (Result.bEnded) OnConversationEnded.Broadcast();
        }
        catch (const std::exception& Ex) { OnError.Broadcast(UTF8_TO_TCHAR(Ex.what())); }
        return;
    }
    const FString ConvId = UTF8_TO_TCHAR(Runtime->GetConversationId().c_str());
    if (ConvId.IsEmpty()) { SendCreateConversationRequest(Message); return; }
    SendCreateStagingRequest(ConvId, Message, FString(), 0);
}

void UNpcifyCharacterComponent::Reply(const FString& OptionLabel) { Speak(OptionLabel); }

bool UNpcifyCharacterComponent::LoadBakedConversationBytes(const TArray<uint8>& NpcpBytes, bool bAutoStart)
{
    EnsureInitialized();
    try
    {
        std::vector<uint8_t> Bytes(NpcpBytes.GetData(), NpcpBytes.GetData() + NpcpBytes.Num());
        auto Baked = std::make_unique<Npcify::FNpcifyBakedConversation>(
            Npcify::FNpcifyBakedConversation::FromBytes(Bytes, TCHAR_TO_UTF8(*CharacterId))
        );
        Runtime->AttachBakedConversation(std::move(Baked));
        if (bAutoStart) Runtime->StartBakedConversation();
        return true;
    }
    catch (const std::exception& Ex) { OnError.Broadcast(UTF8_TO_TCHAR(Ex.what())); return false; }
}

bool UNpcifyCharacterComponent::StartBakedConversation(const FString& StagingId)
{
    EnsureInitialized();
    try { Runtime->StartBakedConversation(TCHAR_TO_UTF8(*StagingId)); return true; }
    catch (const std::exception& Ex) { OnError.Broadcast(UTF8_TO_TCHAR(Ex.what())); return false; }
}

bool UNpcifyCharacterComponent::SelectBakedStaging(const FString& StagingId)
{
    EnsureInitialized();
    try { Runtime->SelectBakedStaging(TCHAR_TO_UTF8(*StagingId)); return true; }
    catch (const std::exception& Ex) { OnError.Broadcast(UTF8_TO_TCHAR(Ex.what())); return false; }
}

void UNpcifyCharacterComponent::StopSpeaking()
{
    EnsureInitialized();
    if (AudioComponent && AudioComponent->IsPlaying()) AudioComponent->Stop();
    Runtime->StopSpeaking();
}

void UNpcifyCharacterComponent::ResetConversation()
{
    EnsureInitialized();
    StopSpeaking();
    ++ConversationGeneration;
    Runtime->ResetConversationState();
}

void UNpcifyCharacterComponent::SetEmotion(const FString& Expression, float Weight)
{
    EnsureInitialized();
    Runtime->GetExpressions().SetEmotion(TCHAR_TO_UTF8(*Expression), Weight);
}

void UNpcifyCharacterComponent::ClearEmotion()
{
    EnsureInitialized();
    Runtime->GetExpressions().ClearEmotion();
}

void UNpcifyCharacterComponent::PlayExpressionCue(
    const FString& Expression,
    float Weight,
    float DurationSeconds,
    float FadeInSeconds,
    float FadeOutSeconds
)
{
    EnsureInitialized();
    Runtime->GetExpressions().PlayCue(TCHAR_TO_UTF8(*Expression), Weight, DurationSeconds, FadeInSeconds, FadeOutSeconds);
}

void UNpcifyCharacterComponent::PlayAnimation(const FString& AnimationId, bool bLoop)
{
    EnsureInitialized();
    Runtime->PlayAnimation(TCHAR_TO_UTF8(*AnimationId), bLoop);
}

void UNpcifyCharacterComponent::LookAtActor(AActor* TargetActor)
{
    EnsureInitialized();
    GazeTargetActor = TargetActor;
    GazeTargetComponent = nullptr;
    bHasLookAtLocation = false;
    if (!TargetActor) Runtime->ClearLookAt();
}

void UNpcifyCharacterComponent::LookAtComponent(USceneComponent* TargetComponent)
{
    EnsureInitialized();
    GazeTargetComponent = TargetComponent;
    GazeTargetActor = nullptr;
    bHasLookAtLocation = false;
    if (!TargetComponent) Runtime->ClearLookAt();
}

void UNpcifyCharacterComponent::LookAtLocation(FVector WorldLocation)
{
    EnsureInitialized();
    GazeTargetActor = nullptr;
    GazeTargetComponent = nullptr;
    LookAtWorldLocation = WorldLocation;
    bHasLookAtLocation = true;
    UpdateGazeLayer();
}

void UNpcifyCharacterComponent::ClearLookAt()
{
    EnsureInitialized();
    GazeTargetActor = nullptr;
    GazeTargetComponent = nullptr;
    bHasLookAtLocation = false;
    Runtime->ClearLookAt();
}

float UNpcifyCharacterComponent::GetMouthOpenness() const { return Runtime.IsValid() ? Runtime->GetExpressions().GetMouthOpenness() : 0.0f; }
float UNpcifyCharacterComponent::GetExpressionWeight(const FString& ExpressionName) const { return Runtime.IsValid() ? Runtime->GetExpressions().GetWeight(TCHAR_TO_UTF8(*ExpressionName)) : 0.0f; }
bool UNpcifyCharacterComponent::IsSpeaking() const { return Runtime.IsValid() && Runtime->GetSpeechState().bSpeaking; }
bool UNpcifyCharacterComponent::IsThinking() const { return Runtime.IsValid() && Runtime->GetSpeechState().bThinking; }
FString UNpcifyCharacterComponent::GetCurrentText() const { return Runtime.IsValid() ? UTF8_TO_TCHAR(Runtime->GetSpeechState().CurrentText.c_str()) : FString(); }
FString UNpcifyCharacterComponent::GetRevealedText() const { return Runtime.IsValid() ? UTF8_TO_TCHAR(Runtime->GetSpeechState().RevealedText.c_str()) : FString(); }

TArray<FString> UNpcifyCharacterComponent::GetCurrentReplyOptions() const
{
    TArray<FString> Out;
    if (Runtime.IsValid())
    {
        for (const auto& Opt : Runtime->GetSpeechState().Options) Out.Add(UTF8_TO_TCHAR(Opt.Label.c_str()));
    }
    return Out;
}

Npcify::FNpcifyCharacterRuntime& UNpcifyCharacterComponent::GetRuntime() { EnsureInitialized(); return *Runtime; }
void UNpcifyCharacterComponent::EnsureInitialized() { if (!Runtime.IsValid()) RebuildRuntime(); }

void UNpcifyCharacterComponent::RebuildRuntime()
{
    Npcify::FNpcifyOptions Opts;
    Opts.BaseUrl = TCHAR_TO_UTF8(*(Endpoint.IsEmpty() ? TEXT("https://api.npcify.ai") : Endpoint));
    Opts.CharacterId = TCHAR_TO_UTF8(*CharacterId);
    Opts.ApiKey = TCHAR_TO_UTF8(*ApiKey);

    Runtime = MakeUnique<Npcify::FNpcifyCharacterRuntime>(Opts);
    Runtime->GetExpressions().bAutoBlinkEnabled = bAutoBlink;
    Runtime->OnSpeechStart = [this](const Npcify::FStagingResultPayload& Staging)
    {
        OnSpeechStart.Broadcast(UTF8_TO_TCHAR(Staging.StagingId.c_str()), UTF8_TO_TCHAR(Staging.Text.c_str()));
    };
    Runtime->OnSpeechEnd = [this](const Npcify::FStagingResultPayload& Staging)
    {
        OnSpeechEnd.Broadcast(UTF8_TO_TCHAR(Staging.StagingId.c_str()), UTF8_TO_TCHAR(Staging.Text.c_str()));
    };
    Runtime->OnCharacterRevealed = [this](const std::string& Text, const Npcify::FStagingCharacterCue& Cue)
    {
        OnCharacterRevealed.Broadcast(UTF8_TO_TCHAR(Text.c_str()), UTF8_TO_TCHAR(Cue.Character.c_str()));
    };
    Runtime->OnAnimationPlayRequested = [this](const std::string& AnimId, bool bLoop)
    {
        OnAnimationRequested.Broadcast(UTF8_TO_TCHAR(AnimId.c_str()), bLoop);
    };
    Runtime->OnAudioBlock = [this](const Npcify::FStagingDialogueBlock& Block, const Npcify::FNpcifyDecodedAudio& Audio)
    {
        HandleAudioBlock(Block, Audio);
    };
}

void UNpcifyCharacterComponent::UpdateGazeLayer()
{
    FVector TargetPos = FVector::ZeroVector;
    if (IsValid(GazeTargetComponent)) TargetPos = GazeTargetComponent->GetComponentLocation();
    else if (IsValid(GazeTargetActor)) TargetPos = GazeTargetActor->GetActorLocation();
    else if (bHasLookAtLocation) TargetPos = LookAtWorldLocation;
    else { Runtime->ClearLookAt(); return; }

    const FTransform HeadTransform = GetBoneTransform(HeadBoneName);
    const FVector LocalDir = HeadTransform.InverseTransformPosition(TargetPos).GetSafeNormal();
    Runtime->LookAtDirection(
        static_cast<float>(LocalDir.Y),
        static_cast<float>(LocalDir.Z),
        static_cast<float>(LocalDir.X)
    );
}

void UNpcifyCharacterComponent::ApplyMorphTargets()
{
    if (!Runtime.IsValid() || SkeletalMeshes.Num() == 0) return;
    for (USkeletalMeshComponent* MeshComp : SkeletalMeshes)
    {
        if (!IsValid(MeshComp)) continue;
        USkeletalMesh* SkelMesh = MeshComp->GetSkeletalMeshAsset();
        if (!IsValid(SkelMesh)) continue;
        for (const UMorphTarget* Morph : SkelMesh->GetMorphTargets())
        {
            if (!IsValid(Morph)) continue;
            const FName MorphName = Morph->GetFName();
            const float Weight = Runtime->GetExpressions().ResolveMorphTargetWeight01(
                TCHAR_TO_UTF8(*MorphName.ToString())
            );
            MeshComp->SetMorphTarget(MorphName, Weight);
        }
    }
}

void UNpcifyCharacterComponent::HandleAudioBlock(
    const Npcify::FStagingDialogueBlock& Block,
    const Npcify::FNpcifyDecodedAudio& DecodedAudio
)
{
    if (!IsValid(AudioComponent)) return;
    if (!DecodedAudio.Pcm16Samples.empty())
    {
        QueuePcm(
            DecodedAudio.Pcm16Samples.data(),
            static_cast<int32>(DecodedAudio.Pcm16Samples.size()),
            DecodedAudio.SampleRate,
            DecodedAudio.Channels
        );
        return;
    }

    const std::vector<uint8_t>& Encoded = Block.Audio.Data;
    if (Encoded.empty()) return;
    TArray<int16> Pcm;
    int32 SampleRate = 0;
    int32 Channels = 0;
    if (!NpcifyAudio::DecodeMp3(Encoded.data(), static_cast<int32>(Encoded.size()), Pcm, SampleRate, Channels))
    {
        OnError.Broadcast(FString::Printf(TEXT("Cannot decode %s audio."), UTF8_TO_TCHAR(Block.Audio.MimeType.c_str())));
        return;
    }
    QueuePcm(Pcm.GetData(), Pcm.Num(), SampleRate, Channels);
}

void UNpcifyCharacterComponent::QueuePcm(const int16* Samples, int32 NumSamples, int32 InSampleRate, int32 InChannels)
{
    if (NumSamples <= 0) return;
    const int32 SampleRate = FMath::Max(8000, InSampleRate);
    const int32 Channels = FMath::Max(1, InChannels);

    if (!IsValid(ProceduralSoundWave) ||
        ProceduralSoundWave->GetSampleRateForCurrentPlatform() != SampleRate ||
        ProceduralSoundWave->NumChannels != Channels)
    {
        ProceduralSoundWave = NewObject<USoundWaveProcedural>(this);
        ProceduralSoundWave->SetSampleRate(SampleRate);
        ProceduralSoundWave->NumChannels = Channels;
        ProceduralSoundWave->Duration = INDEFINITELY_LOOPING_DURATION;
        ProceduralSoundWave->SoundGroup = SOUNDGROUP_Voice;
        ProceduralSoundWave->bLooping = false;
    }
    else ProceduralSoundWave->ResetAudio();

    ProceduralSoundWave->QueueAudio(reinterpret_cast<const uint8*>(Samples), NumSamples * static_cast<int32>(sizeof(int16)));
    AudioComponent->SetSound(ProceduralSoundWave);
    AudioComponent->SetVolumeMultiplier(Runtime->IsMuted() ? 0.0f : Runtime->GetVolume());
    AudioComponent->Play();
}
