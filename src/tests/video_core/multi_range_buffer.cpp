// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/container/unordered_map.h"

#include "video_core/renderer_vulkan/vk_multi_range_buffer.h"

// The cache releases an entry's buffers once it went unbound for longer than the limit; an entry
// bound this frame or within the limit must stay (its buffer is in the frame's descriptors).
TEST_CASE("Multi-range entries are released only after the unused limit", "[multi_range_buffer]") {
    using Cache = Vulkan::MultiRangeBufferCache;
    const u64 limit = Cache::MAX_UNUSED_FRAMES;
    CHECK(limit == 60);
    CHECK_FALSE(Cache::IsUnused(1000, 1000));
    CHECK_FALSE(Cache::IsUnused(1000, 999));
    CHECK_FALSE(Cache::IsUnused(1000 + limit, 1000));
    CHECK(Cache::IsUnused(1001 + limit, 1000));
    CHECK(Cache::IsUnused(5000, 0));
}

// TickFrame's sweep: every entry past the limit is handed to the retire callback exactly once and
// removed, the others stay untouched (their buffers may be in the current frame's descriptors).
TEST_CASE("Multi-range sweep retires and erases only unused entries", "[multi_range_buffer]") {
    using Cache = Vulkan::MultiRangeBufferCache;
    struct Entry {
        u64 last_used;
    };
    const u64 frame = 1000;
    Common::unordered_map<u64, Entry> map{
        {1, {frame}},                                // bound this frame
        {2, {frame - Cache::MAX_UNUSED_FRAMES}},     // exactly at the limit: stays
        {3, {frame - Cache::MAX_UNUSED_FRAMES - 1}}, // one past: goes
        {4, {0}},                                    // long unused: goes
    };
    std::vector<u64> retired;
    Cache::EraseUnused(map, frame, [&](Entry& entry) { retired.push_back(entry.last_used); });
    CHECK(map.size() == 2);
    CHECK(map.contains(1));
    CHECK(map.contains(2));
    REQUIRE(retired.size() == 2);
    CHECK(retired[0] + retired[1] == frame - Cache::MAX_UNUSED_FRAMES - 1);

    // Nothing unused: nothing retired, nothing removed.
    retired.clear();
    Cache::EraseUnused(map, frame, [&](Entry& entry) { retired.push_back(entry.last_used); });
    CHECK(map.size() == 2);
    CHECK(retired.empty());
}
