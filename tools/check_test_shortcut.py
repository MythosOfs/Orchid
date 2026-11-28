#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Orchid Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check a test shortcut before it is handed over: python3 tools/check_test_shortcut.py FILE.desktop

Parses the Exec line into an argument list (shlex, no shell), rejects a path the desktop launcher
would not expand (`~`, `$HOME`), requires `[env VAR=value...] <absolute>/orchid-test-run <label>
<absolute binary> ...`, then runs `orchid-test-run --check <label> <binary> ...` as that list.
"""

import os
import re
import shlex
import subprocess
import sys

WRAPPER = "orchid-test-run"


def build_check_command(exec_line):
    argv = [a for a in shlex.split(exec_line) if not re.fullmatch(r"%[a-zA-Z]", a)]
    for arg in argv:
        if arg.startswith("~") or re.search(r"\$\{?HOME\b", arg):
            raise ValueError(f"the launcher does not expand: {arg}")
    found = [i for i, a in enumerate(argv) if os.path.basename(a) == WRAPPER]
    if len(found) != 1:
        raise ValueError(f"{WRAPPER} must appear exactly once in Exec")
    at = found[0]
    prefix, wrapper, rest = argv[:at], argv[at], argv[at + 1 :]
    if prefix and (prefix[0] != "env" or not all(re.fullmatch(r"[A-Za-z_]\w*=.*", a) for a in prefix[1:])):
        raise ValueError("only `env VAR=value` may precede the wrapper")
    if not os.path.isabs(wrapper):
        raise ValueError(f"wrapper path is not absolute: {wrapper}")
    if len(rest) < 2 or not os.path.isabs(rest[1]):
        raise ValueError("Exec needs <label> and an absolute binary path after the wrapper")
    return prefix + [wrapper, "--check"] + rest


def exec_line_of(path):
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("Exec="):
                return line[len("Exec=") :].rstrip("\n")
    raise ValueError("no Exec line")


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        return 2
    try:
        command = build_check_command(exec_line_of(argv[1]))
    except ValueError as err:
        print(f"check_test_shortcut: {err}", file=sys.stderr)
        return 1
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv))
