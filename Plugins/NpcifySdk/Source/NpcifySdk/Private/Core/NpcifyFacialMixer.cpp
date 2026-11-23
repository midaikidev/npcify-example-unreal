#include "Core/NpcifyFacialMixer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

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

        constexpr size_t BlinkIndex = 11;
        constexpr size_t BlinkLeftIndex = 12;
        constexpr size_t BlinkRightIndex = 13;
        constexpr size_t LookUpIndex = 14;

        bool IsBlinkIndex(size_t Index)
        {
            return Index >= BlinkIndex && Index <= BlinkRightIndex;
        }

        bool IsLookIndex(size_t Index)
        {
            return Index >= LookUpIndex && Index < NpcifyExpressionCount;
        }

        float ClampUnit(float Value)
        {
            return Value < 0.0f ? 0.0f : (Value > 1.0f ? 1.0f : Value);
        }

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
        : FNpcifyFacialMixer(std::function<double()>())
    {
    }

    FNpcifyFacialMixer::FNpcifyFacialMixer(std::function<double()> BlinkRandom)
        : IdleBlinks(std::move(BlinkRandom))
    {
        Layers[ENpcifyExpressionLayer::Emotion] = {};
        Layers[ENpcifyExpressionLayer::Viseme] = {};
        Layers[ENpcifyExpressionLayer::Blink] = {};
        Layers[ENpcifyExpressionLayer::Gaze] = {};
        for (const char* Key : CanonicalList)
        {
            SmoothedWeights[Key] = 0.0f;
            AppliedWeights[Key] = 0.0f;
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

    size_t FNpcifyFacialMixer::ExpressionIndex(const std::string& Name)
    {
        if (ToLowerAscii(Name).empty())
        {
            return NpcifyExpressionCount;
        }
        const std::string Canonical = NormalizeExpressionName(Name);
        for (size_t Index = 0; Index < NpcifyExpressionCount; ++Index)
        {
            if (Canonical == CanonicalList[Index])
            {
                return Index;
            }
        }
        return NpcifyExpressionCount;
    }

    size_t FNpcifyFacialMixer::PresetIndex(const std::string& Name)
    {
        // The VRM 0 preset names with the index of their VRM 1 preset.
        static const std::pair<const char*, size_t> Vrm0Presets[] = {
            {"joy", 1}, {"sorrow", 3}, {"fun", 4},
            {"a", 6}, {"i", 7}, {"u", 8}, {"e", 9}, {"o", 10},
            {"blink_l", 12}, {"blink_r", 13},
        };
        const std::string Lower = ToLowerAscii(Name);
        for (size_t Index = 0; Index < NpcifyExpressionCount; ++Index)
        {
            if (Lower == ToLowerAscii(CanonicalList[Index]))
            {
                return Index;
            }
        }
        for (const auto& Preset : Vrm0Presets)
        {
            if (Lower == Preset.first)
            {
                return Preset.second;
            }
        }
        return NpcifyExpressionCount;
    }

    void FNpcifyFacialMixer::StoreAppliedList()
    {
        for (size_t Index = 0; Index < NpcifyExpressionCount; ++Index)
        {
            AppliedList[Index] = AppliedWeights[CanonicalList[Index]];
        }
    }

    float FNpcifyFacialMixer::GetWeight(const std::string& ExpressionName) const
    {
        const std::string Canonical = NormalizeExpressionName(ExpressionName);
        auto It = AppliedWeights.find(Canonical);
        return It != AppliedWeights.end() ? It->second : 0.0f;
    }

    bool FNpcifyFacialMixer::RequestBlink(float Chance)
    {
        return bAutoBlinkEnabled && IdleBlinks.RequestBlink(Clock, Chance);
    }

    void FNpcifyFacialMixer::TrackHead(float X, float Y, float Z, float W)
    {
        if (bAutoBlinkEnabled)
        {
            IdleBlinks.TrackHead(Clock, X, Y, Z, W);
        }
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
        // A fast and large shift of the eyes brings a blink.
        WatchGaze(
            std::max(-SafeMaxYaw, std::min(SafeMaxYaw, YawDegrees)),
            std::max(-SafeMaxPitch, std::min(SafeMaxPitch, PitchDegrees)));
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
        WatchGaze(0.0f, 0.0f);
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
        for (auto& Pair : AppliedWeights)
        {
            Pair.second = 0.0f;
        }
        AppliedList.fill(0.0f);
        IdleBlinks.Reset();
        IdleBlink = 0.0f;
    }

    void FNpcifyFacialMixer::Update(float DeltaTimeSeconds)
    {
        const float Dt = std::max(0.0f, DeltaTimeSeconds);
        Clock += Dt;

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

        std::map<std::string, float> Targets;
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
            Targets[Pair.first] = Target;
        }
        UpdateIdleBlink(Targets);

        // The blink plays in full with every emotion. A generated face plays happy here with the
        // mouth smile only and surprised not at all (the alias table does not know the names of
        // its emotion shapes), so the emotion does not close the lids and the blink must. A face
        // with its own happy shape adds the blink to it, as its VRM file tells the Web SDK.
        const float Alpha = Dt <= 0.0f ? 1.0f : Clamp01(1.0f - std::exp(-SmoothingRate * Dt));
        for (auto& Pair : SmoothedWeights)
        {
            const float Target = Targets[Pair.first];
            const float Next = Pair.second + (Target - Pair.second) * Alpha;
            // A weight within 0.001 of its target takes the target (the rule of the Web SDK).
            Pair.second = std::fabs(Next - Target) < 0.001f ? Clamp01(Target) : Clamp01(Next);
            AppliedWeights[Pair.first] = Pair.second;
        }
        // The idle blink has its own curve: it is not smoothed.
        if (IdleBlink > AppliedWeights["blink"])
        {
            AppliedWeights["blink"] = IdleBlink;
        }
        StoreAppliedList();
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

    void FNpcifyFacialMixer::UpdateIdleBlink(const std::map<std::string, float>& Targets)
    {
        // A blink expression that a cue, the emotion or a layer drives holds the idle blink: a
        // blink that is due is skipped, and a blink that plays ends.
        bool bHeld = !bAutoBlinkEnabled;
        for (const char* Key : {"blink", "blinkLeft", "blinkRight"})
        {
            auto It = Targets.find(Key);
            bHeld = bHeld || (It != Targets.end() && It->second > 0.0f);
        }
        const bool bSpeaking = !Layers[ENpcifyExpressionLayer::Viseme].empty();
        const double Weight = IdleBlinks.Sample(Clock, bHeld, bSpeaking);
        IdleBlink = bHeld ? 0.0f : Clamp01(static_cast<float>(Weight));
    }

    void FNpcifyFacialMixer::WatchGaze(float YawDegrees, float PitchDegrees)
    {
        if (bAutoBlinkEnabled)
        {
            IdleBlinks.WatchGaze(Clock, YawDegrees, PitchDegrees);
        }
    }

    FNpcifyMorphTargetPlan::FNpcifyMorphTargetPlan(const std::vector<std::string>& InNames)
        : Names(InNames), Roles(InNames.size(), ERole::Other)
    {
        bHasBoth = Choose({"blink", "fcl_eye_close"}, ERole::Both);
        Choose({"blinkleft", "blink_l", "fcl_eye_close_l", "eyeblinkleft"}, ERole::Left);
        Choose({"blinkright", "blink_r", "fcl_eye_close_r", "eyeblinkright"}, ERole::Right);
        AliasExpressions.reserve(Names.size());
        for (const std::string& Name : Names)
        {
            AliasExpressions.push_back(FNpcifyFacialMixer::ExpressionIndex(Name));
        }
    }

    FNpcifyMorphTargetPlan::FNpcifyMorphTargetPlan(
        const std::vector<std::string>& InNames,
        const std::vector<FNpcifyVrmExpression>& Expressions)
        : FNpcifyMorphTargetPlan(InNames)
    {
        Bound.resize(Names.size());
        for (const FNpcifyVrmExpression& Expression : Expressions)
        {
            // The mixer plays the presets only: a custom expression of the file has no weight.
            const size_t Index = FNpcifyFacialMixer::PresetIndex(Expression.Name);
            if (Index >= NpcifyExpressionCount)
            {
                continue;
            }
            bHasExpressions = true;
            FRule& Rule = Rules[Index];
            Rule.bDefined = true;
            Rule.bIsBinary = Rule.bIsBinary || Expression.bIsBinary;
            Rule.bBound = Rule.bBound || Expression.bBoundOnOtherMesh;
            if (Expression.OverrideBlink != ENpcifyExpressionOverride::None)
            {
                Rule.OverrideBlink = Expression.OverrideBlink;
            }
            if (Expression.OverrideLookAt != ENpcifyExpressionOverride::None)
            {
                Rule.OverrideLookAt = Expression.OverrideLookAt;
            }
            for (const FNpcifyExpressionBind& Bind : Expression.Binds)
            {
                // A bind on a target that the mesh does not have still makes the expression a
                // bound one: the file binds it, so the alias names do not play it.
                Rule.bBound = true;
                if (Bind.MorphIndex < Names.size())
                {
                    Bound[Bind.MorphIndex].push_back({Index, ClampUnit(Bind.Weight)});
                }
            }
        }
        for (size_t Index = 0; Index < NpcifyExpressionCount; ++Index)
        {
            const FRule& Rule = Rules[Index];
            const bool bOverridesBlink = Rule.OverrideBlink != ENpcifyExpressionOverride::None && !IsBlinkIndex(Index);
            const bool bOverridesLook = Rule.OverrideLookAt != ENpcifyExpressionOverride::None && !IsLookIndex(Index);
            bHasOverrides = bHasOverrides || bOverridesBlink || bOverridesLook;
        }
    }

    bool FNpcifyMorphTargetPlan::Choose(const std::vector<const char*>& Aliases, ERole Role)
    {
        // Gives the best target of these names the role and mutes the other targets of them.
        size_t Best = Names.size();
        size_t BestRank = Aliases.size();
        for (size_t Index = 0; Index < Names.size(); ++Index)
        {
            const std::string Lower = ToLowerAscii(Names[Index]);
            for (size_t Rank = 0; Rank < Aliases.size(); ++Rank)
            {
                if (Lower == Aliases[Rank])
                {
                    Roles[Index] = ERole::Muted;
                    if (Rank < BestRank)
                    {
                        Best = Index;
                        BestRank = Rank;
                    }
                    break;
                }
            }
        }
        if (Best == Names.size())
        {
            return false;
        }
        Roles[Best] = Role;
        return true;
    }

    float FNpcifyMorphTargetPlan::GetWeight(size_t Index, const FNpcifyFacialMixer& Mixer) const
    {
        if (Index >= Names.size())
        {
            return 0.0f;
        }
        if (!bHasExpressions)
        {
            return AliasWeight(Index, Mixer);
        }
        float Weight = 0.0f;
        for (const FBound& Bind : Bound[Index])
        {
            Weight += BoundWeight(Bind.Expression, Mixer) * Bind.Weight;
        }
        // The alias name plays only an expression that the file does not bind.
        const ERole Role = Roles[Index];
        if (Role == ERole::Other)
        {
            const size_t Alias = AliasExpressions[Index];
            if (Alias < NpcifyExpressionCount && !Rules[Alias].bBound)
            {
                Weight += AliasWeight(Index, Mixer);
            }
        }
        else if (Role != ERole::Muted &&
            !Rules[BlinkIndex].bBound && !Rules[BlinkLeftIndex].bBound && !Rules[BlinkRightIndex].bBound)
        {
            Weight += AliasWeight(Index, Mixer);
        }
        return ClampUnit(Weight);
    }

    float FNpcifyMorphTargetPlan::AliasWeight(size_t Index, const FNpcifyFacialMixer& Mixer) const
    {
        const ERole Role = Roles[Index];
        if (Role == ERole::Other)
        {
            const size_t Alias = AliasExpressions[Index];
            return Alias < NpcifyExpressionCount ? Mixer.GetWeightAt(Alias) : 0.0f;
        }
        if (Role == ERole::Muted)
        {
            return 0.0f;
        }
        const float Blink = Mixer.GetWeightAt(BlinkIndex);
        const float Left = std::max(Blink, Mixer.GetWeightAt(BlinkLeftIndex));
        const float Right = std::max(Blink, Mixer.GetWeightAt(BlinkRightIndex));
        const float Shared = bHasBoth ? std::min(Left, Right) : 0.0f;
        if (Role == ERole::Both)
        {
            return Shared;
        }
        return ClampUnit((Role == ERole::Left ? Left : Right) - Shared);
    }

    float FNpcifyMorphTargetPlan::BoundWeight(size_t Expression, const FNpcifyFacialMixer& Mixer) const
    {
        float Weight = Mixer.GetWeightAt(Expression);
        // A file without binds for "blink" plays a blink on the two sides.
        if ((Expression == BlinkLeftIndex || Expression == BlinkRightIndex) && !Rules[BlinkIndex].bBound)
        {
            Weight = std::max(Weight, Mixer.GetWeightAt(BlinkIndex));
        }
        if (Rules[Expression].bIsBinary)
        {
            Weight = Weight > 0.5f ? 1.0f : 0.0f;
        }
        if (bHasOverrides && Weight > 0.0f)
        {
            if (IsBlinkIndex(Expression))
            {
                Weight *= OverrideFactor(true, Mixer);
            }
            else if (IsLookIndex(Expression))
            {
                Weight *= OverrideFactor(false, Mixer);
            }
        }
        return Weight;
    }

    float FNpcifyMorphTargetPlan::OverrideFactor(bool bBlink, const FNpcifyFacialMixer& Mixer) const
    {
        // VRM 1.0: an expression that overrides the blinks (or the gaze) blocks them while it has
        // a weight, or takes its own weight from them.
        float Factor = 1.0f;
        for (size_t Index = 0; Index < NpcifyExpressionCount; ++Index)
        {
            const FRule& Rule = Rules[Index];
            const ENpcifyExpressionOverride Override = bBlink ? Rule.OverrideBlink : Rule.OverrideLookAt;
            if (Override == ENpcifyExpressionOverride::None || (bBlink ? IsBlinkIndex(Index) : IsLookIndex(Index)))
            {
                continue;
            }
            float Weight = Mixer.GetWeightAt(Index);
            if (Rule.bIsBinary)
            {
                Weight = Weight > 0.5f ? 1.0f : 0.0f;
            }
            if (Override == ENpcifyExpressionOverride::Block)
            {
                if (Weight > 0.0f)
                {
                    return 0.0f;
                }
            }
            else
            {
                Factor *= 1.0f - ClampUnit(Weight);
            }
        }
        return Factor;
    }

    float FNpcifyFacialMixer::Clamp01(float Value)
    {
        return Value < 0.0f ? 0.0f : (Value > 1.0f ? 1.0f : Value);
    }
}
