# Fire Emblem: Fortune's Weave (`04007EF01F8AE000`) — bring-up notes

Base: `nx2-20261031` (`60ef6d275`), branch `fe-fortunes-weave`. Linux build (CachyOS,
GCC 16, radv 26.99.99 / RX 9070 XT), `eden-cli`, loose ExeFS dump with update `131072` (2.0.0).

## State (2026-10-31)

Boots and runs at 60 fps, but the picture stays black: the native (Ampere) shader lowering
rejects the shaders the game dispatches, so every draw is skipped (`Draw skipped (no pipeline)`)
and the compute dispatches are dropped (`Native QMD failed: native shader dispatch required`).
The game logic itself keeps running (~56 presents/s, vblank-paced, no hangs, no crashes).

## How to run a loose dump from the CLI

`eden-cli` opens `<path>/main`, so the path must be the **exefs directory**, not the game folder:

```
SDL_VIDEODRIVER=wayland WAYLAND_DISPLAY=wayland-1 \
  ./eden-cli -g "/home/leon/Switch2/Fire Emblem Fortune's Weave/updates/131072/exefs"
```

The update's `romfs/` is found next to that `exefs/` directory. Log: `~/.local/share/eden/log/eden_log.txt`.

## Blockers, in order of first hit

`lower_to_maxwell.h` fails on the first offending instruction of a program, so later modes
surface only once the earlier ones are lowered.

Lowered already (2026-10-31, this branch): `ISETP.EX` (64-bit compare high word), `REDUX.MIN`,
`SHF` with a constant source (`0xa19`).

Lowered so far, each verified by a run: `ISETP.EX`, `REDUX.MIN`, `SHF` with a constant
source (`0xa19`), `IMNMX` with a 32-bit immediate (`0x817`), `IMAD.HI.S32` (`0x827`), `IMAD.HI`
with a constant multiplier (`0xa27`), `IADD3.X` with a constant source (`0xa10`), `LEA.HI.X`
with a constant base (`0xa11`), `ATOMS.MIN/MAX` (`0x38c`), `BAR.SYNC.DEFER_BLOCKING` (`0xb1d`),
`BPT.TRAP` as a no-op (`0x95c`), and `LDG` through `desc[URn]` (`0x981`).

Still rejected (fail-fast: the next one only surfaces once this one is lowered):

| Message | Instruction |
|---|---|
| native high effective address mode | `211 034f5211 000000ab 010f0eff` (4 programs) |
| native integer multiply mode | `824 9a2b7824 00000001 008e0603` |
| native surface descriptor buffer conflict | `b9b 00007b9b 8080480c 00100f00` |
| native formatted surface store mode | `b9b 02007b9b 60005804 0010af00` |
| unsupported native opcode | `370 1c1d7370 08000000 000001ff` (TXQ.B, bindless handle) |
| unsupported native opcode | `fae 020d7fae 00000000 0e101c48` |
| unsupported native opcode | `311 00047311 00000004 0021d800` (F2I.64) |
| native integer carry/third source | `c10 0c0a7c10 00000004 0ff9e0ff` |
| native global store uniform addressing mode | `986 02007986 00000005 0c101104` (the `desc[URn]` store twin of the above) |
| native matrix multiply | `23c 0a08723c 0000000c 00000008` — needs `EDEN_NX2_TENSOR=1` |

Also seen, not yet shader related:

- `Unmapped Device ReadBlock` bursts — guest reads inside ranges that never got mapped.
- `nx2:gsv` TIPC `11` unimplemented (falls back to the auto stub).
- `Load: Failed to read title for ROM` — no control NACP in a loose dump, expected.

## What the nxnative repo says about this game (for reference)

`git.eden-emu.dev/penis6/nxnative-penis6`, profile `dm/games/04007EF01F8AE000`. That project is
not an emulator — it recompiles the game's own binaries and rebuilds Switch graphics on Metal —
so its patches do not transfer, but its game profile does:

- 2.0.0 / `131072`, SDK 22.2.0, modules `main subsdk0 subsdk1 subsdk2 sdk`, NVN2.
- Shaders live in RomFS as `.bfsha.zst` / `.ptcl.zst`: 16796 of 16809 programs convert offline.
- Memory: reserve `0x1400000000`, alias `0xC00000000`, stack `0x10000000`, app pool `0x263000000`;
  the game asks for a 36 GB allocator at boot.
- Movies are AV1; there the host's decoder needed the CPU-buffer path, forced by patching the
  movie player in `main`. Not relevant to an emulator, which runs the console path.
- DLSS goes through a temporal scaler.

The same SASS semantics keep biting both projects (FSEL source mods, FSET with a uniform,
IMAD.HI's 64-bit addend, F2I.U64's high word, calls returning into their block, jump tables in
`c1`) — ours already lower all of these; the modes above are the ones neither has yet.
