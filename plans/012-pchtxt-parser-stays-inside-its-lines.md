# Plan 012: The `.pchtxt` patch parser stays inside each line and never throws on a short one

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 07d884a918..HEAD -- src/core/file_sys/ips_layer.cpp src/core/file_sys/ips_layer.h`
> If either changed since this plan was written, compare the "Current state" excerpts against
> the live code before proceeding; on a mismatch, treat it as a STOP condition.

## Status

- **Priority**: P2
- **Effort**: S
- **Risk**: LOW
- **Depends on**: none
- **Category**: bug
- **Planned at**: commit `07d884a918`, 2026-11-19

## Why this matters

`.pchtxt` files are text patches for game executables that users download as mods. At boot,
`PatchManager` opens every `.pchtxt` in a mod's `exefs` folder with `IPSwitchCompiler` just to
read its build id (`patch_manager.cpp:421-426`). The parser has three bugs:

1. A line that starts with `@n` but is shorter than 8 characters makes `line.substr(8)` throw
   `std::out_of_range`. Nothing catches it, so one malformed mod file aborts the game start.
2. `@flag offset_shift` is read with `strtoll(line.data() + 19, ...)`. The line is a
   `std::string_view` into the file buffer, not a zero-terminated string: for a bare
   `@flag offset_shift` the pointer lands on the next line, whose digits then become the shift.
   At the end of the buffer it reads past the vector.
3. Patch lines are read with `strtoul(line.data(), ...)`, which also scans past the view (and past
   the buffer when the file ends with digits and no newline).

There are no tests for this parser. After this plan each line is parsed only within itself,
behaviour for well-formed files is unchanged, and tests pin it.

## Current state

`src/core/file_sys/ips_layer.cpp`. The constructor parses the whole file:

```cpp
// :114-116
IPSwitchCompiler::IPSwitchCompiler(VirtualFile patch_text_) : patch_text(std::move(patch_text_)) {
    Parse(patch_text->ReadAllBytes());
}
```

`Parse` (line 150) splits the bytes into lines, trims spaces, strips `/` and `#` comments outside
quotes, and calls `parse_line(std::string_view line)` for each non-empty line. The three sites:

```cpp
// :160-166
        if (line.size() > 2 && line[0] == '@') {
            switch (line[1]) {
            // yes, @nsobid too -- NSO Build ID Specifier
            case 'n':
            case 'N':
                nso_build_id = Common::HexStringToArray<0x20>(fmt::format("{:0<64}", line.substr(8)));
                break;

// :181-189
            case 'f':
            case 'F': {
                if (line.starts_with("@flag offset_shift")) {
                    offset_shift = std::strtoll(line.data() + 19, nullptr, 0);  // Offset Shift Flag
                } else if (line.starts_with("@flag print_values")) {
                    //print_values = true; // Force printing of applied values
                }
                break;
            }

// :204-208
        } else if (patches.empty()) {
            LOG_WARNING(Loader, "Invalid line not in a patch {}", line);
        } else {
            size_t offset = size_t(std::strtoul(line.data(), nullptr, 16));
            offset += size_t(offset_shift);
```

`"@nsobid-"` is 8 characters, `"@flag offset_shift"` is 18. A string replacement line looks like
`00000002 "hi"` and writes the bytes `h`, `i` at offset 2 + shift. `Apply(in)` (line ~290) copies
`in`'s bytes, writes every record of every `@enabled` patch, and returns a new `VectorVfsFile`.
`GetBuildID()` returns `nso_build_id` (`std::array<u8, 0x20>`).

Conventions: 4 spaces, about 100 columns, comments say why. Tests: Catch2 in `tests`, sources
listed by hand in `src/tests/CMakeLists.txt`. Exemplar layout: `src/tests/core/vfs_vector.cpp`.
`FileSys::VectorVfsFile` (`core/file_sys/vfs/vfs_vector.h`) wraps a `std::vector<u8>`.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Configure a build with tests (once) | `./tools/build-local.sh --configure-only -DYUZU_TESTS=ON` | exit 0 |
| Build the tests | `cmake --build build-local --target tests --parallel 6` | exit 0 |
| New tests | `./build-local/bin/tests "[ipswitch]"` | `All tests passed` |
| Whole suite | `./build-local/bin/tests` | `All tests passed` |
| Mutation check | `orchid-agent-test --build-dir build-local --base HEAD -- "[ipswitch]"` | exit 0 |

Notes: add `-DCPM_SOURCE_CACHE=/home/leon/src/Eglantine/.cache/cpm` to the configure command if
that folder exists; skip the configure if `build-local/CMakeCache.txt` already has
`YUZU_TESTS:BOOL=ON`. One full build at a time. If `orchid-agent-test` is missing, say so.

## Scope

**In scope**:

- `src/core/file_sys/ips_layer.cpp` (the three sites above only)
- `src/tests/core/ips_switch.cpp` (create)
- `src/tests/CMakeLists.txt` (one added line)
- `plans/README.md` (status row)

**Out of scope**:

- The line splitter and comment stripping in `Parse`, `EscapeStringSequences`, the hex
  replacement branch, the binary IPS/IPS32 patcher (`PatchIPS`) in the same file.
- `patch_manager.cpp`: with the parser no longer throwing, the caller needs no `try`.

## Git workflow

- One commit. Suggested subject: `file_sys: parse pchtxt numbers within their line`.
- Do NOT push unless the operator told you to, or you work under this repository's `AGENTS.md`
  rules.

## Steps

### Step 1: Tests (two fail before the fix)

Create `src/tests/core/ips_switch.cpp`; add `    core/ips_switch.cpp` to `add_executable(tests ...)`
directly after `    core/vfs_vector.cpp`.

```cpp
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/ips_layer.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {
FileSys::VirtualFile Text(std::string_view text) {
    return std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(text.begin(), text.end()),
                                                    "patch.pchtxt");
}

std::vector<u8> Patched(const FileSys::IPSwitchCompiler& compiler) {
    return compiler.Apply(std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(16, 0)))
        ->ReadAllBytes();
}
} // namespace
```

Test cases (tag `[core][ipswitch]`):

1. **"A pchtxt patch applies its build id, shift and string record"** —
   `Text("@nsobid-0123\n@flag offset_shift 0x4\n@enabled\n00000002 \"hi\"\n")`. Expect
   `GetBuildID()[0] == 0x01`, `[1] == 0x23`, `[2] == 0`; `Patched(...)[6] == 'h'`, `[7] == 'i'`, and
   bytes 0-5 and 8-15 are zero. (Passes before the fix; guards the happy path.)
2. **"A short @n line does not throw"** — `REQUIRE_NOTHROW(FileSys::IPSwitchCompiler{Text("@nsob\n")})`;
   then a compiler from the same text has an all-zero build id. (Fails before the fix:
   `std::out_of_range`.)
3. **"A bare offset_shift flag does not read the next line"** —
   `Text("@enabled\n@flag offset_shift\n00000002 \"hi\"\n")`. Expect `Patched(...)[2] == 'h'`,
   `[3] == 'i'`. (Fails before the fix: the old code reads the next line's `00000002` as an octal
   shift of 2 and writes at 4.)
4. **"A patch line at the end of the file without a newline is applied"** —
   `Text("@enabled\n00000003 \"x\"")`. Expect `Patched(...)[3] == 'x'`. (Passes before the fix,
   since the quote stops `strtoul`; it pins the end-of-buffer case for step 2.)

**Verify**: build, `./build-local/bin/tests "[ipswitch]"` → tests 1 and 4 pass, tests 2 and 3 fail.

### Step 2: Parse within the line

Edit the three sites:

```cpp
            case 'n':
            case 'N':
                // "@nsobid-" is 8 characters; a shorter line has no id.
                nso_build_id = Common::HexStringToArray<0x20>(
                    fmt::format("{:0<64}", line.size() > 8 ? line.substr(8) : std::string_view{}));
                break;
```

```cpp
                if (line.starts_with("@flag offset_shift")) {
                    // The line is a view into the file: copy it so strtoll stops at its end.
                    offset_shift = std::strtoll(std::string{line.substr(18)}.c_str(), nullptr, 0);
                }
```

(Keep the `else if (line.starts_with("@flag print_values"))` branch and its comment as they are.)

```cpp
            size_t offset = size_t(std::strtoul(std::string{line}.c_str(), nullptr, 16));
```

`strtoll` skips the leading space of `" 0x4"`, so `line.substr(18)` gives the same value as the
old `line.data() + 19` for well-formed lines.

**Verify**: build, `./build-local/bin/tests "[ipswitch]"` → `All tests passed` (4 cases).

### Step 3: Full suite and mutation check

**Verify**: `./build-local/bin/tests` → `All tests passed`;
`orchid-agent-test --build-dir build-local --base HEAD -- "[ipswitch]"` → exit 0, score ≥ 80 %.
A mutant that turns `line.size() > 8` into `>= 8` is equivalent (an 8-character line yields an
empty id either way); name it in the report if it survives, do not add a test for it.

## Test plan

`src/tests/core/ips_switch.cpp`, 4 cases, tag `[ipswitch]`: build id + shift + string record, a
short `@n` line, a bare `offset_shift`, a last line without newline.

## Done criteria

ALL must hold:

- [ ] `./build-local/bin/tests "[ipswitch]"` prints `All tests passed` with 4 test cases
- [ ] `./build-local/bin/tests` prints `All tests passed`
- [ ] `grep -nE "line\.data\(\) \+ 19|strtoul\(line\.data\(\)" src/core/file_sys/ips_layer.cpp` prints nothing
- [ ] Mutation check exits 0 (or the report states the tool is not installed)
- [ ] `git status --short` lists only the in-scope files
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- Test 1 or 4 fails before step 2, or test 2 or 3 passes before step 2.
- After step 2 any other test fails.
- The build id bytes in test 1 come out reversed or shifted: then `HexStringToArray` behaves
  differently than assumed; report the bytes instead of changing the expectation.

## Maintenance notes

- Copying a line into a `std::string` costs one small allocation per patch line; patch files have
  tens of lines.
- `std::from_chars` would avoid the copy but does not accept the `0x` prefix that base-0/base-16
  `strto*` accept, and existing mod files use it. Keep `strto*`.
- The same file's binary IPS path was not reviewed for this plan.
