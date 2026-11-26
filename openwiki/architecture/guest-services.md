---
type: architecture
title: Guest IPC and services
description: Service dispatch and major service families.
tags: [orchid, architecture]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T21:59:01.672Z
sources:
  - id: openwiki-source-8c724a03f8ca5cd35b4dbb79
    resource: repo://src/audio_core/audio_core.cpp
  - id: openwiki-source-b2ed0e05d4631111b622209d
    resource: repo://src/core/hle/service/am/am.cpp
  - id: openwiki-source-539234cf166fd6ccb1229f00
    resource: repo://src/core/hle/service/audio/audio.cpp
  - id: openwiki-source-91092240489f2462d708d291
    resource: repo://src/core/hle/service/filesystem/filesystem.cpp
  - id: openwiki-source-448ae7bc10a1ca6737776b18
    resource: repo://src/core/hle/service/hid/applet_resource.cpp
  - id: openwiki-source-41826080fc28f45897c3c86c
    resource: repo://src/core/hle/service/hid/hid_server.cpp
  - id: openwiki-source-45fc218f22125c0ee8960b81
    resource: repo://src/core/hle/service/nvdrv/devices/nvhost_gpu.cpp
  - id: openwiki-source-b1cd303bb1e673007a7a715f
    resource: repo://src/core/hle/service/nvdrv/nvdrv.cpp
  - id: openwiki-source-90cba46fd437db4a0692b497
    resource: repo://src/core/hle/service/server_manager.cpp
  - id: openwiki-source-3c34e5671645930f1d51e475
    resource: repo://src/core/hle/service/service.cpp
  - id: openwiki-source-85edb7d5ad8c2cc5d7eaf5dc
    resource: repo://src/core/hle/service/services.cpp
  - id: openwiki-source-093f53fcf3b8777ad21b6e9a
    resource: repo://src/core/hle/service/vi/vi.cpp
  - id: openwiki-source-93c31a1bef6c97e6cdc1aba0
    resource: repo://src/input_common/main.cpp
generated: { by: "codex", at: "2026-11-26T21:59:01.672Z" }
---

# Guest IPC and services

## Service dispatch

[Application setup](emulation.md) constructs the service manager and starts the service collection. A service registers a name and session limit through [ServerManager::RegisterNamedService](../../src/core/hle/service/server_manager.cpp#L175). The server manager obtains the server port from the system service manager and tracks it for incoming sessions.

[ServiceFrameworkBase::HandleSyncRequest](../../src/core/hle/service/service.cpp#L282) handles close requests, forwards control requests to the service manager, and dispatches normal and TIPC requests to their respective handlers. To trace a guest call, start from its service registration, find the command in the service's handler table, then follow the handler into the subsystem.

## Audio

The [audio service process](../../src/core/hle/service/audio/audio.cpp#L187) registers separate names for output, input and renderer managers, along with Switch 2 endpoints such as `aud:u`. Host device selection lives in [AudioCore](../../src/audio_core/audio_core.cpp#L14). It creates input and output sinks from the configured backend and device IDs, then constructs its audio DSP using the output sink.

## Input

The [HID service](../../src/core/hle/service/hid/hid_server.cpp#L296) asks the HID resource manager to create a resource for the applet's user ID and returns an applet resource interface. That [interface](../../src/core/hle/service/hid/applet_resource.cpp#L15) exposes the shared-memory handle through the resource manager and releases the resource ID on destruction.

Host input engines belong on the input side of this boundary. [InputSubsystem](../../src/input_common/main.cpp#L61) registers input and output factories for engines such as keyboard, mouse, touch and SDL. For GPU device IPC, follow [the nvdrv submission path](graphics.md).

## Service family routing

Services starts the audio, filesystem, nvservices and socket entry points as host core processes, and starts VI with a stop token. Other named entries include SM, AM, settings and HID. Follow the process table to the relevant LoopProcess and then the registered guest port, since the process label and public service name can differ. [`services.cpp`](../../src/core/hle/service/services.cpp#L88)

The AM service starts a WindowSystem with its button poller and event observer, then exposes appletAE and appletOE through separate proxy implementations. VI registers its manager, system and user ports over one shared Container and calls its termination handler when the stop token fires. [`am.cpp`](../../src/core/hle/service/am/am.cpp#L17) [`vi.cpp`](../../src/core/hle/service/vi/vi.cpp#L17)

The filesystem service registers distinct loader, program registry and filesystem proxy names. NVDRV registers multiple driver-facing names, including an applet-specific factory. To trace storage use fsp-srv and the filesystem module. For a GPU command list follow the nvdrv device handler into the GPU queue. [`filesystem.cpp`](../../src/core/hle/service/filesystem/filesystem.cpp#L826) [`nvdrv.cpp`](../../src/core/hle/service/nvdrv/nvdrv.cpp#L104) [`nvhost_gpu.cpp`](../../src/core/hle/service/nvdrv/devices/nvhost_gpu.cpp#L450)

## Related pages

[Kernel and HLE](../architecture/kernel.md) · [Audio and host sinks](../architecture/audio.md) · [Input and HID](../architecture/input.md) · [Filesystem and application loading](../architecture/filesystem.md) · [GPU command processing](../architecture/graphics.md)
