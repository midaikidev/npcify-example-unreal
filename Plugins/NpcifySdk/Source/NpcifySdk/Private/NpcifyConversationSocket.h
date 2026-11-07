#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Core/NpcifySocketIo.h"

#include <memory>
#include <string>

class IWebSocket;

/**
 * The conversation stream (conversations:stream over Socket.IO) on the engine's WebSockets
 * module. The component joins the room of its conversation before a turn, so that live motion
 * frames reach it while the server makes the turn. After a lost connection it connects again
 * and joins the conversation again. Callbacks run on the game thread.
 */
class FNpcifyConversationSocket : public TSharedFromThis<FNpcifyConversationSocket>
{
public:
    FNpcifyConversationSocket(const std::string& BaseUrl, const std::string& ApiToken);
    ~FNpcifyConversationSocket();

    /** A stream event of the joined conversation. */
    TFunction<void(const Npcify::FNpcifyMsgPackValue&)> OnStreamEvent;
    TFunction<void(const FString&)> OnLog;

    /**
     * Joins the room; OnDone(true) after the acknowledgement, false after a failure or 10 s.
     * A conversation that the server refused gets false at once.
     */
    void Join(const FString& ConversationId, TFunction<void(bool)> OnDone);
    bool IsJoined(const FString& ConversationId) const;
    /** The last join of this conversation failed; the socket reconnects in the background. */
    bool IsFailing(const FString& ConversationId) const;
    /** The server refused this conversation (for example a rotated key): no retry for it. */
    bool IsRefused(const FString& ConversationId) const;
    void Leave();

private:
    void Open();
    void Shutdown(bool bReconnect);
    bool Tick(float DeltaSeconds);
    void FinishWaiters(bool bJoined);

    FString Url;
    std::string ApiToken;
    TSharedPtr<IWebSocket> Socket;
    TUniquePtr<Npcify::FNpcifyConversationSession> Session;
    TArray<uint8> BinaryBuffer;
    FString DesiredConversationId;
    FString FailedConversationId;
    FString RefusedConversationId;
    bool bRefusedNow = false;
    /** The conversation of the refusal that the next Tick handles. */
    FString RefusedNowId;
    TArray<TFunction<void(bool)>> Waiters;
    FTSTicker::FDelegateHandle TickHandle;
    double JoinDeadline = 0.0;
    double LastReceived = 0.0;
    double ReconnectAt = 0.0;
    int32 ReconnectAttempt = 0;
};
