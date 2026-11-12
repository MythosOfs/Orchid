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
    REQUIRE(VideoCommon::MaxFeedbackShadows == 8);
}

TEST_CASE("Budget-driven thresholds keep a quarter of the budget free", "[memory_budget]") {
    using VideoCommon::TextureCacheBaseMargin;
    using VideoCommon::TextureCacheBudgetThresholds;
    // Titles under the legacy ceiling keep the margin they had: 1/8 of the budget, at most 1 GiB,
    // and the expected threshold the same distance below the critical one.
    for (const u64 budget : {2_GiB, 6_GiB, 8_GiB, 12_GiB, 16_GiB, 24_GiB}) {
        const u64 margin = (std::min)(budget / 8, u64{1_GiB});
        CHECK(TextureCacheBaseMargin(budget, false) == margin);
        const auto legacy = TextureCacheBudgetThresholds(budget, false, 0, 0, 1536_MiB);
        CHECK(legacy.critical == budget - margin);
        CHECK(legacy.expected == budget - 2 * margin);
        CHECK(legacy.minimum == 1536_MiB);
    }
    // Budget-driven: a quarter of the budget, at most 3 GiB.
    const auto small = TextureCacheBudgetThresholds(8_GiB, true, 0, 0, 0);
    CHECK(small.critical == 6_GiB);
    CHECK(small.expected == 5_GiB);
    const auto medium = TextureCacheBudgetThresholds(12_GiB, true, 0, 0, 0);
    CHECK(medium.critical == 9_GiB);
    CHECK(medium.expected == 8_GiB);
    const auto large = TextureCacheBudgetThresholds(16_GiB, true, 0, 0, 0);
    CHECK(large.critical == 13_GiB);
    CHECK(large.expected == 12_GiB);
    const auto huge = TextureCacheBudgetThresholds(24_GiB, true, 0, 0, 0);
    CHECK(huge.critical == 21_GiB);
    CHECK(huge.expected == 20_GiB);
    const auto tiny = TextureCacheBudgetThresholds(2_GiB, true, 0, 0, 0);
    CHECK(tiny.critical == 1536_MiB);
    CHECK(tiny.expected == 1280_MiB);
    // Issue 123, a 12 GB card with a budget of 11342 MiB and 17 MiB of block slack: 9407/10431 MiB
    // ran out of memory and froze the host, 7168/8192 MiB ran for 13 minutes.
    const auto issue = TextureCacheBudgetThresholds(11342_MiB, true, 0, 17_MiB, 1536_MiB);
    CHECK(issue.critical < 8704_MiB);
    CHECK(issue.critical >= 8192_MiB);
    CHECK(issue.expected + 1_GiB == issue.critical);
    CHECK(issue.minimum == 1536_MiB);
    // The other margins come off the critical threshold too, all together at most half the budget.
    const auto pressed = TextureCacheBudgetThresholds(12_GiB, true, 1_GiB, 512_MiB, 0);
    CHECK(pressed.critical == 9_GiB - 1536_MiB);
    CHECK(pressed.expected == 8_GiB - 1536_MiB);
    const auto capped = TextureCacheBudgetThresholds(12_GiB, true, 6_GiB, 1_GiB, 0);
    CHECK(capped.critical == 6_GiB);
    CHECK(capped.expected == 5_GiB);
}

TEST_CASE("An image that did not fit puts the critical threshold under the usage",
          "[memory_budget]") {
    using VideoCommon::PressureMarginAfterOutOfMemory;
    using VideoCommon::TextureCacheBaseMargin;
    using VideoCommon::TextureCacheBudgetThresholds;
    // Wherever the usage was when memory ran out, the collector must be in aggressive mode
    // afterwards (usage at or over critical) with at least 512 MiB to free.
    for (const bool budget_driven : {false, true}) {
        const u64 budget = 11342_MiB;
        const u64 slack = 17_MiB;
        const u64 base = TextureCacheBaseMargin(budget, budget_driven);
        const u64 before =
            TextureCacheBudgetThresholds(budget, budget_driven, 0, slack, 0).critical;
        for (u64 usage = budget / 2 + 1_GiB; usage <= before; usage += 256_MiB) {
            const u64 margin = PressureMarginAfterOutOfMemory(budget, usage, base, slack, 0);
            const u64 after =
                TextureCacheBudgetThresholds(budget, budget_driven, margin, slack, 0).critical;
            CAPTURE(budget_driven, usage >> 20, margin >> 20, after >> 20);
            CHECK(margin >= 512_MiB);
            CHECK(after + 512_MiB <= usage);
            // No further than 1/8 of the usage (at most 1 GiB) or the usual step.
            CHECK(after + (std::max)(u64{1_GiB}, before - usage + 512_MiB) >= usage);
        }
    }
    // The run of issue 123: thresholds 9407/10431 MiB, 1 GiB base margin, memory ran out around
    // 9900 MiB. The old 512 MiB step left the critical threshold at 9919 MiB, above the usage.
    const u64 margin = PressureMarginAfterOutOfMemory(11472_MiB, 9900_MiB, 1_GiB, 17_MiB, 0);
    CHECK(margin == 11472_MiB - 1_GiB - 17_MiB - (9900_MiB - 1_GiB));
    // A cache that had reached its thresholds takes the step it always took.
    CHECK(PressureMarginAfterOutOfMemory(7578_MiB, 7200_MiB, 947_MiB, 300_MiB, 0) == 512_MiB);
    CHECK(PressureMarginAfterOutOfMemory(7578_MiB, 7200_MiB, 947_MiB, 300_MiB, 512_MiB) == 1_GiB);
    CHECK(PressureMarginAfterOutOfMemory(12_GiB, 12_GiB, 1_GiB, 0, 2816_MiB) == 3_GiB);
    CHECK(PressureMarginAfterOutOfMemory(12_GiB, 12_GiB, 1_GiB, 0, 3_GiB) == 3_GiB);
    // It never shrinks, and never asks for more than half the budget.
    CHECK(PressureMarginAfterOutOfMemory(12_GiB, 12_GiB, 1_GiB, 0, 4_GiB) == 4_GiB);
    CHECK(PressureMarginAfterOutOfMemory(12_GiB, 0, 3_GiB, 0, 0) == 6_GiB);
    CHECK(PressureMarginAfterOutOfMemory(12_GiB, 1_GiB, 3_GiB, 0, 0) == 6_GiB);
}

TEST_CASE("The out of memory margin decays to its floor", "[memory_budget]") {
    using VideoCommon::DecayPressureMargin;
    CHECK(DecayPressureMargin(512_MiB, 0) == 384_MiB);
    CHECK(DecayPressureMargin(100_MiB, 0) == 0);
    CHECK(DecayPressureMargin(0, 0) == 0);
    CHECK(DecayPressureMargin(1_GiB, 512_MiB) == 896_MiB);
    CHECK(DecayPressureMargin(576_MiB, 512_MiB) == 512_MiB);
    CHECK(DecayPressureMargin(512_MiB, 512_MiB) == 512_MiB);
    CHECK(DecayPressureMargin(256_MiB, 512_MiB) == 256_MiB);
}

TEST_CASE("The collector deletes more per frame after an image did not fit", "[memory_budget]") {
    using VideoCommon::CollectorIterations;
    CHECK(CollectorIterations(false, false, false) == 10);
    CHECK(CollectorIterations(true, false, false) == 20);
    CHECK(CollectorIterations(true, true, false) == 40);
    CHECK(CollectorIterations(true, true, true) == 160);
    // Only once the usage is over the critical threshold.
    CHECK(CollectorIterations(true, false, true) == 20);
    CHECK(CollectorIterations(false, false, true) == 10);
    CHECK(VideoCommon::OutOfMemoryCollectFrames == 60);
}
