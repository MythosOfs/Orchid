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

Still rejected, with the instruction that fails:

| Stage | Message | Instruction |
|---|---|---|
| graphics | native surface descriptor buffer conflict | `b9b 00007b9b 8080480c 00100f00` |
| graphics | unsupported native opcode | `370 1c1d7370 08000000 000001ff` |
| graphics | native shared atomic mode | `38c ffff038c 00000801 00800000` |
| compute | native formatted surface store mode | `b9b 02007b9b 60005804 0010af00` |
| compute | native extended constant add mode | `a10 94052a10 00005b00 007fe4ff` (4 programs) |
| compute | unsupported native opcode | `a27 00022a27 0000a800 078e00ff` (4) |
| compute | native constant LEA carry mode | `a11 0f0d8a11 00005900 010f0eff` |
| compute | native integer min/max mode | `817 00097817 ffffffc0 07800200` |
| compute | native high multiply mode | `827 05067827 66666667 078e02ff` |
| compute | unsupported native opcode | `95c 0000795c 00000004 00300000` |

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
