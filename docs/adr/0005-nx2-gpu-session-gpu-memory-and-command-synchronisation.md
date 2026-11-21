# 0005. nx2 GPU session, GPU memory and command synchronisation

Status: accepted, 2026-11-20 (moved out of code comments, see [0001](0001-no-code-comments.md)).

## Context

The Switch 2 GPU SDK (`nn::gpu::sdk`, gsv 497) manages address spaces, GPU-accessible memory, sparse and GPU-only regions itself and submits through NVN2 queues whose semaphores and command lists the emulated GPU must see in order.

## Decision

Map GPU-accessible memory at GPU == CPU address, back GPU-only and sparse regions with kernel memory in the alias code region (sparse ones lazily per block), share device ranges per process, and track GPU-written pages and queued maps so the split GPU thread reads command lists that are current.

## Consequences

GPU-session map jobs and the worker-session request complete at once, which is equivalent to the console (#189, evidence below). No console component copies the second page of a two-page GPU-accessible memory into the first; the mirror table that assumed one is gone (#191, evidence below). Memory accounting and lock order (`m_region_mutex`, the sparse state lock taken last) are load-bearing.

## Decision log

One entry per former code comment, by file, named after the code it described.

### `src/core/hle/service/nvdrv/devices/nx2_gpu_command_scope.h`

- `static void Run(GPU& gpu, std::function<void()>&& command, bool map = false)`: `map`: the command maps GPU memory, so its batch goes through MapInOrder (with the GPU split it runs before the front reads later command lists). Fakes without it use RunInOrder.

### `src/core/hle/service/nvdrv/devices/nx2_gpu_mapping_table.h`

- `struct Window`: The ranges reaching [gpu, gpu + size). An update only changes that window, so comparing it alone gives the same changes as comparing the whole table (thousands of ranges, once per map or unmap while a course streams in).

### `src/core/hle/service/nvdrv/devices/nx2_stub_device.h`

- `class Nx2GpuMemoryRegistry`: GPU mappings a Switch 2 process made through nx2:gpu: CreateGpuAccessibleMemory (sub 101) maps the memory at GPU address == CPU address, and NormalMapGpuMemory4K (211) maps a memory handle at a chosen GPU address (also in sparse regions). Every nvhost-as-gpu address space applies each mapping, including ones made before the address space existed.
- `void ReleaseBacking(u64 cpu_address, u64 size, std::function<void()> retired = {})`: Backing memory [cpu_address, +size) is about to be freed (its GPU mappings are gone): each address space drops its SMMU mapping of it, in GPU-thread order.
- `bool CreateSparse(u64 gpu_address, u64 size)`: Sparse regions (nx2:gpu 214 with the sparse flag): the GPU range reaches device memory of its own from the start, and each block gets memory when the GPU first writes it (BackSparse). The GPU mapping never changes, so the texture cache doesn't rebuild the images over it as blocks get memory; unbacked blocks read zero.
- `void Reset()`: A new emulation session starts: forget the previous game's mappings and device ranges (its address spaces removed their listeners when they closed).
- `u64 AcquireDeviceRange(size_t asid, u64 cpu_address, u64 size,`: Device (SMMU) range of a mapping outside the heap, shared by every address space of one process: the caches key memory by device address, so a range per address space made separate copies of the same memory, and presentation could read one the game never drew into. The first address space to map the range allocates it with `allocate` (0: out of space); every call takes a reference.
- `bool ReleaseDeviceRange(size_t asid, u64 cpu_address, u64 size)`: Drops a reference. True for the last one: the caller frees the range.
- `void ForgetDeviceRanges(size_t asid)`: The process's session closed and its ASID can be reused: stop sharing its ranges.
- `std::mutex device_mutex;`: Separate lock: ranges are recorded from listeners that run under `mutex`.

### `src/core/hle/service/nx2_gsv.h`

- `class INx2GpuSession final : public ServiceFramework<INx2GpuSession>`: The Switch 2 GPU session (nn::gpu::sdk), handed out by gsv 497.
  - TIPC 11: [first, last] id range (first <= min(last, 64)); ids outside it are masked out.
  - TIPC 12(id): event handle for that id (handle at word 3, result at word 4); the SDK attaches it as a SystemEvent.
  - TIPC 21(id): asks for a second session (likely the one the SDK's debug templates name `nn::gpuk::impl::kuser::KUserWorkerSession`). Its only callers are nn::gpu::sdk::BootWithTransferMemory and BootWithRemoteMemory: they take an id from the id pool, send 21(id), wait on the id's event in 1 s steps, then fetch the session with TIPC 22. The event means "the session is ready", no GPU work stands behind it, and here the session is this object, so the event is signalled at once (#189).
  - TIPC 22: returns that session's handle (move handle in the reply); the sub commands 101-216 go to it.
- `in RegisterHandlersTipc()`: Sub-session commands (gpu 22 hands out a second session to this same object).
- `void CreateGraphicsDevice(HLERequestContext& ctx)`: 41(u64 name "NvRmGpu", u64 flags, u64 debug label): u32 device id, the first free slot, that later NvRm calls name (Fire Emblem: Fortune's Weave). Id 0 is invalid to the SDK.
- `void GetVaRange(HLERequestContext& ctx)`: Used by nn::gpu::sdk::CreateGpuAddressSpace: three u64s after the result, a kind (< 5, 0 = none) and the GPU VA range of the in-process VirtualMachine that serves AllocSpace (nn::gpu::sdk::util::ExecuteVirtualMachineAllocSpace). NVN reserves fixed VA from 0x8000000 up to 512 GiB, so report the same range NvRm gives AllocAsEx: [0x8000000, 0xC000000000).
- `void GetUserPartition(HLERequestContext& ctx)`: Newer SDKs (title 1) query the address space's user and gsv partitions with 201/202(id): two u64s each, stored as the partition's min/max. The user value works whether it is read as (start, size) or (min, max). Title 4 never needed the gsv partition, so it stays empty.
- `void GetGsvPartition(HLERequestContext& ctx)`: An empty partition is min = ~0, max = 0 (min > max). Fire Emblem: Fortune's Weave's SDK reads (0, 0) as a one-byte partition at address 0 and aborts in nn::gpu::sdk.
- `in OpenSubSession()`: Served by this object so sub-session jobs can signal the per-id events.
- `void SubRegisterMemory(HLERequestContext& ctx)`: Sub 101: (start u64, last u64, flags...) after CreateGpuAccessibleMemory's svc 0x80.
- `in SubRegisterMemory()`: nn::gpu::sdk::CreateGpuAccessibleMemory stores word 3 of the reply as the GpuMemoryHandle that later map jobs (211) name.
- `void SubSlotJob(HLERequestContext& ctx)`: Jobs that complete through a job-slot event (word Slot of the request), like 211.
- `void SubSubmitJob(HLERequestContext& ctx)`: Sub 211: nn::gpu::sdk::NormalMapGpuMemory4K. Word 2 is the address space, words 10-16 the permission, cacheability, two kind bytes, RawTextureMode, an address space byte and the AddressSpaceSyncFlag, word 17 the job slot (an id from the same pool as TIPC 21) whose event (gpu TIPC 12) the SDK waits on. The SDK waits on that event whatever the reply says, then returns the slot to the pool; it waits for no GPU progress. So the event means "the GPU page table holds the mapping", and the only readers of a 211 mapping are GPU commands the game submits after the call returns. Here the map is queued for the GPU thread in order (Nx2GpuMemoryRegistry::MapAt, MapInOrder) ahead of every later submission, and the split's front finds it while still queued (Nx2QueuedMaps), so signalling at once is equivalent (#189). Unmaps (213) and destroys (102) still complete behind the GPU work already queued (SignalAfterGpu): GPU work submitted before them may still read the range.
- `in SubSubmitJob()`: Words 3-4: first GPU address, 5-6: last, 7: memory handle, 8-9: offset into it.
- `in SubSubmitJob()`: Also at the memory's own address: an earlier 213 may have unmapped the mapping 101 made there.
- `void SubModifyKind(HLERequestContext& ctx)`: Sub 212 (nn::gpu::sdk::NormalModifyMemoryKind4K, behind NvRmGpuMappingModify): word 2 the address space, 3-4 first GPU address, 5-6 last, 7 the wanted (compressible) kind, 8 the fallback kind, 9 a sync flag, 10 the job slot. The reply's word 3 is the kind applied. Kinds do not matter to the emulated GPU, so keep the mapping and report the wanted kind.
- `void SubUnmapJob(HLERequestContext& ctx)`: Sub 213: words 3-4 first GPU address, 5-6 last, job slot in word 8.
- `void SignalAfterGpu(u32 slot)`: Signals a job slot's event once the GPU thread has run everything queued so far, as the console's graphics server completes unmap jobs behind the submitted work.
- `void SubDestroyMemory(HLERequestContext& ctx)`: Sub 102: word 2 is the memory handle 101 returned, word 3 the job slot. The game frees the memory (svc 0x80 clears GPU access) and reuses it as ordinary heap right after, so nothing on the GPU side may keep it: drop the mapping 101 made at its own address.
- `void SubCreateGpuOnlyRegion(HLERequestContext& ctx)`: Sub 214: GPU-only memory [start, last] (words 3-6, job slot in word 11). The graphics server backs it itself: the client never maps anything there but renders into it (title 4 keeps its render targets in six 300 MB regions at 0x81_0000_0000). Back each region with kernel memory placed in the process's alias code region and map that for the GPU.
- `in GpuCommands::Execute()`: Title 1's terrain texture arrays and layer pools (170 MB-5.8 GB, mostly empty): backed whole, they ran the pool dry and the later ones got no memory at all. The region gets a device range of its own, unbacked pages read zero, and BackSparseWrite gives a block memory before the GPU writes into it.
- `void SubDestroySparseRegion(HLERequestContext& ctx)`: Sub 215 (nn::gpu::sdk::DestroySparseRegion): words 3-6 the range, word 8 the job slot, like 213. Drop the GPU mapping, then the backing 214 added: its SMMU mapping (in GPU-thread order) and, once the GPU thread is past that, its pages. Kept allocated, title 1 ran the pool dry within minutes and later regions had no memory at all (GPU writes to them lost).
- `in GpuCommands::Execute()`: The region's backing: all of it, or the blocks a sparse region got as it was written.
- `bool InGpuOnlyRegion(u64 gpu_address, u64 size) const`: Needs m_region_mutex. True when [gpu_address, +size) lies in a region 214 backed.
- `static const bool map_anyway = std::getenv("EDEN_NX2_SPARSE_MAP_GPU_ONLY") != nullptr;`: A GPU-only region (214) keeps the memory this server backed it with. Donkey Kong Bananza maps one and the same 200 MB block over each of its six regions right after creating them: all six then shared that memory, and its video, render targets and bloom images overwrote each other (tiles over the video, glowing smashes). EDEN_NX2_SPARSE_MAP_GPU_ONLY=1 maps it anyway (debug aid).
- `std::optional<MappedBacking> AllocateGpuOnlyBacking(u64 size, bool log_failure, bool clear)`: Allocates kernel memory and maps it in the alias code region. Touches no region state, so it needs no lock: the GPU back thread calls it without m_region_mutex, because mapping can wait on the guest page table lock and a guest thread holding that lock must not be stalled behind a host mutex the service thread is waiting for.
- `in AllocateGpuOnlyBacking()`: The graphics server's own memory, not the application's: taken from the application pool, title 4's six regions left it 1.8 GB less than the kernel reports to it. Not cleared: the GPU writes these before reading them, and touching 300 MB would commit it on the host.
- `in AllocateGpuOnlyBacking()`: Cheap for memory that's still zero (most of it): only reused pages get cleared.
- `void RegisterGpuOnlyBacking(u64 gpu_address, MappedBacking&& backing)`: Records a mapped backing for the region at gpu_address. Needs m_region_mutex.
- `std::optional<u64> BackGpuOnlyRegion(u64 gpu_address, u64 size)`: Needs m_region_mutex.
- `in BackGpuOnlyRegion()`: A title may create a region again from the same start with another size (Mario Kart World: 300 MB, 600 MB, 1 GB at one address) without destroying it. Overwriting the entry kept the old backing mapped and its pages out of the pool for good: release it as 215 does, once the GPU thread is past it.
- `void BackSparseWrite(u64 gpu_address, u64 size, const u8* data)`: Runs before a GPU-side write that may land in a sparse region (Tegra::Nx2BackSparseWrite, usually on the GPU thread): the blocks the write touches get memory, cleared, behind the region's device pages. The GPU mapping stays as it is, so the images over the region keep their storage and only see the write.
- `in for_regions()`: Zeros over a block nobody wrote change nothing: it reads zero without memory. Title 1 clears ~1 GB of them at boot.
- `in BackSparseWrite()`: Allocating and mapping happens outside the lock (see AllocateGpuOnlyBacking).
- `static bool LazySparseRegions()`: EDEN_NX2_SPARSE_LAZY=0 backs sparse regions whole, as before.
- `static constexpr u32 SparseRegionFlag = 0x200`: Word 7 of 214 (0x60c arrays, 0xa0c pools)
- `void HintColdBacking()`: Every 30 s: the backing of sparse blocks and GPU-only regions is marked cold. The game writes it once (texture uploads by 2D blits: title 1 fills 2-4 GB of texture pools) and it is read again only when the texture cache builds an image over it, so the OS may compress or page it out before anything else. Last member: stops before the regions are destroyed.

### `src/tests/video_core/nx2_queued_maps.cpp`

- `TEST_CASE("A command list inside a map still queued for the GPU thread is found",`: Issue 113: Pokemon Pokopia maps its compute queue's command memory and submits from it right away. While the GPU thread was still busy with the first frame, the map was queued behind it.

### `src/video_core/nx2.h`

- `inline std::atomic<bool> g_nx2_ampere{false};`: Set once a Switch 2 (Ampere, NVN2) driver is talking to the GPU. Its command streams use Ampere classes and the Turing+ macro ISA, which Eden's Maxwell engines don't implement.
- `inline constexpr u64 Nx2WrittenPageBits = 12;`: Device pages the GPU has written through the buffer cache (shader stores, copies), ever. Title 1 runs command lists the GPU writes each frame (20-byte entries its culling fills in); read without a flush they are stale and the draws behind them (trees) go missing. The bits never clear: the dispatch that rewrites a page may still be queued on the split's back when its front reads the command list. One bit per 4 KiB of the 36-bit device address space (2 MiB).
- `inline std::atomic<u64> g_nx2_gpu_written_generation{};`: Counts the times pages were marked GPU-written for the first time: it stops moving once a title has written all the memory it reuses (Nx2CheckCommandPagesFirst).
- `inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenPageBits)) / 64> g_nx2_command_list_p…`: GPU-written device pages the front has read command lists from (same layout as above). The buffer cache copies these pages into host memory right after the compute or copy work that writes them and submits, so the front reads the copy once the GPU is done instead of draining the split and flushing the caches (BufferCache::Nx2ReadListCopy).
- `inline bool Nx2CheckCommandPagesFirst()`: Whether the GPU thread split's front may check a command list's GPU-written page bits before waiting for the back, once no page has been marked for the first time in a while (DmaPusher::FlushGpuWrittenCommands). Draining first for every GPFIFO entry stalled Mario Tennis Fever's front ~40% of the time and serialized Donkey Kong Bananza's front and back (~270,000 drains a second); checking first misses only a page's very first GPU write while it is still queued on the back. EDEN_NX2_CMD_DRAIN=1 always drains first.
- `[[nodiscard]] inline bool Nx2AcquireQueuedEnabled()`: Whether acquires that a release still queued for the split's back meets skip the drain (GpuSplit::NoteQueuedRelease). EDEN_NX2_ACQUIRE_QUEUED=1 turns it on.
- `class Nx2QueuedMaps`: GPU address ranges whose map is queued for the GPU thread (GPU::MapInOrder) and not applied yet. Until then a read through the address space finds them unmapped, or finds what was mapped there before. Pokemon Pokopia maps its compute queue's command memory and submits from it at once: with the GPU thread still busy (first frame, pipelines being built) the words copied at submit time were zeros, the queue's first lists ran as nothing and the game waited forever on their releases (black screen after one frame, issue 113).
- `using Nx2SparseBackingHandler = std::function<void(u64 gpu_address, u64 size, const u8* data)>;`: Switch 2 sparse regions (nx2:gpu 214, word 7 bit 0x200: title 1's terrain texture arrays and layer pools, 170 MB-5.8 GB each, of which the game fills a little) get memory per block when the GPU first writes there, not in full. nx2:gpu installs the handler and the span its regions cover; the GPU memory manager calls Nx2BackSparseWrite before each write, with the bytes it writes (blocks nobody wrote that only get zeros need no memory: they read zero already).
- `struct Nx2SparseRegionState`: Which blocks of each sparse region have been written (got memory), for the texture cache: an image over blocks nobody wrote is all zeros and needs no device memory. Its own lock, taken last: the texture cache asks with its lock held, and nx2:gpu updates it holding only its region mutex.
- `std::vector<bool> mapped_pages;`: Once the game maps its own memory into the region (nx2:gpu 216): which tiles it maps. Only those hold data then (writes elsewhere are dropped, as on the console).
- `inline constexpr u64 Nx2SparseMappingPageSize = 4ULL << 10;`: Coverage granularity of accepted guest GPU mappings, independent of host sparse blocks.
- `inline void Nx2ClearSparseRegions()`: A new emulation session: the previous game's regions are gone (nvdrv's Module clears them).
- `inline bool Nx2SparseBlockIsWritten(u64 region_address, u64 block)`: True unless the block of the region at region_address is known to hold nothing (no write from the CPU or the GPU yet).
- `inline std::vector<std::pair<u64, u64>> Nx2SparseDataRanges(u64 gpu_address, u64 size)`: The parts of [gpu_address, +size), which lies in one sparse region, that can hold data: the tiles the game maps there if it maps any, otherwise the blocks written. As (offset from gpu_address, size) runs.
- `inline void Nx2SparseMarkWritten(u64 gpu_address, u64 size)`: The GPU renders into [gpu_address, +size): count its blocks as written, so images made there later get memory (their contents live in the host image, not in guest memory).
- `class Nx2SemaphoreWaiters`: Channels suspended on a semaphore acquire. A console channel polls and sees every release; Eden writes releases in bursts when fences signal and resumes waiters only between command lists, so an acquire-equal could miss its value. Writes latch the waiters they satisfy.

## Evidence

### #189: what the GPU session's jobs wait for (2026-11-21)

Disassembly of the `sdk` NSO (dynamic symbols name the nn::gpu::sdk functions). Mario Tennis Fever v5.0 (module id `32a9ac14…`), Pokémon Pokopia v10.0 (`cc7da00d…`) and Donkey Kong Bananza v7.0 (`18265dae…`) share the code below; the addresses are Mario Tennis Fever's, the other two sit 0x60 higher. TIPC command = header type - 16.

- TIPC 21 (type 0x25) is sent from one function (0x3ebd20). Its only callers are BootWithTransferMemory (0x3e8570) and, through 0x3ec164, BootWithRemoteMemory (0x3e8610). It takes an id from the id pool (0x3ebf80), sends 21(id), waits on that id's event in 1 s steps (0x3ebdc0) and then calls 0x3ebe88, which sends TIPC 22 (type 0x26) and keeps the move handle of the reply as the session.
- NormalMapGpuMemory4K (0x3e9bc0) sends sub 211 (type 0xe3, 16 words), then waits on the job slot's event (word 17) in 1 s steps whatever the result was, then returns the slot to the pool (0x3e9e64). On success it adds the mapped size to the address space's counter and stores the low byte of reply word 3 through its `unsigned char*` argument; Orchid's reply has no word 3, so that byte is what the request left there (the low byte of the first GPU address, 0 for 4 KiB-aligned maps). Nothing in the call waits for GPU progress.
- SynchronizeGpuAddressSpace (0x3e99e0) is sub 200 (type 0xd8: address space, job slot). Orchid does not serve it; no scope game's log reports it as unimplemented.

### #191: the two-page GPU-accessible memories (2026-11-21)

The mirror came from early bring-up (Takoroka dce3adbc4c, 2026-10-27, first region only). Upstream dropped it (Franseven 9582ebe2d1, "it clobbered 3D report counters"), and on Orchid main nothing has read the table since 60ef6d2750 (2026-10-31); releases v0.4.0 to v0.12.0 all shipped without it.

A diagnostic build of origin/main 2cba629f6a counted every GPU write and acquire inside the 0x2000-byte memories nx2:gpu 101 registered (private harness runs along the scope routes `mtf-stadium-match-v3`, `bananza-leon-tunnel-entry-v4` and `wonder-level-entry-v2`, about 60 s of gameplay each; counts at the last 10 s summary):

| Game | Two-page memories | Host semaphore releases page 1 / page 2 | Reports page 1 / page 2 | QMD releases page 1 / page 2 | Acquires page 1 / page 2 |
|---|---|---|---|---|---|
| Mario Tennis Fever | 139 | 0 / 49,654 | 33,157 / 49,654 | 0 / 0 | 0 / 0 |
| Donkey Kong Bananza | 114 | 0 / 43,122 | 96,587 / 47,092 | 9,470 / 0 | 79,935 / 0 |
| Super Mario Bros. Wonder | 315 | 0 / 89,842 | 53,925 / 89,842 | 37,282 / 0 | 62,951 / 0 |

- They are not one per queue: a game makes 114 to 315 of them.
- Host semaphore releases land only in the second page (they also pass the query cache, so they appear again among the reports), compute QMD releases only in the first, other reports in both. GPU acquires (all greater-or-equal) read only the first page, and every acquired word but the very first per word also got releases of its own (Bananza 79,933 of 79,935, Wonder 62,949 of 62,951).
- The two pages hold different counters at the same offsets: Bananza's page-1 word 0x0 stood at 0x11 while page 2 at 0x0 stood at 0x114 and advanced by 0x40 per step against 3 on page 1. When a page-1 acquire was issued, the page-2 word at the same offset already met it while its own word did not in 70,460 of Bananza's 79,935 and 25,669 of Wonder's 62,951 acquires; a mirror would have ended those waits before the work they wait for ran, which matches the clobbering upstream saw.

So there is no copy between the pages to emulate: the first page's acquires are met by the first page's own releases, as Orchid already runs them. Not measured: which page the guest CPU polls (CPU reads are not visible to the emulator), and the NVN2 code that allocates these memories (it sits in the stripped part of `sdk` and reaches CreateGpuAccessibleMemory only through NvRmMemHandleAllocAttr, 0x57d2e0, with sizes computed at run time).
