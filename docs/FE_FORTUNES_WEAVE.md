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

**Graphics path: nothing is rejected any more and no draw is skipped.** The two structural
fixes that got there:

- Surface stores and loads keep their own descriptor buffer (`required_surface_buffer`,
  `SurfaceBoundBuffer()`), so a program can sample a texture from one constant buffer and store
  through a surface in another; the pipeline cache version went to 19 and the native override
  header to seven words for it.
- `SUST.P` takes a partial channel mask (`Bits(72,4)`) instead of requiring all four channels.
- `TXQ.B` on a bindless handle (`0x370`) becomes Maxwell `TXQ (b)` with the handle in the register.
- `CALL` accepts the predicated NOINC forward transfer when the next instruction is `BSSY`
  (`0x947`), the rule the nxnative build uses.

The screen is still black because the *compute* side never dispatches: 24 × `native shader
dispatch required` (a QMD needs a lowered program that was rejected). What is left is one family
per line, all compute:

| Message | Instruction | Needs |
|---|---|---|
| unsupported native opcode | `625 34028625 00005e00 078e0203`, `625 00288625 00005e00 078e0223`, `625 00228625 00005e00 078e0223`, `825 ccd43825 00000010 078e02d2` (4 programs) | IMAD.WIDE: Maxwell's IMAD is not in the frontend, so lo/hi multiply plus the 64-bit cbuf addend must be rebuilt from `xmad` |
| native integer carry/third source | `210 05087210 00000007 07b5e800`, `c10 0402ac10 0000000a 0ffde0ff`, `c10 0c0a7c10 00000004 0ff9e0ff` | IADD3 with a third source and two carry predicates, in the uniform-rewritten form |
| native constant add carry mode | `a10 2c2c8a10 00005c00 07b5e005`, `a10 1a1aca10 00005c00 0783e011` | IADD3 with a cbuf addend, a third source and both carries |
| unsupported native opcode | `fae 020d7fae 00000000 0e101c48` | LDGSTS.E.BYPASS.128: Maxwell has no async copy, so LDG + STS |
| unsupported native opcode | `226 47407226 00000040 000010ff` | IDP.4A.S8.S8: four signed 8-bit products summed |

Done in this pass, each verified by a run: `LEA`/`LEA.HI` with a carry predicate (Pp out of
bits 81-83), `F2I.FTZ.U64.TRUNC` (`0x311`, the auto-exposure of `Lighting.bfsha`), `IMAD.X` with a
carry value and bit 73, `FMNMX` with a dropped predicate output, `HMMA` with a plain addend, and
`IADD3` third-source modifiers.

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
