#pragma once

#include "Core/NpcifyIdleBlinks.h"
#include "Core/NpcifyModels.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Npcify
{
    enum class ENpcifyExpressionLayer : uint8_t
    {
        Emotion = 0,
        Viseme = 1,
        Blink = 2,
        Gaze = 3,
    };

    /** The number of expressions of a face: the VRM presets that the mixer plays. */
    constexpr size_t NpcifyExpressionCount = 18;

    /** What an expression does to the blinks or to the gaze while it plays (VRM 1.0, override). */
    enum class ENpcifyExpressionOverride : uint8_t
    {
        None = 0,
        /** The others do not play while this expression has a weight. */
        Block = 1,
        /** The others play less by the weight of this expression. */
        Blend = 2,
    };

    /** One morph target that an expression of a VRM file plays. */
    struct FNpcifyExpressionBind
    {
        size_t MorphIndex = 0;
        /** 0 to 1. */
        float Weight = 1.0f;
    };

    /**
     * One expression of a VRM file (VRMC_vrm.expressions, or a VRM 0 blend shape group) for one
     * mesh. The host reads it from the file and gives the list to FNpcifyMorphTargetPlan.
     */
    struct FNpcifyVrmExpression
    {
        /**
         * The name of the expression: a VRM 1 preset ("happy", "aa") or a VRM 0 preset ("joy",
         * "a"). The plan does not play an expression with another name (a custom expression).
         */
        std::string Name;
        /** The morph targets of this mesh that the expression plays. */
        std::vector<FNpcifyExpressionBind> Binds;
        /**
         * The expression has morph binds on another mesh of the model. An expression with binds
         * on any mesh plays by its binds only; the alias names do not play it.
         */
        bool bBoundOnOtherMesh = false;
        bool bIsBinary = false;
        ENpcifyExpressionOverride OverrideBlink = ENpcifyExpressionOverride::None;
        ENpcifyExpressionOverride OverrideLookAt = ENpcifyExpressionOverride::None;
    };

    class FNpcifyVisemePlayer
    {
    public:
        void LoadCues(const std::vector<FStagingVisemeCue>& InCues);
        void Clear();
        std::map<std::string, float> Evaluate(double LocalTimeSeconds, float FallbackRms = 0.0f) const;

    private:
        std::vector<FStagingVisemeCue> Cues;
    };

    class FNpcifyFacialMixer
    {
    public:
        FNpcifyFacialMixer();
        /** A mixer whose blink schedule takes the numbers of BlinkRandom (tests: a seeded stream). */
        explicit FNpcifyFacialMixer(std::function<double()> BlinkRandom);

        /**
         * The face blinks by itself, as people do (FNpcifyIdleBlinks): at irregular intervals,
         * more often while it speaks, and with a large gaze shift, a turn of the head (TrackHead)
         * or a new movement (RequestBlink).
         */
        bool bAutoBlinkEnabled = true;
        float SmoothingRate = 18.0f;

        static std::string NormalizeExpressionName(const std::string& Name);
        float GetWeight(const std::string& ExpressionName) const;
        const std::map<std::string, float>& GetWeights() const { return AppliedWeights; }
        /**
         * The index of an expression name or of one of its alias names in the list of the
         * expressions (neutral, happy, angry, sad, relaxed, surprised, aa, ih, ou, ee, oh, blink,
         * blinkLeft, blinkRight, lookUp, lookDown, lookLeft, lookRight); NpcifyExpressionCount
         * for a name that is not an expression.
         */
        static size_t ExpressionIndex(const std::string& Name);
        /**
         * The index of a preset expression of a VRM file in that list: a VRM 1 preset name
         * ("happy", "blinkLeft") or a VRM 0 preset name ("joy", "blink_l"), without regard to
         * case. NpcifyExpressionCount for every other name: a custom expression of a file is not
         * a preset, also when its name is an alias name of a morph target ("MouthSmileLeft").
         */
        static size_t PresetIndex(const std::string& Name);
        /** The weight of the expression at Index of that list: GetWeight without a name search. */
        float GetWeightAt(size_t Index) const { return Index < NpcifyExpressionCount ? AppliedList[Index] : 0.0f; }
        float GetMouthOpenness() const;

        /**
         * Asks for a blink because the character starts a movement (a new animation, a turn):
         * people blink at such moments. Chance is the part of such movements that bring a blink.
         * Nothing happens with auto blink off, while a blink plays, within 0.6 s after one, or
         * while a cue drives a blink expression. Returns whether a blink starts.
         */
        bool RequestBlink(float Chance = 1.0f);
        /** Takes the rotation of the head in the world. Call it in each frame: a fast and large turn brings a blink. */
        void TrackHead(float X, float Y, float Z, float W);
        /** The clock of the face: the sum of the time steps of Update. */
        double GetClockSeconds() const { return Clock; }
        /** The weight of the idle blink at the last Update, 0 to 1. */
        float GetIdleBlinkWeight() const { return IdleBlink; }
        FNpcifyIdleBlinks& GetIdleBlinks() { return IdleBlinks; }

        void SetLayerWeight(ENpcifyExpressionLayer Layer, const std::string& ExpressionName, float Weight);
        void ClearLayer(ENpcifyExpressionLayer Layer);

        void SetEmotion(const std::string& ExpressionName, float Weight = 1.0f);
        void ClearEmotion();

        void PlayCue(
            const std::string& ExpressionName,
            float Weight = 1.0f,
            float DurationSeconds = 1.2f,
            float FadeInSeconds = 0.15f,
            float FadeOutSeconds = 0.25f
        );

        void SetVisemeWeights(const std::map<std::string, float>& Visemes);
        void ClearVisemes();
        void SetMouthOpenness(float Openness);

        void SetGazeAngles(
            float YawDegrees,
            float PitchDegrees,
            float MaxYawDegrees = 35.0f,
            float MaxPitchDegrees = 25.0f
        );
        void SetGazeDirection(
            float LocalX,
            float LocalY,
            float LocalZ,
            float MaxYawDegrees = 35.0f,
            float MaxPitchDegrees = 25.0f
        );
        void ClearGaze();

        void Reset();
        void Update(float DeltaTimeSeconds);
        float ResolveMorphTargetWeight01(const std::string& MorphTargetName) const;

    private:
        struct FActiveCue
        {
            std::string Expression;
            float TargetWeight = 1.0f;
            float Duration = 1.2f;
            float FadeIn = 0.15f;
            float FadeOut = 0.25f;
            float Elapsed = 0.0f;
        };

        void UpdateIdleBlink(const std::map<std::string, float>& Targets);
        void StoreAppliedList();
        void WatchGaze(float YawDegrees, float PitchDegrees);
        static float Clamp01(float Value);

        std::map<ENpcifyExpressionLayer, std::map<std::string, float>> Layers;
        std::map<std::string, float> SmoothedWeights;
        // The smoothed weights with the idle blink: the weights that the face shows.
        std::map<std::string, float> AppliedWeights;
        // The applied weights in the order of the expression list.
        std::array<float, NpcifyExpressionCount> AppliedList{};
        std::vector<FActiveCue> ActiveCues;

        FNpcifyIdleBlinks IdleBlinks;
        double Clock = 0.0;
        float IdleBlink = 0.0f;
    };

    /**
     * What each morph target of one mesh plays. A face can have more than one target for the same
     * lids: a generated face has blink, blinkLeft, blinkRight and the ARKit eyeBlinkLeft and
     * eyeBlinkRight; a VRoid face has Fcl_EYE_Close and its two sides. Each of them closes the
     * lids by itself, so together they close them two or three times and the lids go through each
     * other. The plan plays one target for each lid: the both-eyes target for the part that both
     * lids share, and one target of each side for the rest. A target with the exact expression
     * name comes first (the target that the VRM's own expression binds), then the aliases in the
     * order of the alias table. Every other target plays as ResolveMorphTargetWeight01 gives it.
     *
     * A plan with the expressions of the VRM file plays as the Web SDK does: each expression
     * plays the morph targets that the file binds to it, with the weights of the file. An
     * expression that has binds does not play by alias names. An expression without binds (for
     * example the gaze of a face whose eyes are bones) keeps the alias names. The file's
     * "is binary" and its overrides of the blinks and of the gaze apply to the bound expressions.
     * A file that binds the two side blinks but not "blink" plays a blink on the two sides.
     */
    class FNpcifyMorphTargetPlan
    {
    public:
        FNpcifyMorphTargetPlan() = default;
        /** The plan of a mesh with the morph targets Names, in their order. */
        explicit FNpcifyMorphTargetPlan(const std::vector<std::string>& Names);
        /** The plan of a mesh with the morph targets Names and the expressions of its VRM file. */
        FNpcifyMorphTargetPlan(const std::vector<std::string>& Names, const std::vector<FNpcifyVrmExpression>& Expressions);

        size_t Num() const { return Names.size(); }
        /** The weight (0 to 1) of the morph target at Index. */
        float GetWeight(size_t Index, const FNpcifyFacialMixer& Mixer) const;

    private:
        enum class ERole : uint8_t
        {
            Other,
            Both,
            Left,
            Right,
            Muted,
        };

        struct FBound
        {
            size_t Expression = 0;
            float Weight = 1.0f;
        };

        struct FRule
        {
            bool bDefined = false;
            /** The expression has morph binds on a mesh of the model. */
            bool bBound = false;
            bool bIsBinary = false;
            ENpcifyExpressionOverride OverrideBlink = ENpcifyExpressionOverride::None;
            ENpcifyExpressionOverride OverrideLookAt = ENpcifyExpressionOverride::None;
        };

        bool Choose(const std::vector<const char*>& Aliases, ERole Role);
        float AliasWeight(size_t Index, const FNpcifyFacialMixer& Mixer) const;
        float BoundWeight(size_t Expression, const FNpcifyFacialMixer& Mixer) const;
        float OverrideFactor(bool bBlink, const FNpcifyFacialMixer& Mixer) const;

        std::vector<std::string> Names;
        std::vector<ERole> Roles;
        // The expression that the alias name of each target plays; NpcifyExpressionCount: none.
        std::vector<size_t> AliasExpressions;
        bool bHasBoth = false;
        // The binds of the VRM file for each target; empty without the expressions of a file.
        std::vector<std::vector<FBound>> Bound;
        std::array<FRule, NpcifyExpressionCount> Rules{};
        bool bHasExpressions = false;
        bool bHasOverrides = false;
    };
}
