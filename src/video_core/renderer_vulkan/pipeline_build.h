// SPDX-FileCopyrightText: Copyright 2026 Orchid Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <utility>

#include "common/unique_function.h"

namespace Vulkan {

class PipelineBuild {
public:
    PipelineBuild(Common::UniqueFunction<void> work, bool can_build_on_demand)
        : task{std::move(work)}, build_on_demand{can_build_on_demand} {}

    void Run() {
        State expected = State::Pending;
        if (!state.compare_exchange_strong(expected, State::Running, std::memory_order_relaxed)) {
            return;
        }
        task();
        task = {};
        state.store(State::Complete, std::memory_order_release);
        state.notify_all();
    }

    void Wait() {
        if (build_on_demand) {
            Run();
        }
        auto observed = state.load(std::memory_order_acquire);
        while (observed != State::Complete) {
            state.wait(observed, std::memory_order_acquire);
            observed = state.load(std::memory_order_acquire);
        }
    }

    [[nodiscard]] bool IsComplete() const noexcept {
        return state.load(std::memory_order_acquire) == State::Complete;
    }

private:
    enum class State { Pending, Running, Complete };

    Common::UniqueFunction<void> task;
    bool build_on_demand;
    std::atomic<State> state{State::Pending};
};

}
