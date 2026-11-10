#pragma once

#include "Core/NpcifyProtocol.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Npcify
{
    struct FSightFrameRequest;

    /** One Engine.IO v4 text packet, with its Socket.IO v5 packet when it is a message. */
    struct FNpcifySocketIoPacket
    {
        /** 0 open, 1 close, 2 ping, 3 pong, 4 message, 6 noop; -1 invalid. */
        int EngineType = -1;
        /** 0 connect, 1 disconnect, 2 event, 3 ack, 4 connect error, 5 binary event, 6 binary ack; -1 none. */
        int SocketType = -1;
        int Attachments = 0;
        std::string Namespace = "/";
        int64_t AckId = -1;
        std::string Json;
    };

    /** Text packets of the Socket.IO protocol (v5 over Engine.IO v4 on a WebSocket). */
    class FNpcifySocketIo
    {
    public:
        static FNpcifySocketIoPacket Parse(const std::string& Text);
        /** The CONNECT packet of the main namespace, with the API key as auth when there is one. */
        static std::string Connect(const std::string& ApiToken);
        /** An EVENT packet with one JSON argument and an acknowledgement ID (-1: none). */
        static std::string Event(const std::string& Name, const std::string& JsonArgument, int64_t AckId = -1);
        /** The first string of a JSON array: the event name of an event packet. */
        static std::string ReadEventName(const std::string& Json);
        static int64_t ReadNumber(const std::string& Json, const std::string& Key, int64_t Fallback);
        static std::string ReadStringField(const std::string& Json, const std::string& Key);
        static bool ReadTrue(const std::string& Json, const std::string& Key);
        /**
         * The conversation that an exception event names: cause.data.conversationId of a refused
         * conversations:join, or an empty string when the event names none.
         */
        static std::string ReadCauseConversationId(const std::string& Json);
        /** A conversation ID without the white space around it, as the server reads it. */
        static std::string TrimId(const std::string& Id);
        static std::string JsonString(const std::string& Value);
        /** The Socket.IO URL of an API base URL (its origin; the path is not used). */
        static std::string BuildUrl(const std::string& BaseUrl);
    };

    /**
     * The protocol state of one Socket.IO connection to the conversation stream. The engine
     * moves the WebSocket messages: it passes received text and binary messages in and sends
     * what SendText gives. The session connects with the API key, joins the room of the
     * conversation with an acknowledgement, answers pings, and decodes conversations:stream.
     * Unreal's WebSockets module delivers text and binary messages from two queues, so an
     * attachment can come before or after the text of other events. Each queue keeps its order:
     * the session pairs the headers of binary events with the attachments in that order.
     */
    class FNpcifyConversationSession
    {
    public:
        explicit FNpcifyConversationSession(std::string InApiToken);

        std::function<void(const std::string&)> SendText;
        /** Sends one binary message: the attachment of a binary event (sight frames). */
        std::function<void(const std::vector<uint8_t>&)> SendBinary;
        /** A stream event of the joined conversation, decoded from NPCP (kind 2). */
        std::function<void(const std::string& ConversationId, const FNpcifyMsgPackValue& Event)> OnStreamEvent;
        /** The result of Join: true when the server acknowledged it. */
        std::function<void(const std::string& ConversationId, bool bJoined)> OnJoinResult;
        /**
         * The server refused the connection or the join (CONVERSATION_ACCESS_DENIED, a connect
         * error): a retry with the same key and conversation fails too. GetRefusedConversationId
         * names the refused conversation. A refusal that names a conversation that the session no
         * longer wants (Leave, or Join of another one, while the server looked it up) changes
         * nothing: the server disconnects, and the engine connects again for the wanted one.
         */
        std::function<void()> OnRefused;
        std::function<void(const std::string&)> OnLog;
        /** An acknowledgement of a request of SendSightFrame: its ID and the JSON array of its arguments. */
        std::function<void(int64_t AckId, const std::string& Json)> OnAck;
        /** A sight_observation event: the JSON array of the event (any conversation of the socket). */
        std::function<void(const std::string& Json)> OnSightObservation;

        /**
         * Returns false when the connection must close (close, disconnect, connect error, or
         * more than MaxUnpaired binary events or attachments that wait for their other part).
         */
        bool HandleText(const std::string& Text);
        /** Returns false when the connection must close: see HandleText. */
        bool HandleBinary(const uint8_t* Data, size_t Size);

        /** Binary event headers, or attachments, that can wait for their other part. */
        static constexpr size_t MaxUnpaired = 1024;

        /** Joins a room; before the CONNECT acknowledgement it joins after it. */
        void Join(const std::string& ConversationId);
        void Leave();
        /** Fails a join in progress, for example after a timeout or a lost connection. */
        void FailPendingJoin();
        /**
         * Sends conversations:sight (NpcifySight.cpp): the header with a placeholder, then the
         * JPEG as its attachment. Returns the acknowledgement ID, or -1 when the session is not
         * connected or did not join the conversation of the frame.
         */
        int64_t SendSightFrame(const FSightFrameRequest& Request);

        bool IsConnected() const { return bConnected; }
        bool IsRefused() const { return bRefused; }
        /** The conversation of the refusal: the one that the refusal names, else the one wanted then. */
        const std::string& GetRefusedConversationId() const { return RefusedConversationId; }
        bool IsJoined(const std::string& ConversationId) const;
        const std::string& GetConversationId() const { return ConversationId; }
        int64_t GetPingTimeoutMs() const { return PingTimeoutMs; }

    private:
        struct FBinaryEvent
        {
            std::string Name;
            int Expected = 0;
            std::vector<std::vector<uint8_t>> Parts;
        };

        void SendJoin();
        void PairAttachments();
        void Dispatch(FBinaryEvent Binary);
        void Refuse(const std::string& InRefusedConversationId);

        std::string ApiToken;
        std::string ConversationId;
        std::string JoinedConversationId;
        bool bConnected = false;
        bool bRefused = false;
        std::string RefusedConversationId;
        bool bJoinPending = false;
        int64_t JoinAckId = -1;
        int64_t NextAckId = 0;
        int64_t PingTimeoutMs = 45000;
        /** Binary events whose attachments did not all arrive, and attachments without their header. */
        std::deque<FBinaryEvent> BinaryEvents;
        std::deque<std::vector<uint8_t>> Attachments;
    };
}
