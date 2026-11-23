#pragma once

#include <functional>
#include <string>

namespace Npcify
{
    /**
     * The idle blinks of a face by the rules of the Web SDK and the rendered video
     * (IdleFaceTimeline in packages/vrm-renderer/src/idle-face.ts; README of that package,
     * "Template-face playback"). The numbers are the same; only the clock and the random numbers
     * differ. Times are in seconds, angles in degrees.
     *
     * The numbers follow the blinks of people. A person at rest blinks about 17 times a minute, at
     * irregular intervals with a long tail, and more often while speaking. The lids close faster
     * than they open. A blink goes with a large gaze shift, with a turn of the head and with the
     * start of a new movement.
     */
    class FNpcifyIdleBlinks
    {
    public:
        /** The middle interval between the starts of two blinks at rest. */
        static constexpr double MedianIntervalSeconds = 3.2;
        /** No interval is shorter or longer than these. */
        static constexpr double MinIntervalSeconds = 1.2;
        static constexpr double MaxIntervalSeconds = 9.0;
        /** While the character speaks, the intervals are this much shorter. */
        static constexpr double SpeakingFactor = 0.75;
        /** The lids close in 80 ms, stay closed for 50 ms and open in 170 ms. */
        static constexpr double BlinkSeconds = 0.3;
        /** The part of the blinks that a second blink follows at once. */
        static constexpr double DoubleChance = 0.12;
        /** After the end of a blink, no movement starts a new one for this long. */
        static constexpr double RefractorySeconds = 0.6;
        /** A shift below this brings no blink; one of the second number or more always does. */
        static constexpr double ShiftMinDegrees = 8.0;
        static constexpr double ShiftSureDegrees = 30.0;
        /** The part of the new animations that bring a blink. */
        static constexpr float ActionBlinkChance = 0.9f;
        /** The part of the returns to the idle animation that bring a blink. */
        static constexpr float IdleReturnBlinkChance = 0.6f;

        /** A schedule on the numbers of Random (each in [0, 1)); empty: a new random seed. */
        explicit FNpcifyIdleBlinks(std::function<double()> InRandom = {});

        /**
         * The weight of the lids at Time seconds after the start of a blink that takes Scale
         * times the usual length. The lids close on an S curve, stay closed, then open fast at
         * first and slowly at the end.
         */
        static double BlinkWeight(double Time, double Scale = 1.0);

        /**
         * The interval to the next blink for a random number in [0, 1): 0.5 gives the middle
         * interval, small numbers short intervals and numbers near 1 the rare long ones (a
         * log-logistic distribution inside the two limits).
         */
        static double BlinkInterval(double Random);

        /** The chance of a blink with a gaze shift or a head turn of Degrees. */
        static double ShiftChance(double Degrees);

        /**
         * A deterministic random stream (mulberry32 on an FNV-1a hash of the seed's bytes): for
         * an ASCII seed, the same numbers in [0, 1) as createSeededRandom of @npcify/vrm-renderer.
         */
        static std::function<double()> CreateSeededRandom(const std::string& Seed);

        /** Starts the schedule again at the next sample. */
        void Reset();

        /**
         * The weight of the lids at Time seconds. While a cue drives a blink expression (bHeld),
         * a blink that is due is skipped, a blink that plays ends at once and the weight is 0;
         * the schedule goes on. While the character speaks, the intervals are shorter. A clock
         * that goes back (or jumps more than 30 s) starts the schedule again.
         */
        double Sample(double Time, bool bHeld = false, bool bSpeaking = false);

        /**
         * Asks for a blink at Time because the character starts a movement: a new animation, a
         * large gaze shift or a turn of the head. Chance is the part of such movements that bring
         * a blink. No blink starts while one plays, within 0.6 s after one, or while a cue drives
         * a blink expression. Requests less than half a second apart count as one movement:
         * together they bring a blink with the largest of their chances. The next idle blink then
         * comes an interval after this one. Returns whether a blink starts.
         */
        bool RequestBlink(double Time, double Chance = 1.0);

        /** Takes the gaze (yaw and pitch in degrees) at Time: a fast and large shift asks for a blink. */
        void WatchGaze(double Time, double Yaw, double Pitch);

        /**
         * Takes the rotation of the head in the world at Time (a quaternion). A fast and large
         * turn of the head asks for a blink, as a gaze shift does. Call it in each frame.
         */
        void TrackHead(double Time, double X, double Y, double Z, double W);

        /**
         * Takes the next rotation of the head as it is, without a turn: call it when the head is
         * another bone, for example on the next mesh of an appearance variant, whose head bone can
         * have another rest rotation.
         */
        void RestartHead() { HeadWatch = FShiftWatch(); }

    private:
        struct FScheduledBlink
        {
            double Start = 0.0;
            double Scale = 1.0;
            /** A second blink follows this one at once. */
            bool bDouble = false;
        };

        /** The watch of one direction (the gaze or the head) for a fast, large shift. */
        struct FShiftWatch
        {
            double Time = 0.0;
            bool bStarted = false;
            /** The chance that the movement in progress asked with, 0 at rest. */
            double Asked = 0.0;
        };

        static bool Advance(FShiftWatch& Watch, double Time, double& OutDelta);
        void WatchShift(FShiftWatch& Watch, double Time, double Degrees);
        double BlinkAt(double Time, bool bHeld);
        FScheduledBlink BlinkAfter(const FScheduledBlink& Blink);
        static double SmoothStep(double Value);
        static double ClampUnit(double Value);

        std::function<double()> Random;
        bool bStarted = false;
        double PreviousTime = 0.0;
        FScheduledBlink Next;
        bool bBlinking = false;
        double BlinkStart = 0.0;
        double BlinkScale = 1.0;
        bool bBlinkedBefore = false;
        double LastBlinkEnd = 0.0;
        bool bHeldNow = false;
        bool bSpeakingNow = false;
        bool bReflexBefore = false;
        double ReflexTime = 0.0;
        double ReflexChance = 0.0;

        FShiftWatch GazeWatch;
        FShiftWatch HeadWatch;
        double PastYaw = 0.0;
        double PastPitch = 0.0;
        double PastX = 0.0;
        double PastY = 0.0;
        double PastZ = 0.0;
        double PastW = 1.0;
    };
}
