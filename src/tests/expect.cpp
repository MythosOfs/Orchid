// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "tests/expect.h"

namespace Tests {

void CheckExpect(std::string_view name, std::string_view actual) {
    const std::string path = std::string{ORCHID_EXPECT_DIR} + '/' + std::string{name};
    if (const char* update = std::getenv("ORCHID_EXPECT_UPDATE"); update && *update == '1') {
        std::ofstream{path, std::ios::binary} << actual;
        WARN("Rewrote " << path);
        return;
    }
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        FAIL("Missing reference " << path << ", create it with ORCHID_EXPECT_UPDATE=1");
    }
    const std::string expected{std::istreambuf_iterator<char>{file}, {}};
    if (expected == actual) {
        SUCCEED();
        return;
    }
    // Report the first line that differs; the whole diff is `git diff` after an update run.
    std::istringstream expected_lines{expected};
    std::istringstream actual_lines{std::string{actual}};
    std::string want, got;
    size_t line = 1;
    while (true) {
        const bool more_want = static_cast<bool>(std::getline(expected_lines, want));
        const bool more_got = static_cast<bool>(std::getline(actual_lines, got));
        if (!more_want)
            want = "<end of file>";
        if (!more_got)
            got = "<end of output>";
        if (want != got || (!more_want && !more_got))
            break;
        ++line;
    }
    FAIL(path << " differs at line " << line << "\n  expected: " << want << "\n  actual:   " << got
              << "\nIf the change is intended, rerun with ORCHID_EXPECT_UPDATE=1 and "
              << "explain the new output in the commit message.");
}

std::string StripIrAddresses(std::string_view dump) {
    static const std::regex address{R"(\[[0-9a-f]{16}\] )"};
    return std::regex_replace(std::string{dump}, address, "");
}

} // namespace Tests
