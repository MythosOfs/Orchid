---
type: architecture
title: Audio and host sinks
description: Guest audio rendering and host sinks.
tags: [orchid, architecture]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T21:54:46.723Z
sources:
  - id: openwiki-source-8c724a03f8ca5cd35b4dbb79
    resource: repo://src/audio_core/audio_core.cpp
  - id: openwiki-source-32a6560ed8428cc0c1a9dfa6
    resource: repo://src/audio_core/renderer/system.cpp
  - id: openwiki-source-5ffd3fd0a98a79fa6c5ef27b
    resource: repo://src/core/hle/service/audio/audio_renderer_manager.cpp
  - id: openwiki-source-ba69e5296bab1a30a60cc144
    resource: repo://src/core/hle/service/audio/audio_renderer.cpp
generated: { by: "codex", at: "2026-11-26T21:54:46.723Z" }
---

# Audio and host sinks

## Guest renderer sessions

The audio service registers distinct input, output and renderer ports. The renderer manager opens an IAudioRenderer session with parameters and transfer memory supplied by the guest, and the session initializes its AudioCore implementation with a process handle and work-buffer information. [`audio.cpp`](../../src/core/hle/service/audio/audio.cpp#L187) [`audio_renderer_manager.cpp`](../../src/core/hle/service/audio/audio_renderer_manager.cpp#L40) [`audio_renderer.cpp`](../../src/core/hle/service/audio/audio_renderer.cpp#L22)

The renderer update IPC receives an input buffer and returns separate status and performance buffers through the renderer implementation. RequestUpdate and RequestUpdateAuto converge on the same update path, so follow that path for state parsing and result propagation. [`audio_renderer.cpp`](../../src/core/hle/service/audio/audio_renderer.cpp#L78)

## Mix state and host output

AudioCore::Renderer::System updates guest-provided state in dependency order. InfoUpdater processes behavior and memory pools before voice resources, voices, effects, mixes and sinks. The update reports failures at the stage that encounters them, which makes the first rejected stage an entry point for renderer diagnostics. [`system.cpp`](../../src/audio_core/renderer/system.cpp#L454)

AudioCore constructs host input and output sinks from the selected engine and the respective device IDs, then creates the audio DSP using the output sink. This separates guest renderer session state from the selected host backend. Start at the sink factory for host device selection and at the renderer session for malformed guest updates. [`audio_core.cpp`](../../src/audio_core/audio_core.cpp#L14) [`sink_details.cpp`](../../src/audio_core/sink/sink_details.cpp#L141)

## Related pages

[Guest IPC and services](../architecture/guest-services.md) · [Qt and SDL frontends](../architecture/frontends.md) · [Configuration and per-title settings](../architecture/configuration.md)
