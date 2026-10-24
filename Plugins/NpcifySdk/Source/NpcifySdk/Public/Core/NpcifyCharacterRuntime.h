#pragma once

#include "Core/NpcifyBakedConversation.h"
#include "Core/NpcifyFacialMixer.h"
#include "Core/NpcifyModels.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Npcify
{
    struct FNpcifyDecodedAudio
    {
        std::vector<float> Samples;
        std::vector<int16_t> Pcm16Samples;
        int SampleRate = 24000;
        int Channels = 1;

        double GetDurationSeconds() const
        {
            return (SampleRate > 0 && Channels > 0)
                ? static_cast<double>(Samples.size()) / static_cast<double>(SampleRate * Channels)
                : 0.0;
        }
    };

    class FNpcifyStagingPlayer
    {
    public:
        FNpcifyStagingPlayer(FNpcifyFacialMixer& InFacialMixer, FNpcifyVisemePlayer& InVisemePlayer);

        std::function<void(const FStagingDialogueBlock&)> OnDialogueBlockStarted;
        std::function<void(const FStagingDialogueBlock&)> OnDialogueBlockEnded;
        std::function<void(const std::string&, const FStagingCharacterCue&)> OnCharacterRevealed;
        std::function<void(const FStagingExpressionClip&)> OnExpressionClipTriggered;
        std::function<void(const FStagingAnimationClip&)> OnAnimationClipTriggered;
        std::function<void(const FStagingResultPayload&)> OnStagingCompleted;

        bool IsPlaying() const { return bIsPlaying; }
        double GetTimeSeconds() const { return CurrentTimeSeconds; }
        double GetDurationSeconds() const { return TotalDurationSeconds; }
        const FStagingResultPayload* GetCurrentStaging() const { return bHasStaging ? &ActiveStaging : nullptr; }
        const FStagingDialogueBlock* GetCurrentBlock() const;
        const std::string& GetCurrentText() const { return CurrentText; }
        const std::string& GetRevealedText() const { return RevealedText; }
        FNpcifyPlaybackCursor GetCursor() const;

        void Play(const FStagingResultPayload& Staging);
        void Stop();
        void Tick(double DeltaTimeSeconds, float FallbackAudioRms = 0.0f);

    private:
        FNpcifyFacialMixer& FacialMixer;
        FNpcifyVisemePlayer& VisemePlayer;
        std::set<std::string> TriggeredExpressionIds;
        std::set<std::string> TriggeredAnimationIds;

        bool bHasStaging = false;
        FStagingResultPayload ActiveStaging;
        int ActiveBlockIndex = -1;
        int NextCharIndex = 0;
        double CurrentTimeSeconds = 0.0;
        double TotalDurationSeconds = 0.0;
        bool bIsPlaying = false;
        std::string CurrentText;
        std::string RevealedText;
    };

    class FNpcifyCharacterRuntime
    {
    public:
        explicit FNpcifyCharacterRuntime(const FNpcifyOptions& InOptions);

        std::function<void(const FStagingResultPayload&)> OnSpeechStart;
        std::function<void(const FStagingResultPayload&)> OnSpeechEnd;
        std::function<void(const FStagingDialogueBlock&, const FNpcifyDecodedAudio&)> OnAudioBlock;
        std::function<void(const std::string&, const FStagingCharacterCue&)> OnCharacterRevealed;
        std::function<void(const std::string&, bool)> OnAnimationPlayRequested;
        std::function<void(const std::string&)> OnError;

        const FNpcifyOptions& GetOptions() const { return Options; }
        FNpcifyFacialMixer& GetExpressions() { return Expressions; }
        const FNpcifyFacialMixer& GetExpressions() const { return Expressions; }
        FNpcifyVisemePlayer& GetVisemePlayer() { return VisemePlayer; }
        FNpcifyStagingPlayer& GetStagingPlayer() { return StagingPlayer; }

        const std::string& GetConversationId() const { return ConversationId; }
        void SetConversationId(const std::string& InId) { ConversationId = InId; }
        const std::string& GetActiveStagingId() const { return ActiveStagingId; }
        void SetActiveStagingId(const std::string& InId) { ActiveStagingId = InId; }
        const std::string& GetCurrentAnimation() const { return CurrentAnimation; }

        float GetVolume() const { return Volume; }
        void SetVolume(float InVolume);
        bool IsMuted() const { return bMuted; }
        void SetMuted(bool bInMuted) { bMuted = bInMuted; }
        bool IsThinking() const { return bThinking; }
        void SetThinking(bool bInThinking) { bThinking = bInThinking; }

        FNpcifySpeechState GetSpeechState() const;

        bool HasBakedConversation() const { return BakedConversation != nullptr; }
        void AttachBakedConversation(std::unique_ptr<FNpcifyBakedConversation> InBaked);
        FStagingResultPayload StartBakedConversation(const std::string& StagingId = "");
        FStagingResultPayload SelectBakedStaging(const std::string& StagingId);
        FBakedReplyResult ReplyBaked(const std::string& OptionLabel, const std::string& ParentStagingId = "");

        void PlayStaging(const FStagingResultPayload& Staging);
        /** Forgets the conversation IDs and the reply options of the last staging. */
        void ResetConversationState();
        void StopSpeaking();
        void PlayAnimation(const std::string& AnimationId, bool bLoop = false);

        void LookAt(float YawDegrees, float PitchDegrees);
        void LookAtDirection(float LocalX, float LocalY, float LocalZ);
        void ClearLookAt();

        void Tick(double DeltaTimeSeconds, float FallbackAudioRms = 0.0f);

        /** True for WAV or raw PCM bytes. Compressed audio (MPEG) is decoded by the engine integration. */
        static bool IsPcmAudio(const FStagingAudioSource& Audio);

        static FNpcifyDecodedAudio DecodeAudioBytes(
            const std::vector<uint8_t>& Bytes,
            int DefaultSampleRate = 24000,
            int DefaultChannels = 1
        );

        static FNpcifyMsgPackValue BuildCreateConversationRequest(
            const std::string& AvatarId,
            const std::string& RenderSessionId = ""
        );

        static FNpcifyMsgPackValue BuildCreateStagingRequest(
            const std::string& Message,
            const std::string& ParentStagingId = "",
            bool bSilent = false,
            int MaxOptions = 6
        );

    private:
        void HandleDialogueBlockStarted(const FStagingDialogueBlock& Block);

        FNpcifyOptions Options;
        FNpcifyFacialMixer Expressions;
        FNpcifyVisemePlayer VisemePlayer;
        FNpcifyStagingPlayer StagingPlayer;
        std::unique_ptr<FNpcifyBakedConversation> BakedConversation;

        std::string ConversationId;
        std::string ActiveStagingId;
        std::string CurrentAnimation = "idle";
        /** Options of the last played staging; they stay available after its speech ends. */
        std::vector<FStagingReplyOption> LastOptions;
        float Volume = 1.0f;
        bool bMuted = false;
        bool bThinking = false;
    };
}
