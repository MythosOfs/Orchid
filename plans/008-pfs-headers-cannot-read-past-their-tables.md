# Plan 008: A PFS0/HFS0 header cannot make the parser read past its tables or loop forever

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report — do not improvise. When done, update the status row for this plan
> in `plans/README.md` — unless a reviewer dispatched you and told you they
> maintain the index.
>
> **Drift check (run first)**: `git diff --stat 07d884a918..HEAD -- src/core/file_sys/partition_filesystem.cpp src/core/file_sys/partition_filesystem.h`
> If either file changed since this plan was written, compare the "Current state" excerpts
> against the live code before proceeding; on a mismatch, treat it as a STOP condition.
> (`src/tests/CMakeLists.txt` may have changed through other plans; only check that the line you
> insert after still exists.)

## Status

- **Priority**: P1
- **Effort**: S
- **Risk**: LOW
- **Depends on**: none
- **Category**: security
- **Planned at**: commit `07d884a918`, 2026-11-19

## Why this matters

`PartitionFilesystem` parses the PFS0/HFS0 container at the heart of every NSP, XCI and ExeFS —
files users download. Three header fields are trusted:

1. Each entry's `strtab_offset` picks where its name starts in the string table. Nothing checks it,
   and the name is read with `std::string(const char*)`, so an offset past the table reads heap
   memory until a zero byte appears (crash or garbage name).
2. `num_entries` and `strtab_size` size a `ReadBytes` before anything checks them against the file,
   and the base `VfsFile::ReadBytes` allocates the full size up front: a 64-byte file can request a
   multi-gigabyte buffer.
3. The entry loop counts with a `u16` while `num_entries` is a `u32`: with more than 65535 entries
   the index wraps and the loop never ends.

After this plan malformed headers fail with the existing error statuses, valid archives parse
exactly as before, and unit tests pin all three.

## Current state

`src/core/file_sys/partition_filesystem.cpp:25-80` (the constructor):

```cpp
PartitionFilesystem::PartitionFilesystem(VirtualFile file) {
    // At least be as large as the header
    if (file->GetSize() < sizeof(Header)) {
        status = Loader::ResultStatus::ErrorBadPFSHeader;
        return;
    }

    // For cartridges, HFSs can get very large, so we need to calculate the size up to
    // the actual content itself instead of just blindly reading in the entire file.
    if (sizeof(Header) != file->ReadObject(&pfs_header)) {
        status = Loader::ResultStatus::ErrorBadPFSHeader;
        return;
    }

    if (!pfs_header.HasValidMagicValue()) {
        status = Loader::ResultStatus::ErrorBadPFSHeader;
        return;
    }

    is_hfs = pfs_header.magic == Common::MakeMagic('H', 'F', 'S', '0');

    std::size_t entry_size = is_hfs ? sizeof(HFSEntry) : sizeof(PFSEntry);
    std::size_t metadata_size =
        sizeof(Header) + (pfs_header.num_entries * entry_size) + pfs_header.strtab_size;

    // Actually read in now...
    std::vector<u8> file_data = file->ReadBytes(metadata_size);
    const std::size_t total_size = file_data.size();
    file_data.push_back(0);

    if (total_size != metadata_size) {
        status = Loader::ResultStatus::ErrorIncorrectPFSFileSize;
        return;
    }

    std::size_t entries_offset = sizeof(Header);
    std::size_t strtab_offset = entries_offset + (pfs_header.num_entries * entry_size);
    content_offset = strtab_offset + pfs_header.strtab_size;
    for (u16 i = 0; i < pfs_header.num_entries; i++) {
        FSEntry entry;

        memcpy(&entry, &file_data[entries_offset + (i * entry_size)], sizeof(FSEntry));
        std::string name(
            reinterpret_cast<const char*>(&file_data[strtab_offset + entry.strtab_offset]));

        offsets.insert_or_assign(name, content_offset + entry.offset);
        sizes.insert_or_assign(name, entry.size);

        pfs_files.emplace_back(std::make_shared<OffsetVfsFile>(
            file, entry.size, content_offset + entry.offset, std::move(name)));
    }

    status = Loader::ResultStatus::Success;
}
```

`src/core/file_sys/partition_filesystem.h` (private): `Header` is `{u32 magic; u32 num_entries;
u32 strtab_size; 4 bytes padding}` (0x10 bytes); `FSEntry` is `{u64 offset; u64 size; u32
strtab_offset}` (0x14); `PFSEntry` is `FSEntry` + 4 padding (0x18); `HFSEntry` is 0x40. Magic
values: `Common::MakeMagic('P','F','S','0')` (`common/common_funcs.h`, little endian
`'P' | 'F'<<8 | 'S'<<16 | '0'<<24`).

`VfsFile::ReadBytes` (`src/core/file_sys/vfs/vfs.cpp:178-183`) is `virtual` and allocates first:

```cpp
std::vector<u8> VfsFile::ReadBytes(std::size_t size, std::size_t offset) const {
    std::vector<u8> out(size);
    std::size_t read_size = Read(out.data(), size, offset);
    out.resize(read_size);
    return out;
}
```

Status values used here exist in `src/core/loader/loader.h`:
`Loader::ResultStatus::ErrorBadPFSHeader`, `ErrorIncorrectPFSFileSize`, `Success`.

Conventions: 4 spaces, about 100 columns, braces on the same line, comments explain why. Tests are
Catch2 in one `tests` executable, sources listed by hand in `src/tests/CMakeLists.txt`.
`src/tests/core/vfs_vector.cpp` shows the file layout (license header, includes, anonymous
namespace, `TEST_CASE("...", "[core][...]")`). `FileSys::VectorVfsFile`
(`core/file_sys/vfs/vfs_vector.h`) wraps a `std::vector<u8>` as a file:
`std::make_shared<FileSys::VectorVfsFile>(bytes, "name")`.

## Commands you will need

Run all commands from the repository root.

| Purpose | Command | Expected on success |
|---|---|---|
| Configure a build with tests (once) | `./tools/build-local.sh --configure-only -DYUZU_TESTS=ON` | exit 0 |
| Build the tests | `cmake --build build-local --target tests --parallel 6` | exit 0 |
| Run the new tests | `./build-local/bin/tests "[pfs]"` | `All tests passed` |
| Run the whole suite | `./build-local/bin/tests` | `All tests passed` |
| Mutation check | `orchid-agent-test --build-dir build-local --base HEAD -- "[pfs]"` | exit 0 |

Notes: add `-DCPM_SOURCE_CACHE=$HOME/src/Eglantine/.cache/cpm` to the configure command if
that folder exists; skip the configure if `build-local/CMakeCache.txt` already has
`YUZU_TESTS:BOOL=ON`. One full build at a time per machine. If `orchid-agent-test` is not
installed, say so in the report.

## Scope

**In scope**:

- `src/core/file_sys/partition_filesystem.cpp`
- `src/tests/core/partition_filesystem.cpp` (create)
- `src/tests/CMakeLists.txt` (one added line)
- `plans/README.md` (status row)

**Out of scope**:

- `partition_filesystem.h`: the struct layouts are on-disk formats.
- `entry.offset` / `entry.size` past the end of the file: `OffsetVfsFile` already clamps reads
  to its parent, so those give short reads, not memory errors. Leave them.
- `card_image.cpp` (XCI), `submission_package.cpp` (NSP): callers; they already check
  `GetStatus()`.

## Git workflow

- Work in the checkout you were given. One commit. Suggested subject:
  `file_sys: bound PFS string table offsets and header sizes`. Body: the three problems and the
  tests.
- Do NOT push unless the operator told you to, or you work under this repository's `AGENTS.md`
  rules.

## Steps

### Step 1: Write the tests (two of them fail before the fix)

Create `src/tests/core/partition_filesystem.cpp` and add `    core/partition_filesystem.cpp` to
`add_executable(tests ...)` in `src/tests/CMakeLists.txt`, directly after `    core/vfs_real.cpp`
(if another plan already inserted a line there, put yours after that one).

Helpers in the anonymous namespace:

```cpp
// A PFS0 image: header, one 0x18-byte entry per (strtab_offset, offset, size), the string
// table, then content.
std::vector<u8> Pfs0(u32 num_entries, const std::vector<std::array<u64, 3>>& entries,
                     std::string_view strtab, std::string_view content) {
    std::vector<u8> out;
    const auto put = [&out](u64 value, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i) {
            out.push_back(static_cast<u8>(value >> (8 * i)));
        }
    };
    put(Common::MakeMagic('P', 'F', 'S', '0'), 4);
    put(num_entries, 4);
    put(strtab.size(), 4);
    put(0, 4);
    for (const auto& [strtab_offset, offset, size] : entries) {
        put(offset, 8);
        put(size, 8);
        put(strtab_offset, 4);
        put(0, 4);
    }
    out.insert(out.end(), strtab.begin(), strtab.end());
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

// Records the largest ReadBytes request, to prove the header is checked before reading.
class RecordingFile final : public FileSys::VectorVfsFile {
public:
    using VectorVfsFile::VectorVfsFile;
    std::vector<u8> ReadBytes(std::size_t size, std::size_t offset) const override {
        largest_read = (std::max)(largest_read, size);
        return VectorVfsFile::ReadBytes(size, offset);
    }
    mutable std::size_t largest_read = 0;
};
```

Includes: `<algorithm>`, `<array>`, `<memory>`, `<string_view>`, `<vector>`,
`<catch2/catch_test_macros.hpp>`, `"common/common_funcs.h"`, `"core/file_sys/partition_filesystem.h"`,
`"core/file_sys/vfs/vfs_vector.h"`, `"core/loader/loader.h"`.

Test cases (tag every one `[core][pfs]`):

1. **"A PFS0 with one file parses its name and content"** — `Pfs0(1, {{0, 0, 5}}, std::string_view{"a.bin\0", 6}, "hello")`;
   `FileSys::PartitionFilesystem pfs{std::make_shared<FileSys::VectorVfsFile>(image)}`;
   `REQUIRE(pfs.GetStatus() == Loader::ResultStatus::Success)`; `GetFiles().size() == 1`;
   the file's `GetName() == "a.bin"`; its `ReadAllBytes()` equals the 5 bytes of `"hello"`.
   (Passes before the fix; it guards the happy path.)
2. **"A PFS0 entry name offset past the string table is rejected"** — string table
   `std::string_view{"a.bin\0", 6}`, entry `{6, 0, 5}` (offset equal to the table size). Expect
   `GetStatus() == Loader::ResultStatus::ErrorBadPFSHeader` and `GetFiles().empty()`.
   (Fails before the fix: the trailing zero byte the constructor appends makes the name `""` and
   the status `Success`.)
3. **"A PFS0 header larger than its file is rejected before it is read"** — `Pfs0(0x100000, {},
   "", "")` (claims a million entries, file is 16 bytes); wrap it in
   `std::make_shared<RecordingFile>(image)`. Expect
   `GetStatus() == Loader::ResultStatus::ErrorIncorrectPFSFileSize` and
   `file->largest_read <= image.size()`. (Fails before the fix: the constructor asks for about
   24 MiB.)

**Verify**: build, then `./build-local/bin/tests "[pfs]"` → test 1 passes, tests 2 and 3 fail on
the expectations named above (not crash).

### Step 2: Check sizes before reading, bound names, widen the loop index

Edit the constructor:

1. Compute `metadata_size` in `u64` so `num_entries * entry_size` cannot wrap on 32-bit size_t,
   and refuse a header that claims more than the file holds, before `ReadBytes`:

   ```cpp
       const u64 entry_size = is_hfs ? sizeof(HFSEntry) : sizeof(PFSEntry);
       const u64 metadata_size =
           sizeof(Header) + u64{pfs_header.num_entries} * entry_size + pfs_header.strtab_size;
       // The header's counts come from the file: check them before they size a read.
       if (metadata_size > file->GetSize()) {
           status = Loader::ResultStatus::ErrorIncorrectPFSFileSize;
           return;
       }
   ```

   Keep the existing `ReadBytes`, `push_back(0)` and `total_size != metadata_size` check after it.
   Adjust the later `std::size_t` uses of `entry_size` if the compiler warns about narrowing
   (cast with `static_cast<std::size_t>`, do not change their logic).

2. Loop with `u32 i` instead of `u16 i`.

3. Replace the `std::string name(reinterpret_cast<const char*>(...))` line with a bounded read:

   ```cpp
           if (entry.strtab_offset >= pfs_header.strtab_size) {
               status = Loader::ResultStatus::ErrorBadPFSHeader;
               offsets.clear();
               sizes.clear();
               pfs_files.clear();
               return;
           }
           // A name ends at its zero byte or at the end of the table, never beyond it.
           const std::string_view table{
               reinterpret_cast<const char*>(&file_data[strtab_offset + entry.strtab_offset]),
               pfs_header.strtab_size - entry.strtab_offset};
           std::string name{table.substr(0, table.find('\0'))};
   ```

   Clearing the three containers keeps the object consistent with its failed status (test 2
   checks `GetFiles().empty()`).

**Verify**: build, then `./build-local/bin/tests "[pfs]"` → `All tests passed` (3 test cases).

### Step 3: Pin the loop index

Add test 4 (tag `[core][pfs]`), **after** the fix — before it, this test never ends:

4. **"A PFS0 with more than 65535 entries lists every one"** — 65537 entries, each
   `{0, 0, 0}`, string table `std::string_view{"a\0", 2}`, no content:
   `std::vector<std::array<u64, 3>> entries(65537, {0, 0, 0});`. Expect `Success` and
   `GetFiles().size() == 65537`.

**Verify**: `./build-local/bin/tests "[pfs]"` → `All tests passed` (4 test cases) within a few
seconds.

### Step 4: Full suite and mutation check

**Verify**: `./build-local/bin/tests` → `All tests passed`;
`orchid-agent-test --build-dir build-local --base HEAD -- "[pfs]"` → exit 0, score ≥ 80 %.

If a mutant of the `metadata_size > file->GetSize()` line survives, check whether test 3 still
fails with that mutant; if the mutant only changes the comparison by one byte (`>` to `>=`), it
is equivalent for every input the existing `total_size != metadata_size` check also rejects —
state that in the report with the mutant text.

## Test plan

New file `src/tests/core/partition_filesystem.cpp`, 4 test cases, tag `[pfs]`: happy path, name
offset past the table, header larger than the file (with a read-size probe), more than 65535
entries.

## Done criteria

ALL must hold:

- [ ] `./build-local/bin/tests "[pfs]"` prints `All tests passed` with 4 test cases
- [ ] `./build-local/bin/tests` prints `All tests passed`
- [ ] `grep -n "for (u16 i" src/core/file_sys/partition_filesystem.cpp` prints nothing
- [ ] `grep -n "reinterpret_cast<const char\*>(&file_data\[strtab_offset + entry.strtab_offset\]))" src/core/file_sys/partition_filesystem.cpp` prints nothing
- [ ] Mutation check exits 0 (or the report states the tool is not installed)
- [ ] `git status --short` lists only the in-scope files
- [ ] `plans/README.md` status row updated

## STOP conditions

Stop and report back (do not improvise) if:

- Test 2 or 3 passes before step 2, or test 1 fails before step 2.
- Any other existing test fails after step 2 (`[nup]`, `[loose_directory]`, `[nso]` and others
  parse real PFS images). That means a valid archive is rejected; report which test.
- A test crashes instead of failing in step 1.
- You find a caller that relies on a `PartitionFilesystem` with a non-`Success` status still
  listing files.

## Maintenance notes

- Valid archives are unaffected: their names end inside the table and their metadata fits the
  file.
- An entry whose name has no zero byte now ends at the table's end, which is what the old code
  produced too (it appended one zero byte after the table).
- HFS0 uses the same code path with 0x40-byte entries; the tests use PFS0 only because it is
  shorter to build. A reviewer may ask for an HFS0 case; it would only repeat the PFS0 logic.
