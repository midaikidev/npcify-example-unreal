#include "Core/NpcifyFacialMixer.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Npcify
{
    namespace
    {
        constexpr double PiValue = 3.14159265358979323846;

        std::string ToLowerAscii(const std::string& Value)
        {
            std::string Out;
            Out.reserve(Value.size());
            for (unsigned char Ch : Value)
            {
                if (!std::isspace(Ch))
                {
                    Out.push_back(static_cast<char>(std::tolower(Ch)));
                }
            }
            return Out;
        }

        const char* CanonicalList[] = {
            "neutral", "happy", "angry", "sad", "relaxed", "surprised",
            "aa", "ih", "ou", "ee", "oh",
            "blink", "blinkLeft", "blinkRight",
            "lookUp", "lookDown", "lookLeft", "lookRight"
        };

        bool IsVisemeKey(const std::string& Key)
        {
            return Key == "aa" || Key == "ih" || Key == "ou" || Key == "ee" || Key == "oh";
        }
    }

    void FNpcifyVisemePlayer::LoadCues(const std::vector<FStagingVisemeCue>& InCues)
    {
        Cues = InCues;
        std::sort(Cues.begin(), Cues.end(), [](const FStagingVisemeCue& A, const FStagingVisemeCue& B)
        {
            return A.Time < B.Time;
        });
    }

    void FNpcifyVisemePlayer::Clear()
    {
        Cues.clear();
    }

    std::map<std::string, float> FNpcifyVisemePlayer::Evaluate(double LocalTimeSeconds, float FallbackRms) const
    {
        std::map<std::string, float> Weights = {
            {"aa", 0.0f}, {"ih", 0.0f}, {"ou", 0.0f}, {"ee", 0.0f}, {"oh", 0.0f}
        };

        if (LocalTimeSeconds < 0.0)
        {
            return Weights;
        }

        bool bAnyCueActive = false;
        for (const auto& Cue : Cues)
        {
            const std::string Key = FNpcifyFacialMixer::NormalizeExpressionName(Cue.Viseme);
            if (!IsVisemeKey(Key)) continue;

            const double Duration = std::max(0.04, Cue.Duration);
            const double Attack = std::min(0.045, Duration * 0.35);
            const double Release = std::min(0.075, Duration * 0.45);
            const double Start = Cue.Time - Attack * 0.5;
            const double End = Cue.Time + Duration + Release;

            if (LocalTimeSeconds < Start || LocalTimeSeconds > End)
            {
                continue;
            }

            double Env = 1.0;
            if (LocalTimeSeconds < Cue.Time)
            {
                const double T = (LocalTimeSeconds - Start) / std::max(0.001, Cue.Time - Start);
                Env = 0.5 - 0.5 * std::cos(PiValue * std::max(0.0, std::min(1.0, T)));
            }
            else if (LocalTimeSeconds > Cue.Time + Duration)
            {
                const double T = (LocalTimeSeconds - (Cue.Time + Duration)) / std::max(0.001, Release);
                Env = 0.5 + 0.5 * std::cos(PiValue * std::max(0.0, std::min(1.0, T)));
            }

            const float Val = static_cast<float>(std::max(0.0, std::min(1.0, Cue.Weight * Env)));
            if (Val > Weights[Key])
            {
                Weights[Key] = Val;
                bAnyCueActive = true;
            }
        }

        if (!bAnyCueActive && Cues.empty() && FallbackRms > 0.01f)
        {
            const float Clamped = std::max(0.0f, std::min(1.0f, FallbackRms));
            Weights["aa"] = Clamped;
            Weights["oh"] = Clamped * 0.35f;
        }

        return Weights;
    }

    FNpcifyFacialMixer::FNpcifyFacialMixer()
    {
        Layers[ENpcifyExpressionLayer::Emotion] = {};
        Layers[ENpcifyExpressionLayer::Viseme] = {};
        Layers[ENpcifyExpressionLayer::Blink] = {};
        Layers[ENpcifyExpressionLayer::Gaze] = {};
        for (const char* Key : CanonicalList)
        {
            SmoothedWeights[Key] = 0.0f;
        }
    }

    std::string FNpcifyFacialMixer::NormalizeExpressionName(const std::string& Name)
    {
        const std::string Lower = ToLowerAscii(Name);
        if (Lower.empty() || Lower == "neutral") return "neutral";
        if (Lower == "happy" || Lower == "joy" || Lower == "smile" || Lower == "mouthsmileleft" || Lower == "mouthsmileright") return "happy";
        if (Lower == "angry" || Lower == "anger") return "angry";
        if (Lower == "sad" || Lower == "sorrow") return "sad";
        if (Lower == "relaxed" || Lower == "fun" || Lower == "calm") return "relaxed";
        if (Lower == "surprised" || Lower == "surprise") return "surprised";
        if (Lower == "aa" || Lower == "a" || Lower == "fcl_mth_a" || Lower == "vrc.v_aa" || Lower == "jawopen" || Lower == "mouthopen") return "aa";
        if (Lower == "ih" || Lower == "i" || Lower == "fcl_mth_i" || Lower == "vrc.v_ih") return "ih";
        if (Lower == "ou" || Lower == "u" || Lower == "fcl_mth_u" || Lower == "vrc.v_ou" || Lower == "mouthpucker") return "ou";
        if (Lower == "ee" || Lower == "e" || Lower == "fcl_mth_e" || Lower == "vrc.v_e") return "ee";
        if (Lower == "oh" || Lower == "o" || Lower == "fcl_mth_o" || Lower == "vrc.v_oh" || Lower == "mouthfunnel") return "oh";
        if (Lower == "blink" || Lower == "fcl_eye_close") return "blink";
        if (Lower == "blinkleft" || Lower == "blink_l" || Lower == "fcl_eye_close_l" || Lower == "eyeblinkleft") return "blinkLeft";
        if (Lower == "blinkright" || Lower == "blink_r" || Lower == "fcl_eye_close_r" || Lower == "eyeblinkright") return "blinkRight";
        if (Lower == "lookup" || Lower == "look_up" || Lower == "eyelookupleft" || Lower == "eyelookupright") return "lookUp";
        if (Lower == "lookdown" || Lower == "look_down" || Lower == "eyelookdownleft" || Lower == "eyelookdownright") return "lookDown";
        if (Lower == "lookleft" || Lower == "look_left" || Lower == "eyelookinright" || Lower == "eyelookoutleft") return "lookLeft";
        if (Lower == "lookright" || Lower == "look_right" || Lower == "eyelookinleft" || Lower == "eyelookoutright") return "lookRight";
        return Name;
    }

    float FNpcifyFacialMixer::GetWeight(const std::string& ExpressionName) const
    {
        const std::string Canonical = NormalizeExpressionName(ExpressionName);
        auto It = SmoothedWeights.find(Canonical);
        return It != SmoothedWeights.end() ? It->second : 0.0f;
    }

    float FNpcifyFacialMixer::GetMouthOpenness() const
    {
        const float Aa = GetWeight("aa");
        const float Oh = GetWeight("oh") * 0.85f;
        const float Ee = GetWeight("ee") * 0.55f;
        const float Ih = GetWeight("ih") * 0.45f;
        const float Ou = GetWeight("ou") * 0.5f;
        return Clamp01(std::max({Aa, Oh, Ee, Ih, Ou}));
    }

    void FNpcifyFacialMixer::SetLayerWeight(
        ENpcifyExpressionLayer Layer,
        const std::string& ExpressionName,
        float Weight
    )
    {
        Layers[Layer][NormalizeExpressionName(ExpressionName)] = Clamp01(Weight);
    }

    void FNpcifyFacialMixer::ClearLayer(ENpcifyExpressionLayer Layer)
    {
        Layers[Layer].clear();
    }

    void FNpcifyFacialMixer::SetEmotion(const std::string& ExpressionName, float Weight)
    {
        Layers[ENpcifyExpressionLayer::Emotion].clear();
        const std::string Canonical = NormalizeExpressionName(ExpressionName);
        if (Canonical != "neutral")
        {
            Layers[ENpcifyExpressionLayer::Emotion][Canonical] = Clamp01(Weight);
        }
    }

    void FNpcifyFacialMixer::ClearEmotion()
    {
        Layers[ENpcifyExpressionLayer::Emotion].clear();
        ActiveCues.clear();
    }

    void FNpcifyFacialMixer::PlayCue(
        const std::string& ExpressionName,
        float Weight,
        float DurationSeconds,
        float FadeInSeconds,
        float FadeOutSeconds
    )
    {
        const std::string Canonical = NormalizeExpressionName(ExpressionName);
        if (Canonical == "neutral") return;
        FActiveCue Cue;
        Cue.Expression = Canonical;
        Cue.TargetWeight = Clamp01(Weight);
        Cue.Duration = std::max(0.05f, DurationSeconds);
        Cue.FadeIn = std::max(0.0f, FadeInSeconds);
        Cue.FadeOut = std::max(0.0f, FadeOutSeconds);
        Cue.Elapsed = 0.0f;
        ActiveCues.push_back(std::move(Cue));
    }

    void FNpcifyFacialMixer::SetVisemeWeights(const std::map<std::string, float>& Visemes)
    {
        auto& Layer = Layers[ENpcifyExpressionLayer::Viseme];
        Layer.clear();
        for (const auto& Pair : Visemes)
        {
            const std::string Canonical = NormalizeExpressionName(Pair.first);
            if (IsVisemeKey(Canonical))
            {
                Layer[Canonical] = Clamp01(Pair.second);
            }
        }
    }

    void FNpcifyFacialMixer::ClearVisemes()
    {
        Layers[ENpcifyExpressionLayer::Viseme].clear();
    }

    void FNpcifyFacialMixer::SetMouthOpenness(float Openness)
    {
        auto& Layer = Layers[ENpcifyExpressionLayer::Viseme];
        Layer.clear();
        const float Clamped = Clamp01(Openness);
        if (Clamped > 0.001f)
        {
            Layer["aa"] = Clamped;
            Layer["oh"] = Clamped * 0.35f;
        }
    }

    void FNpcifyFacialMixer::SetGazeAngles(
        float YawDegrees,
        float PitchDegrees,
        float MaxYawDegrees,
        float MaxPitchDegrees
    )
    {
        const float SafeMaxYaw = std::max(1.0f, MaxYawDegrees);
        const float SafeMaxPitch = std::max(1.0f, MaxPitchDegrees);
        auto& Layer = Layers[ENpcifyExpressionLayer::Gaze];
        Layer["lookRight"] = Clamp01(YawDegrees / SafeMaxYaw);
        Layer["lookLeft"] = Clamp01(-YawDegrees / SafeMaxYaw);
        Layer["lookUp"] = Clamp01(PitchDegrees / SafeMaxPitch);
        Layer["lookDown"] = Clamp01(-PitchDegrees / SafeMaxPitch);
    }

    void FNpcifyFacialMixer::SetGazeDirection(
        float LocalX,
        float LocalY,
        float LocalZ,
        float MaxYawDegrees,
        float MaxPitchDegrees
    )
    {
        const float Mag = std::sqrt(LocalX * LocalX + LocalY * LocalY + LocalZ * LocalZ);
        if (Mag <= 0.0001f)
        {
            ClearGaze();
            return;
        }
        const float Nx = LocalX / Mag;
        const float Ny = std::max(-1.0f, std::min(1.0f, LocalY / Mag));
        const float Nz = LocalZ / Mag;
        const float YawDegrees = static_cast<float>(std::atan2(Nx, Nz) * (180.0 / PiValue));
        const float PitchDegrees = static_cast<float>(std::asin(Ny) * (180.0 / PiValue));
        SetGazeAngles(YawDegrees, PitchDegrees, MaxYawDegrees, MaxPitchDegrees);
    }

    void FNpcifyFacialMixer::ClearGaze()
    {
        Layers[ENpcifyExpressionLayer::Gaze].clear();
    }

    void FNpcifyFacialMixer::Reset()
    {
        for (auto& Pair : Layers)
        {
            Pair.second.clear();
        }
        ActiveCues.clear();
        for (auto& Pair : SmoothedWeights)
        {
            Pair.second = 0.0f;
        }
    }

    void FNpcifyFacialMixer::Update(float DeltaTimeSeconds)
    {
        const float Dt = std::max(0.0f, DeltaTimeSeconds);
        UpdateAutoBlink(Dt);

        std::map<std::string, float> CueTargets;
        for (int i = static_cast<int>(ActiveCues.size()) - 1; i >= 0; --i)
        {
            FActiveCue& Cue = ActiveCues[static_cast<size_t>(i)];
            Cue.Elapsed += Dt;
            if (Cue.Elapsed >= Cue.Duration)
            {
                ActiveCues.erase(ActiveCues.begin() + i);
                continue;
            }
            float Envelope = 1.0f;
            if (Cue.FadeIn > 0.0f && Cue.Elapsed < Cue.FadeIn)
            {
                Envelope = Cue.Elapsed / Cue.FadeIn;
            }
            else if (Cue.FadeOut > 0.0f && Cue.Elapsed > Cue.Duration - Cue.FadeOut)
            {
                Envelope = (Cue.Duration - Cue.Elapsed) / Cue.FadeOut;
            }
            const float Effective = Clamp01(Cue.TargetWeight * Clamp01(Envelope));
            if (Effective > CueTargets[Cue.Expression])
            {
                CueTargets[Cue.Expression] = Effective;
            }
        }

        const float Alpha = Dt <= 0.0f ? 1.0f : Clamp01(1.0f - std::exp(-SmoothingRate * Dt));
        for (auto& Pair : SmoothedWeights)
        {
            float Target = 0.0f;
            for (const auto& LayerPair : Layers)
            {
                auto It = LayerPair.second.find(Pair.first);
                if (It != LayerPair.second.end() && It->second > Target)
                {
                    Target = It->second;
                }
            }
            auto CueIt = CueTargets.find(Pair.first);
            if (CueIt != CueTargets.end() && CueIt->second > Target)
            {
                Target = CueIt->second;
            }

            if ((Pair.first == "blink" || Pair.first == "blinkLeft" || Pair.first == "blinkRight") &&
                SmoothedWeights["happy"] > 0.5f)
            {
                Target *= (1.0f - (SmoothedWeights["happy"] - 0.5f) * 1.6f);
            }

            const float Next = Pair.second + (Target - Pair.second) * Alpha;
            Pair.second = Next < 0.0005f ? 0.0f : Clamp01(Next);
        }
    }

    float FNpcifyFacialMixer::ResolveMorphTargetWeight01(const std::string& MorphTargetName) const
    {
        if (MorphTargetName.empty()) return 0.0f;
        const std::string Canonical = NormalizeExpressionName(MorphTargetName);
        if (Canonical == "blinkLeft")
        {
            return Clamp01(std::max(GetWeight("blinkLeft"), GetWeight("blink")));
        }
        if (Canonical == "blinkRight")
        {
            return Clamp01(std::max(GetWeight("blinkRight"), GetWeight("blink")));
        }
        return GetWeight(Canonical);
    }

    void FNpcifyFacialMixer::UpdateAutoBlink(float Dt)
    {
        auto& BlinkLayer = Layers[ENpcifyExpressionLayer::Blink];
        if (!bAutoBlinkEnabled)
        {
            BlinkLayer.clear();
            bIsBlinking = false;
            return;
        }
        BlinkTimer += Dt;
        if (!bIsBlinking)
        {
            if (BlinkTimer >= NextBlinkInterval)
            {
                bIsBlinking = true;
                BlinkTimer = 0.0f;
            }
            BlinkLayer["blink"] = 0.0f;
        }
        else
        {
            const float T = BlinkTimer / std::max(0.05f, BlinkDuration);
            if (T >= 1.0f)
            {
                bIsBlinking = false;
                BlinkTimer = 0.0f;
                NextBlinkInterval = 2.4f + NextRandom01() * 3.4f;
                BlinkLayer["blink"] = 0.0f;
            }
            else
            {
                BlinkLayer["blink"] = Clamp01(static_cast<float>(std::sin(T * PiValue)));
            }
        }
    }

    float FNpcifyFacialMixer::NextRandom01()
    {
        RngState += 0x6D2B79F5u;
        uint32_t Z = RngState;
        Z = (Z ^ (Z >> 15)) * (Z | 1u);
        Z ^= Z + (Z ^ (Z >> 7)) * (Z | 61u);
        Z ^= Z >> 14;
        return (Z & 0xFFFFFFu) / static_cast<float>(0x1000000u);
    }

    float FNpcifyFacialMixer::Clamp01(float Value)
    {
        return Value < 0.0f ? 0.0f : (Value > 1.0f ? 1.0f : Value);
    }
}
