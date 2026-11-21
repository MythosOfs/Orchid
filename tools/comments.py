#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Orchid Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Comment ban with a ratchet for src/.

check [--staged]  fail if a file has more comment lines than tools/comment-baseline.txt allows
update            lower the baseline to the current counts (never raises it)
list PATH...      print each comment block with its line and the code it sits on, for audits
strip PATH...     delete every comment in the files, verifying the code tokens stay identical
stats             comment lines in scope, split into own lines and trailing comments
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "tools", "comment-baseline.txt")
SCOPE = "src/"
EXTS = {".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".mm", ".glsl", ".comp", ".frag", ".vert",
        ".geom", ".tesc", ".tese", ".java", ".kt"}
EXEMPT = re.compile(r"SPDX-|clang-format (?:on|off)|NOLINT|IWYU pragma")

_C = r"""(?P<line>//[^\n]*)|(?P<block>/\*.*?\*/)|R"(?P<d>[^()\\\s]{0,16})\(.*?\)(?P=d)"|"(?:\\.|[^"\\\n])*"|(?<![\w'])'(?:\\.|[^'\\\n])*'"""
TOKENS = re.compile(_C, re.S)
TOKENS_JVM = re.compile(r'""".*?"""|' + _C, re.S)


def in_scope(path):
    return path.startswith(SCOPE) and os.path.splitext(path)[1] in EXTS


def comments(path, text):
    """(start, end, text) of every non-exempt comment."""
    pattern = TOKENS_JVM if path.endswith((".kt", ".java")) else TOKENS
    out = []
    for m in pattern.finditer(text):
        if (m.group("line") or m.group("block")) and not EXEMPT.search(m.group(0)):
            out.append((m.start(), m.end(), m.group(0)))
    return out


def comment_lines(path, text):
    lines = set()
    for start, end, _ in comments(path, text):
        first = text.count("\n", 0, start)
        lines.update(range(first, first + text.count("\n", start, end) + 1))
    return len(lines)


def code_tokens(path, text):
    pattern = TOKENS_JVM if path.endswith((".kt", ".java")) else TOKENS
    return pattern.sub(lambda m: " " if m.group("line") or m.group("block") else m.group(0),
                       text).split()


def strip(path, text):
    """Deletes every non-exempt comment; lines left empty go, with the blank line they leave."""
    spans = comments(path, text)
    if not spans:
        return text
    pieces, last, marked = [], 0, set()
    for start, end, body in spans:
        first = text.count("\n", 0, start)
        marked.update(range(first, first + body.count("\n") + 1))
        line_start = text.rfind("\n", 0, start) + 1
        line_end = text.find("\n", end)
        glue = text[line_start:start].strip() and text[end:line_end if line_end >= 0 else None].strip()
        pieces += [text[last:start], " " if glue and not body.count("\n") else "", "\n" * body.count("\n")]
        last = end
    pieces.append(text[last:])
    lines = "".join(pieces).split("\n")

    out, drop_blank = [], False
    for number, line in enumerate(lines):
        if number in marked:
            line = line.rstrip()
            if not line:
                # Inside a run of deleted lines: decide once the next kept line is known.
                drop_blank = True
                continue
        if drop_blank:
            drop_blank = False
            prev = out[-1] if out else ""
            if not line.strip() and (not prev.strip() or prev.endswith("{")):
                continue
            if not prev.strip() and out and line.lstrip().startswith("}"):
                out.pop()
        out.append(line)
    result = "\n".join(out)
    if code_tokens(path, result) != code_tokens(path, text):
        sys.exit(f"{path}: stripping changed the code, file left unchanged")
    if comments(path, result):
        sys.exit(f"{path}: comments left after stripping, file left unchanged")
    return result


def blocks(path, text):
    """Adjacent comments merged: (first line, text, the next line of code)."""
    lines = text.split("\n")
    out = []
    for start, end, body in comments(path, text):
        first = text.count("\n", 0, start)
        last = first + body.count("\n")
        if out and first <= out[-1][1] + 1 and not lines[first][:start - text.rfind("\n", 0, start) - 1].strip():
            out[-1][1], out[-1][2] = last, out[-1][2] + "\n" + body
        else:
            out.append([first, last, body])
    result = []
    for first, last, body in out:
        anchor = next((l.strip() for l in lines[last + 1:] if l.strip()), "")
        if lines[last].strip() != body.split("\n")[-1].strip():
            anchor = lines[last].strip()
        result.append((first + 1, body, anchor))
    return result


def tracked():
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "--", SCOPE],
                         capture_output=True, text=True, check=True).stdout.split("\n")
    return [p for p in out if p and in_scope(p)]


def read(path, staged=False):
    if staged:
        return subprocess.run(["git", "-C", ROOT, "show", ":" + path], capture_output=True,
                              text=True, errors="replace", check=True).stdout
    with open(os.path.join(ROOT, path), encoding="utf-8", errors="replace") as f:
        return f.read()


def load_baseline():
    counts = {}
    with open(BASELINE, encoding="utf-8") as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                count, path = line.split(maxsplit=1)
                counts[path.strip()] = int(count)
    return counts


def write_baseline(counts):
    with open(BASELINE, "w", encoding="utf-8") as f:
        f.write("# Allowed comment lines per file (tools/comments.py). Only ever goes down:\n"
                "# delete comments, then run `python3 tools/comments.py update`.\n")
        for path in sorted(counts):
            if counts[path]:
                f.write(f"{counts[path]} {path}\n")


def check(staged):
    baseline = load_baseline()
    if staged:
        names = subprocess.run(["git", "-C", ROOT, "diff", "--cached", "--name-only",
                                "--diff-filter=ACMR"], capture_output=True, text=True,
                               check=True).stdout.split()
        paths = [p for p in names if in_scope(p)]
    else:
        paths = tracked()
    failed = False
    for path in paths:
        count = comment_lines(path, read(path, staged))
        allowed = baseline.get(path, 0)
        if count > allowed:
            failed = True
            print(f"{path}: {count} comment lines, {allowed} allowed")
    if failed:
        print("Comments are banned in src/ (docs/adr/0001-no-code-comments.md). Delete the new "
              "comments; record decisions as an ADR in docs/adr/.")
    return 1 if failed else 0


def update():
    baseline = load_baseline()
    current = {p: comment_lines(p, read(p)) for p in tracked()}
    write_baseline({p: min(c, baseline.get(p, 0)) for p, c in current.items()})
    return 0


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    command, args = argv[0], argv[1:]
    if command == "check":
        return check("--staged" in args)
    if command == "update":
        return update()
    if command == "list":
        for path in args:
            for line, body, anchor in blocks(path, read(path)):
                print(f"{path}:{line}  -> {anchor}\n{body}\n")
        return 0
    if command == "strip":
        for path in args:
            text = read(path)
            new = strip(path, text)
            if new != text:
                with open(os.path.join(ROOT, path), "w", encoding="utf-8") as f:
                    f.write(new)
        return 0
    if command == "stats":
        own = trailing = 0
        for path in tracked():
            text = read(path)
            code = list(text)
            seen = set()
            for start, end, _ in comments(path, text):
                first = text.count("\n", 0, start)
                seen.update(range(first, first + text.count("\n", start, end) + 1))
                code[start:end] = [c if c == "\n" else " " for c in text[start:end]]
            code_lines = "".join(code).split("\n")
            trailing += sum(1 for n in seen if code_lines[n].strip())
            own += sum(1 for n in seen if not code_lines[n].strip())
        print(f"own-line comment lines: {own}\ntrailing comments: {trailing}")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
