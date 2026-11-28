# Housekeeping: test shortcuts, worktree cleanup, disk budget

Detail for [AGENTS.md](../../AGENTS.md); every rule there applies here too.

## Test shortcuts for the maintainer (final build before a release)

Create the shortcut only for the final release build, not for intermediate builds. It is done
only when its exact `Exec` passes `--check`; the maintainer must never have to click twice.

1. File `~/.local/share/applications/orchid-test-<label>.desktop`:
   ```ini
   [Desktop Entry]
   Type=Application
   Name=Orchid <label> (Test)
   Exec=env EDEN_XYZ=1 /home/<user>/.local/bin/orchid-test-run <label> <absolute path to AppImage or build*/bin/orchid> %f
   Icon=orchid
   Terminal=false
   Categories=Game;Emulator;
   X-Orchid-Test=true
   ```
   The desktop launcher expands neither `~` nor `$HOME`: write the absolute home path in `Exec`. Set environment
   switches only via `env VAR=value` before `orchid-test-run`. Quote paths with spaces in `"..."`.
   Do not set `QT_QPA_PLATFORM` or `DISPLAY`; `orchid-test-run` does that.
2. Check; all three must be green (from the repo root):
   ```sh
   desktop-file-validate ~/.local/share/applications/orchid-test-<label>.desktop
   python3 tools/check_test_shortcut.py <file>   # parses Exec (no shell), rejects `~` and `$HOME`, runs `orchid-test-run --check` -> "check ok"
   ldd <binary> | grep 'not found'    # must be empty (not needed for AppImages)
   ```
3. `update-desktop-database ~/.local/share/applications`.

Why (2026-11-07): Hyprland 0.56 provides Xwayland only as `/tmp/.X11-unix/X0_`, but Qt-xcb looks
for `X0` and aborts after 0.5 s with exit 134, which looks as if the shortcut does not start.
`orchid-test-run` creates the link `X0 -> X0_` itself, checks the display with `xprop` and reports
start failures as a desktop notification. If a test still does not start: `coredumpctl list` and
`~/orchid-perf/<newest>/meta.txt` (exit code).

## Clean up worktrees and builds when a task is done

When an Orchid task is finished and its worktree/build is no longer needed, clean up yourself
without asking. Covers worktrees under `~/.t3/worktrees/Eglantine/`, `~/src/Eglantine-*`,
`~/src/Eglantine/.claude/worktrees/`, `~/Documents/Codex/*/work/`, their `build*/` directories,
test binaries under `~/Orchid/orchid-builds/` and private builds under `~/.cache/orchid-mutation/`.

- Remove only if all hold: `git status` shows no code changes (only logs/builds),
  `git rev-list HEAD --not --remotes` is empty, no process has its cwd/binary there, no
  `orchid-test-*.desktop` points to it, no running T3 thread works in it.
- Then `git -C ~/src/Eglantine worktree remove --force <path>`, delete the local branch (its tip
  is on the remote), `git worktree prune`.
- Worktrees with unsaved work stay; their old `build*/` directories may go (ccache rebuilds).
- Delete test binaries in `~/Orchid/orchid-builds/` together with the `orchid-test-*.desktop`
  shortcuts once the maintainer is done testing. Do not delete game data (`.nup`, dumps).
- Do not touch: the main checkout `~/src/Eglantine` and its `build/`, worktrees of other
  repositories (e.g. `~/Development/Eglantine-Official`, Djihads80), your own running worktree.

### Disk budget (clean up regularly, not only at task end)

Limit 100 GB for everything agents create: the worktrees above, their `build*/` directories,
`~/Orchid/orchid-builds/`, `~/.cache/orchid-mutation/`, the compiler cache `~/.cache/orchid/`,
perf runs in `~/orchid-perf/` (where `orchid-test-run` writes) and the older archive `~/Orchid/orchid-perf/`, and the harness state
`~/.local/share/codex-orchid-mcp/state/` (it cleans itself, see [emulator-tests.md](emulator-tests.md)). Not counted and never deleted for
space: game data (`.nup`, dumps), the main checkout, issue attachments in
`~/Orchid/orchid-issues/`. A source checkout is about 0.5 GB, a build directory several GB, so
only a handful of worktrees with builds fit: one worktree per active task, removed when it ends.

Check at the start and end of every Orchid task, and before creating a worktree:

```sh
du -sch ~/.t3/worktrees/Eglantine ~/src/Eglantine-* ~/Documents/Codex ~/Orchid/orchid-builds \
  ~/.cache/orchid-mutation ~/.cache/orchid ~/orchid-perf ~/Orchid/orchid-perf \
  ~/.local/share/codex-orchid-mcp/state 2>/dev/null | tail -1
```

Above the limit, clean until below it, in this order, each only under the safety rules above:
1. private mutation builds in `~/.cache/orchid-mutation/` that no run is using (rebuilt on demand),
2. test binaries in `~/Orchid/orchid-builds/` that no `orchid-test-*.desktop` points to,
3. finished worktrees, oldest first,
4. `build*/` directories of worktrees untouched for 7 days (ccache rebuilds them),
5. perf runs older than 14 days, keeping `audio-baseline/` and the latest baseline run per game.

Never clear the compiler cache for space. If still above the limit after all safe steps, tell
the maintainer what blocks (e.g. worktrees with unpushed work); do not delete unsaved work.
