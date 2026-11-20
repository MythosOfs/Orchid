// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstring>
#include <span>
#include <string_view>
#include "common/game_presets.h"

namespace FileSys {

// Yoshi and the Mysterious Book 1.0.2, main module.
inline constexpr std::string_view Yoshi4kBuildId = "893C30E75521AFB8B23326EB6AA0E2DC";

struct Yoshi4kSite {
    u32 offset;
    u32 original;
    u32 replacement;
};

// Patch "4K No AA" by Walnutoast830 (https://github.com/MythosOfs/Orchid/issues/144), shipped
// with the author's permission. Module offsets of 4-byte words; the loader passes its
// decompressed NSO including the 0x100-byte header.
inline constexpr std::array Yoshi4kSites{
    // UI scale: read the DPI curve at half the viewport size and double its result, so the HUD
    // keeps its 1080p proportions at 2160p. The second word rebuilds the minimum-scale constant
    // in one instruction (0.01 -> 0.00995), which frees the first for the doubling.
    Yoshi4kSite{0x02D7A490, 0x529AE148, 0x1E202800}, // mov w8, #0xd70a -> fadd s0, s0, s0
    Yoshi4kSite{0x02D7A494, 0x72A78468, 0x52A78468}, // movk w8, #0x3c23, lsl #16 -> mov w8, ..
    Yoshi4kSite{0x02D7A53C, 0x1E220280, 0x1E02FE80}, // scvtf s0, w20 -> scvtf s0, w20, #1
    // Per-tick resolution check: the sizes the game expects, or it resets the output resolution.
    // Handheld 1280x720 and docked 1920x1080 both become 3840x2160 (doubles built by mov/movk).
    Yoshi4kSite{0x0582D400, 0xD2E81288, 0xD2E815C8}, // handheld width 1280.0 -> 3840.0
    Yoshi4kSite{0x0582D40C, 0xD2E813C8, 0xD2E815C8}, // docked width 1920.0 -> 3840.0
    Yoshi4kSite{0x0582D434, 0xD2D00008, 0xD2DC0008}, // handheld height 720.0 -> 2160.0, low bits
    Yoshi4kSite{0x0582D438, 0xF2E810C8, 0xF2E81408}, // handheld height, top 16 bits
    Yoshi4kSite{0x0582D444, 0xF2E81208, 0xF2E81408}, // docked height 1080.0 -> 2160.0, top bits
    // Graphics-mode setter: ignore the per-area mode table and take case 0 (no anti-aliasing,
    // r.ScreenPercentage 100, dynamic resolution off). ldrb w11, [x9, x8] -> mov w11, #0
    Yoshi4kSite{0x0582D56C, 0x3868692B, 0x5280000B},
    // r.SetRes chooser: ignore the resolution-option table and use option 0, the command
    // "r.SetRes 1920x1080". ldrb w11, [x9, x8] -> mov w11, wzr
    Yoshi4kSite{0x0582D870, 0x3868692B, 0x2A1F03EB},
    // That command's UTF-16 text: "1920x1080" -> "3840x2160" (same length).
    Yoshi4kSite{0x070529D8, 0x00390031, 0x00380033},
    Yoshi4kSite{0x070529DC, 0x00300032, 0x00300034},
    Yoshi4kSite{0x070529E0, 0x00310078, 0x00320078},
    Yoshi4kSite{0x070529E4, 0x00380030, 0x00360031},
};

// Changes every site or none: one word that is not the original (another game version, or a
// resolution mod that already patched it) leaves the module untouched.
inline bool ApplyYoshi4kPatch(std::span<u8> nso, u64 program_id, std::string_view build_id) {
    if (program_id != Settings::YoshiMysteriousBookProgramId || build_id != Yoshi4kBuildId) {
        return false;
    }
    for (const auto& site : Yoshi4kSites) {
        const size_t at = 0x100 + site.offset;
        if (at + sizeof(u32) > nso.size())
            return false;
        u32 word{};
        std::memcpy(&word, nso.data() + at, sizeof(word));
        if (word != site.original)
            return false;
    }
    for (const auto& site : Yoshi4kSites) {
        std::memcpy(nso.data() + 0x100 + site.offset, &site.replacement, sizeof(u32));
    }
    return true;
}

} // namespace FileSys
