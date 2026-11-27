# Plan 010: A damaged pipeline cache file is deleted instead of crashing the emulator at boot

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 07d884a918..HEAD -- src/video_core/shader_environment.cpp src/video_core/shader_environment.h src/video_core/renderer_vulkan/vk_pipeline_cache.cpp`
> If any of them changed since this plan was written, compare the "Current state" excerpts
> against the live code before proceeding; on a mismatch, treat it as a STOP condition.
> (`vk_pipeline_cache.cpp` changes often; only the `LoadUsedPipelines` function matters here.)

## Status

- **Priority**: P1
- **Effort**: S-M
- **Risk**: LOW
- **Depends on**: none
- **Category**: bug
- **Planned at**: commit `07d884a918`, 2026-11-19

## Why this matters

At every game start, Orchid reads its transferable pipeline cache (`vulkan.<suffix>.bin` in the
title's shader cache folder) with `VideoCommon::LoadPipelines`. Users also copy these files
between machines ("shader cache packs"), and a file can be cut off by a crash. The loader trusts
the sizes inside each record:

1. A record with `num_envs == 0` calls `envs.front()` on an empty vector (undefined behaviour).
2. `code_size` resizes a vector before anything checks it. A huge value throws `std::bad_alloc`,
   which the loader's `catch (const std::ios_base::failure&)` does not catch, so the loader thread
   terminates the process — on every boot, until the user finds and deletes the file.
3. `read_lowest`/`read_highest` bound `FileEnvironment::ReadInstruction`, but nothing ties them to
   `code.size()`, so a record can make the shader translator read past the code vector.
4. The separate "used pipelines" list reserves a hash map with a count from its file
   (`used.reserve(count)`), up to 4 billion buckets.

The loader already has the right failure path: on an `std::ios_base::failure` it logs and deletes
the file. After this plan malformed records take that path, and records this build writes load
exactly as before.

## Current state

`src/video_core/shader_environment.cpp`, `FileEnvironment::Deserialize` (lines 1145-1219). The
header part and the allocation:

```cpp
void FileEnvironment::Deserialize(std::ifstream& file) {
    u64 code_size{};
    u64 num_texture_types{};
    u64 num_texture_pixel_formats{};
    u64 num_cbuf_values{};
    u64 num_storage_pointer_types{};
    u64 num_cbuf_replacement_values{};
    file.read(reinterpret_cast<char*>(&code_size), sizeof(code_size))
        .read(reinterpret_cast<char*>(&num_texture_types), sizeof(num_texture_types))
        .read(reinterpret_cast<char*>(&num_texture_pixel_formats),
              sizeof(num_texture_pixel_formats))
        .read(reinterpret_cast<char*>(&num_cbuf_values), sizeof(num_cbuf_values))
        .read(reinterpret_cast<char*>(&num_storage_pointer_types),
              sizeof(num_storage_pointer_types))
        .read(reinterpret_cast<char*>(&num_cbuf_replacement_values),
              sizeof(num_cbuf_replacement_values))
        .read(reinterpret_cast<char*>(&local_memory_size), sizeof(local_memory_size))
        .read(reinterpret_cast<char*>(&texture_bound), sizeof(texture_bound))
        .read(reinterpret_cast<char*>(&surface_bound), sizeof(surface_bound))
        .read(reinterpret_cast<char*>(&start_address), sizeof(start_address))
        .read(reinterpret_cast<char*>(&read_lowest), sizeof(read_lowest))
        .read(reinterpret_cast<char*>(&read_highest), sizeof(read_highest))
        .read(reinterpret_cast<char*>(&viewport_transform_state), sizeof(viewport_transform_state))
        .read(reinterpret_cast<char*>(&stage), sizeof(stage));
    code.resize(Common::DivCeil(code_size, sizeof(u64)));
    file.read(reinterpret_cast<char*>(code.data()), code_size);
    // ... then num_* key/value pairs (each loop reads until the stream fails, so they are bounded
    // by the file), then for Compute: workgroup_size (3 x u32) and shared_memory_size (u32);
    // otherwise sph, and for Geometry gp_passthrough_mask.
```

Field types: `local_memory_size`, `texture_bound`, `surface_bound`, `read_lowest`, `read_highest`,
`viewport_transform_state` are `u32` (`shader_environment.h:300-315`); `start_address` is `u32`
and `stage` is `Shader::Stage` (a `u32` enum, `Compute == 5`; see `src/shader_recompiler/stage.h`)
in the base `Shader::Environment` (`src/shader_recompiler/environment.h:156-157`);
`workgroup_size` is `std::array<u32, 3>`, `shared_memory_size` is `u32`.

```cpp
// shader_environment.cpp:1250-1255
u64 FileEnvironment::ReadInstruction(u32 address) {
    if (address < read_lowest || address > read_highest) {
        throw Shader::LogicError("Out of bounds address {}", address);
    }
    return code[(address - read_lowest) / sizeof(u64)];
}
```

The writer (`GenericEnvironment::Serialize`, lines 269-300) stores `cached_lowest` and
`cached_highest` as `read_lowest`/`read_highest` and `code_size = CachedSizeBytes()`, where

```cpp
// shader_environment.cpp:246-248
size_t GenericEnvironment::CachedSizeBytes() const noexcept {
    return static_cast<size_t>(cached_highest) - cached_lowest + INST_SIZE;   // INST_SIZE == 8
}
```

So every record this build writes satisfies `code_size == read_highest - read_lowest + 8` with
`read_highest >= read_lowest`. (Native Switch 2 programs too: they set `cached_lowest = 0` and
`cached_highest = (code.size() - 1) * 8`, line 528-529.) That identity is the check to add.

```cpp
// shader_environment.cpp:1350-1405 (LoadPipelines, abridged)
void LoadPipelines(
    std::stop_token stop_loading, const std::filesystem::path& filename, u32 expected_cache_version,
    Common::UniqueFunction<void, std::ifstream&, FileEnvironment> load_compute,
    Common::UniqueFunction<void, std::ifstream&, std::vector<FileEnvironment>> load_graphics) try {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return;
    }
    file.exceptions(std::ifstream::failbit);
    const auto end{file.tellg()};
    file.seekg(0, std::ios::beg);

    std::array<char, 8> magic_number;
    u32 cache_version;
    file.read(magic_number.data(), magic_number.size())
        .read(reinterpret_cast<char*>(&cache_version), sizeof(cache_version));
    if (magic_number != MAGIC_NUMBER || cache_version != expected_cache_version) {
        // ... deletes the file and returns
    }
    while (file.tellg() != end) {
        if (stop_loading.stop_requested()) {
            return;
        }
        u32 num_envs{};
        file.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
        std::vector<FileEnvironment> envs(num_envs);
        for (FileEnvironment& env : envs) {
            env.Deserialize(file);
        }
        if (envs.front().ShaderStage() == Shader::Stage::Compute) {
            load_compute(file, std::move(envs.front()));
        } else {
            load_graphics(file, std::move(envs));
        }
    }

} catch (const std::ios_base::failure& e) {
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}
```

`MAGIC_NUMBER` is `{'y','u','z','u','c','a','c','h'}` (line 48). The callbacks read the pipeline
key that follows the environments. Writers pass at most `Maxwell::MaxShaderProgram` (6)
environments per graphics pipeline and 1 for compute (`vk_pipeline_cache.cpp:1720-1730`,
`gl_shader_cache.cpp:468,590`).

`src/video_core/renderer_vulkan/vk_pipeline_cache.cpp:769-790` (anonymous namespace):

```cpp
std::unordered_map<u64, u8> LoadUsedPipelines(const std::filesystem::path& path, u8 max_age) {
    std::unordered_map<u64, u8> used;
    std::ifstream file{path, std::ios::binary};
    u32 magic{}, count{};
    if (!file.read(reinterpret_cast<char*>(&magic), sizeof(magic)) || magic != UsedPipelinesMagic ||
        !file.read(reinterpret_cast<char*>(&count), sizeof(count))) {
        return used;
    }
    used.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        // reads hash + age per entry, breaks at the end of the file
```

A comment a few lines below says a cache "held 48k pipelines", so real lists stay well under
65536 entries.

Conventions: 4 spaces, about 100 columns, comments say why. Tests: Catch2 in `tests`, sources
listed in `src/tests/CMakeLists.txt`; video_core tests live in `src/tests/video_core/`. Temporary
files: `std::filesystem::temp_directory_path()` plus a unique name, removed at the end (pattern:
`struct TempDir` in `src/tests/core/nup_writer.cpp:35-46`).

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Configure a build with tests (once) | `./tools/build-local.sh --configure-only -DYUZU_TESTS=ON` | exit 0 |
| Build the tests | `cmake --build build-local --target tests --parallel 6` | exit 0 |
| Run the new tests | `./build-local/bin/tests "[pipeline_cache_file]"` | `All tests passed` |
| Whole suite | `./build-local/bin/tests` | `All tests passed` |
| GUI build (compiles `vk_pipeline_cache.cpp`) | `cmake --build build-local --target yuzu --parallel 6` | exit 0 |
| Mutation check | `orchid-agent-test --build-dir build-local --base HEAD -- "[pipeline_cache_file]"` | exit 0 |

Notes: add `-DCPM_SOURCE_CACHE=$HOME/src/Eglantine/.cache/cpm` to the configure command if
that folder exists; skip the configure if `build-local/CMakeCache.txt` already has
`YUZU_TESTS:BOOL=ON`. One full build at a time. If `orchid-agent-test` is missing, say so.

## Scope

**In scope**:

- `src/video_core/shader_environment.cpp` (`FileEnvironment::Deserialize`, `LoadPipelines`)
- `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp` (`LoadUsedPipelines` only)
- `src/tests/video_core/pipeline_cache_file.cpp` (create)
- `src/tests/CMakeLists.txt` (one added line)
- `plans/README.md` (status row)

**Out of scope**:

- `CACHE_VERSION` in `vk_pipeline_cache.cpp` / `gl_shader_cache.cpp`: the file format does not
  change, so existing caches stay valid. Do not bump it.
- The writer (`GenericEnvironment::Serialize`, `SerializePipeline`).
- The Vulkan driver cache (`vulkan_pipelines.*.bin`), which the driver validates itself.
- The `num_*` key/value loops: they read until the stream fails, which the existing catch handles.

## Git workflow

- One commit. Suggested subject: `shader cache: reject malformed pipeline cache records instead of
  crashing`. Body: the four problems, the identity used, the tests.
- Do NOT push unless the operator told you to, or you work under this repository's `AGENTS.md`
  rules.

## Steps

### Step 1: Tests

Create `src/tests/video_core/pipeline_cache_file.cpp`; add `    video_core/pipeline_cache_file.cpp`
to `add_executable(tests ...)` directly after `    video_core/shader_reuse_environment.cpp`.

Includes: `<array>`, `<chrono>`, `<cstring>`, `<filesystem>`, `<fstream>`, `<stop_token>`,
`<string>`, `<vector>`, `<catch2/catch_test_macros.hpp>`, `"shader_recompiler/stage.h"`,
`"video_core/shader_environment.h"`.

Helpers (anonymous namespace):

```cpp
namespace fs = std::filesystem;
constexpr u32 Version = 0x7e570001;

struct CacheFile {
    fs::path path = fs::temp_directory_path() /
                    ("pipeline-cache-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~CacheFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

template <typename T>
void Put(std::vector<char>& out, T value) {
    const auto* bytes = reinterpret_cast<const char*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

// One compute environment as FileEnvironment::Deserialize reads it, with no table entries.
std::vector<char> ComputeRecord(u64 code_size, u32 read_lowest, u32 read_highest,
                                const std::vector<u64>& code) {
    std::vector<char> out;
    Put<u32>(out, 1);              // num_envs
    Put<u64>(out, code_size);
    for (int i = 0; i < 5; ++i) {
        Put<u64>(out, 0);          // texture types, pixel formats, cbuf values, pointers, replacements
    }
    Put<u32>(out, 0);              // local_memory_size
    Put<u32>(out, 0);              // texture_bound
    Put<u32>(out, ~0U);            // surface_bound
    Put<u32>(out, read_lowest);    // start_address
    Put<u32>(out, read_lowest);
    Put<u32>(out, read_highest);
    Put<u32>(out, 1);              // viewport_transform_state
    Put<u32>(out, static_cast<u32>(Shader::Stage::Compute));
    for (const u64 word : code) {
        Put<u64>(out, word);
    }
    for (int i = 0; i < 4; ++i) {
        Put<u32>(out, 1);          // workgroup_size x, y, z and shared_memory_size
    }
    return out;
}

void Write(const fs::path& path, const std::vector<char>& record) {
    std::ofstream file{path, std::ios::binary};
    file.write("yuzucach", 8);
    const u32 version = Version;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(record.data(), static_cast<std::streamsize>(record.size()));
}

struct LoadResult {
    int compute = 0;
    int graphics = 0;
    u64 last_word = 0;
};

LoadResult Load(const fs::path& path) {
    LoadResult result;
    VideoCommon::LoadPipelines(
        std::stop_token{}, path, Version,
        [&result](std::ifstream&, VideoCommon::FileEnvironment env) {
            ++result.compute;
            result.last_word = env.ReadInstruction(0x18);
        },
        [&result](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) { ++result.graphics; });
    return result;
}
```

Test cases (tag `[video_core][pipeline_cache_file]`):

1. **"A pipeline cache record this build writes loads"** — `Write(t.path, ComputeRecord(16, 0x10,
   0x18, {0x1111, 0x2222}))`; `Load` gives `compute == 1`, `graphics == 0`, `last_word == 0x2222`,
   and `fs::exists(t.path)`.
2. **"A pipeline cache record with no environments deletes the file"** — a record that is only
   `Put<u32>(record, 0)`. `Load` gives `compute == 0`, `graphics == 0`, and
   `!fs::exists(t.path)`.
3. **"A pipeline cache record with a huge code size deletes the file"** —
   `ComputeRecord(u64{1} << 60, 0x10, 0x18, {0x1111, 0x2222})` (2^60 bytes: more than any address space, so the old code fails the allocation at once instead of touching memory). `REQUIRE_NOTHROW` around `Load`,
   then `compute == 0` and `!fs::exists(t.path)`.
4. **"A pipeline cache record whose read range exceeds its code deletes the file"** —
   `ComputeRecord(16, 0x10, 0x10000, {0x1111, 0x2222})`. `compute == 0` and `!fs::exists(t.path)`.
   (Do NOT call `ReadInstruction` past the code here; the callback must not run at all.)

Each test creates its own `CacheFile t;`.

**Verify**: build, `./build-local/bin/tests "[pipeline_cache_file]"` → test 1 passes; tests 3 and 4
fail (3 with an escaped `std::bad_alloc`, 4 because `compute == 1`). Test 2 is undefined behaviour
before the fix: it may fail or crash the test binary — both count as red. If it crashes, run the
others with `"[pipeline_cache_file]~*no environments*"` to see their results. If test 1 fails,
STOP.

### Step 2: Validate records in the reader

1. In `shader_environment.cpp`, near `MAGIC_NUMBER` / `INST_SIZE` (lines 48-50), add:

   ```cpp
   // Bounds for records read back from a pipeline cache file. A graphics pipeline writes at most
   // Maxwell's six programs; no Maxwell or native program comes near 64 MiB of code.
   constexpr u32 MaxCachedEnvironments = 6;
   constexpr u64 MaxCachedCodeSize = u64{64} << 20;
   ```

2. In `FileEnvironment::Deserialize`, directly before `code.resize(...)`:

   ```cpp
       // Every record the writer produces holds the code from its lowest to its highest read
       // address (CachedSizeBytes). Anything else is damaged and must not size the vector or
       // ReadInstruction's index; the failure makes LoadPipelines delete the file.
       if (read_highest < read_lowest || code_size > MaxCachedCodeSize ||
           code_size != u64{read_highest} - read_lowest + INST_SIZE) {
           throw std::ios_base::failure("Malformed shader record in pipeline cache");
       }
   ```

3. In `LoadPipelines`, directly after `file.read(... &num_envs ...)`:

   ```cpp
           if (num_envs == 0 || num_envs > MaxCachedEnvironments) {
               throw std::ios_base::failure("Malformed pipeline record in pipeline cache");
           }
   ```

   (`<ios>` comes in through `<fstream>`, already included.)

**Verify**: build, `./build-local/bin/tests "[pipeline_cache_file]"` → `All tests passed`
(4 test cases).

### Step 3: Cap the used-pipelines reservation

In `vk_pipeline_cache.cpp`, `LoadUsedPipelines`, replace `used.reserve(count);` with:

```cpp
    // The count comes from the file; the loop below stops at its end anyway.
    used.reserve((std::min)(count, u32{65536}));
```

(`<algorithm>` is already included there if `std::min` compiles; if not, add it.)

**Verify**: `cmake --build build-local --target yuzu --parallel 6` → exit 0;
`grep -n "used.reserve(count)" src/video_core/renderer_vulkan/vk_pipeline_cache.cpp` → no output.

### Step 4: Full suite and mutation check

**Verify**: `./build-local/bin/tests` → `All tests passed`;
`orchid-agent-test --build-dir build-local --base HEAD -- "[pipeline_cache_file]"` → exit 0,
score ≥ 80 %. A surviving mutant in `LoadUsedPipelines` (no unit test reaches that anonymous
function) must be named in the report with that reason.

## Test plan

`src/tests/video_core/pipeline_cache_file.cpp`, 4 cases, tag `[pipeline_cache_file]`: a valid
compute record round trip (including `ReadInstruction` at `read_highest`), zero environments, huge
code size, read range larger than the code. The `MaxCachedCodeSize` and `> MaxCachedEnvironments`
bounds are not tested separately (test 3 is caught by both the identity and the size cap); say so
if a mutant there survives.

## Done criteria

ALL must hold:

- [ ] `./build-local/bin/tests "[pipeline_cache_file]"` prints `All tests passed` with 4 test cases
- [ ] `./build-local/bin/tests` prints `All tests passed`
- [ ] `cmake --build build-local --target yuzu --parallel 6` exits 0
- [ ] `grep -c "Malformed" src/video_core/shader_environment.cpp` prints `2`
- [ ] `grep -n "CACHE_VERSION =" src/video_core/renderer_vulkan/vk_pipeline_cache.cpp` shows the
  same value as at the planned commit (`0x4e4f0008`)
- [ ] Mutation check exits 0 (or the report states the tool is not installed)
- [ ] `git status --short` lists only the in-scope files
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- Test 1 fails before step 2: the record layout in the helper does not match `Deserialize`. Re-read
  `Deserialize` once; if the layout differs from "Current state", STOP.
- After step 2 test 1 fails: the identity rejects a valid record. Do not loosen it; report.
- You find a writer that stores `read_lowest`/`read_highest` or `code_size` differently from
  `GenericEnvironment::Serialize` (search `rg -n "Serialize\(std::ofstream" src/video_core`).
- An optional emulator boot (below) logs `Malformed ... in pipeline cache` for a cache this same
  build wrote.

## Maintenance notes

- Optional check on the real thing: boot one game twice with the built `orchid` (the first boot
  writes the cache, the second reads it) and confirm the log has no `Malformed` line. Per this
  repository's `AGENTS.md`, every emulator run goes to the test subagent described there; do not
  start the emulator yourself outside that rule.
- If the record format ever changes (a new field), bump `CACHE_VERSION` as before; the identity
  check only depends on `code_size` and the read range.
- A reviewer should check that the thrown type is `std::ios_base::failure`: any other type escapes
  `LoadPipelines` and terminates the loader thread, which is the bug this plan removes.
