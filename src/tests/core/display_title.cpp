// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "core/loader/loader.h"

namespace {
std::string Title(std::string_view loader_title, std::string installed, std::string loose,
                  std::string_view file_name) {
    return Loader::DisplayTitle(
        loader_title, [&] { return installed; }, [&] { return loose; }, file_name);
}
} // namespace

TEST_CASE("DisplayTitle keeps the loader's own title", "[loader][display_title]") {
    CHECK(Title("Sidecar", "Installed", "Folder", "main") == "Sidecar");
    CHECK(Title("NUP Override", "Installed", "", "Game") == "NUP Override");
}

TEST_CASE("DisplayTitle looks up fallbacks only when the loader has no title",
          "[loader][display_title]") {
    int lookups = 0;
    const auto count = [&] {
        ++lookups;
        return std::string{"Fallback"};
    };
    CHECK(Loader::DisplayTitle("Sidecar", count, count, "main") == "Sidecar");
    CHECK(lookups == 0);
    CHECK(Loader::DisplayTitle(" ", count, count, "main") == "Fallback");
    CHECK(lookups == 1);
}

TEST_CASE("DisplayTitle falls back in a fixed order", "[loader][display_title]") {
    CHECK(Title("", "Installed", "Folder", "main") == "Installed");
    CHECK(Title(" ", "Installed", "Folder", "main") == "Installed");
    CHECK(Title("", "", "Folder", "main") == "Folder");
    CHECK(Title(" ", " ", "Folder", "main") == "Folder");
    CHECK(Title("", "", "", "Game") == "Game");
    CHECK(Title("", "", " ", "Game") == "Game");
}
