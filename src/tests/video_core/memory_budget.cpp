// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/literals.h"
#include "video_core/texture_cache/memory_budget.h"
#include "video_core/vulkan_common/memory_budget.h"

using namespace Common::Literals;

TEST_CASE("Host commit pressure limits reclaimable cache budgets", "[memory_budget]") {
    using Vulkan::LimitBudgetForHostCommit;
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 20_GiB, 4_GiB) == 10_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 5_GiB, 4_GiB) == 9_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 4_GiB, 4_GiB) == 8_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 3_GiB, 4_GiB) == 7_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 0, 4_GiB) == 4_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 1_GiB, 0, 4_GiB) == 0);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, 5_GiB, 4_GiB) ==
            LimitBudgetForHostCommit(10_GiB, 7_GiB, 6_GiB, 4_GiB));
    REQUIRE(LimitBudgetForHostCommit(6_GiB, 8_GiB, 20_GiB, 4_GiB) == 6_GiB);
    REQUIRE(LimitBudgetForHostCommit(6_GiB, 8_GiB, 3_GiB, 4_GiB) == 6_GiB);
    REQUIRE(LimitBudgetForHostCommit(10_GiB, 8_GiB, std::numeric_limits<u64>::max(), 4_GiB) ==
            10_GiB);
}

TEST_CASE("Budget-aware texture cache retains fitting Overworld working sets", "[memory_budget]") {
    using VideoCommon::ClampTextureCacheThresholds;
    // An 8-GiB working set fits a 10/11-GiB driver-adjusted policy; the legacy
    // ceiling would collect it repeatedly despite several GiB of safe headroom.
    const auto roomy = ClampTextureCacheThresholds({10_GiB, 11_GiB, 2_GiB}, true, 5_GiB);
    CHECK(8_GiB < roomy.expected);
    CHECK(roomy.critical == 11_GiB);
    CHECK(roomy.expected < roomy.critical);
    const auto fallback = ClampTextureCacheThresholds({10_GiB, 11_GiB, 2_GiB}, false, 5_GiB);
    CHECK(fallback.critical == 5_GiB);
    CHECK(fallback.expected < 5_GiB);
    // Budget shrink and low-memory devices must keep their lower thresholds.
    const auto smaller = ClampTextureCacheThresholds({2_GiB, 3_GiB, 1536_MiB}, true, 5_GiB);
    CHECK(smaller.expected == 2_GiB);
    CHECK(smaller.critical == 3_GiB);
    CHECK(smaller.minimum == 1_GiB);
    const auto tiny = ClampTextureCacheThresholds({256_MiB, 512_MiB, 128_MiB}, true, 5_GiB);
    CHECK(tiny.expected == 0);
    CHECK(tiny.minimum == 0);
}

TEST_CASE("Feedback shadow copies stay bounded", "[memory_budget]") {
    using VideoCommon::FeedbackShadowToEvict;
    constexpr auto none = std::dynamic_extent;
    const std::vector<int> shadows{10, 11, 12};
    const std::vector<int> nothing_bound;
    // Below the cap a new shape gets its own copy.
    REQUIRE(FeedbackShadowToEvict<int>(shadows, nothing_bound, 4) == none);
    // At the cap the least recently used copy (front) goes.
    REQUIRE(FeedbackShadowToEvict<int>(shadows, nothing_bound, 3) == 0);
    REQUIRE(FeedbackShadowToEvict<int>(shadows, nothing_bound, 2) == 0);
    // Copies bound by the current draw are skipped; all bound keeps them all.
    const std::vector<int> front_bound{10};
    REQUIRE(FeedbackShadowToEvict<int>(shadows, front_bound, 3) == 1);
    const std::vector<int> two_bound{11, 10};
    REQUIRE(FeedbackShadowToEvict<int>(shadows, two_bound, 3) == 2);
    REQUIRE(FeedbackShadowToEvict<int>(shadows, shadows, 3) == none);
}

TEST_CASE("Images replaced by a newer write over their memory are found", "[memory_budget]") {
    using VideoCommon::ReplacedByNewerWrite;
    const std::vector<u64> ticks{5, 9, 12};
    const auto tick_of = [&](size_t index) { return ticks[index]; };
    const std::vector<size_t> none{};
    const std::vector<size_t> older{0, 1};
    const std::vector<size_t> with_newer{0, 2};
    CHECK_FALSE(ReplacedByNewerWrite(10, none, tick_of));
    CHECK_FALSE(ReplacedByNewerWrite(10, older, tick_of));
    CHECK(ReplacedByNewerWrite(10, with_newer, tick_of));
    // A write in the same tick is not newer: the image may still hold that frame's data.
    CHECK_FALSE(ReplacedByNewerWrite(12, with_newer, tick_of));
    CHECK(ReplacedByNewerWrite(11, with_newer, tick_of));
}
