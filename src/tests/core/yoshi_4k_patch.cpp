// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <string_view>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "common/hex_util.h"
#include "core/file_sys/yoshi_4k_patch.h"

namespace {
// Lines of the contributor's "4K No AA.pchtxt" (issue 144): module offset, then the bytes as
// stored in the game before and after the patch.
struct Change {
    u32 offset;
    std::string_view was;
    std::string_view now;
};
constexpr std::array Changes{
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
    Change{0x070529D8, "310039003200300078003100300038003000",
           "330038003400300078003200310036003000"},
};
// The last patched word ends two bytes before the end of the "1920x1080" text.
constexpr size_t LastWordEnd = 0x100 + 0x070529E8;

std::vector<u8> MakeModule(bool patched = false) {
    std::vector<u8> module(LastWordEnd + 2);
    for (const Change& change : Changes) {
        const auto bytes = Common::HexStringToVector(patched ? change.now : change.was, false);
        std::memcpy(module.data() + 0x100 + change.offset, bytes.data(), bytes.size());
    }
    return module;
}

// A failed == would make Catch2 print both operands, 118 MB each.
bool Same(const std::vector<u8>& a, const std::vector<u8>& b) {
    return a == b;
}

bool Apply(std::vector<u8>& module, u64 program_id = Settings::YoshiMysteriousBookProgramId,
           std::string_view build_id = FileSys::Yoshi4kBuildId) {
    return FileSys::ApplyYoshi4kPatch(module, program_id, build_id);
}
} // namespace

TEST_CASE("Yoshi 4K patch writes exactly the contributed bytes", "[yoshi_4k]") {
    auto module = MakeModule();
    REQUIRE(Apply(module));
    CHECK(Same(module, MakeModule(true)));

    // A module that ends with the last patched word is still complete.
    module = MakeModule();
    module.resize(LastWordEnd);
    CHECK(Apply(module));
}

TEST_CASE("Yoshi 4K patch leaves other games, builds and altered or truncated code untouched",
          "[yoshi_4k]") {
    const auto original = MakeModule();
    auto module = original;
    CHECK_FALSE(Apply(module, Settings::StarFoxProgramId));
    CHECK_FALSE(Apply(module, Settings::YoshiMysteriousBookProgramId, "other build"));
    CHECK(Same(module, original));

    // One differing word anywhere, e.g. from a resolution mod, rejects the whole patch.
    for (const auto& site : FileSys::Yoshi4kSites) {
        module[0x100 + site.offset + 3] ^= 0x80;
        CHECK_FALSE(Apply(module));
        module[0x100 + site.offset + 3] ^= 0x80;
        CHECK(Same(module, original));
    }

    module.resize(LastWordEnd - 1);
    const auto truncated = module;
    CHECK_FALSE(Apply(module));
    CHECK(Same(module, truncated));
    module.clear();
    CHECK_FALSE(Apply(module));
}

TEST_CASE("Yoshi 4K option is visible only in Yoshi's own properties", "[yoshi_4k]") {
    for (const auto id : {u64{0}, Settings::YoshiMysteriousBookProgramId,
                          Settings::StarFoxProgramId, u64{0x04007C6028AA8000}}) {
        CHECK_FALSE(Settings::ShowYoshi4kPatch(id, true));
        CHECK(Settings::ShowYoshi4kPatch(id, false) ==
              (id == Settings::YoshiMysteriousBookProgramId));
    }
}
