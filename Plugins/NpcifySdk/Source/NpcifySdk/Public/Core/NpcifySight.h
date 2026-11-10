#pragma once

#include "Core/NpcifyProtocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Npcify
{
    /**
     * Unavailable: the character info did not turn sight on, or the conversation is baked. Off:
     * sight can start. Starting: the source opens and the conversation joins. On: frames go.
     */
    enum class ESightState : uint8_t
    {
        Unavailable,
        Off,
        Starting,
        On,
    };

    /** The limits and defaults of sight (docs/sight.md, SIGHT_LIMITS and SIGHT_CAPTURE_SIZE). */
    struct FSightLimits
    {
        static constexpr int MaxFrameBytes = 200 * 1024;
        static constexpr int CaptureWidth = 640;
        static constexpr int CaptureHeight = 360;
        /** JPEG quality 70 (0.7 in the web SDK), then 50 once when the first JPEG is too large. */
        static constexpr int JpegQuality = 70;
        static constexpr int SmallerJpegQuality = 50;
        static constexpr int MinFrameIntervalMs = 1000;
        static constexpr int IntervalMs = 5000;
        static constexpr int CheckMs = 250;
        static constexpr float MotionThreshold = 8.0f;
        static constexpr int AckTimeoutMs = 20000;
        static constexpr int GridWidth = 32;
        static constexpr int GridHeight = 18;
        /** A pause of the checks longer than this resets the motion grid (a paused game). */
        static constexpr int TickGapMs = 1000;
    };

    /** The codes of sight: refusals of the server, codes of the connection and of the SDK. */
    namespace SightCodes
    {
        constexpr const char* Disabled = "SIGHT_DISABLED";
        constexpr const char* RateLimited = "SIGHT_RATE_LIMITED";
        constexpr const char* FrameTooLarge = "SIGHT_FRAME_TOO_LARGE";
        constexpr const char* FrameInvalid = "SIGHT_FRAME_INVALID";
        constexpr const char* ProviderFailed = "SIGHT_PROVIDER_FAILED";
        constexpr const char* InsufficientPoints = "INSUFFICIENT_POINTS";
        constexpr const char* AccessDenied = "CONVERSATION_ACCESS_DENIED";
        constexpr const char* Disconnected = "CONVERSATION_DISCONNECTED";
        constexpr const char* Timeout = "CONVERSATION_TIMEOUT";
        constexpr const char* ConversationRequired = "CONVERSATION_REQUIRED";
        constexpr const char* AlreadyStarted = "SIGHT_ALREADY_STARTED";
        constexpr const char* CameraPermissionDenied = "CAMERA_PERMISSION_DENIED";
        constexpr const char* CameraUnavailable = "CAMERA_UNAVAILABLE";
        /** The game view cannot be read: no viewport, render target or texture for it. */
        constexpr const char* SourceUnavailable = "SIGHT_SOURCE_UNAVAILABLE";
        /** The source ended: a destroyed render target or scene capture, or a closed viewport. */
        constexpr const char* SourceEnded = "SIGHT_SOURCE_ENDED";

        /** The refusals that stop sight. The SDK skips the frame for other codes. */
        bool StopsSight(const std::string& Code);
        std::string Message(const std::string& Code);
    }

    /** One JPEG frame of a source. */
    struct FSightFrame
    {
        std::vector<uint8_t> Jpeg;
        int Width = 0;
        int Height = 0;
    };

    /** The body of conversations:sight. */
    struct FSightFrameRequest
    {
        std::string ConversationId;
        /** The capture time in Unix milliseconds. */
        int64_t CapturedAt = 0;
        int Width = 0;
        int Height = 0;
        /** "game" for a render of the game, "camera" for a webcam. */
        std::string Source;
        std::shared_ptr<const std::vector<uint8_t>> Jpeg;

        /** The JSON of the body, with the Socket.IO placeholder of the JPEG attachment. */
        std::string ToJson() const;
        /** The text of the binary event packet with an acknowledgement ID: 451-<id>["conversations:sight",{...}]. */
        std::string EventPacket(int64_t AckId) const;
    };

    /**
     * The result of one frame: the acknowledgement of the server (bAnswered), or the code of the
     * connection when no acknowledgement came (CONVERSATION_TIMEOUT, CONVERSATION_DISCONNECTED).
     */
    struct FSightAck
    {
        bool bAnswered = false;
        bool bOk = false;
        std::string ObservationId;
        /** The code of a refused frame or of the connection; empty when ok. */
        std::string Error;

        static FSightAck Accepted(const std::string& ObservationId);
        static FSightAck Refused(const std::string& Code);
        static FSightAck NotAnswered(const std::string& Code);
        /** The acknowledgement of an ack packet (the JSON array of its arguments). */
        static FSightAck FromAckJson(const std::string& Json);
    };

    struct FSightPerson
    {
        /** "left", "center" or "right". */
        std::string Position;
        /** What the person does, in five words or fewer. */
        std::string Action;
        bool bFacingCamera = false;
    };

    /** sight_observation: what the vision model saw in one frame of the conversation. */
    struct FSightObservation
    {
        std::string ConversationId;
        std::string ObservationId;
        /** The capture time that the client sent, as an ISO 8601 string. */
        std::string CapturedAt;
        int People = 0;
        std::vector<FSightPerson> Persons;
        std::vector<std::string> Text;
        /** One sentence about the frame, for the character. */
        std::string Summary;
        std::string Model;
        /** The points charged to the owner; bHasCostPoints is false while the charge is pending. */
        bool bHasCostPoints = false;
        double CostPoints = 0.0;

        /** False when a field that the SDK reads is missing. */
        static bool FromValue(const FNpcifyMsgPackValue& Value, FSightObservation& Out);
        /** The observation of an event packet's JSON array ["sight_observation", {...}]. */
        static bool FromEventJson(const std::string& Json, FSightObservation& Out);
    };

    /** The size that fits inside 640 x 360 with the same aspect ratio, without an upscale. */
    void FitCaptureSize(int Width, int Height, int& OutWidth, int& OutHeight);
    /**
     * The 32 x 18 luma grid of BGRA pixels (Unreal's FColor): each cell is the mean luma of the
     * pixels in its area. RowPitch is in pixels.
     */
    std::vector<uint8_t> LumaGridOfBgra(const uint8_t* Pixels, int Width, int Height, int RowPitch);
    /** The mean absolute difference of two luma grids, 0 to 255. */
    double MeanLumaChange(const std::vector<uint8_t>& Previous, const std::vector<uint8_t>& Next);

    /** The schedule of sight. */
    struct FSightSchedule
    {
        /** The longest time between two frames; the minimum is 1000 ms. */
        int IntervalMs = FSightLimits::IntervalMs;
        /** A change of the picture sends a frame at once (one each second at most). */
        bool bMotion = true;
        /** The mean change of the 32 x 18 luma grid that counts as motion, 0 to 255. */
        float MotionThreshold = FSightLimits::MotionThreshold;
    };

    /** The pictures of sight. The engine reads them; tests use a fake. Callbacks can run on any thread. */
    class ISightSource
    {
    public:
        virtual ~ISightSource() = default;
        /** "game" or "camera". */
        virtual std::string Kind() const = 0;
        /** False after the source ended. */
        virtual bool IsLive() const = 0;
        /** Done("") when the source gives pictures, else Done(code). */
        virtual void Open(std::function<void(const std::string&)> Done) = 0;
        /** The 32 x 18 luma grid of the current picture; an empty grid while there is no picture. */
        virtual void ReadLuminance(std::function<void(std::vector<uint8_t>)> Done) = 0;
        /** The current picture as a JPEG; null while there is no picture. */
        virtual void Capture(std::function<void(std::shared_ptr<FSightFrame>)> Done) = 0;
        virtual void Stop() = 0;
    };

    /** The part of the conversation connection that sight uses. Callbacks can run on any thread. */
    class ISightConnection
    {
    public:
        virtual ~ISightConnection() = default;
        /** Done("") when the conversation joined, else Done(code). */
        virtual void Join(const std::string& ConversationId, std::function<void(const std::string&)> Done) = 0;
        virtual void SendFrame(const FSightFrameRequest& Request, std::function<void(const FSightAck&)> Done) = 0;
    };

    /**
     * Sight of the conversation (docs/sight.md), as the web SDK: a motion check every 250 ms, a
     * frame at the interval or after motion, and one frame in flight at a time. Call it on one
     * thread; the results of the source and the connection come back through Post.
     */
    class FNpcifySightInput
    {
    public:
        /**
         * @param ConversationId The current conversation, or "".
         * @param NowMs A monotonic clock in milliseconds.
         * @param UnixNowMs The time in Unix milliseconds, for capturedAt.
         * @param Post Runs a function on the thread of Tick, after the current one.
         */
        FNpcifySightInput(
            std::shared_ptr<ISightConnection> Connection,
            std::function<std::string()> ConversationId,
            std::function<int64_t()> NowMs,
            std::function<int64_t()> UnixNowMs,
            std::function<void(std::function<void()>)> Post);
        ~FNpcifySightInput();

        FNpcifySightInput(const FNpcifySightInput&) = delete;
        FNpcifySightInput& operator=(const FNpcifySightInput&) = delete;

        /** The state changed; ErrorCode is the failure that stopped sight or its start, else "". */
        std::function<void(ESightState, const std::string& ErrorCode)> OnStateChanged;
        /** An error stopped sight after it started. */
        std::function<void(const std::string& Code)> OnStopped;
        std::function<void(const FSightObservation&)> OnObservation;

        ESightState GetState() const { return State; }
        /** True after the server refused a frame with SIGHT_DISABLED, until Configure. */
        bool IsDisabledByServer() const { return bDisabledByServer; }
        /** Why sight cannot start; "" when it can. */
        const std::string& GetUnavailableReason() const { return Unavailable; }
        bool IsHidden() const { return bHidden; }
        /** True while the game is paused or in the background: no frames, no motion against an old picture. */
        void SetHidden(bool bInHidden);

        /** The character info was read. Sight that becomes unavailable stops. */
        void Configure(bool bEnabled, bool bBaked);
        /**
         * Opens the source and joins the conversation. Returns "" when sight starts, else the code
         * of the refusal. Done("") runs when frames can go, else Done(code) after a failed start.
         */
        std::string Start(const FSightSchedule& Schedule, std::shared_ptr<ISightSource> Source, std::function<void(const std::string&)> Done);
        /** Stops the frames and releases the source. */
        void Stop();
        /** Call it every frame: it runs a check when 250 ms passed since the last one. */
        void Tick();
        /** An observation of the current conversation (the caller checked the conversation). */
        void HandleObservation(const FSightObservation& Observation);

    private:
        struct FSession;
        using FSessionPtr = std::shared_ptr<FSession>;

        // The session goes by value: a call can end the current session.
        void Opened(FSessionPtr Session, std::shared_ptr<ISightSource> Source, const std::string& Failure, const std::function<void(const std::string&)>& Done);
        void Check(FSessionPtr Session);
        void GridRead(FSessionPtr Session, std::vector<uint8_t> Grid);
        void SendWhenDue(FSessionPtr Session);
        void Send(FSessionPtr Session);
        void Captured(FSessionPtr Session, int64_t CapturedAt, std::shared_ptr<FSightFrame> Frame);
        void Rejoined(FSessionPtr Session, const FSightFrameRequest& Request, const std::string& Code);
        void Emit(FSessionPtr Session, const FSightFrameRequest& Request);
        void Acknowledged(FSessionPtr Session, const FSightAck& Ack);
        void Failed(FSessionPtr Session, const std::string& Code);
        void Finish(FSessionPtr Session);
        void Fail(FSessionPtr Session, const std::string& Code);
        void End(FSessionPtr Session);
        void Publish(const std::string& ErrorCode);
        /** A function that runs Work on the thread of Tick while this input exists. */
        std::function<void(std::function<void()>)> Later();

        std::shared_ptr<ISightConnection> Connection;
        std::function<std::string()> ConversationId;
        std::function<int64_t()> NowMs;
        std::function<int64_t()> UnixNowMs;
        std::function<void(std::function<void()>)> Post;
        std::shared_ptr<bool> Alive;
        FSessionPtr Current;
        std::string Unavailable;
        ESightState State = ESightState::Unavailable;
        bool bDisabledByServer = false;
        bool bHidden = false;
        bool bTicked = false;
        int64_t LastTick = 0;
    };
}
