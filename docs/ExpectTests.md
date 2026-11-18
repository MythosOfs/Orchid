# Expect tests

An expect test (also called a golden or snapshot test) lets the code produce a readable text
dump and compares it with a reference file stored in the repository:

```
input (e.g. captured shader words)  ->  code under test  ->  text dump  ==  src/tests/expect/<name> ?
```

- Equal: green.
- Different: red, with the first differing line.
- Intended change: rerun with `ORCHID_EXPECT_UPDATE=1`, which rewrites the reference. The diff
  goes into the commit and is reviewed there; the commit message says why it changed.

Nobody types the expected values by hand: they are generated and then read. The test still pins
down what was learned about the hardware, and every behaviour change, including one nobody
thought about, appears as a diff in review.

## What they do not replace

An expect test only shows *that* the output changed, not whether it is still correct. Tests that
check semantics, such as the small Maxwell machines in `ampere_kirby_fsel.cpp` (NaN payloads,
predicates, R/UR aliasing), stay. The two complement each other.

## Usage

```cpp
#include "tests/expect.h"

TEST_CASE("...", "[video_core][expect]") {
    Tests::CheckExpect("name.ir", Tests::StripIrAddresses(Shader::IR::DumpProgram(program)));
}
```

- References live in `src/tests/expect/`; CMake passes the path as `ORCHID_EXPECT_DIR`.
- A missing reference fails instead of being created, so a forgotten file never passes.
- Dumps must be deterministic. `IR::DumpProgram` prints each instruction's address;
  `StripIrAddresses` removes it.
- Create or update references: `ORCHID_EXPECT_UPDATE=1 build-local/bin/tests "[expect]"`, then
  `git diff src/tests/expect/`.
- Tag every expect test `[expect]` so the update command reaches all of them.

`src/tests/video_core/expect_shader_ir.cpp` is the first example: two FSEL instructions captured
from Kirby and the Forgotten Land go through `LowerToMaxwell`, `TranslateProgram` and all
optimization passes, and the resulting IR is compared with `kirby_fsel_pair.ir`.

## Where more would pay off

| Area | Dump | Today |
|---|---|---|
| Ampere to Maxwell lowering of captured sequences | IR after `TranslateProgram`, as in the example | about 230 test cases in 30 `ampere_*.cpp` files, most with a hand-written machine |
| Optimization passes on hand-built IR | `IR::DumpProgram` before/after the pass | counts of opcodes (`storage_buffer_*.cpp`) |
| Texture descriptors (TIC, GOB, swizzle) | decoded fields as `key = value` | single `CHECK`s |

Use only the short captured sequences already in the tests as input. Whole game shaders are game
code and stay out of the repository.

## Mutation check

`orchid-agent-test` runs expect tests like any other test. Measured 2026-11-18 on the FSEL
lowering (`lower_to_maxwell.h` lines 3166-3168): the single expect test and the three
hand-written machine tests in `ampere_kirby_fsel.cpp` (260 assertions) each killed the same 10 of
12 mutants. Both missed the same two: widening `Bits(87, 3)` reads bits that are zero in the
captured instructions, so the output does not change. An expect test catches what its input
exercises; for wider coverage, add inputs, not assertions.
