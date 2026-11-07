#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Voice input (docs/voice-input.md): the input modes and the speech audio of the SDK.
// Engine-independent C++17, tested with the other core files.
namespace Npcify
{
    /** How the player can reply. */
    enum class EInputMode : uint8_t
    {
        Options,
        Text,
        Voice,
    };

    const char* InputModeName(EInputMode Mode);

    /**
     * The modes that a character permits. Voice and typed text are custom replies; voice also
     * needs the owner's voice input (speechInputEnabled, the STT tab).
     */
    std::vector<EInputMode> PermittedInputModes(bool bAllowCustomReply, bool bBaked, bool bSpeechInputEnabled);

    /** Throws std::invalid_argument when a mode occurs two times. An empty list is no configuration. */
    std::vector<EInputMode> ValidateInputModes(const std::vector<EInputMode>& Configured);

    /**
     * The modes in use: the configured modes, or every mode that the character permits.
     * Throws std::invalid_argument when the character does not permit a configured mode:
     * a baked conversation permits only Options, a character without custom replies
     * permits only Options, and a character with voice input off does not permit Voice.
     */
    std::vector<EInputMode> ResolveInputModes(
        const std::vector<EInputMode>& Configured,
        bool bAllowCustomReply,
        bool bBaked,
        bool bSpeechInputEnabled
    );

    bool HasInputMode(const std::vector<EInputMode>& Modes, EInputMode Mode);

    /** An empty string when the modes include Mode; otherwise the reason, for an error message. */
    std::string CheckInputMode(const std::vector<EInputMode>& Modes, EInputMode Mode, const char* Reason);

    /** 16-bit mono PCM at 16 kHz in a WAV file: the format of the speech endpoint. */
    namespace SpeechAudio
    {
        constexpr int32_t SampleRate = 16000;
        constexpr float MaxDurationSeconds = 30.0f;
        constexpr int32_t MaxSamples = 480000;
        /** A shorter push-to-talk press sends nothing. */
        constexpr float MinHoldSeconds = 0.3f;

        /**
         * Interleaved float samples at any rate to 16 kHz mono 16-bit. Downsampling averages
         * the input samples of each output sample, as the web and Unity SDKs do.
         */
        std::vector<int16_t> ToPcm16Mono16k(const float* Samples, size_t Count, int32_t Channels, int32_t InputRate);

        std::vector<uint8_t> EncodeWav(const std::vector<int16_t>& Pcm);

        /** The loudness of float samples (RMS, 0 to 1). */
        float Level(const float* Samples, size_t Count);
    }

    /**
     * The end of speech for listening without a release (EndOnSilenceSeconds): it follows
     * the noise floor and ends after the given silence that follows at least 200 ms of
     * speech. Push-to-talk never uses it.
     */
    class FNpcifySilenceDetector
    {
    public:
        explicit FNpcifySilenceDetector(float InSilenceSeconds);

        /** True when the speech has ended. */
        bool Push(float Level, float FrameSeconds);

        bool HeardSpeech() const { return bHeardSpeech; }

    private:
        float SilenceSeconds = 0.0f;
        float Floor = -1.0f;
        float LoudSeconds = 0.0f;
        float QuietSeconds = 0.0f;
        bool bHeardSpeech = false;
    };
}
