// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <filesystem>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/launch_params.h"
#include "tests/core/system_test_context.h"

namespace {

using Common::FS::EdenPath;
using Service::Account::ProfileManager;

TEST_CASE("A profile reset on an empty NAND makes and opens the default user", "[core][acc]") {
    ScopedSystemTestContext fixture;
    Settings::values.current_user = 0;
    ProfileManager manager;
    std::error_code error;
    std::filesystem::remove_all(Common::FS::GetEdenPath(EdenPath::NANDDir) / "system", error);

    manager.ResetUserSaveFile();
    CHECK(manager.GetUserCount() == 1);
    CHECK(manager.GetOpenUserCount() == 1);
}

TEST_CASE("A profile reset opens the current user", "[core][acc]") {
    ScopedSystemTestContext fixture;
    Settings::values.current_user = 0;
    ProfileManager manager;
    REQUIRE(manager.CreateNewUser(Common::UUID::MakeRandom(), "Second").IsSuccess());
    manager.WriteUserSaveFile();
    REQUIRE(manager.GetUserCount() == 2);

    Settings::values.current_user = 1;
    manager.ResetUserSaveFile();
    REQUIRE(manager.GetOpenUserCount() == 1);
    const auto second = manager.GetUser(1);
    REQUIRE(second.has_value());
    CHECK(manager.GetOpenUsers()[0] == *second);
}

TEST_CASE("LaunchParams treats -u as an index only when all digits and replaces earlier -u",
          "[core][acc]") {
    ScopedSystemTestContext fixture;
    Settings::values.current_user = 0;
    Core::System system;
    auto& manager = system.GetProfileManager();
    REQUIRE(manager.CreateNewUser(Common::UUID::MakeRandom(), "2Alice").IsSuccess());
    REQUIRE(manager.CreateNewUser(Common::UUID::MakeRandom(), "Bob").IsSuccess());

    {
        char arg0[] = "orchid";
        char flag[] = "-u";
        char name[] = "2Alice";
        std::array<char*, 4> argv{arg0, flag, name, nullptr};
        const auto params = Core::ParseLaunchParams(system, 3, argv.data(), nullptr);
        CHECK(params.selected_user == 1);
        CHECK(Settings::values.current_user.GetValue() == 0);
    }

    {
        char arg0[] = "orchid";
        char flag1[] = "-u";
        char name[] = "2Alice";
        char flag2[] = "-u";
        char missing[] = "2NoSuchUser";
        std::array<char*, 6> argv{arg0, flag1, name, flag2, missing, nullptr};
        const auto params = Core::ParseLaunchParams(system, 5, argv.data(), nullptr);
        CHECK_FALSE(params.selected_user.has_value());
        CHECK(Settings::values.current_user.GetValue() == 0);
        Core::ApplyLaunchParams(params);
        CHECK(Settings::values.current_user.GetValue() == 0);
    }

    {
        char arg0[] = "orchid";
        char flag1[] = "-u";
        char name[] = "2Alice";
        char flag2[] = "-u";
        char out_of_range[] = "4294967297";
        std::array<char*, 6> argv{arg0, flag1, name, flag2, out_of_range, nullptr};
        const auto params = Core::ParseLaunchParams(system, 5, argv.data(), nullptr);
        CHECK_FALSE(params.selected_user.has_value());
        CHECK(Settings::values.current_user.GetValue() == 0);
    }

    {
        char arg0[] = "orchid";
        char flag1[] = "-u";
        char name[] = "2Alice";
        char flag2[] = "-u";
        char index[] = "2";
        std::array<char*, 6> argv{arg0, flag1, name, flag2, index, nullptr};
        const auto params = Core::ParseLaunchParams(system, 5, argv.data(), nullptr);
        CHECK(params.selected_user == 2);
        CHECK(Settings::values.current_user.GetValue() == 0);
        Core::ApplyLaunchParams(params);
        CHECK(Settings::values.current_user.GetValue() == 2);
    }
}

TEST_CASE("Home Menu startup accepts the CLI flag and existing shortcut", "[home-menu]") {
    ScopedSystemTestContext fixture;
    Core::System system;
    char app[] = "orchid";
    char flag[] = "--home-menu";
    char shortcut[] = "-qlaunch";
    char fullscreen[] = "-f";
    std::array<char*, 2> plain{app, nullptr};
    CHECK_FALSE(Core::ParseLaunchParams(system, 1, plain.data(), nullptr).launch_qlaunch);

    std::array<char*, 3> cli{app, flag, nullptr};
    const auto direct = Core::ParseLaunchParams(system, 2, cli.data(), nullptr);
    CHECK(direct.launch_qlaunch);
    CHECK(direct.filepath.empty());

    std::array<char*, 4> old{app, fullscreen, shortcut, nullptr};
    const auto legacy = Core::ParseLaunchParams(system, 3, old.data(), nullptr);
    CHECK(legacy.launch_qlaunch);
    CHECK(legacy.fullscreen);
    CHECK(legacy.filepath.empty());

    char user_flag[] = "-u";
    char user[] = "0";
    std::array<char*, 4> subsequent{app, user_flag, user, nullptr};
    const auto next = Core::ParseLaunchParams(system, 3, subsequent.data(), nullptr);
    CHECK(next.selected_user == 0);
    CHECK_FALSE(next.launch_qlaunch);
}

}
