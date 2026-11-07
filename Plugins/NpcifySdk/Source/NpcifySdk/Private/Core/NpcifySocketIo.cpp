#include "Core/NpcifySocketIo.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <utility>

namespace Npcify
{
    namespace
    {
        bool IsDigit(char C)
        {
            return C >= '0' && C <= '9';
        }

        size_t SkipSpace(const std::string& Json, size_t Index)
        {
            while (Index < Json.size() && std::isspace(static_cast<unsigned char>(Json[Index])))
            {
                ++Index;
            }
            return Index;
        }

        void AppendUtf8(std::string& Out, unsigned Code)
        {
            if (Code < 0x80)
            {
                Out.push_back(static_cast<char>(Code));
            }
            else if (Code < 0x800)
            {
                Out.push_back(static_cast<char>(0xC0 | (Code >> 6)));
                Out.push_back(static_cast<char>(0x80 | (Code & 0x3F)));
            }
            else
            {
                Out.push_back(static_cast<char>(0xE0 | (Code >> 12)));
                Out.push_back(static_cast<char>(0x80 | ((Code >> 6) & 0x3F)));
                Out.push_back(static_cast<char>(0x80 | (Code & 0x3F)));
            }
        }

        /** Reads a JSON string at Index (on its opening quote); false when it is not complete. */
        bool ReadString(const std::string& Json, size_t& Index, std::string& Out)
        {
            Out.clear();
            ++Index;
            while (Index < Json.size())
            {
                const char C = Json[Index++];
                if (C == '"')
                {
                    return true;
                }
                if (C != '\\' || Index >= Json.size())
                {
                    Out.push_back(C);
                    continue;
                }
                const char Escape = Json[Index++];
                switch (Escape)
                {
                case 'n': Out.push_back('\n'); break;
                case 'r': Out.push_back('\r'); break;
                case 't': Out.push_back('\t'); break;
                case 'b': Out.push_back('\b'); break;
                case 'f': Out.push_back('\f'); break;
                case 'u':
                    if (Index + 4 <= Json.size())
                    {
                        AppendUtf8(Out, static_cast<unsigned>(std::strtoul(Json.substr(Index, 4).c_str(), nullptr, 16)));
                        Index += 4;
                    }
                    break;
                default: Out.push_back(Escape); break;
                }
            }
            return false;
        }

        /** The index of a field's value in a JSON object, or npos. */
        size_t FindValue(const std::string& Json, const std::string& Key)
        {
            const std::string Token = "\"" + Key + "\"";
            size_t Index = Json.find(Token);
            if (Index == std::string::npos)
            {
                return std::string::npos;
            }
            Index = SkipSpace(Json, Index + Token.size());
            if (Index >= Json.size() || Json[Index] != ':')
            {
                return std::string::npos;
            }
            return SkipSpace(Json, Index + 1);
        }
    }

    FNpcifySocketIoPacket FNpcifySocketIo::Parse(const std::string& Text)
    {
        FNpcifySocketIoPacket Packet;
        if (Text.empty() || Text[0] < '0' || Text[0] > '6')
        {
            return Packet;
        }
        Packet.EngineType = Text[0] - '0';
        if (Packet.EngineType != 4)
        {
            Packet.Json = Text.substr(1);
            return Packet;
        }
        if (Text.size() < 2 || Text[1] < '0' || Text[1] > '6')
        {
            Packet.EngineType = -1;
            return Packet;
        }
        Packet.SocketType = Text[1] - '0';
        size_t Index = 2;
        if (Packet.SocketType == 5 || Packet.SocketType == 6)
        {
            const size_t Start = Index;
            while (Index < Text.size() && IsDigit(Text[Index]) && Index - Start < 6)
            {
                ++Index;
            }
            if (Index == Start || Index >= Text.size() || Text[Index] != '-')
            {
                Packet.EngineType = -1;
                return Packet;
            }
            Packet.Attachments = std::atoi(Text.substr(Start, Index - Start).c_str());
            ++Index;
        }
        if (Index < Text.size() && Text[Index] == '/')
        {
            const size_t Comma = Text.find(',', Index);
            if (Comma == std::string::npos)
            {
                Packet.Namespace = Text.substr(Index);
                return Packet;
            }
            Packet.Namespace = Text.substr(Index, Comma - Index);
            Index = Comma + 1;
        }
        const size_t IdStart = Index;
        while (Index < Text.size() && IsDigit(Text[Index]) && Index - IdStart < 15)
        {
            ++Index;
        }
        if (Index > IdStart)
        {
            Packet.AckId = std::atoll(Text.substr(IdStart, Index - IdStart).c_str());
        }
        Packet.Json = Text.substr(Index);
        return Packet;
    }

    std::string FNpcifySocketIo::Connect(const std::string& ApiToken)
    {
        const size_t First = ApiToken.find_first_not_of(" \t\r\n");
        if (First == std::string::npos)
        {
            return "40";
        }
        const size_t Last = ApiToken.find_last_not_of(" \t\r\n");
        return "40{\"apiToken\":" + JsonString(ApiToken.substr(First, Last - First + 1)) + "}";
    }

    std::string FNpcifySocketIo::Event(const std::string& Name, const std::string& JsonArgument, int64_t AckId)
    {
        std::string Out = "42";
        if (AckId >= 0)
        {
            Out += std::to_string(AckId);
        }
        Out += "[" + JsonString(Name);
        if (!JsonArgument.empty())
        {
            Out += "," + JsonArgument;
        }
        return Out + "]";
    }

    std::string FNpcifySocketIo::ReadEventName(const std::string& Json)
    {
        size_t Index = SkipSpace(Json, 0);
        if (Index >= Json.size() || Json[Index] != '[')
        {
            return std::string();
        }
        Index = SkipSpace(Json, Index + 1);
        std::string Name;
        return Index < Json.size() && Json[Index] == '"' && ReadString(Json, Index, Name) ? Name : std::string();
    }

    int64_t FNpcifySocketIo::ReadNumber(const std::string& Json, const std::string& Key, int64_t Fallback)
    {
        size_t Index = FindValue(Json, Key);
        if (Index == std::string::npos)
        {
            return Fallback;
        }
        const size_t Start = Index;
        if (Index < Json.size() && Json[Index] == '-')
        {
            ++Index;
        }
        while (Index < Json.size() && IsDigit(Json[Index]) && Index - Start < 18)
        {
            ++Index;
        }
        return Index > Start ? std::atoll(Json.substr(Start, Index - Start).c_str()) : Fallback;
    }

    std::string FNpcifySocketIo::ReadStringField(const std::string& Json, const std::string& Key)
    {
        size_t Index = FindValue(Json, Key);
        std::string Value;
        return Index != std::string::npos && Index < Json.size() && Json[Index] == '"' && ReadString(Json, Index, Value)
            ? Value
            : std::string();
    }

    bool FNpcifySocketIo::ReadTrue(const std::string& Json, const std::string& Key)
    {
        const size_t Index = FindValue(Json, Key);
        return Index != std::string::npos && Json.compare(Index, 4, "true") == 0;
    }

    std::string FNpcifySocketIo::JsonString(const std::string& Value)
    {
        std::string Out = "\"";
        for (const char C : Value)
        {
            switch (C)
            {
            case '"': Out += "\\\""; break;
            case '\\': Out += "\\\\"; break;
            case '\n': Out += "\\n"; break;
            case '\r': Out += "\\r"; break;
            case '\t': Out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(C) < 0x20)
                {
                    char Buffer[8];
                    std::snprintf(Buffer, sizeof(Buffer), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(C)));
                    Out += Buffer;
                }
                else
                {
                    Out.push_back(C);
                }
                break;
            }
        }
        return Out + "\"";
    }

    std::string FNpcifySocketIo::BuildUrl(const std::string& BaseUrl)
    {
        const std::string Source = BaseUrl.empty() ? std::string("https://api.npcify.ai") : BaseUrl;
        const size_t SchemeEnd = Source.find("://");
        const std::string Scheme = SchemeEnd == std::string::npos ? std::string("https") : Source.substr(0, SchemeEnd);
        const size_t HostStart = SchemeEnd == std::string::npos ? 0 : SchemeEnd + 3;
        size_t HostEnd = Source.find_first_of("/?#", HostStart);
        if (HostEnd == std::string::npos)
        {
            HostEnd = Source.size();
        }
        const bool bSecure = Scheme == "https" || Scheme == "wss";
        return std::string(bSecure ? "wss://" : "ws://") + Source.substr(HostStart, HostEnd - HostStart) +
            "/socket.io/?EIO=4&transport=websocket";
    }

    FNpcifyConversationSession::FNpcifyConversationSession(std::string InApiToken)
        : ApiToken(std::move(InApiToken))
    {
    }

    bool FNpcifyConversationSession::IsJoined(const std::string& InConversationId) const
    {
        return bConnected && !InConversationId.empty() && JoinedConversationId == InConversationId;
    }

    void FNpcifyConversationSession::Join(const std::string& InConversationId)
    {
        ConversationId = InConversationId;
        if (bConnected && JoinedConversationId == InConversationId)
        {
            if (OnJoinResult)
            {
                OnJoinResult(InConversationId, true);
            }
            return;
        }
        bJoinPending = true;
        if (bConnected)
        {
            SendJoin();
        }
    }

    void FNpcifyConversationSession::Leave()
    {
        const std::string Left = !JoinedConversationId.empty() ? JoinedConversationId : ConversationId;
        if (bConnected && !Left.empty() && SendText)
        {
            SendText(FNpcifySocketIo::Event("conversations:leave", "{\"conversationId\":" + FNpcifySocketIo::JsonString(Left) + "}"));
        }
        ConversationId.clear();
        JoinedConversationId.clear();
        bJoinPending = false;
        JoinAckId = -1;
    }

    void FNpcifyConversationSession::FailPendingJoin()
    {
        if (!bJoinPending)
        {
            return;
        }
        bJoinPending = false;
        JoinAckId = -1;
        if (OnJoinResult)
        {
            OnJoinResult(ConversationId, false);
        }
    }

    void FNpcifyConversationSession::Refuse()
    {
        bRefused = true;
        if (bJoinPending)
        {
            bJoinPending = false;
            JoinAckId = -1;
            if (OnJoinResult)
            {
                OnJoinResult(ConversationId, false);
            }
        }
        if (OnRefused)
        {
            OnRefused();
        }
    }

    void FNpcifyConversationSession::SendJoin()
    {
        if (!SendText || ConversationId.empty())
        {
            return;
        }
        JoinAckId = NextAckId++;
        SendText(FNpcifySocketIo::Event(
            "conversations:join",
            "{\"conversationId\":" + FNpcifySocketIo::JsonString(ConversationId) + "}",
            JoinAckId));
    }

    bool FNpcifyConversationSession::HandleText(const std::string& Text)
    {
        const FNpcifySocketIoPacket Packet = FNpcifySocketIo::Parse(Text);
        switch (Packet.EngineType)
        {
        case 0:
            PingTimeoutMs = FNpcifySocketIo::ReadNumber(Packet.Json, "pingInterval", 25000) +
                FNpcifySocketIo::ReadNumber(Packet.Json, "pingTimeout", 20000);
            if (SendText)
            {
                SendText(FNpcifySocketIo::Connect(ApiToken));
            }
            return true;
        case 1:
            return false;
        case 2:
            if (SendText)
            {
                SendText("3");
            }
            return true;
        case 4:
            break;
        default:
            return true;
        }
        if (Packet.Namespace != "/")
        {
            return true;
        }
        switch (Packet.SocketType)
        {
        case 0:
            bConnected = true;
            if (bJoinPending)
            {
                SendJoin();
            }
            return true;
        case 1:
            return false;
        case 3:
            if (bJoinPending && Packet.AckId == JoinAckId)
            {
                bJoinPending = false;
                JoinAckId = -1;
                const bool bOk = FNpcifySocketIo::ReadTrue(Packet.Json, "ok");
                if (bOk)
                {
                    JoinedConversationId = ConversationId;
                }
                if (OnJoinResult)
                {
                    OnJoinResult(ConversationId, bOk);
                }
            }
            return true;
        case 4:
            if (OnLog)
            {
                const std::string Reason = FNpcifySocketIo::ReadStringField(Packet.Json, "message");
                OnLog("The conversation stream refused the connection: " + (Reason.empty() ? std::string("no reason") : Reason));
            }
            Refuse();
            return false;
        case 2:
            if (FNpcifySocketIo::ReadEventName(Packet.Json) == "exception")
            {
                if (OnLog)
                {
                    OnLog("The conversation stream reported: " + Packet.Json);
                }
                // The server refuses the join of a conversation that the key cannot read (for
                // example a rotated key) and disconnects: retrying fails the same way.
                if (Packet.Json.find("CONVERSATION_ACCESS_DENIED") != std::string::npos)
                {
                    Refuse();
                    return false;
                }
            }
            return true;
        case 5:
        case 6:
        {
            FBinaryEvent Event;
            Event.Name = Packet.SocketType == 5 ? FNpcifySocketIo::ReadEventName(Packet.Json) : std::string();
            Event.Expected = Packet.Attachments;
            if (Event.Expected <= 0)
            {
                Dispatch(std::move(Event));
                return true;
            }
            if (BinaryEvents.size() >= MaxUnpaired)
            {
                // A lost header would pair every later attachment with the wrong event.
                if (OnLog)
                {
                    OnLog("The conversation stream sent too many binary events without their data; it connects again.");
                }
                return false;
            }
            BinaryEvents.push_back(std::move(Event));
            PairAttachments();
            return true;
        }
        default:
            return true;
        }
    }

    bool FNpcifyConversationSession::HandleBinary(const uint8_t* Data, size_t Size)
    {
        if (Attachments.size() >= MaxUnpaired)
        {
            // A lost attachment would pair every later header with the wrong data.
            if (OnLog)
            {
                OnLog("The conversation stream sent too much binary data without its events; it connects again.");
            }
            return false;
        }
        Attachments.emplace_back(Data, Data + Size);
        PairAttachments();
        return true;
    }

    void FNpcifyConversationSession::PairAttachments()
    {
        while (!BinaryEvents.empty() && !Attachments.empty())
        {
            FBinaryEvent& Head = BinaryEvents.front();
            Head.Parts.push_back(std::move(Attachments.front()));
            Attachments.pop_front();
            if (static_cast<int>(Head.Parts.size()) >= Head.Expected)
            {
                FBinaryEvent Event = std::move(Head);
                BinaryEvents.pop_front();
                Dispatch(std::move(Event));
            }
        }
    }

    void FNpcifyConversationSession::Dispatch(FBinaryEvent Binary)
    {
        const std::vector<std::vector<uint8_t>>& Parts = Binary.Parts;
        if (Binary.Name != "conversations:stream" || Parts.size() != 1 || !OnStreamEvent)
        {
            return;
        }
        try
        {
            const FNpcifyMsgPackValue Payload = FNpcifyProtocol::DecodeNpcp(Parts[0], ENpcifyPerformanceKind::Stream);
            const FNpcifyMsgPackValue* Id = Payload.FindField("conversationId");
            const FNpcifyMsgPackValue* Event = Payload.FindField("event");
            if (!Id || !Id->IsString() || !Event || !Event->IsMap() ||
                (Id->StringValue != JoinedConversationId && Id->StringValue != ConversationId))
            {
                return;
            }
            OnStreamEvent(Id->StringValue, *Event);
        }
        catch (const std::exception& Error)
        {
            if (OnLog)
            {
                OnLog(std::string("A conversation stream event was dropped: ") + Error.what());
            }
        }
    }
}
