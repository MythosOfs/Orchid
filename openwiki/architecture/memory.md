---
type: architecture
title: Guest memory and address translation
description: Process page tables, CPU memory and GPU address spaces.
tags: [orchid, architecture]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T21:54:46.723Z
sources:
  - id: openwiki-source-2ea1eb6a8d056a595b6dd761
    resource: repo://src/core/arm/dynarmic/arm_dynarmic_64.cpp
  - id: openwiki-source-0b1b7fd3a6acb251ed9862c3
    resource: repo://src/core/memory.cpp
  - id: openwiki-source-0676e1ad9ca8bae26c650233
    resource: repo://src/video_core/memory_manager.cpp
generated: { by: "codex", at: "2026-11-26T21:54:46.723Z" }
---

# Guest memory and address translation

## CPU address space

Core::Memory installs the owning KProcess page table and uses DeviceMemory as backing storage. The application can expose the fastmem arena when fastmem is enabled; other cases leave it unset. The Dynarmic integration takes the process page table and configures its own execution-side access paths from that memory state. [`memory.cpp`](../../src/core/memory.cpp#L64) [`arm_dynarmic_64.cpp`](../../src/core/arm/dynarmic/arm_dynarmic_64.cpp#L230) [`arm_dynarmic_64.cpp`](../../src/core/arm/dynarmic/arm_dynarmic_64.cpp#L436)

Block accesses walk the process mapping. Reads of unmapped portions fill the destination with zeroes. Rasterizer-tracked pages can trigger a download before a safe read, or a renderer write notification before a safe write. The unsafe variants skip those renderer callbacks, so a mapped address alone does not establish coherency. [`memory.cpp`](../../src/core/memory.cpp#L318) [`memory.cpp`](../../src/core/memory.cpp#L352) [`memory.cpp`](../../src/core/memory.cpp#L1254)

## GPU address space

Tegra::MemoryManager maps GPU virtual addresses to device addresses using small or large pages. Sparse reservations have a separate entry type. Despite its name, GpuToCpuAddress returns an optional DAddr, rejects unmapped or out-of-range input and prefers mapped big pages before testing small pages. [`memory_manager.cpp`](../../src/video_core/memory_manager.cpp#L274)

GPU unmap walks the submapped ranges and notifies the rasterizer before clearing entries from both page tables. This ordering lets the renderer release its tracking while the mapped ranges are still known. Start with the GPU memory manager for address translation and the GPU command page for coherency across queued work. [`memory_manager.cpp`](../../src/video_core/memory_manager.cpp#L286)

## Related pages

[CPU execution and Dynarmic](../architecture/cpu-jit.md) · [Kernel and HLE](../architecture/kernel.md) · [GPU command processing](../architecture/graphics.md)
