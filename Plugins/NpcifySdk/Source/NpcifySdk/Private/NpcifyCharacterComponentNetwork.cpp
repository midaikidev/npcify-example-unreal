#include "NpcifyCharacterComponent.h"

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

void UNpcifyCharacterComponent::PrepareAndPlayStaging(const Npcify::FStagingResultPayload& Staging)
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
        Runtime->PlayStaging(*Shared);
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
            [WeakThis, Shared, Remaining, Index, Generation](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
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
                if (--(*Remaining) == 0) Self->Runtime->PlayStaging(*Shared);
            }
        );
        HttpRequest->ProcessRequest();
    }
}

void UNpcifyCharacterComponent::SendCreateConversationRequest(const FString& FollowUpMessage)
{
    Runtime->SetThinking(true);
    const std::string BaseUrl = Runtime->GetOptions().BaseUrl;
    const std::string Token = Runtime->GetOptions().ResolvedToken();
    TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetStringField(TEXT("avatarId"), CharacterId);

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
                if (!FollowUpMessage.IsEmpty())
                {
                    Self->SendCreateStagingRequest(
                        UTF8_TO_TCHAR(Created.ConversationId.c_str()),
                        FollowUpMessage,
                        MakeUuidV4(),
                        0
                    );
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

void UNpcifyCharacterComponent::SendCreateStagingRequest(
    const FString& ConversationIdStr,
    const FString& Message,
    const FString& IdempotencyKey,
    int32 Attempt
)
{
    // Retries reuse the key, so the server runs the turn once.
    const FString Key = IdempotencyKey.IsEmpty() ? MakeUuidV4() : IdempotencyKey;
    Runtime->SetThinking(true);
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
        [WeakThis, ConversationIdStr, Message, Key, Attempt, Generation](
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
                Self->SendCreateStagingRequest(ConversationIdStr, Message, Key, Attempt + 1);
                return;
            }
            if (!bConnectedSuccessfully || !Response.IsValid() || !EHttpResponseCodes::IsOk(Code))
            {
                Self->Runtime->SetThinking(false);
                Self->OnError.Broadcast(DescribeHttpFailure(TEXT("Npcify staging request failed"), Response));
                return;
            }
            try
            {
                const TArray<uint8>& Content = Response->GetContent();
                std::vector<uint8_t> Bytes(Content.GetData(), Content.GetData() + Content.Num());
                const Npcify::FStagingResultPayload Staging = Npcify::FNpcifySchemaMapper::DecodeStaging(Bytes);
                Self->Runtime->SetActiveStagingId(Staging.StagingId);
                Self->PrepareAndPlayStaging(Staging);
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
