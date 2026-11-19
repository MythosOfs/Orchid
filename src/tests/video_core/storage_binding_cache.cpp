// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/storage_binding_cache.h"

namespace {

using Cache = VideoCommon::StorageBindingCache<u32>;
using Key = VideoCommon::StorageBindingKey;

constexpr Key BASE{
    .address_space = 3,
    .generation = 7,
    .pointer = 0x1'2345'6000,
    .next_word = 0x10a,
    .alignment = 16,
    .is_written = false,
    .native_compute = true,
};

// Keys that differ only in the pointer and share the set of the first one.
std::vector<Key> CollidingKeys(size_t count) {
    std::vector<Key> keys{BASE};
    for (u64 pointer = BASE.pointer + 0x1000; keys.size() < count; pointer += 0x1000) {
        Key key = BASE;
        key.pointer = pointer;
        if (Cache::SetIndex(key) == Cache::SetIndex(BASE)) {
            keys.push_back(key);
        }
    }
    return keys;
}

} // namespace

TEST_CASE("Storage binding cache answers only the key it stored", "[video_core][storage_binding]") {
    const auto cache = std::make_unique<Cache>();
    CHECK(cache->Find(BASE) == nullptr);
    CHECK(cache->Find(Key{}) == nullptr);

    cache->Insert(BASE, 42);
    const u32* const hit = cache->Find(BASE);
    REQUIRE(hit != nullptr);
    CHECK(*hit == 42);

    const auto misses = [&](auto change) {
        Key key = BASE;
        change(key);
        return cache->Find(key) == nullptr;
    };
    CHECK(misses([](Key& key) { key.address_space = 4; }));
    // A mapping change moves the generation: the binding is resolved again.
    CHECK(misses([](Key& key) { key.generation = 8; }));
    CHECK(misses([](Key& key) { key.pointer += 0x1000; }));
    CHECK(misses([](Key& key) { key.next_word = 0x10b; }));
    CHECK(misses([](Key& key) { key.alignment = 64; }));
    CHECK(misses([](Key& key) { key.is_written = true; }));
    CHECK(misses([](Key& key) { key.native_compute = false; }));
    REQUIRE(cache->Find(BASE) != nullptr);
    CHECK(*cache->Find(BASE) == 42);
}

TEST_CASE("Storage binding cache keeps the four most recently used keys of a set",
          "[video_core][storage_binding]") {
    const auto keys = CollidingKeys(Cache::WAYS + 2);
    const auto cache = std::make_unique<Cache>();
    for (u32 i = 0; i < Cache::WAYS; ++i) {
        cache->Insert(keys[i], i);
    }
    for (u32 i = 0; i < Cache::WAYS; ++i) {
        REQUIRE(cache->Find(keys[i]) != nullptr);
        CHECK(*cache->Find(keys[i]) == i);
    }

    // Used last, the first key outlives the second.
    REQUIRE(cache->Find(keys[0]) != nullptr);
    cache->Insert(keys[4], 4);
    CHECK(cache->Find(keys[1]) == nullptr);
    for (const u32 i : {0U, 2U, 3U, 4U}) {
        REQUIRE(cache->Find(keys[i]) != nullptr);
        CHECK(*cache->Find(keys[i]) == i);
    }

    // The loop above used 0, 2, 3, 4 in that order.
    cache->Insert(keys[5], 5);
    CHECK(cache->Find(keys[0]) == nullptr);
    for (const u32 i : {2U, 3U, 4U, 5U}) {
        REQUIRE(cache->Find(keys[i]) != nullptr);
        CHECK(*cache->Find(keys[i]) == i);
    }
}

TEST_CASE("Storage binding cache spreads page-aligned pointers over its sets",
          "[video_core][storage_binding]") {
    // The multiplicative hash of the pointer and the word after it, top nine bits.
    CHECK(Cache::SetIndex(BASE) == 443);
    Key key = BASE;
    key.next_word = 0x10b;
    CHECK(Cache::SetIndex(key) == 248);
    key = BASE;
    key.pointer += 0x1000;
    CHECK(Cache::SetIndex(key) == 171);
    key.pointer = 0xff'ffff'0000;
    key.next_word = 0;
    CHECK(Cache::SetIndex(key) == 417);
    // A graphics descriptor's next word can be any 64 bits.
    key = BASE;
    key.next_word = 0xfedc'ba98'7654'3210;
    CHECK(Cache::SetIndex(key) == 509);
    key.pointer = 0xff'ffff'0000;
    key.next_word = 0x8000'0000'0000'0001;
    CHECK(Cache::SetIndex(key) == 477);

    const auto cache = std::make_unique<Cache>();
    constexpr u32 count = 256;
    for (u32 i = 0; i < count; ++i) {
        Key key = BASE;
        key.pointer = 0x4000'0000ULL + u64{i} * 0x1000;
        cache->Insert(key, i);
    }
    u32 kept = 0;
    for (u32 i = 0; i < count; ++i) {
        Key key = BASE;
        key.pointer = 0x4000'0000ULL + u64{i} * 0x1000;
        const u32* const hit = cache->Find(key);
        kept += hit != nullptr && *hit == i ? 1 : 0;
    }
    CHECK(kept == count);
}
