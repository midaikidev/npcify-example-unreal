#include "Core/NpcifySight.h"

#include "Core/NpcifyJson.h"
#include "Core/NpcifySocketIo.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

namespace Npcify
{
    namespace
    {
        const char* const OffMessage = "Sight is off for this character. Its owner turns it on in the Sight tab.";

        // Math.round of JavaScript: .5 rounds up.
        int RoundHalfUp(double Value)
        {
            return static_cast<int>(std::floor(Value + 0.5));
        }

        const FNpcifyMsgPackValue* Field(const FNpcifyMsgPackValue& Map, const char* Key)
        {
            return Map.IsMap() ? Map.FindField(Key) : nullptr;
        }
    }

    bool SightCodes::StopsSight(const std::string& Code)
    {
        return Code == Disabled || Code == InsufficientPoints || Code == AccessDenied;
    }

    std::string SightCodes::Message(const std::string& Code)
    {
        static const std::pair<const char*, const char*> Messages[] = {
            {Disabled, OffMessage},
            {InsufficientPoints, "The owner of the character has no points to pay for sight."},
            {AccessDenied, "This connection has no access to the conversation."},
            {Disconnected, "The conversation connection is not open."},
            {Timeout, "The server did not answer in time."},
            {ConversationRequired, "Create a conversation before sight starts."},
            {AlreadyStarted, "Sight has already started."},
            {CameraPermissionDenied, "Camera access was denied."},
            {CameraUnavailable, "No camera is available."},
            {SourceUnavailable, "The sight source cannot be read."},
            {SourceEnded, "The video of sight ended."},
        };
        for (const std::pair<const char*, const char*>& Entry : Messages)
        {
            if (Code == Entry.first)
            {
                return Entry.second;
            }
        }
        return Code;
    }

    std::string FSightFrameRequest::ToJson() const
    {
        return "{\"conversationId\":" + FNpcifySocketIo::JsonString(ConversationId) +
            ",\"capturedAt\":" + std::to_string(CapturedAt) +
            ",\"width\":" + std::to_string(Width) +
            ",\"height\":" + std::to_string(Height) +
            ",\"source\":" + FNpcifySocketIo::JsonString(Source) +
            ",\"jpeg\":{\"_placeholder\":true,\"num\":0}}";
    }

    std::string FSightFrameRequest::EventPacket(int64_t AckId) const
    {
        return "451-" + std::to_string(AckId) + "[\"conversations:sight\"," + ToJson() + "]";
    }

    FSightAck FSightAck::Accepted(const std::string& ObservationId)
    {
        FSightAck Ack;
        Ack.bAnswered = true;
        Ack.bOk = true;
        Ack.ObservationId = ObservationId;
        return Ack;
    }

    FSightAck FSightAck::Refused(const std::string& Code)
    {
        FSightAck Ack;
        Ack.bAnswered = true;
        Ack.Error = Code;
        return Ack;
    }

    FSightAck FSightAck::NotAnswered(const std::string& Code)
    {
        FSightAck Ack;
        Ack.Error = Code;
        return Ack;
    }

    FSightAck FSightAck::FromAckJson(const std::string& Json)
    {
        FNpcifyMsgPackValue Arguments;
        if (!FNpcifyJson::TryParse(Json, Arguments) || !Arguments.IsArray() || Arguments.ArrayValue.empty())
        {
            return Refused(SightCodes::FrameInvalid);
        }
        const FNpcifyMsgPackValue& Body = Arguments.ArrayValue[0];
        const FNpcifyMsgPackValue* Ok = Field(Body, "ok");
        if (!Ok || !Ok->IsBool())
        {
            return Refused(SightCodes::FrameInvalid);
        }
        if (Ok->BoolValue)
        {
            const FNpcifyMsgPackValue* Id = Field(Body, "observationId");
            return Accepted(Id && Id->IsString() ? Id->StringValue : std::string());
        }
        const FNpcifyMsgPackValue* Error = Field(Body, "error");
        return Refused(Error && Error->IsString() && !Error->StringValue.empty() ? Error->StringValue : std::string(SightCodes::FrameInvalid));
    }

    bool FSightObservation::FromValue(const FNpcifyMsgPackValue& Value, FSightObservation& Out)
    {
        const FNpcifyMsgPackValue* Conversation = Field(Value, "conversationId");
        const FNpcifyMsgPackValue* Observation = Field(Value, "observationId");
        const FNpcifyMsgPackValue* Summary = Field(Value, "summary");
        const FNpcifyMsgPackValue* Body = Field(Value, "observation");
        const FNpcifyMsgPackValue* People = Body ? Field(*Body, "people") : nullptr;
        const FNpcifyMsgPackValue* Persons = Body ? Field(*Body, "persons") : nullptr;
        const FNpcifyMsgPackValue* Text = Body ? Field(*Body, "text") : nullptr;
        if (!Conversation || !Conversation->IsString() || !Observation || !Observation->IsString() ||
            !Summary || !Summary->IsString() || !People || !People->IsNumber() ||
            !Persons || !Persons->IsArray() || !Text || !Text->IsArray())
        {
            return false;
        }
        FSightObservation Result;
        Result.ConversationId = Conversation->StringValue;
        Result.ObservationId = Observation->StringValue;
        Result.Summary = Summary->StringValue;
        Result.People = static_cast<int>(std::max<double>(0.0, std::min<double>(1e9, People->AsDouble())));
        if (const FNpcifyMsgPackValue* CapturedAt = Field(Value, "capturedAt"))
        {
            Result.CapturedAt = CapturedAt->AsString();
        }
        if (const FNpcifyMsgPackValue* Model = Field(Value, "model"))
        {
            Result.Model = Model->AsString();
        }
        if (const FNpcifyMsgPackValue* Cost = Field(Value, "costPoints"))
        {
            Result.bHasCostPoints = Cost->IsNumber();
            Result.CostPoints = Cost->AsDouble();
        }
        for (const FNpcifyMsgPackValue& Entry : Persons->ArrayValue)
        {
            if (!Entry.IsMap())
            {
                continue;
            }
            FSightPerson Person;
            if (const FNpcifyMsgPackValue* Position = Field(Entry, "position")) Person.Position = Position->AsString();
            if (const FNpcifyMsgPackValue* Action = Field(Entry, "action")) Person.Action = Action->AsString();
            if (const FNpcifyMsgPackValue* Facing = Field(Entry, "facingCamera")) Person.bFacingCamera = Facing->IsBool() && Facing->BoolValue;
            Result.Persons.push_back(std::move(Person));
        }
        for (const FNpcifyMsgPackValue& Entry : Text->ArrayValue)
        {
            if (Entry.IsString())
            {
                Result.Text.push_back(Entry.StringValue);
            }
        }
        Out = std::move(Result);
        return true;
    }

    bool FSightObservation::FromEventJson(const std::string& Json, FSightObservation& Out)
    {
        FNpcifyMsgPackValue Arguments;
        return FNpcifyJson::TryParse(Json, Arguments) && Arguments.IsArray() && Arguments.ArrayValue.size() > 1 &&
            FromValue(Arguments.ArrayValue[1], Out);
    }

    void FitCaptureSize(int Width, int Height, int& OutWidth, int& OutHeight)
    {
        Width = std::max(1, Width);
        Height = std::max(1, Height);
        const double Scale = std::min(1.0, std::min(
            static_cast<double>(FSightLimits::CaptureWidth) / Width,
            static_cast<double>(FSightLimits::CaptureHeight) / Height));
        OutWidth = std::max(1, RoundHalfUp(Width * Scale));
        OutHeight = std::max(1, RoundHalfUp(Height * Scale));
    }

    std::vector<uint8_t> LumaGridOfBgra(const uint8_t* Pixels, int Width, int Height, int RowPitch)
    {
        constexpr int GridWidth = FSightLimits::GridWidth;
        constexpr int GridHeight = FSightLimits::GridHeight;
        std::vector<uint8_t> Grid(GridWidth * GridHeight, 0);
        if (!Pixels || Width <= 0 || Height <= 0 || RowPitch < Width)
        {
            return Grid;
        }
        // Whole numbers: the luma times 1000 (299 R + 587 G + 114 B) is exact.
        std::vector<uint64_t> Sums(Grid.size(), 0);
        std::vector<uint32_t> Counts(Grid.size(), 0);
        std::vector<uint32_t> Columns(static_cast<size_t>(Width));
        for (int X = 0; X < Width; ++X)
        {
            Columns[static_cast<size_t>(X)] = static_cast<uint32_t>(std::min(GridWidth - 1, X * GridWidth / Width));
        }
        for (int Y = 0; Y < Height; ++Y)
        {
            const size_t Row = static_cast<size_t>(std::min(GridHeight - 1, Y * GridHeight / Height) * GridWidth);
            uint64_t* RowSums = &Sums[Row];
            uint32_t* RowCounts = &Counts[Row];
            const uint8_t* Pixel = Pixels + static_cast<size_t>(Y) * static_cast<size_t>(RowPitch) * 4;
            for (int X = 0; X < Width; ++X, Pixel += 4)
            {
                const uint32_t Column = Columns[static_cast<size_t>(X)];
                RowSums[Column] += 114u * Pixel[0] + 587u * Pixel[1] + 299u * Pixel[2];
                ++RowCounts[Column];
            }
        }
        for (size_t Cell = 0; Cell < Grid.size(); ++Cell)
        {
            // Round half up, as Math.round: (2 * sum + 1000 * count) / (2000 * count).
            Grid[Cell] = Counts[Cell] > 0
                ? static_cast<uint8_t>((2 * Sums[Cell] + 1000ull * Counts[Cell]) / (2000ull * Counts[Cell]))
                : 0;
        }
        return Grid;
    }

    int64_t FNpcifyConversationSession::SendSightFrame(const FSightFrameRequest& Request)
    {
        if (!bConnected || !SendText || !SendBinary || !Request.Jpeg ||
            JoinedConversationId.empty() || JoinedConversationId != FNpcifySocketIo::TrimId(Request.ConversationId))
        {
            return -1;
        }
        const int64_t AckId = NextAckId++;
        // The server reads the next binary message as the attachment of this header.
        SendText(Request.EventPacket(AckId));
        SendBinary(*Request.Jpeg);
        return AckId;
    }

    double MeanLumaChange(const std::vector<uint8_t>& Previous, const std::vector<uint8_t>& Next)
    {
        const size_t Length = std::min(Previous.size(), Next.size());
        if (Length == 0)
        {
            return 0.0;
        }
        int64_t Sum = 0;
        for (size_t Index = 0; Index < Length; ++Index)
        {
            Sum += std::abs(static_cast<int>(Next[Index]) - static_cast<int>(Previous[Index]));
        }
        return static_cast<double>(Sum) / static_cast<double>(Length);
    }
}
