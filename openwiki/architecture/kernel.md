---
type: architecture
title: Kernel and HLE
description: Guest processes, scheduling and system calls.
tags: [orchid, architecture]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T21:54:46.723Z
sources:
  - id: openwiki-source-bdd0cf49717c7db456ccbad5
    resource: repo://src/core/hle/kernel/k_process.cpp
  - id: openwiki-source-49dd2864617d3143adaab456
    resource: repo://src/core/hle/kernel/k_scheduler.cpp
  - id: openwiki-source-0352ddb46f8dbe96eea70c8d
    resource: repo://src/core/hle/kernel/kernel.cpp
  - id: openwiki-source-cff596ec37af16837623a258
    resource: repo://src/core/hle/kernel/physical_core.cpp
  - id: openwiki-source-dc3bae3db11254c9464a99e9
    resource: repo://src/core/hle/kernel/svc.cpp
generated: { by: "codex", at: "2026-11-26T21:54:46.723Z" }
---

# Kernel and HLE

## Kernel objects and scheduling

KernelCore initializes per-core schedulers with their main and idle threads. The scheduler selects a candidate from the per-core priority queue while holding its scheduler lock. Its selection accounts for a process's pinned thread and whether a competing thread has kernel waiters. [`kernel.cpp`](../../src/core/hle/kernel/kernel.cpp#L239) [`k_scheduler.cpp`](../../src/core/hle/kernel/k_scheduler.cpp#L218)

KScheduler reloads the selected guest thread context during a switch. The CPU backend is selected by the owning KProcess, while the kernel's scheduling logic chooses which KThread can use the current core. Inspect the scheduler when a thread stays runnable without executing, and the physical core when execution halts unexpectedly. [`k_scheduler.cpp`](../../src/core/hle/kernel/k_scheduler.cpp#L492) [`k_process.cpp`](../../src/core/hle/kernel/k_process.cpp#L1317) [`physical_core.cpp`](../../src/core/hle/kernel/physical_core.cpp#L90)

## Supervisor call boundary

PhysicalCore sees the supervisor-call halt returned by the ARM interface. Svc::Call saves guest argument registers, uses the process bitness for normal dispatch, then reloads the return registers. To trace an SVC, follow its wrapper to the kernel object implementation and keep the guest ABI separate from the internal method signature. [`physical_core.cpp`](../../src/core/hle/kernel/physical_core.cpp#L115) [`svc.cpp`](../../src/core/hle/kernel/svc.cpp#L2513) [`svc.cpp`](../../src/core/hle/kernel/svc.cpp#L2679)

## Related pages

[Emulation lifecycle](../architecture/emulation.md) · [CPU execution and Dynarmic](../architecture/cpu-jit.md) · [Guest IPC and services](../architecture/guest-services.md) · [Guest memory and address translation](../architecture/memory.md)
