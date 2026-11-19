# Plan 009: RomFS extraction always terminates, and a damaged table fails instead of hanging

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 07d884a918..HEAD -- src/core/file_sys/romfs.cpp`
> If it changed since this plan was written, compare the "Current state" excerpts against the
> live code before proceeding; on a mismatch, treat it as a STOP condition.
> (`src/tests/CMakeLists.txt` may have changed through other plans; only check that the line you
> insert after still exists.)

## Status

- **Priority**: P1
- **Effort**: S-M
- **Risk**: LOW
- **Depends on**: none
- **Category**: bug
- **Planned at**: commit `07d884a918`, 2026-11-19

## Why this matters

`FileSys::ExtractRomFS` turns a RomFS image (the game data of every title, update and many mods)
into a directory tree. It follows `sibling` / `child_dir` / `child_file` offsets read from the
image. Three things go wrong on a damaged or crafted image:

1. When an offset points outside its table, `GetEntry` returns a **zero-filled** entry. Zero is a
   valid offset (the root), not the "no entry" marker `0xFFFFFFFF`, so the walk continues at
   offset 0 forever: `ProcessFile` adds files until memory runs out, and `ProcessDirectory`
   recurses until the stack overflows. A RomFS whose table is merely truncated triggers this.
2. A valid offset that points back to an earlier entry (a cycle) loops forever too.
3. The two table sizes from the header size a `ReadBytes` that allocates the whole size first;
   a huge value throws `std::bad_alloc` out of the loader.

After this plan an unreadable entry ends its chain, a visit budget ends cycles, directory depth is
bounded, and table locations are checked against the file before reading.

## Current state

`src/core/file_sys/romfs.cpp` (about 170 lines; anonymous namespace inside `namespace FileSys`).

```cpp
// romfs.cpp:23
constexpr u32 ROMFS_ENTRY_EMPTY = 0xFFFFFFFF;

// :25-58 (on-disk layouts)
struct TableLocation { u64_le offset; u64_le size; };                  // 0x10
struct RomFSHeader {
    u64_le header_size;            // +0x00, must be 0x50
    TableLocation directory_hash;  // +0x08
    TableLocation directory_meta;  // +0x18 offset, +0x20 size
    TableLocation file_hash;       // +0x28
    TableLocation file_meta;       // +0x38 offset, +0x40 size
    u64_le data_offset;            // +0x48
};
struct DirectoryEntry {   // 0x18 bytes, then name_length bytes of name
    u32_le parent;        // +0x00
    u32_le sibling;       // +0x04
    u32_le child_dir;     // +0x08
    u32_le child_file;    // +0x0C
    u32_le hash;          // +0x10
    u32_le name_length;   // +0x14
};
struct FileEntry {        // 0x20 bytes, then the name
    u32_le parent;        // +0x00
    u32_le sibling;       // +0x04
    u64_le offset;        // +0x08
    u64_le size;          // +0x10
    u32_le hash;          // +0x18
    u32_le name_length;   // +0x1C
};
```

(The `static_assert`s in the file confirm these sizes; check them if in doubt.)

```cpp
// romfs.cpp:60-127
struct RomFSTraversalContext {
    RomFSHeader header;
    VirtualFile file;
    std::vector<u8> directory_meta;
    std::vector<u8> file_meta;
};

template <typename EntryType, auto Member>
std::pair<EntryType, std::string> GetEntry(const RomFSTraversalContext& ctx, size_t offset) {
    const size_t entry_end = offset + sizeof(EntryType);
    const std::vector<u8>& vec = ctx.*Member;
    const size_t size = vec.size();
    const u8* data = vec.data();
    EntryType entry{};

    if (entry_end > size) {
        return {};
    }
    std::memcpy(&entry, data + offset, sizeof(EntryType));

    const size_t name_length = (std::min)(entry_end + entry.name_length, size) - entry_end;
    std::string name(reinterpret_cast<const char*>(data + entry_end), name_length);

    return {entry, std::move(name)};
}

std::pair<DirectoryEntry, std::string> GetDirectoryEntry(const RomFSTraversalContext& ctx,
                                                         size_t directory_offset) {
    return GetEntry<DirectoryEntry, &RomFSTraversalContext::directory_meta>(ctx, directory_offset);
}

std::pair<FileEntry, std::string> GetFileEntry(const RomFSTraversalContext& ctx,
                                               size_t file_offset) {
    return GetEntry<FileEntry, &RomFSTraversalContext::file_meta>(ctx, file_offset);
}

void ProcessFile(const RomFSTraversalContext& ctx, u32 this_file_offset,
                 std::shared_ptr<VectorVfsDirectory>& parent) {
    while (this_file_offset != ROMFS_ENTRY_EMPTY) {
        auto entry = GetFileEntry(ctx, this_file_offset);

        parent->AddFile(std::make_shared<OffsetVfsFile>(ctx.file, entry.first.size,
                                                        entry.first.offset + ctx.header.data_offset,
                                                        std::move(entry.second)));

        this_file_offset = entry.first.sibling;
    }
}

void ProcessDirectory(const RomFSTraversalContext& ctx, u32 this_dir_offset,
                      std::shared_ptr<VectorVfsDirectory>& parent) {
    while (this_dir_offset != ROMFS_ENTRY_EMPTY) {
        auto entry = GetDirectoryEntry(ctx, this_dir_offset);
        auto current = std::make_shared<VectorVfsDirectory>(
            std::vector<VirtualFile>{}, std::vector<VirtualDir>{}, entry.second);

        if (entry.first.child_file != ROMFS_ENTRY_EMPTY) {
            ProcessFile(ctx, entry.first.child_file, current);
        }

        if (entry.first.child_dir != ROMFS_ENTRY_EMPTY) {
            ProcessDirectory(ctx, entry.first.child_dir, current);
        }

        parent->AddDirectory(current);
        this_dir_offset = entry.first.sibling;
    }
}
} // Anonymous namespace

// romfs.cpp:130-159
VirtualDir ExtractRomFS(VirtualFile file) {
    auto root_container = std::make_shared<VectorVfsDirectory>();
    if (!file) {
        return root_container;
    }

    RomFSTraversalContext ctx{};

    if (file->ReadObject(&ctx.header) != sizeof(RomFSHeader)) {
        return nullptr;
    }

    if (ctx.header.header_size != sizeof(RomFSHeader)) {
        return nullptr;
    }

    ctx.file = file;
    ctx.directory_meta =
        file->ReadBytes(ctx.header.directory_meta.size, ctx.header.directory_meta.offset);
    ctx.file_meta = file->ReadBytes(ctx.header.file_meta.size, ctx.header.file_meta.offset);

    ProcessDirectory(ctx, 0, root_container);

    if (auto root = root_container->GetSubdirectory(""); root) {
        return root;
    }

    ASSERT(false);
    return nullptr;
}
```

`VfsFile::ReadBytes` (`src/core/file_sys/vfs/vfs.cpp:178-183`) allocates `size` bytes before it
reads. `CreateRomFS(VirtualDir dir, VirtualDir ext = nullptr)` (same file, end) builds a valid
image from a directory tree; the tests use it to get a real image.

Conventions: 4 spaces, about 100 columns, braces on the same line, comments say why. Catch2 tests
in one `tests` executable, sources listed in `src/tests/CMakeLists.txt`. Exemplar test file
layout: `src/tests/core/vfs_vector.cpp`.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Configure a build with tests (once) | `./tools/build-local.sh --configure-only -DYUZU_TESTS=ON` | exit 0 |
| Build the tests | `cmake --build build-local --target tests --parallel 6` | exit 0 |
| Run the new tests | `./build-local/bin/tests "[romfs]"` | `All tests passed` |
| Tests that extract real RomFS images | `./build-local/bin/tests "[loose_directory],[nup]"` | `All tests passed` |
| Whole suite | `./build-local/bin/tests` | `All tests passed` |
| Mutation check | `orchid-agent-test --build-dir build-local --base HEAD -- "[romfs]"` | exit 0 |

Notes: add `-DCPM_SOURCE_CACHE=/home/leon/src/Eglantine/.cache/cpm` to the configure command if
that folder exists; skip the configure if `build-local/CMakeCache.txt` already has
`YUZU_TESTS:BOOL=ON`. One full build at a time. If `orchid-agent-test` is missing, say so.

## Scope

**In scope**:

- `src/core/file_sys/romfs.cpp`
- `src/tests/core/romfs.cpp` (create)
- `src/tests/CMakeLists.txt` (one added line)
- `plans/README.md` (status row)

**Out of scope**:

- `CreateRomFS` and `src/core/file_sys/fsmitm_romfsbuild.*` (the writer).
- `entry.offset + data_offset` past the file: `OffsetVfsFile` clamps reads, no memory error.
- `src/core/file_sys/romfs_factory.cpp`, `patch_manager.cpp`: callers; they handle `nullptr`.
- The `ASSERT(false)` at the end of `ExtractRomFS`: it stays for the case that cannot happen
  after step 2 (root entry readable but not added).

## Git workflow

- One commit. Suggested subject: `file_sys: end RomFS walks at unreadable entries and cycles`.
- Do NOT push unless the operator told you to, or you work under this repository's `AGENTS.md`
  rules.

## Steps

### Step 1: Tests for the happy path and the oversized table

Create `src/tests/core/romfs.cpp`; add `    core/romfs.cpp` to `add_executable(tests ...)` in
`src/tests/CMakeLists.txt` after `    core/vfs_real.cpp` (or after the line another plan inserted
there).

Includes: `<cstring>`, `<memory>`, `<string>`, `<vector>`, `<catch2/catch_test_macros.hpp>`,
`"core/file_sys/romfs.h"`, `"core/file_sys/vfs/vfs_vector.h"`.

Helpers in the anonymous namespace:

```cpp
using FileSys::VectorVfsDirectory;
using FileSys::VectorVfsFile;

// A real image from the writer: "a.txt" ("hi") at the root and "s/b.txt" ("yo").
std::vector<u8> Image() {
    const auto file = [](const char* name, const char* text) -> FileSys::VirtualFile {
        return std::make_shared<VectorVfsFile>(std::vector<u8>(text, text + 2), name);
    };
    const auto sub = std::make_shared<VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{file("b.txt", "yo")}, std::vector<FileSys::VirtualDir>{},
        "s");
    const auto root = std::make_shared<VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{file("a.txt", "hi")},
        std::vector<FileSys::VirtualDir>{sub}, "");
    return FileSys::CreateRomFS(root)->ReadAllBytes();
}

u64 Read64(const std::vector<u8>& image, size_t at) {
    u64 value;
    std::memcpy(&value, image.data() + at, sizeof(value));
    return value;
}

template <typename T>
void Write(std::vector<u8>& image, size_t at, T value) {
    std::memcpy(image.data() + at, &value, sizeof(value));
}

FileSys::VirtualDir Extract(const std::vector<u8>& image) {
    return FileSys::ExtractRomFS(std::make_shared<VectorVfsFile>(image));
}
```

Test cases (tag `[core][romfs]`):

1. **"A RomFS image from the writer extracts its files and folders"** — `Extract(Image())` is not
   null; `GetFile("a.txt")` reads `"hi"`; `GetSubdirectory("s")->GetFile("b.txt")` reads `"yo"`.
2. **"A RomFS whose table lies past the end of the image is rejected"** — `auto image = Image();
   Write(image, 0x40, u64{1} << 62);` (file_meta size). `CHECK(Extract(image) == nullptr)`.

**Verify**: build, `./build-local/bin/tests "[romfs]"` → test 1 passes; test 2 FAILS because
`ExtractRomFS` throws (`std::bad_alloc` from the 2^62-byte `ReadBytes`). If test 1 fails, STOP.

### Step 2: Check tables, end chains at unreadable entries, budget visits, bound depth

In `romfs.cpp`:

1. Add `#include <optional>`.
2. Extend the context with visit budgets and add a depth limit:

   ```cpp
   struct RomFSTraversalContext {
       RomFSHeader header;
       VirtualFile file;
       std::vector<u8> directory_meta;
       std::vector<u8> file_meta;
       // Entries left to visit. A valid table visits each entry once, so a link cycle runs out.
       size_t directories_left{};
       size_t files_left{};
   };

   // A Switch path holds at most 0x300 bytes and each level adds at least "/x".
   constexpr size_t MaxDirectoryDepth = 0x180;
   ```

3. `GetEntry`, `GetDirectoryEntry`, `GetFileEntry` return
   `std::optional<std::pair<EntryType, std::string>>`; `GetEntry` returns `std::nullopt` where it
   returned `{}`. The rest of `GetEntry` stays.
4. `ProcessFile` and `ProcessDirectory` take `RomFSTraversalContext& ctx` (non-const), and
   `ProcessDirectory` takes an extra `size_t depth`. The loops become:

   ```cpp
   void ProcessFile(RomFSTraversalContext& ctx, u32 this_file_offset,
                    std::shared_ptr<VectorVfsDirectory>& parent) {
       while (this_file_offset != ROMFS_ENTRY_EMPTY && ctx.files_left != 0) {
           auto entry = GetFileEntry(ctx, this_file_offset);
           if (!entry) {
               break;
           }
           --ctx.files_left;
           parent->AddFile(std::make_shared<OffsetVfsFile>(ctx.file, entry->first.size,
                                                           entry->first.offset + ctx.header.data_offset,
                                                           std::move(entry->second)));
           this_file_offset = entry->first.sibling;
       }
   }
   ```

   and the same shape for `ProcessDirectory` (`ctx.directories_left`), whose recursion becomes:

   ```cpp
           if (entry->first.child_dir != ROMFS_ENTRY_EMPTY && depth + 1 < MaxDirectoryDepth) {
               ProcessDirectory(ctx, entry->first.child_dir, current, depth + 1);
           }
   ```

5. In `ExtractRomFS`, replace the two `ReadBytes` lines and the `ProcessDirectory` call with:

   ```cpp
       const u64 file_size = file->GetSize();
       // The table locations come from the image; ReadBytes allocates the size it is given.
       const auto fits = [file_size](const TableLocation& table) {
           return table.offset <= file_size && table.size <= file_size - table.offset;
       };
       if (!fits(ctx.header.directory_meta) || !fits(ctx.header.file_meta)) {
           return nullptr;
       }
       ctx.file = file;
       ctx.directory_meta =
           file->ReadBytes(ctx.header.directory_meta.size, ctx.header.directory_meta.offset);
       ctx.file_meta = file->ReadBytes(ctx.header.file_meta.size, ctx.header.file_meta.offset);
       ctx.directories_left = ctx.directory_meta.size() / sizeof(DirectoryEntry);
       ctx.files_left = ctx.file_meta.size() / sizeof(FileEntry);
       if (!GetDirectoryEntry(ctx, 0)) {
           return nullptr;
       }

       ProcessDirectory(ctx, 0, root_container, 0);
   ```

   (Keep the existing `ctx.file = file;` only once.)

**Verify**: build, `./build-local/bin/tests "[romfs]"` → `All tests passed` (2 cases);
`./build-local/bin/tests "[loose_directory],[nup]"` → `All tests passed`.

### Step 3: Tests for truncated tables and cycles (only after the fix — before it they never end)

Add (tag `[core][romfs]`):

3. **"A RomFS whose directory table is too short for the root is rejected"** —
   `Write(image, 0x20, u64{0x10});` (directory_meta size smaller than one 0x18-byte entry).
   `CHECK(Extract(image) == nullptr)`.
4. **"A RomFS whose file table is cut off keeps the readable files"** — set file_meta size
   (`0x40`) to `0`. `Extract(image)` is not null and `GetFile("a.txt") == nullptr` (the chain
   starts at an unreadable entry and ends), `GetSubdirectory("s")` is not null.
5. **"A RomFS directory that is its own sibling ends the walk"** —
   `const auto dir_meta = Read64(image, 0x18); Write(image, dir_meta + 0x04, u32{0});` (the root's
   `sibling` points to the root). `Extract(image)` is not null and `GetFile("a.txt")` is not null.
6. **"A RomFS file that is its own sibling ends the walk"** —
   `const auto dir_meta = Read64(image, 0x18); const auto file_meta = Read64(image, 0x38);`
   `u32 first_file; std::memcpy(&first_file, image.data() + dir_meta + 0x0C, 4);`
   `Write(image, file_meta + first_file + 0x04, first_file);` (the root's first file links to
   itself). `Extract(image)` is not null and `GetFile("a.txt")` is not null.

**Verify**: `./build-local/bin/tests "[romfs]"` → `All tests passed` (6 cases), finishing within a
few seconds.

### Step 4: Full suite and mutation check

**Verify**: `./build-local/bin/tests` → `All tests passed`;
`orchid-agent-test --build-dir build-local --base HEAD -- "[romfs]"` → exit 0, score ≥ 80 %.

## Test plan

`src/tests/core/romfs.cpp`, 6 cases, tag `[romfs]`: writer round trip, table past the image,
directory table shorter than the root, cut-off file table, directory self-cycle, file self-cycle.
Existing RomFS users in `[loose_directory]`, `[nup]` and `builtin_font_glyphs.cpp` must stay green.

## Done criteria

ALL must hold:

- [ ] `./build-local/bin/tests "[romfs]"` prints `All tests passed` with 6 test cases
- [ ] `./build-local/bin/tests` prints `All tests passed`
- [ ] `grep -c "std::nullopt" src/core/file_sys/romfs.cpp` prints `1` (the failed read in `GetEntry`)
- [ ] `grep -c "MaxDirectoryDepth" src/core/file_sys/romfs.cpp` prints `2`
- [ ] Mutation check exits 0 (or the report states the tool is not installed)
- [ ] `git status --short` lists only the in-scope files
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- Test 1 fails before the fix (the writer image does not extract as expected).
- Test 2 passes before the fix.
- After the fix any `[loose_directory]`, `[nup]` or font test fails: the budget or depth limit
  rejected a valid image. Report the test; do not raise the limits without saying so.
- The struct offsets used in the tests (0x18, 0x20, 0x38, 0x40, `+0x04`, `+0x0C`) do not match the
  `static_assert`ed layouts in `romfs.cpp`.

## Maintenance notes

- The budget counts entries, not bytes: a valid table has at most `size / sizeof(entry)` entries
  and visits each once, so the budget never truncates a valid tree.
- A cycle in a damaged image now yields a partial tree instead of a hang. The game then fails on a
  missing file with a normal error.
- If extraction is ever made iterative, `MaxDirectoryDepth` can go; until then it keeps the
  recursion's stack use small on worker threads.
