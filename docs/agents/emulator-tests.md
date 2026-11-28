# Emulator tests: harness, gameplay gate, performance, sound

Detail for [AGENTS.md](../../AGENTS.md#emulator-tests-on-the-test-host-run-by-the-agent); every rule there applies here too.

## Harness facts every test brief relies on (`orchid` MCP)

- A route belongs to the snapshot (save state) it was recorded on. `route_list` shows it, `boot`
  takes it as `snapshot=...`, and `route_play` refuses a session booted from another one. From the
  default `initial` (a fresh save) the same inputs end in tutorials and intros.

  | Game | Snapshot | Route | Ends in |
  |---|---|---|---|
  | Mario Tennis Fever | `mtf-after-first-practice-20261107` | `mtf-stadium-match-v4` | Free Play match (waits for the player HUD) |
  | Donkey Kong Bananza | imported save (see `gate_list`) | tunnel entry v4 (see `gate_list`) | mining tunnel, HUD checkpoint |
  | Yoshi and the Mysterious Book | `yoshi-chapter-one-saved-20261106-2114` | `yoshi-wildwoods-entry-v2` | Wildwoods level, Yoshi controllable |
  | Super Mario Bros. Wonder | `wonder-goal60-overworld-20261107` | `wonder-level-entry-v2` | inside a level, HUD checkpoint |
  | Pokémon Pokopia | `pokopia-intro-language-set-20261107` | `pokopia-cave-v3` | first cave, character controllable |
  | Hyrule Warriors: Age of Imprisonment | `hw-blur-mission-20261106` (2x; the gate overrides to 1x) | `hw-mission-v2` (long: `route_start`) | mission, objective text checkpoint |
  | Star Fox | `starfox-tutorial-controls-loop-20261107` | `starfox-flight-v3` | flight tutorial, Arwing controllable |
  | Kirby and the Forgotten Land | DLC save (see `snapshot_list`) | `kirby-stage-v2` | stage "Meteor Shower's Arrival" (no HUD at full health: checkpoint on the mission screen before, confirm on the screenshot) |
  | Orbitals | `initial`, boot with `settings={"player_1_connected":"true"}` (co-op: needs a second controller) | `orbitals-gameplay-v2` | ship interior, both characters controllable |
  | Xenoblade Chronicles: Definitive Edition | no save: New Game (see `snapshot_list`) | `xbde-battle-v2` | prologue battle |
  | Xenoblade Chronicles 2 | no save: New Game (see `snapshot_list`) | `xb2-field-v2` | first battle, Rex controllable |
  | Xenoblade Chronicles X: Definitive Edition | the maintainer's save (see `snapshot_list`) | `xbx-field-v2-a` | field, minimap HUD |

  Snapshots and routes imported from the maintainer's saves are described by scene above; get
  the concrete IDs from `snapshot_list` and `route_list`. All twelve are registered for the gate
  (`gate_list`). Kirby Air Riders has no route on purpose: its races can hang the GPU (README:
  do not play).

- Routes should wait for screens, not for seconds: `wait_checkpoint(name, buttons=..)` waits until
  a recorded screen region appears (optionally pressing a button meanwhile) and is saved into the
  route, so a replay survives longer loading. `route_list` shows `screen_waits` per route; a route
  with 0 is timed inputs only (`route_play` then keeps the 35 s lead-in it was recorded with) and
  where it ended must be confirmed on the last screenshot. A failed checkpoint says how close the
  screen came ("closest rmse 0.090, limit 0.08"): close = limit too strict or another build's
  picture, far = wrong scene.
- Checkpoints: region on HUD that only exists in that scene, at most 120000 px (then the picture
  may sit up to 8 px elsewhere, which it does between builds), `max_rmse` 0.12 for opaque HUD and
  0.2 for text on a translucent bar over the 3D scene, recorded on the current build and checked
  twice. HUD that hides at times (between tennis points) needs a wait, not one look.
  `checkpoint_list` shows which are usable.
- Recording a route: explore first, then a clean boot with only the needed steps, then
  `route_save` (it keeps the time between your steps as waits, so do nothing extra in that boot),
  then replay from a fresh boot. If the run that found the way is already stopped,
  `route_from_run(name, run_id, final_checkpoint=..)` builds the route from its input log. Routes
  longer than 240 s replay with `route_start` (a job). Brief for a subagent: the template
  `~/.local/share/codex-orchid-mcp/briefs/record-route.md` plus the game's details, one game per
  brief; several games in one brief ended early every time (2026-11-11).
- Every `boot` should pass `bin=<build under test>`; without it the harness's stored default build
  runs and the reply says so. `default_binary_set` replaces the default (the last good build).
  `queue_s=240` waits for the GPU test lock instead of failing while another agent tests (call
  again if it is still held); the
  error names the holder. A session unused for 15 minutes is stopped to free the lock.
- More `boot` options: `settings={"use_docked_mode":"1"}` overrides `qt-config.ini` keys for one
  run (no extra snapshot needed); `audio=True` records the sound (see "Sound checks" below).
  `boot_ui` opens the frontend without a game; drive it with `click`, `hotkey`, `type_text`.
  `seed_import` makes a snapshot from the maintainer's saves (and settings) or raw save files.
- Judging a run: `health` (ok / stalled / device_lost / gpu_reset / exited; "alive" alone does not
  tell a hang from a running game), `summary` (frame times, log errors, and which error groups are
  new against the baseline of that game and route), `clip` (a few seconds as one sheet of frames,
  for flicker and animation). `stop` returns the summary, the audio report and, after a crash,
  a backtrace from the core dump. `baseline_save` stores a run you checked as "last good".
- Rendering against real hardware: `reference_import` stores frames of Switch 2 footage (video
  URL + timestamp) per game; `reference_compare(game, image)` puts an emulator screenshot next to
  them, and the gate adds a `reference_sheet` per game that has references. A judge subagent
  follows `briefs/judge-rendering.md` (match / differs / broken / unsure); new references:
  `briefs/collect-references.md`, one game per brief.
- What slows agents down (GPU lock queueing, emulator hours per day, gate and build times, OOM
  kills, disk budget): `orchid-bottlenecks [days]`. Run it when the loop feels slow, before
  guessing.
- Long runs are jobs, polled with `job_status(wait_s=240)`: `soak_start` (minutes under watch,
  ends at a hang with thread backtraces), `gate_start`, `desktop_run_start`, `benchmark_start`.
- `boot` returns `out`, the run directory. Screenshots: `<out>/NNNN-<name>.png`. Frame times:
  `<out>/frames/*.csv` (column `frame_ms`, one row per guest frame). Log: what the `log` and
  `wait_log` tools read, on disk `<out>/profile/data/eden/log/orchid_<tag>_<title id>.txt`, after
  `stop` copied to `<out>/eden_log.txt`. The `eden_log.txt` inside `profile/` is an old log from
  the seed, not this run.
- Games run in a private throwaway profile without network. In-game dialogs (EULA, tutorials) are
  part of the game: accept and continue.
- Say in the brief: every reported screenshot must be opened and described. The first real failure
  (crash, hang, device loss, wrong rendering) stops the remaining runs, the gate included: report
  it. An infrastructure fault (harness error, GPU lock timeout, out-of-memory kill, a start that
  never reached the game) is inconclusive, not a failure: write it down and rerun. When a number or
  a log count decides the outcome, check it against the log yourself.
- The harness lives in `~/.local/share/codex-orchid-mcp/` (its own local git repository, not this
  one). A running MCP server keeps the code it started with; changes reach a session only after
  its restart. After changing it: `uv run --script selftest.py` (no emulator needed), then
  `uv run --script livetest.py <binary> core` (real boot; more tests in its header).
- Its `state/` cleans itself at every boot (`cleanup`): profile clones of runs after 2 days, whole
  runs and bundles after 14 days. Screenshots, logs and frame times of a run stay until then.

## Gameplay gate: only before a release

Before starting a release build, boot every game listed under "Project scope" in `README.md`
once and play it briefly into real gameplay (not the title or main menu). Commits, pushes to
`main` and pull requests need no gate: the full gate takes about an hour, too long for every
change. A fix for a specific game is still verified in that game's real gameplay (its route
from the table above, or by hand) before the commit. CI builds run only for a release, started by
hand (`release-binaries`); pushes start none.

- Release freeze: before the gate, write the SHA under test to `~/Orchid/release-freeze`; delete
  the file after publication. While it exists, the local pre-push hook refuses pushes to `main`
  from every worktree (other sessions commit and push afterwards; the release session pushes its
  own fixes with `ORCHID_RELEASE_PUSH=1`). Why: on 2026-11-18 a push during the 0.11 gate forced a
  second hour-long gate.
- Run it with `gate_start(bin=<build>)`: every game registered with `gate_set` (see `gate_list`)
  is booted, played along its route into gameplay and on for about a minute, and stopped; the
  result gives pass / review / fail per game with reasons and one contact sheet of all last
  frames. A scope game without a gate entry is played by hand as before. Skip a game only if it
  is not on the test host or is known not to reach gameplay yet (README table); name every skipped game.
- The harness cannot judge rendering: open the contact sheet (and single screenshots where it is
  too small), and have the `reference_sheet`s judged against hardware (`judge-rendering.md`;
  look at every `differs`/`broken` yourself). After a gate you accept, `gate_accept` stores its
  runs as the new baselines.
- Pass: each game renders correctly (characters, world, HUD, text without missing glyphs), runs
  without hang, crash or device loss, and its log shows no new errors compared with the last good
  build. A game that worked in the last good build and fails now blocks the release.
- The test subagent runs it and returns screenshots and log excerpts. The main agent looks at
  the key screenshots and decides pass or fail.
- The first real fail stops the gate (`job_cancel`): report it, find the regression, fix it, run
  the gate again; no release meanwhile. An infrastructure fault is inconclusive: rerun that game.

## Performance measurements

- Compare builds using the same private harness display, snapshot, route and settings. Report
  guest frame times as private-display measurements. Record the scene, sampling window, host
  contention and screenshots. Desktop FPS requires a separately authorized desktop measurement.
- Measure in the real scene (an actual match or gameplay, not menus or videos) at 1x resolution
  scaling, and judge against the fps cap. Loop (baseline first, one change per run): skill
  `orchid-fps-bench`.
- fps can be bimodal from host scheduling (issue 128 swung between about 21 and 30 fps with
  identical code). So: at least 3 runs per variant, alternating A and B, always with a baseline of
  the previous good build. A change counts only if it clearly exceeds the spread between runs of
  the same variant. Check scheduler churn in `threads.log`/`meta.txt` before blaming the code.
- A/B comparisons keep scene, settings, resolution, power profile and cache state identical, name
  build/commit and measurement duration, and run one load test at a time.
- `summary.txt` is a starting point and its bottleneck verdict a heuristic: back every finding
  with the CSV, `perf-top.txt` and `eden_log.txt`.

## Sound checks (dropouts show up as gaps in the waveform)

`boot(audio=True)` of the `orchid` MCP unmutes the game into a private null sink, keeps the
speakers silent and records `<out>/audio.wav`; `stop` reports its silences (`middle_silences` =
between start and end). `readiness.audio.streams` in the boot reply must be 1 or more; 0 means
the sound is on the speakers instead: stop at once. By hand the same looks like this:

```sh
M=$(pactl load-module module-null-sink sink_name=orchid_test)
parec -d orchid_test.monitor --file-format=wav rec.wav &   # start before the game, kill afterwards
# ... PIPEWIRE_NODE=orchid_test PULSE_SINK=orchid_test agent-run <binary> -g <game> ...
ffmpeg -hide_banner -i rec.wav -af silencedetect=n=-60dB:d=0.02 -f null - 2>&1 | grep silence_
pactl unload-module $M
```

Orchid's SDL talks to PipeWire directly and ignores `PULSE_SINK` (measured 2026-11-11: the
recording stayed silent and the game played on the speakers); `PIPEWIRE_NODE` is what routes it.

- Silence of 20 ms or more in the middle of continuous music or effects is a dropout (underrun).
  Silence at the start, on loading screens and in real game pauses is normal.
- Always compare with a baseline recording of the last good build in the same scene, and check
  the emulator log for audio underruns.
- Baselines: `~/orchid-perf/audio-baseline/<game>-<scene>.wav` (the directory appears with the first baseline), one fixed scene per game. Create
  the baseline from the last known good build before judging a change, and keep it up to date.
- This finds gaps, not wrong pitch, wrong tempo or missing instruments; for those compare length
  and a spectrogram with the baseline. Verified with a test tone containing an 80 ms gap and
  with a Mario Tennis Fever boot (recording not silent, peak -31.7 dB).
