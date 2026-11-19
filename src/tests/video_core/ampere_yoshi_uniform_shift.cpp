// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using namespace Shader::Ampere;
constexpr Instruction Captured{{0x3f0b0299, 0x00000005, 0x08011609, 0x000fe200}};
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

TEST_CASE("Yoshi UP0 uniform right shift preserves guard and operands",
          "[video_core][yoshi_ushf]") {
    for (const u32 dest : {11U, 5U, 9U}) {
        auto insn = Captured;
        insn.words[0] = (insn.words[0] & ~0xff0000U) | (dest << 16);
        const auto lowered = LowerToMaxwell(std::array{insn, Exit});
        INFO(lowered.error);
        REQUIRE(lowered);
        REQUIRE(lowered.instructions.size() == 5);
        constexpr u64 mask = 64;
        CHECK(lowered.instructions[0] ==
              (0x38e8000000070000ULL | 253ULL | (255ULL << 8) | (mask << 20)));
        CHECK(lowered.instructions[1] == (0x38f0000000070000ULL | (239ULL << 8) | (mask << 20)));
        CHECK(lowered.instructions[2] == (0x5c28000000000000ULL | (6ULL << 16) | (216ULL + dest) |
                                          (225ULL << 8) | (221ULL << 20)));
        CHECK(lowered.instructions[3] == (0x38f0000000070000ULL | (253ULL << 8) | (mask << 20)));
    }
}

TEST_CASE("Yoshi uniform shift retains unsupported mode guards", "[video_core][yoshi_ushf]") {
    for (const u32 guard : {1U, 8U}) {
        auto i = Captured;
        i.words[0] = (i.words[0] & ~0xf000U) | (guard << 12);
        CHECK_FALSE(LowerToMaxwell(std::array{i, Exit}));
    }
    for (const auto [word, bit] :
         std::array{std::pair{2U, 8U}, std::pair{2U, 10U}, std::pair{2U, 9U}}) {
        auto i = Captured;
        i.words[word] ^= 1U << bit;
        CHECK_FALSE(LowerToMaxwell(std::array{i, Exit}));
    }
    for (const u32 bit : {0U, 8U, 30U, 31U}) {
        auto i = Captured;
        i.words[3] ^= 1U << bit;
        CHECK_FALSE(LowerToMaxwell(std::array{i, Exit}));
    }
    for (const u32 bit : {11U, 12U, 16U}) {
        auto i = Captured;
        i.words[2] ^= 1U << bit;
        CHECK_FALSE(LowerToMaxwell(std::array{i, Exit}));
    }
}
} // namespace
