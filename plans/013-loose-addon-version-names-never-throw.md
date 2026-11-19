# Plan 013: An add-on version folder name too long for a number is skipped instead of aborting the boot

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 07d884a918..HEAD -- src/core/loader/deconstructed_rom_directory.cpp src/tests/core/loose_directory.cpp`
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

Loose (extracted) game dumps keep add-on content in `<game>/dlc/<aoc id>/<version>/...`, and
patched add-ons in `<version>/updates/<patch>/...`. `Loader::FindLooseAOC` accepts any folder
name made only of digits as a version or patch number and converts it with `std::stoull`. A name
with 20 or more digits does not fit in a `u64`, so `std::stoull` throws `std::out_of_range`, and
nothing catches it: the game does not start. A user's renamed or backup folder is enough. The same
file already parses version folder names safely with `std::from_chars` elsewhere. After this plan
such names are skipped like any other non-number.

## Current state

`src/core/loader/deconstructed_rom_directory.cpp`, inside `namespace Loader`. An anonymous
namespace runs from line 85 to line 400 (`} // namespace`); `FindLooseAOC` starts right after it
(line 406). The safe pattern already used in this file:

```cpp
// deconstructed_rom_directory.cpp:128-138 (OrderedUpdateVersionDirs)
        const std::string name = candidate->GetName();
        u64 version{0};
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), version);
        // from_chars stops at the first non digit and still reports success, so a
        // directory called "655360_old" or "999999.bak" would be accepted with its
        // leading digits. Require the whole name to be the number; anything else
        // only serves when no numeric version exists at all.
        if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size()) {
            version = 0;
        }
```

The throwing sites in `FindLooseAOC`:

```cpp
// :424-428
        for (const auto& version_dir : aoc_dir->GetSubdirectories()) {
            const auto& version = version_dir->GetName();
            if (version.empty() || version.find_first_not_of("0123456789") != std::string::npos) {
                continue;
            }

// :454-472
            if (const auto updates = version_dir->GetSubdirectory("updates")) {
                u64 best_patch = 0;
                for (const auto& patch_dir : updates->GetSubdirectories()) {
                    const auto& patch = patch_dir->GetName();
                    if (patch.empty() || patch.find_first_not_of("0123456789") != std::string::npos) {
                        continue;
                    }
                    const auto programs = patch_dir->GetSubdirectory("program");
                    const auto zero = programs != nullptr ? programs->GetSubdirectory("0") : nullptr;
                    const auto patched = zero != nullptr ? zero->GetSubdirectory("romfs") : nullptr;
                    if (patched != nullptr && std::stoull(patch) >= best_patch) {
                        candidate = patched;
                        best_patch = std::stoull(patch);
                    }
                }
            }
            if (candidate != nullptr && (romfs_dir == nullptr || std::stoull(version) >= best_version)) {
                romfs_dir = candidate;
                best_version = std::stoull(version);
            }
```

Two other `stoull` calls in this file are safe and stay: line 419 parses a name already checked to
be exactly 16 hex digits, line 504 a name checked to have at most 3 digits (`is_index`).

`<charconv>` is already included (line 9); `<optional>` is not.

Test: `src/tests/core/loose_directory.cpp:263-302`, test case
`"Loose add-on content resolves nested extracted program data"` (tag `[loose_directory]`). It
builds a real folder tree under a temp dir and has one `SECTION` per layout; every section must
end with exactly one add-on whose `data.bin` reads `"payload"`:

```cpp
TEST_CASE("Loose add-on content resolves nested extracted program data", "[loose_directory]") {
    LooseTestDirectory t;
    const auto game = t.root / "Game";
    const auto exefs = game / "updates/262144/program/1/exefs";
    LooseTestDirectory::ExeFS(exefs);
    const auto addon = game / "dlc/0100ff500e34b001";
    SECTION("legacy flat RomFS") {
        LooseTestDirectory::Put(addon / "0/romfs/data.bin", "payload");
    }
    ...
    SECTION("newest valid version wins") {
        LooseTestDirectory::Put(addon / "0/romfs/data.bin", "old");
        LooseTestDirectory::Put(addon / "65536/base/program/0/romfs/data.bin", "payload");
        LooseTestDirectory::Put(addon / "999999_old/romfs/data.bin", "invalid");
    }
    SECTION("patched add-on replaces its base") {
        LooseTestDirectory::Put(addon / "0/base/program/0/romfs/data.bin", "old");
        LooseTestDirectory::Put(addon / "0/updates/65536/program/0/romfs/data.bin", "payload");
        LooseTestDirectory::Put(addon / "0/updates/x1/program/0/romfs/data.bin", "invalid");
    }
    FileSys::RealVfsFilesystem vfs;
    const auto dir = vfs.OpenDirectory(exefs.string());
    const auto entries = Loader::FindLooseAOC(dir, 0x0100FF500E34A000ULL);
    REQUIRE(entries.size() == 1);
    ...
    CHECK(std::string(bytes.begin(), bytes.end()) == "payload");
```

Conventions: 4 spaces, about 100 columns, comments say why.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Configure a build with tests (once) | `./tools/build-local.sh --configure-only -DYUZU_TESTS=ON` | exit 0 |
| Build the tests | `cmake --build build-local --target tests --parallel 6` | exit 0 |
| Affected tests | `./build-local/bin/tests "[loose_directory]"` | `All tests passed` |
| Whole suite | `./build-local/bin/tests` | `All tests passed` |
| Mutation check | `orchid-agent-test --build-dir build-local --base HEAD -- "[loose_directory]"` | exit 0 |

Notes: add `-DCPM_SOURCE_CACHE=/home/leon/src/Eglantine/.cache/cpm` to the configure command if
that folder exists; skip the configure if `build-local/CMakeCache.txt` already has
`YUZU_TESTS:BOOL=ON`. One full build at a time. If `orchid-agent-test` is missing, say so.

## Scope

**In scope**:

- `src/core/loader/deconstructed_rom_directory.cpp` (`FindLooseAOC` and one new helper)
- `src/tests/core/loose_directory.cpp` (two new `SECTION`s in the test case above)
- `plans/README.md` (status row)

**Out of scope**:

- The `stoull` calls at lines 419 and 504 (already bounded).
- `src/core/loader/loose_directory.h` (it already uses `from_chars`).
- `OrderedUpdateVersionDirs`.

## Git workflow

- One commit. Suggested subject: `loader: skip loose add-on version names past u64`.
- Do NOT push unless the operator told you to, or you work under this repository's `AGENTS.md`
  rules.

## Steps

### Step 1: Two failing sections

In the test case above, add two `SECTION`s after `"patched add-on replaces its base"`:

```cpp
    SECTION("a version name too long for a number is skipped") {
        LooseTestDirectory::Put(addon / "0/romfs/data.bin", "payload");
        LooseTestDirectory::Put(addon / "123456789012345678901234567890/romfs/data.bin", "invalid");
    }
    SECTION("a patch name too long for a number is skipped") {
        LooseTestDirectory::Put(addon / "0/base/program/0/romfs/data.bin", "old");
        LooseTestDirectory::Put(addon / "0/updates/65536/program/0/romfs/data.bin", "payload");
        LooseTestDirectory::Put(
            addon / "0/updates/123456789012345678901234567890/program/0/romfs/data.bin", "invalid");
    }
```

**Verify**: build, `./build-local/bin/tests "[loose_directory]"` → the two new sections fail with
an exception (`stoull` / `out_of_range`); all other sections and test cases pass.

### Step 2: Parse with `from_chars`, skip what does not fit

1. Add `#include <optional>` to the includes.
2. At the end of the anonymous namespace (just before `} // namespace` at line 400), add:

   ```cpp
   // A folder name that is entirely a decimal number fitting u64; std::stoull would throw on a
   // longer one and abort the boot.
   std::optional<u64> ParseDecimalName(std::string_view name) {
       u64 value{};
       const auto parsed = std::from_chars(name.data(), name.data() + name.size(), value);
       if (name.empty() || parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size()) {
           return std::nullopt;
       }
       return value;
   }
   ```

3. In `FindLooseAOC`, use it for the version and the patch:

   ```cpp
           for (const auto& version_dir : aoc_dir->GetSubdirectories()) {
               const auto version = ParseDecimalName(version_dir->GetName());
               if (!version) {
                   continue;
               }
   ```

   ```cpp
                   for (const auto& patch_dir : updates->GetSubdirectories()) {
                       const auto patch = ParseDecimalName(patch_dir->GetName());
                       if (!patch) {
                           continue;
                       }
                       ...
                       if (patched != nullptr && *patch >= best_patch) {
                           candidate = patched;
                           best_patch = *patch;
                       }
   ```

   ```cpp
               if (candidate != nullptr && (romfs_dir == nullptr || *version >= best_version)) {
                   romfs_dir = candidate;
                   best_version = *version;
               }
   ```

   `from_chars` for an unsigned type rejects a leading `-` or `+`, so "digits only" holds as
   before. Keep everything else in the loop unchanged.

**Verify**: build, `./build-local/bin/tests "[loose_directory]"` → `All tests passed`;
`grep -n "stoull" src/core/loader/deconstructed_rom_directory.cpp` → exactly two lines (the hex id
and the program index).

### Step 3: Full suite and mutation check

**Verify**: `./build-local/bin/tests` → `All tests passed`;
`orchid-agent-test --build-dir build-local --base HEAD -- "[loose_directory]"` → exit 0, score ≥ 80 %.
If the `name.empty()` part of the helper survives a mutation: `from_chars` on an empty range
already fails with `invalid_argument`, so that test is equivalent; say so in the report (or drop
`name.empty() ||` and re-run).

## Test plan

Two new `SECTION`s in the existing `[loose_directory]` add-on test: an overlong version name and
an overlong patch name next to valid ones; the valid add-on must still be found.

## Done criteria

ALL must hold:

- [ ] `./build-local/bin/tests "[loose_directory]"` prints `All tests passed`
- [ ] `./build-local/bin/tests` prints `All tests passed`
- [ ] `grep -c "stoull" src/core/loader/deconstructed_rom_directory.cpp` prints `2`
- [ ] Mutation check exits 0 (or the report states the tool is not installed)
- [ ] `git status --short` lists only the in-scope files
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- The new sections pass before step 2, or another section fails before step 2.
- An existing section fails after step 2 (for example `"newest valid version wins"`).
- The excerpts do not match the code (the loop was restructured since the planned commit).

## Maintenance notes

- `ParseDecimalName` is the helper to use for any further numeric folder name in this file.
- Version numbers above `u64` cannot exist on the console; skipping such folders loses nothing.
