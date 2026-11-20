// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"

TEST_CASE("SetGameLog copies startup header, reopens the same game log, and recovers on error",
          "[common][logging]") {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path log_dir = fs::temp_directory_path() / ("orchid_game_log_test_" + std::to_string(stamp));
    fs::create_directories(log_dir);
    SCOPE_EXIT {
        std::error_code ec;
        fs::remove_all(log_dir, ec);
    };

    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, log_dir);
    Common::Log::Initialize();

    LOG_INFO(Common, "startup-header-marker");
    Common::Log::Flush();

    Common::Log::SetGameLog(0x040074A01BF12000ULL, "Donkey Kong Bananza");
    const fs::path game_log = log_dir / "orchid_DKB_040074A01BF12000.txt";
    REQUIRE(Common::Log::GetLogFilePath() == game_log);
    LOG_INFO(Common, "first-run-marker");
    Common::Log::Flush();

    const std::string first_run = Common::FS::ReadStringFromFile(game_log, Common::FS::FileType::TextFile);
    CHECK(first_run.find("startup-header-marker") != std::string::npos);
    CHECK(first_run.find("first-run-marker") != std::string::npos);

    Common::Log::SetGameLog(0x040074A01BF12000ULL, "Donkey Kong Bananza");
    REQUIRE(Common::Log::GetLogFilePath() == game_log);
    LOG_INFO(Common, "second-run-marker");
    Common::Log::Flush();

    const std::string second_run = Common::FS::ReadStringFromFile(game_log, Common::FS::FileType::TextFile);
    CHECK(second_run.find("startup-header-marker") != std::string::npos);
    CHECK(second_run.find("first-run-marker") == std::string::npos);
    CHECK(second_run.find("second-run-marker") != std::string::npos);

    const fs::path blocked_target = log_dir / "orchid_0400000000000001.txt";
    fs::create_directory(blocked_target);
    Common::Log::SetGameLog(0x0400000000000001ULL, "");
    REQUIRE(Common::Log::GetLogFilePath() == game_log);
    LOG_INFO(Common, "after-failed-switch-marker");
    Common::Log::Flush();

    const std::string after_failure = Common::FS::ReadStringFromFile(game_log, Common::FS::FileType::TextFile);
    CHECK(after_failure.find("second-run-marker") != std::string::npos);
    CHECK(after_failure.find("after-failed-switch-marker") != std::string::npos);
    CHECK(after_failure.find("Failed to open the file") == std::string::npos);
}
