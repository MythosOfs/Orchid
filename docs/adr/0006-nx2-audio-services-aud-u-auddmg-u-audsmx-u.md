# 0006. nx2 audio services (aud:u, auddmg:u, audsmx:u)

Status: accepted, 2026-11-20 (moved out of code comments, see [0001](0001-no-code-comments.md)).

## Context

Switch 2 titles reach Switch 1 audio through aud:u and add sysmix streams: ring buffers in game memory that the system drains at the stream's rate.

## Decision

Back aud:u with the registered audout/audren managers, expose one stereo 48 kHz virtual device, play supported sysmix formats through host sink streams and drain the others in real time.

## Consequences

Formats the host cannot play are drained silently; the drain timer and event signalling keep the game's audio thread from hanging.

## Decision log

One entry per former code comment, by file, named after the code it described.

### `src/core/hle/service/audio/nx2_audio_device_manager.h`

- `struct Nx2SysmixStream`: Switch 2 sound mixer streams (nn::audio::sysmix, behind audsmx:u). A stream is a ring buffer in the game's own memory: CreateStream hands over its address and size (4 KiB aligned), OpenStream its format and length in frames, and once the streams are engaged the system takes frames out of the ring at the stream's sample rate. Stereo and six-channel 48 kHz S16 or F32 streams play through a host sink stream of their own (Star Fox has all its sound there); other formats are drained in real time so the game still sees its audio being played (Switch Sports' menu movie feeds one). The ring's shared indices, in frames modulo (frame count + 1): the system's read index at buffer + 0x44, the game's write index at buffer + 0x1004; the frames start at buffer + 0x1040.
- `u32 owner{}`: the user session whose timer drains it
- `u32 data_mode{}`: command 7's mode; 0 leaves the data event alone
- `s32 data_threshold{}`: free frames that signal the data event
- `AudioCore::Sink::SinkStream* host{}`: plays the frames, null for formats it can't take
- `inline std::shared_ptr<Nx2SysmixStreams> Nx2SysmixStreamList()`: Streams are created through audsmx:u but started and stopped through auddmg:u (AttachStream and DetachStream), so both services share one list.
- `inline void Nx2SysmixCloseHost(Core::System& system, Nx2SysmixStream& stream)`: Call with the list locked.
- `class INx2AudioVirtualDeviceManager final : public ServiceFramework<INx2AudioVirtualDeviceManager>`: Switch 2 virtual output devices (nn::audio::ListVirtualOutputDevices and friends), reached through the object auddmg:u command 0 returns. Eden plays everything through one stereo 48 kHz output, so there is one virtual device. The nx2: name keeps the other commands auto-stubbed and logged.
  - 0(u32): u32 count, the device names in the out buffer (0x100 bytes each).
  - 100(u32): the default device's name in the out buffer.
  - 300(16-byte stream format, name buffer): whether the format is supported (result only).
  - 401(u32, name buffer): the device's 0x228-byte property block in the out buffer. The sdk hands the game the name, the channel count (0x108) and the u64 at 0x218; with the zeroed placeholder reply it saw 0 channels and Switch Sports failed to set up audio (2153-1604).
  - 500(16 bytes, name buffer): an event signalled when the device's state changes.
  - 700(u32 stream id, u32 device functionality, name buffer): attach a sysmix stream to the device, which starts it (nn::audio::devmgr::AttachStream, behind OutputStream Start).
  - 800(u32 stream id): detach it again (Stop).
- `in IsStreamFormatSupported()`: Unplayable formats are still drained in real time, so every format stays "supported" (Switch Sports' menu movie relies on that).
- `void GetVirtualOutputDeviceProperties(HLERequestContext& ctx)`: Property block: name (0x000), u32 type (0x100, 3), u32 (0x104, 2), u32 channel count (0x108), u32 sample rate (0x10c), physical output name (0x110), its id (0x210), u64 latency in ns (0x218), u32 device id (0x220). The sdk's output streams pass the device id to sysmix CreateStream, which refuses 0 (Switch Sports failed with 2153-1604 on a zero there).
- `class INx2AudioDeviceManagerService final : public ServiceFramework<INx2AudioDeviceManagerService>`: Switch 2 "auddmg:u": command 0 (16 bytes and the pid) returns the virtual device manager.
- `class INx2SysmixStreamSession final : public ServiceFramework<INx2SysmixStreamSession>`: The stream object IUserSession command 1 returns. Commands (ids, request -> reply):
  - 0(u32 sample type, u32 channels, u32 sample rate, u32 frames, u32 prefill, u32 flags): open.
  - 1: close. 2: u32 state. 3: f32 gain. 4(f32 gain, s64 ramp ns): set the gain.
  - 5 / 6: acquire (copy handle) / release the state-change event.
  - 7(u32 mode, s32 threshold) / 8: acquire (copy handle) / release the data event.
  - 9(s64 ns): clock the stream. 10(s32 frames) -> u32: prime. 11(s32 frames) -> u32: purge.
  - 12(name buffer): rename.
- `in ~INx2SysmixStreamSession()`: The drain timer signals these events under the lock: unlist the stream first.
- `void OpenStream(HLERequestContext& ctx)`: States the session reports: 0 created, 1 open, 2 open and engaged.
- `in AcquireDataEvent()`: The game waits on this before its first write, so signal it at once when the ring already has room (it is empty right after OpenStream); the drain timer keeps it up.
- `class INx2SysmixUserSession final : public ServiceFramework<INx2SysmixUserSession>`: The session audsmx:u command 0 returns. Commands:
  - 1(u32, u32 creation flags, u64 pid, u64 buffer address, u64 buffer size, name buffer): u32 stream id and the stream object.
  - 2 / 3(buffer of u32 stream ids): engage / disengage those streams.
- `static constexpr u32 MaxHostBlocks = 6;`: Blocks of about 5 ms the host may hold; past that the frames stay in the ring and the game waits for room, so a host that plays slower than emulated time doesn't overflow its buffer.
- `static void Drain(Core::System& system, Nx2SysmixStreams& state, u32 owner)`: Takes up to one interval's worth of frames out of each engaged ring (frames owed while a ring is empty are dropped, as an underrun plays silence), and signals the data event of every open stream with at least its threshold of free frames, engaged or not: the game fills the ring before it engages the stream.
- `in Drain()`: Consume even if the copy fails: a stalled ring would also stop the data event below and hang the game's audio thread.
- `class INx2AudioSoundMixerService final : public ServiceFramework<INx2AudioSoundMixerService>`: Switch 2 "audsmx:u": command 0 (16 bytes, the pid and a copy handle) returns the user session.

### `src/core/hle/service/audio/nx2_audio_user_client.h`

- `class INx2AudioUserClient final : public ServiceFramework<INx2AudioUserClient>`: Switch 2 "aud:u" (nn::audio::CreateUserClientSessionByHipc). The SDK reaches the Switch 1 audio interfaces through it: command 2 returns an object used exactly like IAudioRendererManager (vtable slot 1 = GetWorkBufferSize with a Switch 1 parameter block), so it is backed by Eden's audren:u implementation. Command 1 is nn::audio::OpenAudioOut's manager, called like IAudioOutManager (slot 1 = OpenAudioOut), backed by audout:u. The nx2: name keeps unknown commands auto-stubbed and logged.
- `std::shared_ptr<T> SharedManager(const char* name)`: The managers must be the registered audin:u / audout:u / audren:u instances. A fresh IAudioOutManager owns its own AudioCore::AudioOut::Manager, but AudioManager accepts only the first registered buffer-release callback, so sessions opened through a second manager never release buffers: the title queues 3 silent buffers and its audio thread stalls.

### `src/core/hle/service/audio/nx2_sysmix_pcm.h`

- `inline std::vector<s16> ConvertPcm(std::span<const u8> raw, bool is_float, f32 gain)`: Keep interleaved FL, FR, C, LFE, BL, BR order for SinkStream's existing downmix.
- `bool ReadRing(std::span<u8> raw, u32 read, u32 count, u32 capacity, u32 frame_bytes,`: The reader takes a payload-relative byte offset. Failed copies must not consume the ring.

### `src/tests/core/nx2_sysmix_pcm.cpp`

- `in TakeSamples()`: ReleaseBuffer applies the microphone boost; cancel it for this sample-queue check.
- `in test "Sysmix six-channel PCM reaches the sink and stereo downmix"`: SinkStream's established coefficients: front + .596 center + .354 LFE + .707 rear.
