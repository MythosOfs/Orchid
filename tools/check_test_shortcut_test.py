#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Orchid Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Self-check for tools/check_test_shortcut.py: python3 tools/check_test_shortcut_test.py"""

import os
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "check_test_shortcut.py")

with tempfile.TemporaryDirectory() as tmp:
    spaced = os.path.join(tmp, "my dir")
    os.makedirs(spaced)
    log = os.path.join(tmp, "argv.log")
    wrapper = os.path.join(spaced, "orchid-test-run")
    with open(wrapper, "w") as f:
        f.write(f'#!/bin/sh\nprintf "%s\\n" "$@" > "{log}"\n')
    os.chmod(wrapper, 0o755)
    binary = os.path.join(spaced, "orchid")

    def run(exec_line):
        if os.path.exists(log):
            os.remove(log)
        desktop = os.path.join(tmp, "t.desktop")
        with open(desktop, "w") as f:
            f.write(f"[Desktop Entry]\nExec={exec_line}\n")
        code = subprocess.run([sys.executable, TOOL, desktop], capture_output=True, check=False).returncode
        return code, os.path.exists(log)

    failures = []

    def expect(name, ok):
        if not ok:
            failures.append(name)

    def args_logged():
        return open(log).read().split("\n")[:-1] if os.path.exists(log) else None

    code, ran = run(f'env EDEN_XYZ=1 "{wrapper}" lbl "{binary}" %f')
    expect("quoted path with a space starts the wrapper", code == 0 and ran)
    expect("--check is the first argument", (args_logged() or [])[:3] == ["--check", "lbl", binary])

    code, ran = run(f'env X=1 "{wrapper}" lbl "{binary}" "Build 100%%"')
    expect("%% reaches the wrapper as a single %", code == 0 and (args_logged() or [])[-1:] == ["Build 100%"])

    bad = {
        "tilde": f"env X=1 ~/.local/bin/orchid-test-run lbl {binary}",
        "HOME unquoted": f"env X=1 $HOME/.local/bin/orchid-test-run lbl {binary}",
        "HOME double-quoted": f'env X=1 "{spaced}/"$HOME"/orchid-test-run" lbl {binary}',
        "HOME in binary": f'env X=1 "{wrapper}" lbl "/opt/"$HOME"/orchid"',
        "relative binary": f'env X=1 "{wrapper}" lbl orchid',
        "no wrapper": f'env X=1 "{spaced}/orchid test-run" lbl "{binary}"',
        "other command first": f'sh -c x "{wrapper}" lbl "{binary}"',
        "tilde in env value": f'env TEST=~/cache "{wrapper}" lbl "{binary}"',
        "tilde after = in an argument": f'env X=1 "{wrapper}" lbl "{binary}" --cache=~/c',
        "field code inside an argument": f'env X=1 "{wrapper}" lbl "{binary}" "Build 100%x"',
        "field code glued to an option": f'env X=1 "{wrapper}" lbl "{binary}" --file=%f',
        "lone % at the end": f'env X=1 "{wrapper}" lbl "{binary}" "100%"',
    }
    for name, line in bad.items():
        code, ran = run(line)
        expect(f"{name}: rejected before anything starts", code == 1 and not ran)

    if failures:
        print("check_test_shortcut: FAILED\n  " + "\n  ".join(failures))
        sys.exit(1)

print("check_test_shortcut: ok")
