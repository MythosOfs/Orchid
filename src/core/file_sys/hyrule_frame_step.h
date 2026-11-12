// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include "common/common_types.h"

namespace FileSys {

// Hyrule Warriors: Age of Imprisonment, update v196608 (main, build 659CB288...).
inline constexpr std::array<u8, 20> HyruleFrameStepBuildId{
    0x65, 0x9C, 0xB2, 0x88, 0xB5, 0xC5, 0xB2, 0xB4, 0x98, 0x53,
    0xAD, 0xD6, 0x16, 0xC0, 0xD5, 0xBF, 0x4E, 0xA4, 0xE1, 0xBA};

// The game simulates a whole number of 60 Hz frames per rendered frame. Once per frame
// main+1af5b18 copies that number from this variable into main+2896720, which 176 places in the
// gameplay code read (timers subtract it, movement multiplies by it). The game itself only stores
// 1 or 2 there (60 or 30 FPS mode, main+11b05d4) and never looks at the real frame time, so below
// 60 FPS it ran in slow motion.
inline constexpr u64 HyruleFrameStepVariable = 0x28966a8;

// Module offset of the frame-step variable when `image` (the loaded main module from offset 0)
// is the supported build and still has the code that reads it: nullopt otherwise. Nothing is
// patched; the present path writes the variable (data) once per frame.
inline std::optional<u64> FindHyruleFrameStep(std::span<const u8> image,
                                              std::span<const u8> build_id) {
    // The variable lies behind everything read below, so one size check covers all of it.
    if (build_id.size() < HyruleFrameStepBuildId.size() ||
        !std::equal(HyruleFrameStepBuildId.begin(), HyruleFrameStepBuildId.end(),
                    build_id.begin()) ||
        image.size() < HyruleFrameStepVariable + sizeof(u32)) {
        return std::nullopt;
    }
    struct Word {
        u64 offset;
        u32 value;
    };
    // adrp x9, got ; ldr x9, [x9, #0x650] ; ... ; adrp x8 ; add x8, x8, #0x720 ;
    // ldr w19, [x9] ; str w19, [x8]
    constexpr std::array<Word, 6> code{{{0x1af5af8, 0xb0006ac9}, {0x1af5b00, 0xf9432929},
                                        {0x1af5b10, 0xb0006d08}, {0x1af5b14, 0x911c8108},
                                        {0x1af5b18, 0xb9400133}, {0x1af5b1c, 0xb9000113}}};
    for (const auto& [offset, value] : code) {
        u32 word{};
        std::memcpy(&word, image.data() + offset, sizeof(word));
        if (word != value) {
            return std::nullopt;
        }
    }
    // The GOT slot that code loads must point at the variable (its relocation addend).
    constexpr u64 got_slot = 0x284e650;
    u64 target{};
    std::memcpy(&target, image.data() + got_slot, sizeof(target));
    if (target != HyruleFrameStepVariable) {
        return std::nullopt;
    }
    return HyruleFrameStepVariable;
}

} // namespace FileSys
