// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/register_pressure.h"

namespace {
using namespace Shader::Ampere;
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

TEST_CASE("F2IP pair preserves sources, rounding and guard", "[video_core][ampere]") {
    for (const u32 dst : {39U, 40U, 41U}) {   // aliases B, A, neither
        for (const u32 rounding : {0U, 3U}) { // nearest-even and truncate
            for (const u32 guard : {1U, 7U, 9U}) {
                const Instruction input{{0x28000243U | (dst << 16) | (guard << 12), 39,
                                         0x000004ffU | (rounding << 14), 0x000fc400}};
                const auto result = LowerToMaxwell(std::array{input, Exit});
                REQUIRE(static_cast<bool>(result));
                REQUIRE(result.instructions.size() == 16);
                const auto& w = result.instructions;
                // FSET NUM reads each original float. Both conversions finish before
                // the final ISCADD writes Rd, including when it aliases either source.
                REQUIRE(((w[0] >> 8) & 255) == 40);
                REQUIRE(((w[7] >> 8) & 255) == 39);
                REQUIRE((w[6] & 255) == 248);
                REQUIRE((w[13] & 255) == 249);
                REQUIRE(((w[6] >> 39) & 3) == rounding);
                REQUIRE(((w[13] >> 39) & 3) == rounding);
                REQUIRE((w[14] & 255) == dst);
                REQUIRE(((w[14] >> 8) & 255) == 248);
                REQUIRE(((w[14] >> 20) & 255) == 249);
                REQUIRE(((w[14] >> 39) & 31) == 8);
                for (size_t n = 0; n != 15; ++n)
                    REQUIRE(((w[n] >> 16) & 15) == guard);
            }
        }
    }
}

TEST_CASE("F2IP pair rejects unverified modes and tracks both shadowed sources",
          "[video_core][ampere]") {
    const Instruction valid{{0x28297243, 39, 0x000004ff, 0x000fc400}};
    for (const u32 change : {1U, 1U << 10, 1U << 14, 1U << 15}) {
        auto input = valid;
        input.words[2] ^= change; // C != RZ, unverified mode, FLOOR or CEIL
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{input, Exit})));
    }
    // Either float input may be RZ; the other source still contributes.
    for (const u32 zero_source : {0U, 1U}) {
        auto input = valid;
        if (zero_source == 0)
            input.words[0] = (input.words[0] & 0x00ffffffU) | (255U << 24);
        else
            input.words[1] = 255;
        REQUIRE(static_cast<bool>(LowerToMaxwell(std::array{input, Exit})));
    }
    auto bad = valid;
    bad.words[1] |= 1U << 8; // modifier outside the verified source field
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{bad, Exit})));
    bad = valid;
    bad.words[3] |= 1; // unsupported flags
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{bad, Exit})));

    const Instruction high{{0xf7f67243, 248, 0x000004ff, 0x000fc400}};
    const auto pressure = PrepareRegisterPressure(high, 244, 244);
    REQUIRE(pressure.valid);
    REQUIRE(pressure.instruction.Bits(16, 8) < 244);
    REQUIRE(pressure.instruction.Bits(24, 8) < 244);
    REQUIRE(pressure.instruction.Bits(32, 8) < 244);
    REQUIRE(pressure.instruction.Bits(24, 8) != pressure.instruction.Bits(32, 8));
    REQUIRE_FALSE(pressure.prefix.empty());
    REQUIRE_FALSE(pressure.suffix.empty());
}
} // namespace
