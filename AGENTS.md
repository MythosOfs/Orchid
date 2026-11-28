# Repository instructions

Applies to the main checkout, all its worktrees (also under `~/.t3/worktrees/` and
`~/Documents/Codex/`) and other checkouts of MythosOfs/Orchid, for every agent and subagent
(including automatic issue, review/merge and fork-scan runs). Machine addresses live in the
maintainer's private `~/AGENTS.md`, his own rules in `~/.claude/CLAUDE.md`, not here.

This codebase is yours: what it's for and how it's built are your call, and you'll keep building on it.

## Commit and push

Standing authorization, unless the user says otherwise: commit and push requested changes
directly to `origin/main` without asking.

- Small, clearly scoped fixes (e.g. emulator UI, status bar): after appropriate checks.
- Larger changes: only after all required tests and checks passed. Failed, skipped or
  unavailable required checks do not count.
- Include only changes of the requested task. Fetch `origin/main` before pushing and integrate
  newer changes. Never force-push `main` or overwrite unrelated user changes. If branch
  protection or permissions block the push, report it; do not bypass.

## Local development builds

`./tools/build-local.sh` builds, `./build-local/bin/orchid` runs the GUI. Keep the build
directory between edits; do not clean or recreate it routinely.

- Configuration: Ninja, ccache, the available faster linker, `-O2 -g1`, no LTO. Builds the `yuzu`
  GUI target (output name `orchid`) and its dependencies, without tests, CLI or the standalone
  room server; emulation and graphics features stay enabled. Development configuration: use a
  separate release/PGO build for performance comparisons, full debug info for variable inspection.
- Needs the normal Linux build dependencies plus `ccache` and Ninja.
- Compiler cache `${XDG_CACHE_HOME:-$HOME/.cache}/orchid/ccache`, shared across worktrees. Use the
  script so its ccache environment and debug path mapping stay consistent. Do not clear the cache
  routinely.
- Parallelism is capped by CPU count and available RAM; override with
  `CMAKE_BUILD_PARALLEL_LEVEL=6 ./tools/build-local.sh`. Avoid several full builds at once on one
  host.
- Existing builds skip explicit reconfiguration (avoids timestamp-induced rebuilds); Ninja detects
  changed CMake inputs itself. Configure explicitly with `./tools/build-local.sh --configure-only`.
- Extra arguments are CMake configure options. `ORCHID_BUILD_DIR` sets another output directory.
  Never share one build directory between source worktrees.
- Share dependency downloads with `-DCPM_SOURCE_CACHE=/absolute/cache/path` (CMake keeps the choice
  in that build directory). On the maintainer's PC reuse `$HOME/src/Eglantine/.cache/cpm`.
- First builds fill the compiler cache and take time. Do not claim a speedup without measuring.
  Verify the build and report limitations. Cached rebuild timings are not timings for changed
  source. Details: [docs/Build.md](docs/Build.md#faster-local-linux-test-builds), measured results
  and their limits: [docs/LocalBuildBenchmark.md](docs/LocalBuildBenchmark.md).
- These settings are for local iteration; preserve release/CI configurations.

GitHub Actions: preserve Release/PGO flags and packaging targets. The release workflow already
uses compiler caching and Ninja; normal Linux builds use mold, PGO builds keep LLD. Before claiming
a CI speedup, read per-job durations and cache statistics in the Actions summaries; local timings
say nothing about hosted runners. Do not cancel or restart an important running release to apply
build optimizations; prepare them for later runs.

## C++ navigation and refactoring (LSP, clangd, ast-grep)

The code is almost entirely C++. For symbol questions (definition, references, call hierarchy,
`workspaceSymbol`) use the `LSP` tool (clangd), not grep. It needs a compile database.

- Once per worktree, in its root: `orchid-lsp-setup` (`~/.local/bin`). It runs
  `tools/build-local.sh --configure-only` (no build), writes a local `.clangd` pointing at
  `build-local/` and excludes it via `.git/info/exclude`. Without it clangd finds no includes and
  `findReferences` sees one file only. The background index (about 3000 files, 30 MB in
  `build-local/.cache/clangd`) builds in a few minutes while clangd runs and stays on disk. After
  editing `.clangd`, kill clangd; the plugin restarts it.
- `findReferences` on widely used classes (e.g. `Scheduler`) returns hundreds of lines. Prefer
  `workspaceSymbol`, `incomingCalls`, `outgoingCalls`, or read only the files you need.
- LSP cannot rename, apply fix-its or debug. Use `clang-tidy --fix -p build-local`, `clang-format`
  and `ast-grep run -l cpp -p '<pattern>' [--rewrite '<new>'] <dir>` (structural search and
  multi-file rewrite). After a rewrite, build and run the tests.

## Code comments: ADRs instead

Code in `src/` carries no comments: name things so the code reads on its own, and write a
non-obvious decision or reverse-engineered fact into the matching ADR in `docs/adr/`
([0001](docs/adr/0001-no-code-comments.md)). The pre-commit hook runs
`python3 tools/comments.py check --staged`, which fails when a file has more comment lines than
`tools/comment-baseline.txt` allows. Code ported from upstream or forks arrives with comments:
delete the ported ones in the same change. To clear a whole section: `/delete-comment`.

## Expect tests

Tests tagged `[expect]` compare a text dump (e.g. shader IR) with a reference file in
`src/tests/expect/` via `Tests::CheckExpect` (`src/tests/expect.h`). When one goes red, read the
diff: an unintended change is a bug; an intended one is accepted by
`ORCHID_EXPECT_UPDATE=1 build-local/bin/tests "[expect]"` and committing the rewritten files. The
commit message says why each reference changed; a reference update without that reason counts as
a failed test. Purpose and where to add more: [docs/ExpectTests.md](docs/ExpectTests.md).

## Mutation check (mandatory part of final verification)

When production code or unit tests change, run it yourself, unasked. Pick the Catch2 filter,
build directory and diff base yourself.

```sh
orchid-agent-test -- "<matching Catch2 filter>"
# Already committed changes:
orchid-agent-test --base origin/main -- "<filter>"
# Test-only changes: name the matching production file (its changed lines come first):
orchid-agent-test --file src/common/example.h -- "<filter>"
# Exact lines of the new logic:
orchid-agent-test --line src/common/example.h:42 --line src/common/example.h:57 -- "<filter>"
```

- Build directory: `build-local` when configured, else `build` (`--build-dir` overrides). It needs
  the tests target: `./tools/build-local.sh -DYUZU_TESTS=ON -DBUILD_TESTING=ON` once per worktree.
- Mutant rebuild timeout defaults to 180 s (`ORCHID_MUTATION_BUILD_TIMEOUT_MS`, at most 300000).
- When the automatic pick lands on lines no unit test loads (e.g. `texture_cache.h`), run again
  with `--line` on the new logic; more than three lines need `--lines N`. A red or incomplete
  run blocks the push like a failing test; hand-made mutants do not stand in for it.

- The runner builds and tests normally, then mutates a limited selection of production code with
  dumbmutate in a private source copy. Default: at most three changed lines, at least 80 % of
  valid mutants detected.
- `--base HEAD` covers unstaged, staged and new production files; for committed branch changes
  pass the matching base.
- Running `build/bin/tests` directly is fine for quick diagnosis but does not replace this run.
- Exit 1 = score below threshold; exit 2 = failed baseline or incomplete/inconclusive check.
  "No matching mutations" is not a pass.
- Surviving mutants: add a test or justify the behavioural equivalence concretely. Never lower
  the threshold to get green.
- Name partial checks (files, line budget) in the result; they do not guarantee freedom from bugs.
- Time limit per line 10 minutes; timeouts count as incomplete.
- Reports: `~/Orchid/orchid-issues/mutation/<run>/`. Install: `~/.local/share/orchid-mutation/`,
  engine `~/.local/share/orchid-dumbmutate/`. No git hooks or global compiler flags. Do not ship or
  start the private builds under `~/.cache/orchid-mutation/`.
- Do not aim ninja diagnostics (`-n`, `-t`) at a build directory while another ninja runs there;
  even diagnostics can corrupt ninja log files.

## Emulator tests: on the test host, run by the agent

Every test that starts the emulator (performance/fps, headless boots, repro runs, A/B
comparisons, screenshots, logs) runs on the RDNA4 test host. The agent runs performance and sound
tests itself, even while the maintainer is at home. Start emulators with `agent-run` (the `orchid`
MCP does this already), keep runs short, end the emulator afterwards. The laptop runs no emulator
tests; it may only compile (see "Builds: test host or laptop"). The maintainer tests only the final
build before a release.

Use the Orchid harness's private display and virtual input for every automated run, including
benchmarks and delegated runs. Keep the maintainer's mouse, keyboard, window focus and active
desktop free. If a measurement requires the active desktop, report the limitation. A desktop run
requires explicit permission for that individual run. Include the private-display requirement in
every test brief.

### Every emulator run goes to the Sol test subagent

Every run that starts the emulator (boot a game, drive the `orchid` MCP, `orchid-test-run`,
repro and diagnosis runs while debugging, screenshots, sound recordings, collecting logs, CSV
and `summary.txt`) goes to a test subagent on GPT-6.2 Sol (`gpt-6.2-sol`), reasoning effort
`medium` (not the default `low`), started with `delegate_task` on the Codex provider. It runs on
the maintainer's ChatGPT subscription and spares the Claude limits.

- This section is the maintainer's standing, explicit request for that subagent. It holds in
  every harness, also where the harness's own rules allow subagents only when the user asks
  (Devin ran a whole debugging session's boots itself on that ground, 2026-11-19).
- `delegate_task` is a T3 Code tool. Where it is not among your tools, call it on the `t3-code`
  MCP server (`orchestrator_capabilities` first, for the provider and option names). If Sol
  cannot be reached, report that and run a test yourself only with the maintainer's go-ahead.
- The main agent (Fable or Opus) writes the brief up front: game, scene, exact steps, what to
  record, return format (file paths, raw numbers, screenshot paths). While debugging, one brief
  per experiment (build, snapshot, settings, environment variables, what to wait for).
- The subagent returns data only. The main agent decides: regression or not, root cause, code
  changes, whether to ship. When a screenshot decides the outcome, look at it yourself.
- All rules in this file and in `docs/agents/` apply to the subagent run (private display and input, short runs, end the emulator afterwards).

Before any emulator run, route, gameplay gate, performance or sound check, or test-subagent brief:
read [docs/agents/emulator-tests.md](docs/agents/emulator-tests.md) (harness facts, gate, performance, sound).
Before creating a test shortcut, cleaning worktrees or builds, or checking the disk budget:
read [docs/agents/housekeeping.md](docs/agents/housekeeping.md).

### Builds: test host or laptop

If the laptop is idle, compile there to relieve the test host. Check:
`ssh <laptop> 'pgrep -x orchid || pgrep -x eden-cli || pgrep -x ninja'` (no output = idle; address
in the maintainer's private `~/AGENTS.md`). One full build per machine at a time; otherwise build
on the test host (`build-scope`/`agent-run`). Copy the finished binary or AppImage to the test
host with `scp` and check it with `ldd`.

<!-- OPENWIKI:START -->

## OpenWiki

This repository has a generated `openwiki/` evidence index. It is optional just-in-time context, not required startup reading.

- Do not enumerate, preload, or search wikis at task start. Use retrieval when the user asks for it, when unfamiliar architecture or dependency behavior materially affects the task, or when source inspection leaves an important uncertainty. Stop once the question is grounded.
- When those conditions apply and OpenWiki retrieval tools are available, use `openwiki_search` for just-in-time context and `openwiki_read` for the relevant complete sections. If search returns `workspace_required`, ask which listed workspace to use and retry with its ID.
- Use `openwiki_list_workspaces` or `openwiki_list_wikis` when workspace membership itself needs to be discovered.
- If the retrieval tools are unavailable, read `openwiki/quickstart.md` and follow its links to the relevant pages.
- Treat source code and tests as authoritative. A brief's unknowns and review items are verification gaps, not automatic requirements.
- Prefer the narrowest quiet validation that proves the changed behavior. Preserve complete failure output.

Use the repository's documented maintenance process to refresh generated pages after source changes. Check its configuration and completed runs before relying on automatic updates. Do not hand-edit generated OpenWiki pages unless explicitly asked; prefer updating source code/docs and letting OpenWiki regenerate.

<!-- OPENWIKI:END -->
