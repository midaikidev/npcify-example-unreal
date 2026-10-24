#include "Core/NpcifyCharacterRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace Npcify
{
    FNpcifyStagingPlayer::FNpcifyStagingPlayer(
        FNpcifyFacialMixer& InFacialMixer,
        FNpcifyVisemePlayer& InVisemePlayer
    )
        : FacialMixer(InFacialMixer)
        , VisemePlayer(InVisemePlayer)
    {
    }

    const FStagingDialogueBlock* FNpcifyStagingPlayer::GetCurrentBlock() const
    {
        if (!bHasStaging || ActiveBlockIndex < 0 ||
            static_cast<size_t>(ActiveBlockIndex) >= ActiveStaging.Timeline.Dialogue.size())
        {
            return nullptr;
        }
        return &ActiveStaging.Timeline.Dialogue[static_cast<size_t>(ActiveBlockIndex)];
    }

    FNpcifyPlaybackCursor FNpcifyStagingPlayer::GetCursor() const
    {
        FNpcifyPlaybackCursor Out;
        Out.TimeSeconds = CurrentTimeSeconds;
        Out.DurationSeconds = TotalDurationSeconds;
        const FStagingDialogueBlock* Block = GetCurrentBlock();
        Out.BlockId = Block ? Block->Id : "";
        return Out;
    }

    void FNpcifyStagingPlayer::Play(const FStagingResultPayload& Staging)
    {
        Stop();
        bHasStaging = true;
        ActiveStaging = Staging;
        CurrentTimeSeconds = 0.0;
        bIsPlaying = true;
        CurrentText = Staging.Text;
        RevealedText.clear();

        double ComputedEnd = Staging.Timeline.Duration;
        for (const auto& B : Staging.Timeline.Dialogue) ComputedEnd = std::max(ComputedEnd, B.EndTime);
        for (const auto& E : Staging.Timeline.Expressions) ComputedEnd = std::max(ComputedEnd, E.EndTime);
        for (const auto& A : Staging.Timeline.Animations) ComputedEnd = std::max(ComputedEnd, A.EndTime);
        TotalDurationSeconds = std::max(0.01, ComputedEnd);

        Tick(0.0);
    }

    void FNpcifyStagingPlayer::Stop()
    {
        const FStagingDialogueBlock* Prev = GetCurrentBlock();
        if (bIsPlaying && Prev && OnDialogueBlockEnded)
        {
            OnDialogueBlockEnded(*Prev);
        }
        bHasStaging = false;
        ActiveBlockIndex = -1;
        NextCharIndex = 0;
        CurrentTimeSeconds = 0.0;
        TotalDurationSeconds = 0.0;
        bIsPlaying = false;
        TriggeredExpressionIds.clear();
        TriggeredAnimationIds.clear();
        VisemePlayer.Clear();
        FacialMixer.ClearVisemes();
    }

    void FNpcifyStagingPlayer::Tick(double DeltaTimeSeconds, float FallbackAudioRms)
    {
        if (!bIsPlaying || !bHasStaging)
        {
            FacialMixer.Update(static_cast<float>(std::max(0.0, DeltaTimeSeconds)));
            return;
        }

        const double Dt = std::max(0.0, DeltaTimeSeconds);
        CurrentTimeSeconds += Dt;

        const auto& Timeline = ActiveStaging.Timeline;
        for (size_t i = 0; i < Timeline.Expressions.size(); ++i)
        {
            const auto& Clip = Timeline.Expressions[i];
            const std::string Key = !Clip.Id.empty() ? Clip.Id : ("expr-" + std::to_string(i));
            if (CurrentTimeSeconds >= Clip.StartTime && TriggeredExpressionIds.insert(Key).second)
            {
                const float Dur = static_cast<float>(std::max(0.2, Clip.EndTime - Clip.StartTime));
                FacialMixer.PlayCue(Clip.Expression, static_cast<float>(Clip.Weight), Dur);
                if (OnExpressionClipTriggered) OnExpressionClipTriggered(Clip);
            }
        }

        for (size_t i = 0; i < Timeline.Animations.size(); ++i)
        {
            const auto& Clip = Timeline.Animations[i];
            const std::string Key = !Clip.Id.empty() ? Clip.Id : ("anim-" + std::to_string(i));
            if (CurrentTimeSeconds >= Clip.StartTime && TriggeredAnimationIds.insert(Key).second)
            {
                if (OnAnimationClipTriggered) OnAnimationClipTriggered(Clip);
            }
        }

        int NewBlockIdx = -1;
        for (size_t i = 0; i < Timeline.Dialogue.size(); ++i)
        {
            const auto& B = Timeline.Dialogue[i];
            if (CurrentTimeSeconds >= B.StartTime && CurrentTimeSeconds <= B.EndTime)
            {
                NewBlockIdx = static_cast<int>(i);
                break;
            }
        }

        if (NewBlockIdx != ActiveBlockIndex)
        {
            if (ActiveBlockIndex >= 0 && static_cast<size_t>(ActiveBlockIndex) < Timeline.Dialogue.size())
            {
                if (OnDialogueBlockEnded) OnDialogueBlockEnded(Timeline.Dialogue[static_cast<size_t>(ActiveBlockIndex)]);
            }
            ActiveBlockIndex = NewBlockIdx;
            NextCharIndex = 0;

            if (ActiveBlockIndex >= 0)
            {
                const auto& Block = Timeline.Dialogue[static_cast<size_t>(ActiveBlockIndex)];
                CurrentText = Block.Text;
                RevealedText.clear();
                VisemePlayer.LoadCues(Block.Visemes);
                if (OnDialogueBlockStarted) OnDialogueBlockStarted(Block);
            }
            else
            {
                VisemePlayer.Clear();
                FacialMixer.ClearVisemes();
            }
        }

        if (ActiveBlockIndex >= 0)
        {
            const auto& Block = Timeline.Dialogue[static_cast<size_t>(ActiveBlockIndex)];
            const double LocalTime = std::max(0.0, CurrentTimeSeconds - Block.StartTime);
            if (!Block.Characters.empty())
            {
                while (static_cast<size_t>(NextCharIndex) < Block.Characters.size())
                {
                    const auto& Cue = Block.Characters[static_cast<size_t>(NextCharIndex)];
                    if (LocalTime < Cue.StartTime) break;
                    RevealedText += Cue.Character;
                    ++NextCharIndex;
                    if (OnCharacterRevealed) OnCharacterRevealed(RevealedText, Cue);
                }
            }
            else if (!Block.Text.empty())
            {
                const double BlockDur = std::max(0.05, Block.EndTime - Block.StartTime);
                const double Ratio = std::max(0.0, std::min(1.0, LocalTime / BlockDur));
                const int TargetCount = static_cast<int>( std::round(Ratio * static_cast<double>(Block.Text.size())));
                if (TargetCount > NextCharIndex)
                {
                    NextCharIndex = TargetCount;
                    RevealedText = Block.Text.substr(0, static_cast<size_t>(NextCharIndex));
                }
            }

            if (Block.Type == "speech")
            {
                FacialMixer.SetVisemeWeights(VisemePlayer.Evaluate(LocalTime, FallbackAudioRms));
            }
            else
            {
                FacialMixer.ClearVisemes();
            }
        }

        FacialMixer.Update(static_cast<float>(Dt));

        if (CurrentTimeSeconds >= TotalDurationSeconds)
        {
            const FStagingResultPayload Completed = ActiveStaging;
            RevealedText = Completed.Text;
            Stop();
            if (OnStagingCompleted) OnStagingCompleted(Completed);
        }
    }

    FNpcifyCharacterRuntime::FNpcifyCharacterRuntime(const FNpcifyOptions& InOptions)
        : Options(InOptions)
        , StagingPlayer(Expressions, VisemePlayer)
        , Volume(InOptions.Volume)
        , bMuted(InOptions.bMuted)
    {
        StagingPlayer.OnDialogueBlockStarted = [this](const FStagingDialogueBlock& Block)
        {
            HandleDialogueBlockStarted(Block);
        };
        StagingPlayer.OnCharacterRevealed = [this](const std::string& Text, const FStagingCharacterCue& Cue)
        {
            if (OnCharacterRevealed) OnCharacterRevealed(Text, Cue);
        };
        StagingPlayer.OnAnimationClipTriggered = [this](const FStagingAnimationClip& Clip)
        {
            PlayAnimation(Clip.Animation, Clip.bLoop);
        };
        StagingPlayer.OnStagingCompleted = [this](const FStagingResultPayload& Staging)
        {
            if (OnSpeechEnd) OnSpeechEnd(Staging);
        };
    }

    void FNpcifyCharacterRuntime::SetVolume(float InVolume)
    {
        Volume = std::max(0.0f, std::min(1.0f, InVolume));
    }

    FNpcifySpeechState FNpcifyCharacterRuntime::GetSpeechState() const
    {
        FNpcifySpeechState State;
        State.bSpeaking = StagingPlayer.IsPlaying();
        State.bThinking = bThinking;
        const FStagingDialogueBlock* Block = StagingPlayer.GetCurrentBlock();
        if (Block)
        {
            State.bHasCurrentSpeech = true;
            State.CurrentSpeech = *Block;
        }
        State.CurrentText = StagingPlayer.GetCurrentText();
        State.RevealedText = StagingPlayer.GetRevealedText();
        State.Options = LastOptions;
        State.Cursor = StagingPlayer.GetCursor();
        return State;
    }

    void FNpcifyCharacterRuntime::AttachBakedConversation(std::unique_ptr<FNpcifyBakedConversation> InBaked)
    {
        if (!InBaked)
        {
            throw std::runtime_error("Baked conversation cannot be null.");
        }
        ConversationId = "baked:" + InBaked->GetAsset().AvatarId;
        BakedConversation = std::move(InBaked);
    }

    FStagingResultPayload FNpcifyCharacterRuntime::StartBakedConversation(const std::string& StagingId)
    {
        if (!BakedConversation)
        {
            throw std::runtime_error("No baked conversation is attached.");
        }
        const FStagingResultPayload Staging = StagingId.empty()
            ? BakedConversation->Start()
            : BakedConversation->Select(StagingId);
        ActiveStagingId = BakedConversation->GetActiveStagingId();
        PlayStaging(Staging);
        return Staging;
    }

    FStagingResultPayload FNpcifyCharacterRuntime::SelectBakedStaging(const std::string& StagingId)
    {
        if (!BakedConversation)
        {
            throw std::runtime_error("No baked conversation is attached.");
        }
        const FStagingResultPayload Staging = BakedConversation->Select(StagingId);
        ActiveStagingId = BakedConversation->GetActiveStagingId();
        PlayStaging(Staging);
        return Staging;
    }

    FBakedReplyResult FNpcifyCharacterRuntime::ReplyBaked(
        const std::string& OptionLabel,
        const std::string& ParentStagingId
    )
    {
        if (!BakedConversation)
        {
            throw std::runtime_error("No baked conversation is attached.");
        }
        const FBakedReplyResult Result = BakedConversation->Reply(OptionLabel, ParentStagingId);
        ActiveStagingId = BakedConversation->GetActiveStagingId();
        if (!Result.bEnded)
        {
            PlayStaging(Result.Payload);
        }
        return Result;
    }

    void FNpcifyCharacterRuntime::ResetConversationState()
    {
        StagingPlayer.Stop();
        bThinking = false;
        ActiveStagingId.clear();
        LastOptions.clear();
        if (!BakedConversation)
        {
            ConversationId.clear();
        }
    }

    void FNpcifyCharacterRuntime::PlayStaging(const FStagingResultPayload& Staging)
    {
        bThinking = false;
        ActiveStagingId = Staging.StagingId;
        LastOptions = Staging.Options;
        if (OnSpeechStart) OnSpeechStart(Staging);
        StagingPlayer.Play(Staging);
    }

    void FNpcifyCharacterRuntime::StopSpeaking()
    {
        bThinking = false;
        StagingPlayer.Stop();
    }

    void FNpcifyCharacterRuntime::PlayAnimation(const std::string& AnimationId, bool bLoop)
    {
        if (AnimationId.empty()) return;
        CurrentAnimation = AnimationId;
        if (OnAnimationPlayRequested) OnAnimationPlayRequested(AnimationId, bLoop);
    }

    void FNpcifyCharacterRuntime::LookAt(float YawDegrees, float PitchDegrees)
    {
        Expressions.SetGazeAngles(YawDegrees, PitchDegrees);
    }

    void FNpcifyCharacterRuntime::LookAtDirection(float LocalX, float LocalY, float LocalZ)
    {
        Expressions.SetGazeDirection(LocalX, LocalY, LocalZ);
    }

    void FNpcifyCharacterRuntime::ClearLookAt()
    {
        Expressions.ClearGaze();
    }

    void FNpcifyCharacterRuntime::Tick(double DeltaTimeSeconds, float FallbackAudioRms)
    {
        StagingPlayer.Tick(DeltaTimeSeconds, FallbackAudioRms);
    }

    void FNpcifyCharacterRuntime::HandleDialogueBlockStarted(const FStagingDialogueBlock& Block)
    {
        FNpcifyDecodedAudio Decoded;
        if (IsPcmAudio(Block.Audio))
        {
            Decoded = DecodeAudioBytes(Block.Audio.Data);
        }
        if (OnAudioBlock)
        {
            OnAudioBlock(Block, Decoded);
        }
    }

    bool FNpcifyCharacterRuntime::IsPcmAudio(const FStagingAudioSource& Audio)
    {
        const std::vector<uint8_t>& Data = Audio.Data;
        if (Data.empty())
        {
            return false;
        }
        if (Data.size() >= 12 && Data[0] == 'R' && Data[1] == 'I' && Data[2] == 'F' && Data[3] == 'F')
        {
            return true;
        }
        const std::string& Mime = Audio.MimeType;
        return Mime.find("wav") != std::string::npos ||
            Mime.find("pcm") != std::string::npos ||
            Mime.find("L16") != std::string::npos;
    }

    FNpcifyDecodedAudio FNpcifyCharacterRuntime::DecodeAudioBytes(
        const std::vector<uint8_t>& Bytes,
        int DefaultSampleRate,
        int DefaultChannels
    )
    {
        FNpcifyDecodedAudio Out;
        Out.SampleRate = DefaultSampleRate > 0 ? DefaultSampleRate : 24000;
        Out.Channels = DefaultChannels > 0 ? DefaultChannels : 1;
        if (Bytes.empty())
        {
            return Out;
        }

        if (Bytes.size() >= 44 &&
            Bytes[0] == 'R' && Bytes[1] == 'I' && Bytes[2] == 'F' && Bytes[3] == 'F' &&
            Bytes[8] == 'W' && Bytes[9] == 'A' && Bytes[10] == 'V' && Bytes[11] == 'E')
        {
            int AudioFormat = 1;
            int BitsPerSample = 16;
            size_t DataOffset = 0;
            size_t DataSize = 0;
            bool bFoundData = false;

            size_t Offset = 12;
            while (Offset + 8 <= Bytes.size())
            {
                const std::string ChunkId(reinterpret_cast<const char*>(Bytes.data() + Offset), 4);
                const uint32_t ChunkSize =
                    static_cast<uint32_t>(Bytes[Offset + 4]) |
                    (static_cast<uint32_t>(Bytes[Offset + 5]) << 8) |
                    (static_cast<uint32_t>(Bytes[Offset + 6]) << 16) |
                    (static_cast<uint32_t>(Bytes[Offset + 7]) << 24);
                Offset += 8;
                if (Offset + ChunkSize > Bytes.size()) break;

                if (ChunkId == "fmt " && ChunkSize >= 16)
                {
                    AudioFormat = Bytes[Offset] | (Bytes[Offset + 1] << 8);
                    Out.Channels = std::max(1, static_cast<int>(Bytes[Offset + 2] | (Bytes[Offset + 3] << 8)));
                    Out.SampleRate = static_cast<int>(
                        static_cast<uint32_t>(Bytes[Offset + 4]) |
                        (static_cast<uint32_t>(Bytes[Offset + 5]) << 8) |
                        (static_cast<uint32_t>(Bytes[Offset + 6]) << 16) |
                        (static_cast<uint32_t>(Bytes[Offset + 7]) << 24)
                    );
                    BitsPerSample = Bytes[Offset + 14] | (Bytes[Offset + 15] << 8);
                }
                else if (ChunkId == "data")
                {
                    DataOffset = Offset;
                    DataSize = ChunkSize;
                    bFoundData = true;
                    break;
                }
                Offset += ChunkSize + (ChunkSize & 1U);
            }

            if (!bFoundData || DataSize == 0)
            {
                return Out;
            }

            if (AudioFormat == 3 && BitsPerSample == 32)
            {
                const size_t SampleCount = DataSize / 4;
                Out.Samples.resize(SampleCount);
                Out.Pcm16Samples.resize(SampleCount);
                std::memcpy(Out.Samples.data(), Bytes.data() + DataOffset, SampleCount * 4);
                for (size_t i = 0; i < SampleCount; ++i)
                {
                    const float Clamped = std::max(-1.0f, std::min(1.0f, Out.Samples[i]));
                    Out.Pcm16Samples[i] = static_cast<int16_t>(Clamped * 32767.0f);
                }
                return Out;
            }

            if (BitsPerSample == 16)
            {
                const size_t SampleCount = DataSize / 2;
                Out.Samples.resize(SampleCount);
                Out.Pcm16Samples.resize(SampleCount);
                for (size_t i = 0; i < SampleCount; ++i)
                {
                    const int16_t Pcm = static_cast<int16_t>(
                        Bytes[DataOffset + i * 2] | (Bytes[DataOffset + i * 2 + 1] << 8)
                    );
                    Out.Pcm16Samples[i] = Pcm;
                    Out.Samples[i] = Pcm / 32768.0f;
                }
                return Out;
            }
        }

        const size_t RawCount = Bytes.size() / 2;
        Out.Samples.resize(RawCount);
        Out.Pcm16Samples.resize(RawCount);
        for (size_t i = 0; i < RawCount; ++i)
        {
            const int16_t Pcm = static_cast<int16_t>(Bytes[i * 2] | (Bytes[i * 2 + 1] << 8));
            Out.Pcm16Samples[i] = Pcm;
            Out.Samples[i] = Pcm / 32768.0f;
        }
        return Out;
    }

    FNpcifyMsgPackValue FNpcifyCharacterRuntime::BuildCreateConversationRequest(
        const std::string& AvatarId,
        const std::string& RenderSessionId
    )
    {
        std::map<std::string, FNpcifyMsgPackValue> Body;
        Body["avatarId"] = FNpcifyMsgPackValue::MakeString(AvatarId);
        if (!RenderSessionId.empty())
        {
            Body["renderSessionId"] = FNpcifyMsgPackValue::MakeString(RenderSessionId);
        }
        return FNpcifyMsgPackValue::MakeMap(std::move(Body));
    }

    FNpcifyMsgPackValue FNpcifyCharacterRuntime::BuildCreateStagingRequest(
        const std::string& Message,
        const std::string& ParentStagingId,
        bool bSilent,
        int MaxOptions
    )
    {
        std::map<std::string, FNpcifyMsgPackValue> Body;
        Body["message"] = FNpcifyMsgPackValue::MakeString(Message);
        if (!ParentStagingId.empty())
        {
            Body["parentStagingId"] = FNpcifyMsgPackValue::MakeString(ParentStagingId);
        }
        if (bSilent)
        {
            Body["silent"] = FNpcifyMsgPackValue::MakeBool(true);
        }
        Body["maxOptions"] = FNpcifyMsgPackValue::MakeInt(std::max(1, MaxOptions));
        return FNpcifyMsgPackValue::MakeMap(std::move(Body));
    }
}
