// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/address_space.h"

namespace {
using Allocator = Common::FlatAllocator<u32, 0, 32>;

TEST_CASE("FlatAllocator hands out consecutive ranges", "[common][flat_allocator]") {
    Allocator allocator{0x10, 0x1000};
    CHECK(allocator.Allocate(0x10) == 0x10);
    CHECK(allocator.Allocate(0x20) == 0x20);
    CHECK(allocator.Allocate(0x8) == 0x40);
}

TEST_CASE("FlatAllocator does not overlap a fixed range ahead of its linear end",
          "[common][flat_allocator]") {
    Allocator allocator{0x10, 0x1000};
    REQUIRE(allocator.Allocate(0x10) == 0x10);
    allocator.AllocateFixed(0x28, 0x8); // a guest's fixed reservation inside the next range
    CHECK(allocator.Allocate(0x10) == 0x30);
    CHECK(allocator.Allocate(0x4) == 0x40);
}

TEST_CASE("FlatAllocator uses the free tail behind several fixed ranges",
          "[common][flat_allocator]") {
    Allocator allocator{0x10, 0x1000};
    REQUIRE(allocator.Allocate(0x10) == 0x10);
    allocator.AllocateFixed(0x24, 0x4);
    allocator.AllocateFixed(0x30, 0x4);
    CHECK(allocator.Allocate(0x20) == 0x34);
}

TEST_CASE("FlatAllocator skips a free gap too small for the request",
          "[common][flat_allocator]") {
    Allocator allocator{0x10, 0x1000};
    REQUIRE(allocator.Allocate(0x10) == 0x10);
    allocator.AllocateFixed(0x24, 0x8);
    allocator.AllocateFixed(0x34, 0x4);
    CHECK(allocator.Allocate(0x10) == 0x38);
}

TEST_CASE("FlatAllocator does not take a large fixed range", "[common][flat_allocator]") {
    Allocator allocator{0x10, 0x1000};
    REQUIRE(allocator.Allocate(0x10) == 0x10);
    allocator.AllocateFixed(0x28, 0x18);
    CHECK(allocator.Allocate(0x10) == 0x40);
}

TEST_CASE("FlatAllocator takes a free gap between fixed ranges", "[common][flat_allocator]") {
    Allocator allocator{0x10, 0x1000};
    REQUIRE(allocator.Allocate(0x10) == 0x10);
    allocator.AllocateFixed(0x24, 0x4);
    allocator.AllocateFixed(0x40, 0x4);
    CHECK(allocator.Allocate(0x10) == 0x28);
}

TEST_CASE("FlatAllocator reports a full address space", "[common][flat_allocator]") {
    Allocator allocator{0x10, 0x40};
    REQUIRE(allocator.Allocate(0x30) == 0x10);
    CHECK(allocator.Allocate(0x10) == 0);
}
} // namespace
