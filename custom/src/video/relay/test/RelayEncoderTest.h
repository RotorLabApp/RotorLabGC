#pragma once

#include "UnitTest.h"

/// G109: the Low and Medium path on this machine's own encoder. Skips when
/// none opens (EncoderProbeTest logs which do). Low from a 1280x720 30 fps
/// source: 640x360, about 15 fps, a keyframe at least every second, no
/// B-frames, bitrate from 80 percent of the encoder's aim (0.68 Mb/s) to the
/// guard's trip line (0.88 Mb/s) over 10 s, and fragments
/// that start at keyframes. Then the CPU cap: an injected slow encoder trips
/// it at Medium; the real encoder at Low does not.
///
/// The output-bitrate guard, on a noise source. The synthetic
/// cases need no encoder, so they run wherever the relay is built (Windows,
/// and Linux in the G88 FFmpeg build): real tap, real RelayPipeline, real
/// LiveFragmenter, BitrateGuard, and a second tap consumer standing in for
/// local recording. The last case runs the real Low encoder on noise.
///
/// P5 run: the real Low encoder on the scene for 60 s with the guard as
/// VideoRelay runs it (aim 680 kb/s, cap 800, start grace): no trip, under
/// the cap. And a source restart (stream stop and start, or a new generation
/// alone) gives the guard its start grace again.
class RelayEncoderTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _lowOutput();
    void _bitrateSurvey();
    void _capTripsOnSlowEncoder();

    void _guardLogic();
    void _guardNoiseHeldOrPaused();
    void _guardSceneUnaffected();
    void _guardRecoversWithBackoff();
    void _guardRealEncoderNoise();

    // S20/S21 end-to-end: the CPU cap and the guard's start.
    void _capIgnoresInputGap();
    void _capTripsOnTrulySlowEncoder();
    void _capSurvivesSourceRestart();
    void _guardStartGrace();

    // P5 end-to-end run: a normal picture at Low tripped the guard.
    void _guardRestartRearmsGrace();
    void _guardRealEncoderSceneUnderCap();
};
