#include "Core/NpcifyCharacterRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace Npcify
{
    namespace
    {
        // Not the list index: a revision can add a clip in front of clips that played.
        std::string ClipKey(const std::string& Id, const std::string& Name, double StartTime)
        {
            if (!Id.empty())
            {
                return Id;
            }
            char Start[32];
            std::snprintf(Start, sizeof(Start), "%.3f", StartTime);
            return Name + ":" + Start;
        }
    }

    void FNpcifyPlayedClips::Clear()
    {
        Keys.clear();
        Spans.clear();
    }

    bool FNpcifyPlayedClips::Add(const std::string& Id, const std::string& Name, double StartTime, double EndTime, bool bMatchMoved)
    {
        bool bMoved = false;
        for (size_t Index = 0; bMatchMoved && !bMoved && Index < Spans.size(); ++Index)
        {
            const FSpan& Span = Spans[Index];
            bMoved = Span.Name == Name && Span.Start < EndTime && StartTime < Span.End;
        }
        if (!Keys.insert(ClipKey(Id, Name, StartTime)).second)
        {
            return false;
        }
        Spans.push_back(FSpan{Name, StartTime, EndTime});
        return !bMoved;
    }

    FNpcifyStagingPlayer::FNpcifyStagingPlayer(
        FNpcifyFacialMixer& InFacialMixer,
        FNpcifyVisemePlayer& InVisemePlayer
    )
        : FacialMixer(InFacialMixer)
        , VisemePlayer(InVisemePlayer)
    {
    }

    const FStagingDialogueBlock* FNpcifyStagingPlayer::GetCurrentBlock() const
    {
        if (!ActiveStaging || ActiveBlockIndex < 0 ||
            static_cast<size_t>(ActiveBlockIndex) >= ActiveStaging->Timeline.Dialogue.size())
        {
            return nullptr;
        }
        return &ActiveStaging->Timeline.Dialogue[static_cast<size_t>(ActiveBlockIndex)];
    }

    FNpcifyPlaybackCursor FNpcifyStagingPlayer::GetCursor() const
    {
        FNpcifyPlaybackCursor Out;
        const FStagingDialogueBlock* Block = GetCurrentBlock();
        // A block that started after its time ends after the timeline.
        Out.DurationSeconds = Block ? std::max(TotalDurationSeconds, ActiveEnd()) : TotalDurationSeconds;
        Out.TimeSeconds = std::min(CurrentTimeSeconds, Out.DurationSeconds);
        Out.BlockId = Block ? Block->Id : "";
        return Out;
    }

    void FNpcifyStagingPlayer::Play(const FStagingResultPayload& Staging)
    {
        Start(std::make_shared<const FStagingResultPayload>(Staging), false, 0, true);
    }

    void FNpcifyStagingPlayer::PlayStream(
        std::shared_ptr<const FStagingResultPayload> Staging,
        size_t InReadyBlocks,
        bool bComplete
    )
    {
        Start(std::move(Staging), true, InReadyBlocks, bComplete);
    }

    void FNpcifyStagingPlayer::UpdateStream(size_t InReadyBlocks, bool bComplete)
    {
        if (!ActiveStaging || !bStreamed)
        {
            return;
        }
        ReadyBlocks = InReadyBlocks;
        bStreamComplete = bComplete;
        RecomputeDuration();
        // A pending block that plays grows: its lip-sync cues load again, at the same block time.
        if (const FStagingDialogueBlock* Block = GetCurrentBlock())
        {
            VisemePlayer.LoadCues(Block->Visemes);
        }
        // Clips of a new revision that ended before the current staging time do not play.
        const FStagingTimeline& Timeline = ActiveStaging->Timeline;
        for (const FStagingExpressionClip& Clip : Timeline.Expressions)
        {
            if (Clip.EndTime <= CurrentTimeSeconds)
            {
                PlayedExpressions.Add(Clip.Id, Clip.Expression, Clip.StartTime, Clip.EndTime, false);
            }
        }
        for (const FStagingAnimationClip& Clip : Timeline.Animations)
        {
            if (Clip.EndTime <= CurrentTimeSeconds)
            {
                PlayedAnimations.Add(Clip.Id, Clip.Animation, Clip.StartTime, Clip.EndTime, false);
            }
        }
    }

    void FNpcifyStagingPlayer::Start(
        std::shared_ptr<const FStagingResultPayload> Staging,
        bool bInStreamed,
        size_t InReadyBlocks,
        bool bComplete
    )
    {
        Stop();
        ActiveStaging = std::move(Staging);
        bStreamed = bInStreamed;
        ReadyBlocks = InReadyBlocks;
        bStreamComplete = bComplete;
        CurrentTimeSeconds = 0.0;
        bIsPlaying = true;
        CurrentText = ActiveStaging->Text;
        RevealedText.clear();
        RecomputeDuration();
        Tick(0.0);
    }

    void FNpcifyStagingPlayer::RecomputeDuration()
    {
        const FStagingTimeline& Timeline = ActiveStaging->Timeline;
        double ComputedEnd = Timeline.Duration;
        // A pending block plays to the end of its audio.
        for (const auto& B : Timeline.Dialogue) ComputedEnd = std::max(ComputedEnd, BlockEnd(B));
        for (const auto& E : Timeline.Expressions) ComputedEnd = std::max(ComputedEnd, E.EndTime);
        for (const auto& A : Timeline.Animations) ComputedEnd = std::max(ComputedEnd, A.EndTime);
        TotalDurationSeconds = std::max(0.01, ComputedEnd);
    }

    double FNpcifyStagingPlayer::AvailableEnd() const
    {
        if (!bStreamed)
        {
            return std::numeric_limits<double>::infinity();
        }
        // The clock stops at the first block that cannot play yet, or at the end of the
        // published dialogue while more may come.
        const std::vector<FStagingDialogueBlock>& Dialogue = ActiveStaging->Timeline.Dialogue;
        double Limit = 0.0;
        if (ReadyBlocks < Dialogue.size())
        {
            Limit = Dialogue[ReadyBlocks].StartTime;
        }
        else if (bStreamComplete)
        {
            return std::numeric_limits<double>::infinity();
        }
        else
        {
            Limit = Dialogue.empty() ? 0.0 : BlockEnd(Dialogue.back());
        }
        // A block that started after its time plays to its own end.
        return GetCurrentBlock() ? std::max(Limit, ActiveEnd()) : Limit;
    }

    void FNpcifyStagingPlayer::SetBlockAudioLength(const std::string& BlockId, double Seconds)
    {
        if (!BlockId.empty() && Seconds > 0.0 && !std::isinf(Seconds))
        {
            AudioLengths[BlockId] = Seconds;
            if (ActiveStaging)
            {
                RecomputeDuration();
            }
        }
    }

    double FNpcifyStagingPlayer::BlockEnd(const FStagingDialogueBlock& Block) const
    {
        // The estimated end of a pending block comes before the end of its complete audio; the
        // voice keeps playing, so the clock does too. Its final version ends it at its end time.
        if (!Block.bPending)
        {
            return Block.EndTime;
        }
        const auto Found = AudioLengths.find(Block.Id);
        return Found == AudioLengths.end() ? Block.EndTime : std::max(Block.EndTime, Block.StartTime + Found->second);
    }

    double FNpcifyStagingPlayer::ActiveEnd() const
    {
        const FStagingDialogueBlock& Block = ActiveStaging->Timeline.Dialogue[static_cast<size_t>(ActiveBlockIndex)];
        return ActiveStartedAt + (BlockEnd(Block) - Block.StartTime);
    }

    bool FNpcifyStagingPlayer::HasStarted(size_t Index) const
    {
        return Index < StartedBlocks.size() && StartedBlocks[Index];
    }

    bool FNpcifyStagingPlayer::AllBlocksStarted() const
    {
        for (size_t Index = 0; Index < ActiveStaging->Timeline.Dialogue.size(); ++Index)
        {
            if (!HasStarted(Index))
            {
                return false;
            }
        }
        return true;
    }

    int FNpcifyStagingPlayer::FindBlockToPlay() const
    {
        // The earliest block that can play, did not start and whose time came; also when the clock
        // passed its end: it then plays from its own start. The end of a block can move back when
        // its final version comes, and a block never starts again.
        const std::vector<FStagingDialogueBlock>& Dialogue = ActiveStaging->Timeline.Dialogue;
        const size_t Playable = bStreamed ? std::min(ReadyBlocks, Dialogue.size()) : Dialogue.size();
        int Due = -1;
        for (size_t Index = 0; Index < Playable; ++Index)
        {
            const FStagingDialogueBlock& Block = Dialogue[Index];
            if (!HasStarted(Index) && Block.StartTime <= CurrentTimeSeconds &&
                (Due < 0 || Block.StartTime < Dialogue[static_cast<size_t>(Due)].StartTime))
            {
                Due = static_cast<int>(Index);
            }
        }
        // The block that plays stays until its end. At its end a block that is due starts: after a
        // block that started late, the clock limit is its end, and the next block must not wait for
        // a later one.
        if (GetCurrentBlock())
        {
            const double End = ActiveEnd();
            if (CurrentTimeSeconds < End || (CurrentTimeSeconds == End && Due < 0))
            {
                return ActiveBlockIndex;
            }
        }
        return Due;
    }

    void FNpcifyStagingPlayer::Stop()
    {
        const FStagingDialogueBlock* Prev = GetCurrentBlock();
        if (bIsPlaying && Prev && OnDialogueBlockEnded)
        {
            OnDialogueBlockEnded(*Prev);
        }
        ActiveStaging.reset();
        ActiveBlockIndex = -1;
        StartedBlocks.clear();
        ActiveStartedAt = 0.0;
        ActiveSeenAt = 0.0;
        LastEndedAt = 0.0;
        AudioLengths.clear();
        NextCharIndex = 0;
        CurrentTimeSeconds = 0.0;
        TotalDurationSeconds = 0.0;
        bIsPlaying = false;
        bStreamed = false;
        bWaiting = false;
        PlayedExpressions.Clear();
        PlayedAnimations.Clear();
        VisemePlayer.Clear();
        FacialMixer.ClearVisemes();
    }

    void FNpcifyStagingPlayer::Tick(double DeltaTimeSeconds, float FallbackAudioRms)
    {
        if (!bIsPlaying || !ActiveStaging)
        {
            FacialMixer.Update(static_cast<float>(std::max(0.0, DeltaTimeSeconds)));
            return;
        }

        // Keep the staging alive while callbacks run: a callback can play another staging.
        const std::shared_ptr<const FStagingResultPayload> Staging = ActiveStaging;
        const double Dt = std::max(0.0, DeltaTimeSeconds);
        // A generation gap of a streamed staging does not advance its clock. The clock never runs
        // back: the final version of a pending block can end before the end of its audio.
        const double Limit = AvailableEnd();
        CurrentTimeSeconds = std::max(CurrentTimeSeconds, std::min(CurrentTimeSeconds + Dt, Limit));
        bWaiting = bStreamed && CurrentTimeSeconds >= Limit;

        const auto& Timeline = Staging->Timeline;
        for (size_t i = 0; i < Timeline.Expressions.size(); ++i)
        {
            const auto& Clip = Timeline.Expressions[i];
            // Revisions of a streamed staging can move clips that played.
            if (CurrentTimeSeconds >= Clip.StartTime &&
                PlayedExpressions.Add(Clip.Id, Clip.Expression, Clip.StartTime, Clip.EndTime, bStreamed))
            {
                // A clip that a revision added after its start plays for the rest of its time.
                const float Dur = static_cast<float>(std::max(0.2, Clip.EndTime - std::max(Clip.StartTime, CurrentTimeSeconds)));
                FacialMixer.PlayCue(Clip.Expression, static_cast<float>(Clip.Weight), Dur);
                if (OnExpressionClipTriggered) OnExpressionClipTriggered(Clip);
            }
        }

        for (size_t i = 0; i < Timeline.Animations.size(); ++i)
        {
            const auto& Clip = Timeline.Animations[i];
            if (CurrentTimeSeconds >= Clip.StartTime &&
                PlayedAnimations.Add(Clip.Id, Clip.Animation, Clip.StartTime, Clip.EndTime, bStreamed))
            {
                if (OnAnimationClipTriggered) OnAnimationClipTriggered(Clip);
            }
        }
        if (ActiveStaging != Staging)
        {
            return;
        }

        const int NewBlockIdx = FindBlockToPlay();
        if (NewBlockIdx != ActiveBlockIndex)
        {
            if (GetCurrentBlock())
            {
                // Its voice played to its end, or longer: the clock ran with the audio of a
                // pending block until its final version moved its end back.
                LastEndedAt = std::max(ActiveEnd(), ActiveSeenAt);
                if (OnDialogueBlockEnded) OnDialogueBlockEnded(Timeline.Dialogue[static_cast<size_t>(ActiveBlockIndex)]);
            }
            ActiveBlockIndex = NewBlockIdx;
            NextCharIndex = 0;

            if (ActiveBlockIndex >= 0)
            {
                const size_t Index = static_cast<size_t>(ActiveBlockIndex);
                if (StartedBlocks.size() <= Index)
                {
                    StartedBlocks.resize(Index + 1, false);
                }
                StartedBlocks[Index] = true;
                const auto& Block = Timeline.Dialogue[Index];
                // A block starts at its time, or when the voice before it ended: its audio, visemes
                // and text count from there.
                ActiveStartedAt = std::max(Block.StartTime, LastEndedAt);
                ActiveSeenAt = ActiveStartedAt;
                CurrentText = Block.Text;
                RevealedText.clear();
                VisemePlayer.LoadCues(Block.Visemes);
                if (OnDialogueBlockStarted) OnDialogueBlockStarted(Block);
                if (ActiveStaging != Staging)
                {
                    return;
                }
            }
            else
            {
                VisemePlayer.Clear();
                FacialMixer.ClearVisemes();
            }
        }

        if (ActiveBlockIndex >= 0)
        {
            const auto& Block = Timeline.Dialogue[static_cast<size_t>(ActiveBlockIndex)];
            if (CurrentTimeSeconds <= ActiveEnd())
            {
                ActiveSeenAt = std::max(ActiveSeenAt, CurrentTimeSeconds);
            }
            const double LocalTime = std::max(0.0, CurrentTimeSeconds - ActiveStartedAt);
            if (!Block.Characters.empty())
            {
                while (static_cast<size_t>(NextCharIndex) < Block.Characters.size())
                {
                    const auto& Cue = Block.Characters[static_cast<size_t>(NextCharIndex)];
                    if (LocalTime < Cue.StartTime) break;
                    RevealedText += Cue.Character;
                    ++NextCharIndex;
                    if (OnCharacterRevealed) OnCharacterRevealed(RevealedText, Cue);
                }
            }
            else if (!Block.Text.empty())
            {
                const double BlockDur = std::max(0.05, BlockEnd(Block) - Block.StartTime);
                const double Ratio = std::max(0.0, std::min(1.0, LocalTime / BlockDur));
                const int TargetCount = static_cast<int>( std::round(Ratio * static_cast<double>(Block.Text.size())));
                if (TargetCount > NextCharIndex)
                {
                    NextCharIndex = TargetCount;
                    RevealedText = Block.Text.substr(0, static_cast<size_t>(NextCharIndex));
                }
            }

            if (Block.Type == "speech" && !bWaiting)
            {
                FacialMixer.SetVisemeWeights(VisemePlayer.Evaluate(LocalTime, FallbackAudioRms));
            }
            else
            {
                // A pause, or the wait at the end of the ready dialogue: neutral mouth.
                FacialMixer.ClearVisemes();
            }
        }

        FacialMixer.Update(static_cast<float>(Dt));

        const bool bDialogueComplete = !bStreamed || bStreamComplete;
        // Every block plays to its end first, also a block that started after its time.
        if (bDialogueComplete && ActiveBlockIndex < 0 && CurrentTimeSeconds >= TotalDurationSeconds && AllBlocksStarted())
        {
            const FStagingResultPayload Completed = *Staging;
            RevealedText = Completed.Text;
            Stop();
            if (OnStagingCompleted) OnStagingCompleted(Completed);
        }
    }
}
