---
type: architecture
title: Configuration and per-title settings
description: Defaults, persisted settings and scoped overrides.
tags: [orchid, architecture]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T21:54:46.723Z
sources:
  - id: openwiki-source-d41bdd24cc5696ad991edc6b
    resource: repo://src/common/settings_setting.h
  - id: openwiki-source-664647317b8f3aa15647b1a5
    resource: repo://src/core/core.cpp
  - id: openwiki-source-89266edeb65c562c35b2840c
    resource: repo://src/core/game_settings.cpp
  - id: openwiki-source-1f9a47102bd21d586bc85702
    resource: repo://src/frontend_common/config.cpp
  - id: openwiki-source-2f75cf9add4ee78edf14128c
    resource: repo://src/qt_common/config/qt_config.cpp
  - id: openwiki-source-849abc74091a24580c4169e4
    resource: repo://src/yuzu/main_window.cpp
generated: { by: "codex", at: "2026-11-26T21:54:46.723Z" }
---

# Configuration and per-title settings

## Settings ownership

Settings::SwitchableSetting retains a global value and a custom value for a setting. SetGlobal selects the active slot, and the getter can explicitly request the global value. The type registers a restoration callback, so selecting the global slot again also works for settings added through the same linkage. [`settings_setting.h`](../../src/common/settings_setting.h#L291)

FrontendCommon::Config reads persisted keys into the shared settings registry. Its generic reader distinguishes a custom configuration from the global one, reads the per-setting global flag for switchable settings, and loads the appropriate value. QtConfig wraps the shared reload and save routines with its own GUI value handling. [`config.cpp`](../../src/frontend_common/config.cpp#L946) [`qt_config.cpp`](../../src/qt_common/config/qt_config.cpp#L66)

## Launch precedence

Qt reads a per-game configuration during BootGame. Core applies early title defaults before constructing the GPU, then invokes renderer-aware overrides after the renderer exists. Start with the Qt boot path for persisted selection and core setup for defaults that affect initialization. [`main_window.cpp`](../../src/yuzu/main_window.cpp#L1994) [`core.cpp`](../../src/core/core.cpp#L446) [`core.cpp`](../../src/core/core.cpp#L491)

GameSettings::OverrideIfGlobal changes the custom slot only while the setting is still using its global value. The renderer-aware pass reapplies early defaults before applying settings informed by title and host GPU. This keeps explicit per-title values for settings routed through OverrideIfGlobal. [`game_settings.cpp`](../../src/core/game_settings.cpp#L125) [`game_settings.cpp`](../../src/core/game_settings.cpp#L169)

## Related pages

[Emulation lifecycle](../architecture/emulation.md) · [Qt and SDL frontends](../architecture/frontends.md) · [Build, PGO and release](../development/build-and-test.md)
