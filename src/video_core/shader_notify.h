// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <utility>

#include "common/scope_exit.h"

namespace VideoCore {
class ShaderNotify {
public:
    [[nodiscard]] int ShadersBuilding() noexcept;

    void MarkShaderComplete() noexcept {
        ++num_complete;
    }

    void MarkShaderBuilding() noexcept {
        ++num_building;
    }

    template <typename Work>
    static auto TrackBuild(ShaderNotify* notify, Work work) {
        if (notify) {
            notify->MarkShaderBuilding();
        }
        auto complete = detail::MakeScopeGuard([notify] {
            if (notify) {
                notify->MarkShaderComplete();
            }
        });
        return [complete = std::move(complete), work = std::move(work)]() mutable {
            const auto on_exit = std::move(complete);
            work();
        };
    }

private:
    std::atomic_int num_building{};
    std::atomic_int num_complete{};
    int report_base{};

    bool completed{};
    int num_when_completed{};
    std::chrono::steady_clock::time_point complete_time;
};
} // namespace VideoCore
