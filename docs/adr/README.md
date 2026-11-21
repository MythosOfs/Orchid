# Architecture decision records

Decisions and facts the code cannot show. Code in `src/` carries no comments ([0001](0001-no-code-comments.md)); look up a symbol here instead.

| ADR | Covers |
|---|---|
| [0001](0001-no-code-comments.md) | No code comments, the ratchet, the four verdicts |
| [0002](0002-nx2-graphics-system-server-and-its-sessions.md) | nx2:gsv and its sessions: syncpt, native memory and images, GameChat, camera, nvsrv, stub interfaces |
| [0003](0003-nx2-display-session-vi2-and-frame-presentation.md) | vi2 display session, present pacing, swap interval, vsync events, display queries |
| [0004](0004-nx2-video-decoder-service.md) | Video decoder service and `Nx2VideoDecoder` |
| [0005](0005-nx2-gpu-session-gpu-memory-and-command-synchronisation.md) | GPU session, GPU-only and sparse regions, device ranges, GPU-written pages, semaphores |
| [0006](0006-nx2-audio-services-aud-u-auddmg-u-audsmx-u.md) | aud:u, auddmg:u, audsmx:u sysmix streams |
| [0007](0007-nx2-diagnostics-switches.md) | `EDEN_NX2_*` profiling, tracing and capture switches |
| [0008](0008-host-and-container-fixes-from-the-fork-scan.md) | KIP and NUP parsing, play time and config saves, orphaned profiles, motion, Windows partial unmaps, kernel shutdown order, present and sampler failure paths |
| [0009](0009-built-in-game-patches-in-the-add-ons-tab.md) | Built-in game patches in the Add-Ons tab; Yoshi fixed-resolution patch words |
| [0010](0010-shader-folding-and-ir-operands.md) | Bitfield constant folding, constant-buffer texture handles, immediate phi operands |
| [0011](0011-kernel-thread-termination.md) | Scheduler and process lock ordering during thread termination |
| [0012](0012-cmif-output-buffer-contents.md) | Preserving guest bytes in partially written CMIF output buffers |
| [0013](0013-native-program-capture-geometry-exit-envelope.md) | Native program capture end at the geometry exit handler (issue 111) |
| [0014](0014-vulkan-pipeline-build-ownership.md) | Vulkan pipeline build claims, completion and progress lifetime |
| [0015](0015-texture-cache.md) | Texture cache upload and write-back |
| [0016](0016-guest-memory-spans.md) | Guest address-space span lookup |
