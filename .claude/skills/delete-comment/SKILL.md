---
name: delete-comment
description: Audit and delete every code comment in one section of src/, moving decisions into ADRs and filing issues for hacks.
disable-model-invocation: true
argument-hint: "<files or directory under src/>"
---

# Delete comments in one section

The policy and its four verdicts: [docs/adr/0001-no-code-comments.md](../../../docs/adr/0001-no-code-comments.md). The tool: `python3 tools/comments.py` (run it without arguments for its commands). The section is the paths given as arguments; with none, ask which section, since a wrong guess rewrites thousands of lines.

1. **Inventory.** `python3 tools/comments.py list <files>` prints each comment block with its line and the code it sits on. Done when you have the block count and every block in front of you.

2. **Verdict per block.** Read the code around each block and give it exactly one verdict:
   - *tautology*: the code already says it (names, `} // namespace`, `///< size in bytes` on `size_bytes`);
   - *stale*: it describes code, files, flags or behaviour that no longer exist. Check by grepping: every environment variable, function and file a comment names must still be in `src/`;
   - *excuse*: it lets a hack stand: a guess, an unknown, a stub, a per-title branch, a duplicated constant, `shortcut:` / `ponytail:`;
   - *decision record*: a non-obvious decision or a fact the code cannot show (protocol layouts, why a workaround exists, which title broke, lock order).
   A block can be an excuse with a decision inside; record both parts. Done when every block from step 1 has a verdict and the counts add up to the block count.

3. **Excuses become issues.** One issue per distinct hack on `MythosOfs/Orchid` (`gh issue create -R MythosOfs/Orchid`), quoting each comment verbatim with a permalink to the current commit and stating what a fix looks like. Search open issues first and comment on a matching one instead. Done when every excuse block is quoted in an issue.

4. **Decision records become ADRs.** Add them to the ADR in `docs/adr/` that covers the component, or a new `NNNN-<topic>.md` with Status, Context, Decision, Consequences and a decision log: one entry per former comment, named after the symbol it described, text kept verbatim except stale sentences and excuse parts (those point at their issue). Add new ADRs to `docs/adr/README.md`. Done when every decision-record block appears in an ADR.

5. **Strip.** `python3 tools/comments.py strip <files>` deletes every comment except SPDX and tool directives, and refuses a file if its code tokens would change. Then `python3 tools/comments.py update` lowers the baseline. Done when `list` prints nothing for the section and `python3 tools/comments.py check` passes.

6. **Verify.** Build (`./tools/build-local.sh` with tests on) and run the unit tests the section has; a comment-only diff that changes the build is a strip bug. Done when the build and tests are green.

7. **Ship.** One PR with the stripped files, ADRs and baseline; its body lists the verdict counts, the issues filed and the lines removed (`git diff --shortstat`).
