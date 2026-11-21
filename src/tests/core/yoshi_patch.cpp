// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string_view>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "common/hex_util.h"
#include "common/settings.h"
#include "core/file_sys/yoshi_patch.h"

namespace {
struct Change {
    u32 offset;
    std::string_view was;
    std::string_view now;
};
struct Variant {
    std::string_view name;
    Settings::YoshiPatchResolution resolution;
    Settings::YoshiPatchAntiAliasing anti_aliasing;
    std::span<const Change> changes;
};
constexpr std::array Changes1080pNoAa{
    Change{0x0582D400, "8812E8D2", "C813E8D2"},
    Change{0x0582D434, "0800D0D2", "0800DCD2"},
    Change{0x0582D438, "C810E8F2", "0812E8F2"},
    Change{0x0582D56C, "2B696838", "0B008052"},
    Change{0x0582D870, "2B696838", "EB031F2A"},
};
constexpr std::array Changes1080pFxaa{
    Change{0x0582D400, "8812E8D2", "C813E8D2"},
    Change{0x0582D434, "0800D0D2", "0800DCD2"},
    Change{0x0582D438, "C810E8F2", "0812E8F2"},
    Change{0x0582D56C, "2B696838", "2B038052"},
    Change{0x0582D870, "2B696838", "EB031F2A"},
};
constexpr std::array Changes1080pDlaa{
    Change{0x0582D400, "8812E8D2", "C813E8D2"},
    Change{0x0582D434, "0800D0D2", "0800DCD2"},
    Change{0x0582D438, "C810E8F2", "0812E8F2"},
    Change{0x0582D56C, "2B696838", "AB068052"},
    Change{0x0582D65C, "48068052", "880C8052"},
    Change{0x0582D670, "41068052", "810C8052"},
    Change{0x0582D870, "2B696838", "EB031F2A"},
};
constexpr std::array Changes1440pNoAa{
    Change{0x02D7A490, "48E19A52", "02102D1E"},
    Change{0x02D7A494, "6884A772", "0018221E"},
    Change{0x02D7A4A0, "0101271E", "0110281E"},
    Change{0x02D7A4B4, "00000000", "9406140B"},
    Change{0x02D7A4B8, "00000000", "A0A20791"},
    Change{0x02D7A4BC, "00000000", "1F000014"},
    Change{0x02D7A534, "A0A20791", "E0FFFF17"},
    Change{0x02D7A53C, "8002221E", "80FA021E"},
    Change{0x0582D400, "8812E8D2", "8814E8D2"},
    Change{0x0582D40C, "C813E8D2", "8814E8D2"},
    Change{0x0582D438, "C810E8F2", "C812E8F2"},
    Change{0x0582D440, "0800DCD2", "0800D0D2"},
    Change{0x0582D444, "0812E8F2", "C812E8F2"},
    Change{0x0582D56C, "2B696838", "0B008052"},
    Change{0x0582D870, "2B696838", "EB031F2A"},
    Change{0x070529D8, "310039003200300078003100300038003000", "320035003600300078003100340034003000"},
};
constexpr std::array Changes1440pFxaa{
    Change{0x02D7A490, "48E19A52", "02102D1E"},
    Change{0x02D7A494, "6884A772", "0018221E"},
    Change{0x02D7A4A0, "0101271E", "0110281E"},
    Change{0x02D7A4B4, "00000000", "9406140B"},
    Change{0x02D7A4B8, "00000000", "A0A20791"},
    Change{0x02D7A4BC, "00000000", "1F000014"},
    Change{0x02D7A534, "A0A20791", "E0FFFF17"},
    Change{0x02D7A53C, "8002221E", "80FA021E"},
    Change{0x0582D400, "8812E8D2", "8814E8D2"},
    Change{0x0582D40C, "C813E8D2", "8814E8D2"},
    Change{0x0582D438, "C810E8F2", "C812E8F2"},
    Change{0x0582D440, "0800DCD2", "0800D0D2"},
    Change{0x0582D444, "0812E8F2", "C812E8F2"},
    Change{0x0582D56C, "2B696838", "2B038052"},
    Change{0x0582D870, "2B696838", "EB031F2A"},
    Change{0x070529D8, "310039003200300078003100300038003000", "320035003600300078003100340034003000"},
};
constexpr std::array Changes1440pDlaa{
    Change{0x02D7A490, "48E19A52", "02102D1E"},
    Change{0x02D7A494, "6884A772", "0018221E"},
    Change{0x02D7A4A0, "0101271E", "0110281E"},
    Change{0x02D7A4B4, "00000000", "9406140B"},
    Change{0x02D7A4B8, "00000000", "A0A20791"},
    Change{0x02D7A4BC, "00000000", "1F000014"},
    Change{0x02D7A534, "A0A20791", "E0FFFF17"},
    Change{0x02D7A53C, "8002221E", "80FA021E"},
    Change{0x0582D400, "8812E8D2", "8814E8D2"},
    Change{0x0582D40C, "C813E8D2", "8814E8D2"},
    Change{0x0582D438, "C810E8F2", "C812E8F2"},
    Change{0x0582D440, "0800DCD2", "0800D0D2"},
    Change{0x0582D444, "0812E8F2", "C812E8F2"},
    Change{0x0582D56C, "2B696838", "AB068052"},
    Change{0x0582D65C, "48068052", "880C8052"},
    Change{0x0582D670, "41068052", "810C8052"},
    Change{0x0582D870, "2B696838", "EB031F2A"},
    Change{0x070529D8, "310039003200300078003100300038003000", "320035003600300078003100340034003000"},
};
constexpr std::array Changes4KNoAa{
    Change{0x02D7A490, "48E19A52", "0028201E"},
    Change{0x02D7A494, "6884A772", "6884A752"},
    Change{0x02D7A53C, "8002221E", "80FE021E"},
    Change{0x0582D400, "8812E8D2", "C815E8D2"},
    Change{0x0582D40C, "C813E8D2", "C815E8D2"},
    Change{0x0582D434, "0800D0D2", "0800DCD2"},
    Change{0x0582D438, "C810E8F2", "0814E8F2"},
    Change{0x0582D444, "0812E8F2", "0814E8F2"},
    Change{0x0582D56C, "2B696838", "0B008052"},
    Change{0x0582D870, "2B696838", "EB031F2A"},
    Change{0x070529D8, "310039003200300078003100300038003000", "330038003400300078003200310036003000"},
};
constexpr std::array Changes4KFxaa{
    Change{0x02D7A490, "48E19A52", "0028201E"},
    Change{0x02D7A494, "6884A772", "6884A752"},
    Change{0x02D7A53C, "8002221E", "80FE021E"},
    Change{0x0582D400, "8812E8D2", "C815E8D2"},
    Change{0x0582D40C, "C813E8D2", "C815E8D2"},
    Change{0x0582D434, "0800D0D2", "0800DCD2"},
    Change{0x0582D438, "C810E8F2", "0814E8F2"},
    Change{0x0582D444, "0812E8F2", "0814E8F2"},
    Change{0x0582D56C, "2B696838", "2B038052"},
    Change{0x0582D870, "2B696838", "EB031F2A"},
    Change{0x070529D8, "310039003200300078003100300038003000", "330038003400300078003200310036003000"},
};
constexpr std::array Changes4KDlaa{
    Change{0x02D7A490, "48E19A52", "0028201E"},
    Change{0x02D7A494, "6884A772", "6884A752"},
    Change{0x02D7A53C, "8002221E", "80FE021E"},
    Change{0x0582D400, "8812E8D2", "C815E8D2"},
    Change{0x0582D40C, "C813E8D2", "C815E8D2"},
    Change{0x0582D434, "0800D0D2", "0800DCD2"},
    Change{0x0582D438, "C810E8F2", "0814E8F2"},
    Change{0x0582D444, "0812E8F2", "0814E8F2"},
    Change{0x0582D56C, "2B696838", "AB068052"},
    Change{0x0582D65C, "48068052", "880C8052"},
    Change{0x0582D670, "41068052", "810C8052"},
    Change{0x0582D870, "2B696838", "EB031F2A"},
    Change{0x070529D8, "310039003200300078003100300038003000", "330038003400300078003200310036003000"},
};
constexpr std::array Variants{
    Variant{"1080p No AA", Settings::YoshiPatchResolution::Res1080p, Settings::YoshiPatchAntiAliasing::None, Changes1080pNoAa},
    Variant{"1080p FXAA", Settings::YoshiPatchResolution::Res1080p, Settings::YoshiPatchAntiAliasing::Fxaa, Changes1080pFxaa},
    Variant{"1080p DLAA", Settings::YoshiPatchResolution::Res1080p, Settings::YoshiPatchAntiAliasing::Dlaa, Changes1080pDlaa},
    Variant{"1440p No AA", Settings::YoshiPatchResolution::Res1440p, Settings::YoshiPatchAntiAliasing::None, Changes1440pNoAa},
    Variant{"1440p FXAA", Settings::YoshiPatchResolution::Res1440p, Settings::YoshiPatchAntiAliasing::Fxaa, Changes1440pFxaa},
    Variant{"1440p DLAA", Settings::YoshiPatchResolution::Res1440p, Settings::YoshiPatchAntiAliasing::Dlaa, Changes1440pDlaa},
    Variant{"4K No AA", Settings::YoshiPatchResolution::Res2160p, Settings::YoshiPatchAntiAliasing::None, Changes4KNoAa},
    Variant{"4K FXAA", Settings::YoshiPatchResolution::Res2160p, Settings::YoshiPatchAntiAliasing::Fxaa, Changes4KFxaa},
    Variant{"4K DLAA", Settings::YoshiPatchResolution::Res2160p, Settings::YoshiPatchAntiAliasing::Dlaa, Changes4KDlaa},
};
constexpr size_t TextEnd = 0x100 + 0x070529EA;
constexpr size_t LastWordEnd = TextEnd - 2;

void Write(std::vector<u8>& module, const Variant& variant, bool patched) {
    for (const Change& change : variant.changes) {
        const auto bytes = Common::HexStringToVector(patched ? change.now : change.was, false);
        std::memcpy(module.data() + 0x100 + change.offset, bytes.data(), bytes.size());
    }
}

std::vector<u8> MakeModule() {
    std::vector<u8> module(TextEnd);
    for (const Variant& variant : Variants) {
        Write(module, variant, false);
    }
    return module;
}

std::vector<u8> Patched(std::vector<u8> module, const Variant& variant) {
    Write(module, variant, true);
    return module;
}

bool Same(const std::vector<u8>& a, const std::vector<u8>& b) {
    return a == b;
}

bool Apply(std::vector<u8>& module, const Variant& variant,
           u64 program_id = Settings::YoshiMysteriousBookProgramId,
           std::string_view build_id = FileSys::YoshiPatchBuildId) {
    return FileSys::ApplyYoshiPatch(module, program_id, build_id, variant.resolution,
                                    variant.anti_aliasing);
}
}

TEST_CASE("Yoshi patch writes exactly the contributed bytes of each variant", "[yoshi_patch]") {
    const auto original = MakeModule();
    for (const Variant& variant : Variants) {
        DYNAMIC_SECTION(variant.name) {
            auto module = original;
            REQUIRE(Apply(module, variant));
            CHECK(Same(module, Patched(original, variant)));
        }
    }

    auto module = original;
    module.resize(LastWordEnd);
    CHECK(Apply(module, Variants.back()));
}

TEST_CASE("Yoshi patch leaves other games, builds, modes and altered or truncated code untouched",
          "[yoshi_patch]") {
    const auto original = MakeModule();
    auto module = original;
    const Variant& variant = Variants.back();
    CHECK_FALSE(Apply(module, variant, Settings::StarFoxProgramId));
    CHECK_FALSE(Apply(module, variant, Settings::YoshiMysteriousBookProgramId, "other build"));
    CHECK_FALSE(FileSys::ApplyYoshiPatch(module, Settings::YoshiMysteriousBookProgramId,
                                         FileSys::YoshiPatchBuildId,
                                         static_cast<Settings::YoshiPatchResolution>(3),
                                         Settings::YoshiPatchAntiAliasing::None));
    CHECK_FALSE(FileSys::ApplyYoshiPatch(module, Settings::YoshiMysteriousBookProgramId,
                                         FileSys::YoshiPatchBuildId,
                                         Settings::YoshiPatchResolution::Res2160p,
                                         static_cast<Settings::YoshiPatchAntiAliasing>(3)));
    CHECK(Same(module, original));

    for (const Variant& each : Variants) {
        DYNAMIC_SECTION(each.name) {
            for (const Change& change : each.changes) {
                for (size_t byte = 0; byte + 4 <= change.was.size() / 2; byte += 4) {
                    module[0x100 + change.offset + byte + 3] ^= 0x80;
                    CHECK_FALSE(Apply(module, each));
                    module[0x100 + change.offset + byte + 3] ^= 0x80;
                }
            }
            CHECK(Same(module, original));
        }
    }

    module.resize(LastWordEnd - 1);
    const auto truncated = module;
    CHECK_FALSE(Apply(module, variant));
    CHECK(Same(module, truncated));
    module.clear();
    CHECK_FALSE(Apply(module, variant));
}

TEST_CASE("Yoshi patch keeps the old 4K option's key and mode", "[yoshi_patch]") {
    CHECK(Settings::values.yoshi_patch.GetLabel() == "yoshi_4k_patch");
    CHECK_FALSE(Settings::values.yoshi_patch.GetDefault());
    CHECK(Settings::values.yoshi_patch_resolution.GetDefault() ==
          Settings::YoshiPatchResolution::Res2160p);
    CHECK(Settings::values.yoshi_patch_anti_aliasing.GetDefault() ==
          Settings::YoshiPatchAntiAliasing::None);
}

TEST_CASE("Yoshi patch variants match the game's main module", "[.][yoshi_patch_dump]") {
    const char* path = std::getenv("ORCHID_YOSHI_MAIN_NSO");
    REQUIRE(path != nullptr);
    std::ifstream file{path, std::ios::binary};
    const std::vector<u8> dump{std::istreambuf_iterator<char>{file}, {}};
    REQUIRE(dump.size() >= TextEnd);
    for (const Variant& variant : Variants) {
        DYNAMIC_SECTION(variant.name) {
            for (const Change& change : variant.changes) {
                const auto was = Common::HexStringToVector(change.was, false);
                CHECK(std::memcmp(dump.data() + 0x100 + change.offset, was.data(), was.size()) ==
                      0);
            }
            auto module = dump;
            REQUIRE(Apply(module, variant));
            CHECK(Same(module, Patched(dump, variant)));
        }
    }
}
