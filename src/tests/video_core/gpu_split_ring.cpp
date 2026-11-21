// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Needs a per-thread CPU clock of another thread (not on macOS).
#if defined(_WIN32) || defined(__linux__)

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "core/core.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2.h"

using namespace std::chrono_literals;

namespace {
// False when the clock of `thread` cannot be read.
bool ThreadCpuTime(std::jthread& thread, std::chrono::nanoseconds& cpu) {
#ifdef _WIN32
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(static_cast<HANDLE>(thread.native_handle()), &creation, &exit, &kernel,
                        &user)) {
        return false;
    }
    const auto ticks = [](FILETIME time) {
        return (u64{time.dwHighDateTime} << 32) | time.dwLowDateTime;
    };
    cpu = std::chrono::nanoseconds{(ticks(kernel) + ticks(user)) * 100};
#else
    clockid_t clock{};
    timespec time{};
    if (pthread_getcpuclockid(thread.native_handle(), &clock) != 0 ||
        clock_gettime(clock, &time) != 0) {
        return false;
    }
    cpu = std::chrono::seconds{time.tv_sec} + std::chrono::nanoseconds{time.tv_nsec};
#endif
    return true;
}

template <typename Predicate>
bool WaitFor(Predicate&& predicate, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
} // Anonymous namespace

// Hidden: it measures CPU time, so it runs alone in its own process (CTest, tests.yml).
TEST_CASE("The GPU front sleeps while it waits for ring space", "[.][gpu_split]") {
    // Three words per call: more than the 1 Mi-word ring holds. The back blocks in the first
    // call, so nothing is consumed and the ring is full after this many more calls.
    constexpr u32 Calls = 400'000;
    constexpr u32 FullCalls = ((1u << 20) - 3) / 3;
    Core::System system;
    auto& split = VideoCommon::GpuSplit::Get();
    const auto accuracy = Settings::values.current_gpu_accuracy;
    const auto backend = Settings::values.renderer_backend.GetValue();
    const bool ampere = Tegra::g_nx2_ampere.load();
    Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::Low;
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    Tegra::g_nx2_ampere = true;

    std::atomic<int> started{-1};
    std::atomic<bool> release{false};
    std::atomic<u32> sent{0};
    std::atomic<bool> finished{false};
    std::jthread front([&] {
        split.MaybeStart(system);
        started = split.Active() ? 1 : 0;
        if (started == 0) {
            finished = true;
            return;
        }
        // The back blocks in the first call until the test releases it.
        split.Defer([flag = &release] { flag->wait(false); });
        for (u32 i = 0; i < Calls; ++i) {
            split.Defer([] {});
            sent.store(i + 1, std::memory_order_relaxed);
        }
        split.Drain();
        split.Stop();
        finished = true;
    });
    const auto restore = [&] {
        Settings::values.current_gpu_accuracy = accuracy;
        Settings::values.renderer_backend.SetValue(backend);
        Tegra::g_nx2_ampere = ampere;
    };
    // Joining a front that never finishes would hang the test run.
    const auto wait_finished = [&](const char* failure) {
        release = true;
        release.notify_all();
        if (!WaitFor([&] { return finished.load(); }, 60000ms)) {
            std::fprintf(stderr, "FAILED: %s\n", failure);
            std::fflush(stderr);
            std::_Exit(EXIT_FAILURE);
        }
        front.join();
    };
    if (!WaitFor([&] { return started != -1; }, 60000ms)) {
        wait_finished("the GPU split did not start within 60 s and never finished");
        restore();
        FAIL("the GPU split did not start within 60 s");
    }
    if (started == 0) {
        front.join();
        restore();
        SKIP("the GPU split does not start here (EDEN_NX2_SPLIT=0?)");
    }

    // The front is waiting for space once it filled the ring and sends nothing more. It sleeps
    // until the back consumed the 3 words its next call needs.
    const bool full = WaitFor([&] { return sent.load() >= FullCalls; }, 60000ms);
    const bool sleeping = WaitFor([&] { return split.FrontSleepTarget() == 3; }, 60000ms);
    const u32 sent_when_full = sent.load();
    std::chrono::nanoseconds cpu_before{}, cpu_after{};
    const bool clock_before = ThreadCpuTime(front, cpu_before);
    const auto wall_before = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(300ms);
    const bool clock_after = ThreadCpuTime(front, cpu_after);
    const auto wait_wall = std::chrono::steady_clock::now() - wall_before;
    const u32 sent_while_blocked = sent.load();
    wait_finished("the front still waits 60 s after the back consumed the ring");
    restore();

    REQUIRE(full);
    CHECK(sleeping);
    CHECK(sent_when_full == FullCalls);
    CHECK(sent_while_blocked == FullCalls);
    REQUIRE(clock_before);
    REQUIRE(clock_after);
    CHECK(sent.load() == Calls);
    // A spinning front used about as much CPU as the wait lasted.
    CHECK(cpu_after - cpu_before < wait_wall / 4);
}

#endif
