// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "core/memory/cheat_engine.h"

namespace {

using Core::Memory::TextCheatParser;

// The closing bracket stays readable right after the view, where an overread would find it.
std::string_view WithoutLastChar(const std::string& buffer) {
    return std::string_view{buffer}.substr(0, buffer.size() - 1);
}

} // Anonymous namespace

TEST_CASE("Cheat text with a name parses", "[core][cheats]") {
    const auto cheats = TextCheatParser{}.Parse("[Inf HP]\n04000000 00001234 00000063\n");
    REQUIRE(cheats.size() == 2);
    REQUIRE(std::string{cheats[1].definition.readable_name.data()} == "Inf HP");
    REQUIRE(cheats[1].definition.num_opcodes == 3);
    REQUIRE(cheats[1].definition.opcodes[2] == 0x63);
    REQUIRE_FALSE(cheats[0].enabled);
    REQUIRE(cheats[1].enabled);
}

TEST_CASE("Cheat name that runs to the end of the text is rejected", "[core][cheats]") {
    const std::string buffer = "[Inf HP]";
    REQUIRE(TextCheatParser{}.Parse(WithoutLastChar(buffer)).empty());
}

TEST_CASE("Master cheat name that runs to the end of the text is rejected", "[core][cheats]") {
    const std::string buffer = "{M}";
    REQUIRE(TextCheatParser{}.Parse(WithoutLastChar(buffer)).empty());
}
