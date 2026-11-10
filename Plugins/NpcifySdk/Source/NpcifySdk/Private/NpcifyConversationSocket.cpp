#include "NpcifyConversationSocket.h"

#include "HAL/PlatformTime.h"
#include "IWebSocket.h"
#include "WebSocketsModule.h"

namespace
{
    constexpr double JoinTimeoutSeconds = 10.0;
    constexpr double SightAckTimeoutSeconds = Npcify::FSightLimits::AckTimeoutMs / 1000.0;
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
    SightWaits.Reset();
    Shutdown(false);
}

void FNpcifyConversationSocket::Join(const FString& InConversationId, TFunction<void(bool)> OnDone)
{
    // The server trims the ID and names that one in a refusal: send and compare that one.
    const FString ConversationId = InConversationId.TrimStartAndEnd();
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
        Session->IsJoined(TCHAR_TO_UTF8(*ConversationId.TrimStartAndEnd()));
}

bool FNpcifyConversationSocket::IsFailing(const FString& InConversationId) const
{
    const FString ConversationId = InConversationId.TrimStartAndEnd();
    return !ConversationId.IsEmpty() && FailedConversationId == ConversationId && !IsJoined(ConversationId);
}

bool FNpcifyConversationSocket::IsRefused(const FString& InConversationId) const
{
    const FString ConversationId = InConversationId.TrimStartAndEnd();
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

void FNpcifyConversationSocket::SendSightFrame(const Npcify::FSightFrameRequest& Request, TFunction<void(const Npcify::FSightAck&)> Done)
{
    const int64 AckId = Socket.IsValid() && Socket->IsConnected() && Session.IsValid() ? Session->SendSightFrame(Request) : -1;
    if (AckId < 0)
    {
        if (Done)
        {
            Done(Npcify::FSightAck::NotAnswered(Npcify::SightCodes::Disconnected));
        }
        return;
    }
    FSightWait& Wait = SightWaits.Add(AckId);
    Wait.Done = MoveTemp(Done);
    Wait.Deadline = FPlatformTime::Seconds() + SightAckTimeoutSeconds;
}

void FNpcifyConversationSocket::FinishSightFrames(const char* Code, bool bExpiredOnly)
{
    const double Now = FPlatformTime::Seconds();
    TArray<TFunction<void(const Npcify::FSightAck&)>> Done;
    for (auto It = SightWaits.CreateIterator(); It; ++It)
    {
        if (!bExpiredOnly || Now > It.Value().Deadline)
        {
            Done.Add(MoveTemp(It.Value().Done));
            It.RemoveCurrent();
        }
    }
    for (TFunction<void(const Npcify::FSightAck&)>& Callback : Done)
    {
        if (Callback)
        {
            Callback(Npcify::FSightAck::NotAnswered(Code));
        }
    }
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
    // The WebSocket sends text and binary messages from one queue, in order.
    Session->SendBinary = [Weak](const std::vector<uint8_t>& Data)
    {
        const TSharedPtr<FNpcifyConversationSocket> Self = Weak.Pin();
        if (Self.IsValid() && Self->Socket.IsValid() && Self->Socket->IsConnected())
        {
            Self->Socket->Send(Data.data(), Data.size(), true);
        }
    };
    Session->OnAck = [Weak](int64_t AckId, const std::string& Json)
    {
        const TSharedPtr<FNpcifyConversationSocket> Self = Weak.Pin();
        FSightWait Wait;
        if (Self.IsValid() && Self->SightWaits.RemoveAndCopyValue(AckId, Wait) && Wait.Done)
        {
            Wait.Done(Npcify::FSightAck::FromAckJson(Json));
        }
    };
    Session->OnSightObservation = [Weak](const std::string& Json)
    {
        const TSharedPtr<FNpcifyConversationSocket> Self = Weak.Pin();
        if (Self.IsValid() && Self->OnSightObservation)
        {
            Self->OnSightObservation(Json);
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
            // Close outside the socket's own callback. The conversation of the refusal is the one
            // that the session wanted at that moment: the game can join another before the Tick.
            Self->bRefusedNow = true;
            Self->RefusedNowId = Self->Session.IsValid()
                ? FString(UTF8_TO_TCHAR(Self->Session->GetRefusedConversationId().c_str()))
                : FString();
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
            const bool bKeep = Self->Session->HandleBinary(Self->BinaryBuffer.GetData(), static_cast<size_t>(Self->BinaryBuffer.Num()));
            Self->BinaryBuffer.Reset();
            if (!bKeep)
            {
                Self->ReconnectAt = -1.0;
            }
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
    // A lost connection loses the acknowledgements: the next frame joins again first.
    FinishSightFrames(Npcify::SightCodes::Disconnected, false);
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
    if (SightWaits.Num() > 0)
    {
        FinishSightFrames(Npcify::SightCodes::Timeout, true);
    }
    if (ReconnectAt < 0.0)
    {
        // The socket closed or failed in one of its callbacks.
        if (bRefusedNow)
        {
            bRefusedNow = false;
            if (!RefusedNowId.IsEmpty() && RefusedNowId == DesiredConversationId)
            {
                RefusedConversationId = DesiredConversationId;
                DesiredConversationId.Empty();
                if (OnLog)
                {
                    OnLog(TEXT("The conversation stream refused the conversation; no retry for it."));
                }
                Shutdown(false);
                return true;
            }
            // The game left the refused conversation meanwhile: connect again for the wanted one.
            if (OnLog)
            {
                OnLog(TEXT("The conversation stream refused a conversation that is no longer wanted."));
            }
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
