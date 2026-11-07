#include "NpcifyCharacterComponent.h"

#include "NpcifyConversationSocket.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

#include <exception>
#include <string>
#include <vector>

// The conversation stream of UNpcifyCharacterComponent: replies that play while they stream
// (docs/streaming_reply.md) and live motion frames (docs/realtime-motion.md).

DEFINE_LOG_CATEGORY_STATIC(LogNpcifyStream, Log, All);

void UNpcifyCharacterComponent::LogStreamNote(const std::string& Message)
{
    UE_LOG(LogNpcifyStream, Log, TEXT("%s"), UTF8_TO_TCHAR(Message.c_str()));
}

bool UNpcifyCharacterComponent::WantsStreamReplies() const
{
    return bStreamReplies && Runtime.IsValid() && !Runtime->HasBakedConversation();
}

void UNpcifyCharacterComponent::JoinConversationStream(const FString& ConversationIdStr, TFunction<void(bool)> OnDone)
{
    if (!StreamSocket.IsValid())
    {
        StreamSocket = MakeShared<FNpcifyConversationSocket>(Runtime->GetOptions().BaseUrl, Runtime->GetOptions().ResolvedToken());
        TWeakObjectPtr<UNpcifyCharacterComponent> WeakThis(this);
        StreamSocket->OnStreamEvent = [WeakThis](const Npcify::FNpcifyMsgPackValue& Event)
        {
            if (WeakThis.IsValid())
            {
                WeakThis->HandleStreamEvent(Event);
            }
        };
        StreamSocket->OnLog = [](const FString& Message)
        {
            UE_LOG(LogNpcifyStream, Log, TEXT("%s"), *Message);
        };
    }
    StreamSocket->Join(ConversationIdStr, MoveTemp(OnDone));
}

void UNpcifyCharacterComponent::HandleStreamEvent(const Npcify::FNpcifyMsgPackValue& Event)
{
    if (!Runtime.IsValid())
    {
        return;
    }
    try
    {
        if (Npcify::FNpcifyCharacterRuntime::IsTurnEvent(Event))
        {
            Runtime->HandleStreamEvent(Event);
            return;
        }
        Npcify::FNpcifyMotionFramesEvent Motion;
        if (Npcify::FNpcifyMotionFrames::TryParseEvent(Event, Motion) && Runtime->GetLiveMotion().Push(Motion))
        {
            OnLiveMotionFrames.Broadcast(
                UTF8_TO_TCHAR(Motion.StagingId.c_str()),
                static_cast<int32>(Motion.StartFrame),
                Motion.FrameCount);
        }
    }
    catch (const std::exception& Ex)
    {
        // An invalid event never stops the speech of the staging.
        UE_LOG(LogNpcifyStream, Warning, TEXT("Dropped a conversation stream event: %s"), UTF8_TO_TCHAR(Ex.what()));
    }
}

void UNpcifyCharacterComponent::DownloadBlockAudio(const std::string& TaskId, const std::string& BlockId, const std::string& Url)
{
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    // Signed storage URLs must not receive the API key.
    Request->SetURL(UTF8_TO_TCHAR(Url.c_str()));
    Request->SetVerb(TEXT("GET"));
    TWeakObjectPtr<UNpcifyCharacterComponent> WeakThis(this);
    const int32 Generation = ConversationGeneration;
    Request->OnProcessRequestComplete().BindLambda(
        [WeakThis, TaskId, BlockId, Generation](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
        {
            if (!WeakThis.IsValid() || WeakThis->ConversationGeneration != Generation || !WeakThis->Runtime.IsValid())
            {
                return;
            }
            std::vector<uint8_t> Bytes;
            if (bConnected && Response.IsValid() && EHttpResponseCodes::IsOk(Response->GetResponseCode()))
            {
                const TArray<uint8>& Content = Response->GetContent();
                Bytes.assign(Content.GetData(), Content.GetData() + Content.Num());
            }
            else
            {
                // Missing audio must not stop the reply: the block plays its text and timeline.
                WeakThis->OnError.Broadcast(TEXT("Npcify speech audio download failed."));
            }
            WeakThis->Runtime->SetBlockAudio(TaskId, BlockId, Bytes);
        }
    );
    Request->ProcessRequest();
}
