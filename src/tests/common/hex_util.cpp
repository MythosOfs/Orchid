// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "common/hex_util.h"

TEST_CASE("HexStringToArray reads a full-length string", "[common]") {
    REQUIRE(Common::HexStringToArray<2>("a1b2") == std::array<u8, 2>{0xA1, 0xB2});
    REQUIRE(Common::HexStringToArray<2, true>("a1b2") == std::array<u8, 2>{0xA1, 0xB2});
}

TEST_CASE("HexStringToArray does not read past a short string", "[common]") {
    const std::string buffer = "a1b2c3d4";
    const std::string_view text = std::string_view{buffer}.substr(0, 4);
    REQUIRE(Common::HexStringToArray<4>(text) == std::array<u8, 4>{});
    REQUIRE(Common::HexStringToArray<4, true>(text) == std::array<u8, 4>{});
}
