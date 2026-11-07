#include "NpcifyCharacterComponent.h"

#include "NpcifyConversationSocket.h"

#include "Containers/Ticker.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Guid.h"

// HTTP requests of UNpcifyCharacterComponent: live conversations and speech audio downloads.

namespace
{
    /** The longest time that a turn waits for the conversation stream to join. */
    constexpr float TurnJoinWaitSeconds = 3.0f;

    /** RFC 4122 version 4 UUID; the server rejects other Idempotency-Key formats. */
    FString MakeUuidV4()
    {
        FGuid Guid = FGuid::NewGuid();
        Guid.B = (Guid.B & 0xFFFF0FFFu) | 0x00004000u;
        Guid.C = (Guid.C & 0x3FFFFFFFu) | 0x80000000u;
        return Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
    }

    FString SerializeJson(const TSharedRef<FJsonObject>& Object)
    {
        FString Out;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Object, Writer);
        return Out;
    }

    FString DescribeHttpFailure(const FString& Prefix, const FHttpResponsePtr& Response)
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
}

void UNpcifyCharacterComponent::PrepareAndPlayStaging(const Npcify::FStagingResultPayload& Staging, uint64_t PlaySequence)
{
    TSharedRef<Npcify::FStagingResultPayload> Shared = MakeShared<Npcify::FStagingResultPayload>(Staging);
    TArray<int32> Pending;
    for (int32 Index = 0; Index < static_cast<int32>(Shared->Timeline.Dialogue.size()); ++Index)
    {
        const Npcify::FStagingAudioSource& Audio = Shared->Timeline.Dialogue[Index].Audio;
        if (Audio.Data.empty() && !Audio.Url.empty()) Pending.Add(Index);
    }
    if (Pending.Num() == 0)
    {
        Runtime->PlayTurnStaging(*Shared, PlaySequence);
        return;
    }

    // Speech starts after every audio file is downloaded, so audio and lip-sync stay in step.
    TSharedRef<int32> Remaining = MakeShared<int32>(Pending.Num());
    TWeakObjectPtr<UNpcifyCharacterComponent> WeakThis(this);
    const int32 Generation = ConversationGeneration;
    for (const int32 Index : Pending)
    {
        TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
        // Signed storage URLs must not receive the API key.
        HttpRequest->SetURL(UTF8_TO_TCHAR(Shared->Timeline.Dialogue[Index].Audio.Url.c_str()));
        HttpRequest->SetVerb(TEXT("GET"));
        HttpRequest->OnProcessRequestComplete().BindLambda(
            [WeakThis, Shared, Remaining, Index, Generation, PlaySequence](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
            {
                if (!WeakThis.IsValid() || WeakThis->ConversationGeneration != Generation) return;
                UNpcifyCharacterComponent* Self = WeakThis.Get();
                if (bConnectedSuccessfully && Response.IsValid() && EHttpResponseCodes::IsOk(Response->GetResponseCode()))
                {
                    const TArray<uint8>& Content = Response->GetContent();
                    Shared->Timeline.Dialogue[Index].Audio.Data.assign(Content.GetData(), Content.GetData() + Content.Num());
                }
                else
                {
                    // Missing audio must not block the conversation; text and timeline still play.
                    Self->OnError.Broadcast(TEXT("Npcify speech audio download failed."));
                }
                // A newer reply that started during the downloads keeps playing.
                if (--(*Remaining) == 0) Self->Runtime->PlayTurnStaging(*Shared, PlaySequence);
            }
        );
        HttpRequest->ProcessRequest();
    }
}

void UNpcifyCharacterComponent::SendCreateConversationRequest(const FString& FollowUpMessage)
{
    Runtime->SetThinking(true);
    if (bLiveMotion)
    {
        // A request before the first tick still finds the mesh of live motion.
        BindLiveMotionMesh();
    }
    const std::string BaseUrl = Runtime->GetOptions().BaseUrl;
    const std::string Token = Runtime->GetOptions().ResolvedToken();
    TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetStringField(TEXT("avatarId"), CharacterId);
    if (WantsLiveMotion())
    {
        // This client plays live motion frames (docs/realtime-motion.md).
        Body->SetArrayField(TEXT("capabilities"), {MakeShared<FJsonValueString>(UTF8_TO_TCHAR(Npcify::FNpcifyMotionFrames::Capability))});
    }

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
    HttpRequest->SetURL(UTF8_TO_TCHAR((BaseUrl + "/conversations").c_str()));
    HttpRequest->SetVerb(TEXT("POST"));
    HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    HttpRequest->SetHeader(TEXT("Accept"), TEXT("application/vnd.npcify.performance"));
    if (!Token.empty())
    {
        const FString TokenStr = UTF8_TO_TCHAR(Token.c_str());
        HttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *TokenStr));
        HttpRequest->SetHeader(TEXT("X-API-Token"), TokenStr);
    }
    HttpRequest->SetContentAsString(SerializeJson(Body));

    TWeakObjectPtr<UNpcifyCharacterComponent> WeakThis(this);
    const int32 Generation = ConversationGeneration;
    HttpRequest->OnProcessRequestComplete().BindLambda(
        [WeakThis, FollowUpMessage, Generation](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
        {
            if (!WeakThis.IsValid() || WeakThis->ConversationGeneration != Generation) return;
            UNpcifyCharacterComponent* Self = WeakThis.Get();
            if (!bConnectedSuccessfully || !Response.IsValid() || !EHttpResponseCodes::IsOk(Response->GetResponseCode()))
            {
                Self->Runtime->SetThinking(false);
                Self->OnError.Broadcast(DescribeHttpFailure(TEXT("Failed to create Npcify conversation"), Response));
                return;
            }
            try
            {
                const TArray<uint8>& Content = Response->GetContent();
                std::vector<uint8_t> Bytes(Content.GetData(), Content.GetData() + Content.Num());
                const Npcify::FConversationCreationResult Created =
                    Npcify::FNpcifySchemaMapper::DecodeConversation(Bytes);
                Self->Runtime->SetConversationId(Created.ConversationId);
                Self->Runtime->SetActiveStagingId(Created.OpeningStaging.StagingId);
                const FString CreatedId = UTF8_TO_TCHAR(Created.ConversationId.c_str());
                if (Self->WantsLiveMotion() || Self->WantsStreamReplies())
                {
                    // Join the stream early, so that the first turn does not wait for it.
                    Self->JoinConversationStream(CreatedId, nullptr);
                }
                if (!FollowUpMessage.IsEmpty())
                {
                    Self->SendTurnRequest(CreatedId, FollowUpMessage);
                }
                else Self->PrepareAndPlayStaging(Created.OpeningStaging);
            }
            catch (const std::exception& Ex)
            {
                Self->Runtime->SetThinking(false);
                Self->OnError.Broadcast(UTF8_TO_TCHAR(Ex.what()));
            }
        }
    );
    HttpRequest->ProcessRequest();
}

void UNpcifyCharacterComponent::SendTurnRequest(const FString& ConversationIdStr, const FString& Message)
{
    // The character thinks until the reply of its newest request plays or fails.
    const uint64_t Request = Runtime->BeginTurnRequest();
    if (!WantsLiveMotion() && !WantsStreamReplies())
    {
        SendCreateStagingRequest(ConversationIdStr, Message, MakeUuidV4(), 0, false, false, Request);
        return;
    }
    // The streamed reply and the live motion frames arrive on the conversation stream while the
    // server makes the turn: use them only when the stream has joined the conversation.
    if (StreamSocket.IsValid() && StreamSocket->IsJoined(ConversationIdStr))
    {
        SendCreateStagingRequest(ConversationIdStr, Message, MakeUuidV4(), 0, WantsLiveMotion(), WantsStreamReplies(), Request);
        return;
    }
    if (StreamJoinGivenUp == ConversationIdStr ||
        (StreamSocket.IsValid() &&
            (StreamSocket->IsFailing(ConversationIdStr) || StreamSocket->IsRefused(ConversationIdStr))))
    {
        // A join that is known to fail: the reply plays after its HTTP result, with prepared
        // clips, while the socket reconnects in the background.
        SendCreateStagingRequest(ConversationIdStr, Message, MakeUuidV4(), 0, false, false, Request);
        return;
    }
    TWeakObjectPtr<UNpcifyCharacterComponent> WeakThis(this);
    const int32 Generation = ConversationGeneration;
    const TSharedRef<bool> bSent = MakeShared<bool>(false);
    const TFunction<void(bool)> Send = [WeakThis, ConversationIdStr, Message, Generation, bSent, Request](bool bJoined)
    {
        if (*bSent)
        {
            return;
        }
        *bSent = true;
        if (!WeakThis.IsValid() || WeakThis->ConversationGeneration != Generation || !WeakThis->Runtime.IsValid())
        {
            return;
        }
        if (!bJoined)
        {
            WeakThis->StreamJoinGivenUp = ConversationIdStr;
        }
        WeakThis->SendCreateStagingRequest(
            ConversationIdStr,
            Message,
            MakeUuidV4(),
            0,
            bJoined && WeakThis->WantsLiveMotion(),
            bJoined && WeakThis->WantsStreamReplies(),
            Request);
    };
    JoinConversationStream(ConversationIdStr, Send);
    // A turn waits a few seconds for the stream at most; the join goes on in the background.
    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([Send](float)
        {
            Send(false);
            return false;
        }),
        TurnJoinWaitSeconds
    );
}

void UNpcifyCharacterComponent::SendCreateStagingRequest(
    const FString& ConversationIdStr,
    const FString& Message,
    const FString& IdempotencyKey,
    int32 Attempt,
    bool bWithLiveMotion,
    bool bStreamed,
    uint64_t Request
)
{
    // Retries reuse the key, so the server runs the turn once.
    const FString Key = IdempotencyKey.IsEmpty() ? MakeUuidV4() : IdempotencyKey;
    if (Attempt == 0 && bStreamed)
    {
        // The server uses the Idempotency-Key as the task ID of the stream events.
        Runtime->BeginStreamTurn(TCHAR_TO_UTF8(*Key), Request);
    }
    const std::string BaseUrl = Runtime->GetOptions().BaseUrl;
    const std::string Token = Runtime->GetOptions().ResolvedToken();
    // Same JSON body as the Web and Unity SDKs; the response is NPCP.
    TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetStringField(TEXT("content"), Message);
    Body->SetBoolField(TEXT("silent"), false);
    Body->SetBoolField(TEXT("incrementalPlayback"), true);
    const FString ParentStagingId = UTF8_TO_TCHAR(Runtime->GetActiveStagingId().c_str());
    if (ParentStagingId.IsEmpty()) Body->SetField(TEXT("parentStagingId"), MakeShared<FJsonValueNull>());
    else Body->SetStringField(TEXT("parentStagingId"), ParentStagingId);
    // Always send the list: without it the server uses the capabilities of the conversation.
    TArray<TSharedPtr<FJsonValue>> Capabilities;
    if (bWithLiveMotion)
    {
        Capabilities.Add(MakeShared<FJsonValueString>(UTF8_TO_TCHAR(Npcify::FNpcifyMotionFrames::Capability)));
    }
    Body->SetArrayField(TEXT("capabilities"), Capabilities);

    const FString Url = FString::Printf(
        TEXT("%s/conversations/%s/stagings"),
        UTF8_TO_TCHAR(BaseUrl.c_str()),
        *ConversationIdStr
    );

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
    HttpRequest->SetURL(Url);
    HttpRequest->SetVerb(TEXT("POST"));
    HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    HttpRequest->SetHeader(TEXT("Accept"), TEXT("application/vnd.npcify.performance"));
    HttpRequest->SetHeader(TEXT("Idempotency-Key"), Key);
    if (!Token.empty())
    {
        const FString TokenStr = UTF8_TO_TCHAR(Token.c_str());
        HttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *TokenStr));
        HttpRequest->SetHeader(TEXT("X-API-Token"), TokenStr);
    }
    HttpRequest->SetContentAsString(SerializeJson(Body));

    TWeakObjectPtr<UNpcifyCharacterComponent> WeakThis(this);
    const int32 Generation = ConversationGeneration;
    HttpRequest->OnProcessRequestComplete().BindLambda(
        [WeakThis, ConversationIdStr, Message, Key, Attempt, Generation, bWithLiveMotion, bStreamed, Request](
            FHttpRequestPtr,
            FHttpResponsePtr Response,
            bool bConnectedSuccessfully
        )
        {
            if (!WeakThis.IsValid() || WeakThis->ConversationGeneration != Generation) return;
            UNpcifyCharacterComponent* Self = WeakThis.Get();
            const int32 Code = Response.IsValid() ? Response->GetResponseCode() : 0;
            const bool bTransient = !bConnectedSuccessfully || Code == 409 || Code == 502 || Code == 503 || Code == 504;
            if (bTransient && Attempt < 2)
            {
                if (bStreamed)
                {
                    // The server can run the turn again; the start event of the rerun can be lost.
                    Self->Runtime->NoteTurnRetry(TCHAR_TO_UTF8(*Key));
                }
                Self->SendCreateStagingRequest(ConversationIdStr, Message, Key, Attempt + 1, bWithLiveMotion, bStreamed, Request);
                return;
            }
            const std::string TaskId = TCHAR_TO_UTF8(*Key);
            if (!bConnectedSuccessfully || !Response.IsValid() || !EHttpResponseCodes::IsOk(Code))
            {
                // A reply that the stream delivered completely plays on.
                if (bStreamed && Self->Runtime->HandleTurnFailure(TaskId))
                {
                    return;
                }
                Self->Runtime->EndTurnRequest(Request);
                Self->OnError.Broadcast(DescribeHttpFailure(TEXT("Npcify staging request failed"), Response));
                return;
            }
            try
            {
                const TArray<uint8>& Content = Response->GetContent();
                std::vector<uint8_t> Bytes(Content.GetData(), Content.GetData() + Content.Num());
                const Npcify::FStagingResultPayload Staging = Npcify::FNpcifySchemaMapper::DecodeStaging(Bytes);
                // The stream plays the reply already (the result only completes it), or a newer
                // reply plays: the runtime keeps the active staging of the conversation.
                uint64_t PlaySequence = 0;
                if (bStreamed && Self->Runtime->HandleTurnResult(TaskId, Staging, &PlaySequence))
                {
                    return;
                }
                Self->Runtime->SetActiveStagingId(Staging.StagingId);
                // Also a turn without the stream keeps the order of the replies.
                Self->PrepareAndPlayStaging(Staging, PlaySequence != 0 ? PlaySequence : Request);
            }
            catch (const std::exception& Ex)
            {
                if (bStreamed && Self->Runtime->HandleTurnFailure(TaskId))
                {
                    return;
                }
                Self->Runtime->EndTurnRequest(Request);
                Self->OnError.Broadcast(UTF8_TO_TCHAR(Ex.what()));
            }
        }
    );
    HttpRequest->ProcessRequest();
}
