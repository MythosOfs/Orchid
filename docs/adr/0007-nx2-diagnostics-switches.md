# 0007. nx2 diagnostics switches

Status: accepted, 2026-11-20 (moved out of code comments, see [0001](0001-no-code-comments.md)).

## Context

Switch 2 bring-up needs data the normal log does not give: GPU thread time per zone, host thread samples, present timing, frame and memory captures.

## Decision

Keep these as opt-in `EDEN_NX2_*` environment switches read once, with no cost when unset.

## Consequences

The switches below are part of the debugging workflow; removing one needs a replacement for the investigations that use it.

## Decision log

One entry per former code comment, by file, named after the code it described.

### `src/core/hle/service/nx2_gsv.h`

- `inline bool Nx2ImageTrace()`: EDEN_NX2_IMAGE_TRACE: log GPU memory registrations and mappings (checked on every nx2:gpu map, so read once).
- `in PresentClientBuffer()`: Where each frame's time goes on the present thread, averaged per 600 presents: the game is blocked here while Eden's GPU thread catches up (fence) and composites.
- `static const u32 window = []`: EDEN_NX2_PRESENT_STATS=<n> averages over n presents instead (shorter benchmark windows).
- `in PaceToVblank()`: For reports: how often each step was used. All 1 = the PC held 60 FPS.
- `void CaptureFrame()`: EDEN_NX2_FRAME_CAPTURE=<dir>: every 120th present, save what the renderer shows as nx2-frame-<n>.tga in <dir> (lets hidden test runs check the output; idea from changes.patch).
- `auto pending = m_capture_pending`: the screenshot callback can outlive the session
- `header[2] = 2`: uncompressed true colour
- `in CaptureFrame()`: Descriptor bit 5: rows top to bottom. invert_y means the data is bottom-up.
- `void DumpRanges()`: EDEN_NX2_DUMP=<present>:<hex address>:<hex size>[,<hex address>:<hex size>...]: at that present, flush those GPU ranges back from the host GPU and save their bytes as nx2-dump-<address>.bin in the EDEN_NX2_FRAME_CAPTURE directory (to inspect what a pass rendered, e.g. a 3D volume, offline).
- `static_cast<void>(system.GPU().RunOnGpuThread([&system = system, address, size, path]`: On the GPU thread: waiting for it here can deadlock with a GPU wait on this present.
- `in Log()`: EDEN_NX2_VDEC_TRACE=1: every video decoder call at Info level.
- `void DumpFrame(const std::vector<u8>& bytes, const Nx2DecoderImageLayout& layout, u32 width,`: EDEN_NX2_VDEC_DUMP=<dir>: sample every 120th decoded frame at quarter size, saving NV12 luma or the first RGB channel as PGM, and RGB output as PPM before storage conversion.

### `src/video_core/nx2_profile.h`

- `namespace VideoCommon::Nx2Profile`: EDEN_NX2_GPU_PROFILE: time where the GPU thread spends its time. Each zone counts its own (exclusive) time, nested zones excluded; every 10 s the thread logs each zone's share.
- `Other,`: GPU thread time outside every zone below (register writes, command parsing)
- `SplitDrain,`: Split GPU thread: the front waits for the back (gpu_split.h)
- `inline u64 Now()`: Nanoseconds; from the host's own counter where the wall clock reads it directly (the zones run ~150,000 times a second, and QueryPerformanceCounter cost ~5% of the GPU thread).
- `DownloadBytesOnDmaPages,`: Of DownloadBytes: on pages the MME has DMA-read
- `MethodWrites,`: 3D register writes (Maxwell3D::CallMethod below the macro range)
- `inline std::unordered_set<u64>& DmaReadPages()`: Device pages (4 KiB) the native MME has DMA-read since start (GPU thread only; filled only while profiling): which buffer downloads anything on the GPU thread reads back.
- `inline void Report(const char* thread_name = "GPU thread")`: Called by the GPU thread (and the split's back thread) between commands: logs and resets the totals every 10 s.
- `u64 zoned{};`: Time in no zone at all is the wall time minus the zones' totals.

### `src/yuzu_cmd/nx2_sampler.cpp`

- `unsigned Walk(CONTEXT context, std::array<DWORD64, MaxDepth>& frames)`: Walks a suspended thread's stack from its context. Nothing here may allocate: the thread could be paused holding the heap lock.
- `in Walk()`: A leaf function (no unwind data): its return address is on top of the stack. JIT code has none either; past the first frame, stop there.
- `std::map<std::string, unsigned> folded;`: <out>.folded: every distinct stack with its count, root first ("thread;a;b;c count").

### `src/yuzu_cmd/nx2_sampler.h`

- `void StartFromEnvironment();`: EDEN_NX2_SAMPLE=<thread names>: a sampling profiler for named host threads (Windows only). Names are comma separated; a name ending in '*' matches by prefix (CPUCore_*). From EDEN_NX2_SAMPLE_START seconds (default 60) for EDEN_NX2_SAMPLE_SECONDS (default 30), each thread is paused about 1000 times a second and its call stack recorded; the counts per function (where the thread is, and everything on its stack) go to EDEN_NX2_SAMPLE_OUT (default nx2_samples.txt).
