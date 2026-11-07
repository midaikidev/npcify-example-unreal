#include "NpcifyCharacterComponent.h"

#include "AudioCapture.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"

#include <stdexcept>

// Voice input of UNpcifyCharacterComponent (docs/voice-input.md): the microphone records while
// the player holds PushToTalkKey, or until StopListening. The recording goes to
// POST /conversations/:id/speech as one WAV file, and its text becomes the player's message.

/** The microphone of one listening. The capture thread appends samples under the lock. */
struct FNpcifyVoiceCapture
{
    TStrongObjectPtr<UAudioCapture> Capture;
    FAudioGeneratorHandle Handle;
    FCriticalSection Lock;
    /** Interleaved samples of the whole recording. */
    TArray<float> Samples;
    /** Samples that the silence rule has not seen. */
    TArray<float> Pending;
    int32 SampleRate = 0;
    int32 Channels = 0;
    TOptional<Npcify::FNpcifySilenceDetector> Silence;
};

namespace
{
    Npcify::EInputMode ToCore(ENpcifyInputMode Mode)
    {
        switch (Mode)
        {
            case ENpcifyInputMode::Options:
                return Npcify::EInputMode::Options;
            case ENpcifyInputMode::Text:
                return Npcify::EInputMode::Text;
            default:
                return Npcify::EInputMode::Voice;
        }
    }

    ENpcifyInputMode FromCore(Npcify::EInputMode Mode)
    {
        switch (Mode)
        {
            case Npcify::EInputMode::Options:
                return ENpcifyInputMode::Options;
            case Npcify::EInputMode::Text:
                return ENpcifyInputMode::Text;
            default:
                return ENpcifyInputMode::Voice;
        }
    }

    std::vector<Npcify::EInputMode> ToCore(const TArray<ENpcifyInputMode>& Modes)
    {
        std::vector<Npcify::EInputMode> Out;
        for (const ENpcifyInputMode Mode : Modes)
        {
            Out.push_back(ToCore(Mode));
        }
        return Out;
    }

    void SetAuthHeaders(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request, const std::string& Token)
    {
        if (!Token.empty())
        {
            const FString TokenStr = UTF8_TO_TCHAR(Token.c_str());
            Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *TokenStr));
            Request->SetHeader(TEXT("X-API-Token"), TokenStr);
        }
    }

    FString DescribeFailure(const FString& Prefix, const FHttpResponsePtr& Response)
    {
        if (!Response.IsValid())
        {
            return Prefix + TEXT(": no connection.");
        }
        FString Message;
        TSharedPtr<FJsonObject> Json;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
        if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
        {
            Json->TryGetStringField(TEXT("message"), Message);
        }
        return FString::Printf(TEXT("%s (HTTP %d). %s"), *Prefix, Response->GetResponseCode(), *Message).TrimEnd();
    }

    /** 30 seconds of 48 kHz stereo, the largest recording that the server takes after resampling. */
    constexpr int32 MaxCapturedSamples = 48000 * 2 * 31;
}

std::vector<Npcify::EInputMode> UNpcifyCharacterComponent::ToCoreInputModes(const TArray<ENpcifyInputMode>& Modes)
{
    return ToCore(Modes);
}

std::vector<Npcify::EInputMode> UNpcifyCharacterComponent::CurrentInputModes() const
{
    if (bInputModesChecked)
    {
        return CheckedInputModes;
    }
    if (InputModes.Num() > 0)
    {
        return ToCore(InputModes);
    }
    return Npcify::PermittedInputModes(true, Runtime && Runtime->HasBakedConversation(), true);
}

TArray<ENpcifyInputMode> UNpcifyCharacterComponent::GetInputModes() const
{
    TArray<ENpcifyInputMode> Out;
    for (const Npcify::EInputMode Mode : CurrentInputModes())
    {
        Out.Add(FromCore(Mode));
    }
    return Out;
}

bool UNpcifyCharacterComponent::RequireInputMode(Npcify::EInputMode Mode, const TCHAR* Reason)
{
    if (Mode == Npcify::EInputMode::Voice && Runtime->HasBakedConversation())
    {
        OnError.Broadcast(TEXT("Voice input is not available in a baked (offline) conversation."));
        return false;
    }
    if (Mode == Npcify::EInputMode::Voice && bSpeechInputKnown && !bSpeechInputEnabled)
    {
        OnError.Broadcast(TEXT("Voice input is off for this character. Its owner turns it on in the STT tab."));
        return false;
    }
    // Without a configuration the server's rules apply alone, as before.
    if (InputModes.Num() == 0)
    {
        return true;
    }
    const std::string Error = Npcify::CheckInputMode(CurrentInputModes(), Mode, TCHAR_TO_UTF8(Reason));
    if (!Error.empty())
    {
        OnError.Broadcast(UTF8_TO_TCHAR(Error.c_str()));
        return false;
    }
    return true;
}

void UNpcifyCharacterComponent::CheckInputModesWithCharacter(TFunction<void()> OnPermitted)
{
    if (bInputModesChecked)
    {
        OnPermitted();
        return;
    }
    const bool bConfigured = InputModes.Num() > 0;
    const std::string BaseUrl = Runtime->GetOptions().BaseUrl;
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(FString::Printf(
        TEXT("%s/avatars/%s/info"),
        UTF8_TO_TCHAR(BaseUrl.c_str()),
        *FGenericPlatformHttp::UrlEncode(CharacterId)
    ));
    Request->SetVerb(TEXT("GET"));
    Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
    SetAuthHeaders(Request, Runtime->GetOptions().ResolvedToken());
    TWeakObjectPtr<UNpcifyCharacterComponent> WeakThis(this);
    const int32 Generation = ConversationGeneration;
    Request->OnProcessRequestComplete().BindLambda(
        [WeakThis, Generation, OnPermitted, bConfigured](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
        {
            if (!WeakThis.IsValid() || WeakThis->ConversationGeneration != Generation) return;
            UNpcifyCharacterComponent* Self = WeakThis.Get();
            TSharedPtr<FJsonObject> Json;
            if (!bConnected || !Response.IsValid() || !EHttpResponseCodes::IsOk(Response->GetResponseCode()) ||
                !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Json) ||
                !Json.IsValid())
            {
                if (!bConfigured)
                {
                    // Without a configuration the server's rules apply, as before.
                    OnPermitted();
                    return;
                }
                Self->OnError.Broadcast(DescribeFailure(TEXT("Could not check InputModes with the character"), Response));
                return;
            }
            bool bAllowCustomReply = true;
            bool bBaked = false;
            bool bSpeechInput = false;
            Json->TryGetBoolField(TEXT("allowCustomReply"), bAllowCustomReply);
            Json->TryGetBoolField(TEXT("bakedConversationEnabled"), bBaked);
            Json->TryGetBoolField(TEXT("speechInputEnabled"), bSpeechInput);
            bool bRealtimeMotion = false;
            Json->TryGetBoolField(TEXT("realtimeMotionEnabled"), bRealtimeMotion);
            Self->bLiveMotionInfoKnown = true;
            Self->bLiveMotionInEffect = bRealtimeMotion;
            Self->bSpeechInputKnown = true;
            Self->bSpeechInputEnabled = bSpeechInput;
            try
            {
                Self->CheckedInputModes =
                    Npcify::ResolveInputModes(ToCore(Self->InputModes), bAllowCustomReply, bBaked, bSpeechInput);
                Self->bInputModesChecked = true;
            }
            catch (const std::exception& Ex)
            {
                Self->OnError.Broadcast(UTF8_TO_TCHAR(Ex.what()));
                return;
            }
            OnPermitted();
        }
    );
    Request->ProcessRequest();
}

bool UNpcifyCharacterComponent::StartListening(float MaxDurationSeconds, float EndOnSilenceSeconds, bool bSend)
{
    EnsureInitialized();
    if (ListeningState != ENpcifyListeningState::Idle)
    {
        OnError.Broadcast(TEXT("Listening has already started."));
        return false;
    }
    if (!RequireInputMode(Npcify::EInputMode::Voice, TEXT("Voice input is off")))
    {
        return false;
    }
    if (Runtime->GetConversationId().empty())
    {
        OnError.Broadcast(TEXT("Create a conversation before listening."));
        return false;
    }
    UAudioCapture* Capture = UAudioCaptureFunctionLibrary::CreateAudioCapture();
    if (Capture == nullptr)
    {
        OnError.Broadcast(TEXT("MICROPHONE_UNAVAILABLE: No microphone is available."));
        return false;
    }
    VoiceCapture = MakeShared<FNpcifyVoiceCapture>();
    VoiceCapture->Capture.Reset(Capture);
    VoiceCapture->SampleRate = Capture->GetSampleRate();
    VoiceCapture->Channels = FMath::Max(1, Capture->GetNumChannels());
    if (EndOnSilenceSeconds > 0.0f)
    {
        VoiceCapture->Silence.Emplace(EndOnSilenceSeconds);
    }
    TWeakPtr<FNpcifyVoiceCapture> WeakCapture = VoiceCapture;
    VoiceCapture->Handle = Capture->AddGeneratorDelegate(
        [WeakCapture](const float* InAudio, int32 NumSamples)
        {
            if (TSharedPtr<FNpcifyVoiceCapture> State = WeakCapture.Pin())
            {
                FScopeLock Lock(&State->Lock);
                if (State->Samples.Num() + NumSamples <= MaxCapturedSamples)
                {
                    State->Samples.Append(InAudio, NumSamples);
                    State->Pending.Append(InAudio, NumSamples);
                }
            }
        }
    );
    Capture->StartCapturingAudio();
    // A talk press is the player's intent to reply: warm up live motion.
    PrepareLiveMotion();
    ListeningMaxSeconds = FMath::Clamp(MaxDurationSeconds, 1.0f, Npcify::SpeechAudio::MaxDurationSeconds);
    bSendTranscript = bSend;
    ListeningStartedAt = FPlatformTime::Seconds();
    SetListeningState(ENpcifyListeningState::Listening);
    return true;
}

void UNpcifyCharacterComponent::StopListening()
{
    if (ListeningState == ENpcifyListeningState::Listening)
    {
        FinishListening();
    }
}

void UNpcifyCharacterComponent::CancelListening()
{
    if (ListeningState == ENpcifyListeningState::Listening || ListeningState == ENpcifyListeningState::Starting)
    {
        CloseMicrophone();
        SetListeningState(ENpcifyListeningState::Idle);
    }
}

void UNpcifyCharacterComponent::BeginPushToTalk()
{
    if (bPushToTalkActive || ListeningState != ENpcifyListeningState::Idle)
    {
        return;
    }
    bPushToTalkActive = true;
    PushToTalkPressedAt = FPlatformTime::Seconds();
    // Push-to-talk ends at the release, never at a silence.
    if (!StartListening(Npcify::SpeechAudio::MaxDurationSeconds, 0.0f, true))
    {
        bPushToTalkActive = false;
    }
}

void UNpcifyCharacterComponent::EndPushToTalk()
{
    if (!bPushToTalkActive)
    {
        return;
    }
    bPushToTalkActive = false;
    if (FPlatformTime::Seconds() - PushToTalkPressedAt < Npcify::SpeechAudio::MinHoldSeconds)
    {
        CancelListening();
        return;
    }
    StopListening();
}

void UNpcifyCharacterComponent::SendTranscript(const FString& Text)
{
    EnsureInitialized();
    if (!RequireInputMode(Npcify::EInputMode::Voice, TEXT("Voice input is off")))
    {
        return;
    }
    const FString ConversationIdStr = UTF8_TO_TCHAR(Runtime->GetConversationId().c_str());
    if (ConversationIdStr.IsEmpty() || Text.TrimStartAndEnd().IsEmpty())
    {
        OnError.Broadcast(TEXT("A conversation and a text are needed."));
        return;
    }
    SendReply(Text);
}

void UNpcifyCharacterComponent::TickVoiceInput()
{
    if (PushToTalkKey.IsValid())
    {
        UWorld* World = GetWorld();
        if (APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr)
        {
            if (Controller->WasInputKeyJustPressed(PushToTalkKey))
            {
                BeginPushToTalk();
            }
            else if (Controller->WasInputKeyJustReleased(PushToTalkKey))
            {
                EndPushToTalk();
            }
        }
    }
    if (ListeningState != ENpcifyListeningState::Listening || !VoiceCapture.IsValid())
    {
        return;
    }
    if (VoiceCapture->Silence.IsSet())
    {
        TArray<float> Pending;
        {
            FScopeLock Lock(&VoiceCapture->Lock);
            Pending = MoveTemp(VoiceCapture->Pending);
            VoiceCapture->Pending.Reset();
        }
        const int32 Rate = FMath::Max(1, VoiceCapture->SampleRate);
        if (Pending.Num() > 0 &&
            VoiceCapture->Silence->Push(
                Npcify::SpeechAudio::Level(Pending.GetData(), static_cast<size_t>(Pending.Num())),
                static_cast<float>(Pending.Num()) / static_cast<float>(Rate * VoiceCapture->Channels)
            ))
        {
            FinishListening();
            return;
        }
    }
    if (FPlatformTime::Seconds() - ListeningStartedAt >= ListeningMaxSeconds)
    {
        FinishListening();
    }
}

void UNpcifyCharacterComponent::SetListeningState(ENpcifyListeningState State)
{
    if (ListeningState != State)
    {
        ListeningState = State;
        OnListeningStateChanged.Broadcast(State);
    }
}

void UNpcifyCharacterComponent::CloseMicrophone()
{
    if (VoiceCapture.IsValid())
    {
        if (UAudioCapture* Capture = VoiceCapture->Capture.Get())
        {
            Capture->StopCapturingAudio();
            Capture->RemoveGeneratorDelegate(VoiceCapture->Handle);
        }
        VoiceCapture.Reset();
    }
}

void UNpcifyCharacterComponent::FinishListening()
{
    TArray<float> Samples;
    int32 Rate = Npcify::SpeechAudio::SampleRate;
    int32 Channels = 1;
    if (VoiceCapture.IsValid())
    {
        FScopeLock Lock(&VoiceCapture->Lock);
        Samples = MoveTemp(VoiceCapture->Samples);
        Rate = VoiceCapture->SampleRate > 0 ? VoiceCapture->SampleRate : Rate;
        Channels = VoiceCapture->Channels;
    }
    CloseMicrophone();
    SetListeningState(ENpcifyListeningState::Recognizing);
    const std::vector<int16_t> Pcm = Npcify::SpeechAudio::ToPcm16Mono16k(
        Samples.GetData(),
        static_cast<size_t>(Samples.Num()),
        Channels,
        Rate
    );
    // Less than 100 ms is no speech: nothing is sent or charged.
    if (Pcm.size() < static_cast<size_t>(Npcify::SpeechAudio::SampleRate / 10))
    {
        SetListeningState(ENpcifyListeningState::Idle);
        OnTranscript.Broadcast(FString(), false);
        return;
    }
    const std::vector<uint8_t> Wav = Npcify::SpeechAudio::EncodeWav(Pcm);
    SendSpeechRequest(TArray<uint8>(Wav.data(), static_cast<int32>(Wav.size())), bSendTranscript);
}

void UNpcifyCharacterComponent::SendSpeechRequest(TArray<uint8> Wav, bool bSend)
{
    const std::string BaseUrl = Runtime->GetOptions().BaseUrl;
    const FString ConversationIdStr = UTF8_TO_TCHAR(Runtime->GetConversationId().c_str());
    FString Url = FString::Printf(
        TEXT("%s/conversations/%s/speech"),
        UTF8_TO_TCHAR(BaseUrl.c_str()),
        *ConversationIdStr
    );
    const FString Language = SpeechLanguage.TrimStartAndEnd();
    if (!Language.IsEmpty())
    {
        Url += TEXT("?language=") + FGenericPlatformHttp::UrlEncode(Language);
    }
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(Url);
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("audio/wav"));
    Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
    SetAuthHeaders(Request, Runtime->GetOptions().ResolvedToken());
    Request->SetContent(MoveTemp(Wav));
    TWeakObjectPtr<UNpcifyCharacterComponent> WeakThis(this);
    const int32 Generation = ConversationGeneration;
    Request->OnProcessRequestComplete().BindLambda(
        [WeakThis, Generation, ConversationIdStr, bSend](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
        {
            if (!WeakThis.IsValid()) return;
            UNpcifyCharacterComponent* Self = WeakThis.Get();
            Self->SetListeningState(ENpcifyListeningState::Idle);
            if (Self->ConversationGeneration != Generation) return;
            TSharedPtr<FJsonObject> Json;
            if (!bConnected || !Response.IsValid() || !EHttpResponseCodes::IsOk(Response->GetResponseCode()) ||
                !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Json) ||
                !Json.IsValid())
            {
                Self->OnError.Broadcast(DescribeFailure(TEXT("Npcify speech recognition failed"), Response));
                return;
            }
            FString Text;
            Json->TryGetStringField(TEXT("text"), Text);
            Text.TrimStartAndEndInline();
            const bool bSent = bSend && !Text.IsEmpty();
            Self->OnTranscript.Broadcast(Text, bSent);
            if (bSent)
            {
                Self->SendTurnRequest(ConversationIdStr, Text);
            }
        }
    );
    Request->ProcessRequest();
}
