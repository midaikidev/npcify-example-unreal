#pragma once

#include "Core/NpcifyProtocol.h"

#include <string>

namespace Npcify
{
    /**
     * A small JSON reader for the events of the conversation stream. Objects become maps, arrays
     * arrays, numbers floats (whole numbers that fit an int64 become ints), and true, false and
     * null their values.
     */
    class FNpcifyJson
    {
    public:
        /** The value of a JSON text. Throws std::runtime_error when the text is not valid JSON. */
        static FNpcifyMsgPackValue Parse(const std::string& Json);
        /** False when the text is not valid JSON. */
        static bool TryParse(const std::string& Json, FNpcifyMsgPackValue& Out);
    };
}
