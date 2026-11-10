#include "NpcifyCharacterComponent.h"

#include "NpcifyConversationSocket.h"
#include "NpcifySightCapture.h"

#include "Containers/Queue.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"

#include <functional>
#include <memory>
#include <string>

// Sight of UNpcifyCharacterComponent (docs/sight.md): the game viewport, a render target, a scene
// capture or another texture; frames go on the conversation stream.

DEFINE_LOG_CATEGORY_STATIC(LogNpcifySight, Log, All);

namespace
{
    /** The conversation stream as the connection of sight; the component gives the two calls. */
    class FNpcifySightConnection final : public Npcify::ISightConnection
    {
    public:
        std::function<void(const std::string&, std::function<void(const std::string&)>)> JoinCall;
        std::function<void(const Npcify::FSightFrameRequest&, std::function<void(const Npcify::FSightAck&)>)> SendCall;

        void Join(const std::string& ConversationId, std::function<void(const std::string&)> Done) override
        {
            JoinCall(ConversationId, std::move(Done));
        }

        void SendFrame(const Npcify::FSightFrameRequest& Request, std::function<void(const Npcify::FSightAck&)> Done) override
        {
            SendCall(Request, std::move(Done));
        }
    };

    ENpcifySightState ToUnreal(Npcify::ESightState State)
    {
        switch (State)
        {
        case Npcify::ESightState::Off: return ENpcifySightState::Off;
        case Npcify::ESightState::Starting: return ENpcifySightState::Starting;
        case Npcify::ESightState::On: return ENpcifySightState::On;
        default: return ENpcifySightState::Unavailable;
        }
    }

    FString ToText(const std::string& Value)
    {
        return UTF8_TO_TCHAR(Value.c_str());
    }

    /** "CODE: message", as the other errors of the component. */
    FString SightError(const std::string& Code)
    {
        return ToText(Code + ": " + Npcify::SightCodes::Message(Code));
    }

    FNpcifySightObservation ToUnreal(const Npcify::FSightObservation& Observation)
    {
        FNpcifySightObservation Result;
        Result.ConversationId = ToText(Observation.ConversationId);
        Result.ObservationId = ToText(Observation.ObservationId);
        Result.CapturedAt = ToText(Observation.CapturedAt);
        Result.People = Observation.People;
        for (const Npcify::FSightPerson& Person : Observation.Persons)
        {
            FNpcifySightPerson& Entry = Result.Persons.AddDefaulted_GetRef();
            Entry.Position = ToText(Person.Position);
            Entry.Action = ToText(Person.Action);
            Entry.bFacingCamera = Person.bFacingCamera;
        }
        for (const std::string& Line : Observation.Text)
        {
            Result.Text.Add(ToText(Line));
        }
        Result.Summary = ToText(Observation.Summary);
        Result.Model = ToText(Observation.Model);
        Result.bCostKnown = Observation.bHasCostPoints;
        Result.CostPoints = static_cast<float>(Observation.CostPoints);
        return Result;
    }

    int64_t UnixMilliseconds()
    {
        return static_cast<int64_t>((FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds());
    }
}

/** The sight state of a component: the schedule, its work for the game thread, and the source. */
struct FNpcifySightBinding
{
    using FWork = TQueue<TFunction<void()>, EQueueMode::Mpsc>;

    TSharedRef<FWork, ESPMode::ThreadSafe> Work = MakeShared<FWork, ESPMode::ThreadSafe>();
    std::unique_ptr<Npcify::FNpcifySightInput> Input;
    /** The source of the current or the last session: its readbacks move on in TickSight. */
    std::shared_ptr<FNpcifySightCapture> Capture;
    FDelegateHandle Background;
    FDelegateHandle Foreground;

    ~FNpcifySightBinding()
    {
        FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(Background);
        FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(Foreground);
        // The schedule stops its source; later results find no schedule.
        Input.reset();
    }
};

FNpcifySightBinding& UNpcifyCharacterComponent::GetSightBinding()
{
    if (SightBinding.IsValid())
    {
        return *SightBinding;
    }
    SightBinding = MakeShared<FNpcifySightBinding>();
    FNpcifySightBinding& Binding = *SightBinding;
    TWeakObjectPtr<UNpcifyCharacterComponent> WeakThis(this);
    TWeakPtr<FNpcifySightBinding::FWork, ESPMode::ThreadSafe> WeakWork = Binding.Work;

    std::shared_ptr<FNpcifySightConnection> Connection = std::make_shared<FNpcifySightConnection>();
    Connection->JoinCall = [WeakThis](const std::string& ConversationId, std::function<void(const std::string&)> Done)
    {
        UNpcifyCharacterComponent* Self = WeakThis.Get();
        if (Self == nullptr || !Self->Runtime.IsValid())
        {
            Done(Npcify::SightCodes::Disconnected);
            return;
        }
        const FString Id = ToText(ConversationId);
        Self->JoinConversationStream(Id, [WeakThis, Id, Done](bool bJoined)
        {
            if (bJoined)
            {
                Done(std::string());
                return;
            }
            const UNpcifyCharacterComponent* Me = WeakThis.Get();
            Done(Me != nullptr && Me->StreamSocket.IsValid() && Me->StreamSocket->IsRefused(Id)
                ? Npcify::SightCodes::AccessDenied
                : Npcify::SightCodes::Disconnected);
        });
    };
    Connection->SendCall = [WeakThis](const Npcify::FSightFrameRequest& Request, std::function<void(const Npcify::FSightAck&)> Done)
    {
        UNpcifyCharacterComponent* Self = WeakThis.Get();
        if (Self == nullptr || !Self->StreamSocket.IsValid())
        {
            Done(Npcify::FSightAck::NotAnswered(Npcify::SightCodes::Disconnected));
            return;
        }
        const double Sent = FPlatformTime::Seconds();
        const int32 Bytes = Request.Jpeg ? static_cast<int32>(Request.Jpeg->size()) : 0;
        const int32 Width = Request.Width;
        const int32 Height = Request.Height;
        Self->StreamSocket->SendSightFrame(Request, [Done, Sent, Bytes, Width, Height](const Npcify::FSightAck& Ack)
        {
            const FString Result = Ack.bOk
                ? TEXT("ok ") + ToText(Ack.ObservationId)
                : Ack.bAnswered ? ToText(Ack.Error) : TEXT("no acknowledgement, ") + ToText(Ack.Error);
            UE_LOG(LogNpcifySight, Log, TEXT("Sight frame %dx%d, %d bytes: %s after %.0f ms"),
                Width, Height, Bytes, *Result, (FPlatformTime::Seconds() - Sent) * 1000.0);
            Done(Ack);
        });
    };

    Binding.Input = std::make_unique<Npcify::FNpcifySightInput>(
        Connection,
        [WeakThis]()
        {
            const UNpcifyCharacterComponent* Self = WeakThis.Get();
            return Self != nullptr && Self->Runtime.IsValid() && !Self->Runtime->HasBakedConversation()
                ? Self->Runtime->GetConversationId()
                : std::string();
        },
        []() { return static_cast<int64_t>(FPlatformTime::Seconds() * 1000.0); },
        []() { return UnixMilliseconds(); },
        [WeakWork](std::function<void()> Item)
        {
            if (const TSharedPtr<FNpcifySightBinding::FWork, ESPMode::ThreadSafe> Work = WeakWork.Pin())
            {
                Work->Enqueue([Item = std::move(Item)]() { Item(); });
            }
        });
    // The state that the events reported already: configured before the handlers.
    if (bSightInfoRead)
    {
        Binding.Input->Configure(bSightEnabled, bSightBaked);
    }
    Binding.Input->OnStateChanged = [WeakThis](Npcify::ESightState State, const std::string& Code)
    {
        UNpcifyCharacterComponent* Self = WeakThis.Get();
        if (Self == nullptr)
        {
            return;
        }
        if ((State == Npcify::ESightState::Off || State == Npcify::ESightState::Unavailable) &&
            Self->SightBinding.IsValid() && Self->SightBinding->Capture)
        {
            UE_LOG(LogNpcifySight, Log, TEXT("Sight stopped%s: %s"),
                Code.empty() ? TEXT("") : *(TEXT(" (") + ToText(Code) + TEXT(")")),
                *Self->SightBinding->Capture->Describe());
            Self->SightBinding->Capture.reset();
        }
        Self->OnSightState.Broadcast(ToUnreal(State), ToText(Code));
    };
    Binding.Input->OnStopped = [WeakThis](const std::string& Code)
    {
        if (UNpcifyCharacterComponent* Self = WeakThis.Get())
        {
            Self->OnError.Broadcast(SightError(Code));
        }
    };
    Binding.Input->OnObservation = [WeakThis](const Npcify::FSightObservation& Observation)
    {
        if (UNpcifyCharacterComponent* Self = WeakThis.Get())
        {
            Self->OnSight.Broadcast(ToUnreal(Observation));
        }
    };
    // No frames while the application is in the background.
    Binding.Background = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddLambda([WeakThis]()
    {
        UNpcifyCharacterComponent* Self = WeakThis.Get();
        if (Self != nullptr && Self->SightBinding.IsValid())
        {
            Self->SightBinding->Input->SetHidden(true);
        }
    });
    Binding.Foreground = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddLambda([WeakThis]()
    {
        UNpcifyCharacterComponent* Self = WeakThis.Get();
        if (Self != nullptr && Self->SightBinding.IsValid())
        {
            Self->SightBinding->Input->SetHidden(false);
        }
    });
    return Binding;
}

bool UNpcifyCharacterComponent::StartSight(const FNpcifySightOptions& Options)
{
    EnsureInitialized();
    FNpcifySightBinding& Binding = GetSightBinding();
    std::shared_ptr<FNpcifySightCapture> Source = std::make_shared<FNpcifySightCapture>(
        Options.SceneCapture != nullptr ? nullptr : Options.Texture.Get(),
        Options.SceneCapture.Get(),
        Options.bCameraSource);
    Npcify::FSightSchedule Schedule;
    Schedule.IntervalMs = Options.IntervalMs;
    Schedule.bMotion = Options.bMotion;
    Schedule.MotionThreshold = Options.MotionThreshold;
    TWeakObjectPtr<UNpcifyCharacterComponent> WeakThis(this);
    const std::string Refused = Binding.Input->Start(Schedule, Source, [WeakThis](const std::string& Code)
    {
        UNpcifyCharacterComponent* Self = WeakThis.Get();
        if (Self != nullptr && !Code.empty())
        {
            Self->OnError.Broadcast(SightError(Code));
        }
    });
    if (!Refused.empty())
    {
        OnError.Broadcast(SightError(Refused));
        return false;
    }
    Binding.Capture = Source;
    return true;
}

void UNpcifyCharacterComponent::StopSight()
{
    if (SightBinding.IsValid())
    {
        SightBinding->Input->Stop();
    }
}

ENpcifySightState UNpcifyCharacterComponent::GetSightState() const
{
    return SightBinding.IsValid() ? ToUnreal(SightBinding->Input->GetState()) : ConfiguredSightState();
}

ENpcifySightState UNpcifyCharacterComponent::ConfiguredSightState() const
{
    return bSightInfoRead && bSightEnabled && !bSightBaked ? ENpcifySightState::Off : ENpcifySightState::Unavailable;
}

void UNpcifyCharacterComponent::ConfigureSight(bool bEnabled, bool bBaked)
{
    const ENpcifySightState Before = GetSightState();
    bSightInfoRead = true;
    bSightEnabled = bEnabled;
    bSightBaked = bBaked;
    if (SightBinding.IsValid())
    {
        SightBinding->Input->Configure(bEnabled, bBaked);
        return;
    }
    // No binding yet: the event reports the change, as the schedule would.
    const ENpcifySightState After = ConfiguredSightState();
    if (After != Before)
    {
        OnSightState.Broadcast(After, FString());
    }
}

bool UNpcifyCharacterComponent::IsSightInfoStale() const
{
    return SightBinding.IsValid() && SightBinding->Input->IsDisabledByServer();
}

void UNpcifyCharacterComponent::TickSight()
{
    if (!SightBinding.IsValid())
    {
        return;
    }
    // A handler can end the play of this component: keep the state alive for this tick.
    const TSharedPtr<FNpcifySightBinding> Binding = SightBinding;
    TFunction<void()> Item;
    while (Binding->Work->Dequeue(Item))
    {
        Item();
    }
    if (Binding->Capture)
    {
        Binding->Capture->Tick();
    }
    Binding->Input->Tick();
}

void UNpcifyCharacterComponent::ResetSight()
{
    if (!SightBinding.IsValid())
    {
        return;
    }
    SightBinding->Input->Stop();
    SightBinding.Reset();
}

void UNpcifyCharacterComponent::HandleSightObservation(const std::string& Json)
{
    Npcify::FSightObservation Observation;
    if (!Runtime.IsValid() || Runtime->HasBakedConversation() || !Npcify::FSightObservation::FromEventJson(Json, Observation) ||
        Observation.ConversationId != Runtime->GetConversationId())
    {
        // Only the observations of the current conversation count.
        return;
    }
    if (SightBinding.IsValid())
    {
        SightBinding->Input->HandleObservation(Observation);
    }
    else
    {
        // Another client of the conversation can send frames before this one starts sight.
        OnSight.Broadcast(ToUnreal(Observation));
    }
}
