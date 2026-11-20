// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"

namespace {
constexpr GPUVAddr Base = 0x3185333000;
constexpr u64 Size = 0x80000;
}

TEST_CASE("A command list inside a map still queued for the GPU thread is found",
          "[video_core][nx2_queued_maps]") {
    Tegra::Nx2QueuedMaps maps;
    CHECK_FALSE(maps.Overlaps(Base, 0x40));
    maps.Add(Base, Size);
    CHECK(maps.Overlaps(Base, 0x40));
    CHECK(maps.Overlaps(Base + Size - 4, 4));
    maps.Remove(Base, Size);
    CHECK_FALSE(maps.Overlaps(Base, 0x40));
    CHECK_FALSE(maps.Overlaps(Base + Size - 4, 4));
}

TEST_CASE("Queued maps cover a list that only partly lies in them, not their neighbours",
          "[video_core][nx2_queued_maps]") {
    Tegra::Nx2QueuedMaps maps;
    maps.Add(Base, Size);
    CHECK(maps.Overlaps(Base - 4, 8));
    CHECK(maps.Overlaps(Base + Size - 4, 8));
    CHECK(maps.Overlaps(Base - 0x1000, Size + 0x2000));
    CHECK_FALSE(maps.Overlaps(Base - 8, 8));
    CHECK_FALSE(maps.Overlaps(Base + Size, 8));
}

TEST_CASE("Each applied map ends one queued map of its range and leaves the others",
          "[video_core][nx2_queued_maps]") {
    Tegra::Nx2QueuedMaps maps;
    maps.Add(Base, Size);
    maps.Add(Base, Size);
    maps.Add(Base + 2 * Size, Size);
    maps.Remove(Base, Size / 2);
    maps.Remove(Base + Size, Size);
    CHECK(maps.Overlaps(Base, 4));
    maps.Remove(Base, Size);
    CHECK(maps.Overlaps(Base, 4));
    CHECK(maps.Overlaps(Base + 2 * Size, 4));
    maps.Remove(Base, Size);
    CHECK_FALSE(maps.Overlaps(Base, 4));
    CHECK(maps.Overlaps(Base + 2 * Size, 4));
    maps.Remove(Base + 2 * Size, Size);
    CHECK_FALSE(maps.Overlaps(Base + 2 * Size, 4));
}
