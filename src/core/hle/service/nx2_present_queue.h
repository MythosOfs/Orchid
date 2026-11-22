// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "common/common_types.h"
#include "core/core_timing.h"
#include "video_core/host1x/syncpoint_manager.h"

namespace Service {

class Nx2PresentReleaseQueue : public std::enable_shared_from_this<Nx2PresentReleaseQueue> {
public:
    using Time = std::chrono::nanoseconds;
    static constexpr size_t Capacity = 15;

    struct Release {
        u32 syncpoint;
        u32 value;
        std::function<void()> on_composited;
    };

    Nx2PresentReleaseQueue(Core::Timing::CoreTiming& timing_,
                           Tegra::Host1x::SyncpointManager& syncpoints_, u32 syncpoint_)
        : timing{timing_}, syncpoints{syncpoints_}, syncpoint{syncpoint_},
          next_value{syncpoints.GetGuestSyncpointValue(syncpoint)} {
        timer = Core::Timing::CreateEvent("nx2:vi2 buffer release", [this](s64, Time) {
            SignalReady();
            return std::nullopt;
        });
    }

    ~Nx2PresentReleaseQueue() {
        Stop();
    }

    std::optional<Release> Queue(Time delay) {
        std::scoped_lock lock{mutex};
        if (stopped || pending.size() == Capacity) {
            return std::nullopt;
        }
        const u32 value = ++next_value;
        pending.push_back({timing.GetGlobalTimeNs() + std::max(delay, Time{}), value, false});
        return Release{syncpoint, value, [weak = weak_from_this(), value] {
            if (const auto queue = weak.lock()) {
                queue->Composited(value);
            }
        }};
    }

    Time RunAhead() {
        std::scoped_lock lock{mutex};
        return pending.empty() ? Time{} :
               std::max(pending.back().deadline - timing.GetGlobalTimeNs(), Time{});
    }

    void Stop() {
        {
            std::scoped_lock lock{mutex};
            if (stopped) {
                return;
            }
            stopped = true;
        }
        timing.UnscheduleEvent(timer);
        for (const auto& entry : pending) {
            Signal(entry.value);
        }
        pending.clear();
    }

private:
    struct Entry {
        Time deadline;
        u32 value;
        bool composited;
    };

    void Composited(u32 value) {
        std::scoped_lock lock{mutex};
        if (stopped) {
            return;
        }
        const auto found = std::ranges::find(pending, value, &Entry::value);
        if (found != pending.end()) {
            found->composited = true;
            ArmNext();
        }
    }

    void ArmNext() {
        if (!armed && !pending.empty() && pending.front().composited) {
            armed = true;
            timing.ScheduleEvent(pending.front().deadline, timer, true);
        }
    }

    void SignalReady() {
        size_t count{};
        std::array<u32, Capacity> ready{};
        {
            std::scoped_lock lock{mutex};
            armed = false;
            if (stopped) {
                return;
            }
            const auto now = timing.GetGlobalTimeNs();
            while (!pending.empty() && pending.front().composited &&
                   pending.front().deadline <= now) {
                ready[count++] = pending.front().value;
                pending.pop_front();
            }
            ArmNext();
        }
        for (size_t index = 0; index < count; ++index) {
            Signal(ready[index]);
        }
    }

    void Signal(u32 value) {
        if (static_cast<s32>(value - syncpoints.GetGuestSyncpointValue(syncpoint)) > 0) {
            syncpoints.IncrementGuest(syncpoint);
        }
    }

    Core::Timing::CoreTiming& timing;
    Tegra::Host1x::SyncpointManager& syncpoints;
    const u32 syncpoint;
    u32 next_value;
    std::mutex mutex;
    bool stopped{};
    bool armed{};
    std::deque<Entry> pending;
    std::shared_ptr<Core::Timing::EventType> timer;
};

}
