# Plan 014: Without a memfd, `HostMemory` uses its plain-buffer fallback instead of a mapping that aborts

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 07d884a918..HEAD -- src/common/host_memory.cpp src/tests/common/host_memory_fallback.cpp`
> If either changed since this plan was written, compare the "Current state" excerpts against
> the live code before proceeding; on a mismatch, treat it as a STOP condition.

## Status

- **Priority**: P3
- **Effort**: S
- **Risk**: LOW
- **Depends on**: none
- **Category**: bug
- **Planned at**: commit `07d884a918`, 2026-11-19

## Why this matters

`Common::HostMemory` holds the emulated console's RAM. On POSIX it backs guest memory with a
memfd so the same pages can be mapped twice (fastmem). If `memfd_create` or `ftruncate` fails
(file descriptor limit, memory limit), `Impl::Init` switches to a private anonymous backing and
still reports success. But every later `Impl::Map` calls `mmap(..., MAP_PRIVATE | MAP_FIXED, fd = -1, ...)`
without `MAP_ANONYMOUS`, which fails with `EBADF`; the `ASSERT_MSG` there only logs, and the guest
then runs on unmapped addresses. On top of that the fallback leaks the memfd (`fd = -1; close(fd);`
closes `-1`). `HostMemory` already has a working path for "fastmem not possible": when `Init`
returns `false` it allocates a plain buffer and runs without a virtual base. This plan makes those
failures take that path.

## Current state

`src/common/host_memory.cpp`, POSIX `HostMemory::Impl::Init` (starts line 569). After the
platform-specific `fd = ...` block:

```cpp
// host_memory.cpp:592-618
        bool use_anon = false;
        if (fd <= 0) {
            LOG_WARNING(Common_Memory, "memfd_create: {}", strerror(errno));
            use_anon = true;
        }
        if (!use_anon) {
            // Defined to extend the file with zeros
            int ret = ftruncate(fd, backing_size);
            if (ret != 0) {
                LOG_WARNING(Common_Memory, "ftruncate: {} (likely out-of-emory)", strerror(errno));
                use_anon = true;
            }
        }
        if (use_anon) {
            LOG_WARNING(Common_Memory, "Using private mappings instead of shared ones");
            backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_NOCORE, -1, 0));
            if (fd > 0) {
                fd = -1;
                close(fd);
            }
        } else {
            backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NOCORE, fd, 0));
        }
        if (backing_base == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "mmap failed: {}", strerror(errno));
            return false;
        }
```

`Impl::Map` (line ~652):

```cpp
        int flags = (fd >= 0 ? MAP_SHARED : MAP_PRIVATE) | MAP_FIXED;
        void* ret = mmap(virtual_base + virtual_offset, length, prot_flags, flags, fd, host_offset);
        ASSERT_MSG(ret != MAP_FAILED, "mmap: {} {}", strerror(errno), fd);
```

`Impl::Release()` (called by `~Impl`) unmaps what was mapped and closes `fd` when `fd != -1`.
The constructor's fallback (`host_memory.cpp:773-785`):

```cpp
    if (impl->Init(backing_mode == BackingMode::Deferred)) {
        backing_base = impl->backing_base;
        virtual_base = impl->virtual_base;
        ...
    } else {
        LOG_WARNING(HW_Memory, "Platform can support fastmem, but can't create it");
        fallback_buffer = true;
        backing_base = static_cast<u8*>(impl->Allocate(backing_size));
        virtual_base = nullptr;
        impl.reset();
    }
```

`HostMemory::Map`/`Unmap`/`Protect` return early when `!virtual_base || !impl` (lines 814 ff.), so
the fallback is a supported mode.

Existing test file `src/tests/common/host_memory_fallback.cpp` (whole file is inside
`#ifdef _WIN32`):

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN32
#include <windows.h>
#include <catch2/catch_test_macros.hpp>

#include "common/host_memory.h"

TEST_CASE("HostMemory: Fallback allocation is released", "[common][host_memory]") {
    u8* backing{};
    {
        Common::HostMemory memory{0x1000, 1ULL << 63};
        REQUIRE(memory.VirtualBasePointer() == nullptr);
        ...
#endif
```

`HostMemory` is not copyable but movable (`host_memory.h:44-51`).

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Configure a build with tests (once) | `./tools/build-local.sh --configure-only -DYUZU_TESTS=ON` | exit 0 |
| Build the tests | `cmake --build build-local --target tests --parallel 6` | exit 0 |
| Affected tests | `./build-local/bin/tests "[host_memory]"` | `All tests passed` |
| Whole suite | `./build-local/bin/tests` | `All tests passed` |
| GUI build | `cmake --build build-local --target yuzu --parallel 6` | exit 0 |
| Mutation check | `orchid-agent-test --build-dir build-local --base HEAD -- "[host_memory]"` | exit 0 |

Notes: add `-DCPM_SOURCE_CACHE=/home/leon/src/Eglantine/.cache/cpm` to the configure command if
that folder exists; skip the configure if `build-local/CMakeCache.txt` already has
`YUZU_TESTS:BOOL=ON`. One full build at a time. If `orchid-agent-test` is missing, say so.

## Scope

**In scope**:

- `src/common/host_memory.cpp` (`Impl::Init`, POSIX branch, the lines shown above)
- `src/tests/common/host_memory_fallback.cpp` (add a Linux test)
- `plans/README.md` (status row)

**Out of scope**:

- The Windows `Impl` (`#ifdef _WIN32` part of `host_memory.cpp`).
- `Impl::Map`'s `MAP_PRIVATE` branch: after this plan it is unreachable on POSIX; removing it is a
  separate clean-up.
- The platform-specific `fd = ...` lines (shm_open, mkstemp, memfd_create).

## Git workflow

- One commit. Suggested subject: `host_memory: use the plain buffer when no memfd can back guest
  memory`.
- Do NOT push unless the operator told you to, or you work under this repository's `AGENTS.md`
  rules.

## Steps

### Step 1: A failing Linux test

Append to `src/tests/common/host_memory_fallback.cpp`, after the existing `#endif`:

```cpp
#ifdef __linux__
#include <optional>
#include <sys/resource.h>
#include <catch2/catch_test_macros.hpp>

#include "common/host_memory.h"

TEST_CASE("HostMemory: No memfd leads to the plain buffer", "[common][host_memory]") {
    rlimit old_limit{};
    REQUIRE(getrlimit(RLIMIT_NOFILE, &old_limit) == 0);
    rlimit no_files = old_limit;
    no_files.rlim_cur = 0; // memfd_create now fails with EMFILE
    REQUIRE(setrlimit(RLIMIT_NOFILE, &no_files) == 0);
    std::optional<Common::HostMemory> memory;
    memory.emplace(0x10000, 0x10000);
    REQUIRE(setrlimit(RLIMIT_NOFILE, &old_limit) == 0);
    CHECK(memory->VirtualBasePointer() == nullptr);
    CHECK(memory->BackingBasePointer() != nullptr);
}
#endif
```

Keep the window with the lowered limit as short as shown: restore it before any `CHECK`.

**Verify**: build, `./build-local/bin/tests "[host_memory]"` → the new test FAILS on
`VirtualBasePointer() == nullptr` (today `Init` succeeds with the private backing).

### Step 2: Fail `Init` instead of the private backing

Replace the block shown in "Current state" (from `bool use_anon = false;` through the closing brace
of the `if (use_anon) { ... } else { ... }`) with:

```cpp
        // Fastmem maps the backing a second time, which only a shared file allows. Without one,
        // HostMemory falls back to a plain buffer.
        if (fd < 0) {
            LOG_WARNING(Common_Memory, "memfd_create: {}", strerror(errno));
            return false;
        }
        // Defined to extend the file with zeros
        if (ftruncate(fd, backing_size) != 0) {
            LOG_WARNING(Common_Memory, "ftruncate: {} (likely out-of-memory)", strerror(errno));
            return false; // Release() closes fd
        }
        backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NOCORE, fd, 0));
```

Leave the following `if (backing_base == MAP_FAILED)` check as it is.

**Verify**: build, `./build-local/bin/tests "[host_memory]"` → `All tests passed`;
`grep -n "use_anon\|Using private mappings" src/common/host_memory.cpp` → no output.

### Step 3: Full suite, GUI build, mutation check

**Verify**: `./build-local/bin/tests` → `All tests passed`;
`cmake --build build-local --target yuzu --parallel 6` → exit 0;
`orchid-agent-test --build-dir build-local --base HEAD -- "[host_memory]"` → exit 0, score ≥ 80 %.
The `ftruncate` branch has no test (provoking it needs `RLIMIT_FSIZE`, which raises `SIGXFSZ` in
the test process); if its mutants survive, name that reason in the report.

## Test plan

One Linux test in `src/tests/common/host_memory_fallback.cpp`: with the file descriptor limit at 0,
`HostMemory` must come up in fallback mode (no virtual base, a backing buffer). The existing
`[host_memory]` tests (`host_memory.cpp`) must stay green; they cover the normal fastmem path.

## Done criteria

ALL must hold:

- [ ] `./build-local/bin/tests "[host_memory]"` prints `All tests passed`
- [ ] `./build-local/bin/tests` prints `All tests passed`
- [ ] `cmake --build build-local --target yuzu --parallel 6` exits 0
- [ ] `grep -c "use_anon" src/common/host_memory.cpp` prints `0`
- [ ] Mutation check exits 0 (or the report states the tool is not installed)
- [ ] `git status --short` lists only the in-scope files
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- The new test passes before step 2, or `setrlimit` fails in the test environment (some sandboxes
  forbid it; report instead of skipping the test).
- Any existing `[host_memory]` test fails after step 2.
- You find code outside `Impl` that relies on `Init` succeeding without a memfd
  (`rg -n "fallback_buffer|VirtualBasePointer" src/core src/common`).

## Maintenance notes

- On a machine where memfd fails, the emulator now runs without fastmem (slower, but correct)
  instead of on broken mappings. The log line `Platform can support fastmem, but can't create it`
  tells a user why.
- A follow-up may remove the now-unreachable `MAP_PRIVATE` branch in `Impl::Map`.
