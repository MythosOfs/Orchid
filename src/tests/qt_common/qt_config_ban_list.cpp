// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
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
