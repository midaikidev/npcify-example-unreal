#include "Core/NpcifyLiveMotion.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace Npcify
{
    namespace
    {
        const std::map<std::string, std::string>& ParentTable()
        {
            static const std::map<std::string, std::string> Table = {
                {"spine", "hips"},
                {"chest", "spine"},
                {"upperChest", "chest"},
                {"neck", "upperChest"},
                {"head", "neck"},
                {"leftShoulder", "upperChest"},
                {"leftUpperArm", "leftShoulder"},
                {"leftLowerArm", "leftUpperArm"},
                {"leftHand", "leftLowerArm"},
                {"rightShoulder", "upperChest"},
                {"rightUpperArm", "rightShoulder"},
                {"rightLowerArm", "rightUpperArm"},
                {"rightHand", "rightLowerArm"},
                {"leftUpperLeg", "hips"},
                {"leftLowerLeg", "leftUpperLeg"},
                {"leftFoot", "leftLowerLeg"},
                {"leftToes", "leftFoot"},
                {"rightUpperLeg", "hips"},
                {"rightLowerLeg", "rightUpperLeg"},
                {"rightFoot", "rightLowerLeg"},
                {"rightToes", "rightFoot"},
            };
            return Table;
        }

        bool ReadCount(const FNpcifyMsgPackValue& Map, const char* Key, int64_t Max, int64_t& Out)
        {
            const FNpcifyMsgPackValue* Field = Map.FindField(Key);
            if (!Field)
            {
                return false;
            }
            if (Field->IsInt())
            {
                Out = Field->IntValue;
            }
            else if (Field->IsFloat() && std::floor(Field->FloatValue) == Field->FloatValue &&
                     std::fabs(Field->FloatValue) < 9.0e15)
            {
                Out = static_cast<int64_t>(Field->FloatValue);
            }
            else
            {
                return false;
            }
            return Out >= 0 && Out <= Max;
        }

        double ReadNumber(const FNpcifyMsgPackValue& Map, const char* Key)
        {
            const FNpcifyMsgPackValue* Field = Map.FindField(Key);
            return Field && Field->IsNumber() ? Field->AsDouble(NAN) : NAN;
        }

        bool ReadBones(const FNpcifyMsgPackValue& Map, std::vector<std::string>& Out)
        {
            const FNpcifyMsgPackValue* Field = Map.FindField("bones");
            if (!Field || !Field->IsArray() || Field->ArrayValue.empty() ||
                Field->ArrayValue.size() > static_cast<size_t>(FNpcifyMotionFrames::MaxBones))
            {
                return false;
            }
            Out.clear();
            for (const FNpcifyMsgPackValue& Bone : Field->ArrayValue)
            {
                if (!Bone.IsString())
                {
                    return false;
                }
                Out.push_back(Bone.StringValue);
            }
            return true;
        }

        double Approach(double Value, double Target, double Delta, double Seconds)
        {
            if (Seconds <= 0.0)
            {
                return Target;
            }
            const double Step = Delta / Seconds;
            return Value < Target ? std::min(Target, Value + Step) : std::max(Target, Value - Step);
        }
    }

    double FNpcifyVec3::Length() const
    {
        return std::sqrt(X * X + Y * Y + Z * Z);
    }

    FNpcifyVec3 FNpcifyVec3::Normalized() const
    {
        const double Size = Length();
        return Size > 1e-12 ? FNpcifyVec3(X / Size, Y / Size, Z / Size) : FNpcifyVec3();
    }

    double FNpcifyVec3::Dot(const FNpcifyVec3& A, const FNpcifyVec3& B)
    {
        return A.X * B.X + A.Y * B.Y + A.Z * B.Z;
    }

    FNpcifyVec3 FNpcifyVec3::Cross(const FNpcifyVec3& A, const FNpcifyVec3& B)
    {
        return {A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X};
    }

    FNpcifyQuat FNpcifyQuat::operator*(const FNpcifyQuat& B) const
    {
        return {
            W * B.X + X * B.W + Y * B.Z - Z * B.Y,
            W * B.Y - X * B.Z + Y * B.W + Z * B.X,
            W * B.Z + X * B.Y - Y * B.X + Z * B.W,
            W * B.W - X * B.X - Y * B.Y - Z * B.Z,
        };
    }

    FNpcifyQuat FNpcifyQuat::Normalized() const
    {
        const double Size = std::sqrt(X * X + Y * Y + Z * Z + W * W);
        return Size > 1e-12 ? FNpcifyQuat(X / Size, Y / Size, Z / Size, W / Size) : FNpcifyQuat();
    }

    FNpcifyVec3 FNpcifyQuat::Rotate(const FNpcifyVec3& V) const
    {
        const double TX = 2.0 * (Y * V.Z - Z * V.Y);
        const double TY = 2.0 * (Z * V.X - X * V.Z);
        const double TZ = 2.0 * (X * V.Y - Y * V.X);
        return {
            V.X + W * TX + (Y * TZ - Z * TY),
            V.Y + W * TY + (Z * TX - X * TZ),
            V.Z + W * TZ + (X * TY - Y * TX),
        };
    }

    FNpcifyQuat FNpcifyQuat::Slerp(const FNpcifyQuat& A, const FNpcifyQuat& InB, double T)
    {
        if (T <= 0.0)
        {
            return A;
        }
        if (T >= 1.0)
        {
            return InB;
        }
        FNpcifyQuat B = InB;
        double Cos = A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W;
        if (Cos < 0.0)
        {
            Cos = -Cos;
            B = FNpcifyQuat(-B.X, -B.Y, -B.Z, -B.W);
        }
        double WA = 1.0 - T;
        double WB = T;
        if (Cos <= 0.9995)
        {
            const double Angle = std::acos(std::min(1.0, Cos));
            const double Sin = std::sin(Angle);
            WA = std::sin((1.0 - T) * Angle) / Sin;
            WB = std::sin(T * Angle) / Sin;
        }
        return FNpcifyQuat(A.X * WA + B.X * WB, A.Y * WA + B.Y * WB, A.Z * WA + B.Z * WB, A.W * WA + B.W * WB).Normalized();
    }

    const std::vector<std::string>& FNpcifyMotionFrames::Bones()
    {
        static const std::vector<std::string> List = {
            "hips", "spine", "chest", "upperChest", "neck", "head",
            "leftShoulder", "leftUpperArm", "leftLowerArm", "leftHand",
            "rightShoulder", "rightUpperArm", "rightLowerArm", "rightHand",
            "leftUpperLeg", "leftLowerLeg", "leftFoot", "leftToes",
            "rightUpperLeg", "rightLowerLeg", "rightFoot", "rightToes",
        };
        return List;
    }

    const std::string& FNpcifyMotionFrames::ParentOf(const std::string& Bone)
    {
        static const std::string None;
        const auto& Table = ParentTable();
        const auto Found = Table.find(Bone);
        return Found != Table.end() ? Found->second : None;
    }

    std::vector<float> FNpcifyMotionFrames::Read(const uint8_t* Data, size_t Length, int FrameCount, int BoneCount)
    {
        if (FrameCount < 0 || BoneCount <= 0 || BoneCount > MaxBones)
        {
            throw std::runtime_error("Motion frames have an invalid shape.");
        }
        const size_t Count = static_cast<size_t>(FrameCount) * static_cast<size_t>(FrameWidth(BoneCount));
        if (Length != Count * 4 || (Count > 0 && Data == nullptr))
        {
            throw std::runtime_error("Motion frames do not match their frame count.");
        }
        std::vector<float> Values(Count);
        for (size_t Index = 0; Index < Count; ++Index)
        {
            const uint8_t* Word = Data + Index * 4;
            const uint32_t Bits = static_cast<uint32_t>(Word[0]) | (static_cast<uint32_t>(Word[1]) << 8) |
                (static_cast<uint32_t>(Word[2]) << 16) | (static_cast<uint32_t>(Word[3]) << 24);
            float Value = 0.0f;
            std::memcpy(&Value, &Bits, sizeof(Value));
            Values[Index] = Value;
        }
        return Values;
    }

    bool FNpcifyMotionFrames::TryParseEvent(const FNpcifyMsgPackValue& Event, FNpcifyMotionFramesEvent& Out)
    {
        if (!Event.IsMap())
        {
            return false;
        }
        const FNpcifyMsgPackValue* Type = Event.FindField("type");
        const FNpcifyMsgPackValue* TaskId = Event.FindField("taskId");
        const FNpcifyMsgPackValue* StagingId = Event.FindField("stagingId");
        const FNpcifyMsgPackValue* Frames = Event.FindField("frames");
        int64_t Revision = 0;
        int64_t StartFrame = 0;
        int64_t FrameCount = 0;
        std::vector<std::string> BoneNames;
        const double Fps = ReadNumber(Event, "fps");
        const double RestHipsHeight = ReadNumber(Event, "restHipsHeight");
        if (!Type || !Type->IsString() || Type->StringValue != "motion_frames" || !TaskId || !TaskId->IsString() ||
            !StagingId || !StagingId->IsString() || !Frames || !Frames->IsBinary() ||
            !ReadCount(Event, "revision", FNpcifyProtocol::MaxSafeInteger, Revision) ||
            !ReadCount(Event, "startFrame", 24LL * 60 * 60 * 240, StartFrame) ||
            !ReadCount(Event, "frameCount", MaxFramesPerEvent, FrameCount) ||
            !(Fps > 0.0 && Fps <= 240.0) || !(RestHipsHeight > 0.0 && std::isfinite(RestHipsHeight)) ||
            !ReadBones(Event, BoneNames) ||
            Frames->BinaryValue.size() !=
                static_cast<size_t>(FrameCount) * static_cast<size_t>(FrameWidth(static_cast<int>(BoneNames.size()))) * 4)
        {
            return false;
        }
        Out.TaskId = TaskId->StringValue;
        Out.StagingId = StagingId->StringValue;
        Out.Revision = Revision;
        Out.Fps = Fps;
        Out.StartFrame = StartFrame;
        Out.FrameCount = static_cast<int>(FrameCount);
        Out.Bones = BoneNames;
        Out.RestHipsHeight = RestHipsHeight;
        Out.Values = std::make_shared<const std::vector<float>>(Read(
            Frames->BinaryValue.data(),
            Frames->BinaryValue.size(),
            Out.FrameCount,
            static_cast<int>(BoneNames.size())));
        return true;
    }

    bool FNpcifyMotionFrames::TryParseTrack(const FNpcifyMsgPackValue& Track, FStagingMotionTrack& Out)
    {
        if (!Track.IsMap())
        {
            return false;
        }
        int64_t FrameCount = 0;
        std::vector<std::string> BoneNames;
        const double Fps = ReadNumber(Track, "fps");
        const double RestHipsHeight = ReadNumber(Track, "restHipsHeight");
        const FNpcifyMsgPackValue* Frames = Track.FindField("frames");
        if (!(Fps > 0.0 && Fps <= 240.0) || !(RestHipsHeight > 0.0) ||
            !ReadCount(Track, "frameCount", 1LL << 30, FrameCount) || !ReadBones(Track, BoneNames) ||
            !Frames || !Frames->IsMap())
        {
            return false;
        }
        FStagingMotionTrack Result;
        Result.Fps = Fps;
        Result.FrameCount = static_cast<int>(FrameCount);
        Result.Bones = BoneNames;
        Result.RestHipsHeight = RestHipsHeight;
        const FNpcifyMsgPackValue* Kind = Frames->FindField("type");
        const FNpcifyMsgPackValue* Data = Frames->FindField("data");
        const FNpcifyMsgPackValue* Url = Frames->FindField("url");
        if (Kind && Kind->IsString() && Kind->StringValue == "bytes" && Data && Data->IsBinary())
        {
            Result.FramesData = Data->BinaryValue;
        }
        else if (Url && Url->IsString() && !Url->StringValue.empty())
        {
            Result.FramesUrl = Url->StringValue;
        }
        else
        {
            return false;
        }
        Result.bValid = true;
        Out = std::move(Result);
        return true;
    }

    FNpcifyLiveMotionPlayer::FNpcifyLiveMotionPlayer(const FNpcifyLiveMotionOptions& InOptions)
        : Options(InOptions)
    {
    }

    FNpcifyLiveMotionPlayer::FTrack& FNpcifyLiveMotionPlayer::GetOrCreate(
        const std::string& StagingId,
        double Fps,
        const std::vector<std::string>& BoneNames,
        double RestHipsHeight)
    {
        const auto Found = Tracks.find(StagingId);
        if (Found != Tracks.end())
        {
            return *Found->second;
        }
        auto Track = std::make_unique<FTrack>();
        Track->StagingId = StagingId;
        Track->Fps = Fps;
        Track->RestHipsHeight = RestHipsHeight > 0.0 ? RestHipsHeight : 1.0;
        Track->Width = FNpcifyMotionFrames::FrameWidth(static_cast<int>(BoneNames.size()));
        Track->Stats.StagingId = StagingId;
        for (size_t Slot = 0; Slot < BoneNames.size(); ++Slot)
        {
            Track->Slots[BoneNames[Slot]] = static_cast<int>(Slot);
        }
        FTrack& Result = *Track;
        Tracks[StagingId] = std::move(Track);
        Order.push_back(StagingId);
        while (Order.size() > MaxTracks && Order.front() != StagingId)
        {
            const std::string Oldest = Order.front();
            Order.erase(Order.begin());
            if (PoseTrack == Tracks[Oldest].get())
            {
                PoseTrack = nullptr;
                PoseValues.reset();
                Weight = 0.0;
            }
            Tracks.erase(Oldest);
        }
        return Result;
    }

    bool FNpcifyLiveMotionPlayer::Push(const FNpcifyMotionFramesEvent& Event)
    {
        if (Event.StagingId.empty() || Event.Bones.empty() || !Event.Values || Event.Fps <= 0.0 ||
            Event.Values->size() != static_cast<size_t>(Event.FrameCount) *
                static_cast<size_t>(FNpcifyMotionFrames::FrameWidth(static_cast<int>(Event.Bones.size()))))
        {
            return false;
        }
        FTrack& Track = GetOrCreate(Event.StagingId, Event.Fps, Event.Bones, Event.RestHipsHeight);
        for (int Frame = 0; Frame < Event.FrameCount; ++Frame)
        {
            const int64_t Index = Event.StartFrame + Frame;
            const auto Existing = Track.Frames.find(Index);
            if (Existing != Track.Frames.end())
            {
                if (Existing->second.Revision > Event.Revision)
                {
                    continue;
                }
            }
            else
            {
                ++Track.Stats.FramesReceived;
            }
            FFrame& Stored = Track.Frames[Index];
            Stored.Revision = Event.Revision;
            Stored.Values = Event.Values;
            Stored.Offset = static_cast<size_t>(Frame) * static_cast<size_t>(Track.Width);
        }
        Track.Stats.LastRevision = std::max(Track.Stats.LastRevision, Event.Revision);
        return true;
    }

    void FNpcifyLiveMotionPlayer::Load(
        const std::string& StagingId,
        const FStagingMotionTrack& Stored,
        const std::vector<uint8_t>& Bytes)
    {
        if (StagingId.empty() || Tracks.count(StagingId) > 0)
        {
            return;
        }
        const std::vector<uint8_t>& Source = Bytes.empty() ? Stored.FramesData : Bytes;
        auto Values = std::make_shared<const std::vector<float>>(FNpcifyMotionFrames::Read(
            Source.data(),
            Source.size(),
            Stored.FrameCount,
            static_cast<int>(Stored.Bones.size())));
        FTrack& Track = GetOrCreate(StagingId, Stored.Fps, Stored.Bones, Stored.RestHipsHeight);
        for (int Frame = 0; Frame < Stored.FrameCount; ++Frame)
        {
            FFrame& Item = Track.Frames[Frame];
            Item.Revision = FNpcifyProtocol::MaxSafeInteger;
            Item.Values = Values;
            Item.Offset = static_cast<size_t>(Frame) * static_cast<size_t>(Track.Width);
        }
        Track.Stats.FramesReceived = Stored.FrameCount;
    }

    bool FNpcifyLiveMotionPlayer::Has(const std::string& StagingId) const
    {
        const auto Found = Tracks.find(StagingId);
        return Found != Tracks.end() && !Found->second->Frames.empty();
    }

    void FNpcifyLiveMotionPlayer::Forget(const std::string& StagingId)
    {
        const auto Found = Tracks.find(StagingId);
        if (Found == Tracks.end())
        {
            return;
        }
        if (PoseTrack == Found->second.get())
        {
            PoseTrack = nullptr;
            PoseValues.reset();
            Weight = 0.0;
        }
        Tracks.erase(Found);
        Order.erase(std::remove(Order.begin(), Order.end(), StagingId), Order.end());
    }

    void FNpcifyLiveMotionPlayer::Clear()
    {
        Tracks.clear();
        Order.clear();
        PoseTrack = nullptr;
        PoseValues.reset();
        Weight = 0.0;
    }

    bool FNpcifyLiveMotionPlayer::GetStats(const std::string& StagingId, FNpcifyLiveMotionStats& Out) const
    {
        const auto Found = Tracks.find(StagingId);
        if (Found == Tracks.end())
        {
            return false;
        }
        Out = Found->second->Stats;
        return true;
    }

    void FNpcifyLiveMotionPlayer::Update(double DeltaSeconds, const std::string& StagingId, double Time)
    {
        FTrack* Track = nullptr;
        bool bSampled = false;
        if (!StagingId.empty() && Time >= 0.0)
        {
            const auto Found = Tracks.find(StagingId);
            if (Found != Tracks.end())
            {
                Track = Found->second.get();
                bSampled = Sample(*Track, Time);
            }
        }
        const double Delta = std::max(0.0, DeltaSeconds);
        Weight = Approach(Weight, bSampled ? 1.0 : 0.0, Delta, bSampled ? Options.BlendInSeconds : Options.BlendOutSeconds);
        if (bSampled)
        {
            if (Track->Stats.FramesPlayed == 0)
            {
                Track->Stats.FirstFrameTime = Time;
                if (OnFirstFramePlayed)
                {
                    OnFirstFramePlayed(Track->StagingId, Time);
                }
            }
            ++Track->Stats.FramesPlayed;
        }
        else if (Weight <= 0.0)
        {
            // Keep the last pose while the weight blends out of it.
            PoseTrack = nullptr;
            PoseValues.reset();
        }
    }

    bool FNpcifyLiveMotionPlayer::Sample(FTrack& Track, double Time)
    {
        const double Position = Time * Track.Fps;
        const int64_t First = static_cast<int64_t>(std::floor(Position));
        const auto A = Track.Frames.find(First);
        if (A == Track.Frames.end())
        {
            return false;
        }
        const std::vector<float>& AValues = *A->second.Values;
        const size_t AOffset = A->second.Offset;
        if (!Track.bHasOrigin)
        {
            Track.bHasOrigin = true;
            Track.OriginX = AValues[AOffset];
            Track.OriginZ = AValues[AOffset + 2];
        }
        PoseTrack = &Track;
        const double Amount = Position - static_cast<double>(First);
        const auto B = Track.Frames.find(First + 1);
        if (Amount <= 0.0 || B == Track.Frames.end())
        {
            PoseValues = A->second.Values;
            PoseOffset = AOffset;
            return true;
        }
        const std::vector<float>& BValues = *B->second.Values;
        const size_t BOffset = B->second.Offset;
        const size_t Width = static_cast<size_t>(Track.Width);
        auto Blended = std::make_shared<std::vector<float>>(Width);
        for (size_t Index = 0; Index < 3; ++Index)
        {
            (*Blended)[Index] = static_cast<float>(AValues[AOffset + Index] * (1.0 - Amount) + BValues[BOffset + Index] * Amount);
        }
        for (size_t Offset = 3; Offset + 4 <= Width; Offset += 4)
        {
            const FNpcifyQuat From(AValues[AOffset + Offset], AValues[AOffset + Offset + 1], AValues[AOffset + Offset + 2], AValues[AOffset + Offset + 3]);
            const FNpcifyQuat To(BValues[BOffset + Offset], BValues[BOffset + Offset + 1], BValues[BOffset + Offset + 2], BValues[BOffset + Offset + 3]);
            const FNpcifyQuat Q = FNpcifyQuat::Slerp(From, To, Amount);
            (*Blended)[Offset] = static_cast<float>(Q.X);
            (*Blended)[Offset + 1] = static_cast<float>(Q.Y);
            (*Blended)[Offset + 2] = static_cast<float>(Q.Z);
            (*Blended)[Offset + 3] = static_cast<float>(Q.W);
        }
        PoseValues = Blended;
        PoseOffset = 0;
        return true;
    }

    double FNpcifyLiveMotionPlayer::GetRestHipsHeight() const
    {
        return PoseTrack ? PoseTrack->RestHipsHeight : 1.0;
    }

    std::string FNpcifyLiveMotionPlayer::GetPoseStagingId() const
    {
        return PoseTrack ? PoseTrack->StagingId : std::string();
    }

    bool FNpcifyLiveMotionPlayer::TryGetRotation(const std::string& Bone, FNpcifyQuat& Out) const
    {
        Out = FNpcifyQuat();
        if (!PoseTrack || !PoseValues)
        {
            return false;
        }
        const auto Slot = PoseTrack->Slots.find(Bone);
        if (Slot != PoseTrack->Slots.end())
        {
            const std::vector<float>& Values = *PoseValues;
            const size_t Offset = PoseOffset + 3 + static_cast<size_t>(Slot->second) * 4;
            Out = FNpcifyQuat(Values[Offset], Values[Offset + 1], Values[Offset + 2], Values[Offset + 3]).Normalized();
        }
        return true;
    }

    FNpcifyVec3 FNpcifyLiveMotionPlayer::GetRawHipsPosition() const
    {
        if (!PoseTrack || !PoseValues)
        {
            return {0.0, GetRestHipsHeight(), 0.0};
        }
        const std::vector<float>& Values = *PoseValues;
        double X = Values[PoseOffset];
        const double Y = Values[PoseOffset + 1];
        double Z = Values[PoseOffset + 2];
        if (Options.RootMode == ENpcifyLiveMotionRootMode::Anchored && PoseTrack->bHasOrigin)
        {
            X -= PoseTrack->OriginX;
            Z -= PoseTrack->OriginZ;
        }
        return {X, Y, Z};
    }

    FNpcifyLiveMotionRig::FNpcifyLiveMotionRig(const std::map<std::string, FNpcifyVec3>& TPose, const FNpcifyVec3& InOrigin)
        : Origin(InOrigin)
    {
        const auto Find = [&TPose](const char* Bone, FNpcifyVec3& Out)
        {
            const auto Found = TPose.find(Bone);
            if (Found == TPose.end())
            {
                return false;
            }
            Out = Found->second;
            return true;
        };
        FNpcifyVec3 Hips;
        FNpcifyVec3 Head;
        FNpcifyVec3 LeftArm;
        FNpcifyVec3 RightArm;
        if (!Find("hips", Hips) || !Find("head", Head) || !Find("leftUpperArm", LeftArm) || !Find("rightUpperArm", RightArm))
        {
            throw std::invalid_argument("Live motion needs the hips, the head and both upper arms of the rig.");
        }
        // The legs hang straight down in a VRM T-pose; the spine can lean.
        FNpcifyVec3 Legs;
        FNpcifyVec3 Thigh;
        FNpcifyVec3 Ankle;
        if (Find("leftUpperLeg", Thigh) && Find("leftFoot", Ankle))
        {
            Legs = Legs + (Thigh - Ankle);
        }
        if (Find("rightUpperLeg", Thigh) && Find("rightFoot", Ankle))
        {
            Legs = Legs + (Thigh - Ankle);
        }
        Up = (Legs.Length() > 1e-6 ? Legs : Head - Hips).Normalized();
        const FNpcifyVec3 Across = LeftArm - RightArm;
        Left = (Across - Up * FNpcifyVec3::Dot(Across, Up)).Normalized();
        if (Up.Length() < 0.5 || Left.Length() < 0.5)
        {
            throw std::invalid_argument("The rig is not in a T-pose: its axes cannot be read.");
        }
        const FNpcifyVec3 CrossAxis = FNpcifyVec3::Cross(Left, Up);
        // Unreal and Unity are left-handed against glTF; the toes confirm the facing direction.
        Handedness = -1.0;
        FNpcifyVec3 Feet;
        int FeetCount = 0;
        FNpcifyVec3 Foot;
        FNpcifyVec3 Toes;
        if (Find("leftFoot", Foot) && Find("leftToes", Toes))
        {
            Feet = Feet + (Toes - Foot);
            ++FeetCount;
        }
        if (Find("rightFoot", Foot) && Find("rightToes", Toes))
        {
            Feet = Feet + (Toes - Foot);
            ++FeetCount;
        }
        if (FeetCount > 0)
        {
            const FNpcifyVec3 Flat = Feet - Up * FNpcifyVec3::Dot(Feet, Up);
            const double Facing = FNpcifyVec3::Dot(CrossAxis, Flat.Normalized());
            if (std::fabs(Facing) > 0.25)
            {
                Handedness = Facing > 0.0 ? 1.0 : -1.0;
            }
        }
        Forward = CrossAxis * Handedness;
        RestHips = Hips;
        HipsHeight = FNpcifyVec3::Dot(Hips - Origin, Up);
        for (const std::string& Bone : FNpcifyMotionFrames::Bones())
        {
            if (TPose.count(Bone) > 0)
            {
                Present.insert(Bone);
            }
        }
        for (const std::string& Bone : FNpcifyMotionFrames::Bones())
        {
            if (Present.count(Bone) == 0 || Bone == "hips")
            {
                continue;
            }
            std::string Parent = FNpcifyMotionFrames::ParentOf(Bone);
            while (!Parent.empty() && Present.count(Parent) == 0)
            {
                Parent = FNpcifyMotionFrames::ParentOf(Parent);
            }
            Parents[Bone] = Parent;
        }
    }

    std::string FNpcifyLiveMotionRig::ParentOf(const std::string& Bone) const
    {
        const auto Found = Parents.find(Bone);
        return Found != Parents.end() ? Found->second : std::string();
    }

    std::vector<std::string> FNpcifyLiveMotionRig::PresentBones() const
    {
        std::vector<std::string> Result;
        for (const std::string& Bone : FNpcifyMotionFrames::Bones())
        {
            if (Present.count(Bone) > 0)
            {
                Result.push_back(Bone);
            }
        }
        return Result;
    }

    FNpcifyVec3 FNpcifyLiveMotionRig::ToTarget(const FNpcifyVec3& V) const
    {
        return Left * V.X + Up * V.Y + Forward * V.Z;
    }

    FNpcifyQuat FNpcifyLiveMotionRig::ToTarget(const FNpcifyQuat& Q) const
    {
        const FNpcifyVec3 Axis = ToTarget(FNpcifyVec3(Q.X, Q.Y, Q.Z)) * Handedness;
        return {Axis.X, Axis.Y, Axis.Z, Q.W};
    }

    bool FNpcifyLiveMotionRig::TryGetLiveLocalRotation(
        const FNpcifyLiveMotionPlayer& Player,
        const std::string& Bone,
        FNpcifyQuat& Out) const
    {
        Out = FNpcifyQuat();
        FNpcifyQuat Local;
        if (Present.count(Bone) == 0 || !Player.TryGetRotation(Bone, Local))
        {
            return false;
        }
        // A missing VRM bone between the bone and its present parent gives its rotation to the bone.
        const std::string Stop = ParentOf(Bone);
        FNpcifyQuat Chain = Local;
        std::string Node = FNpcifyMotionFrames::ParentOf(Bone);
        while (!Node.empty() && Node != Stop)
        {
            FNpcifyQuat Missing;
            Player.TryGetRotation(Node, Missing);
            Chain = Missing * Chain;
            Node = FNpcifyMotionFrames::ParentOf(Node);
        }
        Out = ToTarget(Chain);
        return true;
    }

    FNpcifyVec3 FNpcifyLiveMotionRig::GetLiveHipsPosition(const FNpcifyLiveMotionPlayer& Player) const
    {
        const FNpcifyVec3 Raw = Player.GetRawHipsPosition();
        const double Scale = HipsHeight / std::max(1e-6, Player.GetRestHipsHeight());
        const FNpcifyVec3 Offset = ToTarget(Raw * Scale);
        if (Player.GetOptions().RootMode == ENpcifyLiveMotionRootMode::World)
        {
            return Origin + Offset;
        }
        const FNpcifyVec3 Rest = RestHips - Origin;
        const FNpcifyVec3 FlatRest = Rest - Up * FNpcifyVec3::Dot(Rest, Up);
        return Origin + FlatRest + Offset;
    }
}
