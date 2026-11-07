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
        if (bStopped || !Event.IsMap())
        {
            return ENpcifyStreamChange::None;
        }
        const std::string Type = EventString(Event, "type");
        if (Type == "start")
        {
            AdoptStagingId(Event);
        }
        else if (Type == "speech_chunk")
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
            Fail();
            return ENpcifyStreamChange::Failed;
        }
        return ENpcifyStreamChange::None;
    }

    bool FNpcifyStreamTurn::ApplyResult(const FStagingResultPayload& Result)
    {
        if (!bHasTimeline)
        {
            return false;
        }
        Payload->ProtocolVersion = Result.ProtocolVersion;
        Payload->TaskId = Result.TaskId.empty() ? TaskId : Result.TaskId;
        Payload->StagingId = Result.StagingId;
        Payload->Text = Result.Text;
        Payload->Options = Result.Options;
        bHasFinal = true;
        if (bStopped)
        {
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
        if (bStopped)
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
        const int64_t Number = EventInt(Event, "revision", 0);
        if (bComplete || Number <= Revision || !TimelineValue || !TimelineValue->IsMap())
        {
            return ENpcifyStreamChange::None;
        }
        Merge(FNpcifySchemaMapper::MapTimeline(*TimelineValue));
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

    // A published block stays as it is, except a pending speech block: it keeps its ID, text and
    // start, and takes the end, timing and lip-sync of its next version (its end grows while it
    // stays pending).
    void FNpcifyStreamTurn::Merge(FStagingTimeline&& Next)
    {
        std::vector<FStagingDialogueBlock>& Dialogue = Payload->Timeline.Dialogue;
        bool bValid = Next.Dialogue.size() >= Dialogue.size();
        for (size_t Index = 0; bValid && Index < Dialogue.size(); ++Index)
        {
            const FStagingDialogueBlock& A = Dialogue[Index];
            const FStagingDialogueBlock& B = Next.Dialogue[Index];
            if (A.Id != B.Id || A.Type != B.Type || A.Text != B.Text || std::abs(A.StartTime - B.StartTime) > 1e-6)
            {
                bValid = false;
            }
            else if (A.bPending && A.Type == "speech")
            {
                bValid = !B.bPending || B.EndTime >= A.EndTime - 1e-6;
            }
            else
            {
                bValid = std::abs(A.EndTime - B.EndTime) <= 1e-6;
            }
        }
        if (!bValid)
        {
            Fail();
            throw std::runtime_error(ChangedDialogueMessage);
        }
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
