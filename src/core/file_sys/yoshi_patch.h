// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstring>
#include <span>
#include <string_view>
#include "common/game_presets.h"
#include "common/settings_enums.h"

namespace FileSys {

inline constexpr std::string_view YoshiPatchBuildId = "893C30E75521AFB8B23326EB6AA0E2DC";

struct YoshiPatchSite {
    u32 offset;
    u32 original;
    u32 replacement;
};

inline constexpr std::array Yoshi1080pSites{
    YoshiPatchSite{0x0582D400, 0xD2E81288, 0xD2E813C8},
    YoshiPatchSite{0x0582D434, 0xD2D00008, 0xD2DC0008},
    YoshiPatchSite{0x0582D438, 0xF2E810C8, 0xF2E81208},
    YoshiPatchSite{0x0582D870, 0x3868692B, 0x2A1F03EB},
};
inline constexpr std::array Yoshi1440pSites{
    YoshiPatchSite{0x02D7A490, 0x529AE148, 0x1E2D1002},
    YoshiPatchSite{0x02D7A494, 0x72A78468, 0x1E221800},
    YoshiPatchSite{0x02D7A4A0, 0x1E270101, 0x1E281001},
    YoshiPatchSite{0x02D7A4B4, 0x00000000, 0x0B140694},
    YoshiPatchSite{0x02D7A4B8, 0x00000000, 0x9107A2A0},
    YoshiPatchSite{0x02D7A4BC, 0x00000000, 0x1400001F},
    YoshiPatchSite{0x02D7A534, 0x9107A2A0, 0x17FFFFE0},
    YoshiPatchSite{0x02D7A53C, 0x1E220280, 0x1E02FA80},
    YoshiPatchSite{0x0582D400, 0xD2E81288, 0xD2E81488},
    YoshiPatchSite{0x0582D40C, 0xD2E813C8, 0xD2E81488},
    YoshiPatchSite{0x0582D438, 0xF2E810C8, 0xF2E812C8},
    YoshiPatchSite{0x0582D440, 0xD2DC0008, 0xD2D00008},
    YoshiPatchSite{0x0582D444, 0xF2E81208, 0xF2E812C8},
    YoshiPatchSite{0x0582D870, 0x3868692B, 0x2A1F03EB},
    YoshiPatchSite{0x070529D8, 0x00390031, 0x00350032},
    YoshiPatchSite{0x070529DC, 0x00300032, 0x00300036},
    YoshiPatchSite{0x070529E0, 0x00310078, 0x00310078},
    YoshiPatchSite{0x070529E4, 0x00380030, 0x00340034},
};
inline constexpr std::array Yoshi2160pSites{
    YoshiPatchSite{0x02D7A490, 0x529AE148, 0x1E202800},
    YoshiPatchSite{0x02D7A494, 0x72A78468, 0x52A78468},
    YoshiPatchSite{0x02D7A53C, 0x1E220280, 0x1E02FE80},
    YoshiPatchSite{0x0582D400, 0xD2E81288, 0xD2E815C8},
    YoshiPatchSite{0x0582D40C, 0xD2E813C8, 0xD2E815C8},
    YoshiPatchSite{0x0582D434, 0xD2D00008, 0xD2DC0008},
    YoshiPatchSite{0x0582D438, 0xF2E810C8, 0xF2E81408},
    YoshiPatchSite{0x0582D444, 0xF2E81208, 0xF2E81408},
    YoshiPatchSite{0x0582D870, 0x3868692B, 0x2A1F03EB},
    YoshiPatchSite{0x070529D8, 0x00390031, 0x00380033},
    YoshiPatchSite{0x070529DC, 0x00300032, 0x00300034},
    YoshiPatchSite{0x070529E0, 0x00310078, 0x00320078},
    YoshiPatchSite{0x070529E4, 0x00380030, 0x00360031},
};
inline constexpr std::array YoshiNoAaSites{
    YoshiPatchSite{0x0582D56C, 0x3868692B, 0x5280000B},
};
inline constexpr std::array YoshiFxaaSites{
    YoshiPatchSite{0x0582D56C, 0x3868692B, 0x5280032B},
};
inline constexpr std::array YoshiDlaaSites{
    YoshiPatchSite{0x0582D56C, 0x3868692B, 0x528006AB},
    YoshiPatchSite{0x0582D65C, 0x52800648, 0x52800C88},
    YoshiPatchSite{0x0582D670, 0x52800641, 0x52800C81},
};

inline constexpr std::array<std::span<const YoshiPatchSite>, 3> YoshiResolutionSites{
    Yoshi1080pSites, Yoshi1440pSites, Yoshi2160pSites};
inline constexpr std::array<std::span<const YoshiPatchSite>, 3> YoshiAntiAliasingSites{
    YoshiNoAaSites, YoshiFxaaSites, YoshiDlaaSites};
inline constexpr std::array<std::string_view, 3> YoshiResolutionNames{"1080p", "1440p", "4K"};
inline constexpr std::array<std::string_view, 3> YoshiAntiAliasingNames{"No AA", "FXAA", "DLAA"};

inline bool ApplyYoshiPatch(std::span<u8> nso, u64 program_id, std::string_view build_id,
                            Settings::YoshiPatchResolution resolution,
                            Settings::YoshiPatchAntiAliasing anti_aliasing) {
    const auto res = static_cast<size_t>(resolution);
    const auto aa = static_cast<size_t>(anti_aliasing);
    if (program_id != Settings::YoshiMysteriousBookProgramId || build_id != YoshiPatchBuildId ||
        res >= YoshiResolutionSites.size() || aa >= YoshiAntiAliasingSites.size()) {
        return false;
    }
    const std::array parts{YoshiResolutionSites[res], YoshiAntiAliasingSites[aa]};
    for (const auto part : parts) {
        for (const auto& site : part) {
            const size_t at = 0x100 + site.offset;
            if (at + sizeof(u32) > nso.size())
                return false;
            u32 word{};
            std::memcpy(&word, nso.data() + at, sizeof(word));
            if (word != site.original)
                return false;
        }
    }
    for (const auto part : parts) {
        for (const auto& site : part) {
            std::memcpy(nso.data() + 0x100 + site.offset, &site.replacement, sizeof(u32));
        }
    }
    return true;
}

}
