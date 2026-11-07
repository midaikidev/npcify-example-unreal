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
    Shutdown(false);
}

void FNpcifyConversationSocket::Join(const FString& ConversationId, TFunction<void(bool)> OnDone)
{
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

void FNpcifyConversationSocket::Leave()
{
    DesiredConversationId.Empty();
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
        Shutdown(true);
        return true;
    }
    if (Session.IsValid() && Waiters.Num() > 0 && Now > JoinDeadline)
    {
        Session->FailPendingJoin();
        FinishWaiters(false);
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
    TArray<TFunction<void(bool)>> Done = MoveTemp(Waiters);
    Waiters.Reset();
    for (TFunction<void(bool)>& Waiter : Done)
    {
        Waiter(bJoined);
    }
}
