#include "Core/NpcifySpeechInput.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Npcify
{
    namespace
    {
        constexpr float MinSpeechLevel = 0.015f;
        constexpr float SpeechToNoise = 3.0f;
        constexpr float SpeechStartSeconds = 0.2f;

        void WriteUint32(std::vector<uint8_t>& Out, uint32_t Value)
        {
            for (int Shift = 0; Shift < 32; Shift += 8)
            {
                Out.push_back(static_cast<uint8_t>((Value >> Shift) & 0xffu));
            }
        }

        void WriteUint16(std::vector<uint8_t>& Out, uint16_t Value)
        {
            Out.push_back(static_cast<uint8_t>(Value & 0xffu));
            Out.push_back(static_cast<uint8_t>((Value >> 8) & 0xffu));
        }

        void WriteTag(std::vector<uint8_t>& Out, const char* Tag)
        {
            Out.insert(Out.end(), Tag, Tag + 4);
        }

        int16_t ToPcm16(double Value)
        {
            const double Clamped = std::max(-1.0, std::min(1.0, Value));
            return static_cast<int16_t>(std::lround(Clamped < 0.0 ? Clamped * 32768.0 : Clamped * 32767.0));
        }
    }

    const char* InputModeName(EInputMode Mode)
    {
        switch (Mode)
        {
            case EInputMode::Options:
                return "options";
            case EInputMode::Text:
                return "text";
            default:
                return "voice";
        }
    }

    std::vector<EInputMode> PermittedInputModes(bool bAllowCustomReply, bool bBaked, bool bSpeechInputEnabled)
    {
        if (bBaked || !bAllowCustomReply)
        {
            return {EInputMode::Options};
        }
        if (!bSpeechInputEnabled)
        {
            return {EInputMode::Options, EInputMode::Text};
        }
        return {EInputMode::Options, EInputMode::Text, EInputMode::Voice};
    }

    bool HasInputMode(const std::vector<EInputMode>& Modes, EInputMode Mode)
    {
        return std::find(Modes.begin(), Modes.end(), Mode) != Modes.end();
    }

    std::vector<EInputMode> ValidateInputModes(const std::vector<EInputMode>& Configured)
    {
        std::vector<EInputMode> Modes;
        for (const EInputMode Mode : Configured)
        {
            if (HasInputMode(Modes, Mode))
            {
                throw std::invalid_argument(std::string("InputModes has \"") + InputModeName(Mode) + "\" two times.");
            }
            Modes.push_back(Mode);
        }
        return Modes;
    }

    std::vector<EInputMode> ResolveInputModes(
        const std::vector<EInputMode>& Configured,
        bool bAllowCustomReply,
        bool bBaked,
        bool bSpeechInputEnabled
    )
    {
        const std::vector<EInputMode> Modes = ValidateInputModes(Configured);
        const std::vector<EInputMode> Permitted = PermittedInputModes(bAllowCustomReply, bBaked, bSpeechInputEnabled);
        if (Modes.empty())
        {
            return Permitted;
        }
        if (bBaked && !(Modes.size() == 1 && Modes[0] == EInputMode::Options))
        {
            throw std::invalid_argument(
                "A baked (offline) conversation accepts only reply options. Set InputModes to Options or leave it empty."
            );
        }
        std::string Refused;
        for (const EInputMode Mode : Modes)
        {
            if (!HasInputMode(Permitted, Mode))
            {
                Refused += Refused.empty() ? "" : " and ";
                Refused += std::string("\"") + InputModeName(Mode) + "\"";
            }
        }
        if (!Refused.empty())
        {
            throw std::invalid_argument(
                std::string(
                    bAllowCustomReply
                        ? "Voice input is off for this character (its owner turns it on in the STT tab). Remove "
                        : "This character accepts only its reply options (Allow custom replies is off). Remove "
                ) +
                Refused + " from InputModes."
            );
        }
        return Modes;
    }

    std::string CheckInputMode(const std::vector<EInputMode>& Modes, EInputMode Mode, const char* Reason)
    {
        if (HasInputMode(Modes, Mode))
        {
            return std::string();
        }
        std::string Names;
        for (const EInputMode Entry : Modes)
        {
            Names += Names.empty() ? "" : ", ";
            Names += InputModeName(Entry);
        }
        return std::string(Reason) + ": InputModes is [" + Names + "].";
    }

    namespace SpeechAudio
    {
        std::vector<int16_t> ToPcm16Mono16k(const float* Samples, size_t Count, int32_t Channels, int32_t InputRate)
        {
            std::vector<int16_t> Output;
            if (Samples == nullptr || Channels <= 0 || InputRate <= 0)
            {
                return Output;
            }
            const size_t Frames = Count / static_cast<size_t>(Channels);
            const double Ratio = static_cast<double>(InputRate) / SampleRate;
            Output.reserve(static_cast<size_t>(static_cast<double>(Frames) / Ratio) + 1);
            double Sum = 0.0;
            int32_t Summed = 0;
            double Phase = 0.0;
            for (size_t Frame = 0; Frame < Frames; ++Frame)
            {
                double Value = 0.0;
                for (int32_t Channel = 0; Channel < Channels; ++Channel)
                {
                    Value += Samples[Frame * static_cast<size_t>(Channels) + static_cast<size_t>(Channel)];
                }
                Sum += Value / Channels;
                ++Summed;
                Phase += 1.0;
                while (Phase >= Ratio)
                {
                    Phase -= Ratio;
                    Output.push_back(ToPcm16(Sum / std::max(1, Summed)));
                    if (Ratio >= 1.0)
                    {
                        Sum = 0.0;
                        Summed = 0;
                    }
                }
                if (Ratio < 1.0)
                {
                    Sum = 0.0;
                    Summed = 0;
                }
            }
            if (Summed > 0 && Ratio >= 1.0)
            {
                Output.push_back(ToPcm16(Sum / Summed));
            }
            if (Output.size() > static_cast<size_t>(MaxSamples))
            {
                Output.resize(static_cast<size_t>(MaxSamples));
            }
            return Output;
        }

        std::vector<uint8_t> EncodeWav(const std::vector<int16_t>& Pcm)
        {
            const uint32_t DataBytes = static_cast<uint32_t>(Pcm.size() * 2);
            std::vector<uint8_t> Out;
            Out.reserve(44 + DataBytes);
            WriteTag(Out, "RIFF");
            WriteUint32(Out, 36 + DataBytes);
            WriteTag(Out, "WAVE");
            WriteTag(Out, "fmt ");
            WriteUint32(Out, 16);
            WriteUint16(Out, 1);
            WriteUint16(Out, 1);
            WriteUint32(Out, static_cast<uint32_t>(SampleRate));
            WriteUint32(Out, static_cast<uint32_t>(SampleRate * 2));
            WriteUint16(Out, 2);
            WriteUint16(Out, 16);
            WriteTag(Out, "data");
            WriteUint32(Out, DataBytes);
            for (const int16_t Sample : Pcm)
            {
                WriteUint16(Out, static_cast<uint16_t>(Sample));
            }
            return Out;
        }

        float Level(const float* Samples, size_t Count)
        {
            if (Samples == nullptr || Count == 0)
            {
                return 0.0f;
            }
            double Sum = 0.0;
            for (size_t Index = 0; Index < Count; ++Index)
            {
                Sum += static_cast<double>(Samples[Index]) * Samples[Index];
            }
            return static_cast<float>(std::sqrt(Sum / static_cast<double>(Count)));
        }
    }

    FNpcifySilenceDetector::FNpcifySilenceDetector(float InSilenceSeconds)
        : SilenceSeconds(InSilenceSeconds)
    {
    }

    bool FNpcifySilenceDetector::Push(float Level, float FrameSeconds)
    {
        const float CurrentFloor = Floor < 0.0f ? MinSpeechLevel / SpeechToNoise : Floor;
        const float Threshold = std::max(MinSpeechLevel, CurrentFloor * SpeechToNoise);
        if (Level >= Threshold)
        {
            LoudSeconds += FrameSeconds;
            QuietSeconds = 0.0f;
            if (LoudSeconds >= SpeechStartSeconds - 1e-4f)
            {
                bHeardSpeech = true;
            }
            return false;
        }
        Floor = Floor < 0.0f ? Level : Floor * 0.9f + Level * 0.1f;
        LoudSeconds = 0.0f;
        if (!bHeardSpeech)
        {
            return false;
        }
        QuietSeconds += FrameSeconds;
        return QuietSeconds >= SilenceSeconds - 1e-4f;
    }
}
