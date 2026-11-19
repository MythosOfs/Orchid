# Plan 011: `FlatAllocator::Allocate` never hands out a range that overlaps a fixed allocation (investigate first)

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 07d884a918..HEAD -- src/common/address_space.inc src/common/address_space.h`
> If either changed since this plan was written, compare the "Current state" excerpts against
> the live code before proceeding; on a mismatch, treat it as a STOP condition.

## Status

- **Priority**: P2
- **Effort**: S-M
- **Risk**: MED (the allocator hands out GPU virtual address space for every game)
- **Depends on**: none
- **Category**: bug (investigate: the tests decide whether the fix is applied)
- **Planned at**: commit `07d884a918`, 2026-11-19

## Why this matters

`Common::FlatAllocator` hands out ranges of an address space: GPU virtual pages in
`nvhost_as_gpu` (`AllocateSpace`) and device addresses in `DeviceMemoryManager`. Allocation is
linear: the next range starts where the last one ended. When a range ahead of that point was
already reserved with `AllocateFixed` (guests ask for fixed GPU addresses with
`MappingFlags::Fixed`), a loop is meant to skip past it. Its condition is the reverse of the
search loop right below it: it **stops** at the first block that is too small or already
allocated and returns that block's start. Reading the code, an `Allocate` whose linear range
crosses a fixed reservation returns the start of the fixed reservation itself, so two owners get
overlapping GPU addresses (silent memory corruption). A second case: when the linear range ends
past the last block, the loop does not run at all, and the fallback search never looks at the
free tail, so `Allocate` reports the address space as full although it is not.

There are no tests for `FlatAllocator`. This plan first writes characterization tests that show
whether the reading above is right. Only if they fail as predicted is the loop rewritten.

## Current state

`src/common/address_space.h:114-140` — the class (template, instantiated in
`src/common/address_space.cpp` as `template class Common::FlatAllocator<u32, 0, 32>;`):

```cpp
template <typename VaType, VaType UnmappedVa, size_t AddressSpaceBits>
    requires AddressSpaceValid<VaType, AddressSpaceBits>
class FlatAllocator
    : public FlatAddressSpaceMap<VaType, UnmappedVa, bool, false, false, AddressSpaceBits> {
    ...
    explicit FlatAllocator(VaType virt_start, VaType va_limit = Base::VaMaximum);
    VaType Allocate(VaType size);                 // returns UnmappedVa (0 here) when full
    void AllocateFixed(VaType virt, VaType size); // this->Map(virt, true, size)
    void Free(VaType virt, VaType size);
```

The base keeps a sorted `std::vector<Block> blocks` (starts as one block at `virt = 0`,
unmapped). A block marks where a run starts; `Mapped()` means "allocated" (`phys == true`). The
run of a block ends where the next block starts; the last block runs to `va_limit`.

`src/common/address_space.inc:296-350` — `Allocate`:

```cpp
ALLOC_MEMBER(VaType)::Allocate(VaType size) {
    std::scoped_lock lock(this->block_mutex);

    VaType alloc_start{UnmappedVa};
    VaType alloc_end{current_linear_alloc_end + size};

    // Avoid searching backwards in the address space if possible
    if (alloc_end >= current_linear_alloc_end && alloc_end <= this->va_limit) {
        auto alloc_end_successor{
            std::lower_bound(this->blocks.begin(), this->blocks.end(), alloc_end)};
        if (alloc_end_successor == this->blocks.begin()) {
            ASSERT_MSG(false, "First block in AS map is invalid!");
        }

        auto alloc_end_predecessor{std::prev(alloc_end_successor)};
        if (alloc_end_predecessor->virt <= current_linear_alloc_end) {
            alloc_start = current_linear_alloc_end;
        } else {
            // Skip over fixed any mappings in front of us
            while (alloc_end_successor != this->blocks.end()) {
                if (alloc_end_successor->virt - alloc_end_predecessor->virt < size ||
                    alloc_end_predecessor->Mapped()) {
                    alloc_start = alloc_end_predecessor->virt;
                    break;
                }

                alloc_end_predecessor = alloc_end_successor++;

                // Use the VA limit to calculate if we can fit in the final block since it has no
                // successor
                if (alloc_end_successor == this->blocks.end()) {
                    alloc_end = alloc_end_predecessor->virt + size;

                    if (alloc_end >= alloc_end_predecessor->virt && alloc_end <= this->va_limit) {
                        alloc_start = alloc_end_predecessor->virt;
                    }
                }
            }
        }
    }

    if (alloc_start != UnmappedVa) {
        current_linear_alloc_end = alloc_start + size;
    } else { // If linear allocation overflows the AS then find a gap
        if (this->blocks.size() <= 2) {
            ASSERT_MSG(false, "Unexpected allocator state!");
        }

        auto search_predecessor{std::next(this->blocks.begin())};
        auto search_successor{std::next(search_predecessor)};

        while (search_successor != this->blocks.end() &&
               (search_successor->virt - search_predecessor->virt < size ||
                search_predecessor->Mapped())) {
            search_predecessor = search_successor++;
        }

        if (search_successor != this->blocks.end()) {
            alloc_start = search_predecessor->virt;
        } else {
            return {}; // AS is full
        }
    }

    this->MapLocked(alloc_start, true, size, {});
    return alloc_start;
}
```

Compare the two loops: the fallback search **continues while** a block is too small or mapped;
the skip loop **stops (and takes the block) when** it is too small or mapped.

Hand trace of the case tested in step 1 (start 0x10, limit 0x1000): `Allocate(0x10)` → `0x10`,
linear end `0x20`; `AllocateFixed(0x28, 0x8)` → blocks `{0 U}{0x10 M}{0x20 U}{0x28 M}{0x30 U}`;
`Allocate(0x10)`: `alloc_end = 0x30`, successor `{0x30 U}`, predecessor `{0x28 M}` (> linear end)
→ skip loop: gap 8 < 0x10 → `alloc_start = 0x28`, which is the fixed range.

Callers (do not change them): `src/core/hle/service/nvdrv/devices/nvhost_as_gpu.cpp:512-519`
(`AllocateFixed` for `MappingFlags::Fixed`, else `Allocate`, `ASSERT` + `InsufficientMemory` on
0), `:707`; `src/core/device_memory_manager.inc:161-169` (`main_allocator`).

Conventions: 4 spaces, about 100 columns, comments say why. Tests: Catch2 in `tests`, sources
listed by hand in `src/tests/CMakeLists.txt`; common tests in `src/tests/common/`. Exemplar:
`src/tests/common/range_map.cpp` or any small file there.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Configure a build with tests (once) | `./tools/build-local.sh --configure-only -DYUZU_TESTS=ON` | exit 0 |
| Build the tests | `cmake --build build-local --target tests --parallel 6` | exit 0 |
| New tests | `./build-local/bin/tests "[flat_allocator]"` | see each step |
| Whole suite | `./build-local/bin/tests` | `All tests passed` |
| GUI build | `cmake --build build-local --target yuzu --parallel 6` | exit 0 |
| Mutation check | `orchid-agent-test --build-dir build-local --base HEAD -- "[flat_allocator]"` | exit 0 |

Notes: add `-DCPM_SOURCE_CACHE=/home/leon/src/Eglantine/.cache/cpm` to the configure command if
that folder exists; skip the configure if `build-local/CMakeCache.txt` already has
`YUZU_TESTS:BOOL=ON`. One full build at a time. If `orchid-agent-test` is missing, say so.

## Scope

**In scope**:

- `src/common/address_space.inc` (`FlatAllocator::Allocate`, the skip loop only)
- `src/tests/common/flat_allocator.cpp` (create)
- `src/tests/CMakeLists.txt` (one added line)
- `plans/README.md` (status row)

**Out of scope**:

- `FlatAddressSpaceMap::MapLocked` / `UnmapLocked` and the fallback search loop: behaviour
  outside the skip loop stays as it is.
- `nvhost_as_gpu.cpp`, `device_memory_manager.inc`, `FlatMemoryManager`.

## Git workflow

- One commit (tests and fix together) if the fix is applied; a commit with the tests alone if the
  tests already pass (then the plan ends at step 2, see STOP conditions). Suggested subject:
  `common: FlatAllocator skips fixed ranges ahead of its linear end`. Body: what the old loop did,
  the hand trace, the tests.
- Do NOT push unless the operator told you to, or you work under this repository's `AGENTS.md`
  rules.

## Steps

### Step 1: Characterization tests

Create `src/tests/common/flat_allocator.cpp`; add `    common/flat_allocator.cpp` to
`add_executable(tests ...)` directly after `    common/fibers.cpp`.

```cpp
#include <catch2/catch_test_macros.hpp>

#include "common/address_space.h"

namespace {
using Allocator = Common::FlatAllocator<u32, 0, 32>;

TEST_CASE("FlatAllocator hands out consecutive ranges", "[common][flat_allocator]") {
    Allocator allocator{0x10, 0x1000};
    CHECK(allocator.Allocate(0x10) == 0x10);
    CHECK(allocator.Allocate(0x20) == 0x20);
    CHECK(allocator.Allocate(0x8) == 0x40);
}

TEST_CASE("FlatAllocator does not overlap a fixed range ahead of its linear end",
          "[common][flat_allocator]") {
    Allocator allocator{0x10, 0x1000};
    REQUIRE(allocator.Allocate(0x10) == 0x10);
    allocator.AllocateFixed(0x28, 0x8); // a guest's fixed reservation inside the next range
    CHECK(allocator.Allocate(0x10) == 0x30);
    CHECK(allocator.Allocate(0x4) == 0x40);
}

TEST_CASE("FlatAllocator uses the free tail behind several fixed ranges",
          "[common][flat_allocator]") {
    Allocator allocator{0x10, 0x1000};
    REQUIRE(allocator.Allocate(0x10) == 0x10);
    allocator.AllocateFixed(0x24, 0x4);
    allocator.AllocateFixed(0x30, 0x4);
    CHECK(allocator.Allocate(0x20) == 0x34);
}

TEST_CASE("FlatAllocator reports a full address space", "[common][flat_allocator]") {
    Allocator allocator{0x10, 0x40};
    REQUIRE(allocator.Allocate(0x30) == 0x10);
    CHECK(allocator.Allocate(0x10) == 0);
}
} // namespace
```

Copy the license header from a neighbouring test file.

**Verify**: build, `./build-local/bin/tests "[flat_allocator]"` → expected result before any fix:

- "hands out consecutive ranges": passes.
- "does not overlap a fixed range": FAILS, the first `CHECK` shows `0x28 == 0x30` (as `40 == 48`).
- "uses the free tail": FAILS, shows `0 == 0x34` (`0 == 52`).
- "reports a full address space": passes.

Write down the actual values. Then go to step 2.

### Step 2: Decide

- If the two middle tests fail with exactly the values above: continue with step 3.
- If they fail with other values, or pass: STOP (see STOP conditions). Commit nothing but report
  the values.

### Step 3: Rewrite the skip loop

Replace the whole `else { // Skip over fixed any mappings in front of us ... }` branch (from
`} else {` after `alloc_start = current_linear_alloc_end;` to its closing brace) with:

```cpp
        } else {
            // A fixed allocation lies inside the linear range: take the first free run from
            // there on that is large enough. The last block's run ends at the VA limit.
            while (true) {
                const VaType run_end{alloc_end_successor == this->blocks.end()
                                         ? this->va_limit
                                         : alloc_end_successor->virt};
                if (alloc_end_predecessor->Unmapped() &&
                    run_end - alloc_end_predecessor->virt >= size) {
                    alloc_start = alloc_end_predecessor->virt;
                    break;
                }
                if (alloc_end_successor == this->blocks.end()) {
                    break;
                }
                alloc_end_predecessor = alloc_end_successor++;
            }
        }
```

When it finds nothing, `alloc_start` stays `UnmappedVa` and the existing fallback search runs, as
before.

**Verify**: build, `./build-local/bin/tests "[flat_allocator]"` → `All tests passed` (4 cases).

### Step 4: Full suite, GUI build, mutation check

**Verify**: `./build-local/bin/tests` → `All tests passed`;
`cmake --build build-local --target yuzu --parallel 6` → exit 0;
`orchid-agent-test --build-dir build-local --base HEAD -- "[flat_allocator]"` → exit 0, score ≥ 80 %.

## Test plan

`src/tests/common/flat_allocator.cpp`, 4 cases, tag `[flat_allocator]`: linear allocation, a fixed
range inside the next linear range, the free tail behind two fixed ranges, a full address space.
The first and last pin behaviour that must not change.

## Done criteria

ALL must hold (case "fix applied"):

- [ ] Step 1 results recorded in the report (values of the two failing checks before the fix)
- [ ] `./build-local/bin/tests "[flat_allocator]"` prints `All tests passed` with 4 test cases
- [ ] `./build-local/bin/tests` prints `All tests passed`
- [ ] `cmake --build build-local --target yuzu --parallel 6` exits 0
- [ ] `grep -n "Skip over fixed any mappings" src/common/address_space.inc` prints nothing
- [ ] Mutation check exits 0 (or the report states the tool is not installed)
- [ ] `git status --short` lists only the in-scope files
- [ ] `plans/README.md` status row updated (DONE, or BLOCKED with the step-2 values)

## STOP conditions

Stop and report back (do not improvise) if:

- In step 1 the two middle tests pass, or fail with values other than `0x28` and `0`. Then the
  reading in "Why this matters" is wrong; report the values and leave the allocator unchanged
  (set the plan to BLOCKED in `plans/README.md` with the values).
- In step 1 the first or the last test fails: the basic behaviour differs from the excerpt.
- After step 3 any other test fails (`[nvdrv]`, `[kernel]`, `[gpu]`, device memory tests).
- You find a caller that depends on `Allocate` returning a fixed range's start (search
  `rg -n "Allocate\(" src/core/hle/service/nvdrv`).

## Maintenance notes

- After the fix, a gap between the linear end and the fixed range that would fit is skipped
  rather than used; the allocator stays linear. That is intended (matches the "avoid searching
  backwards" design).
- The fallback search (`search_predecessor` loop) still never considers the last block's run.
  With the new skip loop that run is checked first whenever the linear end fits under `va_limit`,
  so this only matters once the linear end has passed the limit. Left alone on purpose.
- Every game uses this allocator through `nvhost_as_gpu`. AGENTS.md needs no gameplay gate for a
  commit, but a reviewer may ask for one boot of a game that maps fixed GPU ranges; per AGENTS.md
  that run goes to the test subagent.
