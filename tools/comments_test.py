#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Orchid Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Self-check for tools/comments.py: python3 tools/comments_test.py"""

import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import comments as c

SRC = """// SPDX-License-Identifier: GPL-3.0-or-later

namespace A {

// Leading comment
// on two lines.
int f() {
    // Only line in the block start.

    const char* s = "http://not-a-comment"; // trailing
    const u64 v = 0x3'00030000; // the title's version
    const char q = '"';
    auto raw = R"(/* not a comment */)";
    return a/*glue*/b;
}

/* block
   comment */
int g;

// clang-format off
} // namespace A
"""

EXPECTED = """// SPDX-License-Identifier: GPL-3.0-or-later

namespace A {

int f() {
    const char* s = "http://not-a-comment";
    const u64 v = 0x3'00030000;
    const char q = '"';
    auto raw = R"(/* not a comment */)";
    return a b;
}

int g;

// clang-format off
}
"""

assert c.comment_lines("x.h", SRC) == 9, c.comment_lines("x.h", SRC)
assert c.strip("x.h", SRC) == EXPECTED, c.strip("x.h", SRC)
assert c.code_tokens("x.h", EXPECTED) == c.code_tokens("x.h", SRC)
assert [b[0] for b in c.blocks("x.h", SRC)] == [5, 8, 10, 11, 14, 17, 22]
assert c.comment_lines("x.kt", 'val s = """// not\n"""\n// yes\n') == 1
print("comments_test ok")
