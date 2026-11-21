// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_worker_task_manager.h"
#include "core/hle/kernel/kernel.h"
#include "tests/core/system_test_context.h"

namespace {

bool InThreadList(Kernel::KProcess& process, const Kernel::KThread* thread) {
    Kernel::KScopedLightLock lk{process.GetListLock()};
    for (const auto& listed : process.GetThreadList()) {
        if (&listed == thread) {
            return true;
        }
    }
    return false;
}

}

TEST_CASE("A thread destroyed by its termination waits for its process's locks",
          "[.][core][kernel][finish_termination]") {
    ScopedSystemTestContext environment;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    Kernel::KThread bootstrap{kernel};
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &bootstrap, nullptr).IsSuccess());
    kernel.SetCurrentEmuThread(&bootstrap);
    kernel.Initialize();
    SCOPE_EXIT {
        system.CoreTiming().ClearPendingEvents();
        kernel.Shutdown();
        kernel.SetCurrentEmuThread(nullptr);
        kernel.UnregisterKernelObject(&bootstrap);
    };

    auto* process = Kernel::KProcess::Create(kernel);
    REQUIRE(process
                ->Initialize(kernel, Kernel::Svc::CreateProcessParameter{},
                             kernel.GetSystemResourceLimit(), false)
                .IsSuccess());
    Kernel::KProcess::Register(kernel, process);
    SCOPE_EXIT {
        process->Close(kernel);
    };

    const u32 process_references = process->GetReferenceCount();

    Kernel::KScopedResourceReservation reservation(kernel, process,
                                                   Kernel::LimitableResource::ThreadCountMax);
    REQUIRE(reservation.Succeeded());
    auto* thread = Kernel::KThread::Create(kernel);
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, thread, process).IsSuccess());
    reservation.Commit();
    Kernel::KThread::Register(kernel, thread);

    SECTION("Termination signals a thread that still has a reference") {
        REQUIRE(thread->Open(kernel));
        SCOPE_EXIT {
            thread->Close(kernel);
        };
        thread->DoWorkerTaskImpl(kernel);
        Kernel::KScopedSchedulerLock lock{kernel};
        CHECK(thread->IsSignaled(kernel));
        CHECK(thread->GetReferenceCount() == 1);
    }

    SECTION("Last-reference destruction waits for the process list lock") {
        {
            Kernel::KScopedLightLock lock{process->GetListLock()};
            Kernel::KWorkerTaskManager::AddTask(
                kernel, Kernel::KWorkerTaskManager::WorkerType::Exit, thread);

            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            CHECK(process->GetReferenceCount() == process_references + 1);
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (process->GetReferenceCount() != process_references &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        REQUIRE(process->GetReferenceCount() == process_references);
        CHECK_FALSE(InThreadList(*process, thread));
    }
}
