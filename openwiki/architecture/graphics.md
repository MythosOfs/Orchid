---
type: architecture
title: GPU command processing
description: Channel submission, GPU workers and guest engines.
tags: [orchid, architecture]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T21:59:01.672Z
sources:
  - id: openwiki-source-664647317b8f3aa15647b1a5
    resource: repo://src/core/core.cpp
  - id: openwiki-source-45fc218f22125c0ee8960b81
    resource: repo://src/core/hle/service/nvdrv/devices/nvhost_gpu.cpp
  - id: openwiki-source-85c246b3aa0886d6f912eaed
    resource: repo://src/video_core/dma_pusher.cpp
  - id: openwiki-source-9d3313274e6f3a0ec3b216bc
    resource: repo://src/video_core/gpu_thread.cpp
  - id: openwiki-source-11cb7cebc368f3f08ac29262
    resource: repo://src/video_core/gpu.cpp
  - id: openwiki-source-f6d634dcba9b9e21088e752e
    resource: repo://src/video_core/renderer_vulkan/renderer_vulkan.cpp
  - id: openwiki-source-708accf73fb6c8d2a57b9c2c
    resource: repo://src/video_core/renderer_vulkan/vk_pipeline_cache.cpp
  - id: openwiki-source-f88269ce3154bf687edfbf0b
    resource: repo://src/video_core/video_core.cpp
generated: { by: "codex", at: "2026-11-26T21:59:01.672Z" }
---

# GPU command processing

## GPU creation and command submission

During [application setup](emulation.md), [VideoCore::CreateGPU](../../src/video_core/video_core.cpp#L49) constructs the emulated Tegra GPU, acquires a frontend graphics context and binds a renderer selected from settings. The factory supports Vulkan and Null, plus OpenGL when compiled with `HAS_OPENGL`. A runtime error raised in renderer creation or binding resets the GPU instance, which lets application setup return a video-core error.

The [nvdrv GPU channel](../../src/core/hle/service/nvdrv/devices/nvhost_gpu.cpp#L461) forwards guest command lists to the Tegra GPU. The [GPU implementation](../../src/video_core/gpu.cpp#L256) hands the list and channel to the GPU thread manager. It passes the configured asynchronous-emulation mode through to submission.

## Vulkan translation and output

The [Vulkan pipeline cache](../../src/video_core/renderer_vulkan/vk_pipeline_cache.cpp#L1455) uses the shared shader recompiler to translate Maxwell programs, then emits SPIR-V for host pipelines. For a concrete compute path, follow translation and emission in [the compute pipeline builder](../../src/video_core/renderer_vulkan/vk_pipeline_cache.cpp#L1919).

[RendererVulkan::Composite](../../src/video_core/renderer_vulkan/renderer_vulkan.cpp#L291) processes applet capture before checking whether the window is shown. On the visible path it obtains a render frame, blits guest framebuffers using the frontend layout, flushes rendering with the frame's ready semaphore, and hands the frame to the presentation manager. Read this path when investigating output presentation. Start with the pipeline cache for shader compilation and with nvdrv and the GPU thread for command delivery.

## Queueing and synchronization

ThreadManager tags each queued command with a monotonically increasing fence. Synchronous emulation forces a producer wait until that fence is signaled, while asynchronous submission can return after enqueueing. A CPU readback requests blocking submission even in asynchronous mode. This fence belongs to the emulated GPU worker queue; the host Vulkan scheduler has its own submission machinery. [`gpu_thread.cpp`](../../src/video_core/gpu_thread.cpp#L221) [`gpu_thread.cpp`](../../src/video_core/gpu_thread.cpp#L311)

The worker's regular submit path applies pending guest writes through GPU cache invalidation before handing the list and channel to the GPU scheduler. The CPU readback branch also invalidates before asking the rasterizer to download contents. Read this ordering when tracking stale constants or unexpected guest-visible GPU memory. [`gpu_thread.cpp`](../../src/video_core/gpu_thread.cpp#L183)

## Decoder and engine seams

DmaPusher distinguishes control methods handled by the puller from methods forwarded to an engine bound to the selected subchannel. An unbound subchannel is reported and skipped. The engine can buffer methods until an execution method consumes that state and calls into the engine. Follow this boundary when the guest method is accepted but the expected engine work is missing. [`dma_pusher.cpp`](../../src/video_core/dma_pusher.cpp#L808) [`dma_pusher.cpp`](../../src/video_core/dma_pusher.cpp#L858)

## Related pages

[Guest memory and address translation](../architecture/memory.md) · [Vulkan rendering and presentation](../architecture/vulkan.md) · [Shader recompiler and pipeline cache](../architecture/shaders.md) · [Guest IPC and services](../architecture/guest-services.md)
