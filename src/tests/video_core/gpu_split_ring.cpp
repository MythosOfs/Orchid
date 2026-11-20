// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Needs a per-thread CPU clock of another thread (not on macOS).
#if defined(_WIN32) || defined(__linux__)

#include <atomic>
#include <chrono>
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
std::chrono::nanoseconds ThreadCpuTime(std::jthread& thread) {
#ifdef _WIN32
    FILETIME creation, exit, kernel, user;
    GetThreadTimes(static_cast<HANDLE>(thread.native_handle()), &creation, &exit, &kernel, &user);
    const auto ticks = [](FILETIME time) {
        return (u64{time.dwHighDateTime} << 32) | time.dwLowDateTime;
    };
    return std::chrono::nanoseconds{(ticks(kernel) + ticks(user)) * 100};
#else
    clockid_t clock{};
    pthread_getcpuclockid(thread.native_handle(), &clock);
    timespec time{};
    clock_gettime(clock, &time);
    return std::chrono::seconds{time.tv_sec} + std::chrono::nanoseconds{time.tv_nsec};
#endif
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

TEST_CASE("The GPU front sleeps while it waits for ring space", "[video_core][gpu_split]") {
    // Three words per call: more than the 1 Mi-word ring holds.
    constexpr u32 Calls = 400'000;
    constexpr u32 RingCalls = (1u << 20) / 3;
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
    if (!WaitFor([&] { return started != -1; }, 5000ms)) {
        // Let a late front run through instead of blocking the join.
        release = true;
        release.notify_all();
        FAIL("the GPU split did not start within 5 s");
    }
    if (started == 0) {
        front.join();
        restore();
        SKIP("the GPU split does not start here (EDEN_NX2_SPLIT=0?)");
    }

    // The front is waiting for space once it filled the ring and sends nothing more.
    const bool waiting = WaitFor(
        [&] {
            const u32 now = sent.load();
            if (now < RingCalls - 16) {
                return false;
            }
            std::this_thread::sleep_for(20ms);
            return sent.load() == now;
        },
        5000ms);
    const auto cpu_before = ThreadCpuTime(front);
    const auto wall_before = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(300ms);
    const auto wait_cpu = ThreadCpuTime(front) - cpu_before;
    const auto wait_wall = std::chrono::steady_clock::now() - wall_before;
    const u32 sent_while_blocked = sent.load();
    release = true;
    release.notify_all();
    if (!WaitFor([&] { return finished.load(); }, 5000ms)) {
        // The front never woke: joining it would hang the test run.
        FAIL_CHECK("the front still waits 5 s after the back consumed the ring");
        std::abort();
    }
    front.join();
    restore();

    CHECK(waiting);
    CHECK(sent_while_blocked < Calls);
    CHECK(sent.load() == Calls);
    // A spinning front used about as much CPU as the wait lasted.
    CHECK(wait_cpu < wait_wall / 4);
}

#endif
