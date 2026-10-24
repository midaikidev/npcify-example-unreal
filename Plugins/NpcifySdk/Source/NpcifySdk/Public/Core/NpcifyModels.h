#pragma once

#include "Core/NpcifyProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Npcify
{
    struct FNpcifyOptions
    {
        std::string BaseUrl = "https://api.npcify.ai";
        std::string CharacterId;
        std::string ApiKey;
        std::string ApiToken;
        float Volume = 1.0f;
        bool bMuted = false;

        std::string ResolvedToken() const
        {
            return !ApiToken.empty() ? ApiToken : ApiKey;
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

    struct FStagingTimeline
    {
        double Duration = 0.0;
        std::vector<FStagingDialogueBlock> Dialogue;
        std::vector<FStagingExpressionClip> Expressions;
        std::vector<FStagingAnimationClip> Animations;
    };

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
    };

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
        static FBakedConversationAsset MapBakedConversationAsset(const FNpcifyMsgPackValue& MapVal);
        static FNpcifyMsgPackValue ToMsgPack(const FStagingResultPayload& Payload);
    };
}
