// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/thread_worker.h"

namespace Kernel {

class KernelCore;
class KWorkerTask;

class KWorkerTaskManager final {
public:
    enum class WorkerType : u32 {
        ExitThread,
        ExitProcess,
    };

    KWorkerTaskManager();

    static void AddTask(KernelCore& kernel, WorkerType type, KWorkerTask* task);
    void WaitForTasks();

private:
    Common::ThreadWorker m_thread_exit_worker{1, "KThreadExit"};
    Common::ThreadWorker m_process_exit_worker{1, "KProcessExit"};
};

} // namespace Kernel
