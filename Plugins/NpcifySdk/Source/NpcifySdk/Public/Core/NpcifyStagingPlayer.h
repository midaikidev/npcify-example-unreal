#pragma once

#include "Core/NpcifyFacialMixer.h"
#include "Core/NpcifyModels.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Npcify
{
    /**
     * The expression or animation clips of a staging that played. A clip is known by its ID, or
     * by its name and start time. In a streamed staging, a revision can move a clip that played
     * (the beats of a pending speech block take new times when more of the block is aligned):
     * there, a clip with the same name whose time overlaps a clip that played is that clip.
     */
    class FNpcifyPlayedClips
    {
    public:
        void Clear();
        /** Records the clip; true when it did not play yet. bMatchMoved: a moved clip counts as played. */
        bool Add(const std::string& Id, const std::string& Name, double StartTime, double EndTime, bool bMatchMoved);

    private:
        struct FSpan
        {
            std::string Name;
            double Start = 0.0;
            double End = 0.0;
        };

        std::set<std::string> Keys;
        std::vector<FSpan> Spans;
    };

    class FNpcifyStagingPlayer
    {
    public:
        FNpcifyStagingPlayer(FNpcifyFacialMixer& InFacialMixer, FNpcifyVisemePlayer& InVisemePlayer);

        std::function<void(const FStagingDialogueBlock&)> OnDialogueBlockStarted;
        std::function<void(const FStagingDialogueBlock&)> OnDialogueBlockEnded;
        std::function<void(const std::string&, const FStagingCharacterCue&)> OnCharacterRevealed;
        std::function<void(const FStagingExpressionClip&)> OnExpressionClipTriggered;
        std::function<void(const FStagingAnimationClip&)> OnAnimationClipTriggered;
        std::function<void(const FNpcifyStagingToolCall&)> OnToolCallTriggered;
        std::function<void(const std::string&, const FNpcifyStagingToolCall*)> OnVariantCue;
        std::function<void(const FStagingResultPayload&)> OnStagingCompleted;

        bool IsPlaying() const { return bIsPlaying; }
        /**
         * A streamed staging waits for its next dialogue block (docs/streaming_reply.md). The
         * staging clock stops meanwhile; the playback stays on.
         */
        bool IsWaitingForBlock() const { return bIsPlaying && bWaiting; }
        double GetTimeSeconds() const { return CurrentTimeSeconds; }
        double GetDurationSeconds() const { return TotalDurationSeconds; }
        const FStagingResultPayload* GetCurrentStaging() const { return ActiveStaging.get(); }
        const FStagingDialogueBlock* GetCurrentBlock() const;
        const std::string& GetCurrentText() const { return CurrentText; }
        const std::string& GetRevealedText() const { return RevealedText; }
        FNpcifyPlaybackCursor GetCursor() const;

        void Play(const FStagingResultPayload& Staging);
        /**
         * Plays a staging whose dialogue still grows (a streamed reply). The clock stops at the
         * first of the dialogue blocks that cannot play yet (ReadyBlocks: the leading blocks that
         * can), and at the end of the dialogue until bComplete. A pending block with a known audio
         * length (SetBlockAudioLength) plays to the end of its audio; without one, the clock
         * waits at its estimated end until its final version. The staging object stays shared: a
         * revision changes it, then UpdateStream.
         */
        void PlayStream(std::shared_ptr<const FStagingResultPayload> Staging, size_t ReadyBlocks, bool bComplete);
        /** The streamed staging that plays has more ready blocks, or its complete dialogue. */
        void UpdateStream(size_t ReadyBlocks, bool bComplete);
        /** The length in seconds of the decoded audio of a block of the staging that plays. */
        void SetBlockAudioLength(const std::string& BlockId, double Seconds);
        void Stop();
        void Tick(double DeltaTimeSeconds, float FallbackAudioRms = 0.0f);

    private:
        void Start(std::shared_ptr<const FStagingResultPayload> Staging, bool bInStreamed, size_t InReadyBlocks, bool bComplete);
        void RecomputeDuration();
        void DispatchDueToolCalls(const FStagingTimeline& Timeline, double ElapsedSeconds);
        double AvailableEnd() const;
        /** The end of a block on its timeline: a pending block plays to the end of its audio. */
        double BlockEnd(const FStagingDialogueBlock& Block) const;
        /** The clock time at which the active block ends: it counts from the time that it started. */
        double ActiveEnd() const;
        bool HasStarted(size_t Index) const;
        bool AllBlocksStarted() const;
        /**
         * The block that plays at the current time; -1 for none. A block never starts again; a
         * block that starts after its time plays from its own start.
         */
        int FindBlockToPlay() const;

        FNpcifyFacialMixer& FacialMixer;
        FNpcifyVisemePlayer& VisemePlayer;
        FNpcifyPlayedClips PlayedExpressions;
        FNpcifyPlayedClips PlayedAnimations;
        std::set<std::string> AppliedToolCallIds;

        std::shared_ptr<const FStagingResultPayload> ActiveStaging;
        bool bStreamed = false;
        bool bStreamComplete = true;
        size_t ReadyBlocks = 0;
        bool bWaiting = false;
        int ActiveBlockIndex = -1;
        /** The blocks that started, by index. */
        std::vector<bool> StartedBlocks;
        /** The clock time at which the active block started: its local time counts from there. */
        double ActiveStartedAt = 0.0;
        /** The last clock time at which the active block played within its end. */
        double ActiveSeenAt = 0.0;
        /** The clock time at which the voice of the last block that played ended. */
        double LastEndedAt = 0.0;
        /** The lengths in seconds of the decoded audio of blocks, by block ID. */
        std::map<std::string, double> AudioLengths;
        int NextCharIndex = 0;
        double CurrentTimeSeconds = 0.0;
        double TotalDurationSeconds = 0.0;
        bool bIsPlaying = false;
        std::string CurrentText;
        std::string RevealedText;
    };
}
