#include "Core/NpcifyCharacterRuntime.h"

#include <algorithm>
#include <cstdio>
#include <exception>

// Replies that play while the server makes them (docs/streaming_reply.md): each published
// dialogue prefix of conversations:stream plays when the audio of its blocks is ready; the HTTP
// result completes the reply and plays as a whole only when no revision arrived.

namespace Npcify
{
    bool FNpcifyCharacterRuntime::IsTurnEvent(const FNpcifyMsgPackValue& Event)
    {
        const FNpcifyMsgPackValue* Type = Event.IsMap() ? Event.FindField("type") : nullptr;
        if (!Type || !Type->IsString())
        {
            return false;
        }
        const std::string& Name = Type->StringValue;
        return Name == "start" || Name == "speech_chunk" || Name == "speech_complete" || Name == "speech_error" ||
            Name == "staging_live" || Name == "staging_prefix" || Name == "staging" || Name == "options" || Name == "error";
    }

    void FNpcifyCharacterRuntime::BeginStreamTurn(const std::string& TaskId)
    {
        StreamTurns.push_back(std::make_shared<FNpcifyStreamTurn>(TaskId));
    }

    void FNpcifyCharacterRuntime::HandleStreamEvent(const FNpcifyMsgPackValue& Event)
    {
        if (!IsTurnEvent(Event))
        {
            return;
        }
        const FNpcifyMsgPackValue* TaskId = Event.FindField("taskId");
        const std::shared_ptr<FNpcifyStreamTurn> Turn =
            TaskId && TaskId->IsString() ? FindTurn(FNpcifyStreamTurn::NormalizeTaskId(TaskId->StringValue)) : nullptr;
        if (!Turn)
        {
            return;
        }
        ENpcifyStreamChange Change = ENpcifyStreamChange::None;
        try
        {
            Change = Turn->Apply(Event);
        }
        catch (const std::exception& Error)
        {
            // A revision that changes published dialogue: the turn stops, nothing replays.
            EndTurn(Turn);
            if (OnError) OnError(Error.what());
            return;
        }
        if (Change == ENpcifyStreamChange::Failed)
        {
            // The HTTP request fails too and reports the error.
            NoteTurn(*Turn, "the server stopped the turn");
            EndTurn(Turn);
        }
        else if (Change != ENpcifyStreamChange::None)
        {
            NoteMilestones(*Turn);
            PumpTurn(Turn);
        }
    }

    bool FNpcifyCharacterRuntime::HandleTurnResult(const std::string& TaskId, const FStagingResultPayload& Result)
    {
        const std::shared_ptr<FNpcifyStreamTurn> Turn = FindTurn(FNpcifyStreamTurn::NormalizeTaskId(TaskId));
        if (!Turn)
        {
            return false;
        }
        Turn->bRequestClosed = true;
        bool bOwned = false;
        try
        {
            bOwned = Turn->ApplyResult(Result);
        }
        catch (const std::exception& Error)
        {
            EndTurn(Turn);
            if (OnError) OnError(Error.what());
            return true;
        }
        NoteTurn(*Turn, bOwned ? "the HTTP result completes the streamed reply" : "the HTTP result plays: no revision arrived");
        if (!bOwned)
        {
            // Late events of this turn are ignored: the result plays as a whole.
            EndTurn(Turn);
            return false;
        }
        PumpTurn(Turn);
        ForgetClosedTurns();
        return true;
    }

    bool FNpcifyCharacterRuntime::HandleTurnFailure(const std::string& TaskId)
    {
        const std::shared_ptr<FNpcifyStreamTurn> Turn = FindTurn(FNpcifyStreamTurn::NormalizeTaskId(TaskId));
        if (!Turn)
        {
            return false;
        }
        Turn->bRequestClosed = true;
        if (Turn->HasTimeline() && Turn->IsComplete() && Turn->HasFinal() && !Turn->IsStopped())
        {
            // The stream delivered the whole reply: a failed answer (for example a timeout) does not fail it.
            PumpTurn(Turn);
            ForgetClosedTurns();
            return true;
        }
        EndTurn(Turn);
        return false;
    }

    void FNpcifyCharacterRuntime::SetBlockAudio(
        const std::string& TaskId,
        const std::string& BlockId,
        const std::vector<uint8_t>& Bytes
    )
    {
        const std::shared_ptr<FNpcifyStreamTurn> Turn = FindTurn(FNpcifyStreamTurn::NormalizeTaskId(TaskId));
        if (Turn && Turn->SetBlockAudio(BlockId, Bytes))
        {
            PumpTurn(Turn);
        }
    }

    std::shared_ptr<FNpcifyStreamTurn> FNpcifyCharacterRuntime::FindTurn(const std::string& TaskId) const
    {
        for (const std::shared_ptr<FNpcifyStreamTurn>& Turn : StreamTurns)
        {
            if (Turn->GetTaskId() == TaskId)
            {
                return Turn;
            }
        }
        return PlayingTurn && PlayingTurn->GetTaskId() == TaskId ? PlayingTurn : nullptr;
    }

    // Starts the playback once the first block can play, adds the blocks that became ready,
    // asks for stored audio, and raises a pending OnSpeechEnd once the options are known.
    void FNpcifyCharacterRuntime::PumpTurn(const std::shared_ptr<FNpcifyStreamTurn>& Turn)
    {
        if (Turn->IsStopped())
        {
            return;
        }
        if (!Turn->IsPlaybackStarted())
        {
            if (Turn->CountReadyBlocks() > 0 && (!PlayingTurn || PlayingTurn->GetSequence() < Turn->GetSequence()))
            {
                StartStreamPlayback(Turn);
            }
        }
        else if (Turn == PlayingTurn)
        {
            StagingPlayer.UpdateStream(Turn->CountReadyBlocks(), Turn->IsComplete());
            if (Turn->HasFinal())
            {
                LastOptions = Turn->GetPayload()->Options;
            }
            if (Turn->bEndPending && Turn->HasFinal())
            {
                Turn->bEndPending = false;
                PlayingTurn.reset();
                const std::shared_ptr<FStagingResultPayload> Payload = Turn->GetPayload();
                if (OnSpeechEnd) OnSpeechEnd(*Payload);
                return;
            }
        }
        if (OnBlockAudioRequested && !Turn->IsStopped())
        {
            for (const auto& Download : Turn->TakeAudioDownloads())
            {
                OnBlockAudioRequested(Turn->GetTaskId(), Download.first, Download.second);
            }
        }
    }

    void FNpcifyCharacterRuntime::StartStreamPlayback(const std::shared_ptr<FNpcifyStreamTurn>& Turn)
    {
        if (PlayingTurn)
        {
            PlayingTurn->Stop();
        }
        for (const std::shared_ptr<FNpcifyStreamTurn>& Waiting : StreamTurns)
        {
            if (Waiting->GetSequence() < Turn->GetSequence() && !Waiting->IsPlaybackStarted())
            {
                // An older reply that did not play yet never plays over this one.
                Waiting->Stop();
            }
        }
        PlayingTurn = Turn;
        Turn->MarkStarted();
        bThinking = false;
        const std::shared_ptr<FStagingResultPayload> Payload = Turn->GetPayload();
        ActiveStagingId = Payload->StagingId;
        LastOptions = Payload->Options;
        NoteTurn(*Turn, "the streamed reply plays (" + std::to_string(Payload->Timeline.Dialogue.size()) + " block(s) published)");
        if (OnSpeechStart) OnSpeechStart(*Payload);
        if (PlayingTurn != Turn)
        {
            return;
        }
        StagingPlayer.PlayStream(Payload, Turn->CountReadyBlocks(), Turn->IsComplete());
        ForgetClosedTurns();
    }

    // The turn plays no more: its playback stops without OnSpeechEnd.
    void FNpcifyCharacterRuntime::EndTurn(const std::shared_ptr<FNpcifyStreamTurn>& Turn)
    {
        Turn->Stop();
        if (PlayingTurn == Turn)
        {
            PlayingTurn.reset();
            bThinking = false;
            StagingPlayer.Stop();
        }
        ForgetClosedTurns();
    }

    // A turn stays findable while its request is open, so that its result never plays it again.
    void FNpcifyCharacterRuntime::ForgetClosedTurns()
    {
        StreamTurns.erase(
            std::remove_if(StreamTurns.begin(), StreamTurns.end(), [](const std::shared_ptr<FNpcifyStreamTurn>& Turn)
            {
                return Turn->bRequestClosed && (Turn->IsStopped() || Turn->IsPlaybackStarted());
            }),
            StreamTurns.end());
    }

    void FNpcifyCharacterRuntime::StopStreamTurns()
    {
        for (const std::shared_ptr<FNpcifyStreamTurn>& Turn : StreamTurns)
        {
            Turn->Stop();
        }
        StreamTurns.clear();
        if (PlayingTurn)
        {
            PlayingTurn->Stop();
            PlayingTurn.reset();
        }
    }

    void FNpcifyCharacterRuntime::NoteMilestones(FNpcifyStreamTurn& Turn) const
    {
        const std::vector<FStagingDialogueBlock>& Dialogue = Turn.GetPayload()->Timeline.Dialogue;
        if (Turn.Noted < 1 && Turn.HasTimeline())
        {
            Turn.Noted = 1;
            NoteTurn(Turn, "the first revision arrived");
        }
        if (Turn.Noted < 2 && !Dialogue.empty() && FNpcifyStreamTurn::HasPlayableAudio(Dialogue.front()))
        {
            Turn.Noted = 2;
            NoteTurn(Turn, "the audio of the first block is complete");
        }
    }

    void FNpcifyCharacterRuntime::NoteTurn(const FNpcifyStreamTurn& Turn, const std::string& Message) const
    {
        if (!OnStreamNote)
        {
            return;
        }
        char Seconds[32];
        std::snprintf(Seconds, sizeof(Seconds), "%.2f", Turn.GetElapsedSeconds());
        OnStreamNote("Turn " + Turn.GetTaskId().substr(0, 8) + ": " + Message + " after " + Seconds + " s.");
    }

    void FNpcifyCharacterRuntime::HandleStagingCompleted(const FStagingResultPayload& Staging)
    {
        if (PlayingTurn && Staging.StagingId == PlayingTurn->GetPayload()->StagingId)
        {
            if (!PlayingTurn->HasFinal())
            {
                // The options are not known yet: the end comes with them.
                PlayingTurn->bEndPending = true;
                return;
            }
            PlayingTurn.reset();
        }
        if (OnSpeechEnd) OnSpeechEnd(Staging);
    }
}
