// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include "common/common_types.h"

namespace VideoCommon {

/// Everything a storage buffer binding is resolved from. The mapping generation stands for the
/// page tables and page kinds: a key of an older generation is never asked for again.
struct StorageBindingKey {
    size_t address_space;
    u64 generation;
    u64 pointer;
    u64 next_word;
    u32 alignment;
    bool is_written;
    bool native_compute;
    u64 native_limit;
    bool dlss_dispatch;

    bool operator==(const StorageBindingKey&) const = default;
};

/// Keeps resolved storage buffer bindings, four per set, replacing the least recently used.
template <typename Value>
class StorageBindingCache {
public:
    static constexpr size_t SETS = 512;
    static constexpr size_t WAYS = 4;

    [[nodiscard]] static constexpr size_t SetIndex(const StorageBindingKey& key) {
        return ((key.pointer ^ key.next_word) * 0x9E3779B97F4A7C15ULL) >> 55;
    }

    /// Returns the value stored for the key and makes it the last of its set to be replaced.
    [[nodiscard]] const Value* Find(const StorageBindingKey& key) {
        auto& set = sets[SetIndex(key)];
        const auto entry = std::ranges::find_if(
            set, [&key](const Entry& candidate) { return candidate.used && candidate.key == key; });
        if (entry == set.end()) {
            return nullptr;
        }
        std::rotate(set.begin(), entry, entry + 1);
        return &set.front().value;
    }

    /// Stores the value of a key Find does not have.
    void Insert(const StorageBindingKey& key, const Value& value) {
        auto& set = sets[SetIndex(key)];
        std::rotate(set.begin(), set.end() - 1, set.end());
        set.front() = {key, value, true};
    }

private:
    struct Entry {
        StorageBindingKey key;
        Value value;
        bool used;
    };

    std::array<std::array<Entry, WAYS>, SETS> sets{};
};

} // namespace VideoCommon
