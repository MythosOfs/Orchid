---
type: overview
title: Orchid quickstart
description: Task routing across all module and workflow pages.
tags: [orchid, development]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T21:54:46.723Z
sources:
  - id: openwiki-source-23775c3de52f3ab95a13cb8b
    resource: repo://README.md
  - id: openwiki-source-6fbd0ecef1f5e21a2e4bf7c9
    resource: repo://src/CMakeLists.txt
  - id: openwiki-source-664647317b8f3aa15647b1a5
    resource: repo://src/core/core.cpp
  - id: openwiki-source-c26a829765cca89f8d97f801
    resource: repo://src/core/cpu_manager.cpp
  - id: openwiki-source-4980c72de3964520648d4185
    resource: repo://src/yuzu_cmd/yuzu.cpp
  - id: openwiki-source-849abc74091a24580c4169e4
    resource: repo://src/yuzu/main_window.cpp
generated: { by: "codex", at: "2026-11-26T21:54:46.723Z" }
---

# Orchid quickstart

Orchid is an experimental Switch 2 emulator. The [project README](../README.md#project-scope) describes its compatibility scope and priorities.

| Task | Start here |
| --- | --- |
| Trace launch, shutdown, or subsystem ownership | [Emulation lifecycle](architecture/emulation.md) |
| Follow CPU workers, JIT memory callbacks and halt reasons | [CPU execution and Dynarmic](architecture/cpu-jit.md) |
| Find thread scheduling or a supervisor call | [Kernel and HLE](architecture/kernel.md) |
| Find a named guest service and its IPC entry | [Guest IPC and services](architecture/guest-services.md) |
| Trace a CPU or GPU address, mapping, or readback | [Guest memory and address translation](architecture/memory.md) |
| Follow nvdrv command lists into a GPU engine | [GPU command processing](architecture/graphics.md) |
| Trace draws, dispatches and host presentation | [Vulkan rendering and presentation](architecture/vulkan.md) |
| Investigate shader translation, compilation or cache reuse | [Shader recompiler and pipeline cache](architecture/shaders.md) |
| Trace audio sessions and host devices | [Audio and host sinks](architecture/audio.md) |
| Follow a control mapping through guest HID sampling | [Input and HID](architecture/input.md) |
| Find content, format selection or application loading | [Filesystem and application loading](architecture/filesystem.md) |
| Trace a Qt or SDL frontend event | [Qt and SDL frontends](architecture/frontends.md) |
| Follow global, per-game or runtime settings | [Configuration and per-title settings](architecture/configuration.md) |
| Select the test harness, route or offline debugging tool | [Orchid harness and development tools](development/harness-and-tools.md) |
| Build locally, inspect PGO or prepare a release | [Build, PGO and release](development/build-and-test.md) |

The [source build graph](../src/CMakeLists.txt#L273) places shared emulation subsystems under `src/` and enables the Qt and command-line frontends through separate options. Each frontend supplies a window to the shared core load path. Follow the emulation page for those callers before changing a behavior that both frontends use.

## Refresh this wiki

From the repository root, run `openwiki --update` with the standalone OpenWiki CLI configured locally. In a coding agent with the OpenWiki integration installed, ask it to update this repository's OpenWiki to use the resumable page queue. The [repository brief](INSTRUCTIONS.md) defines content scope for future updates.

## Start from an entry point

A desktop boot crosses the frontend, Process/Loader and Core::System boundaries before the CPU execution loop owns the running thread. Qt hands session control to an EmuThread after loading, while the core registers the initialized process with the applet manager. Use the table to narrow the next source lookup by the boundary where the behavior first diverges. [`main_window.cpp`](../src/yuzu/main_window.cpp#L2196) [`core.cpp`](../src/core/core.cpp#L404) [`core.cpp`](../src/core/core.cpp#L446) [`cpu_manager.cpp`](../src/core/cpu_manager.cpp#L73)

## Related pages

[Emulation lifecycle](architecture/emulation.md) · [CPU execution and Dynarmic](architecture/cpu-jit.md) · [Kernel and HLE](architecture/kernel.md) · [Guest IPC and services](architecture/guest-services.md) · [Guest memory and address translation](architecture/memory.md) · [GPU command processing](architecture/graphics.md) · [Vulkan rendering and presentation](architecture/vulkan.md) · [Shader recompiler and pipeline cache](architecture/shaders.md) · [Audio and host sinks](architecture/audio.md) · [Input and HID](architecture/input.md) · [Filesystem and application loading](architecture/filesystem.md) · [Qt and SDL frontends](architecture/frontends.md) · [Configuration and per-title settings](architecture/configuration.md) · [Orchid harness and development tools](development/harness-and-tools.md) · [Build, PGO and release](development/build-and-test.md)
