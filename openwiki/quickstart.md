---
type: overview
title: Orchid quickstart
description: A task-based entry point for following game launch, guest services, graphics, and local development.
tags: [overview, emulation]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T21:50:59.685Z
sources:
  - id: openwiki-source-23775c3de52f3ab95a13cb8b
    resource: repo://README.md
  - id: openwiki-source-6fbd0ecef1f5e21a2e4bf7c9
    resource: repo://src/CMakeLists.txt
  - id: openwiki-source-4980c72de3964520648d4185
    resource: repo://src/yuzu_cmd/yuzu.cpp
  - id: openwiki-source-849abc74091a24580c4169e4
    resource: repo://src/yuzu/main_window.cpp
generated: { by: "codex", at: "2026-11-26T21:50:59.685Z" }
---

# Orchid quickstart

Orchid is an experimental Switch 2 emulator. The [project README](../README.md#project-scope) describes its compatibility scope and priorities.

| Task | Start here |
| --- | --- |
| Trace a game launch, load error, or shutdown | [Emulation lifecycle](architecture/emulation.md) |
| Follow guest GPU commands, shader translation, or frame presentation | [Graphics and presentation](architecture/graphics.md) |
| Find a guest IPC handler or investigate host audio and input boundaries | [Guest services and host I/O](architecture/guest-services.md) |
| Build the Linux desktop executable or run focused tests | [Build and test Orchid](development/build-and-test.md) |

The [source build graph](../src/CMakeLists.txt#L273) places shared emulation subsystems under `src/` and enables the Qt and command-line frontends through separate options. Each frontend supplies a window to the shared core load path. Follow the emulation page for those callers before changing a behavior that both frontends use.

## Refresh this wiki

From the repository root, run `openwiki --update` with the standalone OpenWiki CLI configured locally. In a coding agent with the OpenWiki integration installed, ask it to update this repository's OpenWiki to use the resumable page queue. The [repository brief](INSTRUCTIONS.md) defines content scope for future updates.
