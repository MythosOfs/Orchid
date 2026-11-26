---
type: architecture
title: Emulation lifecycle
description: How the desktop frontends load a guest application through Core::System and start or tear down an emulation session.
tags: [architecture, emulation, frontend]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T21:50:59.685Z
sources:
  - id: openwiki-source-664647317b8f3aa15647b1a5
    resource: repo://src/core/core.cpp
  - id: openwiki-source-4980c72de3964520648d4185
    resource: repo://src/yuzu_cmd/yuzu.cpp
  - id: openwiki-source-849abc74091a24580c4169e4
    resource: repo://src/yuzu/main_window.cpp
generated: { by: "codex", at: "2026-11-26T21:50:59.685Z" }
---

# Emulation lifecycle

## Desktop entry points

The Qt executable enters through [main.cpp](../../src/yuzu/main.cpp#L217), which constructs the Qt application and enters its event loop. Game launches converge in [MainWindow::BootGame](../../src/yuzu/main_window.cpp#L1946). The window checks firmware before calling `Core::System::Load` with the render window. After a successful load it creates and starts an `EmuThread`.

The command-line frontend uses SDL application callbacks. It supplies its own emulation window and application applet parameters to the same core loader, maps a failed load to SDL failure, and calls `System::Run` for a successful session. See the [CLI launch path](../../src/yuzu_cmd/yuzu.cpp#L432).

## Guest construction and teardown

[Core::System](../../src/core/core.cpp#L731) hands the load to its implementation. The implementation initializes the kernel, resolves the guest file through the virtual filesystem and asks the applet layer to create the guest process with a loader. It registers the process as the application, applies early title overrides, then initializes Host1x, GPU, audio and guest services. CPU manager initialization follows GPU setup, and the process is handed to the applet manager.

Loader failures and application subsystem setup failures both take a cleanup path before returning an error. During [shutdown](../../src/core/core.cpp#L510), the system notifies the GPU, requests stop, suspends emulation, closes guest services and shuts down the cores before releasing the GPU and audio objects.

Follow [Graphics and presentation](graphics.md) for GPU work, [Guest services and host I/O](guest-services.md) for IPC and host boundaries, and [Build and test Orchid](../development/build-and-test.md) for targets.
