---
type: architecture
title: Qt and SDL frontends
description: Desktop entry points, event loops and shared frontend boundaries.
tags: [orchid, architecture]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T21:54:46.723Z
sources:
  - id: openwiki-source-d7db04fa5ce3dc23bc95d4a9
    resource: repo://src/qt_common/render/emu_thread.cpp
  - id: openwiki-source-4980c72de3964520648d4185
    resource: repo://src/yuzu_cmd/yuzu.cpp
  - id: openwiki-source-849abc74091a24580c4169e4
    resource: repo://src/yuzu/main_window.cpp
  - id: openwiki-source-21e54ae5e663a65238880fbf
    resource: repo://src/yuzu/main.cpp
generated: { by: "codex", at: "2026-11-26T21:54:46.723Z" }
---

# Qt and SDL frontends

## Qt entry and emulation coordination

The desktop Qt entry point creates QApplication and runs the Qt event loop. MainWindow handles launch and starts the EmuThread after the shared core successfully loads the process. QtCommon supplies the control thread, which reports shader-cache progress, releases the graphics context, starts the GPU and signals CPU readiness. [`main.cpp`](../../src/yuzu/main.cpp#L327) [`main_window.cpp`](../../src/yuzu/main_window.cpp#L2196) [`emu_thread.cpp`](../../src/qt_common/render/emu_thread.cpp#L17)

EmuThread uses core Run and Pause in response to control state. Its condition-variable waits permit a stop request and, on the running path, a pending shader-cache reload. It detaches the debugger and shuts down the guest process when the thread exits. [`emu_thread.cpp`](../../src/qt_common/render/emu_thread.cpp#L48)

## SDL command-line entry

SDL_AppInit creates a backend-specific EmuWindow_SDL3, prepares the filesystem and calls System::Load. SDL_AppEvent forwards events to the window, while SDL_AppIterate applies pending window titles and decides whether the application should continue. This frontend shares core emulation but has its own window and event loop. [`yuzu.cpp`](../../src/yuzu_cmd/yuzu.cpp#L410) [`yuzu.cpp`](../../src/yuzu_cmd/yuzu.cpp#L576)

SDL_AppQuit guards guest pause and shutdown with IsPoweredOn. Quit can be called even when initialization exited early, such as for help output, so the guard protects uninitialized kernel state. The callback releases the frontend state and shuts down networking if it was initialized. [`yuzu.cpp`](../../src/yuzu_cmd/yuzu.cpp#L590)

## Related pages

[Emulation lifecycle](../architecture/emulation.md) · [Input and HID](../architecture/input.md) · [Configuration and per-title settings](../architecture/configuration.md)
