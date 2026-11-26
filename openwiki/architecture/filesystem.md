---
type: architecture
title: Filesystem and application loading
description: VFS, format dispatch, applet process creation and content.
tags: [orchid, architecture]
verified:
  - by: openwiki/0.9.0
    at: 2026-11-26T22:05:23.195Z
sources:
  - id: openwiki-source-160d83a24fe077e508d6dcfc
    resource: repo://docs/adr/0008-host-and-container-fixes-from-the-fork-scan.md
  - id: openwiki-source-664647317b8f3aa15647b1a5
    resource: repo://src/core/core.cpp
  - id: openwiki-source-72d6af1e9bfc66c5ebeb8561
    resource: repo://src/core/hle/service/am/process_creation.cpp
  - id: openwiki-source-19e1cd500f4c3edb6c5b209f
    resource: repo://src/core/loader/loader.cpp
generated: { by: "codex", at: "2026-11-26T22:05:23.195Z" }
---

# Filesystem and application loading

## Opening a virtual file

Core resolves the path through VirtualFilesystem before passing the resulting VirtualFile to the applet process creation code. The helper also recognizes split files and constructs a concatenated virtual file, so downstream loaders can consume a single file abstraction. Debug path opening at this boundary before changing a format loader. [`core.cpp`](../../src/core/core.cpp#L85) [`core.cpp`](../../src/core/core.cpp#L398)

Loader::GetLoader first rejects a missing file and checks whether an indexed program can be resolved. It then identifies the file by content, consults the filename, and falls back to the filename type when content identification is unknown. A recognized deconstructed ExeFS directory can take precedence over interpreting its main binary as a lone NSO. [`loader.cpp`](../../src/core/loader/loader.cpp#L465)

## From content to a process

The applet process creation implementation obtains an AppLoader and initializes an AM process with it. This is the seam between file-format handling and process construction. Loader status is propagated to the core launch path, where failed loads are cleaned up. [`process_creation.cpp`](../../src/core/hle/service/am/process_creation.cpp#L40) [`core.cpp`](../../src/core/core.cpp#L404)

Core keeps a ContentProviderUnion and separately holds the loader for the current application. Its launch path uses PatchManager with the filesystem controller and content provider to obtain control metadata. For missing game metadata, trace provider selection and PatchManager separately from executable format recognition. [`core.cpp`](../../src/core/core.cpp#L491) [`core.cpp`](../../src/core/core.cpp#L630)

## Switch 2 NX and NUP formats

The loader recognizes NX and NUP separately and constructs the corresponding `AppLoader_NX` or `AppLoader_NUP`. Packed NX applications started by the native Home Menu use extra content published by game-list discovery; direct loads can publish their own packed extras.

[ADR 0008, Home Menu content ownership](../../docs/adr/0008-host-and-container-fixes-from-the-fork-scan.md#home-menu-application-ownership) · [ADR 0008, NUP writer](../../docs/adr/0008-host-and-container-fixes-from-the-fork-scan.md#nup-writer)

## Related pages

[Emulation lifecycle](../architecture/emulation.md) · [Kernel and HLE](../architecture/kernel.md) · [Guest IPC and services](../architecture/guest-services.md)
