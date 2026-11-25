// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>
#include "common/common_types.h"

namespace VideoCommon {

enum class StorageBufferSizeRule : u8 {
    NotDetermined,
    Declared,
    BarePointer,
    FallbackCap,
};

// A bare pointer's following word is another parameter, not a size.
[[nodiscard]] constexpr std::pair<u32, StorageBufferSizeRule> StorageBufferSizeWithRule(
    u64 kind_extent, u64 next_word, bool bare_pointer) {
    constexpr u32 mib = 1024 * 1024;
    if (bare_pointer) {
        return {static_cast<u32>((std::min)(kind_extent, u64{8U * mib})),
                StorageBufferSizeRule::BarePointer};
    }
    const u32 packed_size = static_cast<u32>(next_word);
    if ((next_word >> 32) == 0 && packed_size != 0 && packed_size <= kind_extent) {
        return {packed_size, StorageBufferSizeRule::Declared};
    }
    return {static_cast<u32>((std::min)(kind_extent, u64{8U * mib})),
            StorageBufferSizeRule::FallbackCap};
}

[[nodiscard]] constexpr u32 StorageBufferSizeLimit(u64 kind_extent, u64 next_word,
                                                   bool bare_pointer) {
    return StorageBufferSizeWithRule(kind_extent, next_word, bare_pointer).first;
}

// The mapping API splits on both physical discontinuities and unmapped holes. Adjacent
// virtual ranges still form one usable prefix for the multi-range storage path.
[[nodiscard]] inline u32 StorageBufferMappedPrefix(
    u64 start, u32 limit, std::span<const std::pair<u64, std::size_t>> ranges) {
    u32 mapped = 0;
    for (const auto& [address, length] : ranges) {
        if (address != start + mapped || length > limit - mapped) {
            break;
        }
        mapped += static_cast<u32>(length);
    }
    return mapped;
}

[[nodiscard]] constexpr u32 StorageBufferAlignedSize(u64 offset, u64 size, u64 limit) {
    const u64 cap = (std::min)(limit, u64{std::numeric_limits<u32>::max()});
    return static_cast<u32>((std::min)(offset + size, cap));
}

} // namespace VideoCommon
