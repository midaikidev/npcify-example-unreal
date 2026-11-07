#include "Core/NpcifyStreamTurn.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace Npcify
{
    namespace
    {
        std::string EventString(const FNpcifyMsgPackValue& Map, const char* Key)
        {
            const FNpcifyMsgPackValue* Value = Map.FindField(Key);
            return Value && Value->IsString() ? Value->StringValue : std::string();
        }

        int64_t EventInt(const FNpcifyMsgPackValue& Map, const char* Key, int64_t Fallback)
        {
            const FNpcifyMsgPackValue* Value = Map.FindField(Key);
            return Value && Value->IsNumber() ? Value->AsInt64(Fallback) : Fallback;
        }

        std::string JoinSpeechText(const std::vector<FStagingDialogueBlock>& Dialogue)
        {
            std::string Text;
            for (const FStagingDialogueBlock& Block : Dialogue)
            {
                if (Block.Type != "speech" || Block.Text.empty())
                {
                    continue;
                }
                if (!Text.empty())
                {
                    Text += '\n';
                }
                Text += Block.Text;
            }
            return Text;
        }

        uint64_t NextTurnSequence()
        {
            static std::atomic<uint64_t> Counter{0};
            return ++Counter;
        }

        bool IsFinalStop(ENpcifyTurnStop Reason)
        {
            return Reason == ENpcifyTurnStop::Game || Reason == ENpcifyTurnStop::Superseded;
        }
    }

    std::string FNpcifyStreamTurn::NormalizeTaskId(std::string Text)
    {
        std::transform(Text.begin(), Text.end(), Text.begin(), [](unsigned char C)
        {
            return static_cast<char>(std::tolower(C));
        });
        return Text;
    }

    FNpcifyStreamTurn::FNpcifyStreamTurn(const std::string& InTaskId)
        : TaskId(NormalizeTaskId(InTaskId))
        , Sequence(NextTurnSequence())
        , Payload(std::make_shared<FStagingResultPayload>())
    {
        Payload->ProtocolVersion = 1;
        Payload->TaskId = TaskId;
    }

    ENpcifyStreamChange FNpcifyStreamTurn::Apply(const FNpcifyMsgPackValue& Event)
    {
        if (!Event.IsMap())
        {
            return ENpcifyStreamChange::None;
        }
        const std::string Type = EventString(Event, "type");
        const bool bStarted = bRunStarted;
        // Each run of the turn sends its start event first.
        bRunStarted = true;
        if (IsFinalStop(StopReason))
        {
            return ENpcifyStreamChange::None;
        }
        if (Type == "start")
        {
            if (bStarted)
            {
                // A retried request runs the turn again under the same task ID.
                return ENpcifyStreamChange::Restarted;
            }
            if (!IsStopped())
            {
                AdoptStagingId(Event);
            }
            return ENpcifyStreamChange::None;
        }
        if (IsStopped())
        {
            // A failed or refused turn: only a rerun of a retried request can start it again.
            return IsRerunPossible() ? ApplyWhileFailed(Type, Event) : ENpcifyStreamChange::None;
        }
        if (Type == "speech_chunk")
        {
            AppendChunk(Event);
        }
        else if (Type == "speech_complete")
        {
            return CompleteAudio(EventString(Event, "streamId"));
        }
        else if (Type == "speech_error")
        {
            BreakAudio(EventString(Event, "streamId"));
        }
        else if (Type == "staging_live" || Type == "staging_prefix")
        {
            return ApplyRevision(Event, false);
        }
        else if (Type == "staging")
        {
            // Servers before the complete flag sent only complete staging events.
            const FNpcifyMsgPackValue* Complete = Event.FindField("complete");
            return ApplyRevision(Event, !(Complete && Complete->IsBool() && !Complete->BoolValue));
        }
        else if (Type == "options")
        {
            return ApplyOptions(Event);
        }
        else if (Type == "error")
        {
            Stop(ENpcifyTurnStop::Failed);
            return ENpcifyStreamChange::Failed;
        }
        return ENpcifyStreamChange::None;
    }

    bool FNpcifyStreamTurn::ApplyResult(const FStagingResultPayload& Result)
    {
        if (StopReason == ENpcifyTurnStop::Rejected)
        {
            throw std::runtime_error(ChangedDialogueMessage);
        }
        if (StopReason == ENpcifyTurnStop::Failed || (!bHasTimeline && !IsStopped()))
        {
            return false;
        }
        Payload->ProtocolVersion = Result.ProtocolVersion;
        Payload->TaskId = Result.TaskId.empty() ? TaskId : Result.TaskId;
        Payload->StagingId = Result.StagingId;
        Payload->Text = Result.Text;
        Payload->Options = Result.Options;
        bHasFinal = true;
        if (IsStopped())
        {
            // The game stopped the reply, or a newer reply replaced it: it does not play again.
            Payload->Timeline = Result.Timeline;
            bComplete = true;
            return true;
        }
        std::vector<FStagingDialogueBlock>& Dialogue = Payload->Timeline.Dialogue;
        const size_t Published = Dialogue.size();
        Merge(FStagingTimeline(Result.Timeline));
        for (size_t Index = 0; Index < Published; ++Index)
        {
            if (!HasPlayableAudio(Dialogue[Index]))
            {
                // Missed or failed live chunks: play the stored audio of the result.
                Dialogue[Index].Audio = Result.Timeline.Dialogue[Index].Audio;
            }
        }
        bComplete = true;
        return true;
    }

    bool FNpcifyStreamTurn::SetBlockAudio(const std::string& BlockId, const std::vector<uint8_t>& Bytes)
    {
        for (FStagingDialogueBlock& Block : Payload->Timeline.Dialogue)
        {
            if (Block.Id != BlockId || HasPlayableAudio(Block))
            {
                continue;
            }
            if (Bytes.empty())
            {
                Silent.insert(BlockId);
            }
            else
            {
                Block.Audio.Data = Bytes;
            }
            return true;
        }
        return false;
    }

    std::vector<std::pair<std::string, std::string>> FNpcifyStreamTurn::TakeAudioDownloads()
    {
        std::vector<std::pair<std::string, std::string>> Downloads;
        if (IsStopped())
        {
            return Downloads;
        }
        for (const FStagingDialogueBlock& Block : Payload->Timeline.Dialogue)
        {
            if (IsBlockReady(Block) || Block.Audio.Type == "stream" || Block.Audio.Url.empty() ||
                !Requested.insert(Block.Id).second)
            {
                continue;
            }
            Downloads.emplace_back(Block.Id, Block.Audio.Url);
        }
        return Downloads;
    }

    size_t FNpcifyStreamTurn::CountReadyBlocks() const
    {
        size_t Count = 0;
        for (const FStagingDialogueBlock& Block : Payload->Timeline.Dialogue)
        {
            if (!IsBlockReady(Block))
            {
                break;
            }
            ++Count;
        }
        return Count;
    }

    void FNpcifyStreamTurn::Stop(ENpcifyTurnStop Reason)
    {
        if (Reason != ENpcifyTurnStop::None && (!IsFinalStop(StopReason) || Reason == ENpcifyTurnStop::Game))
        {
            StopReason = Reason;
        }
    }

    void FNpcifyStreamTurn::Restart(const FNpcifyMsgPackValue& FirstEvent)
    {
        ForgetAudioOf(Payload->Timeline.Dialogue);
        Requested.clear();
        Silent.clear();
        Revision = 0;
        StopReason = ENpcifyTurnStop::None;
        bHasTimeline = false;
        bComplete = false;
        bHasFinal = false;
        bPlaybackStarted = false;
        bRunStarted = true;
        bRerunExpected = false;
        bEndPending = false;
        Noted = 0;
        // A new object: the first run's staging stays as the game got it.
        Payload = std::make_shared<FStagingResultPayload>();
        Payload->ProtocolVersion = 1;
        Payload->TaskId = TaskId;
        AdoptStagingId(FirstEvent);
    }

    // The audio streams of a run that the turn forgets. Each synthesis has a new stream ID, so the
    // streams of a rerun that came before its first revision stay.
    void FNpcifyStreamTurn::ForgetAudioOf(const std::vector<FStagingDialogueBlock>& Dialogue)
    {
        for (const FStagingDialogueBlock& Block : Dialogue)
        {
            if (!Block.Audio.StreamId.empty())
            {
                Audio.erase(Block.Audio.StreamId);
            }
        }
    }

    // A failed or refused turn while its request is sent again: the audio of the rerun can come
    // before its first revision, and a revision that starts over shows the rerun.
    ENpcifyStreamChange FNpcifyStreamTurn::ApplyWhileFailed(const std::string& Type, const FNpcifyMsgPackValue& Event)
    {
        if (Type == "speech_chunk")
        {
            AppendChunk(Event);
        }
        else if (Type == "speech_complete")
        {
            CompleteAudio(EventString(Event, "streamId"));
        }
        else if (Type == "speech_error")
        {
            BreakAudio(EventString(Event, "streamId"));
        }
        else if (Type == "staging_live" || Type == "staging_prefix" || Type == "staging")
        {
            const FNpcifyMsgPackValue* TimelineValue = Event.FindField("timeline");
            if (TimelineValue && TimelineValue->IsMap() &&
                StartsRerun(EventInt(Event, "revision", 0), FNpcifySchemaMapper::MapTimeline(*TimelineValue)))
            {
                return ENpcifyStreamChange::Restarted;
            }
        }
        return ENpcifyStreamChange::None;
    }

    // A revision of a rerun: its numbers start at 1 again, and its dialogue starts over.
    bool FNpcifyStreamTurn::StartsRerun(int64_t Number, const FStagingTimeline& Timeline) const
    {
        return Number <= Revision || !Continues(Timeline);
    }

    bool FNpcifyStreamTurn::HasPlayableAudio(const FStagingDialogueBlock& Block)
    {
        return Block.Type != "speech" || Block.Audio.Type == "silent" || !Block.Audio.Data.empty();
    }

    bool FNpcifyStreamTurn::IsBlockReady(const FStagingDialogueBlock& Block) const
    {
        return HasPlayableAudio(Block) || Silent.count(Block.Id) > 0;
    }

    void FNpcifyStreamTurn::AdoptStagingId(const FNpcifyMsgPackValue& Event)
    {
        const std::string StagingId = EventString(Event, "stagingId");
        if (Payload->StagingId.empty() && !StagingId.empty())
        {
            Payload->StagingId = StagingId;
        }
    }

    void FNpcifyStreamTurn::AppendChunk(const FNpcifyMsgPackValue& Event)
    {
        const std::string StreamId = EventString(Event, "streamId");
        if (StreamId.empty())
        {
            return;
        }
        FAudioStream& Stream = Audio[StreamId];
        if (Stream.bComplete || Stream.bBroken)
        {
            return;
        }
        const FNpcifyMsgPackValue* Data = Event.FindField("data");
        std::vector<uint8_t> Bytes;
        if (Data && Data->IsBinary())
        {
            Bytes = Data->BinaryValue;
        }
        else if (Data && Data->IsString())
        {
            Bytes = FNpcifyProtocol::DecodeBase64(Data->StringValue);
        }
        if (EventInt(Event, "chunkIndex", -1) != Stream.NextIndex || Bytes.empty())
        {
            // A missed chunk: this speech plays the stored audio of the HTTP result.
            BreakAudio(StreamId);
            return;
        }
        Stream.Bytes.insert(Stream.Bytes.end(), Bytes.begin(), Bytes.end());
        ++Stream.NextIndex;
    }

    ENpcifyStreamChange FNpcifyStreamTurn::CompleteAudio(const std::string& StreamId)
    {
        if (StreamId.empty())
        {
            return ENpcifyStreamChange::None;
        }
        FAudioStream& Stream = Audio[StreamId];
        if (Stream.bComplete || Stream.bBroken || Stream.Bytes.empty())
        {
            return ENpcifyStreamChange::None;
        }
        Stream.bComplete = true;
        bool bChanged = false;
        for (FStagingDialogueBlock& Block : Payload->Timeline.Dialogue)
        {
            bChanged = AttachAudio(Block) || bChanged;
        }
        return bChanged ? ENpcifyStreamChange::Audio : ENpcifyStreamChange::None;
    }

    void FNpcifyStreamTurn::BreakAudio(const std::string& StreamId)
    {
        if (StreamId.empty())
        {
            return;
        }
        FAudioStream& Stream = Audio[StreamId];
        if (!Stream.bComplete)
        {
            Stream.bBroken = true;
            Stream.Bytes.clear();
        }
    }

    // A stream block whose chunks are complete gets them as MP3 bytes.
    bool FNpcifyStreamTurn::AttachAudio(FStagingDialogueBlock& Block) const
    {
        if (Block.Audio.Type != "stream" || Block.Audio.StreamId.empty())
        {
            return false;
        }
        const auto Found = Audio.find(Block.Audio.StreamId);
        if (Found == Audio.end() || !Found->second.bComplete)
        {
            return false;
        }
        Block.Audio.Type = "bytes";
        Block.Audio.Data = Found->second.Bytes;
        if (Block.Audio.MimeType.empty())
        {
            Block.Audio.MimeType = "audio/mpeg";
        }
        return true;
    }

    ENpcifyStreamChange FNpcifyStreamTurn::ApplyRevision(const FNpcifyMsgPackValue& Event, bool bCompleteRevision)
    {
        const FNpcifyMsgPackValue* TimelineValue = Event.FindField("timeline");
        if (!TimelineValue || !TimelineValue->IsMap())
        {
            return ENpcifyStreamChange::None;
        }
        const int64_t Number = EventInt(Event, "revision", 0);
        FStagingTimeline Timeline = FNpcifySchemaMapper::MapTimeline(*TimelineValue);
        if (IsRerunPossible() && StartsRerun(Number, Timeline))
        {
            return ENpcifyStreamChange::Restarted;
        }
        if (bComplete || Number <= Revision)
        {
            return ENpcifyStreamChange::None;
        }
        Merge(std::move(Timeline));
        if (!bHasFinal)
        {
            Payload->Text = JoinSpeechText(Payload->Timeline.Dialogue);
        }
        Revision = Number;
        AdoptStagingId(Event);
        bHasTimeline = true;
        bComplete = bCompleteRevision;
        return ENpcifyStreamChange::Timeline;
    }

    ENpcifyStreamChange FNpcifyStreamTurn::ApplyOptions(const FNpcifyMsgPackValue& Event)
    {
        if (bHasFinal)
        {
            return ENpcifyStreamChange::None;
        }
        std::vector<FStagingReplyOption> Options;
        const FNpcifyMsgPackValue* List = Event.FindField("options");
        if (List && List->IsArray())
        {
            for (const FNpcifyMsgPackValue& Item : List->ArrayValue)
            {
                if (!Item.IsMap())
                {
                    continue;
                }
                FStagingReplyOption Option;
                Option.Label = EventString(Item, "label");
                const FNpcifyMsgPackValue* End = Item.FindField("end");
                Option.bEnd = End && End->IsBool() && End->BoolValue;
                Options.push_back(std::move(Option));
            }
        }
        Payload->Options = std::move(Options);
        const FNpcifyMsgPackValue* Text = Event.FindField("text");
        if (Text && Text->IsString())
        {
            Payload->Text = Text->StringValue;
        }
        bHasFinal = true;
        return ENpcifyStreamChange::Final;
    }

    void FNpcifyStreamTurn::ApplyTracks(const FStagingTimeline& Timeline)
    {
        FStagingTimeline& Target = Payload->Timeline;
        Target.Duration = Timeline.Duration;
        Target.Expressions = Timeline.Expressions;
        Target.Animations = Timeline.Animations;
        Target.Motions = Timeline.Motions;
        if (Timeline.Motion.bValid)
        {
            Target.Motion = Timeline.Motion;
        }
    }

    // True when the timeline keeps the published dialogue: the same blocks, of which only a
    // pending speech block can grow.
    bool FNpcifyStreamTurn::Continues(const FStagingTimeline& Next) const
    {
        const std::vector<FStagingDialogueBlock>& Dialogue = Payload->Timeline.Dialogue;
        if (Next.Dialogue.size() < Dialogue.size())
        {
            return false;
        }
        for (size_t Index = 0; Index < Dialogue.size(); ++Index)
        {
            const FStagingDialogueBlock& A = Dialogue[Index];
            const FStagingDialogueBlock& B = Next.Dialogue[Index];
            if (A.Id != B.Id || A.Type != B.Type || A.Text != B.Text || std::abs(A.StartTime - B.StartTime) > 1e-6)
            {
                return false;
            }
            const bool bValid = A.bPending && A.Type == "speech"
                ? !B.bPending || B.EndTime >= A.EndTime - 1e-6
                : std::abs(A.EndTime - B.EndTime) <= 1e-6;
            if (!bValid)
            {
                return false;
            }
        }
        return true;
    }

    // A published block stays as it is, except a pending speech block: it keeps its ID, text and
    // start, and takes the end, timing and lip-sync of its next version (its end grows while it
    // stays pending).
    void FNpcifyStreamTurn::Merge(FStagingTimeline&& Next)
    {
        if (!Continues(Next))
        {
            Stop(ENpcifyTurnStop::Rejected);
            throw std::runtime_error(ChangedDialogueMessage);
        }
        std::vector<FStagingDialogueBlock>& Dialogue = Payload->Timeline.Dialogue;
        for (size_t Index = 0; Index < Dialogue.size(); ++Index)
        {
            FStagingDialogueBlock& Block = Dialogue[Index];
            if (Block.bPending)
            {
                FStagingDialogueBlock& Grown = Next.Dialogue[Index];
                Block.EndTime = Grown.EndTime;
                Block.Duration = Grown.Duration;
                Block.Visemes = std::move(Grown.Visemes);
                Block.Words = std::move(Grown.Words);
                Block.Characters = std::move(Grown.Characters);
                Block.bPending = Grown.bPending;
            }
        }
        for (size_t Index = Dialogue.size(); Index < Next.Dialogue.size(); ++Index)
        {
            FStagingDialogueBlock Block = std::move(Next.Dialogue[Index]);
            AttachAudio(Block);
            Dialogue.push_back(std::move(Block));
        }
        ApplyTracks(Next);
    }
}
