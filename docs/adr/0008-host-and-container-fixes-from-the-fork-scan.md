# 0008. Host and container fixes from the 2026-11-21 fork scan

Status: accepted, 2026-11-21.

Facts behind the ports in this batch that the code does not show.

## KIP parsing (`src/core/file_sys/kernel_executable.cpp`)

- Sections 3 to 5 of a KIP (.bss and two unused ones) hold no file data and nothing reads their vectors: the loader takes the .bss size from the header (`GetBSSSize`). Their sizes come from the file, so they stay empty (an 847-byte KIP could allocate 4 GiB). Sections 0 to 2 are still treated as compressed when stored and decompressed sizes differ, although the KIP1 format marks compression per section in bits 0 to 2 of the header flags (not changed).
- `ReadBytes` stops at the end of the file, so a truncated KIP used to load with a stored section shorter than its header says. It now fails with `ErrorBadKIPHeader`.
- BLZ: the footer sizes come from the file. `compressed_size + additional_size` is added in 64 bits because two u32 can wrap (a wrapped sum shrank the section to 12 bytes or to 0, which loaded as a success). BLZ turns at most 17 input bytes (a control byte and eight 2-byte copies of up to 18 bytes) into 144 output bytes, so a valid stream never asks for more than nine times its compressed size plus 144.

## NUP writer

`memcpy` takes no null pointers, not even for zero bytes. An empty `std::span` can hold one, so `PutBytes` skips the copy. Only UBSan shows it.

## Play time (`src/frontend_common/play_time_manager.cpp`)

`pdm:qry`'s `IQueryService` keeps its own `PlayTimeManager`, which reads `playtime.bin` when the services start and never changes it. The frontend's manager records the session and saves it when the game stops. Closing the services ran the destructor of `pdm:qry`'s manager, which used to save unconditionally and wrote the boot-time values over the session just saved. Only a manager that changed the database (`modified`) saves on destruction.

## Config save (`src/frontend_common/config.cpp`)

The INI is written to `<config>.tmp` beside the real file (resolved through symlinks, so a dotfile-managed config stays a link), flushed and fsynced, given the old file's permissions, renamed over the target and the folder is synced. ext4 with `noauto_da_alloc` and XFS may commit the rename before the data, which left an empty file after a power loss. A failed write or sync removes the temporary file and keeps the old config. The test wraps `fflush` and `fsync` at link time (`-Wl,--wrap`, Linux only).

## Orphaned profile scan (`ProfileManager::FindOrphanedProfiles`)

Only 32-hex user ID folders under `nand/user/save/0000000000000000` are profile candidates. A title folder copied in by hand (for example `0400486024DEC000`) is neither reported nor removed. A folder whose scan fails is kept, not treated as empty.

## Motion (`MotionInput`)

The default PID has no integral term. It summed the gravity error once per sample with no time scaling and no cap, so after a fast swing it kept rotating the orientation for seconds and snapped back when the error fell under the reset threshold. The proportional term alone keeps the orientation locked to gravity.

## Windows host memory (`src/common/host_memory.cpp`)

A partial unmap on Windows takes the whole view down and maps the kept pieces back with `MapViewOfFile3`, which maps them read-write. `Protect` had made some of their pages read-only or inaccessible (rasterizer write traps, read traps), and the trackers still counted them as protected. `UnmapOnePlaceholder` reads the protection runs of the kept pieces with `VirtualQuery` before the unmap and gives every run that was not read-write its protection back right after the piece is mapped. The pieces are mapped read-write because a view's mapping protection is the most `VirtualProtect` can give its pages later. Known limit: between a piece's mapping and its restore (a few system calls) its pages do not fault. A failed query or restore stops the emulator (`UNREACHABLE_MSG`) rather than run on without a trap the trackers count on. `Protect` and `Unmap` both hold `placeholder_mutex`, so a protection changed in between cannot be undone. Linux replaces only the freed range and keeps the rest, so it needs none of this. The test runs on Linux too (reads `/proc/self/maps`); on Windows it was run with MinGW and Wine.

## Kernel shutdown (`src/core/core.cpp`)

The general channel's event and service context are created the first time a game pushes to the channel. They are released just before `kernel.Shutdown()`, not in `~Impl` at `exit()`, where `Event::~Event` closed through freed kernel state. Queued channel data stays; the next kernel's event is created on first use and signalled when data is waiting. A failed load or failed system setup releases the application process before `ShutdownMainProcess`, as a normal stop does.

## Vulkan present and samplers

- A refused submit in `PresentManager::DiscardFrame` leaves the frame's `present_done` fence unsignaled and the next use of the frame waits on it for good. The frame gets a fresh signaled fence. `VK_ERROR_DEVICE_LOST` at present or while waiting for a frame's fence calls `Device::ReportLoss`.
- `Sampler::Emplace` catches a refused `vkCreateSampler` (too many objects, out of host or device memory) and borrows the handle of a variant of the same sampler with identical filters, anisotropy, compare, reduction and border behaviour, or the runtime's first sampler if that is compatible. A custom-border variant only borrows from a variant with the same swizzle. Nothing compatible: rethrow. The borrowed handle lives as long as the runtime, because samplers are never erased from the texture cache's slot vector.
