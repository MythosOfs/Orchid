// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/loader/loader.h"

TEST_CASE("DisplayTitle prefers the installed control name", "[loader][display_title]") {
    CHECK(Loader::DisplayTitle("Installed", "", "Folder", "main") == "Installed");
    CHECK(Loader::DisplayTitle("Installed", " ", "Folder", "main") == "Installed");
    CHECK(Loader::DisplayTitle("Installed", "Sidecar", "Folder", "main") == "Installed");
}

TEST_CASE("DisplayTitle falls back in a fixed order", "[loader][display_title]") {
    CHECK(Loader::DisplayTitle("", "Sidecar", "Folder", "main") == "Sidecar");
    CHECK(Loader::DisplayTitle(" ", "Sidecar", "Folder", "main") == "Sidecar");
    CHECK(Loader::DisplayTitle("", "", "Folder", "main") == "Folder");
    CHECK(Loader::DisplayTitle(" ", " ", "Folder", "main") == "Folder");
    CHECK(Loader::DisplayTitle("", "", "", "Game") == "Game");
    CHECK(Loader::DisplayTitle("", "", " ", "Game") == "Game");
}
