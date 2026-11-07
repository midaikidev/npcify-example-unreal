#pragma once

#include "Core/NpcifyModels.h"
#include "Core/NpcifyProtocol.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Npcify
{
    /** An engine-neutral vector for live motion math. */
    struct FNpcifyVec3
    {
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;

        FNpcifyVec3() = default;
        FNpcifyVec3(double InX, double InY, double InZ) : X(InX), Y(InY), Z(InZ) {}

        double Length() const;
        FNpcifyVec3 Normalized() const;
        FNpcifyVec3 operator+(const FNpcifyVec3& Other) const { return {X + Other.X, Y + Other.Y, Z + Other.Z}; }
        FNpcifyVec3 operator-(const FNpcifyVec3& Other) const { return {X - Other.X, Y - Other.Y, Z - Other.Z}; }
        FNpcifyVec3 operator*(double Scale) const { return {X * Scale, Y * Scale, Z * Scale}; }
        static double Dot(const FNpcifyVec3& A, const FNpcifyVec3& B);
        static FNpcifyVec3 Cross(const FNpcifyVec3& A, const FNpcifyVec3& B);
    };

    /** An engine-neutral rotation (x, y, z, w) for live motion math. */
    struct FNpcifyQuat
    {
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        double W = 1.0;

        FNpcifyQuat() = default;
        FNpcifyQuat(double InX, double InY, double InZ, double InW) : X(InX), Y(InY), Z(InZ), W(InW) {}

        /** The rotation Other followed by this one. */
        FNpcifyQuat operator*(const FNpcifyQuat& Other) const;
        FNpcifyQuat Normalized() const;
        FNpcifyQuat Inverse() const { return {-X, -Y, -Z, W}; }
        FNpcifyVec3 Rotate(const FNpcifyVec3& V) const;
        /** Spherical interpolation along the shorter arc. */
        static FNpcifyQuat Slerp(const FNpcifyQuat& A, const FNpcifyQuat& B, double T);
    };

    /** One motion_frames stream event of NPCP 1.1 (docs/realtime-motion.md). */
    struct FNpcifyMotionFramesEvent
    {
        std::string TaskId;
        std::string StagingId;
        int64_t Revision = 0;
        double Fps = 30.0;
        int64_t StartFrame = 0;
        int FrameCount = 0;
        std::vector<std::string> Bones;
        double RestHipsHeight = 1.0;
        /** FrameCount frames of (3 + 4 x Bones.size()) values. */
        std::shared_ptr<const std::vector<float>> Values;
    };

    /**
     * Shared constants and readers of live motion frames: the hips position in meters (Y up,
     * the body faces +Z), then one rotation (x, y, z, w) per bone in the VRM 1.0 normalized
     * humanoid space, relative to the VRM parent, every bone at rest in the T-pose.
     */
    class FNpcifyMotionFrames
    {
    public:
        static constexpr const char* Capability = "motion_frames";
        static constexpr int MaxFramesPerEvent = 3600;
        static constexpr int MaxBones = 64;

        /** The 22 bones of the server, hips to toes (no fingers, eyes or jaw). */
        static const std::vector<std::string>& Bones();
        /** The VRM parent of a bone, or an empty string for the hips. */
        static const std::string& ParentOf(const std::string& Bone);
        static int FrameWidth(int BoneCount) { return 3 + 4 * BoneCount; }

        /** Reads little-endian float32 frames; throws when the length does not match. */
        static std::vector<float> Read(const uint8_t* Data, size_t Length, int FrameCount, int BoneCount);

        /** Reads a motion_frames event; false for another event or an invalid one. */
        static bool TryParseEvent(const FNpcifyMsgPackValue& Event, FNpcifyMotionFramesEvent& Out);

        /** Reads timeline.motion; false when it is absent or invalid. */
        static bool TryParseTrack(const FNpcifyMsgPackValue& Track, FStagingMotionTrack& Out);
    };

    enum class ENpcifyLiveMotionRootMode : uint8_t
    {
        /** Removes the horizontal hips position of the turn's start; the character stays where the game put it. */
        Anchored,
        /** Applies the generated root movement. */
        World,
    };

    struct FNpcifyLiveMotionOptions
    {
        bool bEnabled = true;
        ENpcifyLiveMotionRootMode RootMode = ENpcifyLiveMotionRootMode::Anchored;
        double BlendInSeconds = 0.3;
        double BlendOutSeconds = 0.5;
    };

    /** Counters of the live frames of one staging, for logs and tests. */
    struct FNpcifyLiveMotionStats
    {
        std::string StagingId;
        int FramesReceived = 0;
        int FramesPlayed = 0;
        /** Staging time in seconds of the first applied live frame, or -1. */
        double FirstFrameTime = -1.0;
        int64_t LastRevision = -1;
    };

    /**
     * Keeps the live motion frames of each staging and samples the pose of the staging clock,
     * like the Web SDK's LiveMotionPlayer (ADR 0011). Engine code reads the sampled pose and
     * blends it over its animation with GetWeight(). Use it from one thread.
     */
    class FNpcifyLiveMotionPlayer
    {
    public:
        static constexpr size_t MaxTracks = 4;

        explicit FNpcifyLiveMotionPlayer(const FNpcifyLiveMotionOptions& InOptions = FNpcifyLiveMotionOptions());

        const FNpcifyLiveMotionOptions& GetOptions() const { return Options; }
        void SetRootMode(ENpcifyLiveMotionRootMode Mode) { Options.RootMode = Mode; }

        /** Raised with the staging ID and its staging time when its first live frame plays. */
        std::function<void(const std::string&, double)> OnFirstFramePlayed;

        bool Push(const FNpcifyMotionFramesEvent& Event);
        /** Loads the stored frames of a completed staging (a replay of timeline.motion). */
        void Load(const std::string& StagingId, const FStagingMotionTrack& Track, const std::vector<uint8_t>& Bytes);
        bool Has(const std::string& StagingId) const;
        void Forget(const std::string& StagingId);
        void Clear();
        bool GetStats(const std::string& StagingId, FNpcifyLiveMotionStats& Out) const;

        /** Samples StagingId at Time seconds of its staging clock; Time below 0: no staging plays. */
        void Update(double DeltaSeconds, const std::string& StagingId, double Time);

        double GetWeight() const { return Weight; }
        bool HasPose() const { return PoseTrack != nullptr && Weight > 0.0; }
        double GetRestHipsHeight() const;
        std::string GetPoseStagingId() const;

        /** The rotation of a bone in the current pose (glTF axes); identity for a bone the frames lack. */
        bool TryGetRotation(const std::string& Bone, FNpcifyQuat& Out) const;
        /** The hips position in meters (glTF axes); anchored mode subtracts the turn's first sampled frame. */
        FNpcifyVec3 GetRawHipsPosition() const;

    private:
        struct FFrame
        {
            int64_t Revision = 0;
            std::shared_ptr<const std::vector<float>> Values;
            size_t Offset = 0;
        };

        struct FTrack
        {
            std::string StagingId;
            double Fps = 30.0;
            std::map<std::string, int> Slots;
            double RestHipsHeight = 1.0;
            int Width = 0;
            std::map<int64_t, FFrame> Frames;
            bool bHasOrigin = false;
            double OriginX = 0.0;
            double OriginZ = 0.0;
            FNpcifyLiveMotionStats Stats;
        };

        FTrack& GetOrCreate(const std::string& StagingId, double Fps, const std::vector<std::string>& Bones, double RestHipsHeight);
        bool Sample(FTrack& Track, double Time);

        FNpcifyLiveMotionOptions Options;
        std::map<std::string, std::unique_ptr<FTrack>> Tracks;
        std::vector<std::string> Order;
        const FTrack* PoseTrack = nullptr;
        std::shared_ptr<const std::vector<float>> PoseValues;
        size_t PoseOffset = 0;
        double Weight = 0.0;
    };

    /**
     * Maps live motion frames (glTF axes: Y up, the body faces +Z, its left is +X) onto a target
     * rig. It reads the target's axes from its T-pose: up from the feet to the upper legs, left
     * from the right to the left upper arm, forward from the feet to the toes. So no table of
     * engine axes is needed (Unreal, VRM4U, a model that faces another way).
     */
    class FNpcifyLiveMotionRig
    {
    public:
        /**
         * Builds the rig from the T-pose positions (target space) of the target's humanoid bones,
         * keyed by VRM bone name, and the model origin (the floor under the model). It needs the
         * hips, the head and both upper arms; throws std::invalid_argument otherwise.
         */
        FNpcifyLiveMotionRig(const std::map<std::string, FNpcifyVec3>& TPose, const FNpcifyVec3& Origin);

        bool Has(const std::string& Bone) const { return Present.count(Bone) > 0; }
        /** The nearest present VRM ancestor of a present bone; empty for the hips. */
        std::string ParentOf(const std::string& Bone) const;
        /** The present bones of the 22 frame bones, parents first. */
        std::vector<std::string> PresentBones() const;

        const FNpcifyVec3& GetLeft() const { return Left; }
        const FNpcifyVec3& GetUp() const { return Up; }
        const FNpcifyVec3& GetForward() const { return Forward; }
        double GetHandedness() const { return Handedness; }
        double GetHipsHeight() const { return HipsHeight; }

        FNpcifyVec3 ToTarget(const FNpcifyVec3& V) const;
        FNpcifyQuat ToTarget(const FNpcifyQuat& Q) const;

        /** The live rotation of a present bone relative to its present parent, in target space. */
        bool TryGetLiveLocalRotation(const FNpcifyLiveMotionPlayer& Player, const std::string& Bone, FNpcifyQuat& Out) const;
        /** The live hips position in target space, scaled by the rest hips heights. */
        FNpcifyVec3 GetLiveHipsPosition(const FNpcifyLiveMotionPlayer& Player) const;

    private:
        std::set<std::string> Present;
        std::map<std::string, std::string> Parents;
        FNpcifyVec3 Left;
        FNpcifyVec3 Up;
        FNpcifyVec3 Forward;
        double Handedness = -1.0;
        FNpcifyVec3 Origin;
        FNpcifyVec3 RestHips;
        double HipsHeight = 1.0;
    };
}
