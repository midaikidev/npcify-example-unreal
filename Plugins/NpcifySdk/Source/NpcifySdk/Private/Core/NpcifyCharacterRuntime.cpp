#include "Core/NpcifyCharacterRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace Npcify
{
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
            HandleStagingCompleted(Staging);
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
        ++PlayGeneration;
        bThinking = false;
        // A reply that played to its end and waits for its options still ends.
        FStoppedSpeech Stop = StopPlayingTurn(ENpcifyTurnStop::Game);
        Stop.bStopped = StopPlayback() || Stop.bStopped;
        StopStreamTurns();
        ActiveStagingId.clear();
        LastOptions.clear();
        if (!BakedConversation)
        {
            ConversationId.clear();
        }
        // The events come last: a handler can start the next conversation.
        Report(Stop);
    }

    void FNpcifyCharacterRuntime::PlayStaging(const FStagingResultPayload& Staging)
    {
        const uint64_t Generation = ++PlayGeneration;
        bThinking = false;
        // A staging played as a whole replaces the speech that plays.
        FStoppedSpeech Stop = StopPlayingTurn(ENpcifyTurnStop::Superseded);
        Stop.bStopped = StopPlayback() || Stop.bStopped;
        Report(Stop);
        if (Generation != PlayGeneration)
        {
            // A handler of the stop played or stopped speech.
            return;
        }
        ActiveStagingId = Staging.StagingId;
        LastOptions = Staging.Options;
        const FStagingMotionTrack& Track = Staging.Timeline.Motion;
        if (Track.bValid && !LiveMotion.Has(Staging.StagingId))
        {
            // A replay plays its stored frames once they load; until then idle.
            if (!Track.FramesData.empty())
            {
                try
                {
                    LiveMotion.Load(Staging.StagingId, Track, Track.FramesData);
                }
                catch (const std::exception& Error)
                {
                    if (OnError) OnError(Error.what());
                }
            }
            else if (OnMotionTrackRequested)
            {
                OnMotionTrackRequested(Staging.StagingId, Track);
            }
        }
        if (OnSpeechStart) OnSpeechStart(Staging);
        StagingPlayer.Play(Staging);
    }

    void FNpcifyCharacterRuntime::StopSpeaking()
    {
        ++PlayGeneration;
        bThinking = false;
        // A reply that played to its end and waits for its options still ends.
        FStoppedSpeech Stop = StopPlayingTurn(ENpcifyTurnStop::Game);
        Stop.bStopped = StopPlayback() || Stop.bStopped;
        Report(Stop);
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
        // Live motion follows the staging clock; no staging: blend back to idle.
        const FStagingResultPayload* Playing = StagingPlayer.IsPlaying() ? StagingPlayer.GetCurrentStaging() : nullptr;
        LiveMotion.Update(
            DeltaTimeSeconds,
            Playing ? Playing->StagingId : std::string(),
            Playing ? StagingPlayer.GetTimeSeconds() : -1.0);
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
