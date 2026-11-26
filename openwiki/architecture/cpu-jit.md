---
type: architecture
title: CPU execution and Dynarmic
description: CPU workers, physical cores and JIT backends.
tags: [orchid, architecture]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T21:54:46.723Z
sources:
  - id: openwiki-source-2ea1eb6a8d056a595b6dd761
    resource: repo://src/core/arm/dynarmic/arm_dynarmic_64.cpp
  - id: openwiki-source-c26a829765cca89f8d97f801
    resource: repo://src/core/cpu_manager.cpp
  - id: openwiki-source-bdd0cf49717c7db456ccbad5
    resource: repo://src/core/hle/kernel/k_process.cpp
  - id: openwiki-source-cff596ec37af16837623a258
    resource: repo://src/core/hle/kernel/physical_core.cpp
generated: { by: "codex", at: "2026-11-26T21:54:46.723Z" }
---

# CPU execution and Dynarmic

## From host workers to guest instructions

CpuManager starts one host worker in single-core mode or one per emulated CPU core in multicore mode. The guest run loop delegates to the current PhysicalCore, then handles interrupts. The single-core loop explicitly advances core timing and performs single-core preemption between execution slices. [`cpu_manager.cpp`](../../src/core/cpu_manager.cpp#L25) [`cpu_manager.cpp`](../../src/core/cpu_manager.cpp#L73) [`cpu_manager.cpp`](../../src/core/cpu_manager.cpp#L106)

KProcess owns the ARM interfaces used on its cores. It selects AArch64 or AArch32 Dynarmic implementations based on process bitness, and a build with native code execution support may select ArmNce for an application when enabled. Both Dynarmic implementations receive the process and its exclusive monitor. [`k_process.cpp`](../../src/core/hle/kernel/k_process.cpp#L1317)

## JIT and kernel boundary

ArmDynarmic64 configures a JIT from the process page table and address width, then translates the JIT's halt reason before handing it back to the kernel. Memory callbacks provide a route through emulated memory for accesses that use the callback path; inspect MakeJit for direct mapping and configuration. [`arm_dynarmic_64.cpp`](../../src/core/arm/dynarmic/arm_dynarmic_64.cpp#L31) [`arm_dynarmic_64.cpp`](../../src/core/arm/dynarmic/arm_dynarmic_64.cpp#L230) [`arm_dynarmic_64.cpp`](../../src/core/arm/dynarmic/arm_dynarmic_64.cpp#L397) [`arm_dynarmic_64.cpp`](../../src/core/arm/dynarmic/arm_dynarmic_64.cpp#L436)

PhysicalCore runs or single-steps the selected interface and classifies the halt after leaving the CPU context. This is the entry point for guest supervisor calls, breakpoints and aborts that leave JIT execution. KThread holds the context restored or saved at the execution boundary. [`physical_core.cpp`](../../src/core/hle/kernel/physical_core.cpp#L29) [`physical_core.cpp`](../../src/core/hle/kernel/physical_core.cpp#L90)

## Related pages

[Emulation lifecycle](../architecture/emulation.md) · [Kernel and HLE](../architecture/kernel.md) · [Guest memory and address translation](../architecture/memory.md)
