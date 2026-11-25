// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/engines/dlss_dispatch_chain.h"
#include "video_core/engines/dlss_dispatch_trace.h"

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
    chain.NoteDispatch();
    chain.NoteProgram(other);
    CHECK_FALSE(chain.active);
    chain.NoteDispatch();
    chain.NoteProgram(fortunes_weave_prepass);
    CHECK(chain.active);
    chain.NoteDispatch();
    chain.NoteProgram(other);
    chain.NoteDispatch();
    chain.NoteProgram(star_fox_output);
    CHECK(chain.active);
    chain.NoteDispatch();
    chain.NoteProgram(fortunes_weave_output);
    CHECK(chain.active);
    chain.NoteDispatch();
    CHECK_FALSE(chain.active);
    chain.NoteProgram(other);
    CHECK_FALSE(chain.active);
}

TEST_CASE("DLSS dispatch chain restarts on the next prepass", "[video_core][compute_storage]") {
    Tegra::Engines::DlssDispatchChain chain;
    chain.NoteDispatch();
    chain.NoteProgram(star_fox_prepass);
    CHECK(chain.active);
    chain.NoteDispatch();
    chain.NoteProgram(star_fox_output);
    CHECK(chain.active);
    chain.NoteDispatch();
    CHECK_FALSE(chain.active);
    chain.NoteDispatch();
    chain.NoteProgram(fortunes_weave_prepass);
    CHECK(chain.active);
    chain.NoteDispatch();
    chain.NoteProgram(other);
    CHECK(chain.active);
}

TEST_CASE("DLSS dispatch chain ends a bounded run with no output pass",
          "[video_core][compute_storage]") {
    Tegra::Engines::DlssDispatchChain chain;
    chain.NoteProgram(fortunes_weave_prepass);
    for (u32 index = 0; index < 64; ++index) {
        chain.NoteDispatch();
        CHECK(chain.active);
    }
    chain.NoteDispatch();
    CHECK_FALSE(chain.active);
    chain.NoteDispatch();
    CHECK_FALSE(chain.active);
}

TEST_CASE("DLSS dispatch chain re-arms on another game's prepass",
          "[video_core][compute_storage]") {
    Tegra::Engines::DlssDispatchChain chain;
    chain.NoteDispatch();
    chain.NoteProgram(fortunes_weave_prepass);
    chain.NoteDispatch();
    chain.NoteProgram(other);
    chain.NoteDispatch();
    chain.NoteProgram(star_fox_prepass);
    CHECK(chain.active);
    chain.NoteDispatch();
    chain.NoteProgram(fortunes_weave_output);
    CHECK(chain.active);
    chain.NoteDispatch();
    chain.NoteProgram(star_fox_output);
    CHECK(chain.active);
    chain.NoteDispatch();
    CHECK_FALSE(chain.active);
}

TEST_CASE("DLSS dispatch chain ends at the next dispatch without a keyed program",
          "[video_core][compute_storage]") {
    Tegra::Engines::DlssDispatchChain chain;
    chain.NoteProgram(fortunes_weave_prepass);
    chain.NoteDispatch();
    chain.NoteProgram(fortunes_weave_output);
    CHECK(chain.active);
    chain.NoteDispatch();
    CHECK_FALSE(chain.active);
    chain.NoteDispatch();
    CHECK_FALSE(chain.active);
}

TEST_CASE("DLSS dispatch chain counts repeated programs as their own dispatches",
          "[video_core][compute_storage]") {
    Tegra::Engines::DlssDispatchChain chain;
    chain.NoteProgram(fortunes_weave_prepass);
    chain.NoteDispatch();
    chain.NoteProgram(other);
    chain.NoteDispatch();
    chain.NoteProgram(other);
    CHECK(chain.active);
    chain.NoteDispatch();
    chain.NoteProgram(fortunes_weave_prepass);
    CHECK(chain.active);
    chain.NoteDispatch();
    chain.NoteProgram(other);
    chain.NoteDispatch();
    chain.NoteProgram(fortunes_weave_output);
    CHECK(chain.active);
    chain.NoteDispatch();
    CHECK_FALSE(chain.active);
}

TEST_CASE("DLSS dispatch trace captures prepass through the first ordinary dispatch",
          "[video_core][compute_storage]") {
    Tegra::Engines::DlssDispatchTrace trace;
    trace.SetTestEnabled(true);
    using Ssbo = Tegra::Engines::DlssDispatchTrace::SsboRecord;
    const auto ssbo = [](u32 index) {
        return Ssbo{.ssbo_index = index, .cbuf_index = 1, .cbuf_offset = 0x80,
                    .declared_word = 0x1000 + index, .next_word = 0x2000 + index,
                    .chosen_size = 0x4000, .bound = true, .native_compute = true,
                    .dlss_dispatch = index % 2 == 0};
    };

    trace.BeginDispatch();
    trace.NoteProgram(other, false);
    trace.EndDispatch();
    CHECK(trace.Entries().empty());
    CHECK_FALSE(trace.Capturing());

    trace.BeginDispatch();
    trace.NoteProgram(fortunes_weave_prepass, false);
    trace.NoteSsbo(ssbo(0));
    trace.EndDispatch();
    trace.BeginDispatch();
    trace.NoteProgram(other, true);
    trace.EndDispatch();
    trace.BeginDispatch();
    trace.EndDispatch();
    trace.BeginDispatch();
    trace.NoteProgram(fortunes_weave_output, false);
    trace.NoteSsbo(ssbo(1));
    trace.EndDispatch();
    CHECK(trace.Capturing());
    CHECK_FALSE(trace.Emitted());
    trace.BeginDispatch();
    trace.NoteProgram(other, true);
    trace.EndDispatch();

    REQUIRE(trace.Emitted());
    REQUIRE(trace.Entries().size() == 5);
    CHECK(trace.Entries()[0].native_key == fortunes_weave_prepass);
    CHECK(trace.Entries()[0].ssbo_count == 1);
    CHECK(trace.Entries()[0].ssbos[0].declared_word == 0x1000);
    CHECK(trace.Entries()[1].native_key == other);
    CHECK(*trace.Entries()[1].cache_warm);
    CHECK(trace.Entries()[2].native_key == 0);
    CHECK_FALSE(trace.Entries()[2].cache_warm.has_value());
    CHECK(trace.Entries()[3].native_key == fortunes_weave_output);
    CHECK(trace.Entries()[3].ssbos[0].dlss_dispatch == false);
    CHECK(trace.Entries()[3].ssbos[0].chosen_size == 0x4000);
    CHECK(trace.Entries()[4].native_key == other);
    CHECK(trace.Entries()[4].Ordinary());

    trace.BeginDispatch();
    trace.NoteProgram(fortunes_weave_prepass, false);
    trace.EndDispatch();
    CHECK(trace.Entries().size() == 5);
}

TEST_CASE("DLSS dispatch trace restarts on a repeat prepass and ignores a lone output",
          "[video_core][compute_storage]") {
    Tegra::Engines::DlssDispatchTrace trace;
    trace.SetTestEnabled(true);

    trace.BeginDispatch();
    trace.NoteProgram(fortunes_weave_output, false);
    trace.EndDispatch();
    trace.BeginDispatch();
    trace.NoteProgram(other, false);
    trace.EndDispatch();
    CHECK_FALSE(trace.Capturing());
    CHECK(trace.Entries().empty());
    CHECK_FALSE(trace.Emitted());

    trace.BeginDispatch();
    trace.NoteProgram(star_fox_prepass, false);
    trace.EndDispatch();
    trace.BeginDispatch();
    trace.NoteProgram(other, true);
    trace.EndDispatch();
    trace.BeginDispatch();
    trace.NoteProgram(fortunes_weave_prepass, true);
    trace.EndDispatch();
    trace.BeginDispatch();
    trace.NoteProgram(fortunes_weave_output, false);
    trace.EndDispatch();
    trace.BeginDispatch();
    trace.NoteProgram(other, false);
    trace.EndDispatch();

    REQUIRE(trace.Emitted());
    REQUIRE(trace.Entries().size() == 3);
    CHECK(trace.Entries()[0].native_key == fortunes_weave_prepass);
    CHECK(*trace.Entries()[0].cache_warm);
    CHECK(trace.Entries()[1].native_key == fortunes_weave_output);
    CHECK(trace.Entries()[2].native_key == other);
}

TEST_CASE("DLSS dispatch trace emits a truncated capture past the chain bound",
          "[video_core][compute_storage]") {
    Tegra::Engines::DlssDispatchTrace trace;
    trace.SetTestEnabled(true);
    trace.BeginDispatch();
    trace.NoteProgram(star_fox_prepass, false);
    trace.EndDispatch();
    for (u32 i = 0; i < Tegra::Engines::DlssDispatchChain::max_dispatches + 4; ++i) {
        trace.BeginDispatch();
        trace.EndDispatch();
    }
    REQUIRE(trace.Truncated());
    REQUIRE(trace.Emitted());
    CHECK(trace.Entries().size() == Tegra::Engines::DlssDispatchChain::max_dispatches + 2);
}

TEST_CASE("DLSS dispatch trace counts SSBOs past the supported maximum",
          "[video_core][compute_storage]") {
    Tegra::Engines::DlssDispatchTrace trace;
    trace.SetTestEnabled(true);
    using Ssbo = Tegra::Engines::DlssDispatchTrace::SsboRecord;
    trace.BeginDispatch();
    trace.NoteProgram(fortunes_weave_prepass, false);
    for (u32 i = 0; i < Shader::Info::MAX_SSBOS + 3; ++i) {
        trace.NoteSsbo(Ssbo{.ssbo_index = i});
    }
    trace.EndDispatch();
    trace.BeginDispatch();
    trace.NoteProgram(fortunes_weave_output, false);
    trace.EndDispatch();
    trace.BeginDispatch();
    trace.EndDispatch();
    REQUIRE(trace.Emitted());
    const auto& entry = trace.Entries().front();
    CHECK(entry.ssbo_count == Shader::Info::MAX_SSBOS);
    CHECK(entry.ssbo_overflow == 3);
}
