#pragma once

#include "Core/NpcifyModels.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Npcify
{
    /** What a stream event changed in a FNpcifyStreamTurn. */
    enum class ENpcifyStreamChange : uint8_t
    {
        None,
        Timeline,
        Audio,
        Final,
        Failed,
        /** A second start event: the server runs the turn again (a retried request). */
        Restarted,
    };

    /**
     * Why a turn plays no more. Game and Superseded are final; after Rejected or Failed, a rerun
     * of the turn (a retried request) can start it again.
     */
    enum class ENpcifyTurnStop : uint8_t
    {
        None,
        /** The game stopped the speech (StopSpeaking, a new conversation). */
        Game,
        /** A newer reply plays, or the HTTP result plays as a whole. */
        Superseded,
        /** The SDK refused a revision that changed published dialogue. */
        Rejected,
        /** The server sent an error event, or the request failed. */
        Failed,
    };

    /**
     * One reply that plays while the server makes it (docs/streaming_reply.md): the numbered
     * timeline revisions of conversations:stream, the MP3 chunks of each speech block, the reply
     * options, and the reconciliation with the HTTP result. A revision only adds dialogue, and
     * only the last (pending) speech block can grow; a revision that changes published dialogue
     * fails the turn and throws std::runtime_error.
     */
    class FNpcifyStreamTurn
    {
    public:
        static constexpr const char* ChangedDialogueMessage = "A staging revision changed published dialogue.";

        /** TaskId is the Idempotency-Key of the turn request; the server uses it as the task ID. */
        explicit FNpcifyStreamTurn(const std::string& TaskId);

        const std::string& GetTaskId() const { return TaskId; }
        /** Later turns have higher numbers. */
        uint64_t GetSequence() const { return Sequence; }
        /** The staging that plays: revisions add to it, and the HTTP result completes it. */
        const std::shared_ptr<FStagingResultPayload>& GetPayload() const { return Payload; }

        /** A timeline revision arrived: the stream plays this reply, not the HTTP result. */
        bool HasTimeline() const { return bHasTimeline; }
        /** The dialogue is complete (the staging event or the HTTP result). */
        bool IsComplete() const { return bComplete; }
        /** The options and the text are known (the options event or the HTTP result). */
        bool HasFinal() const { return bHasFinal; }
        ENpcifyTurnStop GetStopReason() const { return StopReason; }
        bool IsFailed() const { return StopReason == ENpcifyTurnStop::Failed || StopReason == ENpcifyTurnStop::Rejected; }
        /** The turn plays no more: stopped, replaced by a newer reply, refused or failed. */
        bool IsStopped() const { return StopReason != ENpcifyTurnStop::None; }
        bool IsPlaybackStarted() const { return bPlaybackStarted; }

        /** The playback ended before the options were known; OnSpeechEnd waits for them. */
        bool bEndPending = false;
        /** The HTTP request of the turn finished (result or failure). */
        bool bRequestClosed = false;
        /** Milestones already in the notes of the stream: 1 the first revision, 2 the first audio. */
        int Noted = 0;

        /** Seconds since the turn request, for the notes of the stream. */
        double GetElapsedSeconds() const
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - Started).count();
        }

        ENpcifyStreamChange Apply(const FNpcifyMsgPackValue& Event);

        /**
         * Applies the HTTP result. Returns false when the result plays as a whole: no revision
         * arrived, or the server failed the stream (a rerun whose events did not arrive). A turn
         * that the game stopped or that a newer reply replaced does not play again: it takes the
         * complete staging of the result. Speech whose stream did not complete takes the stored
         * audio of the result. Throws std::runtime_error when the result changes published
         * dialogue, or when the SDK refused a revision of this turn.
         */
        bool ApplyResult(const FStagingResultPayload& Result);

        /** The stored audio of a block (empty: the download failed and the block plays without audio). */
        bool SetBlockAudio(const std::string& BlockId, const std::vector<uint8_t>& Bytes);

        /** Blocks whose stored audio must be downloaded, as (block ID, URL); each one is returned once. */
        std::vector<std::pair<std::string, std::string>> TakeAudioDownloads();

        /** The number of leading dialogue blocks that can play. */
        size_t CountReadyBlocks() const;
        /** The dialogue is complete and every block can play: the stream delivered the whole reply. */
        bool AreAllBlocksReady() const { return bComplete && CountReadyBlocks() == Payload->Timeline.Dialogue.size(); }

        void MarkStarted() { bPlaybackStarted = true; }
        void Stop(ENpcifyTurnStop Reason);
        /**
         * The server runs the turn again (a retried request): the turn forgets the first run and
         * applies the new run from its start event, in a new staging object.
         */
        void Restart(const FNpcifyMsgPackValue& StartEvent);

        /** A pause, silent speech, or speech whose audio bytes are complete. */
        static bool HasPlayableAudio(const FStagingDialogueBlock& Block);
        /** The server lowercases the Idempotency-Key that it uses as the task ID. */
        static std::string NormalizeTaskId(std::string TaskId);

    private:
        struct FAudioStream
        {
            std::vector<uint8_t> Bytes;
            int64_t NextIndex = 0;
            bool bComplete = false;
            bool bBroken = false;
        };

        bool IsBlockReady(const FStagingDialogueBlock& Block) const;
        void AdoptStagingId(const FNpcifyMsgPackValue& Event);
        void AppendChunk(const FNpcifyMsgPackValue& Event);
        ENpcifyStreamChange CompleteAudio(const std::string& StreamId);
        void BreakAudio(const std::string& StreamId);
        bool AttachAudio(FStagingDialogueBlock& Block) const;
        ENpcifyStreamChange ApplyRevision(const FNpcifyMsgPackValue& Event, bool bCompleteRevision);
        ENpcifyStreamChange ApplyOptions(const FNpcifyMsgPackValue& Event);
        void ApplyTracks(const FStagingTimeline& Timeline);
        void Merge(FStagingTimeline&& Next);

        std::string TaskId;
        uint64_t Sequence = 0;
        std::chrono::steady_clock::time_point Started = std::chrono::steady_clock::now();
        std::shared_ptr<FStagingResultPayload> Payload;
        std::map<std::string, FAudioStream> Audio;
        /** Blocks whose stored audio was requested, and blocks that play without audio. */
        std::set<std::string> Requested;
        std::set<std::string> Silent;
        int64_t Revision = 0;
        ENpcifyTurnStop StopReason = ENpcifyTurnStop::None;
        bool bHasTimeline = false;
        bool bComplete = false;
        bool bHasFinal = false;
        bool bPlaybackStarted = false;
        /** A start event arrived: a later one means that the server runs the turn again. */
        bool bRunStarted = false;
    };
}
