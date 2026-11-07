#include "NpcifyConversationSocket.h"

#include "HAL/PlatformTime.h"
#include "IWebSocket.h"
#include "WebSocketsModule.h"

namespace
{
    constexpr double JoinTimeoutSeconds = 10.0;
}

FNpcifyConversationSocket::FNpcifyConversationSocket(const std::string& BaseUrl, const std::string& InApiToken)
    : Url(UTF8_TO_TCHAR(Npcify::FNpcifySocketIo::BuildUrl(BaseUrl).c_str()))
    , ApiToken(InApiToken)
{
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateRaw(this, &FNpcifyConversationSocket::Tick),
        0.25f
    );
}

FNpcifyConversationSocket::~FNpcifyConversationSocket()
{
    FTSTicker::RemoveTicker(TickHandle);
    DesiredConversationId.Empty();
    // A waiting turn must not run while its owner goes away: it would send a paid turn
    // that nobody plays.
    Waiters.Reset();
    Shutdown(false);
}

void FNpcifyConversationSocket::Join(const FString& ConversationId, TFunction<void(bool)> OnDone)
{
    if (IsRefused(ConversationId))
    {
        // The server refused this conversation with this key; a retry fails the same way.
        if (OnDone)
        {
            OnDone(false);
        }
        return;
    }
    DesiredConversationId = ConversationId;
    if (IsJoined(ConversationId))
    {
        if (OnDone)
        {
            OnDone(true);
        }
        return;
    }
    if (OnDone)
    {
        Waiters.Add(MoveTemp(OnDone));
    }
    JoinDeadline = FPlatformTime::Seconds() + JoinTimeoutSeconds;
    if (!Socket.IsValid())
    {
        Open();
        return;
    }
    Session->Join(TCHAR_TO_UTF8(*ConversationId));
}

bool FNpcifyConversationSocket::IsJoined(const FString& ConversationId) const
{
    return Socket.IsValid() && Session.IsValid() && Socket->IsConnected() &&
        Session->IsJoined(TCHAR_TO_UTF8(*ConversationId));
}

bool FNpcifyConversationSocket::IsFailing(const FString& ConversationId) const
{
    return !ConversationId.IsEmpty() && FailedConversationId == ConversationId && !IsJoined(ConversationId);
}

bool FNpcifyConversationSocket::IsRefused(const FString& ConversationId) const
{
    return !ConversationId.IsEmpty() && RefusedConversationId == ConversationId;
}

void FNpcifyConversationSocket::Leave()
{
    DesiredConversationId.Empty();
    FailedConversationId.Empty();
    // An explicit leave also allows a refused conversation to try again.
    RefusedConversationId.Empty();
    if (Session.IsValid())
    {
        Session->Leave();
    }
    FinishWaiters(false);
}

void FNpcifyConversationSocket::Open()
{
    const TWeakPtr<FNpcifyConversationSocket> Weak = AsShared();
    Session = MakeUnique<Npcify::FNpcifyConversationSession>(ApiToken);
    Session->SendText = [Weak](const std::string& Text)
    {
        const TSharedPtr<FNpcifyConversationSocket> Self = Weak.Pin();
        if (Self.IsValid() && Self->Socket.IsValid() && Self->Socket->IsConnected())
        {
            Self->Socket->Send(UTF8_TO_TCHAR(Text.c_str()));
        }
    };
    Session->OnStreamEvent = [Weak](const std::string&, const Npcify::FNpcifyMsgPackValue& Event)
    {
        const TSharedPtr<FNpcifyConversationSocket> Self = Weak.Pin();
        if (Self.IsValid() && Self->OnStreamEvent)
        {
            Self->OnStreamEvent(Event);
        }
    };
    Session->OnJoinResult = [Weak](const std::string&, bool bJoined)
    {
        const TSharedPtr<FNpcifyConversationSocket> Self = Weak.Pin();
        if (Self.IsValid())
        {
            if (bJoined)
            {
                Self->ReconnectAttempt = 0;
            }
            Self->FinishWaiters(bJoined);
        }
    };
    Session->OnRefused = [Weak]()
    {
        const TSharedPtr<FNpcifyConversationSocket> Self = Weak.Pin();
        if (Self.IsValid())
        {
            // Close outside the socket's own callback, without a reconnect.
            Self->bRefusedNow = true;
            Self->ReconnectAt = -1.0;
        }
    };
    Session->OnLog = [Weak](const std::string& Message)
    {
        const TSharedPtr<FNpcifyConversationSocket> Self = Weak.Pin();
        if (Self.IsValid() && Self->OnLog)
        {
            Self->OnLog(UTF8_TO_TCHAR(Message.c_str()));
        }
    };
    Session->Join(TCHAR_TO_UTF8(*DesiredConversationId));

    Socket = FWebSocketsModule::Get().CreateWebSocket(Url, FString());
    LastReceived = FPlatformTime::Seconds();
    Socket->OnConnectionError().AddLambda([Weak](const FString& Error)
    {
        const TSharedPtr<FNpcifyConversationSocket> Self = Weak.Pin();
        if (Self.IsValid())
        {
            if (Self->OnLog)
            {
                Self->OnLog(TEXT("The conversation stream could not connect: ") + Error);
            }
            // Close outside the socket's own callback.
            Self->ReconnectAt = -1.0;
        }
    });
    Socket->OnClosed().AddLambda([Weak](int32, const FString&, bool)
    {
        const TSharedPtr<FNpcifyConversationSocket> Self = Weak.Pin();
        if (Self.IsValid())
        {
            Self->ReconnectAt = -1.0;
        }
    });
    Socket->OnMessage().AddLambda([Weak](const FString& Message)
    {
        const TSharedPtr<FNpcifyConversationSocket> Self = Weak.Pin();
        if (!Self.IsValid() || !Self->Session.IsValid())
        {
            return;
        }
        Self->LastReceived = FPlatformTime::Seconds();
        if (!Self->Session->HandleText(TCHAR_TO_UTF8(*Message)))
        {
            Self->ReconnectAt = -1.0;
        }
    });
    Socket->OnBinaryMessage().AddLambda([Weak](const void* Data, SIZE_T Size, bool bIsLastFragment)
    {
        const TSharedPtr<FNpcifyConversationSocket> Self = Weak.Pin();
        if (!Self.IsValid() || !Self->Session.IsValid())
        {
            return;
        }
        Self->LastReceived = FPlatformTime::Seconds();
        Self->BinaryBuffer.Append(static_cast<const uint8*>(Data), static_cast<int32>(Size));
        if (bIsLastFragment)
        {
            Self->Session->HandleBinary(Self->BinaryBuffer.GetData(), static_cast<size_t>(Self->BinaryBuffer.Num()));
            Self->BinaryBuffer.Reset();
        }
    });
    Socket->Connect();
}

void FNpcifyConversationSocket::Shutdown(bool bReconnect)
{
    if (Socket.IsValid())
    {
        Socket->OnConnectionError().Clear();
        Socket->OnClosed().Clear();
        Socket->OnMessage().Clear();
        Socket->OnBinaryMessage().Clear();
        if (Socket->IsConnected())
        {
            Socket->Close();
        }
        Socket.Reset();
    }
    Session.Reset();
    BinaryBuffer.Reset();
    FinishWaiters(false);
    if (bReconnect && !DesiredConversationId.IsEmpty())
    {
        const double Delay = FMath::Min(10.0, static_cast<double>(1 << FMath::Min(ReconnectAttempt, 4)));
        ++ReconnectAttempt;
        ReconnectAt = FPlatformTime::Seconds() + Delay;
    }
    else
    {
        ReconnectAt = 0.0;
    }
}

bool FNpcifyConversationSocket::Tick(float)
{
    const double Now = FPlatformTime::Seconds();
    if (ReconnectAt < 0.0)
    {
        // The socket closed or failed in one of its callbacks.
        if (bRefusedNow)
        {
            bRefusedNow = false;
            RefusedConversationId = DesiredConversationId;
            DesiredConversationId.Empty();
            if (OnLog)
            {
                OnLog(TEXT("The conversation stream refused the conversation; no retry for it."));
            }
            Shutdown(false);
            return true;
        }
        Shutdown(true);
        return true;
    }
    if (Session.IsValid() && !DesiredConversationId.IsEmpty() &&
        !Session->IsJoined(TCHAR_TO_UTF8(*DesiredConversationId)) && Now > JoinDeadline)
    {
        // No acknowledgement in time (also for a join that no turn waits for, so that the
        // conversation counts as failing): a late one would not count, so connect again.
        Session->FailPendingJoin();
        FinishWaiters(false);
        Shutdown(true);
        return true;
    }
    if (Socket.IsValid() && Session.IsValid() && Session->IsConnected() &&
        (Now - LastReceived) * 1000.0 > static_cast<double>(Session->GetPingTimeoutMs()))
    {
        if (OnLog)
        {
            OnLog(TEXT("The conversation stream timed out."));
        }
        Shutdown(true);
        return true;
    }
    if (!Socket.IsValid() && ReconnectAt > 0.0 && Now >= ReconnectAt && !DesiredConversationId.IsEmpty())
    {
        ReconnectAt = 0.0;
        JoinDeadline = Now + JoinTimeoutSeconds;
        Open();
    }
    return true;
}

void FNpcifyConversationSocket::FinishWaiters(bool bJoined)
{
    // A failed join (or a lost connection) marks the conversation as failing, so turns stop
    // waiting for it while the socket reconnects in the background.
    if (bJoined)
    {
        FailedConversationId.Empty();
    }
    else if (!DesiredConversationId.IsEmpty())
    {
        FailedConversationId = DesiredConversationId;
    }
    TArray<TFunction<void(bool)>> Done = MoveTemp(Waiters);
    Waiters.Reset();
    for (TFunction<void(bool)>& Waiter : Done)
    {
        Waiter(bJoined);
    }
}
