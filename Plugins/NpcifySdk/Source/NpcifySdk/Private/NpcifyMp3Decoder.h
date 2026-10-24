#pragma once

#include "CoreMinimal.h"

namespace NpcifyAudio
{
    /** Decodes a complete MP3 file (for example ElevenLabs speech) to interleaved 16-bit PCM. */
    bool DecodeMp3(const uint8* Data, int32 Size, TArray<int16>& OutPcm, int32& OutSampleRate, int32& OutChannels);

    /** True when the bytes start with an ID3 tag or an MPEG audio frame header. */
    bool LooksLikeMp3(const uint8* Data, int32 Size);
}
