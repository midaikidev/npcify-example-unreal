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
        Out.TimeSeconds = CurrentTimeSeconds;
        Out.DurationSeconds = TotalDurationSeconds;
        const FStagingDialogueBlock* Block = GetCurrentBlock();
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
        for (const auto& B : Timeline.Dialogue) ComputedEnd = std::max(ComputedEnd, B.EndTime);
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
        if (ReadyBlocks < Dialogue.size())
        {
            return Dialogue[ReadyBlocks].StartTime;
        }
        if (bStreamComplete)
        {
            return std::numeric_limits<double>::infinity();
        }
        return Dialogue.empty() ? 0.0 : BlockEnd(Dialogue.back());
    }

    void FNpcifyStagingPlayer::SetBlockAudioLength(const std::string& BlockId, double Seconds)
    {
        if (!BlockId.empty() && Seconds > 0.0 && !std::isinf(Seconds))
        {
            AudioLengths[BlockId] = Seconds;
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

    int FNpcifyStagingPlayer::FindBlockToPlay() const
    {
        const std::vector<FStagingDialogueBlock>& Dialogue = ActiveStaging->Timeline.Dialogue;
        if (ActiveBlockIndex >= 0 && static_cast<size_t>(ActiveBlockIndex) < Dialogue.size() &&
            CurrentTimeSeconds <= BlockEnd(Dialogue[static_cast<size_t>(ActiveBlockIndex)]))
        {
            return ActiveBlockIndex;
        }
        // Only blocks that can play, from the first one that did not start: the end of a block
        // can move back when its final version comes, and a block must not start again.
        const size_t Playable = bStreamed ? std::min(ReadyBlocks, Dialogue.size()) : Dialogue.size();
        for (size_t Index = NextBlockIndex; Index < Playable; ++Index)
        {
            const FStagingDialogueBlock& Block = Dialogue[Index];
            if (CurrentTimeSeconds >= Block.StartTime && CurrentTimeSeconds <= BlockEnd(Block))
            {
                return static_cast<int>(Index);
            }
        }
        return -1;
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
        NextBlockIndex = 0;
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
            if (ActiveBlockIndex >= 0 && static_cast<size_t>(ActiveBlockIndex) < Timeline.Dialogue.size())
            {
                if (OnDialogueBlockEnded) OnDialogueBlockEnded(Timeline.Dialogue[static_cast<size_t>(ActiveBlockIndex)]);
            }
            ActiveBlockIndex = NewBlockIdx;
            NextCharIndex = 0;

            if (ActiveBlockIndex >= 0)
            {
                NextBlockIndex = static_cast<size_t>(ActiveBlockIndex) + 1;
                const auto& Block = Timeline.Dialogue[static_cast<size_t>(ActiveBlockIndex)];
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
            const double LocalTime = std::max(0.0, CurrentTimeSeconds - Block.StartTime);
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
        if (bDialogueComplete && CurrentTimeSeconds >= TotalDurationSeconds)
        {
            const FStagingResultPayload Completed = *Staging;
            RevealedText = Completed.Text;
            Stop();
            if (OnStagingCompleted) OnStagingCompleted(Completed);
        }
    }
}
