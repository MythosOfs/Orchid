---
type: workflow
title: Build, PGO and release
description: Local build, focused tests, profile-guided build and release lifecycle.
tags: [orchid, development]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-27T06:45:36.012Z
sources:
  - id: openwiki-source-1b3b02a24e2ae5673ef8671c
    resource: repo://.github/workflows/release-binaries.yml
  - id: openwiki-source-8037e2358a2c4f9b2c722a11
    resource: repo://AGENTS.md
  - id: openwiki-source-d44494ef3e497fea81240ef8
    resource: repo://CMakeLists.txt
  - id: openwiki-source-6fbd0ecef1f5e21a2e4bf7c9
    resource: repo://src/CMakeLists.txt
  - id: openwiki-source-178cd3a8b6b64347d1893796
    resource: repo://src/tests/CMakeLists.txt
  - id: openwiki-source-3e4be3ba3cb1c320fd084347
    resource: repo://src/tests/expect.cpp
  - id: openwiki-source-cfe20b61277d4e73d007b34c
    resource: repo://src/yuzu/CMakeLists.txt
  - id: openwiki-source-3039b7c74d91b572f803dd1d
    resource: repo://tools/build-local.sh
generated: { by: "claude-code", at: "2026-11-27T06:45:36.012Z" }
---

# Build, PGO and release

## Linux development build

From the repository root, run `./tools/build-local.sh`. The [script](../../tools/build-local.sh#L5) uses Ninja and ccache with a `RelWithDebInfo` configuration, `-O2 -g1`, and LTO disabled. It defaults to `build-local/` and accepts a different directory through `ORCHID_BUILD_DIR`. The script reuses the existing CMake configuration unless the cache is missing, options are supplied, or `--configure-only` requests configuration.

The script builds the `yuzu` target. Its [output name](../../src/yuzu/CMakeLists.txt#L252) is `orchid`, so the default executable path is `build-local/bin/orchid`. Tests, the command-line frontend and the standalone room server are disabled by default in this local configuration. The optional command-line target is `yuzu-cmd` and its output is `eden-cli`. See [Build.md](../../docs/Build.md) for dependencies and other configurations.

## Offline shader tools

CMake enables the offline Maxwell disassembler, IR dumper and SPIR-V translator with `-DENABLE_DEBUG_TOOLS=ON`. The option defaults to off. See [the CMake targets](../../CMakeLists.txt#L727) and [their roles](harness-and-tools.md).

## Focused tests

`YUZU_TESTS` enables the [test directory](../../src/CMakeLists.txt#L281). Configure with CTest enabled, then build the test target explicitly. The script's normal build step targets the GUI even when tests are enabled.

```sh
./tools/build-local.sh --configure-only -DYUZU_TESTS=ON -DBUILD_TESTING=ON
cmake --build build-local --target tests
build-local/bin/tests '[expect]'
```

The [test CMake file](../../src/tests/CMakeLists.txt#L270) links Catch2 and registers the binary and selected filters with CTest. [CheckExpect](../../src/tests/expect.cpp#L15) compares emitted text with a file under `src/tests/expect/` and reports the first differing line. After reviewing an intended output change, rerun the relevant filter with `ORCHID_EXPECT_UPDATE=1` to rewrite references and review the resulting diff. See [ExpectTests.md](../../docs/ExpectTests.md) for the reference workflow and [Emulation lifecycle](../architecture/emulation.md) for frontend entry points.

## Release configuration and PGO

The existing release-binaries workflow is manually dispatched and exposes a publication toggle. Its PGO jobs verify that the tracked orchid.profdata is nonempty. Linux PGO uses a matching LLVM compiler and LLD and passes the profile to both C and C++ compilation. The non-PGO Linux branch retains Release optimization and chooses mold for linking. [`release-binaries.yml`](../../.github/workflows/release-binaries.yml#L7) [`release-binaries.yml`](../../.github/workflows/release-binaries.yml#L297) [`release-binaries.yml`](../../.github/workflows/release-binaries.yml#L429)

The release workflow consumes the checked-in PGO profile through `-fprofile-use`. Linux PGO downloads the LLVM release selected by `PGO_LLVM_VERSION`. Windows/MinGW installs unversioned MSYS2 Clang and LLD packages and selects `clang` and `clang++` from that toolchain. [`release-binaries.yml`](../../.github/workflows/release-binaries.yml#L18) [`release-binaries.yml`](../../.github/workflows/release-binaries.yml#L282) [MSYS2 packages](../../.github/workflows/release-binaries.yml#L345) [`release-binaries.yml`](../../.github/workflows/release-binaries.yml#L495) [`Build.md`](../../docs/Build.md#L223)

## Verification before packaging

The release gate is a gameplay check performed before release builds. It exercises registered routes against a selected binary and produces a contact sheet and per-game status. Rendering judgments and reported regressions still require inspection. For ordinary code changes, select the narrow test path that covers the change and follow the repository's mutation-check policy for production or unit-test changes. [`AGENTS.md`](../../AGENTS.md#L91) [`AGENTS.md`](../../AGENTS.md#L261) [`AGENTS.md`](../../AGENTS.md#L273)

## Related pages

[Orchid harness and development tools](../development/harness-and-tools.md) · [Qt and SDL frontends](../architecture/frontends.md)
