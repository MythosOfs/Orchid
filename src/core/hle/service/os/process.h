// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <utility>
#include <string>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Loader {
class AppLoader;
enum class ResultStatus : u16;
} // namespace Loader

namespace Kernel {
class KProcess;
}

namespace Service {

class Process {
public:
    inline explicit Process(Core::System& system) noexcept : m_system(system) {}
    inline ~Process() { this->Finalize(); }

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process& operator=(Process&&) = delete;
    inline Process(Process&& other) noexcept
        : m_system(other.m_system), m_process(std::exchange(other.m_process, nullptr)),
          m_main_thread_stack_size(std::exchange(other.m_main_thread_stack_size, 0)),
          m_main_thread_priority(std::exchange(other.m_main_thread_priority, 0)),
          m_process_started(std::exchange(other.m_process_started, false)),
          m_display_version(std::move(other.m_display_version)),
          m_loose_exefs(other.m_loose_exefs) {}

    bool Initialize(Loader::AppLoader& loader, Loader::ResultStatus& out_load_result);
    void Finalize();
    void RetireRegistrations();
    const std::string& GetDisplayVersion() const { return m_display_version; }
    bool PrefersInstalledDisplayVersion() const { return m_loose_exefs; }

    bool Run();
    bool HasStarted() const { return m_process_started; }
    void Terminate();
    void Suspend(bool suspended);
    void ResetSignal();

    bool IsInitialized() const {
        return m_process != nullptr;
    }

    bool IsRunning() const;
    bool IsTerminated() const;

    u64 GetProcessId() const;
    u64 GetProgramId() const;

    Kernel::KProcess* GetHandle() const {
        return m_process;
    }

private:
    Core::System& m_system;
    Kernel::KProcess* m_process{};
    u64 m_main_thread_stack_size{};
    s32 m_main_thread_priority{};
    bool m_process_started{};
    std::string m_display_version;
    bool m_loose_exefs{};
};

} // namespace Service
