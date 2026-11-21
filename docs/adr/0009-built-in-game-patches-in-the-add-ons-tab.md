# 0009. Built-in game patches in the Add-Ons tab

Status: accepted, 2026-11-21.

## Context

Yoshi and the Mysterious Book renders below its output resolution and upscales, which looks blurry in Orchid. Walnutoast830 wrote nine fixed-resolution patches for main of build `893C30E75521AFB8B23326EB6AA0E2DC` (game version 1.0.2): 1080p, 1440p and 4K, each with no anti-aliasing, FXAA or DLAA, and permitted shipping all of them in Orchid ([issue 144](https://github.com/MythosOfs/Orchid/issues/144)). Orchid 0.12 shipped only "4K No AA", as the System-tab checkbox "Internal resolution 4K (game patch)".

## Decision

A built-in game patch is a row in the game's Add-Ons tab, next to the installed updates, DLC and mods. Its checkbox turns it on or off; a double-click (`QTreeView::activated`) opens a dialog for its mode, and the row text names the mode. The rows come from the `BuiltinPatches` table in `configure_per_game_addons.cpp` (title, name, on/off setting, mode load/describe/edit/apply). The frame-rate patches (Bananza, Wonder) still live in the System tab and can move into that table later.

Yoshi's mode is two choices, resolution and anti-aliasing, because each of the nine patches is exactly one resolution word set plus one anti-aliasing word set (checked for all nine against the author's files): the code holds six small tables instead of nine overlapping ones, and the dialog shows two short lists.

## Consequences

- Settings: `yoshi_patch` (on/off) keeps the config key `yoshi_4k_patch`, and `yoshi_patch_resolution` defaults to 4K, `yoshi_patch_anti_aliasing` to none. A config written by 0.12 with the old option on therefore loads as 4K No AA without migration code. The three settings have empty translation entries, so no settings tab builds a widget for them.
- `FileSys::ApplyYoshiPatch` checks every original word of the chosen resolution and anti-aliasing sets before changing any, so another game version, or a resolution mod that already changed one of those words, leaves the module untouched. It runs after external mods for the same reason: an installed resolution mod keeps its version and the built-in patch is skipped with a log line.
- The test `[yoshi_patch_dump]` (hidden; `ORCHID_YOSHI_MAIN_NSO=<decompressed main with its 0x100-byte header>`) checks every variant's original words against the real module.

## Decision log

### `src/core/file_sys/yoshi_patch.h`

Offsets are module offsets of 4-byte words; the loader passes the decompressed NSO including its 0x100-byte header. Values are the words as the game stores them (little endian).

- `Yoshi*Sites` resolution sets, UI scale (1440p and 4K only): the HUD reads its DPI curve at the viewport size. 4K reads it at half the size (`scvtf s0, w20` -> `scvtf s0, w20, #1`) and doubles the result (`fadd s0, s0, s0`); the minimum-scale constant is rebuilt in one instruction (0.01 -> 0.00995), which frees the first word for the doubling. 1440p detours through three unused padding words at 0x2D7A4B4 (between two functions, originally zero) to read the curve at size x 3 / 4 and divide the result by 0.75; its minimum-scale constant becomes 0.125. 1080p needs no change.
- Resolution sets, per-tick resolution check at 0x582D400-0x582D444: the handheld and docked sizes the game expects (doubles built by mov/movk), or it resets the output resolution. Both become the chosen size.
- Resolution sets, 0x582D870, the r.SetRes chooser: `ldrb w11, [x9, x8]` -> `mov w11, wzr`, always option 0, the command "r.SetRes 1920x1080". 1440p and 4K rewrite that command's UTF-16 text at 0x70529D8 to "2560x1440" or "3840x2160" (same length, the last character and terminator stay; the 1440p word at 0x70529E0, "x1", is unchanged but still checked).
- Anti-aliasing sets, 0x582D56C, the graphics-mode setter: ignore the per-area mode table and always take one case: 0 (no AA, r.ScreenPercentage 100, dynamic resolution off), 0x19 (FXAA, r.AntiAliasingMethod 1) or 0x35 (DLSS, r.AntiAliasingMethod 4). DLAA also sets that case's stored screen percentage (0x582D65C) and its "r.ScreenPercentage %d" argument (0x582D670) from 50 to 100, so DLSS does not upscale.
