#include "NpcifyMp3Decoder.h"

THIRD_PARTY_INCLUDES_START
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "ThirdParty/minimp3/minimp3_ex.h"
THIRD_PARTY_INCLUDES_END

namespace NpcifyAudio
{
    bool DecodeMp3(const uint8* Data, int32 Size, TArray<int16>& OutPcm, int32& OutSampleRate, int32& OutChannels)
    {
        OutPcm.Reset();
        if (Data == nullptr || Size <= 0)
        {
            return false;
        }

        mp3dec_t Decoder;
        mp3dec_file_info_t Info;
        FMemory::Memzero(Info);
        if (mp3dec_load_buf(&Decoder, Data, static_cast<size_t>(Size), &Info, nullptr, nullptr) != 0 || Info.samples == 0)
        {
            if (Info.buffer)
            {
                free(Info.buffer);
            }
            return false;
        }

        OutPcm.Append(reinterpret_cast<const int16*>(Info.buffer), static_cast<int32>(Info.samples));
        OutSampleRate = Info.hz;
        OutChannels = FMath::Max(1, Info.channels);
        free(Info.buffer);
        return true;
    }

    bool LooksLikeMp3(const uint8* Data, int32 Size)
    {
        if (Data == nullptr || Size < 3)
        {
            return false;
        }
        const bool bId3 = Data[0] == 'I' && Data[1] == 'D' && Data[2] == '3';
        const bool bFrameSync = Data[0] == 0xFF && (Data[1] & 0xE0) == 0xE0;
        return bId3 || bFrameSync;
    }
}
