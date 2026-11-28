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

    code, ran = run(f'env EDEN_XYZ=1 "{wrapper}" lbl "{binary}" %f')
    assert code == 0 and ran, "quoted path with a space must pass and start the wrapper"
    assert open(log).read().split("\n")[:3] == ["--check", "lbl", binary], "--check must be the first argument"

    bad = {
        "tilde": f"env X=1 ~/.local/bin/orchid-test-run lbl {binary}",
        "HOME unquoted": f"env X=1 $HOME/.local/bin/orchid-test-run lbl {binary}",
        "HOME double-quoted": f'env X=1 "{spaced}/"$HOME"/orchid-test-run" lbl {binary}',
        "HOME in binary": f'env X=1 "{wrapper}" lbl "/opt/"$HOME"/orchid"',
        "relative binary": f'env X=1 "{wrapper}" lbl orchid',
        "no wrapper": f'env X=1 "{spaced}/orchid test-run" lbl "{binary}"',
        "other command first": f'sh -c x "{wrapper}" lbl "{binary}"',
    }
    for name, line in bad.items():
        code, ran = run(line)
        assert code == 1 and not ran, f"{name}: must be rejected before anything starts"

print("check_test_shortcut: ok")
