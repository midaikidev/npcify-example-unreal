#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Npcify
{
    enum class ENpcifyPerformanceKind : uint8_t
    {
        Staging = 1,
        Stream = 2,
        Turn = 3,
        Conversation = 4,
        Baked = 5,
        BakedResult = 6,
        StoredTimeline = 7,
        CachedOpening = 8,
    };

    enum class ENpcifyMsgPackType : uint8_t
    {
        Null = 0,
        Bool = 1,
        Int = 2,
        Float = 3,
        String = 4,
        Binary = 5,
        Array = 6,
        Map = 7,
    };

    struct FNpcifyMsgPackValue
    {
        ENpcifyMsgPackType Type = ENpcifyMsgPackType::Null;
        bool BoolValue = false;
        int64_t IntValue = 0;
        double FloatValue = 0.0;
        std::string StringValue;
        std::vector<uint8_t> BinaryValue;
        std::vector<FNpcifyMsgPackValue> ArrayValue;
        std::map<std::string, FNpcifyMsgPackValue> MapValue;

        static FNpcifyMsgPackValue MakeNull();
        static FNpcifyMsgPackValue MakeBool(bool Value);
        static FNpcifyMsgPackValue MakeInt(int64_t Value);
        static FNpcifyMsgPackValue MakeFloat(double Value);
        static FNpcifyMsgPackValue MakeString(std::string Value);
        static FNpcifyMsgPackValue MakeBinary(std::vector<uint8_t> Value);
        static FNpcifyMsgPackValue MakeArray(std::vector<FNpcifyMsgPackValue> Value);
        static FNpcifyMsgPackValue MakeMap(std::map<std::string, FNpcifyMsgPackValue> Value);

        bool IsNull() const { return Type == ENpcifyMsgPackType::Null; }
        bool IsBool() const { return Type == ENpcifyMsgPackType::Bool; }
        bool IsInt() const { return Type == ENpcifyMsgPackType::Int; }
        bool IsFloat() const { return Type == ENpcifyMsgPackType::Float; }
        bool IsNumber() const { return Type == ENpcifyMsgPackType::Int || Type == ENpcifyMsgPackType::Float; }
        bool IsString() const { return Type == ENpcifyMsgPackType::String; }
        bool IsBinary() const { return Type == ENpcifyMsgPackType::Binary; }
        bool IsArray() const { return Type == ENpcifyMsgPackType::Array; }
        bool IsMap() const { return Type == ENpcifyMsgPackType::Map; }

        double AsDouble(double DefaultValue = 0.0) const;
        int64_t AsInt64(int64_t DefaultValue = 0) const;
        bool AsBool(bool DefaultValue = false) const;
        std::string AsString(const std::string& DefaultValue = "") const;
        const FNpcifyMsgPackValue* FindField(const std::string& Key) const;
    };

    class FNpcifyProtocol
    {
    public:
        static constexpr const char* MediaType = "application/vnd.npcify.performance";
        static constexpr const char* FileExtension = ".npcp";
        static constexpr size_t HeaderSize = 24;
        static constexpr size_t MaxBytes = 256 * 1024 * 1024;
        static constexpr int MaxDepth = 64;
        static constexpr int MaxTotalValues = 2000000;
        static constexpr size_t MaxStringBytes = 1024 * 1024;
        static constexpr size_t MaxBinaryBytes = 256 * 1024 * 1024;
        static constexpr size_t MaxArrayLength = 1000000;
        static constexpr size_t MaxMapLength = 10000;
        static constexpr int64_t MaxSafeInteger = 9007199254740991LL;

        static uint32_t Checksum(const uint8_t* Data, size_t Length);
        static uint32_t Checksum(const std::vector<uint8_t>& Bytes);

        static std::vector<uint8_t> EncodeMsgPack(const FNpcifyMsgPackValue& Value);
        static FNpcifyMsgPackValue DecodeMsgPack(const uint8_t* Data, size_t Length);
        static FNpcifyMsgPackValue DecodeMsgPack(const std::vector<uint8_t>& Bytes);

        static std::vector<uint8_t> EncodeNpcp(
            ENpcifyPerformanceKind Kind,
            const FNpcifyMsgPackValue& Value,
            bool bCompress = false
        );

        static FNpcifyMsgPackValue DecodeNpcp(
            const uint8_t* Data,
            size_t Length,
            ENpcifyPerformanceKind ExpectedKind
        );

        static FNpcifyMsgPackValue DecodeNpcp(
            const std::vector<uint8_t>& Bytes,
            ENpcifyPerformanceKind ExpectedKind
        );

        static std::vector<uint8_t> DecodeBase64(const std::string& Input);
    };
}
