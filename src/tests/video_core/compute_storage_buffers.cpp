// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <limits>
#include <tuple>
#include <utility>
#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/buffer_cache/storage_buffer_size.h"

TEST_CASE("Bare DLSS pointers size from the mapping; descriptors use their declared size",
          "[video_core][compute_storage]") {
    constexpr u32 mib = 1024 * 1024;
    using VideoCommon::StorageBufferSizeLimit;
    // A bare pointer's next word is another parameter, not a size, even a small plausible one.
    CHECK(StorageBufferSizeLimit(4 * mib, 0x10a, true) == 4 * mib);
    CHECK(StorageBufferSizeLimit(16 * mib, 0x10a, true) == 8 * mib);
    // Descriptor bindings (graphics and non-DLSS native dispatches) declare their size in it.
    CHECK(StorageBufferSizeLimit(16 * mib, 0x10a, false) == 0x10aU);
    CHECK(StorageBufferSizeLimit(16 * mib, 12 * mib, false) == 12 * mib);
    CHECK(StorageBufferSizeLimit(4 * mib, 12 * mib, false) == 4 * mib);
    CHECK(StorageBufferSizeLimit(4 * mib, 0, false) == 4 * mib);
    CHECK(StorageBufferSizeLimit(4 * mib, 0x10000010aULL, false) == 4 * mib);
    // Cap before narrowing: a kind range of 4 GiB must not wrap to zero.
    constexpr u64 four_gib = 1ULL << 32;
    CHECK(StorageBufferSizeLimit(four_gib, 0x10a, true) == 8 * mib);
    CHECK(StorageBufferSizeLimit(0, 0x10a, true) == 0U);
    CHECK(StorageBufferSizeLimit(0x1000, 0x10a, true) == 0x1000U);
}

TEST_CASE("Native storage mapped prefix joins adjacent virtual segments and stops at gaps",
          "[video_core][compute_storage]") {
    using VideoCommon::StorageBufferMappedPrefix;
    constexpr u64 base = 0x10000;
    constexpr std::array adjacent{std::pair<u64, std::size_t>{base, 0x1000},
                                  std::pair<u64, std::size_t>{base + 0x1000, 0x1000}};
    constexpr std::array gap{std::pair<u64, std::size_t>{base, 0x1000},
                             std::pair<u64, std::size_t>{base + 0x2000, 0x1000}};
    constexpr std::array missing_start{std::pair<u64, std::size_t>{base + 0x1000, 0x1000}};
    constexpr std::array unaligned{std::pair<u64, std::size_t>{base + 0x80, 0xf80},
                                   std::pair<u64, std::size_t>{base + 0x1000, 0x140}};
    CHECK(StorageBufferMappedPrefix(base, 0x2000, adjacent) == 0x2000U);
    CHECK(StorageBufferMappedPrefix(base, 0x3000, gap) == 0x1000U);
    CHECK(StorageBufferMappedPrefix(base, 0x2000, missing_start) == 0U);
    CHECK(StorageBufferMappedPrefix(base, 0x1800, adjacent) == 0x1000U);
    CHECK(StorageBufferMappedPrefix(base + 0x80, 0x10c0, unaligned) == 0x10c0U);
}

TEST_CASE("Aligned storage ranges stay representable and inside the backend limit",
          "[video_core][compute_storage]") {
    using VideoCommon::StorageBufferAlignedSize;
    constexpr u64 mib128 = 128ULL * 1024 * 1024;
    constexpr u64 u32_max = std::numeric_limits<u32>::max();
    CHECK(StorageBufferAlignedSize(0, 256ULL * 1024 * 1024, mib128) == mib128);
    CHECK(StorageBufferAlignedSize(8, 0xfffffff8ULL, mib128) == mib128);
    CHECK(StorageBufferAlignedSize(8, 0xfffffff8ULL, u32_max) == u32_max);
    CHECK(StorageBufferAlignedSize(0xf8, 0xfffffff8ULL, u32_max) == u32_max);
    CHECK(StorageBufferAlignedSize(0, 512, mib128) == 512U);
    CHECK(StorageBufferAlignedSize(8, 512, mib128) == 520U);
    CHECK(StorageBufferAlignedSize(0, 0, mib128) == 0U);
}

TEST_CASE("Compute storage bindings cover every shader descriptor and mask bit",
          "[video_core][compute_storage]") {
    using State = VideoCommon::BufferCacheChannelInfo;
    constexpr size_t count = std::tuple_size_v<decltype(State::compute_storage_buffers)>;
    STATIC_REQUIRE(count == Shader::Info::MAX_SSBOS);
    STATIC_REQUIRE(count <= std::numeric_limits<decltype(State::enabled_compute_storage_buffers)>::digits);
    STATIC_REQUIRE(count <= std::numeric_limits<decltype(State::written_compute_storage_buffers)>::digits);
}
