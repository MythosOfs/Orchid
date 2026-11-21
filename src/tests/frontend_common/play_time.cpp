// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <random>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "frontend_common/play_time_manager.h"

namespace {

TEST_CASE("A play time manager that changed nothing does not save over newer play times",
          "[frontend_common]") {
    const auto original = Common::FS::GetEdenPath(Common::FS::EdenPath::PlayTimeDir);
    std::error_code make_error;
    std::filesystem::create_directories(original, make_error);
    REQUIRE(std::filesystem::is_directory(original));
    const auto directory = std::filesystem::temp_directory_path() /
                           fmt::format("orchid-play-time-{}", std::random_device{}());
    REQUIRE(std::filesystem::create_directories(directory));
    Common::FS::SetEdenPath(Common::FS::EdenPath::PlayTimeDir, directory);
    SCOPE_EXIT {
        Common::FS::SetEdenPath(Common::FS::EdenPath::PlayTimeDir, original);
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    };

    constexpr u64 program_id = 0x0400486024DEC000;
    {
        PlayTime::PlayTimeManager frontend;
        frontend.SetPlayTime(program_id, 600);
    }
    {
        PlayTime::PlayTimeManager query_service;
        PlayTime::PlayTimeManager frontend;
        frontend.SetPlayTime(program_id, 1200);
    }
    const PlayTime::PlayTimeManager reader;
    CHECK(reader.GetPlayTime(program_id) == 1200);
}

}
