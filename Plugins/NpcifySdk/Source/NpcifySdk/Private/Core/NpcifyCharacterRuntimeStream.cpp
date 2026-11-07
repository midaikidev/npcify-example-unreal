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
        catch (const std::exception&)
        {
            // A revision that changes published dialogue: the reply stops, nothing replays. The
            // request reports the failure (its result is refused too).
            NoteTurn(*Turn, "a revision changed published dialogue");
            EndTurn(Turn, ENpcifyTurnStop::Rejected);
            return;
        }
        switch (Change)
        {
        case ENpcifyStreamChange::None:
            return;
        case ENpcifyStreamChange::Restarted:
            RestartTurn(Turn, Event);
            return;
        case ENpcifyStreamChange::Failed:
            // The HTTP request fails too and reports the error.
            NoteTurn(*Turn, "the server stopped the turn");
            EndTurn(Turn, ENpcifyTurnStop::Failed);
            return;
        default:
            NoteMilestones(*Turn);
            PumpTurn(Turn);
            return;
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
        if (Turn->GetSequence() >= PlayedSequence)
        {
            // The next turn continues from this staging, unless a newer reply played already.
            ActiveStagingId = Result.StagingId;
        }
        bool bOwned = false;
        try
        {
            bOwned = Turn->ApplyResult(Result);
        }
        catch (const std::exception& Error)
        {
            // The result changes dialogue that played: the turn fails.
            EndTurn(Turn, ENpcifyTurnStop::Rejected);
            SettleThinking();
            if (OnError) OnError(Error.what());
            return true;
        }
        if (bOwned && Turn->IsStopped())
        {
            // The game stopped the reply, or a newer reply replaced it: the result does not play.
            NoteTurn(*Turn, "the HTTP result of a stopped reply does not play");
            ForgetClosedTurns();
            SettleThinking();
            return true;
        }
        if (!bOwned)
        {
            // No revision arrived, or the stream failed: the result plays as a whole, unless a
            // newer reply plays already. Late events of this turn are ignored.
            const bool bNewest = Turn->GetSequence() >= PlayedSequence;
            NoteTurn(*Turn, bNewest ? "the HTTP result plays as a whole" : "a newer reply plays: the HTTP result does not");
            EndTurn(Turn, ENpcifyTurnStop::Superseded);
            if (!bNewest)
            {
                SettleThinking();
                return true;
            }
            PlayedSequence = Turn->GetSequence();
            SupersedeOlderTurns(PlayedSequence);
            return false;
        }
        NoteTurn(*Turn, "the HTTP result completes the streamed reply");
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
        if (Turn->HasTimeline() && Turn->HasFinal() && !Turn->IsFailed() && Turn->AreAllBlocksReady())
        {
            // The stream delivered the whole reply, its audio and its options: a failed answer
            // (for example a timeout) does not fail it.
            PumpTurn(Turn);
            ForgetClosedTurns();
            SettleThinking();
            return true;
        }
        EndTurn(Turn, ENpcifyTurnStop::Failed);
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

    // A retried request: the server runs the turn again under the same task ID.
    void FNpcifyCharacterRuntime::RestartTurn(const std::shared_ptr<FNpcifyStreamTurn>& Turn, const FNpcifyMsgPackValue& StartEvent)
    {
        if (Turn->GetSequence() < PlayedSequence)
        {
            // A newer reply plays already.
            Turn->Stop(ENpcifyTurnStop::Superseded);
            return;
        }
        if (PlayingTurn == Turn)
        {
            // The first run stops without OnSpeechEnd; the new run plays from its start.
            PlayingTurn.reset();
            StagingPlayer.Stop();
        }
        Turn->Restart(StartEvent);
        bThinking = true;
        NoteTurn(*Turn, "the server runs the turn again");
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
            // A rerun of the reply that played last can play again.
            if (Turn->CountReadyBlocks() > 0 && Turn->GetSequence() >= PlayedSequence)
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
        StopPlayingTurn(ENpcifyTurnStop::Superseded);
        SupersedeOlderTurns(Turn->GetSequence());
        PlayingTurn = Turn;
        PlayedSequence = Turn->GetSequence();
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

    // Older replies never play (or play again) over a newer one.
    void FNpcifyCharacterRuntime::SupersedeOlderTurns(uint64_t Sequence)
    {
        for (const std::shared_ptr<FNpcifyStreamTurn>& Older : StreamTurns)
        {
            if (Older->GetSequence() < Sequence && Older != PlayingTurn)
            {
                Older->Stop(ENpcifyTurnStop::Superseded);
            }
        }
        ForgetClosedTurns();
    }

    // The playing reply stops. If it already played to its end and only waited for its options,
    // its OnSpeechEnd fires now: the game heard the whole reply.
    void FNpcifyCharacterRuntime::StopPlayingTurn(ENpcifyTurnStop Reason)
    {
        const std::shared_ptr<FNpcifyStreamTurn> Turn = PlayingTurn;
        if (!Turn)
        {
            return;
        }
        PlayingTurn.reset();
        const bool bEnded = Turn->bEndPending;
        Turn->Stop(Reason);
        StagingPlayer.Stop();
        ForgetClosedTurns();
        if (bEnded && OnSpeechEnd)
        {
            OnSpeechEnd(*Turn->GetPayload());
        }
    }

    // The turn plays no more; a reply that played to its end still gets its OnSpeechEnd.
    void FNpcifyCharacterRuntime::EndTurn(const std::shared_ptr<FNpcifyStreamTurn>& Turn, ENpcifyTurnStop Reason)
    {
        if (PlayingTurn == Turn)
        {
            StopPlayingTurn(Reason);
            bThinking = false;
        }
        else
        {
            Turn->Stop(Reason);
        }
        ForgetClosedTurns();
    }

    // A turn stays findable while its request is open: a retry can run it again, and its result
    // never plays again over its stream.
    void FNpcifyCharacterRuntime::ForgetClosedTurns()
    {
        StreamTurns.erase(
            std::remove_if(StreamTurns.begin(), StreamTurns.end(), [](const std::shared_ptr<FNpcifyStreamTurn>& Turn)
            {
                return Turn->bRequestClosed && (Turn->IsStopped() || Turn->IsPlaybackStarted());
            }),
            StreamTurns.end());
    }

    // Thinking ends when no reply can start any more: no open request and no reply that waits.
    void FNpcifyCharacterRuntime::SettleThinking()
    {
        if (PlayingTurn)
        {
            return;
        }
        for (const std::shared_ptr<FNpcifyStreamTurn>& Turn : StreamTurns)
        {
            if (!Turn->bRequestClosed || (!Turn->IsStopped() && !Turn->IsPlaybackStarted()))
            {
                return;
            }
        }
        bThinking = false;
    }

    void FNpcifyCharacterRuntime::StopStreamTurns()
    {
        for (const std::shared_ptr<FNpcifyStreamTurn>& Turn : StreamTurns)
        {
            Turn->Stop(ENpcifyTurnStop::Game);
        }
        StreamTurns.clear();
        if (PlayingTurn)
        {
            PlayingTurn->Stop(ENpcifyTurnStop::Game);
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
