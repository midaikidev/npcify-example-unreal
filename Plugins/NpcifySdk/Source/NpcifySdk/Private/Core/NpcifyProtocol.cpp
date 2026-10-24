#include "Core/NpcifyProtocol.h"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <zlib.h>

namespace Npcify
{
    namespace
    {
        constexpr uint32_t NpcpMagic = 0x5043504eU; // "NPCP" little-endian

        uint16_t ReadUInt16Le(const uint8_t* Data) { return static_cast<uint16_t>(Data[0]) | (static_cast<uint16_t>(Data[1]) << 8); }
        uint32_t ReadUInt32Le(const uint8_t* Data)
        {
            return static_cast<uint32_t>(Data[0]) | (static_cast<uint32_t>(Data[1]) << 8) |
                   (static_cast<uint32_t>(Data[2]) << 16) | (static_cast<uint32_t>(Data[3]) << 24);
        }
        void WriteUInt16Le(uint8_t* Out, uint16_t Value) { Out[0] = static_cast<uint8_t>(Value & 0xffU); Out[1] = static_cast<uint8_t>((Value >> 8) & 0xffU); }
        void WriteUInt32Le(uint8_t* Out, uint32_t Value)
        {
            Out[0] = static_cast<uint8_t>(Value & 0xffU); Out[1] = static_cast<uint8_t>((Value >> 8) & 0xffU);
            Out[2] = static_cast<uint8_t>((Value >> 16) & 0xffU); Out[3] = static_cast<uint8_t>((Value >> 24) & 0xffU);
        }
        bool IsReservedMapKey(const std::string& Key) { return Key == "__proto__" || Key == "constructor" || Key == "prototype"; }

        bool IsValidUtf8(const uint8_t* Bytes, size_t Length)
        {
            size_t Index = 0;
            while (Index < Length)
            {
                const uint8_t B0 = Bytes[Index];
                if (B0 < 0x80) { Index += 1; }
                else if (B0 >= 0xC2 && B0 <= 0xDF)
                {
                    if (Index + 1 >= Length || (Bytes[Index + 1] & 0xC0) != 0x80) return false;
                    Index += 2;
                }
                else if (B0 >= 0xE0 && B0 <= 0xEF)
                {
                    if (Index + 2 >= Length) return false;
                    const uint8_t B1 = Bytes[Index + 1], B2 = Bytes[Index + 2];
                    if ((B1 & 0xC0) != 0x80 || (B2 & 0xC0) != 0x80 || (B0 == 0xE0 && B1 < 0xA0) || (B0 == 0xED && B1 >= 0xA0)) return false;
                    Index += 3;
                }
                else if (B0 >= 0xF0 && B0 <= 0xF4)
                {
                    if (Index + 3 >= Length) return false;
                    const uint8_t B1 = Bytes[Index + 1], B2 = Bytes[Index + 2], B3 = Bytes[Index + 3];
                    if ((B1 & 0xC0) != 0x80 || (B2 & 0xC0) != 0x80 || (B3 & 0xC0) != 0x80 || (B0 == 0xF0 && B1 < 0x90) || (B0 == 0xF4 && B1 >= 0x90)) return false;
                    Index += 4;
                }
                else return false;
            }
            return true;
        }

        uint8_t ReadByte(const uint8_t* Data, size_t Length, size_t& Offset)
        {
            if (Offset >= Length) throw std::runtime_error("Truncated MessagePack performance.");
            return Data[Offset++];
        }
        uint16_t ReadBe16(const uint8_t* Data, size_t Length, size_t& Offset)
        {
            if (Offset + 2 > Length) throw std::runtime_error("Truncated MessagePack performance.");
            const uint16_t Val = (static_cast<uint16_t>(Data[Offset]) << 8) | Data[Offset + 1];
            Offset += 2; return Val;
        }
        uint32_t ReadBe32(const uint8_t* Data, size_t Length, size_t& Offset)
        {
            if (Offset + 4 > Length) throw std::runtime_error("Truncated MessagePack performance.");
            const uint32_t Val = (static_cast<uint32_t>(Data[Offset]) << 24) | (static_cast<uint32_t>(Data[Offset + 1]) << 16) |
                                 (static_cast<uint32_t>(Data[Offset + 2]) << 8) | static_cast<uint32_t>(Data[Offset + 3]);
            Offset += 4; return Val;
        }
        uint64_t ReadBe64(const uint8_t* Data, size_t Length, size_t& Offset)
        {
            if (Offset + 8 > Length) throw std::runtime_error("Truncated MessagePack performance.");
            uint64_t Val = 0;
            for (int i = 0; i < 8; ++i) Val = (Val << 8) | Data[Offset + i];
            Offset += 8; return Val;
        }
        void WriteBe16(std::vector<uint8_t>& Out, uint16_t Val) { Out.push_back(static_cast<uint8_t>((Val >> 8) & 0xffU)); Out.push_back(static_cast<uint8_t>(Val & 0xffU)); }
        void WriteBe32(std::vector<uint8_t>& Out, uint32_t Val)
        {
            Out.push_back(static_cast<uint8_t>((Val >> 24) & 0xffU)); Out.push_back(static_cast<uint8_t>((Val >> 16) & 0xffU));
            Out.push_back(static_cast<uint8_t>((Val >> 8) & 0xffU)); Out.push_back(static_cast<uint8_t>(Val & 0xffU));
        }
        void WriteBe64(std::vector<uint8_t>& Out, uint64_t Val) { for (int Shift = 56; Shift >= 0; Shift -= 8) Out.push_back(static_cast<uint8_t>((Val >> Shift) & 0xffU)); }

        std::string ReadMsgPackString(const uint8_t* Data, size_t Length, size_t& Offset, uint8_t Tag)
        {
            size_t ByteLen = 0;
            if (Tag >= 0xa0 && Tag <= 0xbf) ByteLen = Tag & 0x1fU;
            else if (Tag == 0xd9) ByteLen = ReadByte(Data, Length, Offset);
            else if (Tag == 0xda) ByteLen = ReadBe16(Data, Length, Offset);
            else if (Tag == 0xdb) ByteLen = ReadBe32(Data, Length, Offset);
            else throw std::runtime_error("Performance map keys must be strings.");

            if (ByteLen > FNpcifyProtocol::MaxStringBytes) throw std::runtime_error("Performance string exceeds the limit.");
            if (Offset + ByteLen > Length) throw std::runtime_error("Truncated MessagePack performance.");
            if (!IsValidUtf8(Data + Offset, ByteLen)) throw std::runtime_error("Invalid UTF-8 string in performance.");
            std::string Result(reinterpret_cast<const char*>(Data + Offset), ByteLen);
            Offset += ByteLen;
            return Result;
        }

        FNpcifyMsgPackValue NormalizeValue(const FNpcifyMsgPackValue& Input, int Depth)
        {
            if (Depth > FNpcifyProtocol::MaxDepth) throw std::runtime_error("Performance nesting exceeds the protocol limit.");
            if (Input.IsInt())
            {
                if (Input.IntValue < -FNpcifyProtocol::MaxSafeInteger || Input.IntValue > FNpcifyProtocol::MaxSafeInteger)
                    throw std::runtime_error("Performance integers must be exactly representable.");
                return Input;
            }
            if (Input.IsFloat())
            {
                if (!std::isfinite(Input.FloatValue)) throw std::runtime_error("Unsupported non-finite performance number.");
                if (std::trunc(Input.FloatValue) == Input.FloatValue &&
                    (Input.FloatValue < -static_cast<double>(FNpcifyProtocol::MaxSafeInteger) ||
                     Input.FloatValue > static_cast<double>(FNpcifyProtocol::MaxSafeInteger)))
                    throw std::runtime_error("Performance integers must be exactly representable.");
                return Input;
            }
            if (Input.IsArray())
            {
                std::vector<FNpcifyMsgPackValue> Normalized;
                Normalized.reserve(Input.ArrayValue.size());
                for (const auto& Item : Input.ArrayValue) Normalized.push_back(NormalizeValue(Item, Depth + 1));
                return FNpcifyMsgPackValue::MakeArray(std::move(Normalized));
            }
            if (Input.IsMap())
            {
                std::map<std::string, FNpcifyMsgPackValue> Normalized;
                for (const auto& Pair : Input.MapValue)
                {
                    if (IsReservedMapKey(Pair.first)) throw std::runtime_error("Invalid performance map key.");
                    Normalized.emplace(Pair.first, NormalizeValue(Pair.second, Depth + 1));
                }
                auto TypeIt = Normalized.find("type");
                auto DataIt = Normalized.find("data");
                if (TypeIt != Normalized.end() && TypeIt->second.IsString() &&
                    DataIt != Normalized.end() && DataIt->second.IsString())
                {
                    const std::string& TypeStr = TypeIt->second.StringValue;
                    if (TypeStr == "base64" || TypeStr == "speech_chunk")
                    {
                        Normalized["data"] = FNpcifyMsgPackValue::MakeBinary(FNpcifyProtocol::DecodeBase64(DataIt->second.StringValue));
                        if (TypeStr == "base64") Normalized["type"] = FNpcifyMsgPackValue::MakeString("bytes");
                    }
                }
                return FNpcifyMsgPackValue::MakeMap(std::move(Normalized));
            }
            return Input;
        }

        FNpcifyMsgPackValue ReadMsgPackNode(const uint8_t* Data, size_t Length, size_t& Offset, int Depth, int& TotalValues)
        {
            if (Depth > FNpcifyProtocol::MaxDepth || ++TotalValues > FNpcifyProtocol::MaxTotalValues)
                throw std::runtime_error("Performance nesting or value count exceeds the limit.");
            const uint8_t Tag = ReadByte(Data, Length, Offset);
            if (Tag < 0x80) return FNpcifyMsgPackValue::MakeInt(static_cast<int64_t>(Tag));
            if (Tag >= 0xe0) return FNpcifyMsgPackValue::MakeInt(static_cast<int64_t>(static_cast<int8_t>(Tag)));
            if (Tag == 0xc0) return FNpcifyMsgPackValue::MakeNull();
            if (Tag == 0xc2) return FNpcifyMsgPackValue::MakeBool(false);
            if (Tag == 0xc3) return FNpcifyMsgPackValue::MakeBool(true);
            if ((Tag >= 0xa0 && Tag <= 0xbf) || (Tag >= 0xd9 && Tag <= 0xdb))
                return FNpcifyMsgPackValue::MakeString(ReadMsgPackString(Data, Length, Offset, Tag));
            if ((Tag >= 0x80 && Tag <= 0x8f) || Tag == 0xde || Tag == 0xdf)
            {
                const size_t Count = (Tag <= 0x8f) ? (Tag & 0x0fU) : (Tag == 0xde ? ReadBe16(Data, Length, Offset) : ReadBe32(Data, Length, Offset));
                if (Count > FNpcifyProtocol::MaxMapLength) throw std::runtime_error("Performance map exceeds the limit.");
                std::map<std::string, FNpcifyMsgPackValue> MapVal;
                for (size_t i = 0; i < Count; ++i)
                {
                    if (++TotalValues > FNpcifyProtocol::MaxTotalValues) throw std::runtime_error("Performance value count exceeds the limit.");
                    std::string Key = ReadMsgPackString(Data, Length, Offset, ReadByte(Data, Length, Offset));
                    if (IsReservedMapKey(Key)) throw std::runtime_error("Invalid performance map key.");
                    if (MapVal.find(Key) != MapVal.end()) throw std::runtime_error("Duplicate performance map key.");
                    MapVal.emplace(std::move(Key), ReadMsgPackNode(Data, Length, Offset, Depth + 1, TotalValues));
                }
                return FNpcifyMsgPackValue::MakeMap(std::move(MapVal));
            }
            if ((Tag >= 0x90 && Tag <= 0x9f) || Tag == 0xdc || Tag == 0xdd)
            {
                const size_t Count = (Tag <= 0x9f) ? (Tag & 0x0fU) : (Tag == 0xdc ? ReadBe16(Data, Length, Offset) : ReadBe32(Data, Length, Offset));
                if (Count > FNpcifyProtocol::MaxArrayLength) throw std::runtime_error("Performance array exceeds the limit.");
                std::vector<FNpcifyMsgPackValue> ArrVal;
                ArrVal.reserve(Count);
                for (size_t i = 0; i < Count; ++i) ArrVal.push_back(ReadMsgPackNode(Data, Length, Offset, Depth + 1, TotalValues));
                return FNpcifyMsgPackValue::MakeArray(std::move(ArrVal));
            }
            if (Tag >= 0xc4 && Tag <= 0xc6)
            {
                const size_t ByteLen = (Tag == 0xc4) ? ReadByte(Data, Length, Offset) : (Tag == 0xc5 ? ReadBe16(Data, Length, Offset) : ReadBe32(Data, Length, Offset));
                if (ByteLen > FNpcifyProtocol::MaxBinaryBytes) throw std::runtime_error("Performance binary exceeds the limit.");
                if (Offset + ByteLen > Length) throw std::runtime_error("Truncated MessagePack performance.");
                std::vector<uint8_t> BinVal(Data + Offset, Data + Offset + ByteLen);
                Offset += ByteLen;
                return FNpcifyMsgPackValue::MakeBinary(std::move(BinVal));
            }
            if (Tag == 0xca)
            {
                const uint32_t Bits = ReadBe32(Data, Length, Offset);
                float FVal = 0.0f; std::memcpy(&FVal, &Bits, sizeof(float));
                if (!std::isfinite(FVal)) throw std::runtime_error("Unsupported non-finite performance float.");
                return FNpcifyMsgPackValue::MakeFloat(static_cast<double>(FVal));
            }
            if (Tag == 0xcb)
            {
                const uint64_t Bits = ReadBe64(Data, Length, Offset);
                double DVal = 0.0; std::memcpy(&DVal, &Bits, sizeof(double));
                if (!std::isfinite(DVal)) throw std::runtime_error("Unsupported non-finite performance number.");
                return FNpcifyMsgPackValue::MakeFloat(DVal);
            }
            if (Tag == 0xcc) return FNpcifyMsgPackValue::MakeInt(static_cast<int64_t>(ReadByte(Data, Length, Offset)));
            if (Tag == 0xcd) return FNpcifyMsgPackValue::MakeInt(static_cast<int64_t>(ReadBe16(Data, Length, Offset)));
            if (Tag == 0xce) return FNpcifyMsgPackValue::MakeInt(static_cast<int64_t>(ReadBe32(Data, Length, Offset)));
            if (Tag == 0xcf)
            {
                const uint64_t UVal = ReadBe64(Data, Length, Offset);
                if (UVal > static_cast<uint64_t>(FNpcifyProtocol::MaxSafeInteger)) throw std::runtime_error("Performance integers must be exactly representable.");
                return FNpcifyMsgPackValue::MakeInt(static_cast<int64_t>(UVal));
            }
            if (Tag == 0xd0) return FNpcifyMsgPackValue::MakeInt(static_cast<int64_t>(static_cast<int8_t>(ReadByte(Data, Length, Offset))));
            if (Tag == 0xd1) return FNpcifyMsgPackValue::MakeInt(static_cast<int64_t>(static_cast<int16_t>(ReadBe16(Data, Length, Offset))));
            if (Tag == 0xd2) return FNpcifyMsgPackValue::MakeInt(static_cast<int64_t>(static_cast<int32_t>(ReadBe32(Data, Length, Offset))));
            if (Tag == 0xd3)
            {
                const int64_t SVal = static_cast<int64_t>(ReadBe64(Data, Length, Offset));
                if (SVal < -FNpcifyProtocol::MaxSafeInteger || SVal > FNpcifyProtocol::MaxSafeInteger) throw std::runtime_error("Performance integers must be exactly representable.");
                return FNpcifyMsgPackValue::MakeInt(SVal);
            }
            throw std::runtime_error("Unsupported MessagePack performance tag.");
        }

        void WriteMsgPackString(std::vector<uint8_t>& Out, const std::string& Str)
        {
            const size_t Len = Str.size();
            if (Len > FNpcifyProtocol::MaxStringBytes) throw std::runtime_error("Performance string exceeds the limit.");
            if (Len <= 31) Out.push_back(static_cast<uint8_t>(0xa0U | Len));
            else if (Len <= 0xffU) { Out.push_back(0xd9U); Out.push_back(static_cast<uint8_t>(Len)); }
            else if (Len <= 0xffffU) { Out.push_back(0xdaU); WriteBe16(Out, static_cast<uint16_t>(Len)); }
            else { Out.push_back(0xdbU); WriteBe32(Out, static_cast<uint32_t>(Len)); }
            Out.insert(Out.end(), Str.begin(), Str.end());
        }

        void WriteMsgPackInt(std::vector<uint8_t>& Out, int64_t Val)
        {
            if (Val >= 0)
            {
                if (Val <= 0x7f) Out.push_back(static_cast<uint8_t>(Val));
                else if (Val <= 0xff) { Out.push_back(0xccU); Out.push_back(static_cast<uint8_t>(Val)); }
                else if (Val <= 0xffff) { Out.push_back(0xcdU); WriteBe16(Out, static_cast<uint16_t>(Val)); }
                else if (Val <= 0xffffffffLL) { Out.push_back(0xceU); WriteBe32(Out, static_cast<uint32_t>(Val)); }
                else { Out.push_back(0xcfU); WriteBe64(Out, static_cast<uint64_t>(Val)); }
            }
            else
            {
                if (Val >= -32) Out.push_back(static_cast<uint8_t>(Val & 0xff));
                else if (Val >= -128) { Out.push_back(0xd0U); Out.push_back(static_cast<uint8_t>(Val & 0xff)); }
                else if (Val >= -32768) { Out.push_back(0xd1U); WriteBe16(Out, static_cast<uint16_t>(Val & 0xffff)); }
                else if (Val >= -2147483648LL) { Out.push_back(0xd2U); WriteBe32(Out, static_cast<uint32_t>(Val & 0xffffffffLL)); }
                else { Out.push_back(0xd3U); WriteBe64(Out, static_cast<uint64_t>(Val)); }
            }
        }

        void WriteMsgPackNode(std::vector<uint8_t>& Out, const FNpcifyMsgPackValue& Val)
        {
            switch (Val.Type)
            {
                case ENpcifyMsgPackType::Null: Out.push_back(0xc0U); break;
                case ENpcifyMsgPackType::Bool: Out.push_back(Val.BoolValue ? 0xc3U : 0xc2U); break;
                case ENpcifyMsgPackType::Int: WriteMsgPackInt(Out, Val.IntValue); break;
                case ENpcifyMsgPackType::Float:
                {
                    if (std::trunc(Val.FloatValue) == Val.FloatValue &&
                        Val.FloatValue >= -static_cast<double>(FNpcifyProtocol::MaxSafeInteger) &&
                        Val.FloatValue <= static_cast<double>(FNpcifyProtocol::MaxSafeInteger))
                    {
                        WriteMsgPackInt(Out, static_cast<int64_t>(Val.FloatValue));
                        break;
                    }
                    Out.push_back(0xcbU);
                    uint64_t Bits = 0; std::memcpy(&Bits, &Val.FloatValue, sizeof(double));
                    WriteBe64(Out, Bits);
                    break;
                }
                case ENpcifyMsgPackType::String: WriteMsgPackString(Out, Val.StringValue); break;
                case ENpcifyMsgPackType::Binary:
                {
                    const size_t Len = Val.BinaryValue.size();
                    if (Len <= 0xffU) { Out.push_back(0xc4U); Out.push_back(static_cast<uint8_t>(Len)); }
                    else if (Len <= 0xffffU) { Out.push_back(0xc5U); WriteBe16(Out, static_cast<uint16_t>(Len)); }
                    else { Out.push_back(0xc6U); WriteBe32(Out, static_cast<uint32_t>(Len)); }
                    Out.insert(Out.end(), Val.BinaryValue.begin(), Val.BinaryValue.end());
                    break;
                }
                case ENpcifyMsgPackType::Array:
                {
                    const size_t Len = Val.ArrayValue.size();
                    if (Len <= 15) Out.push_back(static_cast<uint8_t>(0x90U | Len));
                    else if (Len <= 0xffffU) { Out.push_back(0xdcU); WriteBe16(Out, static_cast<uint16_t>(Len)); }
                    else { Out.push_back(0xddU); WriteBe32(Out, static_cast<uint32_t>(Len)); }
                    for (const auto& Item : Val.ArrayValue) WriteMsgPackNode(Out, Item);
                    break;
                }
                case ENpcifyMsgPackType::Map:
                {
                    const size_t Len = Val.MapValue.size();
                    if (Len <= 15) Out.push_back(static_cast<uint8_t>(0x80U | Len));
                    else if (Len <= 0xffffU) { Out.push_back(0xdeU); WriteBe16(Out, static_cast<uint16_t>(Len)); }
                    else { Out.push_back(0xdfU); WriteBe32(Out, static_cast<uint32_t>(Len)); }
                    for (const auto& Pair : Val.MapValue) { WriteMsgPackString(Out, Pair.first); WriteMsgPackNode(Out, Pair.second); }
                    break;
                }
            }
        }

        std::vector<uint8_t> GzipCompressBytes(const std::vector<uint8_t>& Plain)
        {
            z_stream Stream{};
            if (deflateInit2(&Stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
                throw std::runtime_error("Failed to initialize gzip compressor.");
            Stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(Plain.data()));
            Stream.avail_in = static_cast<uInt>(Plain.size());
            std::vector<uint8_t> Output(deflateBound(&Stream, static_cast<uLong>(Plain.size())) + 32);
            Stream.next_out = reinterpret_cast<Bytef*>(Output.data());
            Stream.avail_out = static_cast<uInt>(Output.size());
            const int Status = deflate(&Stream, Z_FINISH);
            const size_t TotalOut = Stream.total_out;
            deflateEnd(&Stream);
            if (Status != Z_STREAM_END) throw std::runtime_error("Failed to compress gzip performance.");
            Output.resize(TotalOut);
            return Output;
        }

        std::vector<uint8_t> GzipDecompressBounded(const uint8_t* Data, size_t Length, size_t ExpectedLength)
        {
            z_stream Stream{};
            if (inflateInit2(&Stream, 15 + 16) != Z_OK) throw std::runtime_error("Failed to initialize gzip decompressor.");
            Stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(Data));
            Stream.avail_in = static_cast<uInt>(Length);
            std::vector<uint8_t> Output(ExpectedLength);
            Stream.next_out = reinterpret_cast<Bytef*>(Output.data());
            Stream.avail_out = static_cast<uInt>(ExpectedLength);
            const int Status = inflate(&Stream, Z_FINISH);
            const size_t TotalOut = Stream.total_out;
            inflateEnd(&Stream);
            if (Status != Z_STREAM_END || TotalOut != ExpectedLength)
                throw std::runtime_error("Truncated or oversized compressed performance.");
            return Output;
        }
    }

    FNpcifyMsgPackValue FNpcifyMsgPackValue::MakeNull() { return FNpcifyMsgPackValue{}; }
    FNpcifyMsgPackValue FNpcifyMsgPackValue::MakeBool(bool Value) { FNpcifyMsgPackValue Out; Out.Type = ENpcifyMsgPackType::Bool; Out.BoolValue = Value; return Out; }
    FNpcifyMsgPackValue FNpcifyMsgPackValue::MakeInt(int64_t Value) { FNpcifyMsgPackValue Out; Out.Type = ENpcifyMsgPackType::Int; Out.IntValue = Value; return Out; }
    FNpcifyMsgPackValue FNpcifyMsgPackValue::MakeFloat(double Value) { FNpcifyMsgPackValue Out; Out.Type = ENpcifyMsgPackType::Float; Out.FloatValue = Value; return Out; }
    FNpcifyMsgPackValue FNpcifyMsgPackValue::MakeString(std::string Value) { FNpcifyMsgPackValue Out; Out.Type = ENpcifyMsgPackType::String; Out.StringValue = std::move(Value); return Out; }
    FNpcifyMsgPackValue FNpcifyMsgPackValue::MakeBinary(std::vector<uint8_t> Value) { FNpcifyMsgPackValue Out; Out.Type = ENpcifyMsgPackType::Binary; Out.BinaryValue = std::move(Value); return Out; }
    FNpcifyMsgPackValue FNpcifyMsgPackValue::MakeArray(std::vector<FNpcifyMsgPackValue> Value) { FNpcifyMsgPackValue Out; Out.Type = ENpcifyMsgPackType::Array; Out.ArrayValue = std::move(Value); return Out; }
    FNpcifyMsgPackValue FNpcifyMsgPackValue::MakeMap(std::map<std::string, FNpcifyMsgPackValue> Value) { FNpcifyMsgPackValue Out; Out.Type = ENpcifyMsgPackType::Map; Out.MapValue = std::move(Value); return Out; }

    double FNpcifyMsgPackValue::AsDouble(double DefaultValue) const
    {
        if (Type == ENpcifyMsgPackType::Float) return FloatValue;
        if (Type == ENpcifyMsgPackType::Int) return static_cast<double>(IntValue);
        return DefaultValue;
    }
    int64_t FNpcifyMsgPackValue::AsInt64(int64_t DefaultValue) const
    {
        if (Type == ENpcifyMsgPackType::Int) return IntValue;
        if (Type == ENpcifyMsgPackType::Float) return static_cast<int64_t>(FloatValue);
        return DefaultValue;
    }
    bool FNpcifyMsgPackValue::AsBool(bool DefaultValue) const { return Type == ENpcifyMsgPackType::Bool ? BoolValue : DefaultValue; }
    std::string FNpcifyMsgPackValue::AsString(const std::string& DefaultValue) const { return Type == ENpcifyMsgPackType::String ? StringValue : DefaultValue; }
    const FNpcifyMsgPackValue* FNpcifyMsgPackValue::FindField(const std::string& Key) const
    {
        if (Type != ENpcifyMsgPackType::Map) return nullptr;
        auto It = MapValue.find(Key);
        return It != MapValue.end() ? &It->second : nullptr;
    }

    uint32_t FNpcifyProtocol::Checksum(const uint8_t* Data, size_t Length)
    {
        uint32_t Crc = 0xffffffffU;
        for (size_t i = 0; i < Length; ++i)
        {
            uint32_t Value = (Crc ^ Data[i]) & 0xffU;
            for (int Bit = 0; Bit < 8; ++Bit) Value = (Value >> 1) ^ ((Value & 1U) ? 0xedb88320U : 0U);
            Crc = Value ^ (Crc >> 8);
        }
        return Crc ^ 0xffffffffU;
    }

    uint32_t FNpcifyProtocol::Checksum(const std::vector<uint8_t>& Bytes) { return Checksum(Bytes.data(), Bytes.size()); }

    std::vector<uint8_t> FNpcifyProtocol::EncodeMsgPack(const FNpcifyMsgPackValue& Value)
    {
        const FNpcifyMsgPackValue Normalized = NormalizeValue(Value, 0);
        std::vector<uint8_t> Out;
        WriteMsgPackNode(Out, Normalized);
        return Out;
    }

    FNpcifyMsgPackValue FNpcifyProtocol::DecodeMsgPack(const uint8_t* Data, size_t Length)
    {
        if (!Data || Length == 0) throw std::runtime_error("Truncated MessagePack performance.");
        size_t Offset = 0;
        int TotalValues = 0;
        FNpcifyMsgPackValue Decoded = ReadMsgPackNode(Data, Length, Offset, 0, TotalValues);
        if (Offset != Length) throw std::runtime_error("Trailing MessagePack performance data.");
        return NormalizeValue(Decoded, 0);
    }

    FNpcifyMsgPackValue FNpcifyProtocol::DecodeMsgPack(const std::vector<uint8_t>& Bytes) { return DecodeMsgPack(Bytes.data(), Bytes.size()); }

    std::vector<uint8_t> FNpcifyProtocol::EncodeNpcp(ENpcifyPerformanceKind Kind, const FNpcifyMsgPackValue& Value, bool bCompress)
    {
        const std::vector<uint8_t> Plain = EncodeMsgPack(Value);
        if (Plain.empty() || Plain.size() > MaxBytes) throw std::runtime_error("Performance exceeds the protocol size limit.");
        const std::vector<uint8_t> Body = bCompress ? GzipCompressBytes(Plain) : Plain;
        std::vector<uint8_t> Result(HeaderSize + Body.size(), 0);
        WriteUInt32Le(Result.data(), NpcpMagic);
        Result[4] = 1; Result[5] = 0; Result[6] = static_cast<uint8_t>(Kind); Result[7] = bCompress ? 1 : 0;
        WriteUInt16Le(Result.data() + 8, static_cast<uint16_t>(HeaderSize));
        WriteUInt16Le(Result.data() + 10, 0);
        WriteUInt32Le(Result.data() + 12, static_cast<uint32_t>(Body.size()));
        WriteUInt32Le(Result.data() + 16, static_cast<uint32_t>(Plain.size()));
        WriteUInt32Le(Result.data() + 20, Checksum(Plain));
        std::memcpy(Result.data() + HeaderSize, Body.data(), Body.size());
        return Result;
    }

    FNpcifyMsgPackValue FNpcifyProtocol::DecodeNpcp(const uint8_t* Data, size_t Length, ENpcifyPerformanceKind ExpectedKind)
    {
        if (!Data || Length < HeaderSize) throw std::runtime_error("Truncated performance header.");
        if (ReadUInt32Le(Data) != NpcpMagic || Data[4] != 1) throw std::runtime_error("Unsupported performance format or major version.");
        if (Data[6] != static_cast<uint8_t>(ExpectedKind)) throw std::runtime_error("Unexpected performance message kind.");
        const uint8_t Compression = Data[7];
        const uint16_t HdrSize = ReadUInt16Le(Data + 8);
        const uint16_t Flags = ReadUInt16Le(Data + 10);
        const uint32_t BodySize = ReadUInt32Le(Data + 12);
        const uint32_t PlainSize = ReadUInt32Le(Data + 16);
        const uint32_t ExpectedCrc = ReadUInt32Le(Data + 20);

        if (Compression > 1 || Flags != 0 || HdrSize < HeaderSize || HdrSize > Length ||
            BodySize != static_cast<uint32_t>(Length - HdrSize) ||
            BodySize > MaxBytes || PlainSize > MaxBytes || PlainSize == 0)
        {
            throw std::runtime_error("Invalid performance header or size.");
        }

        std::vector<uint8_t> Plain;
        if (Compression == 1)
        {
            if (BodySize < 18 || Data[HdrSize] != 31 || Data[HdrSize + 1] != 139 || Data[HdrSize + 2] != 8)
                throw std::runtime_error("Invalid gzip performance.");
            const size_t TrailerOffset = HdrSize + BodySize - 8;
            if (ReadUInt32Le(Data + TrailerOffset) != ExpectedCrc || ReadUInt32Le(Data + TrailerOffset + 4) != PlainSize)
                throw std::runtime_error("Performance gzip checksum or size mismatch.");
            Plain = GzipDecompressBounded(Data + HdrSize, BodySize, PlainSize);
        }
        else Plain.assign(Data + HdrSize, Data + HdrSize + BodySize);

        if (Plain.size() != PlainSize || Checksum(Plain) != ExpectedCrc)
            throw std::runtime_error("Performance checksum or length mismatch.");
        return DecodeMsgPack(Plain);
    }

    FNpcifyMsgPackValue FNpcifyProtocol::DecodeNpcp(const std::vector<uint8_t>& Bytes, ENpcifyPerformanceKind ExpectedKind)
    {
        return DecodeNpcp(Bytes.data(), Bytes.size(), ExpectedKind);
    }

    std::vector<uint8_t> FNpcifyProtocol::DecodeBase64(const std::string& Input)
    {
        static const int8_t Table[256] = {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
        };
        std::vector<uint8_t> Out;
        uint32_t Accum = 0;
        int Bits = 0;
        for (unsigned char Ch : Input)
        {
            const int8_t Val = Table[Ch];
            if (Val == -1) continue;
            if (Val == -2) break;
            Accum = (Accum << 6) | static_cast<uint32_t>(Val);
            Bits += 6;
            if (Bits >= 8) { Bits -= 8; Out.push_back(static_cast<uint8_t>((Accum >> Bits) & 0xffU)); }
        }
        return Out;
    }
}
