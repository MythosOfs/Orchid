# 0004. nx2 video decoder service

Status: accepted, 2026-11-20 (moved out of code comments, see [0001](0001-no-code-comments.md)).

## Context

Movies on Switch 2 go through `nn::codec::video::detail::ipc` (gsv 902/905/906/1002). The client keeps its own ring of output images and waits on NativeSyncHandles, so decoding order, fences and end-of-stream handling must match what the client counts.

## Decision

Decode with FFmpeg on a thread per session (`Nx2VideoDecoder`), fill output images in registration/release order, count finished frames on a syncpoint of the decoder's own and answer end-of-stream the way each title's player expects.

## Consequences

Fence and EOS semantics are tuned per title; Donkey Kong Bananza still needs its own branch (#190).

## Decision log

One entry per former code comment, by file, named after the code it described.

### `src/core/hle/service/nx2_gsv.h`

- `class INx2VideoSession final : public ServiceFramework<INx2VideoSession>`: The Switch 2 video decoder (nn::codec::video::detail::ipc). gsv 902/906/1002 hand out a session whose TIPC 1 (CreateVideoDecoderService: handle at word 3, result at word 4) creates a decoder service, another session of this class. Layouts read from the SDK's nn::codec VP9 decoder:
  - 200(copy handle: work buffer transfer memory, u64 size, u32, u32, u32): initialize. 201 adds a fourth u32 (a flag that 200 leaves at 1) and 202 that and a u64: the same initialization. 210(copy handle, u32 x4) replies only a result.
  - 101(A: codec name or H264 SPS/PPS; u32 width, u32 height, u32 rate, u32): configure.
  - 3(): the submission-ready event (copy handle at word 3, result at word 4).
  - 6 / 207 / 8: start / stop / flush; 115: finalize.
  - 311(u32 image id): register an output image (gsv 911 creates it); reply its NativeSyncHandle, signalled once a frame has been decoded into the image. The client acquires output images in registration/release order from a ring of its own (no IPC), so frames fill them in that order.
  - 314(u32 image id, u32, NativeSyncHandle): give an output image back; the image may be rewritten once that handle (the client's reads) has passed. Reply the image's next handle.
  - 212(u32 image id): unregister it.
  - 109(A: one compressed frame, u64 pts in us, u8 end of stream): submit input; reply a u64. 400 is the same with a second A buffer after the frame, a 0x838-byte block of frame information; the decoder here only needs the frame.
  - 213(): the next finished frame (16 words): u32 image id, u32 width, u32 height, u32 width, u32 height, u32 colour space, TimeSpan pts (ns), u8 end of stream. A NativeSyncHandle is {u32 SyncCounterId (~syncpoint), u32 mode, u64 value}; clients wait on it with nx2:syncpt 103/201. Each decoder service owns a syncpoint that counts its finished frames. FFmpeg decodes on a thread of the session's own (a 1440p VP9 frame takes milliseconds, and the service thread also serves presents and syncpoint waits) into the layout gsv 961 describes.
- `in ~INx2VideoSession()`: The ready timer runs on the HostTiming thread: clear its event under its lock first.
- `u64 pts`: us
- `std::optional<Tegra::Host1x::Nx2Picture> picture`: none: an empty end-of-stream frame
- `bool after_end`: no frame: an image released after the end of the stream, handed back
- `static constexpr size_t MaxDecodedAhead = 2`: decoded frames held while no image is free
- `in Configure()`: Some titles pass the configuration in an X buffer instead.
- `in GetEvent()`: The client attaches it auto-clearing, so one signal is consumed by the first wait: while the input queue has room, re-signal every 4 ms; a spurious wake only makes it check again.
- `std::array<u32, 4> HandOut(u32 image, std::optional<Fence> release_fence)`: Queues an output image for the next decoded frame; returns the NativeSyncHandle words that signal once that frame is in it.
- `static const bool keep_pending = []` (open work: #190): After the end of a stream nothing fills a released image, so it comes straight back as an empty end-of-stream frame: its fence is the syncpoint's current value and 213 has a frame for it. Mario Kart World releases its images after a movie ends and waits for the returned fences (a fence for a frame that never comes hung it on a black screen after its title movie). Hyrule Warriors' movie player re-acquires each image it releases, waits that fence and asks 213 for the image's frame; an empty reply (0x1c286f) froze the game on a movie's last frame. The image stays out of m_free and uncounted, so later frames (the player keeps submitting empty end-of-stream inputs) still fill images in the order their fences count. EDEN_NX2_VDEC_EOS_KEEP_PENDING=1 keeps such fences pending until Stop or Finalize instead (Djihads build 177 behaviour). Donkey Kong Bananza always keeps them pending: it acquires the handed back frames, then flushes and restarts its looping hint movies and gives both images back, and now and then aborts (2162-0001) right after the restart; its pending fences are filled by the restarted movie. Rechecked 2026-11-22 on 0687268f00 (#190): with the exception removed and releases completed only once the stream is drained, Bananza still aborted with 2162-0001 after the seventh loop of its tunnel hint movie (one of two runs), right after an end-of-stream frame; main ran the loop for 150 s without it. Keeping a frame written during Flush from closing the restarted stream did not prevent it, so the immediate end-of-stream reply stays the suspect. Runs: candidate 20261122-051721-f8879b6b (abort), 20261122-054740-e46db1cc (no abort), main 20261122-052348-8a164f93.
- `in SubmitInput()`: Words 2-4 are the A descriptor of the compressed frame (400: 5-7 that of its information block), the data words follow.
- `void Flush(HLERequestContext& ctx)`: Flush (8) and Stop (207): drop queued input and frames not yet in an image. Images already handed out keep their place, so the sync values stay in step with the client's ring.
- `std::erase_if(m_outputs, [](const Output& output) { return output.after_end; });`: Images handed back after the end of the stream hold no frame for what follows.
- `in Flush()`: Only Stop: Bananza flushes (8) after its movies end and treats every completed fence as a frame to acquire.
- `void CompleteHandedFences()`: Advances the decoder's syncpoint to the last value handed out (m_mutex held).
- `in GetOutputInfo()`: Nothing finished: the code the client's own Acquire uses for an empty ring.
- `in DecodeLoop()`: Even an image written during a flush counts: the client waits for its value.
- `in DecodeLoop()`: Not while the end-of-stream input is still being decoded either: re-arming here would bring back the spurious wakeups before its frame is out.
- `in DecodeLoop()`: No codec name or H.264 parameter sets at the start of the configuration: take a codec name further in, else H.264 when the stream is H.264 (Yoshi demo).
- `in DecodeLoop()`: The client stops at the frame marked end of stream; give it one even when the last input held no picture. Only a picture from this input may carry the mark: marking an earlier one still waiting for an image hands out one frame fewer than the client counts, and its next 213 finds the ring empty (0x1c286f), which Donkey Kong Bananza panics on as a video ends.
- `in INx2VideoSession()`: Read on the GPU (blitted into the game's own texture): drop what its caches hold.
- `in INx2VideoSession()`: The game samples the image on the GPU: drop the texture cache's copy of the old frame.
- `u32 m_syncpoint{NoSyncpoint}`: Counts finished frames (allocated at the first register)
- `u32 m_sync_base{}`: Its value before the first frame
- `u32 m_handed{}`: Output images handed out (registered or released) so far
- `std::deque<u32> m_free`: Output images, in the order the client acquires them
- `bool m_stream_ended{}`: End-of-stream frame handed out, nothing submitted since
- `bool m_end_queued{}`: End-of-stream input submitted, its frame not out yet
- `void CalculateDecoderImagePoolBuffer(HLERequestContext& ctx)`: nn::codec::CalculateDecoderImagePoolBuffer(size*, alignment*, VideoDecoderImagePreference, u32 count) sends 991(preference[16], u32 count) and reads result, u64 size, u64 alignment; the client raises the alignment to at least 4 KiB and rounds the size up to it. The preference (MakeDecodeImagePreference) holds an output layout code at word 1 (1/4 for format 3, which the movie player uses, read here as NV12) and (width - 1) | (height - 1) << 16 at word 2. Left to the auto stub, the reply words keep the request's, so the size became 0x167027f'00000001 and title 1's movie player aborted (main+0x19e5b58) when its first movie started. Sizes are block-linear NV12 (luma, then half-height chroma, 128-row blocks) or 4 bytes per pixel for the other codes.
- `void CreateDecoderImage(HLERequestContext& ctx)`: 911(preference[16], u32 count, u32 memory id) (nn::codec::InitializeDecoderImagePool): result, the id of the first of `count` decoder output images; the client names image i of the pool id + i. They share the pool's memory, one image size apart.
- `void GetCpuImageDescriptor(HLERequestContext& ctx)`: 961(u32 image id): result, the 128-byte nn::gsv::CpuUserImageDescriptor that nn::gs::sdk::ReadUserImage* (behind nn::codec::ReadDecoderOutputImage*) copy an image out with: u8 kind at +0; for the pitch-linear kinds (8 RGBA, 9 BGRA, 10 NV12) the plane's offset into the pool memory (u32 +0x8) and row pitch (u64 +0x10), NV12's chroma plane at +0x18/+0x20. Kind 11 is native tiled RGBA, with block columns at +0x10 and block width and height in pixels at +0x14/+0x16.

### `src/video_core/host1x/nx2_video_decoder.cpp`

- `in CodecFromMime()`: "video/av01" or "video/av1". FFmpeg decodes AV1 with libdav1d.
- `std::multiset<u64> pending_pts;`: Timestamps of submitted frames not yet output. Frames copied back from a host GPU decoder lose theirs and take the earliest one instead.
- `std::vector<std::pair<std::vector<u8>, u64>> gpu_packets;`: Packets the host GPU accepted before its first picture, replayed if the CPU takes over.
- `std::vector<u8> packet`: FFmpeg reads up to AV_INPUT_BUFFER_PADDING_SIZE past the data
- `in Nx2VideoDecoder::Decode()`: Hidden VP9 frames never come out: keep the set from growing over a long movie.
- `in Nx2VideoDecoder::Render()`: NV12 chroma rows hold interleaved U/V pairs, so odd widths write one extra byte.
- `in Nx2VideoDecoder::Render()`: Frames copied back from a host GPU decoder are already NV12. Copy them as they are: FFmpeg 6.1's swscale leaves the chroma plane of an NV12 to NV12 copy at zero, which shows the movies green.
- `in Nx2VideoDecoder::Render()`: Software-decoded frames are planar: interleave U and V ourselves instead of using swscale. Full-range frames stay on the swscale path so they are converted to limited range.
- `in Nx2VideoDecoder::Render()`: The YUVJ formats are full range even when the frame leaves its range unset.
- `int space = SWS_CS_ITU601;`: The stream's matrix and range; untagged HD video is BT.709. NV12 output is limited range. Set for every frame: the cached scaler keeps the previous frame's details otherwise.

### `src/video_core/host1x/nx2_video_decoder.h`

- `Nv12,`: Luma rows, then interleaved Cb/Cr rows at chroma_offset
- `enum class Nx2GpuDecodeFault`: Test seam: makes the host GPU decoder fail to open, reject every packet or answer every packet with EAGAIN. NoCpu rejects every packet and makes the CPU decoder fail to open. Replay rejects every packet and makes the CPU decoder reject the last packet it replays from the host GPU. Hold takes every packet without decoding it. SwitchReceive rejects every packet and fails the CPU decoder's receive after the packet that made it take over.
- `class Nx2VideoDecoder`: The decoder behind the Switch 2 video decoder service (nn::codec::video): FFmpeg decodes the elementary stream the game demuxed, in software or on the host GPU (NVDEC emulation setting). Used from one thread at a time.
- `void Drain(std::vector<Nx2Picture>& pictures);`: End of stream: appends the pictures the decoder still holds, then starts over.
- `void Reset();`: Drops everything queued in the decoder (the game flushed it to seek).

### `src/tests/video_core/nx2_video_gpu_fallback.cpp`

- `constexpr std::array<u8, 825> clip`: Four 64x64 H.264 baseline frames (x264): keyframe, frame, keyframe with repeated parameter sets, frame, each led by an access unit delimiter.
- `constexpr std::array<u8, 1017> reordered_clip`: Six 64x64 H.264 main profile frames (x264) in decode order I P B I P B: the decoder holds each I and P frame back until the B frame before it.
- `HostProbe Judge(std::span<const int> events)`: What the nx2 decoder outputs after the host GPU decoder gave these frame formats, with AV_PIX_FMT_NONE for a rejected packet and AV_PIX_FMT_YUV420P once the CPU took over.
- `const HostProbe probe`: How FFmpeg on this host decodes the clip on the GPU: NV12 frames, its own CPU fallback or rejected packets. The probe drains and closes its decoder before the test opens one.
- `FFmpeg::DecoderContext cpu{decoder};`: Like the nx2 decoder, replay the taken packets and switch only if the CPU decoder accepts them and this one.
