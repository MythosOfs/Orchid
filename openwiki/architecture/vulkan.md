---
type: architecture
title: Vulkan rendering and presentation
description: Rasterizer, renderer, scheduling and presentation.
tags: [orchid, architecture]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T21:54:46.723Z
sources:
  - id: openwiki-source-f6d634dcba9b9e21088e752e
    resource: repo://src/video_core/renderer_vulkan/renderer_vulkan.cpp
  - id: openwiki-source-0c6471f5b826bb7736a53e8c
    resource: repo://src/video_core/renderer_vulkan/vk_present_manager.cpp
  - id: openwiki-source-622f5a9192292ace6519518b
    resource: repo://src/video_core/renderer_vulkan/vk_rasterizer.cpp
  - id: openwiki-source-244251dd7d41d295086259a1
    resource: repo://src/video_core/renderer_vulkan/vk_scheduler.cpp
generated: { by: "codex", at: "2026-11-26T21:54:46.723Z" }
---

# Vulkan rendering and presentation

## Drawing and composition

RasterizerVulkan obtains graphics state through PipelineCache and configures the selected pipeline before recording the draw. DispatchCompute separately selects a compute pipeline and configures it with guest memory, the scheduler and resource caches. A wrong guest register or binding should be traced through the rasterizer before the window's presentation path. [`vk_rasterizer.cpp`](../../src/video_core/renderer_vulkan/vk_rasterizer.cpp#L336) [`vk_rasterizer.cpp`](../../src/video_core/renderer_vulkan/vk_rasterizer.cpp#L493) [`vk_rasterizer.cpp`](../../src/video_core/renderer_vulkan/vk_rasterizer.cpp#L1219)

RendererVulkan::Composite handles applet capture before testing whether the window is shown. On the visible path it prepares a frame and draws the guest framebuffers into it using the frontend layout, then schedules rendering against its ready semaphore and passes the frame to PresentManager. [`renderer_vulkan.cpp`](../../src/video_core/renderer_vulkan/renderer_vulkan.cpp#L291) [`renderer_vulkan.cpp`](../../src/video_core/renderer_vulkan/renderer_vulkan.cpp#L333)

## Worker and swapchain ordering

Vulkan Scheduler::Flush submits execution and allocates a new context without waiting for the submitted work to complete. Finish waits on the pre-submit tick before allocating a new context. WaitWorker drains scheduler-side work, which is a different boundary from waiting for the device tick. [`vk_scheduler.cpp`](../../src/video_core/renderer_vulkan/vk_scheduler.cpp#L62)

PresentManager can queue the frame from a recorded scheduler callback when the present thread is enabled. Otherwise it waits for the scheduler worker, copies to the swapchain on the calling path, and returns the frame to the free queue. A blocked presentation should be investigated in this path after confirming composition reached Present. [`vk_present_manager.cpp`](../../src/video_core/renderer_vulkan/vk_present_manager.cpp#L203)

## Related pages

[GPU command processing](../architecture/graphics.md) · [Shader recompiler and pipeline cache](../architecture/shaders.md) · [Qt and SDL frontends](../architecture/frontends.md)
