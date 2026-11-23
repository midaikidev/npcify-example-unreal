#pragma once

#include "Core/NpcifyProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Npcify
{
    struct FNpcifyOptions
    {
        static constexpr size_t MaxMemoryIdentifierLength = 128;

        std::string BaseUrl = "https://api.npcify.ai";
        std::string CharacterId;
        std::string ApiKey;
        std::string ApiToken;
        /**
         * Optional player memory identifier when SDK Player Memory is enabled on the character.
         * Non-empty identifiers isolate memories per player; empty maps to the character owner's
         * Dashboard -> Memory bucket.
         */
        std::string MemoryIdentifier;
        float Volume = 1.0f;
        bool bMuted = false;

        std::string ResolvedToken() const
        {
            return !ApiToken.empty() ? ApiToken : ApiKey;
        }

        static std::string NormalizeMemoryIdentifier(const std::string& Value)
        {
            std::string Cleaned;
            Cleaned.reserve(Value.size());
            for (unsigned char Ch : Value)
            {
                if (Ch > 0x1fU && Ch != 0x7fU)
                {
                    Cleaned.push_back(static_cast<char>(Ch));
                }
            }
            const auto IsSpace = [](unsigned char Ch)
            {
                return Ch == ' ' || Ch == '\t' || Ch == '\n' || Ch == '\r' || Ch == '\f' || Ch == '\v';
            };
            size_t Start = 0;
            while (Start < Cleaned.size() && IsSpace(static_cast<unsigned char>(Cleaned[Start])))
            {
                ++Start;
            }
            size_t End = Cleaned.size();
            while (End > Start && IsSpace(static_cast<unsigned char>(Cleaned[End - 1])))
            {
                --End;
            }
            if (Start >= End)
            {
                return std::string();
            }
            std::string Trimmed = Cleaned.substr(Start, End - Start);
            // Truncate on UTF-8 code point boundaries up to MaxMemoryIdentifierLength code points.
            size_t BytePos = 0;
            size_t CodePoints = 0;
            while (BytePos < Trimmed.size() && CodePoints < MaxMemoryIdentifierLength)
            {
                const unsigned char Lead = static_cast<unsigned char>(Trimmed[BytePos]);
                size_t Step = 1;
                if ((Lead & 0x80U) == 0U) Step = 1;
                else if ((Lead & 0xE0U) == 0xC0U) Step = 2;
                else if ((Lead & 0xF0U) == 0xE0U) Step = 3;
                else if ((Lead & 0xF8U) == 0xF0U) Step = 4;
                if (BytePos + Step > Trimmed.size())
                {
                    break;
                }
                BytePos += Step;
                ++CodePoints;
            }
            Trimmed.resize(BytePos);
            while (!Trimmed.empty() && IsSpace(static_cast<unsigned char>(Trimmed.back())))
            {
                Trimmed.pop_back();
            }
            return Trimmed;
        }
    };

    struct FStagingVisemeCue
    {
        std::string Viseme;
        double Time = 0.0;
        double Duration = 0.08;
        double Weight = 1.0;
    };

    struct FStagingWordCue
    {
        std::string Word;
        double StartTime = 0.0;
        double EndTime = 0.0;
    };

    struct FStagingCharacterCue
    {
        std::string Character;
        double StartTime = 0.0;
        double EndTime = 0.0;
    };

    struct FStagingAudioSource
    {
        std::string Type = "silent";
        std::string MimeType = "audio/wav";
        std::vector<uint8_t> Data;
        std::string Url;
        std::string StreamId;
        double Duration = 0.0;
    };

    struct FStagingDialogueBlock
    {
        std::string Id;
        std::string Type = "speech";
        double StartTime = 0.0;
        double EndTime = 0.0;
        std::string Text;
        double Duration = 0.0;
        FStagingAudioSource Audio;
        std::vector<FStagingVisemeCue> Visemes;
        std::vector<FStagingWordCue> Words;
        std::vector<FStagingCharacterCue> Characters;
        /**
         * The last speech block of a streamed reply can still grow: later revisions extend its
         * timing and lip-sync until its synthesis and alignment finish (docs/streaming_reply.md).
         */
        bool bPending = false;
    };

    struct FStagingExpressionClip
    {
        std::string Id;
        std::string Expression;
        double StartTime = 0.0;
        double EndTime = 0.0;
        double Weight = 1.0;
    };

    struct FStagingAnimationClip
    {
        std::string Id;
        std::string Animation;
        double StartTime = 0.0;
        double EndTime = 0.0;
        bool bLoop = false;
    };

    /** The motion prompt of a span of a live motion staging (timeline.motions, NPCP 1.1). */
    struct FStagingMotionPrompt
    {
        double StartTime = 0.0;
        double EndTime = 0.0;
        std::string Prompt;
    };

    /**
     * The stored live motion frames of a completed staging (timeline.motion, NPCP 1.1): the same
     * float32 bytes as the motion_frames events, at FramesUrl or in FramesData.
     */
    struct FStagingMotionTrack
    {
        bool bValid = false;
        double Fps = 30.0;
        int FrameCount = 0;
        std::vector<std::string> Bones;
        double RestHipsHeight = 1.0;
        std::string FramesUrl;
        std::vector<uint8_t> FramesData;
    };

    struct FNpcifyVariantSummary
    {
        std::string Key = FNpcifyProtocol::DefaultVariantKey;
        std::string Label;
        std::string Description;
        bool bR18 = false;
        std::string PortraitUrl;
        std::string CreditText;
    };

    struct FNpcifyStagingToolCallResult
    {
        bool bOk = false;
        std::string Status = "applied";
        std::string PreviousVariant = FNpcifyProtocol::DefaultVariantKey;
        std::string ActiveVariant = FNpcifyProtocol::DefaultVariantKey;
        std::string Error;
    };

    struct FNpcifyStagingToolCall
    {
        std::string Id;
        std::string Name = "switch_variant";
        double Timestamp = 0.0;
        std::string Variant = FNpcifyProtocol::DefaultVariantKey;
        FNpcifyStagingToolCallResult Result;
    };

    struct FNpcifyAvatarAnimationInfo
    {
        std::string Id;
        std::string Name;
        bool bLoop = false;
    };

    struct FNpcifyAvatarInfo
    {
        std::string Id;
        std::string Name;
        std::string IdleAnimationId = "idle";
        bool bAllowCustomReply = true;
        bool bSpeechInputEnabled = false;
        bool bBakedConversationEnabled = false;
        int BakedConversationRevision = 0;
        bool bRealtimeMotionEnabled = false;
        bool bSightEnabled = false;
        std::string SightProvider;
        bool bSdkMemoryEnabled = false;
        std::vector<FNpcifyAvatarAnimationInfo> Animations;
        std::vector<FNpcifyVariantSummary> Variants;
    };

    struct FStagingTimeline
    {
        double Duration = 0.0;
        std::string InitialVariant;
        std::string ActiveVariant;
        std::vector<FStagingDialogueBlock> Dialogue;
        std::vector<FStagingExpressionClip> Expressions;
        std::vector<FStagingAnimationClip> Animations;
        std::vector<FStagingMotionPrompt> Motions;
        FStagingMotionTrack Motion;
        std::vector<FNpcifyStagingToolCall> ToolCalls;
    };

    using FNpcifyStagingTimeline = FStagingTimeline;

    struct FStagingReplyOption
    {
        std::string Label;
        bool bEnd = false;
    };

    struct FStagingResultPayload
    {
        int ProtocolVersion = 2;
        std::string TaskId;
        std::string StagingId;
        std::string Text;
        FStagingTimeline Timeline;
        std::vector<FStagingReplyOption> Options;
    };

    struct FConversationCreationResult
    {
        std::string ConversationId;
        FStagingResultPayload OpeningStaging;
        int ChatPricePoints = 0;
    };

    struct FBakedConversationNode
    {
        std::string Id;
        std::string ParentId;
        std::string TriggerText;
        FStagingResultPayload Payload;
    };

    struct FBakedConversationAsset
    {
        static constexpr const char* ExpectedFormat = "npcify.baked-conversation";
        static constexpr int ExpectedVersion = 1;

        std::string Format = ExpectedFormat;
        int Version = ExpectedVersion;
        std::string AvatarId;
        int Revision = 1;
        std::string RootStagingId;
        std::vector<FBakedConversationNode> Stagings;
    };

    struct FConversationWorldlineNode
    {
        std::string Id;
        std::string ParentId;
        std::string TriggerText;
        std::string Text;
        std::vector<FStagingReplyOption> Options;
        std::string CreatedAt = "1970-01-01T00:00:00.000Z";
        std::string InitialVariant;
        std::string ActiveVariant;
        std::vector<FNpcifyStagingToolCall> ToolCalls;
    };

    using FNpcifyWorldlineStaging = FConversationWorldlineNode;

    struct FConversationWorldline
    {
        std::string ConversationId;
        std::string OpeningStagingId;
        std::string ActiveStagingId;
        std::vector<FConversationWorldlineNode> Stagings;
    };

    struct FNpcifyPlaybackCursor
    {
        double TimeSeconds = 0.0;
        double DurationSeconds = 0.0;
        std::string BlockId;
    };

    struct FNpcifySpeechState
    {
        bool bSpeaking = false;
        bool bThinking = false;
        bool bHasCurrentSpeech = false;
        FStagingDialogueBlock CurrentSpeech;
        std::string CurrentText;
        std::string RevealedText;
        std::vector<FStagingReplyOption> Options;
        FNpcifyPlaybackCursor Cursor;
    };

    class FNpcifySchemaMapper
    {
    public:
        static FStagingResultPayload DecodeStaging(const std::vector<uint8_t>& NpcpBytes);
        static FConversationCreationResult DecodeConversation(const std::vector<uint8_t>& NpcpBytes);
        static FBakedConversationAsset DecodeBakedConversation(const std::vector<uint8_t>& NpcpBytes);
        static std::vector<uint8_t> EncodeBakedConversation(
            const FBakedConversationAsset& Asset,
            bool bCompress = true
        );

        static FStagingResultPayload MapStagingResultPayload(const FNpcifyMsgPackValue& MapVal);
        static FStagingTimeline MapTimeline(const FNpcifyMsgPackValue& MapVal);
        static FStagingDialogueBlock MapDialogueBlock(const FNpcifyMsgPackValue& MapVal);
        static FNpcifyStagingToolCall MapToolCall(const FNpcifyMsgPackValue& MapVal);
        static FNpcifyMsgPackValue ToolCallToMsgPack(const FNpcifyStagingToolCall& Call);
        static FNpcifyAvatarInfo MapAvatarInfo(const FNpcifyMsgPackValue& MapVal);
        static FConversationWorldline MapWorldline(const FNpcifyMsgPackValue& MapVal);
        static FBakedConversationAsset MapBakedConversationAsset(const FNpcifyMsgPackValue& MapVal);
        static FNpcifyMsgPackValue ToMsgPack(const FStagingResultPayload& Payload);
    };
}
