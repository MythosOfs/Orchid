# 0002. nx2 graphics-system server and its sessions

Status: accepted, 2026-11-20 (moved out of code comments, see [0001](0001-no-code-comments.md)).

## Context

Switch 2 titles reach the graphics system through `nx2:gsv` and the sessions it hands out (syncpt, nvsrv, gpu_time, native memory and images, GameChat, camera, stream play). Nothing documents these protocols: their layouts were read from the titles' statically linked SDKs, from the shared `changes.patch` (see NX2_NOTES.md) and from failures in real games.

## Decision

Serve every Switch 2 graphics-system session on one host thread of its own (`RunNx2GsvServer`), answer each command with the reply layout the SDK reads, and let unknown commands fall through to the logged `nx2:` auto stub. The entries below record each layout and the failure that forced it.

## Consequences

Unknown commands still get zeroed replies and some layouts are guesses; "SDK evidence (#187, #188)" below lists what was identified and what is still open. Changing a reply below breaks the title named next to it.

## SDK evidence (#187, #188)

Read on 2026-11-21 from the statically linked SDK of Mario Tennis Fever (title version 327680, addresses below are offsets in its `sdk` module) and checked against Star Fox (131072); Donkey Kong Bananza (458752) sends the same commands. A command counts as identified when its client was found and the reply fields it reads are known.

Identified:

- gsv 201 then 202(u64): nn::syncpt::sdk::InitializeEx, 202 replies the syncpt session (handle word 3). gsv 204: see `AttachSyncptMemory`.
- syncpt 1 GetEventSlotCount -> u32 (< 0x400); 21(u32 min(count, 63)) ReserveEventSlots -> result only; 22(u32 slot) GetSyncPointEvent -> copy handle; 201(slot, counter, u64 value, u8 mode) RequestSyncPointEvent -> result only, the client clears the slot's event afterwards; 202(slot) CancelSyncPointEvent -> result only, the client then waits on the slot's event in a 1 s loop (see `auto*& event`). The impl table is at 0xe02da8; MakeCounterId is a single `mvn`, so counter ids carry no base.
- gsv 601() then 602(u64 1): nn::camera::sdk::CreateCameraContext; 602 replies the camera context session. 603() CameraFinalize -> result only, also sent when 602 fails.
- gsv 903() and 1003(): the two Finalize requests of the video decoder client (901/1001 initialize), both result only.
- nvhost-tsg-gpu: 0x01 BIND_CHANNEL(int fd), 0x02 UNBIND_CHANNEL(int fd), 0x08 SET_TIMESLICE(u32), 0x0b BIND_CHANNEL_EX(24 bytes: channel fd, subcontext id, 16 zero bytes; falls back to 0x01 when it fails). The client (NvRmGpu, TSG create at 0x5b6610, the timeslice vtable entry at 0x5b743c) reads no output of any of them.

Open, with what is known:

- syncpt 1's count (32 here): the client only bounds it; the console's value is unknown.
- 204 RO page u32 at +4 ("base id"): copied into the client's state (+0 of the context at 0xece948+8); no reader found in either SDK and counter ids don't use it. 0 is written.
- camera context CMIF 3, 21 and 29 (see `INx2CameraSession`; Welcome Tour and Mario Kart World, no scope title) and their results without the camera accessory: not read again. CMIF 4, 22, 24, 25 and 28 (Star Fox only, auto-stubbed): the client is nn::camera::UserInterface's CMIF proxy in Star Fox's `subsdk3`; its 21 request builders were found, the mapping from command id to method and reply layout was not finished.
- No 426/428/1211 calls were observed in the earlier scope-title gate. Mario Kart World exercises these commands, as recorded below.
- gsv 5, 101, 113, 120-125, 131, 133, 187, 211-215, 303, 424, 425, 436, 461-468, 493, 613, 614, 802, 803, 813, 814, 1011, 1012, 1091, 1112, 1113, 1203, 1602, 1603 and nx2:gpu 23, 24, 25, 34, 51, 8001-8004: no scope title sent them in the harness runs up to 2026-11-18, so they were not read.

## Follow-up evidence for #187 and #188

The registered compatibility replies identify service, transport, command and substituted
behavior once per key in an emulator process. The generic auto stub also logs its first
request per key and retains the request dump and optional unimplemented-function report.
NVDRV identifies the device and raw ioctl, with buffer lengths for echoed/zeroed success,
using its existing first-eight / every-4096th rate limit. These diagnostics preserve the
guest replies. The original #187 goal remains open. The command lists above, gsv 126's
flag, the VI2 list in ADR 0003, and auto-stubbed camera commands still need client or console
evidence. A named result-only operation can still omit state changes, as with syncpt 21.

| Field or operation | Evidence and confidence |
| --- | --- |
| gsv 204 RO page +4, u32 | High confidence the client copies that word. PR #208 found it in the Mario Tennis Fever 327680 and Star Fox 131072 clients, with no reader identified. Counter conversion in the same client inverts the id directly. That supports the current counter indexing, but leaves the word's meaning and hardware value unknown. The original bring-up wrote zero, so Orchid retains zero as a compatibility value. [PR #208](https://github.com/MythosOfs/Orchid/pull/208), [bring-up commit](https://github.com/MythosOfs/Orchid/commit/63f61e109f2d303b62f8b908dbacadc13b79408f) |
| syncpt 1, slot count | High confidence the client expects a u32 less than 0x400, then bounds its reservation request. Low confidence that 32 is the hardware count. Orchid retains 32. [PR #208](https://github.com/MythosOfs/Orchid/pull/208) |
| gsv 426/428 descriptor +0x10, block height | Low confidence in the descriptor formula. Orchid returns a log2 height in rows from its native-image layout calculation, which can use preference +0x10. Decoder block-linear images use the decoder GOB height exponent plus 3. Public DRM documents a separate NVIDIA encoding whose exponent is in GOBs. It corroborates the general layout model and shows why the units need checking. Neither it nor the fork confirms the console's reply encoding. [Linux v6.12 NVIDIA block-linear modifier](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/include/uapi/drm/drm_fourcc.h#L785-L805) |
| gsv 426/428 descriptor +0x14, pitch | Low confidence in the descriptor's units and source. Orchid returns the computed byte pitch divided by 64, so the result is in 64-byte units. The fork corroborates Mario Kart World's use of native handles to build camera image pools. Its change supplies the backing range and gives no console value for pitch. [Lichii native image pool commit](https://github.com/zortos293/Lichii/commit/8a4804532ba882a3b4e4bf7860dd6b045985d84b) |
| Camera CMIF 3 | High confidence in the 16-byte name and u64 id on success from Welcome Tour and Mario Kart World client code. The caller accepts 2452-0006. The no-accessory console result and id value remain unknown. Orchid preserves success and id 0. [Camera client layouts](https://github.com/MythosOfs/Orchid/issues/188) |
| Camera CMIF 21 | High confidence in the readable event handle on success from the client attaching it to a system event. The no-accessory result and signal behavior remain unknown. Orchid returns success and a never-signalled event. [Camera client layouts](https://github.com/MythosOfs/Orchid/issues/188) |
| Camera CMIF 29 | High confidence in the result-only reply shape. The client accepts 2452-0003, 2452-0006 and 2452-0007 in addition to success. Which conditions select those results is unconfirmed. Orchid preserves success. Nintendo's accessory documentation confirms the requirement for compatible USB hardware for video, and gives no service result codes. [Camera client layouts](https://github.com/MythosOfs/Orchid/issues/188), [Nintendo camera accessory](https://www.nintendo.com/us/gaming-systems/switch-2/accessories/camera/) |
| Camera CMIF 4, 22, 24, 25, 28 | High confidence in the command ids and CMIF transport. Each appears in both saved Star Fox PR #208 A/B logs. The caller in version 131072 is the camera interface proxy in `subsdk3`. Method mapping, output fields and no-accessory values remain unknown. The auto stub still returns 32 zero data words and success. [PR #208](https://github.com/MythosOfs/Orchid/pull/208) |

Local Mario Kart World run `20261121-172405-cc54b0cb` reached the first Grand Prix race.
In `eden_log.txt`, five 1211 allocations each created three 384x216 images, and the
client called both 428 and 426 for all 15 handles. The replies and logged size of
0x60000 describe Orchid's layout. This is evidence of the call path and client tolerance.
Hardware captures with differing dimensions or layouts are still needed to determine
the console field values.

## Decision log

One entry per former code comment, by file, named after the code it described.

### `src/core/hle/service/nvdrv/devices/nx2_stub_device.h`

- `inline std::atomic<bool> g_nx2_driver{false};`: Set once a Switch 2 NvRm has identified itself (0xD0-byte GetCharacteristics). Devices then answer ioctls they don't know with a logged, input-echoing success instead of NotImplemented.
- `inline bool Nx2LogStubbedIoctl(std::string_view device, u32 ioctl)`: Stubbed ioctls a game repeats (per frame) were logged every time, on the thread serving the game's request: the first few of each kind, then one in 4096.
- `std::fill(output.begin(), output.end(), u8{0});`: Echo the input (in/out ioctls keep their fields), zero the rest.
- `in Stub()`: nvgpu TSG: subcontext create hands out a veid; bind/unbind/delete and set timeslice succeed; the SDK probes the rest and must see them unsupported. 0x08 (u32 timeslice) is NvRmGpu's "failed to set TSG timeslice": sent right after the TSG is created, only when the characteristics report it (field 0x3c >= 8 and flag bit 8), so the console supports it; its failure was logged and the TSG kept the default timeslice.

### `src/core/hle/service/nx2_gsv.h`

- `inline Kernel::KClientSession* CreateRawSession(Core::System& system, HLERequestContext& ctx,`: Creates a session served by `iface` and returns its client end. Unlike PushIpcInterface this works from a domain too, for callers that want a raw session handle.
- `inline void TipcReplyWords(HLERequestContext& ctx, std::span<const u32> words)`: Replies to a TIPC request with success and raw u32 data words. ResponseBuilder::Push(Result) writes the CMIF two-word result, which shifts TIPC data one word past where the client reads it (and past the copied size); TIPC has a one-word result followed directly by the data.
- `inline void TipcHandshake(HLERequestContext& ctx, std::string_view session)`: TIPC 0(u64 token) on the Switch 2 sessions: a handshake newer SDKs send first; the reply echoes the token. (The auto stub's reply only echoed it by leaving the request's words in place.)
- `class INx2SyncptSession final : public ServiceFramework<INx2SyncptSession>`: Placeholder for the Switch 2 syncpoint server (nn::syncpt), handed out by gsv 202 after gsv 201. TIPC 1 (GetEventSlotCount) returns a u32 the SDK requires to be < 0x400 and then passes min(value, 63) to TIPC 21 (ReserveEventSlots), which replies the result only. Everything else is auto-stubbed through the nx2: prefix.
  - TIPC 0(u64): handshake, echo the token (newer SDKs).
  - TIPC 22(u32 slot): an event handle for that slot (copy handle at word 3, result at word 4). NVN's NvOsEventPool asks for slots and multi-waits on them.
  - TIPC 201(slot, counter, u64 value, mode): signal the slot's event once the counter reaches value. The counter is a SyncCounterId, the bitwise NOT of the host1x syncpoint id; mode 0 compares 32-bit wrapping, mode 1 64-bit. Eden's guest syncpoints back it.
  - TIPC 202(slot): cancel the slot's request.
- `in ~INx2SyncptSession()`: A registered action may be running on the GPU thread right now: mark the session dead under the lock it signals under, so it can't touch a request or event closed below.
- `void Handshake(HLERequestContext& ctx)`: Newer SDKs (title 1) open with TIPC 0(u64 token) and abort unless the reply echoes it.
- `struct SlotRequest`: Map nodes are stable, so a registered action can hold a reference to its request.
- `struct SharedState`: Outlives the session (actions hold a reference): signalling happens under `mutex` and only while `alive`.
- `in RequestSlotEvent()`: A new request starts unsignalled. Slots are reused by different client objects: title 1's movie player got slot 0 after NVN's fence waits had left it signalled, took that as a decoded frame and aborted when the decoder had none.
- `in RequestSlotEvent()`: The action runs on the GPU thread (after the manager's lock; a cancel waits for it), or inline if the value is already reached.
- `auto*& event = m_events[slot];`: nn::syncpt::sdk's cancel always waits on the slot's event after this reply, so a cancel signals it like a completion, also when nothing was pending; the client rechecks the counter (TIPC 103) itself. Title 1 hung forever there when the GPU thread ran a frame behind (EDEN_NX2_FRAMES_AHEAD) and a mid-frame wait outlasted NvOsEventPool's 100 ms, and its movie player cancels each slot before its first request: signalled only for pending requests, the player never got to start a movie.
- `struct Nx2NativeObjects`: Native memory and images handed out by the Switch 2 graphics server (gsv 111/114/423/429/315), shared with the vi2 session so presents can find an image's memory. Command meanings follow the shared changes.patch (IGraphicsSupervisor), re-checked against title 4's SDK.
- `u64 address{}`: CPU (== GPU) address once bound by 114/423
- `u32 count{1}`: images in the pool; handle id + i is image i
- `u64 offset{}`: into its memory (images of a decoder pool share one)
- `bool decoder{}`: a video decoder output image (gsv 911)
- `class INx2ChatGsvSession final : public ServiceFramework<INx2ChatGsvSession>`: GameChat's graphics side (nn::chat::gsv), handed out by gsv 1202. Read from the SDK's nn::chat::gsv::Initialize: TIPC 1() returns a u32 id at word 3 (it aborts with 2162-0001 if the id is over 0x3ff), then TIPC 11(u32 id) returns an event (copy handle at word 3, result at word 4) that it attaches as a system event. 103 (OpenVirtualDisplay: u64 name packed from up to 8 chars, u32 slot id) and 121 (CreateMetadataPool: u64 pool cookie, u64 count, u32 slot id) only read the result; the SDK picks the slot id itself. Everything else falls through to the nx2 auto stub.
- `class INx2CameraSession final : public ServiceFramework<INx2CameraSession>` (open work: #188): nn::camera::sdk camera context (gsv 602). The titles' statically linked camera client talks CMIF on it (not TIPC). Layouts read from that client in Welcome Tour and Mario Kart World: 3(char name[16]) -> u64 camera id. Opens "Default"; the client tolerates result 2452-0006 here. 21(u64 camera id) -> copy handle, attached as the camera's system event. 29(40-byte image preference, u64 camera id) -> result only. Mario Kart World asks it for four preferences (first word 0x180 = 384, the 384x216 images) every frame and tolerates 2452-0003, 2452-0006 and 2452-0007. The answers are success and camera id 0; the event 21 hands out is never signalled.
- `void CheckImagePreference(HLERequestContext& ctx)`: Polled every frame by Mario Kart World, so it is logged once.
- `in RegisterHandlersTipc()`: nn::codec::video::detail::ipc::Finalize (FinalizeServerContext in newer SDKs) sends 903() after closing its sessions and reads only the result.
- `in RegisterHandlersTipc()`: Same request as 429: nn::chat::gsv::CreateImagePool (GameChat camera images).
- `in RegisterHandlersTipc()`: 912(u32 image id): the video group's x12, freeing one decoder output image (Switch Sports frees its four when its movie ends). Unlisting it stops the decode thread writing frames into memory the game reuses.
- `void CheckPermission(HLERequestContext& ctx)`: Feature permission checks (201, 301, 401-403, 1101, 1102, 1501-1503): each tests one bit of the permissions the session was opened with and fails when it is clear. Games only ask for features they are granted.
- `void AttachGpuAddressSpace(HLERequestContext& ctx)`: 434(u32 id from nx2:gpu 33): the session takes a reference on that GPU address space.
- `void RegisterClientHandle(HLERequestContext& ctx)`: 11(copy handle): handed over once per session and kept; a second call fails.
- `void SetFeatureVersion(HLERequestContext& ctx)`: 1111(u32): a version or mode number the server checks and records.
- `in SetFeatureVersion()`: Hyrule Warriors sends it ~50 times a second with the same value (89% of its log lines).
- `static size_t TipcDataOffset(const u32* cmd_buf)`: Index of the first raw data word of a TIPC request: header (2 words), then an optional special header with pid and copy/move handle words.
- `void OpenVideoSession(HLERequestContext& ctx)`: nn::codec::video::detail::ipc::Initialize sends 901, 902, 1001, 1002. 902 and 1002 return a session handle at word 3 and the result at word 4; left to the auto stub, the handle was 0 and InitializeVp9Decoder failed with 2001-0114 (title 1 aborts when a movie starts). Newer SDKs (nn::codec::video::detail::InitializeServerContext) send 906(u8, u64 decoder library version) instead of 902; title 4 sends version 0x3'00030000 and aborted with InvalidHandle at its first movie while 906 was auto-stubbed. The SDK of later titles (ipc::Initialize(bool)) sends 905(u8) instead of 902 when its flag is set; the reply is the same. 1001/1002 open the encoder's creator session (CreateVideoEncoderService uses it).
- `void OpenCameraSession(HLERequestContext& ctx)`: nn::camera::sdk::CreateCameraContext sends 601() then 602(u64 1), which returns the camera context session (handle at word 3, result at word 4). Auto-stubbed, 602 had no handle, the SDK sent 603 (destroy) and title 1 (1.8.0) aborted with 2034-0000. Its commands are answered by INx2CameraSession.
- `void OpenChatSession(HLERequestContext& ctx)`: nn::chat::gsv::Initialize sends 1201 then 1202, the same pair as 901/902 and 1001/1002. Left to the auto stub it aborted with 2162-0001 (GameChat), so 1202 gets a session too.
- `in OpenSyncptSession()`: Used by nn::syncpt::sdk::InitializeEx; handle at word 3, result at word 4.
- `static constexpr u32 SyncptValuesOffset = 0x40;   // RO, u64 per syncpoint`: nn::syncpt::sdk::InitializeEx sends two transfer memories (0x1000 bytes RW, then 0x2000 bytes the client maps read-only) and expects the server to have written a header into the second one: u32 version (must be 1) at +0, u32 base id at +4, u32 count at +8 (the client requires ((count + 0x40) >> 6) - 1 < 0x40), then offsets of: the u64 syncpoint values (RO, +0xc), the RW "armed" summary word (+0x10), the RO summary word (+0x14), the RW armed bits (+0x18) and the RO acknowledge bits (+0x1c). To wait, the client makes its RW bit differ from the RO bit, signals the first returned event (it attaches that handle as writable), and waits on the second until the server makes the bits equal again; it then rereads the value and rearms if the threshold isn't reached. Reply: the two handles (words 3 and 4), then the result.
- `static constexpr u32 SyncptValuesOffset = 0x40`: RO, u64 per syncpoint
- `static constexpr u32 SyncptRoBitsOffset = 0x1100`: RO, up to 0x40 u64 words
- `static constexpr u32 SyncptRwBitsOffset = 0x100`: RW, up to 0x40 u64 words
- `in AttachSyncptMemory()`: RequestParser doesn't skip TIPC handle words, so read the raw data words.
- `in AttachSyncptMemory()`: Mirror each guest increment as it happens, before the waits it meets are answered: the client's TryWaitNativeSync reads this page, and a client woken by an answered wait (nx2:syncpt 201) before the next poll read the old value. Yoshi and the Mysterious Book's movie player then aborted on purpose (2162-0001) a few minutes in. The mirror is the page's only writer (it also fills it here): a poll that copied the values too could put an older one back over a mirrored increment.
- `rb.PushCopyObjects(ctx, *m_syncpt_events[0])`: signalled by the client
- `void PollSyncpts()`: Answers every armed wait: the value page is current, so the client rechecks its threshold and rearms if it still has to wait. Acknowledging without a value change costs the client a wakeup per poll, which keeps waits whose syncpoint we never see from hanging.
- `in PollSyncpts()`: A wait armed on a syncpoint whose value just moved is woken too: re-arming the same bit leaves the bit set unchanged, and a client that only re-arms would otherwise sleep until its own timeout (Ocarina of Time: 1 s freezes, every channel waiting). The manager says which moved; the page cannot, the mirror has already updated it (0.10.0 compared the page and woke nobody: Yoshi and the Mysterious Book stuck loading levels).
- `void CalculateRequirements(HLERequestContext& ctx)`: nn::gsv::shim::CalculateRequirements(u64* size, u64* alignment, preference, u32): the 0x40-byte InterModuleUserImagePreference (words 2..17) and a u32 (word 18). A block-linear preference (MakeInterModuleBlockUserImagePreference) holds kind 2 at word 3, (width - 1) | (height - 1) << 16 at word 4, two u32s at words 6-7 and the pixel format at word 8. Reply: result, u64 size, u64 alignment; the client raises the alignment to at least 4 KiB and rounds the size.
- `void CalculateNativeImageSize(HLERequestContext& ctx)`: 435(preference[16], u32 count): result, u64 size of a GPU native image.
- `void CreateNativeMemory(HLERequestContext& ctx)`: 111(u8 mode at word 2, u64 size): result, u32 native memory id.
- `void ConfigureNativeMemory(HLERequestContext& ctx)`: 115(u32 mode, u32): replies a u64 optional memory mode; 0 keeps the nvdrv-compatible path (an empty reply leaves the request's mode word where the SDK reads it).
- `void BindNativeMemory(HLERequestContext& ctx)`: 114(u32 id, u64 address, u64 size, u64 flags, u64 offset): back native memory with process memory.
- `void BindNativeGpuMemory(HLERequestContext& ctx)`: 423(u32 id, u64 address, u64 size, u32 parent GpuMemoryHandle, u64 offset, ...): back native memory with part of GPU-accessible memory.
- `void CreateNativeImage(HLERequestContext& ctx)`: 429(preference[16], u32 count, u32 memory id): result, u32 native image id.
- `in CreateNativeImage()`: nn::gs::sdk::CalculateNativeImageHandle(pool, i) is pool + i, so keep those free.
- `void CalculateChatImagePoolSize(HLERequestContext& ctx)`: Pool size of a feature's images (1291 here; 391, 691, 891 and 1691 for the other feature groups, which share the server template). nn::chat::gsv::CalculateRequiredBufferMemory: 1291(preference[16], u32 count), the request of 1510, replies result, u64 size, u64 alignment. Auto-stubbed, the size became 0x1000 and the camera pool (1211) got memory for none of its images.
- `void QueryNativeImage(HLERequestContext& ctx)` (open work: #188): 428(u32 native image handle) nn::gpu::sdk::QueryNativeImage and 426(u32 handle, u32, u32 address space, u8 permission) nn::gpu::sdk::MapNativeImage, used by NVN for images another server created (GameChat's camera pool from 1211). Reply words 3-4 the GPU address of the image (426 only; GPU == CPU address, see Nx2NativeObjects::Memory), 5-6 its last-inclusive mapping address, 7-38 a GpuOunceUserImageDescriptor and 39 the flags. The SDK requires descriptor byte 0x00 == 1, byte 0x20 == 1 and a non-zero size at 0x2c (offset at 0x28); NVN reads (width - 1) | (height - 1) << 16 at 0x08 and the layout kind at 0x24 (1 takes RGBA formats 0x25/0x37/0x38, 2 the 0x99-0x9c formats). For block height and pitch Orchid uses the layout calculation described in the follow-up evidence above.
- `void ConfigureWindowImagePool(HLERequestContext& ctx)`: 315(u32 image, u32 count) / 314(u32 image): add/remove a window alias image pool.
- `in OpenViSession()`: Used by nn::vi2::sdk::Initialize; takes one u64, returns a session (handle at word 3, result at word 4).
- `in OpenGpuSession()`: Used by nn::gpu::sdk::BootWithRemoteMemory; takes one u64, returns a session (handle at word 3, result at word 4) that the GPU SDK keeps.
- `in OpenNvsrvSession()`: The SDK reads the handle at word 3 and the result at word 4. nvsrv speaks the nvdrv INvDrvServices protocol (Open/Ioctl/Close/..., 13 = SetGraphicsFirmwareMemoryMarginEnabled), so serve it with Eden's nvdrv on the shared module. The nx2: name keeps unknown commands auto-stubbed and logged.

### `src/core/hle/service/nx2_image_layout.h`

- `enum class Nx2NativeImageFormat : u32`: Pixel format IDs in InterModuleUserImagePreference word 6.
- `inline Tegra::ColorSpace Nx2NativeImageColorSpace(Nx2NativeImageFormat format, bool told_hdr)`: The colour space of a presented native image. FP16 is linear scRGB by its format. RGB10A2 holds PQ BT.2020 only while the game was told the display is HDR: Fire Emblem: Fortune's Weave presents SDR in RGB10A2.
- `struct Nx2ImageLayout`: Block-linear layout of an InterModuleUserImagePreference (16 words): word 2 holds (width - 1) | (height - 1) << 16, word 4 the block height (log2 of GOB rows), word 5 the row pitch in 64-byte GOBs. Formula from changes.patch (native_image_layout.h); preferences outside it fall back to the format's bytes per pixel (word 6: 8 for FP16, else 4) and 128-row blocks.
- `in Nx2CalculateImageLayout()`: 4 KiB, not one block (8 KiB at 128 rows): Star Fox places its window textures at offset 0x1000 in their memory, and NativeWindow::Initialize fails on a misaligned offset, leaving the NVN window without a native window (null call in NativeWindow::Acquire).

### `src/core/hle/service/nx2_gsv_server.h`

- `inline std::atomic<ServerManager*> g_nx2_gsv_server{};`: The server for the Switch 2 graphics system sessions: nx2:gsv (ISelfController 2000) and every session handed out from it (vi2, gpu, syncpt, nvsrv, video decoder, ...), on a host thread of its own. They used to be registered with AM's server, a guest service thread on emulated core 3: vi2's present (229) waits there for the previous frame's GPU fence and then for the next vblank, and for that time core 3 ran no game thread and no other graphics request (GPU submissions through nvsrv included) was answered. One thread, so these handlers stay serialized among themselves as before. EDEN_NX2_GSV_HOST_THREAD=0 keeps them on AM's server.
- `in RunNx2GsvServer()`: Cleared by ~ServerManager before it goes away (also when refused at shutdown): later sessions fall back to the server of the session that creates them.

### `src/core/hle/service/nx2_streamplay.h`

- `class INx2StreamPlayService final : public ServiceFramework<INx2StreamPlayService>`: Switch 2 stream play (GameShare), stpl:u. Command 0 (u32 0x12) opens the user service the SDK keeps for the process; without an object the first nn::streamplay call jumps through null.

### `src/core/hle/service/nx2_stub_interface.h`

- `inline constexpr std::string_view Nx2StubPrefix = "nx2:";`: Prefix of placeholder interfaces for Switch 2-only IPC. ReportUnimplementedFunction answers every call on such an interface with an empty success, so boot can go on while the log shows which commands are used.
- `static const char* InternName(std::string name)`: Names must outlive the interface, so they are interned here.
