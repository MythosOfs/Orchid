// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/engines/dlss_dispatch_chain.h"

namespace {
constexpr u64 fortunes_weave_prepass = 0x4cbf6eff32525aecULL;
constexpr u64 fortunes_weave_output = 0x14b4222bbe23abeeULL;
constexpr u64 star_fox_prepass = 0xd686ca0b9fad9e6dULL;
constexpr u64 star_fox_output = 0x7d3764ff101bec27ULL;
constexpr u64 other = 0x00c0ffee00decafeULL;
}

TEST_CASE("DLSS dispatch chain spans an evaluation's prepass through output pass",
          "[video_core][compute_storage]") {
    Tegra::Engines::DlssDispatchChain chain;
    CHECK_FALSE(chain.active);
    chain.NoteProgram(other);
    CHECK_FALSE(chain.active);
    chain.NoteProgram(fortunes_weave_prepass);
    CHECK(chain.active);
    chain.NoteProgram(other);
    chain.NoteProgram(star_fox_output);
    CHECK(chain.active);
    chain.NoteProgram(fortunes_weave_output);
    CHECK(chain.active);
    chain.NoteProgram(other);
    CHECK_FALSE(chain.active);
    chain.NoteProgram(other);
    CHECK_FALSE(chain.active);
}

TEST_CASE("DLSS dispatch chain restarts on the next prepass", "[video_core][compute_storage]") {
    Tegra::Engines::DlssDispatchChain chain;
    chain.NoteProgram(star_fox_prepass);
    CHECK(chain.active);
    chain.NoteProgram(fortunes_weave_output);
    CHECK(chain.active);
    chain.NoteProgram(star_fox_output);
    chain.NoteProgram(fortunes_weave_prepass);
    CHECK(chain.active);
    chain.NoteProgram(star_fox_output);
    CHECK(chain.active);
    chain.NoteProgram(fortunes_weave_output);
    chain.NoteProgram(other);
    CHECK_FALSE(chain.active);
}

TEST_CASE("DLSS dispatch chain ends a bounded run with no output pass",
          "[video_core][compute_storage]") {
    Tegra::Engines::DlssDispatchChain chain;
    chain.NoteProgram(fortunes_weave_prepass);
    for (u32 index = 0; index < 64; ++index) {
        chain.NoteProgram(other);
        CHECK(chain.active);
    }
    chain.NoteProgram(other);
    CHECK_FALSE(chain.active);
    chain.NoteProgram(other);
    CHECK_FALSE(chain.active);
}

TEST_CASE("DLSS dispatch chain re-arms on another game's prepass",
          "[video_core][compute_storage]") {
    Tegra::Engines::DlssDispatchChain chain;
    chain.NoteProgram(fortunes_weave_prepass);
    chain.NoteProgram(other);
    chain.NoteProgram(star_fox_prepass);
    CHECK(chain.active);
    chain.NoteProgram(fortunes_weave_output);
    CHECK(chain.active);
    chain.NoteProgram(star_fox_output);
    CHECK(chain.active);
    chain.NoteProgram(other);
    CHECK_FALSE(chain.active);
}
