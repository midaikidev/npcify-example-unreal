#include "Core/NpcifyJson.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace Npcify
{
    namespace
    {
        constexpr int MaxDepth = 64;

        class FReader
        {
        public:
            explicit FReader(const std::string& InJson) : Json(InJson) {}

            FNpcifyMsgPackValue ReadDocument()
            {
                FNpcifyMsgPackValue Value = ReadValue(0);
                SkipSpace();
                if (Index != Json.size())
                {
                    Fail("The JSON text has data after its value.");
                }
                return Value;
            }

        private:
            [[noreturn]] static void Fail(const char* Message)
            {
                throw std::runtime_error(Message);
            }

            void SkipSpace()
            {
                while (Index < Json.size() &&
                    (Json[Index] == ' ' || Json[Index] == '\t' || Json[Index] == '\n' || Json[Index] == '\r'))
                {
                    ++Index;
                }
            }

            FNpcifyMsgPackValue ReadValue(int Depth)
            {
                if (Depth > MaxDepth)
                {
                    Fail("The JSON text is nested too deeply.");
                }
                SkipSpace();
                if (Index >= Json.size())
                {
                    Fail("The JSON text ended before a value.");
                }
                switch (Json[Index])
                {
                case '{': return ReadObject(Depth);
                case '[': return ReadArray(Depth);
                case '"': return FNpcifyMsgPackValue::MakeString(ReadString());
                case 't': ReadWord("true"); return FNpcifyMsgPackValue::MakeBool(true);
                case 'f': ReadWord("false"); return FNpcifyMsgPackValue::MakeBool(false);
                case 'n': ReadWord("null"); return FNpcifyMsgPackValue::MakeNull();
                default: return ReadNumber();
                }
            }

            FNpcifyMsgPackValue ReadObject(int Depth)
            {
                std::map<std::string, FNpcifyMsgPackValue> Fields;
                ++Index;
                SkipSpace();
                if (Index < Json.size() && Json[Index] == '}')
                {
                    ++Index;
                    return FNpcifyMsgPackValue::MakeMap(std::move(Fields));
                }
                while (true)
                {
                    SkipSpace();
                    if (Index >= Json.size() || Json[Index] != '"')
                    {
                        Fail("A JSON object key must be a string.");
                    }
                    std::string Key = ReadString();
                    SkipSpace();
                    if (Index >= Json.size() || Json[Index] != ':')
                    {
                        Fail("A JSON object key must have a colon.");
                    }
                    ++Index;
                    Fields[Key] = ReadValue(Depth + 1);
                    SkipSpace();
                    if (Index < Json.size() && Json[Index] == ',')
                    {
                        ++Index;
                        continue;
                    }
                    if (Index < Json.size() && Json[Index] == '}')
                    {
                        ++Index;
                        return FNpcifyMsgPackValue::MakeMap(std::move(Fields));
                    }
                    Fail("A JSON object must end with a brace.");
                }
            }

            FNpcifyMsgPackValue ReadArray(int Depth)
            {
                std::vector<FNpcifyMsgPackValue> Items;
                ++Index;
                SkipSpace();
                if (Index < Json.size() && Json[Index] == ']')
                {
                    ++Index;
                    return FNpcifyMsgPackValue::MakeArray(std::move(Items));
                }
                while (true)
                {
                    Items.push_back(ReadValue(Depth + 1));
                    SkipSpace();
                    if (Index < Json.size() && Json[Index] == ',')
                    {
                        ++Index;
                        continue;
                    }
                    if (Index < Json.size() && Json[Index] == ']')
                    {
                        ++Index;
                        return FNpcifyMsgPackValue::MakeArray(std::move(Items));
                    }
                    Fail("A JSON array must end with a bracket.");
                }
            }

            static void AppendUtf8(std::string& Out, unsigned Code)
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
                else if (Code < 0x10000)
                {
                    Out.push_back(static_cast<char>(0xE0 | (Code >> 12)));
                    Out.push_back(static_cast<char>(0x80 | ((Code >> 6) & 0x3F)));
                    Out.push_back(static_cast<char>(0x80 | (Code & 0x3F)));
                }
                else
                {
                    Out.push_back(static_cast<char>(0xF0 | (Code >> 18)));
                    Out.push_back(static_cast<char>(0x80 | ((Code >> 12) & 0x3F)));
                    Out.push_back(static_cast<char>(0x80 | ((Code >> 6) & 0x3F)));
                    Out.push_back(static_cast<char>(0x80 | (Code & 0x3F)));
                }
            }

            unsigned ReadHex4()
            {
                if (Index + 4 > Json.size())
                {
                    Fail("A JSON string has a bad unicode escape.");
                }
                unsigned Code = 0;
                for (int Digit = 0; Digit < 4; ++Digit)
                {
                    const char C = Json[Index++];
                    Code <<= 4;
                    if (C >= '0' && C <= '9') Code |= static_cast<unsigned>(C - '0');
                    else if (C >= 'a' && C <= 'f') Code |= static_cast<unsigned>(C - 'a' + 10);
                    else if (C >= 'A' && C <= 'F') Code |= static_cast<unsigned>(C - 'A' + 10);
                    else Fail("A JSON string has a bad unicode escape.");
                }
                return Code;
            }

            std::string ReadString()
            {
                std::string Out;
                ++Index;
                while (Index < Json.size())
                {
                    const char C = Json[Index++];
                    if (C == '"')
                    {
                        return Out;
                    }
                    if (C != '\\')
                    {
                        Out.push_back(C);
                        continue;
                    }
                    if (Index >= Json.size())
                    {
                        break;
                    }
                    const char Escape = Json[Index++];
                    switch (Escape)
                    {
                    case '"': case '\\': case '/': Out.push_back(Escape); break;
                    case 'b': Out.push_back('\b'); break;
                    case 'f': Out.push_back('\f'); break;
                    case 'n': Out.push_back('\n'); break;
                    case 'r': Out.push_back('\r'); break;
                    case 't': Out.push_back('\t'); break;
                    case 'u':
                    {
                        unsigned Code = ReadHex4();
                        // A surrogate pair is one code point.
                        if (Code >= 0xD800 && Code <= 0xDBFF && Index + 6 <= Json.size() &&
                            Json[Index] == '\\' && Json[Index + 1] == 'u')
                        {
                            Index += 2;
                            const unsigned Low = ReadHex4();
                            if (Low >= 0xDC00 && Low <= 0xDFFF)
                            {
                                Code = 0x10000 + ((Code - 0xD800) << 10) + (Low - 0xDC00);
                            }
                            else
                            {
                                AppendUtf8(Out, Code);
                                Code = Low;
                            }
                        }
                        AppendUtf8(Out, Code);
                        break;
                    }
                    default: Fail("A JSON string has a bad escape.");
                    }
                }
                Fail("A JSON string has no end quote.");
            }

            FNpcifyMsgPackValue ReadNumber()
            {
                const size_t Start = Index;
                bool bWhole = true;
                while (Index < Json.size())
                {
                    const char C = Json[Index];
                    if (C == '.' || C == 'e' || C == 'E')
                    {
                        bWhole = false;
                    }
                    else if (!(C == '-' || C == '+' || (C >= '0' && C <= '9')))
                    {
                        break;
                    }
                    ++Index;
                }
                if (Index == Start)
                {
                    Fail("The JSON text has a value that is not valid.");
                }
                const std::string Text = Json.substr(Start, Index - Start);
                char* End = nullptr;
                errno = 0;
                if (bWhole)
                {
                    const long long Whole = std::strtoll(Text.c_str(), &End, 10);
                    if (End == Text.c_str() + Text.size() && errno == 0)
                    {
                        return FNpcifyMsgPackValue::MakeInt(static_cast<int64_t>(Whole));
                    }
                    errno = 0;
                }
                const double Real = std::strtod(Text.c_str(), &End);
                if (End != Text.c_str() + Text.size() || !std::isfinite(Real))
                {
                    Fail("The JSON text has a value that is not valid.");
                }
                return FNpcifyMsgPackValue::MakeFloat(Real);
            }

            void ReadWord(const char* Word)
            {
                const std::string Expected(Word);
                if (Json.compare(Index, Expected.size(), Expected) != 0)
                {
                    Fail("The JSON text has a value that is not valid.");
                }
                Index += Expected.size();
            }

            const std::string& Json;
            size_t Index = 0;
        };
    }

    FNpcifyMsgPackValue FNpcifyJson::Parse(const std::string& Json)
    {
        return FReader(Json).ReadDocument();
    }

    bool FNpcifyJson::TryParse(const std::string& Json, FNpcifyMsgPackValue& Out)
    {
        try
        {
            Out = Parse(Json);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }
}
