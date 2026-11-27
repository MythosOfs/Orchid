# Plan 007: Guest fence counts and syncpoint ids cannot index past the syncpoint tables

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 07d884a918..HEAD -- src/video_core/host1x/syncpoint_manager.h src/video_core/host1x/syncpoint_manager.cpp src/core/hle/service/nvnflinger/ui/fence.h src/core/hle/service/nvdrv/devices/nvdisp_disp0.cpp src/tests/video_core/syncpoint_manager.cpp src/tests/CMakeLists.txt`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition. (`src/tests/CMakeLists.txt` may have
> changed through plan 006; that is expected — only check that the lines you edit still exist.)

## Status

- **Priority**: P1
- **Effort**: S
- **Risk**: LOW
- **Depends on**: none (if plan 006 already landed, its orphan check requires the new test file
  to be listed — step 3 does that)
- **Category**: security
- **Planned at**: commit `07d884a918`, 2026-11-19

## Why this matters

When a game presents a frame through the Switch 1 display path, it sends a buffer with a fence
(`android::Fence`: a count and four `{id, value}` pairs) in an IPC parcel. The emulator trusts
both numbers. A count above 4 makes `nvdisp_disp0::Composite` read `NvFence` entries past the end
of the 4-entry array, and any id from 192 up makes `SyncpointManager::RegisterGuestAction` index
past its 192-entry tables, treating unrelated memory as a `std::list` and inserting into it. That
is host memory corruption driven by guest data. A recent fix (`IsValidSyncpoint`) already guards
`IncrementGuest`/`IncrementHost` against exactly this, after it crashed there; the register,
deregister and wait entry points were missed. After this plan every public entry point that
indexes the tables checks the id, and the fence loop never reads past its array.

## Current state

- `src/core/hle/service/nvnflinger/ui/fence.h` — the parcel fence:

```cpp
// fence.h:19-33
class Fence {
public:
    static constexpr Fence NoFence() {
        Fence fence{};
        fence.fences[0].id = -1;
        fence.fences[1].id = -1;
        fence.fences[2].id = -1;
        fence.fences[3].id = -1;
        return fence;
    }

    u32 num_fences{};
    std::array<Service::Nvidia::NvFence, 4> fences{};
};
static_assert(sizeof(Fence) == 36, "Fence has wrong size");
```

  `NvFence` (`src/core/hle/service/nvdrv/nvdata.h:19-23`) is `{ s32 id; u32 value; }`. The fence
  arrives unchanged from the guest: `buffer_queue_producer.cpp` copies it into the queued item
  (`item.fence = fence;`), `hardware_composer.cpp:130` copies it into the layer
  (`.acquire_fence = item.fence`). `num_fences` is read only in `nvdisp_disp0.cpp`.

- `src/core/hle/service/nvdrv/devices/nvdisp_disp0.cpp:83-87` — the loop that trusts the count:

```cpp
        for (size_t i = 0; i < layer.acquire_fence.num_fences; i++) {
            if (layer.acquire_fence.fences[i].id >= 0) {
                output_fences.push_back(layer.acquire_fence.fences[i]);
            }
        }
```

  `output_fences` goes to `GPU::RequestComposite`, which calls
  `syncpoint_manager.RegisterGuestAction(composite_fences[i].id, composite_fences[i].value, executer)`
  for each (`src/video_core/gpu.cpp:373-375`). The composite runs once every fence's action ran.

- `src/video_core/host1x/syncpoint_manager.h` — the unchecked entry points:

```cpp
// syncpoint_manager.h:43-57
    template <typename Func>
    ActionHandle RegisterGuestAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        return RegisterAction(syncpoints_guest[syncpoint_id], guest_action_storage[syncpoint_id],
                              expected_value, std::move(action));
    }

    template <typename Func>
    ActionHandle RegisterHostAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        return RegisterAction(syncpoints_host[syncpoint_id], host_action_storage[syncpoint_id],
                              expected_value, std::move(action));
    }

    void DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle);

    void DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle);

// syncpoint_manager.h:75-85
    void WaitGuest(u32 syncpoint_id, u32 expected_value);

    void WaitHost(u32 syncpoint_id, u32 expected_value);

    bool IsReadyGuest(u32 syncpoint_id, u32 expected_value) const {
        return syncpoints_guest[syncpoint_id].load(std::memory_order_acquire) >= expected_value;
    }

    bool IsReadyHost(u32 syncpoint_id, u32 expected_value) const {
        return syncpoints_host[syncpoint_id].load(std::memory_order_acquire) >= expected_value;
    }
```

  The tables: `std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> syncpoints_guest`, `syncpoints_host`,
  `std::array<std::list<RegisteredAction>, NUM_MAX_SYNCPOINTS> guest_action_storage`,
  `host_action_storage`, with `NUM_MAX_SYNCPOINTS = 192` (line 22). `ActionHandle` is `u64`, and `0`
  means "the action already ran inside the register call" (comment at lines 39-41).

- `src/video_core/host1x/syncpoint_manager.cpp` — the existing guard, file-local:

```cpp
// syncpoint_manager.cpp:17-30
// The puller's fence action and the 3D sync_info register take the syncpoint id from the guest's
// command stream (24 and 16 bits): an id past the table indexed unrelated memory as an action list
// and crashed in Increment.
static bool IsValidSyncpoint(u32 syncpoint_id) {
    if (syncpoint_id < SyncpointManager::NUM_MAX_SYNCPOINTS) [[likely]] {
        return true;
    }
    static std::atomic_flag logged;
    if (!logged.test_and_set()) {
        LOG_ERROR(HW_GPU, "Syncpoint {} is out of range, increment dropped (logged once)",
                  syncpoint_id);
    }
    return false;
}

// :75-81
void SyncpointManager::DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(guest_action_storage[syncpoint_id], handle);
}

void SyncpointManager::DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(host_action_storage[syncpoint_id], handle);
}

// :108-114
void SyncpointManager::WaitGuest(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_guest[syncpoint_id], wait_guest_cv, expected_value);
}

void SyncpointManager::WaitHost(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_host[syncpoint_id], wait_host_cv, expected_value);
}
```

  `IncrementGuest` (:83) and `IncrementHost` (:101) already start with
  `if (!IsValidSyncpoint(syncpoint_id)) { return; }`.

- Precedent for "an id that does not exist counts as reached": the Switch 2 syncpoint service
  signals at once for an out-of-range id (`src/core/hle/service/nx2_gsv.h`, `RequestSlotEvent`,
  `LOG_WARNING(... "out of range, signalling")`). Follow that: an action registered on an invalid
  id runs immediately and the call returns `0`, a wait on it returns at once, `IsReady*` says
  `true`. Running the action matters: the composite only happens after every fence's action ran,
  so dropping it would stop the game's frames.

- Tests: `src/tests/video_core/syncpoint_manager.cpp`, tag `[video_core][syncpoint_manager]`.
  Pattern of its first test (match its style: `std::make_unique<SyncpointManager>()`, plain
  `REQUIRE`):

```cpp
TEST_CASE("Syncpoint ids past the table are dropped, valid ones still run their actions",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    constexpr u32 Last = SyncpointManager::NUM_MAX_SYNCPOINTS - 1;
    bool fired = false;
    manager->RegisterGuestAction(Last, 1, [&fired] { fired = true; });
    ...
```

Conventions: 4 spaces, about 100 columns, braces on the same line, comments say why in one or two
lines. Every test `.cpp` is listed by hand in `src/tests/CMakeLists.txt`; `src/tests/core/vfs_vector.cpp`
is a minimal test file to copy the header/include layout from.

## Commands you will need

Run all commands from the repository root.

| Purpose | Command | Expected on success |
|---|---|---|
| Configure a build with tests (once) | `./tools/build-local.sh --configure-only -DYUZU_TESTS=ON` | exit 0 |
| Build the tests | `cmake --build build-local --target tests --parallel 6` | exit 0 |
| Run the affected tests | `./build-local/bin/tests "[syncpoint_manager],[nvnflinger]"` | `All tests passed` |
| Run the whole suite | `./build-local/bin/tests` | `All tests passed` |
| Build the emulator (checks `nvdisp_disp0.cpp` compiles in the GUI target) | `cmake --build build-local --target yuzu --parallel 6` | exit 0 |
| Mutation check | `orchid-agent-test --build-dir build-local --base HEAD -- "[syncpoint_manager],[nvnflinger]"` | exit 0 |

Notes: add `-DCPM_SOURCE_CACHE=$HOME/src/Eglantine/.cache/cpm` to the configure command if
that folder exists; skip the configure if `build-local/CMakeCache.txt` already has
`YUZU_TESTS:BOOL=ON`. One full build at a time per machine. If `orchid-agent-test` is not
installed, say so in the report.

## Scope

**In scope** (the only files you should modify):

- `src/video_core/host1x/syncpoint_manager.h`
- `src/video_core/host1x/syncpoint_manager.cpp`
- `src/core/hle/service/nvnflinger/ui/fence.h`
- `src/core/hle/service/nvdrv/devices/nvdisp_disp0.cpp`
- `src/tests/video_core/syncpoint_manager.cpp`
- `src/tests/core/nvnflinger_fence.cpp` (create)
- `src/tests/CMakeLists.txt` (one added line)
- `plans/README.md` (status row)

**Out of scope** (do NOT touch, even though they look related):

- `GetGuestSyncpointValue` / `GetHostSyncpointValue` (header lines 24-30): their callers
  (`nx2_gsv.h` `QueryCounter`, `nvhost_ctrl.cpp`) already check the id. Leave them.
- `src/video_core/gpu.cpp`, `src/core/hle/service/nx2_gsv.h`, `nvhost_ctrl.cpp`: callers; with the
  manager checking, they need no change.
- The ordering and locking logic of `RegisterAction`, `DeregisterAction`, `Increment`, `Wait`.
- `TakeMovedGuest(word)`: indexed by a word number the emulator computes, not by a guest id.

## Git workflow

- Work in the checkout you were given. One commit for the whole plan. Message style: area first,
  body says what was wrong and how it was checked (example from the log:
  `nx2:gsv: wake armed syncpoint waits again after a mirrored increment`). Suggested subject:
  `host1x: check syncpoint ids in every action and wait, clamp guest fence counts`.
- Do NOT push unless the operator told you to, or you work under this repository's `AGENTS.md`
  rules, which define when a push to `origin/main` is allowed.

## Steps

### Step 1: Write the failing manager test

Append to `src/tests/video_core/syncpoint_manager.cpp`:

```cpp
TEST_CASE("Actions and waits on a syncpoint past the table count as reached",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    constexpr u32 Past = SyncpointManager::NUM_MAX_SYNCPOINTS;
    bool guest = false;
    bool host = false;
    // A fence id from the guest's parcel: the composite waits for this action, so it must run.
    REQUIRE(manager->RegisterGuestAction(Past, 1, [&guest] { guest = true; }) == 0);
    REQUIRE(guest);
    REQUIRE(manager->RegisterHostAction(Past + 1, 1, [&host] { host = true; }) == 0);
    REQUIRE(host);
    manager->DeregisterGuestAction(Past, 1);
    manager->DeregisterHostAction(Past + 1, 1);
    manager->WaitGuest(Past, 1); // returns instead of waiting forever
    manager->WaitHost(Past + 1, 1);
    REQUIRE(manager->IsReadyGuest(Past, 1));
    REQUIRE(manager->IsReadyHost(Past + 1, 1));

    // Valid ids keep waiting for their value.
    bool valid = false;
    REQUIRE(manager->RegisterGuestAction(Past - 1, 1, [&valid] { valid = true; }) != 0);
    REQUIRE_FALSE(valid);
    REQUIRE_FALSE(manager->IsReadyGuest(Past - 1, 1));
}
```

Keep `Past` and `Past + 1` (not large ids like `0x7FFFFFFF`): before the fix, those ids land just
past the arrays inside the same object, so the first `REQUIRE` fails cleanly instead of crashing.
The first failing `REQUIRE` ends the test case, so the `Wait*` calls (which would hang before the
fix) are never reached.

**Verify**: build, then `./build-local/bin/tests "[syncpoint_manager]"` → the new test case FAILS
at the first `REQUIRE(manager->RegisterGuestAction(Past, ...) == 0)`; all other syncpoint tests
pass.

### Step 2: Check the id in every table-indexing entry point

1. In `syncpoint_manager.h`, add a public static declaration right after `NUM_MAX_SYNCPOINTS`:

   ```cpp
       /// False (logged once) for an id past the tables; such ids come from guest data.
       static bool IsValidSyncpoint(u32 syncpoint_id);
   ```

2. In `syncpoint_manager.cpp`, turn the file-local `static bool IsValidSyncpoint(u32)` into the
   member definition `bool SyncpointManager::IsValidSyncpoint(u32 syncpoint_id)` (drop the
   `static` keyword, keep the comment and the body). Change the log text from
   `"... out of range, increment dropped (logged once)"` to
   `"... out of range, ignored (logged once)"`, because it now covers more than increments.

3. In the header, make the two register templates run the action at once for an invalid id:

   ```cpp
       template <typename Func>
       ActionHandle RegisterGuestAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
           if (!IsValidSyncpoint(syncpoint_id)) {
               // No such syncpoint to wait on: count it as reached, or a composite never runs.
               action();
               return {};
           }
           return RegisterAction(syncpoints_guest[syncpoint_id], guest_action_storage[syncpoint_id],
                                 expected_value, std::move(action));
       }
   ```

   and the same for `RegisterHostAction` (without repeating the comment).

4. `IsReadyGuest` / `IsReadyHost`: return `true` for an invalid id:

   ```cpp
       bool IsReadyGuest(u32 syncpoint_id, u32 expected_value) const {
           return !IsValidSyncpoint(syncpoint_id) ||
                  syncpoints_guest[syncpoint_id].load(std::memory_order_acquire) >= expected_value;
       }
   ```

   (same shape for `IsReadyHost`).

5. In the `.cpp`, start `DeregisterGuestAction`, `DeregisterHostAction`, `WaitGuest` and
   `WaitHost` with the same early return used by `IncrementGuest`:

   ```cpp
       if (!IsValidSyncpoint(syncpoint_id)) {
           return;
       }
   ```

**Verify**: build, then `./build-local/bin/tests "[syncpoint_manager]"` → `All tests passed`
(the new case included).

### Step 3: Clamp the guest fence count

1. In `fence.h`, add `#include <algorithm>` and `#include <span>` to the includes, and a member
   function to `class Fence` (after `NoFence`):

   ```cpp
       /// The fences in use. The count comes from the guest's parcel and may exceed the array.
       std::span<const Service::Nvidia::NvFence> Active() const {
           return {fences.data(), std::min<size_t>(num_fences, fences.size())};
       }
   ```

   The `static_assert(sizeof(Fence) == 36, ...)` must still hold (a member function adds no size).

2. In `nvdisp_disp0.cpp:83-87`, replace the index loop with:

   ```cpp
           for (const auto& fence : layer.acquire_fence.Active()) {
               if (fence.id >= 0) {
                   output_fences.push_back(fence);
               }
           }
   ```

3. Create `src/tests/core/nvnflinger_fence.cpp` (copy the license header lines from
   `src/tests/core/vfs_vector.cpp`):

   ```cpp
   #include <catch2/catch_test_macros.hpp>

   #include "core/hle/service/nvnflinger/ui/fence.h"

   namespace {
   TEST_CASE("A guest fence count past the array yields only the stored fences",
             "[core][nvnflinger]") {
       auto fence = Service::android::Fence::NoFence();
       fence.num_fences = 0xFFFFFFFF;
       CHECK(fence.Active().size() == fence.fences.size());
       CHECK(fence.Active().data() == fence.fences.data());
       fence.num_fences = 2;
       CHECK(fence.Active().size() == 2);
       fence.num_fences = 0;
       CHECK(fence.Active().empty());
   }
   } // namespace
   ```

4. Add `    core/nvnflinger_fence.cpp` to `add_executable(tests ...)` in `src/tests/CMakeLists.txt`,
   directly after the line `    core/vfs_real.cpp`.

**Verify**: `cmake --build build-local --target tests --parallel 6` → exit 0;
`./build-local/bin/tests "[syncpoint_manager],[nvnflinger]"` → `All tests passed`;
`grep -n "acquire_fence.num_fences" src/core/hle/service/nvdrv/devices/nvdisp_disp0.cpp` → no
output.

### Step 4: Full suite, GUI build, mutation check

**Verify**:

- `./build-local/bin/tests` → `All tests passed`.
- `cmake --build build-local --target yuzu --parallel 6` → exit 0.
- `orchid-agent-test --build-dir build-local --base HEAD -- "[syncpoint_manager],[nvnflinger]"`
  → exit 0, score ≥ 80 %. If a surviving mutant sits in `nvdisp_disp0.cpp` (no unit test reaches
  that function; it needs a running `Core::System`), add to your report why it is uncovered
  instead of writing a system test.

## Test plan

- `src/tests/video_core/syncpoint_manager.cpp`: one new test case (step 1) covering register (guest
  and host), deregister, wait and is-ready on ids `NUM_MAX_SYNCPOINTS` and `NUM_MAX_SYNCPOINTS + 1`,
  plus a valid id that still waits.
- `src/tests/core/nvnflinger_fence.cpp`: `Fence::Active()` with a count above, inside and at zero.
- Existing syncpoint tests must stay green unchanged (ordering, cancel-from-inside, cross-thread
  cancel, mirror).

## Done criteria

ALL must hold:

- [ ] `./build-local/bin/tests "[syncpoint_manager],[nvnflinger]"` prints `All tests passed`
- [ ] `./build-local/bin/tests` prints `All tests passed`
- [ ] `cmake --build build-local --target yuzu --parallel 6` exits 0
- [ ] `grep -c "IsValidSyncpoint(syncpoint_id)" src/video_core/host1x/syncpoint_manager.cpp` prints `6`
  (Increment ×2 as before, Deregister ×2, Wait ×2), and the header has 4 uses (Register ×2, IsReady ×2)
- [ ] `grep -n "acquire_fence.num_fences" src/core/hle/service/nvdrv/devices/nvdisp_disp0.cpp` prints nothing
- [ ] Mutation check exits 0 (or the report states the tool is not installed)
- [ ] `git status --short` lists only the in-scope files
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- The new test in step 1 passes before step 2. Then the out-of-range ids do not behave as
  described here.
- The new test crashes (segfault) instead of failing in step 1. Report it; do not switch to other
  ids.
- Any existing `[syncpoint_manager]` test fails after step 2. The guard must not change behaviour
  for valid ids.
- `static_assert(sizeof(Fence) == 36)` fails.
- Another file reads `num_fences` directly (`grep -rn "num_fences" src/core`) besides
  `fence.h` and the loop you replaced.

## Maintenance notes

- `IsValidSyncpoint` now runs on every register/wait. It is one compare plus, on failure, one
  atomic flag test; it does not show up on the GPU thread's profile.
- Treating an unknown id as "reached" means a broken fence never blocks a present. That matches
  `nx2:syncpt`. If a future game relies on a fence id ≥ 192 really waiting, the table size
  (`NUM_MAX_SYNCPOINTS`) is what needs to change, not the guard.
- `Get*SyncpointValue` still index without a check; their two callers check first. A new caller
  that passes a guest id must check too.
- Reviewer focus: that `RegisterGuestAction` on an invalid id runs the action exactly once and
  returns `0`, so `DeregisterGuestAction(id, 0)` stays a no-op.
