// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include "common/common_types.h"
#include "common/literals.h"

namespace VideoCommon {
struct TextureCacheMemoryThresholds {
    u64 expected, critical, minimum;
};

// Budget-driven callers have already reserved driver/allocator pressure headroom.
// Retain the legacy ceiling when there is no reliable, scoped device budget.
constexpr TextureCacheMemoryThresholds ClampTextureCacheThresholds(
    TextureCacheMemoryThresholds limits, bool budget_driven, u64 legacy_ceiling) {
    using namespace Common::Literals;
    if (!budget_driven)
        limits.critical = (std::min)(limits.critical, legacy_ceiling);
    limits.expected =
        (std::min)(limits.expected, limits.critical - (std::min)(limits.critical, u64{768_MiB}));
    limits.minimum =
        (std::min)(limits.minimum, limits.expected - (std::min)(limits.expected, u64{1_GiB}));
    return limits;
}

/// Room kept free under the driver's device memory budget before `pressure` and `slack` are added.
/// Titles under the legacy ceiling keep 1/8 of the budget, at most 1 GiB. A budget-driven title's
/// thresholds really sit that close to the budget, and 1 GiB was not enough: Super Mario Bros.
/// Wonder on a 12 GB card (budget 11342 MiB) ran out of memory at 9407/10431 MiB and the host
/// froze, while 7168/8192 MiB ran for 13 minutes without one failed allocation. A quarter of the
/// budget (at most 3 GiB) gives about 7480/8510 MiB there, and 16 GB and larger cards still
/// collect only past 12 GiB.
constexpr u64 TextureCacheBaseMargin(u64 budget, bool budget_driven) {
    using namespace Common::Literals;
    return budget_driven ? (std::min)(budget / 4, u64{3_GiB}) : (std::min)(budget / 8, u64{1_GiB});
}

/// Distance from the critical down to the expected threshold of budget-following titles.
constexpr u64 TextureCacheExpectedStep(u64 budget) {
    using namespace Common::Literals;
    return (std::min)(budget / 8, u64{1_GiB});
}

/// Collector thresholds under a driver budget: the base margin, the room added after images did
/// not fit (`pressure_margin`) and the allocator's unused block space (`slack_margin`) stay free,
/// together at most half the budget. `minimum` is passed through.
constexpr TextureCacheMemoryThresholds TextureCacheBudgetThresholds(u64 budget, bool budget_driven,
                                                                    u64 pressure_margin,
                                                                    u64 slack_margin, u64 minimum) {
    const u64 margin = TextureCacheBaseMargin(budget, budget_driven);
    const u64 critical = budget - (std::min)(margin + pressure_margin + slack_margin, budget / 2);
    const u64 expected = critical - (std::min)(TextureCacheExpectedStep(budget), critical / 2);
    return {expected, critical, minimum};
}

/// The pressure margin after an image did not fit while `usage` bytes were in use. One more step
/// of 512 MiB (up to 3 GiB) is enough when the cache had reached its thresholds. When it had not
/// (Wonder: allocations failed about 1 GiB below the critical threshold, the step left the
/// threshold above the usage, the collector stayed idle and 200 more render targets got no
/// memory), the memory ran out for a reason the budget does not show, such as the host's commit
/// limit. The margin then grows until the critical threshold is under the usage by 1/8 of it
/// (512 MiB to 1 GiB), so the collector frees memory in the very next frames.
/// TextureCacheBudgetThresholds caps all margins at half the budget.
constexpr u64 PressureMarginAfterOutOfMemory(u64 budget, u64 usage, u64 base_margin,
                                             u64 slack_margin, u64 pressure_margin) {
    using namespace Common::Literals;
    const u64 stepped =
        (std::max)(pressure_margin, (std::min)(pressure_margin + u64{512_MiB}, u64{3_GiB}));
    const u64 relief = (std::clamp)(usage / 8, u64{512_MiB}, u64{1_GiB});
    const u64 target = usage - (std::min)(usage, relief) + base_margin + slack_margin;
    const u64 needed = budget > target ? budget - target : 0;
    return (std::min)((std::max)(stepped, needed), budget / 2);
}

/// Frames the collector deletes more images per frame after an image did not fit.
inline constexpr u32 OutOfMemoryCollectFrames = 60;

/// Images one collector pass may delete: 10, 20 under the expected threshold's pressure, 40 over
/// the critical one, and four times that while an image recently did not fit.
constexpr std::size_t CollectorIterations(bool high_priority, bool aggressive, bool out_of_memory) {
    const std::size_t iterations = aggressive ? 40 : (high_priority ? 20 : 10);
    return aggressive && out_of_memory ? iterations * 4 : iterations;
}

/// The pressure margin shrinks by 128 MiB once the pressure has been gone for a while, down to
/// `floor` (what a budget-driven title keeps for the session after running out of memory).
constexpr u64 DecayPressureMargin(u64 pressure_margin, u64 floor) {
    using namespace Common::Literals;
    if (pressure_margin <= floor) {
        return pressure_margin;
    }
    return (std::max)(floor, pressure_margin - (std::min)(pressure_margin, u64{128_MiB}));
}

/// Copies of colour targets their own draws sample are kept per image shape. Dynamic resolution
/// gives every frame size its own shape (Super Mario Bros. Wonder: 420 sizes in nine minutes, about
/// 6 GiB of copies), so only `cap` are kept. `shadows` is ordered least recently used first.
/// Returns the index of the copy to free before making a new one: the oldest one the current draw
/// does not bind, once `cap` exist. Returns `std::dynamic_extent` when nothing needs to (or can) be freed.
inline constexpr std::size_t MaxFeedbackShadows = 8;
template <typename Id>
constexpr std::size_t FeedbackShadowToEvict(std::span<const Id> shadows, std::span<const Id> in_use,
                                            std::size_t cap = MaxFeedbackShadows) {
    if (shadows.size() < cap) {
        return std::dynamic_extent;
    }
    for (std::size_t i = 0; i < shadows.size(); ++i) {
        if (std::ranges::find(in_use, shadows[i]) == in_use.end()) {
            return i;
        }
    }
    return std::dynamic_extent;
}
} // namespace VideoCommon
