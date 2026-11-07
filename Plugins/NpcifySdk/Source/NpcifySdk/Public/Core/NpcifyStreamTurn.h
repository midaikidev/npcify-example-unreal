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
        bool IsFailed() const { return bFailed; }
        /** The turn plays no more: stopped, replaced by a newer reply, or failed. */
        bool IsStopped() const { return bStopped; }
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
         * Applies the HTTP result. Returns false when no revision arrived: the result then plays
         * as a whole. Speech whose stream did not complete takes the stored audio of the result.
         */
        bool ApplyResult(const FStagingResultPayload& Result);

        /** The stored audio of a block (empty: the download failed and the block plays without audio). */
        bool SetBlockAudio(const std::string& BlockId, const std::vector<uint8_t>& Bytes);

        /** Blocks whose stored audio must be downloaded, as (block ID, URL); each one is returned once. */
        std::vector<std::pair<std::string, std::string>> TakeAudioDownloads();

        /** The number of leading dialogue blocks that can play. */
        size_t CountReadyBlocks() const;

        void MarkStarted() { bPlaybackStarted = true; }
        void Stop()
        {
            bStopped = true;
            bEndPending = false;
        }
        void Fail()
        {
            bFailed = true;
            Stop();
        }

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
        bool bHasTimeline = false;
        bool bComplete = false;
        bool bHasFinal = false;
        bool bFailed = false;
        bool bStopped = false;
        bool bPlaybackStarted = false;
    };
}
