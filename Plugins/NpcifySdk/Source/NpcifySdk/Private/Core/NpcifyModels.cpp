#include "Core/NpcifyModels.h"

#include <algorithm>
#include <stdexcept>

namespace Npcify
{
    namespace
    {
        const FNpcifyMsgPackValue& RequireMap(const FNpcifyMsgPackValue& Val, const char* Context)
        {
            if (!Val.IsMap())
            {
                throw std::runtime_error(std::string("Expected MessagePack map for ") + Context + ".");
            }
            return Val;
        }

        std::string GetStringField(
            const FNpcifyMsgPackValue& MapVal,
            const std::string& Key,
            const std::string& DefaultVal = ""
        )
        {
            const FNpcifyMsgPackValue* Field = MapVal.FindField(Key);
            return Field && Field->IsString() ? Field->StringValue : DefaultVal;
        }

        double GetDoubleField(
            const FNpcifyMsgPackValue& MapVal,
            const std::string& Key,
            double DefaultVal = 0.0
        )
        {
            const FNpcifyMsgPackValue* Field = MapVal.FindField(Key);
            return Field ? Field->AsDouble(DefaultVal) : DefaultVal;
        }

        int64_t GetIntField(
            const FNpcifyMsgPackValue& MapVal,
            const std::string& Key,
            int64_t DefaultVal = 0
        )
        {
            const FNpcifyMsgPackValue* Field = MapVal.FindField(Key);
            return Field ? Field->AsInt64(DefaultVal) : DefaultVal;
        }

        bool GetBoolField(
            const FNpcifyMsgPackValue& MapVal,
            const std::string& Key,
            bool DefaultVal = false
        )
        {
            const FNpcifyMsgPackValue* Field = MapVal.FindField(Key);
            return Field ? Field->AsBool(DefaultVal) : DefaultVal;
        }
    }

    FStagingResultPayload FNpcifySchemaMapper::DecodeStaging(const std::vector<uint8_t>& NpcpBytes)
    {
        const FNpcifyMsgPackValue Raw = FNpcifyProtocol::DecodeNpcp(NpcpBytes, ENpcifyPerformanceKind::Staging);
        return MapStagingResultPayload(RequireMap(Raw, "staging"));
    }

    FConversationCreationResult FNpcifySchemaMapper::DecodeConversation(const std::vector<uint8_t>& NpcpBytes)
    {
        const FNpcifyMsgPackValue Raw = FNpcifyProtocol::DecodeNpcp(NpcpBytes, ENpcifyPerformanceKind::Conversation);
        const FNpcifyMsgPackValue& MapVal = RequireMap(Raw, "conversation");
        const FNpcifyMsgPackValue* ConvMap = MapVal.FindField("conversation");
        const std::string Id = ConvMap && ConvMap->IsMap()
            ? GetStringField(*ConvMap, "id")
            : GetStringField(MapVal, "conversationId");

        if (Id.empty())
        {
            throw std::runtime_error("Missing conversation ID in conversation response.");
        }

        const FNpcifyMsgPackValue* OpeningRaw = MapVal.FindField("openingStaging");
        if (!OpeningRaw || !OpeningRaw->IsMap())
        {
            throw std::runtime_error("Missing openingStaging in conversation response.");
        }

        FConversationCreationResult Out;
        Out.ConversationId = Id;
        Out.OpeningStaging = MapStagingResultPayload(*OpeningRaw);
        Out.ChatPricePoints = static_cast<int>(GetIntField(MapVal, "chatPricePoints", 0));
        return Out;
    }

    FBakedConversationAsset FNpcifySchemaMapper::DecodeBakedConversation(const std::vector<uint8_t>& NpcpBytes)
    {
        const FNpcifyMsgPackValue Raw = FNpcifyProtocol::DecodeNpcp(NpcpBytes, ENpcifyPerformanceKind::Baked);
        return MapBakedConversationAsset(RequireMap(Raw, "baked"));
    }

    std::vector<uint8_t> FNpcifySchemaMapper::EncodeBakedConversation(
        const FBakedConversationAsset& Asset,
        bool bCompress
    )
    {
        std::vector<FNpcifyMsgPackValue> Stagings;
        Stagings.reserve(Asset.Stagings.size());
        for (const auto& Node : Asset.Stagings)
        {
            std::map<std::string, FNpcifyMsgPackValue> NodeMap;
            NodeMap["id"] = FNpcifyMsgPackValue::MakeString(Node.Id);
            NodeMap["parentId"] = Node.ParentId.empty()
                ? FNpcifyMsgPackValue::MakeNull()
                : FNpcifyMsgPackValue::MakeString(Node.ParentId);
            NodeMap["triggerText"] = Node.TriggerText.empty()
                ? FNpcifyMsgPackValue::MakeNull()
                : FNpcifyMsgPackValue::MakeString(Node.TriggerText);
            NodeMap["payload"] = ToMsgPack(Node.Payload);
            Stagings.push_back(FNpcifyMsgPackValue::MakeMap(std::move(NodeMap)));
        }

        std::map<std::string, FNpcifyMsgPackValue> Root;
        Root["format"] = FNpcifyMsgPackValue::MakeString(Asset.Format);
        Root["version"] = FNpcifyMsgPackValue::MakeInt(Asset.Version);
        Root["avatarId"] = FNpcifyMsgPackValue::MakeString(Asset.AvatarId);
        Root["revision"] = FNpcifyMsgPackValue::MakeInt(Asset.Revision);
        Root["rootStagingId"] = Asset.RootStagingId.empty()
            ? FNpcifyMsgPackValue::MakeNull()
            : FNpcifyMsgPackValue::MakeString(Asset.RootStagingId);
        Root["stagings"] = FNpcifyMsgPackValue::MakeArray(std::move(Stagings));

        return FNpcifyProtocol::EncodeNpcp(
            ENpcifyPerformanceKind::Baked,
            FNpcifyMsgPackValue::MakeMap(std::move(Root)),
            bCompress
        );
    }

    FStagingResultPayload FNpcifySchemaMapper::MapStagingResultPayload(const FNpcifyMsgPackValue& MapVal)
    {
        RequireMap(MapVal, "staging payload");
        const std::string StagingId = GetStringField(MapVal, "stagingId");
        if (StagingId.empty())
        {
            throw std::runtime_error("Staging payload is missing stagingId.");
        }

        FStagingResultPayload Out;
        Out.ProtocolVersion = static_cast<int>(GetIntField(MapVal, "protocolVersion", 2));
        Out.TaskId = GetStringField(MapVal, "taskId");
        Out.StagingId = StagingId;
        Out.Text = GetStringField(MapVal, "text");

        const FNpcifyMsgPackValue* TimelineMap = MapVal.FindField("timeline");
        if (TimelineMap && TimelineMap->IsMap())
        {
            Out.Timeline = MapTimeline(*TimelineMap);
        }

        const FNpcifyMsgPackValue* OptionsArr = MapVal.FindField("options");
        if (OptionsArr && OptionsArr->IsArray())
        {
            for (const auto& Item : OptionsArr->ArrayValue)
            {
                if (Item.IsMap())
                {
                    FStagingReplyOption Opt;
                    Opt.Label = GetStringField(Item, "label");
                    Opt.bEnd = GetBoolField(Item, "end", false);
                    Out.Options.push_back(std::move(Opt));
                }
            }
        }
        return Out;
    }

    FStagingTimeline FNpcifySchemaMapper::MapTimeline(const FNpcifyMsgPackValue& MapVal)
    {
        FStagingTimeline Out;
        Out.Duration = std::max(0.0, GetDoubleField(MapVal, "duration", 0.0));

        const FNpcifyMsgPackValue* DialogueArr = MapVal.FindField("dialogue");
        if (DialogueArr && DialogueArr->IsArray())
        {
            for (const auto& Item : DialogueArr->ArrayValue)
            {
                if (Item.IsMap())
                {
                    Out.Dialogue.push_back(MapDialogueBlock(Item));
                }
            }
        }

        const FNpcifyMsgPackValue* ExprArr = MapVal.FindField("expressions");
        if (ExprArr && ExprArr->IsArray())
        {
            for (const auto& Item : ExprArr->ArrayValue)
            {
                if (Item.IsMap())
                {
                    FStagingExpressionClip Clip;
                    Clip.Id = GetStringField(Item, "id");
                    Clip.StartTime = GetDoubleField(Item, "startTime", 0.0);
                    Clip.EndTime = GetDoubleField(Item, "endTime", 0.0);
                    Clip.Weight = GetDoubleField(Item, "weight", 1.0);
                    // The server sends { expression: { name, intensity, weight } }.
                    const FNpcifyMsgPackValue* Expression = Item.FindField("expression");
                    if (Expression && Expression->IsMap())
                    {
                        Clip.Expression = GetStringField(*Expression, "name");
                        Clip.Weight = GetDoubleField(*Expression, "weight", Clip.Weight);
                    }
                    else
                    {
                        Clip.Expression = GetStringField(Item, "expression");
                    }
                    Out.Expressions.push_back(std::move(Clip));
                }
            }
        }

        const FNpcifyMsgPackValue* AnimArr = MapVal.FindField("animations");
        if (AnimArr && AnimArr->IsArray())
        {
            for (const auto& Item : AnimArr->ArrayValue)
            {
                if (Item.IsMap())
                {
                    FStagingAnimationClip Clip;
                    Clip.Id = GetStringField(Item, "id");
                    Clip.Animation = GetStringField(Item, "animation");
                    Clip.StartTime = GetDoubleField(Item, "startTime", 0.0);
                    Clip.EndTime = GetDoubleField(Item, "endTime", 0.0);
                    Clip.bLoop = GetBoolField(Item, "loop", false);
                    Out.Animations.push_back(std::move(Clip));
                }
            }
        }

        return Out;
    }

    FStagingDialogueBlock FNpcifySchemaMapper::MapDialogueBlock(const FNpcifyMsgPackValue& MapVal)
    {
        FStagingDialogueBlock Block;
        Block.Id = GetStringField(MapVal, "id");
        Block.Type = GetStringField(MapVal, "type", "speech");
        Block.StartTime = GetDoubleField(MapVal, "startTime", 0.0);
        Block.EndTime = GetDoubleField(MapVal, "endTime", Block.StartTime);
        Block.Text = GetStringField(MapVal, "text");
        Block.Duration = GetDoubleField(MapVal, "duration", std::max(0.0, Block.EndTime - Block.StartTime));

        const FNpcifyMsgPackValue* AudioMap = MapVal.FindField("audio");
        if (AudioMap && AudioMap->IsMap())
        {
            Block.Audio.Type = GetStringField(*AudioMap, "type", "silent");
            Block.Audio.MimeType = GetStringField(*AudioMap, "mimeType", "audio/mpeg");
            Block.Audio.Url = GetStringField(*AudioMap, "url");
            Block.Audio.StreamId = GetStringField(*AudioMap, "streamId");
            Block.Audio.Duration = GetDoubleField(*AudioMap, "duration", Block.Duration);

            const FNpcifyMsgPackValue* DataVal = AudioMap->FindField("data");
            if (DataVal)
            {
                if (DataVal->IsBinary())
                {
                    Block.Audio.Data = DataVal->BinaryValue;
                }
                else if (DataVal->IsString())
                {
                    Block.Audio.Data = FNpcifyProtocol::DecodeBase64(DataVal->StringValue);
                    if (Block.Audio.Type == "base64")
                    {
                        Block.Audio.Type = "bytes";
                    }
                }
            }
        }

        const FNpcifyMsgPackValue* VisemesArr = MapVal.FindField("visemes");
        if (VisemesArr && VisemesArr->IsArray())
        {
            for (const auto& Item : VisemesArr->ArrayValue)
            {
                if (Item.IsMap())
                {
                    // The server sends { name, timestamp, endTime?, weight? }.
                    FStagingVisemeCue Cue;
                    Cue.Viseme = GetStringField(Item, "name");
                    if (Cue.Viseme.empty()) Cue.Viseme = GetStringField(Item, "viseme");
                    Cue.Time = GetDoubleField(Item, "timestamp", GetDoubleField(Item, "time", 0.0));
                    const double CueEnd = GetDoubleField(Item, "endTime", -1.0);
                    Cue.Duration = CueEnd > Cue.Time ? CueEnd - Cue.Time : GetDoubleField(Item, "duration", 0.08);
                    Cue.Weight = GetDoubleField(Item, "weight", 1.0);
                    Block.Visemes.push_back(std::move(Cue));
                }
            }
        }

        const FNpcifyMsgPackValue* WordsArr = MapVal.FindField("wordTimings");
        if (!WordsArr) WordsArr = MapVal.FindField("words");
        if (WordsArr && WordsArr->IsArray())
        {
            for (const auto& Item : WordsArr->ArrayValue)
            {
                if (Item.IsMap())
                {
                    FStagingWordCue Cue;
                    Cue.Word = GetStringField(Item, "word");
                    Cue.StartTime = GetDoubleField(Item, "startTime", 0.0);
                    Cue.EndTime = GetDoubleField(Item, "endTime", 0.0);
                    Block.Words.push_back(std::move(Cue));
                }
            }
        }

        const FNpcifyMsgPackValue* CharsArr = MapVal.FindField("characterTimings");
        if (!CharsArr) CharsArr = MapVal.FindField("characters");
        if (CharsArr && CharsArr->IsArray())
        {
            for (const auto& Item : CharsArr->ArrayValue)
            {
                if (Item.IsMap())
                {
                    FStagingCharacterCue Cue;
                    Cue.Character = GetStringField(Item, "character");
                    Cue.StartTime = GetDoubleField(Item, "startTime", 0.0);
                    Cue.EndTime = GetDoubleField(Item, "endTime", 0.0);
                    Block.Characters.push_back(std::move(Cue));
                }
            }
        }

        return Block;
    }

    FBakedConversationAsset FNpcifySchemaMapper::MapBakedConversationAsset(const FNpcifyMsgPackValue& MapVal)
    {
        RequireMap(MapVal, "baked conversation");
        FBakedConversationAsset Asset;
        Asset.Format = GetStringField(MapVal, "format");
        Asset.Version = static_cast<int>(GetIntField(MapVal, "version", 0));
        Asset.AvatarId = GetStringField(MapVal, "avatarId");
        Asset.Revision = static_cast<int>(GetIntField(MapVal, "revision", 1));
        Asset.RootStagingId = GetStringField(MapVal, "rootStagingId");

        const FNpcifyMsgPackValue* StagingsArr = MapVal.FindField("stagings");
        if (StagingsArr && StagingsArr->IsArray())
        {
            for (const auto& Item : StagingsArr->ArrayValue)
            {
                if (Item.IsMap())
                {
                    const FNpcifyMsgPackValue* PayloadMap = Item.FindField("payload");
                    if (!PayloadMap || !PayloadMap->IsMap())
                    {
                        throw std::runtime_error("Baked conversation node is missing payload.");
                    }
                    FBakedConversationNode Node;
                    Node.Id = GetStringField(Item, "id");
                    Node.ParentId = GetStringField(Item, "parentId");
                    Node.TriggerText = GetStringField(Item, "triggerText");
                    Node.Payload = MapStagingResultPayload(*PayloadMap);
                    Asset.Stagings.push_back(std::move(Node));
                }
            }
        }
        return Asset;
    }

    FNpcifyMsgPackValue FNpcifySchemaMapper::ToMsgPack(const FStagingResultPayload& Payload)
    {
        std::vector<FNpcifyMsgPackValue> DialogueList;
        for (const auto& Block : Payload.Timeline.Dialogue)
        {
            std::map<std::string, FNpcifyMsgPackValue> AudioMap;
            AudioMap["type"] = FNpcifyMsgPackValue::MakeString(Block.Audio.Type);
            AudioMap["mimeType"] = FNpcifyMsgPackValue::MakeString(Block.Audio.MimeType);
            AudioMap["duration"] = FNpcifyMsgPackValue::MakeFloat(Block.Audio.Duration);
            if (!Block.Audio.Data.empty())
            {
                AudioMap["data"] = FNpcifyMsgPackValue::MakeBinary(Block.Audio.Data);
            }

            std::vector<FNpcifyMsgPackValue> Visemes;
            for (const auto& V : Block.Visemes)
            {
                std::map<std::string, FNpcifyMsgPackValue> VMap;
                VMap["viseme"] = FNpcifyMsgPackValue::MakeString(V.Viseme);
                VMap["time"] = FNpcifyMsgPackValue::MakeFloat(V.Time);
                VMap["duration"] = FNpcifyMsgPackValue::MakeFloat(V.Duration);
                VMap["weight"] = FNpcifyMsgPackValue::MakeFloat(V.Weight);
                Visemes.push_back(FNpcifyMsgPackValue::MakeMap(std::move(VMap)));
            }

            std::vector<FNpcifyMsgPackValue> Chars;
            for (const auto& C : Block.Characters)
            {
                std::map<std::string, FNpcifyMsgPackValue> CMap;
                CMap["character"] = FNpcifyMsgPackValue::MakeString(C.Character);
                CMap["startTime"] = FNpcifyMsgPackValue::MakeFloat(C.StartTime);
                CMap["endTime"] = FNpcifyMsgPackValue::MakeFloat(C.EndTime);
                Chars.push_back(FNpcifyMsgPackValue::MakeMap(std::move(CMap)));
            }

            std::map<std::string, FNpcifyMsgPackValue> BMap;
            BMap["id"] = FNpcifyMsgPackValue::MakeString(Block.Id);
            BMap["type"] = FNpcifyMsgPackValue::MakeString(Block.Type);
            BMap["startTime"] = FNpcifyMsgPackValue::MakeFloat(Block.StartTime);
            BMap["endTime"] = FNpcifyMsgPackValue::MakeFloat(Block.EndTime);
            BMap["text"] = FNpcifyMsgPackValue::MakeString(Block.Text);
            BMap["duration"] = FNpcifyMsgPackValue::MakeFloat(Block.Duration);
            BMap["audio"] = FNpcifyMsgPackValue::MakeMap(std::move(AudioMap));
            BMap["visemes"] = FNpcifyMsgPackValue::MakeArray(std::move(Visemes));
            BMap["characters"] = FNpcifyMsgPackValue::MakeArray(std::move(Chars));
            DialogueList.push_back(FNpcifyMsgPackValue::MakeMap(std::move(BMap)));
        }

        std::vector<FNpcifyMsgPackValue> ExprList;
        for (const auto& E : Payload.Timeline.Expressions)
        {
            std::map<std::string, FNpcifyMsgPackValue> EMap;
            EMap["id"] = FNpcifyMsgPackValue::MakeString(E.Id);
            EMap["expression"] = FNpcifyMsgPackValue::MakeString(E.Expression);
            EMap["startTime"] = FNpcifyMsgPackValue::MakeFloat(E.StartTime);
            EMap["endTime"] = FNpcifyMsgPackValue::MakeFloat(E.EndTime);
            EMap["weight"] = FNpcifyMsgPackValue::MakeFloat(E.Weight);
            ExprList.push_back(FNpcifyMsgPackValue::MakeMap(std::move(EMap)));
        }

        std::vector<FNpcifyMsgPackValue> AnimList;
        for (const auto& A : Payload.Timeline.Animations)
        {
            std::map<std::string, FNpcifyMsgPackValue> AMap;
            AMap["id"] = FNpcifyMsgPackValue::MakeString(A.Id);
            AMap["animation"] = FNpcifyMsgPackValue::MakeString(A.Animation);
            AMap["startTime"] = FNpcifyMsgPackValue::MakeFloat(A.StartTime);
            AMap["endTime"] = FNpcifyMsgPackValue::MakeFloat(A.EndTime);
            AMap["loop"] = FNpcifyMsgPackValue::MakeBool(A.bLoop);
            AnimList.push_back(FNpcifyMsgPackValue::MakeMap(std::move(AMap)));
        }

        std::map<std::string, FNpcifyMsgPackValue> TimelineMap;
        TimelineMap["duration"] = FNpcifyMsgPackValue::MakeFloat(Payload.Timeline.Duration);
        TimelineMap["dialogue"] = FNpcifyMsgPackValue::MakeArray(std::move(DialogueList));
        TimelineMap["expressions"] = FNpcifyMsgPackValue::MakeArray(std::move(ExprList));
        TimelineMap["animations"] = FNpcifyMsgPackValue::MakeArray(std::move(AnimList));

        std::vector<FNpcifyMsgPackValue> OptionsList;
        for (const auto& Opt : Payload.Options)
        {
            std::map<std::string, FNpcifyMsgPackValue> OptMap;
            OptMap["label"] = FNpcifyMsgPackValue::MakeString(Opt.Label);
            OptMap["end"] = FNpcifyMsgPackValue::MakeBool(Opt.bEnd);
            OptionsList.push_back(FNpcifyMsgPackValue::MakeMap(std::move(OptMap)));
        }

        std::map<std::string, FNpcifyMsgPackValue> Out;
        Out["protocolVersion"] = FNpcifyMsgPackValue::MakeInt(Payload.ProtocolVersion);
        Out["taskId"] = FNpcifyMsgPackValue::MakeString(Payload.TaskId);
        Out["stagingId"] = FNpcifyMsgPackValue::MakeString(Payload.StagingId);
        Out["text"] = FNpcifyMsgPackValue::MakeString(Payload.Text);
        Out["timeline"] = FNpcifyMsgPackValue::MakeMap(std::move(TimelineMap));
        Out["options"] = FNpcifyMsgPackValue::MakeArray(std::move(OptionsList));
        return FNpcifyMsgPackValue::MakeMap(std::move(Out));
    }
}
