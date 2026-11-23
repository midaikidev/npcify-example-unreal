#include "Core/NpcifyIdleBlinks.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>

namespace Npcify
{
    namespace IdleBlinkNumbers
    {
        /** The shape of the interval distribution (log-logistic): larger is more regular. */
        constexpr double IntervalShape = 4.0;
        constexpr double CloseSeconds = 0.08;
        constexpr double HoldSeconds = 0.05;
        constexpr double OpenSeconds = FNpcifyIdleBlinks::BlinkSeconds - CloseSeconds - HoldSeconds;
        /** The lids open fast at first and slowly at the end. */
        constexpr double OpenEase = 1.7;
        /** One blink takes 0.85 to 1.25 times the usual length. */
        constexpr double ShortestScale = 0.85;
        constexpr double LongestScale = 1.25;
        /** The second blink starts 80 to 200 ms after the end of the first. */
        constexpr double DoubleMinGapSeconds = 0.08;
        constexpr double DoubleMaxGapSeconds = 0.2;
        constexpr double DoubleScale = 0.85;
        /** A blink that goes with a movement is a little faster. */
        constexpr double ReflexScale = 0.9;
        /** Requests less than this apart count as one movement: one decision for all. */
        constexpr double ReflexWindowSeconds = 0.5;
        /**
         * A shift of the gaze or a turn of the head is measured against the direction of a moment
         * ago (a copy that follows with this time constant): a slow change stays small, a fast
         * one shows nearly its full size.
         */
        constexpr double ShiftLagSeconds = 0.3;
        /** Below this, a movement has ended and the next one counts anew. */
        constexpr double ShiftRestDegrees = 3.0;
        /** After a longer gap of the clock, the schedule starts again from now. */
        constexpr double MaximumCatchUpSeconds = 30.0;
        /** After a longer gap, the gaze and the head are taken as they are, without a shift. */
        constexpr double MaximumShiftGapSeconds = 1.0;
        constexpr double DegreesPerRadian = 57.29577951308232;
    }


    FNpcifyIdleBlinks::FNpcifyIdleBlinks(std::function<double()> InRandom)
        : Random(std::move(InRandom))
    {
        if (!Random)
        {
            std::random_device Device;
            const auto Ticks = std::chrono::steady_clock::now().time_since_epoch().count();
            Random = CreateSeededRandom(std::to_string(Device()) + ":" + std::to_string(Ticks));
        }
    }

    double FNpcifyIdleBlinks::BlinkWeight(double Time, double Scale)
    {
        const double Local = Time / Scale;
        if (!std::isfinite(Local) || Local < 0.0 || Local >= BlinkSeconds)
        {
            return 0.0;
        }
        if (Local < IdleBlinkNumbers::CloseSeconds)
        {
            return SmoothStep(Local / IdleBlinkNumbers::CloseSeconds);
        }
        if (Local < IdleBlinkNumbers::CloseSeconds + IdleBlinkNumbers::HoldSeconds)
        {
            return 1.0;
        }
        const double Opened = (Local - IdleBlinkNumbers::CloseSeconds - IdleBlinkNumbers::HoldSeconds) / IdleBlinkNumbers::OpenSeconds;
        return 1.0 - SmoothStep(1.0 - std::pow(1.0 - Opened, IdleBlinkNumbers::OpenEase));
    }

    double FNpcifyIdleBlinks::BlinkInterval(double RandomNumber)
    {
        const double Odds = RandomNumber / std::max(1.0 - RandomNumber, 1e-9);
        const double Interval = MedianIntervalSeconds * std::pow(Odds, 1.0 / IdleBlinkNumbers::IntervalShape);
        return std::min(MaxIntervalSeconds, std::max(MinIntervalSeconds, Interval));
    }

    double FNpcifyIdleBlinks::ShiftChance(double Degrees)
    {
        return ClampUnit((Degrees - ShiftMinDegrees) / (ShiftSureDegrees - ShiftMinDegrees));
    }

    std::function<double()> FNpcifyIdleBlinks::CreateSeededRandom(const std::string& Seed)
    {
        uint32_t Hash = 2166136261u;
        for (const unsigned char Unit : Seed)
        {
            Hash ^= Unit;
            Hash *= 16777619u;
        }
        auto State = std::make_shared<uint32_t>(Hash);
        return [State]()
        {
            *State += 0x6D2B79F5u;
            uint32_t Value = *State;
            Value = (Value ^ (Value >> 15)) * (Value | 1u);
            Value ^= Value + (Value ^ (Value >> 7)) * (Value | 61u);
            return static_cast<double>(Value ^ (Value >> 14)) / 4294967296.0;
        };
    }

    void FNpcifyIdleBlinks::Reset()
    {
        bStarted = false;
        Next = FScheduledBlink();
        bBlinking = false;
        bBlinkedBefore = false;
        bReflexBefore = false;
        ReflexChance = 0.0;
        GazeWatch = FShiftWatch();
        HeadWatch = FShiftWatch();
    }

    double FNpcifyIdleBlinks::Sample(double Time, bool bHeld, bool bSpeaking)
    {
        bHeldNow = bHeld;
        bSpeakingNow = bSpeaking;
        if (!std::isfinite(Time))
        {
            return 0.0;
        }
        if (!bStarted || Time < PreviousTime || Time - Next.Start > IdleBlinkNumbers::MaximumCatchUpSeconds)
        {
            Reset();
            FScheduledBlink Now;
            Now.Start = Time;
            Next = BlinkAfter(Now);
            bStarted = true;
        }
        PreviousTime = Time;
        return BlinkAt(Time, bHeld);
    }

    bool FNpcifyIdleBlinks::RequestBlink(double Time, double Chance)
    {
        if (!std::isfinite(Time) || bHeldNow || bBlinking ||
            (bBlinkedBefore && Time - LastBlinkEnd < RefractorySeconds))
        {
            return false;
        }
        if (!bReflexBefore || Time < ReflexTime || Time - ReflexTime > IdleBlinkNumbers::ReflexWindowSeconds)
        {
            ReflexChance = 0.0;
        }
        bReflexBefore = true;
        ReflexTime = Time;
        const double Wanted = ClampUnit(Chance);
        if (Wanted <= ReflexChance)
        {
            return false;
        }
        // The part of the chance that the decisions before did not use. A sure blink takes no
        // number from the stream.
        const double Remaining = (Wanted - ReflexChance) / (1.0 - ReflexChance);
        ReflexChance = Wanted;
        if (Remaining < 1.0 && Random() >= Remaining)
        {
            return false;
        }
        bBlinking = true;
        BlinkStart = Time;
        BlinkScale = IdleBlinkNumbers::ReflexScale;
        FScheduledBlink Now;
        Now.Start = Time;
        Next = BlinkAfter(Now);
        return true;
    }

    void FNpcifyIdleBlinks::WatchGaze(double Time, double Yaw, double Pitch)
    {
        double Delta = 0.0;
        if (!Advance(GazeWatch, Time, Delta))
        {
            PastYaw = Yaw;
            PastPitch = Pitch;
            return;
        }
        WatchShift(GazeWatch, Time, std::hypot(Yaw - PastYaw, Pitch - PastPitch));
        const double Follow = 1.0 - std::exp(-Delta / IdleBlinkNumbers::ShiftLagSeconds);
        PastYaw += (Yaw - PastYaw) * Follow;
        PastPitch += (Pitch - PastPitch) * Follow;
    }

    void FNpcifyIdleBlinks::TrackHead(double Time, double X, double Y, double Z, double W)
    {
        double Delta = 0.0;
        if (!Advance(HeadWatch, Time, Delta))
        {
            PastX = X;
            PastY = Y;
            PastZ = Z;
            PastW = W;
            return;
        }
        const double Dot = PastX * X + PastY * Y + PastZ * Z + PastW * W;
        const double Degrees = 2.0 * std::acos(std::min(1.0, std::fabs(Dot))) * IdleBlinkNumbers::DegreesPerRadian;
        WatchShift(HeadWatch, Time, Degrees);
        // The copy follows the head: a normalized blend along the short way.
        const double Follow = 1.0 - std::exp(-Delta / IdleBlinkNumbers::ShiftLagSeconds);
        const double Sign = Dot < 0.0 ? -1.0 : 1.0;
        const double Nx = PastX + (X * Sign - PastX) * Follow;
        const double Ny = PastY + (Y * Sign - PastY) * Follow;
        const double Nz = PastZ + (Z * Sign - PastZ) * Follow;
        const double Nw = PastW + (W * Sign - PastW) * Follow;
        double Length = std::sqrt(Nx * Nx + Ny * Ny + Nz * Nz + Nw * Nw);
        if (!(Length > 0.0))
        {
            Length = 1.0;
        }
        PastX = Nx / Length;
        PastY = Ny / Length;
        PastZ = Nz / Length;
        PastW = Nw / Length;
    }

    bool FNpcifyIdleBlinks::Advance(FShiftWatch& Watch, double Time, double& OutDelta)
    {
        if (!std::isfinite(Time))
        {
            return false;
        }
        const double Delta = Time - Watch.Time;
        if (!Watch.bStarted || Delta < 0.0 || Delta > IdleBlinkNumbers::MaximumShiftGapSeconds)
        {
            // The watch starts (again).
            Watch.bStarted = true;
            Watch.Time = Time;
            Watch.Asked = 0.0;
            return false;
        }
        Watch.Time = Time;
        OutDelta = Delta;
        return true;
    }

    void FNpcifyIdleBlinks::WatchShift(FShiftWatch& Watch, double Time, double Degrees)
    {
        // One movement asks for a blink with the chance of its largest shift. It asks again while
        // the shift grows (each time only the new part of the chance counts), so the blink starts
        // early in a large movement. A movement that stays fast asks no more.
        if (Degrees < IdleBlinkNumbers::ShiftRestDegrees)
        {
            Watch.Asked = 0.0;
            return;
        }
        const double Chance = ShiftChance(Degrees);
        if (Chance > Watch.Asked)
        {
            Watch.Asked = Chance;
            RequestBlink(Time, Chance);
        }
    }

    double FNpcifyIdleBlinks::BlinkAt(double Time, bool bHeld)
    {
        if (bBlinking)
        {
            const double End = BlinkStart + BlinkSeconds * BlinkScale;
            if (!bHeld && Time < End)
            {
                return BlinkWeight(Time - BlinkStart, BlinkScale);
            }
            // The blink ended, or a cue took the lids.
            bBlinkedBefore = true;
            LastBlinkEnd = std::min(Time, End);
            bBlinking = false;
        }
        while (Time >= Next.Start)
        {
            const FScheduledBlink Blink = Next;
            const double End = Blink.Start + BlinkSeconds * Blink.Scale;
            Next = BlinkAfter(Blink);
            if (Time >= End)
            {
                // The blink ended before this sample (a long frame).
                bBlinkedBefore = true;
                LastBlinkEnd = End;
                continue;
            }
            if (bHeld)
            {
                continue;
            }
            bBlinking = true;
            BlinkStart = Blink.Start;
            BlinkScale = Blink.Scale;
            return BlinkWeight(Time - Blink.Start, Blink.Scale);
        }
        return 0.0;
    }

    FNpcifyIdleBlinks::FScheduledBlink FNpcifyIdleBlinks::BlinkAfter(const FScheduledBlink& Blink)
    {
        // Three numbers for each blink: the stream stays aligned.
        const double Wait = Random();
        const bool bIsDouble = Random() < DoubleChance;
        const double Scale = IdleBlinkNumbers::ShortestScale + (IdleBlinkNumbers::LongestScale - IdleBlinkNumbers::ShortestScale) * Random();
        FScheduledBlink After;
        if (Blink.bDouble)
        {
            After.Start = Blink.Start + BlinkSeconds * Blink.Scale + IdleBlinkNumbers::DoubleMinGapSeconds +
                (IdleBlinkNumbers::DoubleMaxGapSeconds - IdleBlinkNumbers::DoubleMinGapSeconds) * Wait;
            After.Scale = Scale * IdleBlinkNumbers::DoubleScale;
            After.bDouble = false;
            return After;
        }
        const double Pace = bSpeakingNow ? SpeakingFactor : 1.0;
        After.Start = Blink.Start + std::max(MinIntervalSeconds, BlinkInterval(Wait) * Pace);
        After.Scale = Scale;
        After.bDouble = bIsDouble;
        return After;
    }

    double FNpcifyIdleBlinks::SmoothStep(double Value)
    {
        return Value * Value * (3.0 - 2.0 * Value);
    }

    double FNpcifyIdleBlinks::ClampUnit(double Value)
    {
        return Value <= 0.0 ? 0.0 : (Value >= 1.0 ? 1.0 : Value);
    }
}
