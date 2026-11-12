# Dynamic frame rate per game

How each supported game can run at another frame rate than its fixed 60 FPS without changing
its speed. All options are per game (Properties > System) and take effect on the next start.
Status 2026-11-12; "checked" names what was actually run.

| Game | Option | How it works |
|---|---|---|
| Super Mario Bros. Wonder | Frame rate (FPS, game patch) | Game patch, see [wonder-dynamic-fps.md](wonder-dynamic-fps.md) |
| Mario Tennis Fever | Frame rate (FPS, HIGHLY EXPERIMENTAL game patch) | Game patch, see [MarioTennisFeverFpsInvestigation.md](MarioTennisFeverFpsInvestigation.md) |
| Donkey Kong Bananza | Dynamic frame rate (variable refresh rate display) | No patch: the game has its own controller for VRR displays |
| Pokémon Pokopia | Frame rate (FPS) | No patch: the game measures real time |
| Hyrule Warriors: Age of Imprisonment | Keep game speed below 60 FPS | The emulator writes the game's frames-per-update variable |

## Donkey Kong Bananza: VRR display

`nn::vi::QueryDisplayVrrSupported` reads bit 63 of the refresh rate flags that nx2:vi2 command
107 (`QueryDisplayModeCapability`) returns. With `vrr_display` on, the game uses its VRR frame
rate controller (main+204640 in update build E25C90C1) instead of the swap interval one: it picks
40 to 60 Hz in steps of 2 from its measured frame time, sets it with
`SetLayerExtendedSwapInterval` and sends it as the refresh rate in present word 12, which
`PaceToVblank` already follows. Below 40 it falls back to swap interval 2 (30 FPS).

Checked: mining tunnel route, rates 40 to 60 seen in the log, no new log errors. Not checked: a
heavy area on the desktop, and other games that import the same functions (Wonder, Yoshi,
Orbitals, Star Fox).

## Pokémon Pokopia: present rate

The game's speed does not depend on the frame rate: its menu background moved equally fast at
30, 60 and 100 FPS, and the character walked the same distance in the same time at 60 and at
about 75 FPS. `timed_fps_target` therefore only changes the rate at which the emulator shows
frames (`GetActiveGameFps`), as for the patched games.

Checked: cave route with target 120 (88 FPS on average in the test harness), final checkpoint
matched. Not checked: long play, cutscenes.

## Hyrule Warriors: Age of Imprisonment: frame step

The game simulates a whole number of 60 Hz frames per rendered frame and keeps that number in a
variable (main+28966a8, update build 659CB288); it only sets 1 or 2 and never looks at the real
frame time, so below 60 FPS it ran in slow motion (measured: 50 % speed at 30 FPS). With
`frame_step_fps` on, the present path writes 1 or 2 there from the measured frame time
(`Core::FrameStepFps`). The game waits that many frames itself, so the result is 60 or 30 FPS at
full speed; below 30 FPS it still slows down. Above 60 FPS is not possible this way.

Step 3 is not used: one boot with it ended in a jump through a bad pointer.

Checked: mission route at normal speed (steps mostly 1, blocks of 2 after slow frames, no CPU
exceptions); with the step forced to 2 at 30 FPS the game's own counter advanced at 99 to 100 %
of real time. Not checked: a PC that is really too slow for 60 FPS in combat.
