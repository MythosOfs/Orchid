// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_worker_task.h"
#include "core/hle/kernel/k_worker_task_manager.h"
#include "core/hle/kernel/kernel.h"

namespace Kernel {

KWorkerTask::KWorkerTask(KernelCore& kernel) : KSynchronizationObject{kernel} {}

void KWorkerTask::DoWorkerTask(KernelCore& kernel) {
    if (auto* const thread = this->DynamicCast<KThread*>(); thread != nullptr) {
        return thread->DoWorkerTaskImpl(kernel);
    } else {
        auto* const process = this->DynamicCast<KProcess*>();
        ASSERT(process != nullptr);

        return process->DoWorkerTaskImpl(kernel);
    }
}

KWorkerTaskManager::KWorkerTaskManager() = default;

void KWorkerTaskManager::WaitForTasks() {
    m_process_exit_worker.WaitForRequests();
    m_thread_exit_worker.WaitForRequests();
}

void KWorkerTaskManager::AddTask(KernelCore& kernel, WorkerType type, KWorkerTask* task) {
    auto& manager = kernel.WorkerTaskManager();
    // Process exit can wait for threads whose exit work must run independently.
    auto& worker = type == WorkerType::ExitThread ? manager.m_thread_exit_worker
                                                 : manager.m_process_exit_worker;
    KScopedSchedulerLock sl(kernel);
    worker.QueueWork([&kernel, task]() {
        // Do the task.
        task->DoWorkerTask(kernel);
    });
}

} // namespace Kernel
