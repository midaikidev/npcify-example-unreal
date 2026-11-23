#pragma once

#include "Core/NpcifyBakedConversation.h"
#include "Core/NpcifyFacialMixer.h"
#include "Core/NpcifyLiveMotion.h"
#include "Core/NpcifyModels.h"
#include "Core/NpcifyStagingPlayer.h"
#include "Core/NpcifyStreamTurn.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Npcify
{
    struct FNpcifyDecodedAudio
    {
        std::vector<float> Samples;
        std::vector<int16_t> Pcm16Samples;
        int SampleRate = 24000;
        int Channels = 1;

        double GetDurationSeconds() const
        {
            return (SampleRate > 0 && Channels > 0)
                ? static_cast<double>(Samples.size()) / static_cast<double>(SampleRate * Channels)
                : 0.0;
        }
    };

    class FNpcifyCharacterRuntime
    {
    public:
        explicit FNpcifyCharacterRuntime(const FNpcifyOptions& InOptions);

        std::function<void(const FStagingResultPayload&)> OnSpeechStart;
        std::function<void(const FStagingResultPayload&)> OnSpeechEnd;
        /**
         * The speech that played stopped before its end: StopSpeaking, a new conversation, a
         * staging or a newer reply that replaces it, a failed turn, or a turn that the server runs
         * again. Stop the audio of the current block here.
         */
        std::function<void()> OnSpeechStopped;
        std::function<void(const FStagingDialogueBlock&, const FNpcifyDecodedAudio&)> OnAudioBlock;
        std::function<void(const std::string&, const FStagingCharacterCue&)> OnCharacterRevealed;
        std::function<void(const std::string&, bool)> OnAnimationPlayRequested;
        std::function<void(const std::string&)> OnVariantChanged;
        std::function<void(const FNpcifyStagingToolCall&)> OnToolCall;
        std::function<void(const std::string&)> OnError;
        /**
         * A staging to play has stored live motion frames at a URL (timeline.motion, a replay):
         * the engine downloads them and calls GetLiveMotion().Load. Until then the staging plays idle.
         */
        std::function<void(const std::string& StagingId, const FStagingMotionTrack& Track)> OnMotionTrackRequested;

        const FNpcifyOptions& GetOptions() const { return Options; }
        /** The live motion frames of the stagings and the pose of the staging clock (docs/realtime-motion.md). */
        FNpcifyLiveMotionPlayer& GetLiveMotion() { return LiveMotion; }
        const FNpcifyLiveMotionPlayer& GetLiveMotion() const { return LiveMotion; }
        FNpcifyFacialMixer& GetExpressions() { return Expressions; }
        const FNpcifyFacialMixer& GetExpressions() const { return Expressions; }
        FNpcifyVisemePlayer& GetVisemePlayer() { return VisemePlayer; }
        FNpcifyStagingPlayer& GetStagingPlayer() { return StagingPlayer; }

        const std::string& GetConversationId() const { return ConversationId; }
        void SetConversationId(const std::string& InId) { ConversationId = InId; }
        const std::string& GetActiveStagingId() const { return ActiveStagingId; }
        void SetActiveStagingId(const std::string& InId) { ActiveStagingId = InId; }
        const std::string& GetMemoryIdentifier() const { return Options.MemoryIdentifier; }
        void SetMemoryIdentifier(const std::string& InMemoryIdentifier);
        const std::string& GetCurrentAnimation() const { return CurrentAnimation; }
        const std::string& GetActiveVariant() const { return ActiveVariant; }
        void SwitchVariant(const std::string& VariantKey);
        const std::vector<FNpcifyVariantSummary>& GetVariants() const { return Variants; }
        void SetVariants(std::vector<FNpcifyVariantSummary> InVariants);
        FConversationWorldline GetBakedWorldline() const;

        float GetVolume() const { return Volume; }
        void SetVolume(float InVolume);
        bool IsMuted() const { return bMuted; }
        void SetMuted(bool bInMuted) { bMuted = bInMuted; }
        bool IsThinking() const { return bThinking; }
        void SetThinking(bool bInThinking) { bThinking = bInThinking; }

        FNpcifySpeechState GetSpeechState() const;

        bool HasBakedConversation() const { return BakedConversation != nullptr; }
        void AttachBakedConversation(std::unique_ptr<FNpcifyBakedConversation> InBaked);
        FStagingResultPayload StartBakedConversation(const std::string& StagingId = "");
        FStagingResultPayload SelectBakedStaging(const std::string& StagingId);
        FBakedReplyResult ReplyBaked(const std::string& OptionLabel, const std::string& ParentStagingId = "");

        void PlayStaging(const FStagingResultPayload& Staging);
        /** Forgets the conversation IDs and the reply options of the last staging. */
        void ResetConversationState();
        /** Stops the speech that plays; a streamed reply that plays does not continue. */
        void StopSpeaking();

        /**
         * Replies that play while they stream (docs/streaming_reply.md), in
         * NpcifyCharacterRuntimeStream.cpp. Call BeginStreamTurn with the Idempotency-Key of a
         * turn request when the conversation stream has joined, pass the turn events of
         * conversations:stream to HandleStreamEvent, then the HTTP result to HandleTurnResult or
         * the failure to HandleTurnFailure.
         */
        /**
         * A turn request starts: the character thinks until the reply of its newest request plays,
         * or that request fails. Returns the order of the request, for BeginStreamTurn,
         * EndTurnRequest and PlayTurnStaging.
         */
        uint64_t BeginTurnRequest();
        /** A turn request failed: thinking ends when no later request started. */
        void EndTurnRequest(uint64_t Sequence);
        /** Sequence: the order from BeginTurnRequest (0: a new order). */
        void BeginStreamTurn(const std::string& TaskId, uint64_t Sequence = 0);
        /** The turn request is sent again: the server can run the turn again under the same task ID. */
        void NoteTurnRetry(const std::string& TaskId);
        void HandleStreamEvent(const FNpcifyMsgPackValue& Event);
        /**
         * True when the stream plays this reply, or when it must not play (the game stopped it, or
         * a newer reply plays): then do not play Result. False: play Result as a whole with
         * PlayTurnStaging and the sequence that OutPlaySequence receives.
         */
        bool HandleTurnResult(const std::string& TaskId, const FStagingResultPayload& Result, uint64_t* OutPlaySequence = nullptr);
        /**
         * Plays the HTTP result of a turn as a whole, unless a newer reply started after
         * HandleTurnResult (for example while its audio downloaded). Sequence 0 always plays.
         */
        bool PlayTurnStaging(const FStagingResultPayload& Staging, uint64_t Sequence);
        /**
         * The HTTP request of a turn failed. True when the stream delivered the whole reply and its
         * options: the reply plays on, and the failure is no error of the turn.
         */
        bool HandleTurnFailure(const std::string& TaskId);
        /** The stored audio of a block that OnBlockAudioRequested asked for; empty after a failed download. */
        void SetBlockAudio(const std::string& TaskId, const std::string& BlockId, const std::vector<uint8_t>& Bytes);
        /**
         * The engine decoded the audio of a dialogue block that starts (OnAudioBlock): its length in
         * seconds. A pending block of a streamed reply plays to the end of its audio while later
         * revisions add its alignment; without a length, its clock waits at its estimated end until
         * its final version. UNpcifyCharacterComponent calls it; the runtime does for PCM audio.
         */
        void SetBlockAudioLength(const std::string& BlockId, double Seconds);
        /** A block of a streamed reply needs its stored audio: the engine downloads it, then SetBlockAudio. */
        std::function<void(const std::string& TaskId, const std::string& BlockId, const std::string& Url)> OnBlockAudioRequested;
        /** True for the turn events of conversations:stream (not motion_frames). */
        static bool IsTurnEvent(const FNpcifyMsgPackValue& Event);
        /** Notes of streamed turns with the time since their request, for logs. */
        std::function<void(const std::string&)> OnStreamNote;
        void PlayAnimation(const std::string& AnimationId, bool bLoop = false);

        /**
         * Tells the face that the body starts the animation AnimationId: people blink when they
         * start another movement. 9 in 10 new animations and 6 in 10 returns to the idle animation
         * bring a blink (FNpcifyIdleBlinks). PlayAnimation calls it; call it when the game starts
         * an animation by itself, for example the return to idle at the end of an animation that
         * plays once.
         */
        void NoteAnimationStarted(const std::string& AnimationId);
        /** The character's idle animation (its info); "idle" until the info is read. */
        const std::string& GetIdleAnimationId() const { return IdleAnimationId; }
        void SetIdleAnimationId(const std::string& AnimationId);

        void LookAt(float YawDegrees, float PitchDegrees);
        void LookAtDirection(float LocalX, float LocalY, float LocalZ);
        void ClearLookAt();

        void Tick(double DeltaTimeSeconds, float FallbackAudioRms = 0.0f);

        /** True for WAV or raw PCM bytes. Compressed audio (MPEG) is decoded by the engine integration. */
        static bool IsPcmAudio(const FStagingAudioSource& Audio);

        static FNpcifyDecodedAudio DecodeAudioBytes(
            const std::vector<uint8_t>& Bytes,
            int DefaultSampleRate = 24000,
            int DefaultChannels = 1
        );

        static FNpcifyMsgPackValue BuildCreateConversationRequest(
            const std::string& AvatarId,
            const std::string& RenderSessionId = "",
            const std::string& MemoryIdentifier = ""
        );

        static FNpcifyMsgPackValue BuildCreateStagingRequest(
            const std::string& Message,
            const std::string& ParentStagingId = "",
            bool bSilent = false,
            int MaxOptions = 6,
            const std::string& MemoryIdentifier = ""
        );

    private:
        void HandleDialogueBlockStarted(const FStagingDialogueBlock& Block);
        void ApplyVariantCue(const std::string& VariantKey);
        /** bEndsThinking: false for the result of an older request while a newer one is in flight. */
        void PlayWholeStaging(const FStagingResultPayload& Staging, bool bEndsThinking);
        /** Thinking belongs to the newest request: an older reply that plays or fails does not end it. */
        bool IsNewestRequest(uint64_t Sequence) const { return Sequence >= NewestRequest; }
        void HandleStagingCompleted(const FStagingResultPayload& Staging);
        std::shared_ptr<FNpcifyStreamTurn> FindTurn(const std::string& TaskId) const;
        /** What a stop leaves to report once the state of the runtime is consistent. */
        struct FStoppedSpeech
        {
            /** The staging player stopped speech that played: the engine stops its audio. */
            bool bStopped = false;
            /** A reply that played to its end before its options came: its OnSpeechEnd. */
            std::shared_ptr<FStagingResultPayload> Ended;
        };

        ENpcifyStreamChange ApplyTurnEvent(const std::shared_ptr<FNpcifyStreamTurn>& Turn, const FNpcifyMsgPackValue& Event);
        void RestartTurn(const std::shared_ptr<FNpcifyStreamTurn>& Turn, const FNpcifyMsgPackValue& FirstEvent);
        void PumpTurn(const std::shared_ptr<FNpcifyStreamTurn>& Turn);
        void StartStreamPlayback(const std::shared_ptr<FNpcifyStreamTurn>& Turn);
        void SupersedeOlderTurns(uint64_t Sequence);
        FStoppedSpeech StopPlayingTurn(ENpcifyTurnStop Reason);
        bool StopPlayback();
        void Report(const FStoppedSpeech& Stop);
        void EndTurn(const std::shared_ptr<FNpcifyStreamTurn>& Turn, ENpcifyTurnStop Reason);
        void ForgetClosedTurns();
        void SettleThinking(const FNpcifyStreamTurn& Turn);
        void NoteTurn(const FNpcifyStreamTurn& Turn, const std::string& Message) const;
        void NoteMilestones(FNpcifyStreamTurn& Turn) const;
        void StopStreamTurns();

        FNpcifyOptions Options;
        FNpcifyFacialMixer Expressions;
        FNpcifyVisemePlayer VisemePlayer;
        FNpcifyStagingPlayer StagingPlayer;
        FNpcifyLiveMotionPlayer LiveMotion;
        std::unique_ptr<FNpcifyBakedConversation> BakedConversation;
        /** Turns in progress, oldest first: an open request, or a reply that waits for its first block. */
        std::vector<std::shared_ptr<FNpcifyStreamTurn>> StreamTurns;
        std::shared_ptr<FNpcifyStreamTurn> PlayingTurn;
        /** The request order of the newest reply that started to play: an older reply never plays over it. */
        uint64_t PlayedSequence = 0;
        /** Counts the calls that start or stop speech: a handler of a stop can make one. */
        uint64_t PlayGeneration = 0;
        /** The order of the newest turn request. */
        uint64_t NewestRequest = 0;

        std::string ConversationId;
        std::string ActiveStagingId;
        std::string CurrentAnimation = "idle";
        std::string IdleAnimationId = "idle";
        /** Live motion showed in the last tick: its start brings a blink. */
        bool bLiveMotionShown = false;
        std::string ActiveVariant = FNpcifyProtocol::DefaultVariantKey;
        std::vector<FNpcifyVariantSummary> Variants;
        /** Options of the last played staging; they stay available after its speech ends. */
        std::vector<FStagingReplyOption> LastOptions;
        float Volume = 1.0f;
        bool bMuted = false;
        bool bThinking = false;
    };
}
