// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <numeric>
#include <catch2/catch_test_macros.hpp>
#include "video_core/texture_cache/pending_download.h"

namespace {
constexpr size_t PAGE = 16;
} // namespace

TEST_CASE("Queued buffer downloads land in the host pages mapped when they were queued",
          "[video_core][pending_download]") {
    // Five pages: 0 and 1 back to back, 2 elsewhere, 3 unmapped, 4 right after 2.
    std::array<u8, PAGE * 8> host{};
    const std::array<u8*, 5> pages{&host[0], &host[PAGE], &host[PAGE * 4], nullptr,
                                   &host[PAGE * 5]};
    const auto get_pointer = [&](u64 address) -> u8* {
        u8* const page = pages[address / PAGE];
        return page ? page + address % PAGE : nullptr;
    };
    // Starts and ends inside a page.
    const u64 address = 4;
    const size_t size = PAGE * 5 - 8;
    // One lookup per page touched; a chunk that stops advancing fails here instead of hanging.
    int lookups = 0;
    const VideoCommon::HostRuns runs =
        VideoCommon::GatherHostRuns(address, size, PAGE, [&](u64 page_address) {
            REQUIRE(++lookups <= 5);
            return get_pointer(page_address);
        });
    REQUIRE(runs.size() == 4);
    CHECK(runs[0] == std::pair{&host[4], PAGE * 2 - 4});
    CHECK(runs[1] == std::pair{&host[PAGE * 4], PAGE});
    CHECK(runs[2] == std::pair<u8*, size_t>{nullptr, PAGE});
    CHECK(runs[3] == std::pair{&host[PAGE * 5], PAGE - 4});

    std::array<u8, PAGE * 5 - 8> data;
    std::iota(data.begin(), data.end(), u8{1});
    VideoCommon::WriteHostRuns(runs, data.data());
    for (u64 offset = 0; offset < size; ++offset) {
        const u8* const target = get_pointer(address + offset);
        if (target != nullptr) {
            CHECK(*target == data[offset]);
        }
    }
    // Nothing outside the mapped destination is touched.
    CHECK(host[3] == 0);
    CHECK(host[PAGE * 2] == 0);
    CHECK(host[PAGE * 6 - 4] == 0);
}

TEST_CASE("Unmapped pages next to each other form one skipped run",
          "[video_core][pending_download]") {
    int lookups = 0;
    const VideoCommon::HostRuns runs = VideoCommon::GatherHostRuns(0, PAGE * 3, PAGE, [&](u64) {
        REQUIRE(++lookups <= 3);
        return static_cast<u8*>(nullptr);
    });
    REQUIRE(runs.size() == 1);
    CHECK(runs[0] == std::pair<u8*, size_t>{nullptr, PAGE * 3});
}
