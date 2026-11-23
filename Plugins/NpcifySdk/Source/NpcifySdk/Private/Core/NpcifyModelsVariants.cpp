#include "Core/NpcifyModels.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Npcify
{
    namespace
    {
        const FNpcifyMsgPackValue& RequireObjectMap(const FNpcifyMsgPackValue& Val, const char* Context)
        {
            if (!Val.IsMap())
            {
                throw std::runtime_error(std::string("Expected map for ") + Context + ".");
            }
            return Val;
        }

        std::string ReadString(
            const FNpcifyMsgPackValue& MapVal,
            const std::string& Key,
            const std::string& DefaultVal = ""
        )
        {
            const FNpcifyMsgPackValue* Field = MapVal.FindField(Key);
            return Field && Field->IsString() ? Field->StringValue : DefaultVal;
        }

        double ReadDouble(
            const FNpcifyMsgPackValue& MapVal,
            const std::string& Key,
            double DefaultVal = 0.0
        )
        {
            const FNpcifyMsgPackValue* Field = MapVal.FindField(Key);
            return Field ? Field->AsDouble(DefaultVal) : DefaultVal;
        }

        int64_t ReadInt(
            const FNpcifyMsgPackValue& MapVal,
            const std::string& Key,
            int64_t DefaultVal = 0
        )
        {
            const FNpcifyMsgPackValue* Field = MapVal.FindField(Key);
            return Field ? Field->AsInt64(DefaultVal) : DefaultVal;
        }

        bool ReadBool(
            const FNpcifyMsgPackValue& MapVal,
            const std::string& Key,
            bool DefaultVal = false
        )
        {
            const FNpcifyMsgPackValue* Field = MapVal.FindField(Key);
            return Field ? Field->AsBool(DefaultVal) : DefaultVal;
        }

        std::vector<FStagingReplyOption> ReadOptions(const FNpcifyMsgPackValue& MapVal)
        {
            std::vector<FStagingReplyOption> Options;
            const FNpcifyMsgPackValue* OptionsArr = MapVal.FindField("options");
            if (OptionsArr && OptionsArr->IsArray())
            {
                for (const auto& Item : OptionsArr->ArrayValue)
                {
                    if (Item.IsMap())
                    {
                        FStagingReplyOption Opt;
                        Opt.Label = ReadString(Item, "label");
                        Opt.bEnd = ReadBool(Item, "end", false);
                        Options.push_back(std::move(Opt));
                    }
                }
            }
            return Options;
        }
    }

    FNpcifyStagingToolCall FNpcifySchemaMapper::MapToolCall(const FNpcifyMsgPackValue& MapVal)
    {
        RequireObjectMap(MapVal, "staging tool call");
        FNpcifyStagingToolCall Call;
        Call.Id = ReadString(MapVal, "id");
        Call.Name = ReadString(MapVal, "name", "switch_variant");
        Call.Timestamp = std::max(0.0, ReadDouble(MapVal, "timestamp", 0.0));

        const FNpcifyMsgPackValue* Args = MapVal.FindField("arguments");
        if (Args && Args->IsMap())
        {
            Call.Variant = ReadString(*Args, "variant", FNpcifyProtocol::DefaultVariantKey);
        }
        else
        {
            Call.Variant = ReadString(MapVal, "variant", FNpcifyProtocol::DefaultVariantKey);
        }

        const FNpcifyMsgPackValue* Res = MapVal.FindField("result");
        if (Res && Res->IsMap())
        {
            Call.Result.bOk = ReadBool(*Res, "ok", false);
            Call.Result.Status = ReadString(*Res, "status", Call.Result.bOk ? "applied" : "rejected");
            Call.Result.PreviousVariant = ReadString(*Res, "previousVariant", FNpcifyProtocol::DefaultVariantKey);
            Call.Result.ActiveVariant = ReadString(*Res, "activeVariant", Call.Result.PreviousVariant);
            Call.Result.Error = ReadString(*Res, "error");
        }
        return Call;
    }

    FNpcifyMsgPackValue FNpcifySchemaMapper::ToolCallToMsgPack(const FNpcifyStagingToolCall& Call)
    {
        std::map<std::string, FNpcifyMsgPackValue> ArgsMap;
        ArgsMap["variant"] = FNpcifyMsgPackValue::MakeString(Call.Variant);

        std::map<std::string, FNpcifyMsgPackValue> ResMap;
        ResMap["ok"] = FNpcifyMsgPackValue::MakeBool(Call.Result.bOk);
        ResMap["status"] = FNpcifyMsgPackValue::MakeString(Call.Result.Status);
        ResMap["previousVariant"] = FNpcifyMsgPackValue::MakeString(Call.Result.PreviousVariant);
        ResMap["activeVariant"] = FNpcifyMsgPackValue::MakeString(Call.Result.ActiveVariant);
        if (!Call.Result.Error.empty())
        {
            ResMap["error"] = FNpcifyMsgPackValue::MakeString(Call.Result.Error);
        }

        std::map<std::string, FNpcifyMsgPackValue> Out;
        Out["id"] = FNpcifyMsgPackValue::MakeString(Call.Id);
        Out["name"] = FNpcifyMsgPackValue::MakeString(Call.Name);
        Out["timestamp"] = FNpcifyMsgPackValue::MakeFloat(Call.Timestamp);
        Out["arguments"] = FNpcifyMsgPackValue::MakeMap(std::move(ArgsMap));
        Out["result"] = FNpcifyMsgPackValue::MakeMap(std::move(ResMap));
        return FNpcifyMsgPackValue::MakeMap(std::move(Out));
    }

    FNpcifyAvatarInfo FNpcifySchemaMapper::MapAvatarInfo(const FNpcifyMsgPackValue& MapVal)
    {
        RequireObjectMap(MapVal, "avatar info");
        FNpcifyAvatarInfo Info;
        Info.Id = ReadString(MapVal, "id");
        Info.Name = ReadString(MapVal, "name");
        Info.IdleAnimationId = ReadString(MapVal, "idleAnimationId", "idle");
        Info.bAllowCustomReply = ReadBool(MapVal, "allowCustomReply", true);
        Info.bSpeechInputEnabled = ReadBool(MapVal, "speechInputEnabled", false);
        Info.bBakedConversationEnabled = ReadBool(MapVal, "bakedConversationEnabled", false);
        Info.BakedConversationRevision = static_cast<int>(ReadInt(MapVal, "bakedConversationRevision", 0));
        Info.bRealtimeMotionEnabled = ReadBool(MapVal, "realtimeMotionEnabled", false);
        Info.bSdkMemoryEnabled = ReadBool(MapVal, "sdkMemoryEnabled", false);

        const FNpcifyMsgPackValue* Sight = MapVal.FindField("sight");
        if (Sight && Sight->IsMap())
        {
            Info.bSightEnabled = ReadBool(*Sight, "enabled", false);
            Info.SightProvider = ReadString(*Sight, "provider");
        }

        const FNpcifyMsgPackValue* AnimsArr = MapVal.FindField("animations");
        if (AnimsArr && AnimsArr->IsArray())
        {
            for (const auto& Item : AnimsArr->ArrayValue)
            {
                if (Item.IsMap())
                {
                    FNpcifyAvatarAnimationInfo Anim;
                    Anim.Id = ReadString(Item, "id");
                    Anim.Name = ReadString(Item, "name", Anim.Id);
                    Anim.bLoop = ReadBool(Item, "loop", false);
                    if (!Anim.Id.empty())
                    {
                        Info.Animations.push_back(std::move(Anim));
                    }
                }
            }
        }

        const FNpcifyMsgPackValue* VariantsArr = MapVal.FindField("variants");
        if (VariantsArr && VariantsArr->IsArray())
        {
            for (const auto& Item : VariantsArr->ArrayValue)
            {
                if (Item.IsMap())
                {
                    FNpcifyVariantSummary Variant;
                    Variant.Key = ReadString(Item, "key", FNpcifyProtocol::DefaultVariantKey);
                    Variant.Label = ReadString(Item, "label", Variant.Key);
                    Variant.Description = ReadString(Item, "description");
                    Variant.bR18 = ReadBool(Item, "r18", false);
                    Variant.PortraitUrl = ReadString(Item, "portraitUrl");
                    Variant.CreditText = ReadString(Item, "creditText");
                    if (FNpcifyProtocol::IsValidVariantKey(Variant.Key))
                    {
                        Info.Variants.push_back(std::move(Variant));
                    }
                }
            }
        }
        return Info;
    }

    FConversationWorldline FNpcifySchemaMapper::MapWorldline(const FNpcifyMsgPackValue& MapVal)
    {
        RequireObjectMap(MapVal, "conversation worldline");
        FConversationWorldline Out;
        Out.ConversationId = ReadString(MapVal, "conversationId");
        Out.OpeningStagingId = ReadString(MapVal, "openingStagingId");
        Out.ActiveStagingId = ReadString(MapVal, "activeStagingId");

        const FNpcifyMsgPackValue* StagingsArr = MapVal.FindField("stagings");
        if (StagingsArr && StagingsArr->IsArray())
        {
            for (const auto& Item : StagingsArr->ArrayValue)
            {
                if (Item.IsMap())
                {
                    FConversationWorldlineNode Node;
                    Node.Id = ReadString(Item, "id");
                    Node.ParentId = ReadString(Item, "parentId");
                    Node.TriggerText = ReadString(Item, "triggerText");
                    Node.Text = ReadString(Item, "text");
                    Node.Options = ReadOptions(Item);
                    Node.CreatedAt = ReadString(Item, "createdAt", "1970-01-01T00:00:00.000Z");
                    Node.InitialVariant = ReadString(Item, "initialVariant");
                    Node.ActiveVariant = ReadString(Item, "activeVariant");

                    const FNpcifyMsgPackValue* CallsArr = Item.FindField("toolCalls");
                    if (CallsArr && CallsArr->IsArray())
                    {
                        for (const auto& CallItem : CallsArr->ArrayValue)
                        {
                            if (CallItem.IsMap())
                            {
                                Node.ToolCalls.push_back(MapToolCall(CallItem));
                            }
                        }
                    }
                    Out.Stagings.push_back(std::move(Node));
                }
            }
        }
        return Out;
    }
}
