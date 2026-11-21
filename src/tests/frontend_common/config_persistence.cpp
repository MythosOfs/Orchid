// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "frontend_common/config.h"

namespace {

enum class ConfigFailure { None, BufferedWrite, FileSync };
ConfigFailure config_failure{};

class TestConfig final : public Config {
public:
    explicit TestConfig(const std::filesystem::path& path) {
        config_loc = path.string();
        SetUpIni();
    }

    void SetValue(const char* value) {
        REQUIRE(config->SetValue("Test", "Value", value) >= 0);
        WriteToIni();
    }

private:
    void ReloadAllValues() override {}
    void SaveAllValues() override {}
    void ReadHidbusValues() override {}
    void ReadDebugControlValues() override {}
    void ReadPathValues() override {}
    void ReadShortcutValues() override {}
    void ReadUIValues() override {}
    void ReadUIGamelistValues() override {}
    void ReadUILayoutValues() override {}
    void ReadMultiplayerValues() override {}
    void SaveHidbusValues() override {}
    void SaveDebugControlValues() override {}
    void SavePathValues() override {}
    void SaveShortcutValues() override {}
    void SaveUIValues() override {}
    void SaveUIGamelistValues() override {}
    void SaveUILayoutValues() override {}
    void SaveMultiplayerValues() override {}
    std::vector<Settings::BasicSetting*>& FindRelevantList(Settings::Category) override {
        return settings;
    }

    std::vector<Settings::BasicSetting*> settings;
};

}

extern "C" int __real_fflush(FILE* stream);
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fflush(FILE* stream);
extern "C" int __wrap_fsync(int fd);

namespace {

bool IsConfigTestFile(int fd) {
    std::error_code error;
    const auto target =
        std::filesystem::read_symlink("/proc/self/fd/" + std::to_string(fd), error);
    return !error && target.string().find("orchid-config-") != std::string::npos;
}

}

extern "C" int __wrap_fflush(FILE* stream) {
    if (config_failure == ConfigFailure::BufferedWrite && stream != nullptr &&
        IsConfigTestFile(fileno(stream))) {
        errno = EIO;
        return EOF;
    }
    return __real_fflush(stream);
}

extern "C" int __wrap_fsync(int fd) {
    if (config_failure == ConfigFailure::FileSync && IsConfigTestFile(fd)) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

TEST_CASE("Config preserves the old INI if writing or syncing the replacement fails",
          "[config_persistence]") {
    const auto root = std::filesystem::temp_directory_path() /
                      ("orchid-config-persistence-" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    SCOPE_EXIT {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    };
    const auto path = root / "persistence.ini";
    TestConfig config{path};
    const auto stored = [&] {
        std::ifstream input(path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>{input}, {}};
    };
    config.SetValue("before");
    const auto previous = stored();
    REQUIRE(previous.find("before") != std::string::npos);

    for (const auto failure : {ConfigFailure::BufferedWrite, ConfigFailure::FileSync}) {
        config_failure = failure;
        config.SetValue("after");
        config_failure = ConfigFailure::None;
        CHECK(stored() == previous);
        CHECK(std::distance(std::filesystem::directory_iterator(root),
                            std::filesystem::directory_iterator{}) == 1);
    }
    config.SetValue("after");
    CHECK(stored().find("after") != std::string::npos);

    REQUIRE(std::filesystem::remove(path));
    config.SetValue("recreated");
    CHECK(stored().find("recreated") != std::string::npos);
    const auto reference = root / "reference.ini";
    { std::ofstream reference_file{reference}; }
    CHECK(std::filesystem::status(path).permissions() ==
          std::filesystem::status(reference).permissions());
}

TEST_CASE("Config leaves a read-only INI as it is", "[config_persistence]") {
    const auto root = std::filesystem::temp_directory_path() /
                      ("orchid-config-read-only-" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto path = root / "read-only.ini";
    SCOPE_EXIT {
        std::error_code error;
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, error);
        std::filesystem::remove_all(root, error);
    };
    TestConfig config{path};
    const auto stored = [&] {
        std::ifstream input(path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>{input}, {}};
    };
    config.SetValue("kept");
    const auto previous = stored();
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::group_read |
                                     std::filesystem::perms::others_read);

    config.SetValue("changed");

    CHECK(stored() == previous);
    CHECK(std::distance(std::filesystem::directory_iterator(root),
                        std::filesystem::directory_iterator{}) == 1);
}
