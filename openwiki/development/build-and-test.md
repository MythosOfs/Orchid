---
type: workflow
title: Build and test Orchid
description: Linux development build settings, target selection and focused Catch2 and expect test commands.
tags: [build, cmake, testing]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T21:50:59.685Z
sources:
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
generated: { by: "codex", at: "2026-11-26T21:50:59.685Z" }
---

# Build and test Orchid

## Linux development build

From the repository root, run `./tools/build-local.sh`. The [script](../../tools/build-local.sh#L5) uses Ninja and ccache with a `RelWithDebInfo` configuration, `-O2 -g1`, and LTO disabled. It defaults to `build-local/` and accepts a different directory through `ORCHID_BUILD_DIR`. The script reuses the existing CMake configuration unless the cache is missing, options are supplied, or `--configure-only` requests configuration.

The script builds the `yuzu` target. Its [output name](../../src/yuzu/CMakeLists.txt#L252) is `orchid`, so the default executable path is `build-local/bin/orchid`. Tests, the command-line frontend and the standalone room server are disabled by default in this local configuration. The optional command-line target is `yuzu-cmd` and its output is `eden-cli`. See [Build.md](../../docs/Build.md) for dependencies and other configurations.

## Focused tests

`YUZU_TESTS` enables the [test directory](../../src/CMakeLists.txt#L281). Configure with CTest enabled, then build the test target explicitly. The script's normal build step targets the GUI even when tests are enabled.

```sh
./tools/build-local.sh --configure-only -DYUZU_TESTS=ON -DBUILD_TESTING=ON
cmake --build build-local --target tests
build-local/bin/tests '[expect]'
```

The [test CMake file](../../src/tests/CMakeLists.txt#L270) links Catch2 and registers the binary and selected filters with CTest. [CheckExpect](../../src/tests/expect.cpp#L15) compares emitted text with a file under `src/tests/expect/` and reports the first differing line. After reviewing an intended output change, rerun the relevant filter with `ORCHID_EXPECT_UPDATE=1` to rewrite references and review the resulting diff. See [ExpectTests.md](../../docs/ExpectTests.md) for the reference workflow and [Emulation lifecycle](../architecture/emulation.md) for frontend entry points.
