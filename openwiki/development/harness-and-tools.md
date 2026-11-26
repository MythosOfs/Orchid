---
type: workflow
title: Orchid harness and development tools
description: Private-display MCP contract, test evidence and repository utilities.
tags: [orchid, development]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T22:05:23.195Z
sources:
  - id: openwiki-source-8037e2358a2c4f9b2c722a11
    resource: repo://AGENTS.md
  - id: openwiki-source-d44494ef3e497fea81240ef8
    resource: repo://CMakeLists.txt
  - id: openwiki-source-178cd3a8b6b64347d1893796
    resource: repo://src/tests/CMakeLists.txt
  - id: openwiki-source-3e4be3ba3cb1c320fd084347
    resource: repo://src/tests/expect.cpp
  - id: openwiki-source-6beb56f71e8be126fb014149
    resource: repo://tools/README.md
generated: { by: "codex", at: "2026-11-26T22:05:23.195Z" }
---

# Orchid harness and development tools

## Choose the verification path

The repository distinguishes a focused Catch2 test, the mutation runner and real emulator tests. The mutation runner establishes a normal test baseline, applies mutations in a private source copy and reruns the selected filter. Its base, file and line options let a developer identify the production logic that the test is expected to exercise. [`AGENTS.md`](../../AGENTS.md#L91) [`CMakeLists.txt`](../../src/tests/CMakeLists.txt#L270)

CMake enables the offline Maxwell disassembler, IR dumper and SPIR-V translator with `-DENABLE_DEBUG_TOOLS=ON`. The option defaults to off. [CMake targets](../../CMakeLists.txt#L727). Offline shader tools under tools provide Maxwell disassembly, recompiler IR and SPIR-V output for focused investigation. Use those tools to identify a translation boundary and the expect helper to capture intentional changes to stable text output. [`README.md`](../../tools/README.md#L8) [`expect.cpp`](../../src/tests/expect.cpp#L15)

## Orchid MCP session contract

The harness is external to the emulator tree. The checked-in contract requires a private display and virtual input for automated runs. A test brief identifies the build, settings, route and snapshot. Pass the build under test as bin, and boot the route's recorded snapshot before route_play. Screen checkpoints make route playback wait for the expected UI state through varying load times. [`AGENTS.md`](../../AGENTS.md#L139) [`AGENTS.md`](../../AGENTS.md#L173) [`AGENTS.md`](../../AGENTS.md#L197) [`AGENTS.md`](../../AGENTS.md#L216)

Use health to distinguish a stall, device loss or exit from a live session. The summary compares current errors against a baseline for the game and route, and frame CSVs report per-guest-frame timings. Use stop to collect the final evidence and end the session. For longer operations, job_status queries the job returned by operations such as gate_start or benchmark_start. [`AGENTS.md`](../../AGENTS.md#L225) [`AGENTS.md`](../../AGENTS.md#L238)

The gameplay gate replays registered routes to produce per-game results and a contact sheet. Inspect the images and new errors before accepting the gate as a baseline. A green unit test and a completed gate job answer different questions because the gate also checks visible gameplay and compares behavior against the last good build. [`AGENTS.md`](../../AGENTS.md#L271)

## Related pages

[Qt and SDL frontends](../architecture/frontends.md) · [Build, PGO and release](../development/build-and-test.md)
