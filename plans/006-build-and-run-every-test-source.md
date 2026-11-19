# Plan 006: Every test source in `src/tests` is built and run, and a forgotten one fails the configure

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 07d884a918..HEAD -- src/tests/CMakeLists.txt src/tests/video_core/maxwell_fmz.cpp src/tests/debug_stream_contract.cpp`
> If any in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding; on a
> mismatch, treat it as a STOP condition.

## Status

- **Priority**: P1
- **Effort**: S
- **Risk**: LOW
- **Depends on**: none
- **Category**: tests
- **Planned at**: commit `07d884a918`, 2026-11-19

## Why this matters

The unit tests are one Catch2 executable, `tests`, whose source files are listed by hand in
`src/tests/CMakeLists.txt`. Two test files are in no list, so they were never compiled or run:

- `src/tests/video_core/maxwell_fmz.cpp` (259 lines, four `TEST_CASE`s) was added by commit
  `9ee986d3e5` ("shader: apply FMZ before SAT in FFMA and HFMA2, and translate half FMZ") as the
  proof of that shader fix. The fix has never been checked by its own tests.
- `src/tests/debug_stream_contract.cpp` is a standalone program with its own `main()` for the
  frame telemetry and texture-handle trace streams (commit `6bdcbc810b`). Its header comment says
  to compile it by hand; nobody does.

After this plan both run, and a CMake check makes the next forgotten file a configure error
instead of a silent gap.

## Current state

`src/tests/CMakeLists.txt` (191 lines). The `tests` target lists sources one per line; this is
the area around where the FMZ file belongs:

```cmake
# src/tests/CMakeLists.txt:135-137
    video_core/ampere_yoshi_cbuf_carry.cpp
    video_core/maxwell_shr.cpp
    video_core/ampere_release_guards.cpp
```

A Windows-only file is added separately, and a second, standalone test executable already exists.
Use the standalone one as the pattern for `debug_stream_contract`:

```cmake
# src/tests/CMakeLists.txt:162-182
create_target_directory_groups(tests)

if (WIN32)
    target_sources(tests PRIVATE common/host_memory_commit.cpp)
endif()

target_link_libraries(tests PRIVATE common core input_common video_core)
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)

# Reference texts of the expect tests (tests/expect.h); ORCHID_EXPECT_UPDATE=1 rewrites them.
target_compile_definitions(tests PRIVATE ORCHID_EXPECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/expect")

add_test(NAME tests COMMAND tests)

# Separate processes exercise the cached opt-in and destruction order without a GPU or profile.
add_executable(stall_diagnostics_session
    video_core/stall_diagnostics_session.cpp
    ../video_core/stall_diagnostics.cpp)
target_link_libraries(stall_diagnostics_session PRIVATE common ${PLATFORM_LIBRARIES} Threads::Threads)
add_test(NAME stall_diagnostics_disabled COMMAND ${CMAKE_COMMAND} -E env
    --unset=EDEN_NX2_STALL_DIAGNOSTICS $<TARGET_FILE:stall_diagnostics_session> disabled)
add_test(NAME stall_diagnostics_enabled COMMAND ${CMAKE_COMMAND} -E env
    EDEN_NX2_STALL_DIAGNOSTICS=1 $<TARGET_FILE:stall_diagnostics_session> enabled)
```

The file ends with a `if (NOT MSVC) target_compile_options(tests ...) endif()` block.

`src/tests/debug_stream_contract.cpp` usage, from its header and `main`:

```cpp
// src/tests/debug_stream_contract.cpp:4-6
// Standalone contract test, no emulator/GPU needed:
// c++ -std=c++20 -Isrc src/tests/debug_stream_contract.cpp -o <test>
// <test> <new-empty-test-directory> [disabled]

// :77-81
int main(int argc, char** argv) {
    Require(argc >= 2);
    const std::filesystem::path root{argv[1]};
    Require(!std::filesystem::exists(root));   // the directory must NOT exist yet
    if (argc > 2 && std::string{argv[2]} == "disabled") {
```

`Require` throws `std::runtime_error` on failure, so a failed check exits non-zero (uncaught
exception). In the "enabled" mode it creates the directory itself and writes files into it, so a
second run with the same path fails `Require(!exists(root))`. It only includes headers
(`core/frame_telemetry.h`, `video_core/texture_handle_trace.h`, both header-only and depending on
`common/common_types.h`), so linking `common` is enough, as for `stall_diagnostics_session`.

`src/tests/video_core/maxwell_fmz.cpp` is a normal Catch2 file (tags `[shader][maxwell][fmz]`)
that includes `shader_recompiler/frontend/maxwell/translate/impl/impl.h` and drives
`Shader::Maxwell::TranslatorVisitor` directly, like the listed `video_core/maxwell_shr.cpp`.

Orphans found while planning (the loop in step 3 reproduces this list):

```
debug_stream_contract.cpp
video_core/maxwell_fmz.cpp
```

The CI workflow `.github/workflows/tests.yml` builds only `--target tests dynarmic_tests` and runs
`tests.exe`; it does not run `ctest`. That is a documented manual workflow; do not change it.

## Commands you will need

Run all commands from the repository root.

| Purpose | Command | Expected on success |
|---|---|---|
| Configure a build with tests (once) | `./tools/build-local.sh --configure-only -DYUZU_TESTS=ON` | exit 0, `build-local/CMakeCache.txt` exists |
| Re-run the configure step only | `cmake build-local` | exit 0 |
| Build the test targets | `cmake --build build-local --target tests debug_stream_contract --parallel 6` | exit 0 |
| Run the FMZ tests | `./build-local/bin/tests "[fmz]"` | `All tests passed` (4 test cases) |
| Run the whole suite | `./build-local/bin/tests` | `All tests passed` |
| Run the contract test | `ctest --test-dir build-local -R debug_stream_contract --output-on-failure` | `100% tests passed`, 3 tests |
| Mutation check (required by `AGENTS.md`) | see step 5 | exit 0 |

Notes:

- If `/home/leon/src/Eglantine/.cache/cpm` exists, add
  `-DCPM_SOURCE_CACHE=/home/leon/src/Eglantine/.cache/cpm` to the first configure command.
- If `build-local` already exists and was configured with `-DYUZU_TESTS=ON`
  (`grep YUZU_TESTS build-local/CMakeCache.txt` shows `ON`), skip the configure line.
- Only one full build at a time on a machine. Do not delete `build-local`.
- `orchid-agent-test` is a local tool of the maintainer. If it does not exist on your machine,
  say so in your report; do not invent a replacement.

## Scope

**In scope** (the only files you should modify):

- `src/tests/CMakeLists.txt`
- `src/tests/video_core/maxwell_fmz.cpp` — only if it fails to compile because an include path or
  a renamed identifier moved (see STOP conditions)
- `plans/README.md` (status row)

**Out of scope** (do NOT touch, even though they look related):

- Any file under `src/shader_recompiler/`, `src/core/`, `src/video_core/`: if an FMZ test fails,
  that is a finding to report, not something to fix here.
- `src/tests/debug_stream_contract.cpp`: register it as it is.
- `src/tests/video_core/unmapped_trace_integration.py`: a Python driver documented in
  `NX2_UNMAPPED_READ_TRACE.md`; wiring Python into CTest is a separate decision.
- `.github/workflows/*.yml`.

## Git workflow

- Work in the checkout you were given. Do not create extra worktrees.
- One commit for the whole plan. Message style: area first, then a body that says what was wrong
  and how it was checked. Example from the log: `tests: add expect tests, starting with the Kirby
  FSEL lowering IR`. Suggested subject: `tests: build the FMZ tests and the debug stream contract,
  fail on unlisted test files`.
- Do NOT push unless the operator told you to, or you work under this repository's `AGENTS.md`
  rules, which define when a push to `origin/main` is allowed.

## Steps

### Step 1: Add the FMZ test file to the `tests` target

Insert `    video_core/maxwell_fmz.cpp` as its own line directly after `    video_core/maxwell_shr.cpp`
in the `add_executable(tests ...)` list.

**Verify**: `cmake --build build-local --target tests --parallel 6` → exit 0, then
`./build-local/bin/tests "[fmz]"` → `All tests passed (... assertions in 4 test cases)`.

If the build fails inside `maxwell_fmz.cpp`, read the error. Only an include that moved or an
identifier that was renamed since `9ee986d3e5` may be fixed in the test file (check with
`git log --oneline 9ee986d3e5..HEAD -- <the header>`). Anything else is a STOP condition.

### Step 2: Register `debug_stream_contract` as a standalone CTest program

After the `stall_diagnostics_*` block, add an executable and three CTest entries. The "enabled"
run needs a directory that does not exist, so a setup test removes it first (CTest fixture):

```cmake
# Standalone: the frame telemetry and handle trace streams read their switches once per process.
add_executable(debug_stream_contract debug_stream_contract.cpp)
target_link_libraries(debug_stream_contract PRIVATE common ${PLATFORM_LIBRARIES} Threads::Threads)
set(debug_stream_contract_dir "${CMAKE_CURRENT_BINARY_DIR}/debug_stream_contract")
add_test(NAME debug_stream_contract_clean
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${debug_stream_contract_dir}")
set_tests_properties(debug_stream_contract_clean PROPERTIES FIXTURES_SETUP debug_stream_contract_dir)
add_test(NAME debug_stream_contract_disabled
    COMMAND debug_stream_contract "${debug_stream_contract_dir}" disabled)
add_test(NAME debug_stream_contract_enabled
    COMMAND debug_stream_contract "${debug_stream_contract_dir}")
set_tests_properties(debug_stream_contract_disabled debug_stream_contract_enabled
    PROPERTIES FIXTURES_REQUIRED debug_stream_contract_dir)
# The disabled run checks that nothing is created, so it runs before the enabled one.
set_tests_properties(debug_stream_contract_enabled PROPERTIES DEPENDS debug_stream_contract_disabled)
```

**Verify**: `cmake build-local && cmake --build build-local --target debug_stream_contract --parallel 6`
→ exit 0; then `ctest --test-dir build-local -R debug_stream_contract --output-on-failure` →
`100% tests passed, 0 tests failed out of 3`. Run the `ctest` line a second time → same result
(the clean fixture makes it repeatable).

### Step 3: Fail the configure when a test source is in no target

At the very end of `src/tests/CMakeLists.txt` (after the `if (NOT MSVC)` block, so every target
exists), add:

```cmake
# A test source in no target is never built or run (maxwell_fmz.cpp and debug_stream_contract.cpp
# sat unbuilt this way). Every .cpp here must belong to one of the targets above.
file(GLOB_RECURSE test_sources RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS "*.cpp")
set(listed_test_sources common/host_memory_commit.cpp) # added above on Windows only
foreach(test_target IN ITEMS tests stall_diagnostics_session debug_stream_contract)
    get_target_property(target_sources ${test_target} SOURCES)
    list(APPEND listed_test_sources ${target_sources})
endforeach()
foreach(test_source IN LISTS test_sources)
    if (NOT test_source IN_LIST listed_test_sources)
        message(FATAL_ERROR "src/tests/${test_source} belongs to no test target; list it in src/tests/CMakeLists.txt")
    endif()
endforeach()
```

**Verify** (the check passes now): `cmake build-local` → exit 0, no `FATAL_ERROR`.

**Verify** (the check catches an orphan): `touch src/tests/common/orphan_check_probe.cpp && cmake build-local; echo "exit=$?"`
→ output contains `src/tests/common/orphan_check_probe.cpp belongs to no test target` and
`exit=1`. Then `rm src/tests/common/orphan_check_probe.cpp && cmake build-local` → exit 0.

### Step 4: Run the whole suite

**Verify**: `cmake --build build-local --target tests debug_stream_contract --parallel 6` → exit 0;
`./build-local/bin/tests` → `All tests passed`.

### Step 5: Mutation check

This is a test-only change, so name the production line the new tests guard: the FMZ select in
FFMA.

```sh
orchid-agent-test --build-dir build-local \
  --line src/shader_recompiler/frontend/maxwell/translate/impl/floating_point_fused_multiply_add.cpp:38 \
  -- "[fmz]"
```

Line 38 of that file at the planned commit is
`        value = IR::F32{v.ir.Select(any_zero, op_c, value)};`. If the line number moved, use the
line with that text (`grep -n "Select(any_zero, op_c, value)" <file>`).

**Verify**: exit 0, score ≥ 80 %. Report the report path it prints.

## Test plan

- No new test code. The plan makes four existing FMZ `TEST_CASE`s and one standalone contract
  program run (3 CTest entries), and adds a configure-time guard proven by the probe in step 3.

## Done criteria

ALL must hold:

- [ ] `./build-local/bin/tests "[fmz]"` prints `All tests passed` with 4 test cases
- [ ] `./build-local/bin/tests` prints `All tests passed`
- [ ] `ctest --test-dir build-local -R debug_stream_contract` reports 3 of 3 passed, twice in a row
- [ ] `cmake build-local` exits 0; with a probe `.cpp` added it fails naming the probe (step 3)
- [ ] `grep -c "maxwell_fmz.cpp" src/tests/CMakeLists.txt` prints `1`
- [ ] The mutation check of step 5 exits 0 (or the report states the tool is not installed)
- [ ] `git status --short` lists only `src/tests/CMakeLists.txt`, `plans/README.md` and, only if
  step 1 needed it, `src/tests/video_core/maxwell_fmz.cpp`
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- An FMZ test fails after it compiles. That means the FMZ translation in
  `src/shader_recompiler/frontend/maxwell/translate/impl/` does not do what its commit claims;
  report the failing test names and the `REQUIRE` output. Do not change shader code.
- `maxwell_fmz.cpp` fails to compile for any reason other than a moved include or a renamed
  identifier.
- `debug_stream_contract` fails a `Require` (exit code non-zero with
  `debug stream contract failed`): report which run (disabled/enabled) failed.
- The orphan check in step 3 reports a file other than the probe. Then another test file is
  unlisted; report its name instead of adding it blindly.
- The build fails twice after a reasonable fix attempt.

## Maintenance notes

- New test files must now be listed, or the configure stops with a message that names them. A
  file that is meant to stay unbuilt (a sample, a fixture) should not end in `.cpp` under
  `src/tests`.
- `CONFIGURE_DEPENDS` makes Ninja re-glob `src/tests` on every build; with about 150 files that
  costs nothing measurable.
- The CI workflow still runs only the `tests` executable. If the maintainer wants the contract
  program in CI, `tests.yml` needs `--target debug_stream_contract` and a `ctest -R
  debug_stream_contract` step; left out on purpose (the workflow is the maintainer's decision).
- `src/tests/video_core/unmapped_trace_integration.py` is still run by hand only.
