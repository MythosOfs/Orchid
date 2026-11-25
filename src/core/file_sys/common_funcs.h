// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <iterator>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace FileSys {

[[nodiscard]] constexpr bool IsFirmwareProgramId(u64 title_id) {
    const u64 platform = title_id >> 56;
    return (platform == 0x01 || platform == 0x04) && (title_id & 0x00FFFFFFFFFFFFFFULL) < 0x2000;
}

constexpr u64 AOC_TITLE_ID_MASK = 0x7FF;
constexpr u64 AOC_TITLE_ID_OFFSET = 0x1000;
constexpr u64 BASE_TITLE_ID_MASK = 0xFFFFFFFFFFFFE000;

/**
 * Gets the base title ID from a given title ID.
 *
 * @param title_id The title ID.
 * @returns The base title ID.
 */
[[nodiscard]] constexpr u64 GetBaseTitleID(u64 title_id) {
    return title_id & BASE_TITLE_ID_MASK;
}

/**
 * Gets the base title ID with a program index offset from a given title ID.
 *
 * @param title_id The title ID.
 * @param program_index The program index.
 * @returns The base title ID with a program index offset.
 */
[[nodiscard]] constexpr u64 GetBaseTitleIDWithProgramIndex(u64 title_id, u64 program_index) {
    return GetBaseTitleID(title_id) + program_index;
}

/**
 * Gets the AOC (Add-On Content) base title ID from a given title ID.
 *
 * @param title_id The title ID.
 * @returns The AOC base title ID.
 */
[[nodiscard]] constexpr u64 GetAOCBaseTitleID(u64 title_id) {
    return GetBaseTitleID(title_id) + AOC_TITLE_ID_OFFSET;
}

/**
 * Gets the AOC (Add-On Content) ID from a given AOC title ID.
 *
 * @param aoc_title_id The AOC title ID.
 * @returns The AOC ID.
 */
[[nodiscard]] constexpr u64 GetAOCID(u64 aoc_title_id) {
    return aoc_title_id & AOC_TITLE_ID_MASK;
}

/// Switch 2 Edition dumps name add-on content after the NX application (0100...) even when the
/// program is the Switch 2 build (0400...): the platform byte follows the running program.
[[nodiscard]] constexpr u64 WithPlatformOf(u64 title_id, u64 program_id) {
    return (title_id & 0x00FFFFFFFFFFFFFFULL) | (program_id & 0xFF00000000000000ULL);
}

[[nodiscard]] constexpr u64 GetSwitch1ApplicationID(u64 title_id) {
    constexpr u64 platform_mask = 0xFF00000000000000ULL;
    constexpr u64 switch2_platform = 0x0400000000000000ULL;
    constexpr u64 switch1_platform = 0x0100000000000000ULL;
    if ((title_id & platform_mask) != switch2_platform) {
        return title_id;
    }
    return (title_id & ~platform_mask) | switch1_platform;
}

[[nodiscard]] constexpr u64 GetAddOnApplicationID(u64 program_id,
                                                  std::span<const u64> add_on_content) {
    const u64 own = GetBaseTitleID(program_id);
    const u64 switch1 = GetSwitch1ApplicationID(own);
    const auto has_add_ons = [add_on_content](u64 base) {
        return std::ranges::any_of(add_on_content,
                                   [base](u64 tid) { return GetBaseTitleID(tid) == base; });
    };
    return switch1 != own && !has_add_ons(own) && has_add_ons(switch1) ? switch1 : own;
}

[[nodiscard]] inline std::vector<u64> GetAddOnContent(u64 program_id,
                                                      std::span<const u64> add_on_content) {
    const u64 base = GetAddOnApplicationID(program_id, add_on_content);
    std::vector<u64> out;
    std::ranges::copy_if(add_on_content, std::back_inserter(out),
                         [base](u64 tid) { return GetBaseTitleID(tid) == base; });
    return out;
}

} // namespace FileSys
