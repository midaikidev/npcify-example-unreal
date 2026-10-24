#pragma once

#include "Core/NpcifyModels.h"

#include <map>
#include <string>
#include <vector>

namespace Npcify
{
    struct FBakedReplyResult
    {
        FStagingResultPayload Payload;
        bool bEnded = false;
    };

    class FNpcifyBakedConversation
    {
    public:
        explicit FNpcifyBakedConversation(
            const FBakedConversationAsset& InAsset,
            const std::string& ExpectedCharacterId = ""
        );

        static FNpcifyBakedConversation FromBytes(
            const std::vector<uint8_t>& NpcpBytes,
            const std::string& ExpectedCharacterId = ""
        );

        const FBakedConversationAsset& GetAsset() const { return Asset; }
        const std::string& GetActiveStagingId() const { return ActiveId; }
        const FBakedConversationNode* GetActiveNode() const;
        std::vector<FStagingReplyOption> GetCurrentOptions() const;

        void AssertAnimationsAvailable(const std::vector<std::string>& AvailableAnimationIds) const;
        FStagingResultPayload Start();
        FStagingResultPayload Select(const std::string& StagingId);
        FBakedReplyResult Reply(const std::string& Message, const std::string& ParentStagingId = "");
        FConversationWorldline Worldline(const std::string& ConversationId) const;

    private:
        const FBakedConversationNode* FindChild(const std::string& ParentId, const std::string& Label) const;
        static std::string TrimString(const std::string& Value);

        FBakedConversationAsset Asset;
        std::map<std::string, FBakedConversationNode> Nodes;
        std::string ActiveId;
    };
}
