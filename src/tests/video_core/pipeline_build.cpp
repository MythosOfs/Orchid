// SPDX-FileCopyrightText: Copyright 2026 Orchid Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <memory>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/pipeline_build.h"

using namespace std::chrono_literals;

TEST_CASE("A pipeline needed by the scheduler bypasses queued builds", "[pipeline_build]") {
    std::latch worker_building{1};
    std::latch release_worker{1};
    Vulkan::PipelineBuild unrelated{[&] {
                                       worker_building.count_down();
                                       release_worker.wait();
                                   },
                                   true};
    std::atomic_int builds{};
    auto pipeline = std::make_shared<Vulkan::PipelineBuild>([&] { ++builds; }, true);
    std::jthread worker{[queued = pipeline, &unrelated] {
        unrelated.Run();
        queued->Run();
    }};
    worker_building.wait();

    auto draw = std::async(std::launch::async, [pipeline] { pipeline->Wait(); });
    const auto before_queue_drained = draw.wait_for(1s);
    const auto built_before_queue_drained = pipeline->IsComplete();
    pipeline.reset();
    release_worker.count_down();
    worker.join();
    draw.get();

    CHECK(before_queue_drained == std::future_status::ready);
    CHECK(built_before_queue_drained);
    CHECK(unrelated.IsComplete());
    CHECK(builds == 1);
}

TEST_CASE("Concurrent pipeline waiters observe a single worker build", "[pipeline_build]") {
    std::latch building{1};
    std::latch finish_build{1};
    std::atomic_int builds{};
    int result = 0;
    Vulkan::PipelineBuild pipeline{[&] {
                                       if (++builds != 1) {
                                           return;
                                       }
                                       building.count_down();
                                       finish_build.wait();
                                       result = 42;
                                   },
                                   true};
    std::jthread worker{[&] { pipeline.Run(); }};
    building.wait();
    auto draw = std::async(std::launch::async, [&] {
        pipeline.Wait();
        return result;
    });
    const bool complete_while_building = pipeline.IsComplete();
    finish_build.count_down();
    CHECK(draw.get() == 42);
    worker.join();
    CHECK_FALSE(complete_while_building);
    CHECK(pipeline.IsComplete());
    CHECK(builds == 1);
}

TEST_CASE("Driver workarounds keep a pending pipeline on its worker", "[pipeline_build]") {
    Vulkan::PipelineBuild pipeline{[] {}, false};
    auto draw = std::async(std::launch::async, [&] { pipeline.Wait(); });
    const auto before_worker = draw.wait_for(100ms);
    pipeline.Run();
    draw.get();
    CHECK(before_worker == std::future_status::timeout);
}
