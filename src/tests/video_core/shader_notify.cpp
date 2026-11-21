// SPDX-FileCopyrightText: Copyright 2026 Orchid Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "video_core/shader_notify.h"

TEST_CASE("Dropping a queued shader build retires its progress once", "[shader_notify]") {
    VideoCore::ShaderNotify notify;
    int built = 0;
    {
        auto pending = VideoCore::ShaderNotify::TrackBuild(&notify, [&] { ++built; });
        auto completed = VideoCore::ShaderNotify::TrackBuild(&notify, [&] { ++built; });
        CHECK(notify.ShadersBuilding() == 2);
        completed();
    }
    CHECK(built == 1);
    CHECK(notify.ShadersBuilding() == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds{2050});
    CHECK(notify.ShadersBuilding() == 0);
}
