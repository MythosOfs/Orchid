// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "common/settings.h"
#include "qt_common/config/qt_config.h"
#include "qt_common/config/uisettings.h"

namespace {

using BanList = std::pair<std::vector<std::string>, std::vector<std::string>>;

struct TemporaryConfigDir {
    TemporaryConfigDir()
        : previous{Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir)},
          saved_list{UISettings::values.multiplayer_ban_list},
          saved_game_dirs{UISettings::values.game_dirs} {
        std::filesystem::create_directories(root);
        Common::FS::SetEdenPath(Common::FS::EdenPath::ConfigDir, root);
        REQUIRE(Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) == root);
    }
    ~TemporaryConfigDir() {
        Common::FS::SetEdenPath(Common::FS::EdenPath::ConfigDir, previous);
        UISettings::values.multiplayer_ban_list = saved_list;
        UISettings::values.game_dirs = saved_game_dirs;
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("qt-config-ban-list-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path previous;
    BanList saved_list;
    decltype(UISettings::values.game_dirs) saved_game_dirs;
};

BanList RoundTrip(const BanList& list) {
    {
        QtConfig config{"ban-list-test"};
        UISettings::values.multiplayer_ban_list = list;
        config.SaveAllValues();
    }
    UISettings::values.multiplayer_ban_list = {{"stale"}, {"stale"}};
    QtConfig reloaded{"ban-list-test"};
    return UISettings::values.multiplayer_ban_list;
}

} // namespace

TEST_CASE("QtConfig reloads the multiplayer ban lists it saved", "[.][qt_config]") {
    const TemporaryConfigDir dir;
    const BanList list{{"alice", "bob"}, {"192.168.0.10", "10.0.0.7"}};
    const auto loaded = RoundTrip(list);
    REQUIRE(loaded.first == list.first);
    REQUIRE(loaded.second == list.second);
}

TEST_CASE("QtConfig reloads empty multiplayer ban lists as empty", "[.][qt_config]") {
    const TemporaryConfigDir dir;
    const auto loaded = RoundTrip({});
    REQUIRE(loaded.first.empty());
    REQUIRE(loaded.second.empty());
}

TEST_CASE("QtConfig reads an old Yoshi 4K option as the 4K No AA patch", "[.][qt_config]") {
    const TemporaryConfigDir dir;
    std::filesystem::create_directories(dir.root / "custom");
    std::ofstream{dir.root / "custom" / "0400245021D2C000.ini"}
        << "[System]\nyoshi_4k_patch\\use_global=false\nyoshi_4k_patch\\default=false\n"
           "yoshi_4k_patch=true\n";
    {
        QtConfig config{"0400245021D2C000", Config::ConfigType::PerGameConfig};
        CHECK(Settings::values.yoshi_patch.GetValue());
        CHECK(Settings::values.yoshi_patch_resolution.GetValue() ==
              Settings::YoshiPatchResolution::Res2160p);
        CHECK(Settings::values.yoshi_patch_anti_aliasing.GetValue() ==
              Settings::YoshiPatchAntiAliasing::None);
    }
    Settings::RestoreGlobalState(false);
    CHECK_FALSE(Settings::values.yoshi_patch.GetValue());
}
