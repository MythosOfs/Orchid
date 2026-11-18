// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace Tests {

/// Compares `actual` with the reference file src/tests/expect/<name>. With ORCHID_EXPECT_UPDATE=1
/// in the environment it rewrites the file instead, so an intended change shows up as a diff in
/// the commit. A missing reference file fails: it is never created silently.
void CheckExpect(std::string_view name, std::string_view actual);

/// Removes the "[<16 hex digits>] " instruction address that IR dumps put on every line, so the
/// text is the same in every run.
std::string StripIrAddresses(std::string_view dump);

} // namespace Tests
