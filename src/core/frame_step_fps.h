// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include "common/common_types.h"

namespace Core {

// For games that simulate a whole number of 60 Hz frames per rendered frame (Hyrule Warriors:
// Age of Imprisonment keeps that number in one variable and only ever sets 1 or 2 itself):
// picks the number for the next frame from how long the last one took, so the game keeps its
// speed when the PC renders fewer than 60 frames a second.
//
// The game also waits that many frames itself before it shows the next one. So the result is
// 60 or 30 FPS at full speed, not a rate in between, and a frame that only lasted as long as its
// step does not show whether the PC could be faster again.
class FrameStepFps {
public:
    // Only the steps the game uses itself; below 30 FPS it slows down as before. With 3 a boot
    // ended in a jump through a bad pointer (2026-11-12, registers full of 3s).
    static constexpr u32 MaxStep = 2;
    // A frame may run this much of a 60 Hz frame over one step before two are used: up to 17 %
    // slow motion is the price for not halving the frame rate at 55 FPS.
    static constexpr double Slack = 0.2;
    // After this many frames that fit their step, try the next faster rate. A PC that cannot
    // hold it runs slow for the frames of the try (a few percent of the time).
    static constexpr u32 ProbeAfter = 60;
    // Frames in a row that must run over before the step goes up: one slow frame is a hitch, and
    // answering it with a lower frame rate for the next seconds would be worse than the hitch.
    static constexpr u32 OverBeforeUp = 2;
    // The game uses a new step two to three frames after it was written (measured: judged on the
    // next frame, every try from 20 FPS failed although the PC held 56). A try lasts this many
    // frames before its frame time counts.
    static constexpr u32 Settle = 3;

    // elapsed_ns: real time since the previous present. speed_percent: the emulation speed limit
    // (100 = real time, 0 = unlimited, which keeps the game's own single step).
    u32 Next(s64 elapsed_ns, u32 speed_percent) {
        // Loading stalls and pauses are not replayed as simulation time.
        if (speed_percent == 0 || elapsed_ns <= 0 || elapsed_ns > 250'000'000) {
            held = over = settle = 0;
            return step = 1;
        }
        if (settle > 0) {
            --settle;
            return step;
        }
        const double frames = static_cast<double>(elapsed_ns) * 60.0 * speed_percent / 100e9;
        const u32 wanted =
            static_cast<u32>(std::clamp(std::floor(frames + 1.0 - Slack), 1.0, double{MaxStep}));
        if (wanted > step) {
            if (++over < OverBeforeUp) {
                return step;
            }
            held = over = 0;
            return step = wanted;
        }
        over = 0;
        if (step > 1 && ++held >= ProbeAfter) {
            held = 0;
            settle = Settle;
            --step;
        }
        return step;
    }

private:
    u32 step{1};
    u32 held{};
    u32 over{};
    u32 settle{};
};

} // namespace Core
