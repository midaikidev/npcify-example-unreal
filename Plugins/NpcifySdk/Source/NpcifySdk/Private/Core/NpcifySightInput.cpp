#include "Core/NpcifySight.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

// The schedule of sight (FNpcifySightInput), as the web SDK (sight-input.ts).

namespace Npcify
{
    namespace
    {
        const char* const OffMessage = "Sight is off for this character. Its owner turns it on in the Sight tab.";
        const char* const BakedMessage = "Sight is not available in a baked (offline) conversation.";
        const char* const UnreadMessage = "Sight needs the character info: start a conversation first.";
    }

    struct FNpcifySightInput::FSession
    {
        int IntervalMs = FSightLimits::IntervalMs;
        bool bMotion = true;
        float Threshold = FSightLimits::MotionThreshold;
        /** Set when the source opened. */
        std::shared_ptr<ISightSource> Source;
        /** The last motion grid; empty after a hidden or paused game. */
        std::vector<uint8_t> Grid;
        bool bReadingGrid = false;
        int64_t NextCheckAt = 0;
        bool bHasSent = false;
        /** The time of the last frame that went to the server. */
        int64_t LastSentAt = 0;
        /** The picture changed after the last frame. */
        bool bMotionSeen = false;
        /** A frame waits for its acknowledgement. */
        bool bInFlight = false;
        /** The connection was lost: join the conversation before the next frame. */
        bool bRejoin = false;
    };

    FNpcifySightInput::FNpcifySightInput(
        std::shared_ptr<ISightConnection> InConnection,
        std::function<std::string()> InConversationId,
        std::function<int64_t()> InNowMs,
        std::function<int64_t()> InUnixNowMs,
        std::function<void(std::function<void()>)> InPost)
        : Connection(std::move(InConnection))
        , ConversationId(std::move(InConversationId))
        , NowMs(std::move(InNowMs))
        , UnixNowMs(std::move(InUnixNowMs))
        , Post(std::move(InPost))
        , Alive(std::make_shared<bool>(true))
        , Unavailable(UnreadMessage)
    {
    }

    FNpcifySightInput::~FNpcifySightInput()
    {
        // Results that come later find no input.
        Alive.reset();
        if (Current && Current->Source)
        {
            Current->Source->Stop();
        }
    }

    std::function<void(std::function<void()>)> FNpcifySightInput::Later()
    {
        std::weak_ptr<bool> Token = Alive;
        std::function<void(std::function<void()>)> Target = Post;
        return [Token, Target](std::function<void()> Work)
        {
            Target([Token, Work = std::move(Work)]()
            {
                if (!Token.expired())
                {
                    Work();
                }
            });
        };
    }

    void FNpcifySightInput::SetHidden(bool bInHidden)
    {
        bHidden = bInHidden;
        if (bInHidden && Current)
        {
            Current->Grid.clear();
        }
    }

    void FNpcifySightInput::Configure(bool bEnabled, bool bBaked)
    {
        bDisabledByServer = false;
        Unavailable = bBaked ? BakedMessage : bEnabled ? "" : OffMessage;
        if (!Unavailable.empty())
        {
            Stop();
        }
        else
        {
            Publish("");
        }
    }

    std::string FNpcifySightInput::Start(
        const FSightSchedule& Schedule,
        std::shared_ptr<ISightSource> Source,
        std::function<void(const std::string&)> Done)
    {
        if (!Unavailable.empty())
        {
            return SightCodes::Disabled;
        }
        if (Current)
        {
            return SightCodes::AlreadyStarted;
        }
        const std::string Conversation = ConversationId ? ConversationId() : std::string();
        if (Conversation.empty())
        {
            return SightCodes::ConversationRequired;
        }
        if (!Source)
        {
            return SightCodes::SourceUnavailable;
        }
        FSessionPtr Session = std::make_shared<FSession>();
        Session->IntervalMs = Schedule.IntervalMs > 0
            ? std::max(FSightLimits::MinFrameIntervalMs, Schedule.IntervalMs)
            : FSightLimits::IntervalMs;
        Session->bMotion = Schedule.bMotion;
        Session->Threshold = std::isfinite(Schedule.MotionThreshold)
            ? std::max(0.0f, Schedule.MotionThreshold)
            : FSightLimits::MotionThreshold;
        Current = Session;
        Publish("");

        struct FPending
        {
            int Count = 2;
            std::string JoinError;
            std::string OpenError;
        };
        std::shared_ptr<FPending> Pending = std::make_shared<FPending>();
        std::function<void(std::function<void()>)> Run = Later();
        std::function<void()> Settled = [this, Session, Source, Pending, Done]()
        {
            if (--Pending->Count == 0)
            {
                Opened(Session, Source, !Pending->OpenError.empty() ? Pending->OpenError : Pending->JoinError, Done);
            }
        };
        // The source opens while the conversation joins.
        Connection->Join(Conversation, [Run, Pending, Settled](const std::string& Code)
        {
            Run([Pending, Settled, Code]() { Pending->JoinError = Code; Settled(); });
        });
        Source->Open([Run, Pending, Settled](const std::string& Code)
        {
            Run([Pending, Settled, Code]() { Pending->OpenError = Code; Settled(); });
        });
        return std::string();
    }

    void FNpcifySightInput::Stop()
    {
        if (Current)
        {
            End(Current);
        }
        Publish("");
    }

    void FNpcifySightInput::Tick()
    {
        const int64_t Now = NowMs();
        const FSessionPtr Session = Current;
        if (bTicked && Now - LastTick > FSightLimits::TickGapMs && Session)
        {
            // The game did not run (paused, or a long load): no motion against that picture.
            Session->Grid.clear();
        }
        bTicked = true;
        LastTick = Now;
        if (!Session || !Session->Source || Now < Session->NextCheckAt)
        {
            return;
        }
        Check(Session);
    }

    void FNpcifySightInput::HandleObservation(const FSightObservation& Observation)
    {
        if (OnObservation)
        {
            OnObservation(Observation);
        }
    }

    void FNpcifySightInput::Opened(
        FSessionPtr Session,
        std::shared_ptr<ISightSource> Source,
        const std::string& Failure,
        const std::function<void(const std::string&)>& Done)
    {
        if (Session != Current)
        {
            // Stopped while the source or the connection opened.
            Source->Stop();
            if (Done)
            {
                Done(std::string());
            }
            return;
        }
        if (!Failure.empty())
        {
            Source->Stop();
            Current.reset();
            Publish(Failure);
            if (Done)
            {
                Done(Failure);
            }
            return;
        }
        Session->Source = Source;
        Publish("");
        if (Done)
        {
            Done(std::string());
        }
        Check(Session);
    }

    void FNpcifySightInput::Check(FSessionPtr Session)
    {
        if (Session != Current || !Session->Source)
        {
            return;
        }
        Session->NextCheckAt = NowMs() + FSightLimits::CheckMs;
        if (!Session->Source->IsLive())
        {
            Fail(Session, SightCodes::SourceEnded);
            return;
        }
        if (bHidden)
        {
            Session->Grid.clear();
            return;
        }
        if (Session->bMotion && !Session->bReadingGrid)
        {
            Session->bReadingGrid = true;
            std::function<void(std::function<void()>)> Run = Later();
            Session->Source->ReadLuminance([this, Run, Session](std::vector<uint8_t> Grid)
            {
                Run([this, Session, Grid = std::move(Grid)]() mutable { GridRead(Session, std::move(Grid)); });
            });
            return;
        }
        SendWhenDue(Session);
    }

    void FNpcifySightInput::GridRead(FSessionPtr Session, std::vector<uint8_t> Grid)
    {
        Session->bReadingGrid = false;
        if (Session != Current)
        {
            return;
        }
        if (bHidden)
        {
            Session->Grid.clear();
            return;
        }
        if (!Grid.empty() && !Session->Grid.empty() && MeanLumaChange(Session->Grid, Grid) > Session->Threshold)
        {
            Session->bMotionSeen = true;
        }
        Session->Grid = std::move(Grid);
        SendWhenDue(Session);
    }

    void FNpcifySightInput::SendWhenDue(FSessionPtr Session)
    {
        // A frame is due at the interval, or after motion when one second passed since the last
        // frame. While a frame waits for its acknowledgement, a due frame waits too.
        if (Session != Current || Session->bInFlight || bHidden || !Session->Source)
        {
            return;
        }
        const int64_t Elapsed = Session->bHasSent ? NowMs() - Session->LastSentAt : std::numeric_limits<int64_t>::max();
        if (Elapsed >= Session->IntervalMs || (Session->bMotionSeen && Elapsed >= FSightLimits::MinFrameIntervalMs))
        {
            Send(Session);
        }
    }

    void FNpcifySightInput::Send(FSessionPtr Session)
    {
        Session->bInFlight = true;
        const int64_t CapturedAt = UnixNowMs();
        std::function<void(std::function<void()>)> Run = Later();
        Session->Source->Capture([this, Run, Session, CapturedAt](std::shared_ptr<FSightFrame> Frame)
        {
            Run([this, Session, CapturedAt, Frame]() { Captured(Session, CapturedAt, Frame); });
        });
    }

    void FNpcifySightInput::Captured(FSessionPtr Session, int64_t CapturedAt, std::shared_ptr<FSightFrame> Frame)
    {
        if (!Frame || Frame->Jpeg.empty() || Session != Current)
        {
            // A source without a picture waits for the next check.
            Session->bInFlight = false;
            return;
        }
        const std::string Conversation = ConversationId ? ConversationId() : std::string();
        if (Conversation.empty())
        {
            Session->bInFlight = false;
            Stop();
            return;
        }
        // Only a frame that went out lets its acknowledgement send the next one.
        Session->bHasSent = true;
        Session->LastSentAt = NowMs();
        Session->bMotionSeen = false;
        FSightFrameRequest Request;
        Request.ConversationId = Conversation;
        Request.CapturedAt = CapturedAt;
        Request.Width = Frame->Width;
        Request.Height = Frame->Height;
        Request.Source = Session->Source->Kind();
        Request.Jpeg = std::make_shared<const std::vector<uint8_t>>(std::move(Frame->Jpeg));
        if (Session->bRejoin)
        {
            std::function<void(std::function<void()>)> Run = Later();
            Connection->Join(Conversation, [this, Run, Session, Request](const std::string& Code)
            {
                Run([this, Session, Request, Code]() { Rejoined(Session, Request, Code); });
            });
            return;
        }
        Emit(Session, Request);
    }

    void FNpcifySightInput::Rejoined(FSessionPtr Session, const FSightFrameRequest& Request, const std::string& Code)
    {
        if (!Code.empty())
        {
            Failed(Session, Code);
            Finish(Session);
            return;
        }
        Session->bRejoin = false;
        if (Session != Current)
        {
            Session->bInFlight = false;
            return;
        }
        Emit(Session, Request);
    }

    void FNpcifySightInput::Emit(FSessionPtr Session, const FSightFrameRequest& Request)
    {
        std::function<void(std::function<void()>)> Run = Later();
        Connection->SendFrame(Request, [this, Run, Session](const FSightAck& Ack)
        {
            Run([this, Session, Ack]() { Acknowledged(Session, Ack); });
        });
    }

    void FNpcifySightInput::Acknowledged(FSessionPtr Session, const FSightAck& Ack)
    {
        if (Ack.bAnswered)
        {
            // An accepted frame gives its observation as an event. Other codes skip the frame.
            if (!Ack.bOk && SightCodes::StopsSight(Ack.Error))
            {
                Fail(Session, Ack.Error);
            }
        }
        else
        {
            Failed(Session, Ack.Error);
        }
        Finish(Session);
    }

    void FNpcifySightInput::Failed(FSessionPtr Session, const std::string& Code)
    {
        // A refused conversation stops sight. A timeout skips the frame; a lost connection also
        // joins the conversation again before the next frame.
        if (SightCodes::StopsSight(Code))
        {
            Fail(Session, Code);
            return;
        }
        if (Code != SightCodes::Timeout)
        {
            Session->bRejoin = true;
        }
    }

    void FNpcifySightInput::Finish(FSessionPtr Session)
    {
        Session->bInFlight = false;
        SendWhenDue(Session);
    }

    void FNpcifySightInput::Fail(FSessionPtr Session, const std::string& Code)
    {
        if (Session != Current)
        {
            return;
        }
        End(Session);
        if (Code == SightCodes::Disabled)
        {
            Unavailable = OffMessage;
            bDisabledByServer = true;
        }
        Publish(Code);
        if (OnStopped)
        {
            OnStopped(Code);
        }
    }

    void FNpcifySightInput::End(FSessionPtr Session)
    {
        if (Current != Session)
        {
            return;
        }
        Current.reset();
        if (Session->Source)
        {
            Session->Source->Stop();
        }
    }

    void FNpcifySightInput::Publish(const std::string& ErrorCode)
    {
        const ESightState Next = Current
            ? (Current->Source ? ESightState::On : ESightState::Starting)
            : (!Unavailable.empty() ? ESightState::Unavailable : ESightState::Off);
        if (Next == State && ErrorCode.empty())
        {
            return;
        }
        State = Next;
        if (OnStateChanged)
        {
            OnStateChanged(Next, ErrorCode);
        }
    }
}
