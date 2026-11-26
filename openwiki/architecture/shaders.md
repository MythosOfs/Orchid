---
type: architecture
title: Shader recompiler and pipeline cache
description: Guest shader translation, IR, SPIR-V and pipelines.
tags: [orchid, architecture]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T21:54:46.723Z
sources:
  - id: openwiki-source-8c3969dc47b6f9c7c4464575
    resource: repo://src/shader_recompiler/frontend/maxwell/translate_program.cpp
  - id: openwiki-source-3e4be3ba3cb1c320fd084347
    resource: repo://src/tests/expect.cpp
  - id: openwiki-source-708accf73fb6c8d2a57b9c2c
    resource: repo://src/video_core/renderer_vulkan/vk_pipeline_cache.cpp
  - id: openwiki-source-6beb56f71e8be126fb014149
    resource: repo://tools/README.md
generated: { by: "codex", at: "2026-11-26T21:54:46.723Z" }
---

# Shader recompiler and pipeline cache

## From Maxwell instructions to IR

The Vulkan compute builder constructs a Maxwell control-flow graph from an environment and start address, then passes it to TranslateProgram. The shared recompiler constructs a structured syntax list and block order and records stage-specific metadata in IR::Program. For compute, it gets workgroup size and shared memory size from the environment. Compare guest decode with IR before investigating a backend opcode. [`vk_pipeline_cache.cpp`](../../src/video_core/renderer_vulkan/vk_pipeline_cache.cpp#L1910) [`translate_program.cpp`](../../src/shader_recompiler/frontend/maxwell/translate_program.cpp#L386) [`translate_program.cpp`](../../src/shader_recompiler/frontend/maxwell/translate_program.cpp#L431)

For graphics stages, PipelineCache supplies EmitSPIRV with a stage profile, runtime info and resource bindings, then builds a Vulkan shader module. The compute path validates the constant buffer mask and can clamp shared memory on selected drivers before emission. Host capability policy can therefore affect lowering or emission for the same guest shader. [`vk_pipeline_cache.cpp`](../../src/video_core/renderer_vulkan/vk_pipeline_cache.cpp#L1555) [`vk_pipeline_cache.cpp`](../../src/video_core/renderer_vulkan/vk_pipeline_cache.cpp#L1919)

## Persistent cache and diagnostics

LoadDiskResources is title-scoped and ignores title ID zero. Before changing cache files it waits for outstanding serialization work. It uses separate paths for serialized pipeline data and used-pipeline tracking, with a distinct suffix for the title workaround that changes lowered code. Review cache compatibility when changing the representation of persisted environments. [`vk_pipeline_cache.cpp`](../../src/video_core/renderer_vulkan/vk_pipeline_cache.cpp#L932)

The repository includes Maxwell disassembly, IR and SPIR-V tools as separate entry points. Use a disassembly to inspect guest decode, an IR dump to inspect translation, and SPIR-V to inspect backend output. For stable regression output, the expect-test helper compares emitted text against a reference. [`README.md`](../../tools/README.md#L8) [`expect.cpp`](../../src/tests/expect.cpp#L15)

## Related pages

[GPU command processing](../architecture/graphics.md) · [Vulkan rendering and presentation](../architecture/vulkan.md) · [Build, PGO and release](../development/build-and-test.md)
