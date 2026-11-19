// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nvnflinger/ui/fence.h"

namespace {
TEST_CASE("A guest fence count past the array yields only the stored fences",
          "[core][nvnflinger]") {
    auto fence = Service::android::Fence::NoFence();
    fence.num_fences = 0xFFFFFFFF;
    CHECK(fence.Active().size() == fence.fences.size());
    CHECK(fence.Active().data() == fence.fences.data());
    fence.num_fences = 2;
    CHECK(fence.Active().size() == 2);
    fence.num_fences = 0;
    CHECK(fence.Active().empty());
}
} // namespace
