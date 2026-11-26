---
type: architecture
title: Input and HID
description: Host devices, controller mappings and guest HID.
tags: [orchid, architecture]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T21:54:46.723Z
sources:
  - id: openwiki-source-448ae7bc10a1ca6737776b18
    resource: repo://src/core/hle/service/hid/applet_resource.cpp
  - id: openwiki-source-41826080fc28f45897c3c86c
    resource: repo://src/core/hle/service/hid/hid_server.cpp
  - id: openwiki-source-9e70a4c3937c03e0fa14e658
    resource: repo://src/hid_core/frontend/emulated_controller.cpp
  - id: openwiki-source-decaf890258c20d7f206ecfb
    resource: repo://src/hid_core/resource_manager.cpp
  - id: openwiki-source-6b0676a0bb582eb89ea1a135
    resource: repo://src/hid_core/resources/npad/npad.cpp
  - id: openwiki-source-e8f71d4733c2087933dacd40
    resource: repo://src/input_common/drivers/sdl_driver.cpp
  - id: openwiki-source-93c31a1bef6c97e6cdc1aba0
    resource: repo://src/input_common/main.cpp
generated: { by: "codex", at: "2026-11-26T21:54:46.723Z" }
---

# Input and HID

## Host events to logical controls

InputSubsystem registers host engines with common input and output factories. The SDL driver routes joystick button events to its engine state using the device identifier. Debug a missing host event in the driver and a recognized event mapped to the wrong guest control in the controller configuration. [`main.cpp`](../../src/input_common/main.cpp#L61) [`sdl_driver.cpp`](../../src/input_common/drivers/sdl_driver.cpp#L620)

EmulatedController reloads its devices and installs per-binding callbacks. Button and stick closures capture the mapping index and source UUID, and ForceUpdate applies current state after callback installation. This is the seam between a settings change and the emulated controller state. [`emulated_controller.cpp`](../../src/hid_core/frontend/emulated_controller.cpp#L344)

## Guest-visible sampling

HID ResourceManager schedules separate update callbacks for NPad, other controllers, mouse and keyboard, and motion. NPad sampling publishes the result to guest state rings selected by controller style, such as the full-key and handheld rings. A host event reaching a binding and a guest program seeing a sampled entry are therefore separate checkpoints. [`resource_manager.cpp`](../../src/hid_core/resource_manager.cpp#L60) [`resource_manager.cpp`](../../src/hid_core/resource_manager.cpp#L483) [`npad.cpp`](../../src/hid_core/resources/npad/npad.cpp#L513) [`npad.cpp`](../../src/hid_core/resources/npad/npad.cpp#L568)

IHidServer creates an IAppletResource keyed by the applet user ID. The resource exposes a copy of the shared-memory handle and frees the resource ID on destruction. Start here for applet-specific handle or allocation problems, then trace ResourceManager for the contents of the shared region. [`hid_server.cpp`](../../src/core/hle/service/hid/hid_server.cpp#L296) [`applet_resource.cpp`](../../src/core/hle/service/hid/applet_resource.cpp#L15)

## Related pages

[Guest IPC and services](../architecture/guest-services.md) · [Qt and SDL frontends](../architecture/frontends.md) · [Configuration and per-title settings](../architecture/configuration.md)
