// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/session_counter.h"

TEST_CASE("SessionCounter: a finish queued at the shutdown deadline stays with its session",
          "[common][session_counter]") {
    Common::SessionCounter sessions;
    const u64 first = sessions.Current();
    CHECK(sessions.IsCurrent(first));

    sessions.End();
    CHECK_FALSE(sessions.IsCurrent(first));

    const u64 second = sessions.Current();
    CHECK(second != first);
    CHECK_FALSE(sessions.IsCurrent(first));
    CHECK(sessions.IsCurrent(second));

    sessions.End();
    CHECK_FALSE(sessions.IsCurrent(second));
    CHECK(sessions.IsCurrent(sessions.Current()));
}
