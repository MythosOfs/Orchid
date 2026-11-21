# 0013. Native program capture ends at the geometry exit handler

Status: accepted, 2026-11-21.

## Context

`Shader::Ampere::ProgramCaptureState` (`program_capture.h`) reads a native Switch 2 program from GPU memory one instruction at a time and decides where it ends. Programs have no length field: the capture ends at an unconditional EXIT past every forward branch target, or, once the program made a real call (an unpredicated `CALL.REL.NOINC`), at the RET that closes the last called function.

Geometry programs compiled with an exit envelope start with five setup instructions (LEPC, `MOV R3, <handler>`, IADD3, IADD3.X, the install instruction) and end with a three-instruction handler (IMAD.MOV, the handler op, EXIT). `AnalyzeGeometryExit` (`geometry_exit.h`) accepts an envelope only when the handler's EXIT is the last captured instruction (`handler + 3 == count`, error "native exit handler location").

Kirby Air Riders 1.3.3 (issue 111) has envelope geometry programs whose body calls a function placed right before the handler (body `CALL.REL.NOINC` at 0x8770 into 0x8f90, function RET at 0x9180, handler at 0x9190 in program 0x12d9fc1000). The capture took the call as a reason to wait for a RET after the main EXIT, ran past the handler into the programs that follow in memory (6645 instead of 2332 instructions) and the analyzer rejected the program. Every draw with that geometry stage was skipped (`Draw skipped (no pipeline)`). In a lavapipe run the intro race HUD disappeared at the moment these programs were rejected, matching the HUD and menu loss the issue reports.

## Decision

Once the five setup instructions name the handler, the unconditional EXIT at `handler + 2` ends the capture, whatever calls the body made. Functions of an envelope program live inside the body, before the handler; the analyzer already rejects branches and calls that leave it.

## Consequences

- Checked offline against 50,585 dumped programs of all games in `~/Orchid/orchid-issues` (old against new capture plus lowering): only three Kirby Air Riders geometry programs change, and all three now lower.
- A fourth Kirby Air Riders envelope (0x12d9cb1900) stays rejected for another reason: its body reads R3 (the handler offset the setup loads) behind predicated writes, which the straight-line liveness check in `AnalyzeGeometryExit` cannot prove dead ("native instruction before exit setup state is dead").
