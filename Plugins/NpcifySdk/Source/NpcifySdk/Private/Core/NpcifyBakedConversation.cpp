#include "Core/NpcifyBakedConversation.h"

#include <cctype>
#include <set>
#include <stdexcept>

namespace Npcify
{
    FNpcifyBakedConversation::FNpcifyBakedConversation(
        const FBakedConversationAsset& InAsset,
        const std::string& ExpectedCharacterId
    )
        : Asset(InAsset)
    {
        if (Asset.Format != FBakedConversationAsset::ExpectedFormat ||
            Asset.Version != FBakedConversationAsset::ExpectedVersion ||
            (!ExpectedCharacterId.empty() && Asset.AvatarId != ExpectedCharacterId))
        {
            throw std::runtime_error("Unsupported baked conversation asset.");
        }

        for (const auto& Node : Asset.Stagings)
        {
            if (Node.Id.empty() ||
                Nodes.find(Node.Id) != Nodes.end() ||
                Node.Payload.StagingId != Node.Id)
            {
                throw std::runtime_error("Invalid baked performance ID.");
            }

            for (const auto& Block : Node.Payload.Timeline.Dialogue)
            {
                if (Block.Type == "speech")
                {
                    const bool bEmbedded =
                        (Block.Audio.Type == "bytes" || Block.Audio.Type == "base64") &&
                        !Block.Audio.Data.empty();
                    const bool bSilent = Block.Audio.Type == "silent";
                    if (!bEmbedded && !bSilent)
                    {
                        throw std::runtime_error("Baked performances must embed all audio.");
                    }
                }
            }

            Nodes.emplace(Node.Id, Node);
        }

        std::vector<const FBakedConversationNode*> Roots;
        for (const auto& Pair : Nodes)
        {
            if (Pair.second.ParentId.empty())
            {
                Roots.push_back(&Pair.second);
            }
        }

        if ((!Nodes.empty() && (Roots.size() != 1 || Roots[0]->Id != Asset.RootStagingId)) ||
            (Nodes.empty() && !Asset.RootStagingId.empty()))
        {
            throw std::runtime_error("Invalid baked conversation opening.");
        }

        for (const auto& Pair : Nodes)
        {
            std::set<std::string> Seen;
            const FBakedConversationNode* Current = &Pair.second;
            while (Current)
            {
                if (!Seen.insert(Current->Id).second)
                {
                    throw std::runtime_error("Baked conversation contains a cycle.");
                }
                if (!Current->ParentId.empty())
                {
                    auto ParentIt = Nodes.find(Current->ParentId);
                    if (ParentIt == Nodes.end())
                    {
                        throw std::runtime_error("Baked conversation has a missing parent.");
                    }
                    Current = &ParentIt->second;
                }
                else
                {
                    Current = nullptr;
                }
            }

            for (const auto& Option : Pair.second.Payload.Options)
            {
                if (!Option.bEnd && !FindChild(Pair.second.Id, Option.Label))
                {
                    throw std::runtime_error("Baked conversation has an unrecorded option.");
                }
            }
        }
    }

    FNpcifyBakedConversation FNpcifyBakedConversation::FromBytes(
        const std::vector<uint8_t>& NpcpBytes,
        const std::string& ExpectedCharacterId
    )
    {
        return FNpcifyBakedConversation(
            FNpcifySchemaMapper::DecodeBakedConversation(NpcpBytes),
            ExpectedCharacterId
        );
    }

    const FBakedConversationNode* FNpcifyBakedConversation::GetActiveNode() const
    {
        if (ActiveId.empty()) return nullptr;
        auto It = Nodes.find(ActiveId);
        return It != Nodes.end() ? &It->second : nullptr;
    }

    std::vector<FStagingReplyOption> FNpcifyBakedConversation::GetCurrentOptions() const
    {
        const FBakedConversationNode* Node = GetActiveNode();
        return Node ? Node->Payload.Options : std::vector<FStagingReplyOption>{};
    }

    void FNpcifyBakedConversation::AssertAnimationsAvailable(
        const std::vector<std::string>& AvailableAnimationIds
    ) const
    {
        const std::set<std::string> Available(AvailableAnimationIds.begin(), AvailableAnimationIds.end());
        for (const auto& Node : Asset.Stagings)
        {
            for (const auto& Clip : Node.Payload.Timeline.Animations)
            {
                if (!Clip.Animation.empty() && Available.find(Clip.Animation) == Available.end())
                {
                    throw std::runtime_error(
                        "Recorded animation " + Clip.Animation + " is unavailable. Restore it or regenerate the performance."
                    );
                }
            }
        }
    }

    FStagingResultPayload FNpcifyBakedConversation::Start()
    {
        if (Asset.RootStagingId.empty())
        {
            throw std::runtime_error("This character has no recorded conversation yet.");
        }
        return Select(Asset.RootStagingId);
    }

    FStagingResultPayload FNpcifyBakedConversation::Select(const std::string& StagingId)
    {
        auto It = Nodes.find(StagingId);
        if (It == Nodes.end())
        {
            throw std::runtime_error("Baked performance not found.");
        }
        ActiveId = It->second.Id;
        return It->second.Payload;
    }

    FBakedReplyResult FNpcifyBakedConversation::Reply(
        const std::string& Message,
        const std::string& ParentStagingId
    )
    {
        const std::string TargetParentId = !ParentStagingId.empty() ? ParentStagingId : ActiveId;
        auto ParentIt = Nodes.find(TargetParentId);
        if (ParentIt == Nodes.end())
        {
            throw std::runtime_error("Choose one of the recorded reply options.");
        }

        const std::string Trimmed = TrimString(Message);
        const FStagingReplyOption* MatchedOption = nullptr;
        for (const auto& Opt : ParentIt->second.Payload.Options)
        {
            if (Opt.Label == Trimmed)
            {
                MatchedOption = &Opt;
                break;
            }
        }

        if (!MatchedOption)
        {
            throw std::runtime_error("Choose one of the recorded reply options.");
        }

        if (MatchedOption->bEnd)
        {
            ActiveId.clear();
            FBakedReplyResult Out;
            Out.Payload = ParentIt->second.Payload;
            Out.Payload.Options.clear();
            Out.bEnded = true;
            return Out;
        }

        const FBakedConversationNode* Child = FindChild(ParentIt->second.Id, MatchedOption->Label);
        if (!Child)
        {
            throw std::runtime_error("This reply has not been recorded.");
        }

        FBakedReplyResult Out;
        Out.Payload = Select(Child->Id);
        Out.bEnded = false;
        return Out;
    }

    FConversationWorldline FNpcifyBakedConversation::Worldline(const std::string& ConversationId) const
    {
        FConversationWorldline Out;
        Out.ConversationId = ConversationId;
        Out.OpeningStagingId = Asset.RootStagingId;
        Out.ActiveStagingId = ActiveId;
        for (const auto& Node : Asset.Stagings)
        {
            FConversationWorldlineNode WNode;
            WNode.Id = Node.Id;
            WNode.ParentId = Node.ParentId;
            WNode.TriggerText = Node.TriggerText;
            WNode.Text = Node.Payload.Text;
            WNode.Options = Node.Payload.Options;
            Out.Stagings.push_back(std::move(WNode));
        }
        return Out;
    }

    const FBakedConversationNode* FNpcifyBakedConversation::FindChild(
        const std::string& ParentId,
        const std::string& Label
    ) const
    {
        for (const auto& Node : Asset.Stagings)
        {
            if (Node.ParentId == ParentId && Node.TriggerText == Label)
            {
                auto It = Nodes.find(Node.Id);
                return It != Nodes.end() ? &It->second : nullptr;
            }
        }
        return nullptr;
    }

    std::string FNpcifyBakedConversation::TrimString(const std::string& Value)
    {
        size_t Start = 0;
        while (Start < Value.size() && std::isspace(static_cast<unsigned char>(Value[Start])))
        {
            ++Start;
        }
        size_t End = Value.size();
        while (End > Start && std::isspace(static_cast<unsigned char>(Value[End - 1])))
        {
            --End;
        }
        return Value.substr(Start, End - Start);
    }
}
