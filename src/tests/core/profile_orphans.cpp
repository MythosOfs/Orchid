// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/hle/service/acc/profile_manager.h"
#include "tests/core/system_test_context.h"

TEST_CASE("Only user ID folders count as orphaned profiles", "[core][acc]") {
    ScopedSystemTestContext fixture;
    Settings::values.current_user = 0;
    Service::Account::ProfileManager manager;
    const auto saves =
        Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "user/save/0000000000000000";
    const auto title = saves / "0400486024DEC000";
    const auto other = saves / "foo";
    const auto not_hex = saves / std::string(32, 'g');
    const auto orphan = std::string{"11111111111111111111111111111111"};
    const auto empty_orphan = std::string{"2222222222222222222222222222222a"};
    for (const auto& dir : {title / "sub", other / "sub", not_hex / "sub", saves / orphan / "0100000000010000",
                            saves / empty_orphan / "0100000000010000"}) {
        REQUIRE(std::filesystem::create_directories(dir));
    }
    std::ofstream{saves / orphan / "0100000000010000/save.bin"} << "save";
    std::ofstream{title / "save.bin"} << "save";

    CHECK(manager.FindOrphanedProfiles() == std::vector{orphan});
    CHECK(std::filesystem::exists(title / "save.bin"));
    CHECK(std::filesystem::exists(other / "sub"));
    CHECK(std::filesystem::exists(not_hex / "sub"));
    CHECK(std::filesystem::exists(saves / orphan / "0100000000010000/save.bin"));
    CHECK_FALSE(std::filesystem::exists(saves / empty_orphan));

    std::filesystem::remove(title / "save.bin");
    CHECK(manager.FindOrphanedProfiles() == std::vector{orphan});
    CHECK(std::filesystem::exists(title / "sub"));
    CHECK(std::filesystem::exists(other / "sub"));
}
