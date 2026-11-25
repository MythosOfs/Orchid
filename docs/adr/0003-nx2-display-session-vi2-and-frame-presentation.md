# 0003. nx2 display session (vi2) and frame presentation

Status: accepted, 2026-11-20 (moved out of code comments, see [0001](0001-no-code-comments.md)).

## Context

Titles present through the vi2 session (gsv 305). The console's compositor paces games through buffer release, vblanks and display queries; the emulator reports every buffer free, so pacing has to be rebuilt in the present path.

## Decision

Hold each present until the GPU thread has processed the frame and until the next vblank (or the title's swap interval / VRR rate), answer display queries with 1080p/4K values the SDKs accept (SDR, or HDR while HDR output is on and the host swapchain presents HDR), and drive the native event slots from vsync. The entries record layouts, pacing rules and the titles behind them.

## Consequences

Pacing lives in `PaceToVblank`/`WaitForFrame`; any change needs an fps and frame-time check in the affected games. Frame history timestamps are emulator ticks, not the console compositor's (the 53 layout is confirmed, see below).

## SDK evidence (#187, #188)

Read on 2026-11-21 from the statically linked SDK of Mario Tennis Fever (title version 327680, offsets in its `sdk` module) and checked against Star Fox (131072); Donkey Kong Bananza (458752) sends the same commands, Star Fox also 108, 354-357. The public nn::vi2 functions call an impl table filled by nn::vi2::sdk::Initialize; the slots were named by following them to the public API. Every command below except 1, 3, 5 and 53 is result only: the client reads word 2 and nothing else.

Identified:

- gsv 305(u64) OpenViSession: replies this session (handle word 3).
- 1 -> u32 notification count (< 0x400), then 11(u32 min(count, 0x3ff)) -> the notification event; 3 -> u32 native event count (< 8), then 31(index) per slot. The handles of 11 and 32 go to os::AttachReadableHandleToSystemEvent, so both are readable event handles; 31's handle is kept in the client's table. 32 carries one copy handle (the client's readable handle) and a u32 index.
- 5 -> u32 timestamp recorder count (< 8), then 51(u32 min(count, 7)) ReserveTimestampRecorders; the client marks the indices above as used (CreateTimestampRecorder, 0x568e40).
- 53(B buffer, u32 recorder, u64 checkpoint bits, u64 max 8): reply u64 bytes written (the client divides by 8) and a byte at word 5, 1 = still pending; the client polls every 100000 ticks or more and indexes the timestamps by popcount order. Matches `GetRecordedTimestamps`.
- 111(3 x u64) SetEndpointLayer / 112(2 x u64) ClearEndpointLayer: an endpoint's front layer, from CreateLayer and DestroyLayer (0x566754, 0x566870).
- 251(u64 layer, u64 endpoint token) AttachLayerToEndpoint / 252(u64 layer) DetachLayerFromEndpoint.
- 301(3 x u64) SetDisplayRefreshRateCapability (0x567a64): rate 1 = 60 Hz, 2 = 120 Hz, 0x8000... = any.
- 351(2 x u64): sent once from the layer present path (0x5661b0) while a pending flag is set; named ApplyLayerPendingState after that, the SDK's own name is unknown.

Open, with what is known:

- 352(u64 layer, 3 x f32) / 353(u64): set / clear the layer's HDR adjustable bezier (setter 0x5653a0 keeps the three floats), result only. Only Mario Kart World sends them; left unchanged.
- 113(2 x u64): result only, no caller found; 13, 108, 230-232, 302, 354-359: not read (no scope title sends them).

## Timestamp semantics follow-up for #188

The Mario Tennis Fever 327680 SDK client establishes high confidence in the VI2 53
wire shape: the recorder, checkpoint mask and maximum count are inputs, the B buffer
contains u64 entries indexed by the ordinal of each set bit, and the reply carries a
u64 byte count and a pending byte. Pending 1 causes the caller to poll. The client
reading this shape cannot establish the point in the compositor pipeline when each
timestamp is latched. [PR #208](https://github.com/MythosOfs/Orchid/pull/208)

Orbitals supplies compatibility evidence. Without usable timestamp entries
`ListFrameHistory` never advanced the frame-shown state and Unreal blocked waiting for
its render fence. Orchid writes the current guest clock tick into each returned slot
and reports pending 0, so it supplies equal query-time values for checkpoints in the
same reply. Orchid bounds the count by the mask population, B buffer space and 8,
matching the observed client request maximum. Confidence in that as hardware timing is low. The console's checkpoint
meanings, tick origin, readiness transitions and treatment of partially completed
masks remain open. A console trace should capture the recorder, mask, byte count,
pending byte and output values before and after present, with a known guest clock and
multiple checkpoint bits set. [Orbitals implementation history](https://github.com/zortos293/Lichii/commit/4cde64ae06451a1e196afd55841b41eea486d74e)

## Decision log

One entry per former code comment, by file, named after the code it described.

### `src/core/hle/service/nx2_gsv.h`

- `in RegisterHandlersTipc()`: Title 1 calls these ~75 times a second with the display name "default"; quiet handlers (the auto stub logged two lines per call: a 50 MB log in an 18 minute session).
- `in ~INx2Vi2Session()`: The vsync callback may be running on the HostTiming thread: empty its event list under its lock first (it holds the state, not this session), then close the events.
- `void PresentClientBuffer(HLERequestContext& ctx)`: 229(u64 client token, u32 slot, u32 image, ...fences): result, u64 mask of buffer slots the client may acquire next, four zero words. Nothing is shown yet; every slot is handed back at once (changes.patch's default mask is 0x7fff).
- `static const int frames_ahead = []`: Wait for this frame's fence, so the game doesn't write the next frame's data (constant buffers holding texture handles, vertex data, Pokopia's glyph atlas) while the GPU thread still reads this frame's. But title 1's GPU waits on semaphores its CPU releases only after this present returns: while a channel stays suspended on one, wait for the previous frame's fence instead and let the game prepare frame N+1 meanwhile (waiting for this frame there cost 8-56 ms of every 34-105 ms frame, or never finished). EDEN_NX2_FRAMES_AHEAD=1 always waits for the previous frame, =0 always for this one.
- `in PresentClientBuffer()`: With EDEN_NX2_FRAMES_AHEAD=1, only run a frame ahead while the GPU keeps up. With the previous frame still on the GPU here, the GPU is the limit and running ahead gains nothing; and Splatoon Raiders, which reuses its command memory as if the GPU were at most a frame behind, rewrote command lists Eden had not read yet (misread commands, then crashes, with a slow GPU). Wait for this frame too then. Not when a suspended channel brought us here: this frame can't finish before the present returns.
- `in PresentClientBuffer()`: long long like the stats: microseconds::rep is long on Linux (std::max needs one type).
- `u32 m_wait_timeouts{};`: The console's compositor frees a buffer only once its frame's GPU fence has passed, which is what keeps a game from running ahead of the GPU. This reply always reports every buffer free, so pace the game here instead: wait (bounded, in case the GPU waits on the CPU) until Eden's GPU thread has processed the frame. Otherwise the game laps the GPU thread and rewrites command memory it hasn't read yet.
- `u32 m_previous_fence_id{~0U}`: none yet (WaitForFrame ignores ids >= 192)
- `std::chrono::steady_clock::time_point m_next_vblank{};`: The console flips one frame per 60 Hz vblank and keeps the rest queued, so a game can't present faster than that. Every slot is reported free here: hold each present until the next vblank instead (title 4 ran at up to 160 fps, its timing and audio with it).
- `void PaceToVblank(u32 timing)`: timing: present word 12, (swap interval << 16) | refresh rate. Fire Emblem: Fortune's Weave sends 0x1003c during its boot logos and 0x2003c (two vblanks per frame, 30 fps) after; paced at every vblank it ran at double speed. EDEN_NX2_SWAP_INTERVAL=<n> forces the interval.
- `in PaceToVblank()`: Settings::frame_step_fps: tell the game how many 60 Hz frames the next rendered frame stands for (Core::FrameStepFps), measured from present to present. A data write to the variable the loader found in the checked build; the game copies it once per frame.
- `static const u32 stable_presents = []`: EDEN_NX2_SWAP_STABLE=<presents>: honour an interval above 1 only once the game has kept it that long. Mario Kart World flips between 1 and 2 every few frames (its own timing on a VRR display); held to two vblanks on each 2 it fell from ~40 to 20-30 fps for Selevet.
- `in PaceToVblank()`: The first 32 changes: a title that flips its interval every few frames filled the log.
- `in PaceToVblank()`: Explicit game patches pair the presentation target with their simulation step. Read the successfully installed target, not a GUI preference changed mid-session.
- `in PaceToVblank()`: Running slower than the swap interval: show at once and don't bank the lost time.
- `in PaceToVblank()`: Sleep this service thread in the guest kernel. It runs on guest core 3, and a host sleep held that core for the whole wait: game threads scheduled there ran only between presents (Fortune's Weave's loader read one RomFS block per frame).
- `bool WaitForFrame(u32 fence_id, u32 fence_value, bool unless_gpu_waits = false)`: Waits until the fence is reached. With `unless_gpu_waits`, gives up after a few ms once a GPU channel was seen suspended on a semaphore (it may be one this thread releases later). Returns whether the fence was reached.
- `in WaitForFrame()`: Long enough to cover shader/pipeline compile stalls: a game that gets more than a few command buffers ahead misses the equal-value gates its other channels acquire.
- `struct Wake`: Woken by the GPU thread as it reaches the fence. Polling with 200 us sleeps slept about 1 ms per check on Windows, added to every frame. An action left registered after a timeout fires later on a state nobody waits on.
- `bool gpu_waited = false;`: Title 1's channels resume and suspend again while they wait on this thread, so any suspension counts, not only an unbroken one.
- `void Composite(u32 image_id, u32 fence_id, u32 fence_value, u32 crop_origin, u32 crop_end,`: Shows a presented native image: word 5 is the image id, ~word 22 / word 24 the GPU fence (syncpoint, value) to wait for (layout from changes.patch). The image is a block-linear surface in the console's native GOB layout at GPU address == CPU address. crop_origin/crop_end: words 6/7, (y << 16) | x of the first and last pixel shown when flags (word 8) bit 1 is set. Without it word 7 alone is ((height - 1) << 16) | (width - 1) of the area the game drew into from the image's top-left corner (0 when it gives none).
- `in Composite()`: The present says how much of the image the game drew: Kirby Air Riders draws 1920x1080 into 2560x1440 images, Mario Kart World 2560x1440 into 3840x2160 ones. Showing the whole image put the frame in the top-left corner.
- `in Composite()`: Handheld titles keep their docked present images (up to 3840x2160) and draw the frame into the top-left 1920x1080, the handheld panel's resolution; the console's display scans out only that region. Showing whole images put the frame in a corner inside black margins that scaled with the window (Mario Kart World, Splatoon Raiders).
- `in Composite()`: Mario Kart World docked draws 2560x1440 into the top-left of its 4K images.
- `void CancelClientBuffer(HLERequestContext& ctx)`: 228(u64 token, u32 slot): return a buffer without presenting it.
- `void QueryDisplayModeCapability(HLERequestContext& ctx)`: nn::vi2::QueryDisplayRefreshRate(display name): u32 refresh rate in Hz, then a bool (a u64 in all). nn::vi::detail::DefaultDisplay::QueryRefreshRate takes only -1, 60 or 120 and fails on anything else; Minecraft polls it and never draws while it fails. A one-word reply left the bool to whatever the message buffer held. 107, nn::vi2::QueryDisplayModeCapability(display name): three 64-bit flag sets (supported refresh rates, HDR output formats, one more). The sdk's QueryDisplayVrrSupported reads bit 63 of the first as "variable refresh rate". Games with their own dynamic frame rate then pick it per frame and send it as the refresh rate of present word 12, which PaceToVblank follows (Donkey Kong Bananza: 40 to 60 Hz in steps of 2 instead of dropping to swap interval 2).
- `static const bool env_vrr = std::getenv("EDEN_NX2_VRR") != nullptr;`: EDEN_NX2_VRR=1 switches it on for test profiles made before the setting existed.
- `void QueryDisplayHdrPaperWhite(HLERequestContext& ctx)`: nn::vi2::QueryDisplayHdrPaperWhite(display name): u16 reference white in nits; 203, the BT.2408 reference white, while the display is reported as SDR. The empty reply left it to the message buffer's leftovers.
- `void QueryDisplayHdrStatus(HLERequestContext& ctx)`: nn::vi2::QueryDisplayHdrStatus(display name): u64 output format (1 = SDR, 2 = HDR), u16 peak luminance in nits. The sdk fails the query on any other format and converts these to public formats 0/1 before DefaultDisplay tests for HDR. SDR is reported with a typical SDR panel's 400 nits. With the empty auto stub reply the SDK read the request's leftover words as the format; Kirby Air Riders re-queries whenever its HDR status event fires and rebuilt its window images every few frames, which showed black.
- `Common::HdrOutput ResolveHostHdrOutput(std::optional<Common::DisplayHdrInfo>& display)`: The HDR output the guest is told about: the HDR settings combined with the state of the monitor showing the game (on Windows: Windows HDR on, the EDID peak, the SDR content brightness), and HDR only while the host swapchain really presents HDR, so an SDR desktop or surface gets an SDR frame and an HDR one the game's own HDR.
- `void NotifyHdrChange()`: Fires the slots bound to the HDR status (265, `m_hdr_status_slots`) and paper white (267, `m_hdr_white_slots`) events once the monitor's state or the host swapchain changed what the game was last told by that query (Windows HDR toggled, the window moved to another monitor, a new HDR calibration or SDR content brightness), so it queries again and switches between SDR and HDR images. Each query has its own last answer: a paper white query after a change must not hide a status the game has not read yet. Runs at every present; costs an atomic load while nothing changes.
- `void GetLayerBufferBits(HLERequestContext& ctx)`: 212(u64 layer): u64 bits of the layer's buffer slots that are ready. The client polls it once a second until some are set; it only got going because the auto stub's reply happened to hold non-zero leftovers. Report the same slots 228/229 hand back.
- `void GetBufferMaxResolution(HLERequestContext& ctx)`: The highest buffer resolution mode of the "default" display. Fire Emblem: Fortune's Weave picks its output from it: 1 -> 1280x720, 2 -> 1920x1080, 4 and 8 -> 2560x1440; 0 (the auto stub's reply), 3 and 5 fall back to 720p. Docked reports 4, handheld 2 (the screen is 1080p); EDEN_VI2_MAX_RESOLUTION_MODE=<n> overrides. Hyrule Warriors: Age of Imprisonment takes only 1 or 2 and terminates with 0x1472 otherwise.
- `constexpr u64 DefaultDisplay = 0x00746c7561666564ULL`: "default"
- `void GetTimestampRecorderCount(HLERequestContext& ctx)`: Must be < 8; nn::vi::detail::LayerHistory creates 7 timestamp recorders per layer.
- `void GetRecordedTimestamps(HLERequestContext& ctx)`: 53(u32 recorder, u64 checkpoint bits, u64 max count; B: u64 timestamps): one system tick per set checkpoint bit, ascending; reply u64 bytes written, u32 still pending. Read from the SDK's GetRecordedTimestamp, which polls this until pending is 0. nn::vi::ListFrameHistory reports a frame as shown only once it has a timestamp, and Unreal Engine games (Orbitals) wait for a shown frame before the next one: the zeroed auto stub stalled them after 8 frames, and the game thread aborted after its render fence timeout.
- `void GetNotificationEvent(HLERequestContext& ctx)`: 11: the notification event the SDK's render thread waits on while it tears a client down. One event per session, signalled as bitmaps fill (201/202, 101/102); a fresh, never signalled event per call left that thread blocked forever after 202.
- `void GetNotifications(HLERequestContext& ctx)`: 21(W: 32 u32 notification counts): hand them over and clear.
- `void OpenDisplay(HLERequestContext& ctx)`: 101(u64 display "default", u32 flags): remember which notification index this display owns.
- `void CloseDisplay(HLERequestContext& ctx)`: 102(u64 display): the display retired, wake whoever waits for that notification.
- `void RegisterClientState(HLERequestContext& ctx)`: 201(u64 token, u32 retire notification, u32 available notification).
- `void UnregisterClientState(HLERequestContext& ctx)`: 202(u64 token): the client retired; the SDK's render thread blocks on this notification.
- `void AcquireNativeEvent(HLERequestContext& ctx)`: 31(u32 index < 8): the session's event for that slot (the sdk picks a free index). The 261..272 commands bind a slot to a display event source and unbind it again. Until a slot is bound it fires on every vblank, as all of them used to.
- `void DuplicateNativeEvent(HLERequestContext& ctx)`: 32(u32 index, copy handle): the client hands back its handle for an acquired slot and gets the slot's event again; the handle descriptor and handle come before the index. Games call it for slot 0 right after binding it to vsync. Title 1's main thread waits on this event until it has fired twice before it submits any GPU work, and hung there while it never fired.
- `void BindVsync(HLERequestContext& ctx)`: 261(u64 display name, u32 index): fire the slot on every vblank.
- `void BindChange(HLERequestContext& ctx)`: 263/265/267/269/271(u64 display name, u32 index): fire the slot when the display's refresh rate, HDR status, HDR paper white, buffer max resolution or mode capability changes, so the game re-queries it. Fire once so the game reads the current state, then leave the slot quiet; HDR status and paper white fire again when the host monitor changes (NotifyHdrChange). Kirby Air Riders re-queried its HDR status on every present while this slot fired with vsync.
- `void UnbindNativeEvent(HLERequestContext& ctx)`: 262..272 even (u64 display name, u32 index): detach the slot from its source.
- `static const std::chrono::nanoseconds VblankInterval{[]`: EDEN_NX2_VSYNC_HZ=<rate> (experiment): the game starts frames on this event, so a frame that misses a 60 Hz tick idles until the next one.
- `std::array<Kernel::KEvent*, 8> m_native_events{}`: 31's slots, also listed in m_events Shared with the vsync timer callback, which can outlive the session by one call.

### `src/core/hle/service/nx2_present.h`

- `inline android::BufferTransformFlags Nx2PresentTransform(u32 flags)`: Native present bit 18 requests vertical inversion (observed in Kirby's edition program).

### `src/tests/video_core/nx2_present.cpp`

- `in test "Native upscale uses active regions inside larger texture allocations"`: Handheld Xenoblade Chronicles 2: word 0 is 160 although its blocks are 128 wide.
- `in test "Native upscale accepts only the formats it can copy"`: Kirby and the Forgotten Land had these two in its constants around scene changes.
