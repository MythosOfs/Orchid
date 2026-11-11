// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

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
