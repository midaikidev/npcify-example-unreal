#pragma once

#include "Core/NpcifyModels.h"

#include <cstdint>
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

        bool bAutoBlinkEnabled = true;
        float SmoothingRate = 18.0f;

        static std::string NormalizeExpressionName(const std::string& Name);
        float GetWeight(const std::string& ExpressionName) const;
        const std::map<std::string, float>& GetWeights() const { return SmoothedWeights; }
        float GetMouthOpenness() const;

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

        void UpdateAutoBlink(float Dt);
        float NextRandom01();
        static float Clamp01(float Value);

        std::map<ENpcifyExpressionLayer, std::map<std::string, float>> Layers;
        std::map<std::string, float> SmoothedWeights;
        std::vector<FActiveCue> ActiveCues;

        float BlinkTimer = 0.0f;
        float NextBlinkInterval = 3.6f;
        float BlinkDuration = 0.14f;
        bool bIsBlinking = false;
        uint32_t RngState = 0x6D2B79F5u;
    };
}
