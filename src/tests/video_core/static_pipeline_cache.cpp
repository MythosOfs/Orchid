// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#endif

#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace {

namespace fs = std::filesystem;

struct CacheDirectory {
    fs::path path = fs::temp_directory_path() /
                    ("static-pipeline-cache-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path cache = path / "vulkan_static_pipelines.bin";
    CacheDirectory() {
        fs::create_directories(path);
    }
    ~CacheDirectory() {
        std::error_code ec;
        fs::permissions(path, fs::perms::owner_all, ec);
        fs::remove_all(path, ec);
    }
    size_t Entries() const {
        return static_cast<size_t>(std::distance(fs::directory_iterator{path}, {}));
    }
};

std::vector<char> Bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

} // Anonymous namespace

TEST_CASE("Static pipeline cache: a save replaces the file whole", "[video_core][pipeline_cache]") {
    CacheDirectory dir;
    CHECK(Vulkan::ReadStaticPipelineCache(dir.cache).empty());

    REQUIRE(Vulkan::WriteStaticPipelineCache(dir.cache, Bytes("first driver blob")));
    CHECK(Vulkan::ReadStaticPipelineCache(dir.cache) == Bytes("first driver blob"));
    REQUIRE(Vulkan::WriteStaticPipelineCache(dir.cache, Bytes("second")));
    CHECK(Vulkan::ReadStaticPipelineCache(dir.cache) == Bytes("second"));
    CHECK(dir.Entries() == 1);

    SECTION("a file without the Orchid header reads as no cache") {
        std::ofstream{dir.cache, std::ios::binary | std::ios::trunc} << "not a cache file";
        CHECK(Vulkan::ReadStaticPipelineCache(dir.cache).empty());
    }

    SECTION("a cache of another version reads as no cache") {
        std::fstream file{dir.cache, std::ios::binary | std::ios::in | std::ios::out};
        file.seekp(8);
        file.put(2);
        file.close();
        CHECK(Vulkan::ReadStaticPipelineCache(dir.cache).empty());
    }

    SECTION("a save that cannot write keeps the previous cache") {
        fs::permissions(dir.path, fs::perms::owner_read | fs::perms::owner_exec);
        const bool writable = std::ofstream{dir.path / "probe"}.is_open();
        if (writable) {
            // Root, or a host whose directory permissions do not stop file creation.
            fs::permissions(dir.path, fs::perms::owner_all);
            WARN("directory permissions do not block writes here");
            return;
        }
        CHECK_FALSE(Vulkan::WriteStaticPipelineCache(dir.cache, Bytes("lost")));
        fs::permissions(dir.path, fs::perms::owner_all);
        CHECK(Vulkan::ReadStaticPipelineCache(dir.cache) == Bytes("second"));
        CHECK(dir.Entries() == 1);
    }

#ifndef _WIN32
    SECTION("a save whose buffered bytes fail to flush keeps the previous cache") {
        // A small cache stays in the stream buffer until close; a file size limit makes that
        // flush fail, as a full disk would.
        rlimit previous{};
        REQUIRE(getrlimit(RLIMIT_FSIZE, &previous) == 0);
        const auto previous_handler = std::signal(SIGXFSZ, SIG_IGN);
        rlimit limit = previous;
        limit.rlim_cur = 16;
        REQUIRE(setrlimit(RLIMIT_FSIZE, &limit) == 0);
        const bool written = Vulkan::WriteStaticPipelineCache(dir.cache, Bytes("cut off by the limit"));
        setrlimit(RLIMIT_FSIZE, &previous);
        std::signal(SIGXFSZ, previous_handler);
        CHECK_FALSE(written);
        CHECK(Vulkan::ReadStaticPipelineCache(dir.cache) == Bytes("second"));
        CHECK(dir.Entries() == 1);
    }
#endif

    SECTION("a save that cannot name its temporary file keeps the previous cache") {
        bool written = true;
        CHECK_NOTHROW(written = Vulkan::WriteStaticPipelineCache(
                          dir.cache, Bytes("lost"),
                          []() -> std::string { throw std::runtime_error("no entropy"); }));
        CHECK_FALSE(written);
        CHECK(Vulkan::ReadStaticPipelineCache(dir.cache) == Bytes("second"));
        CHECK(dir.Entries() == 1);
    }

    SECTION("a save that cannot replace the file removes its temporary file") {
        const fs::path blocked = dir.path / "blocked";
        fs::create_directories(blocked / "child");
        CHECK_FALSE(Vulkan::WriteStaticPipelineCache(blocked, Bytes("lost")));
        CHECK(fs::is_directory(blocked / "child"));
        CHECK(dir.Entries() == 2);
    }
}

TEST_CASE("Static pipeline cache: a rejected cache is retried empty", "[video_core][pipeline_cache]") {
    const auto data = Bytes("driver blob");
    std::vector<size_t> sizes;
    const auto rejecting = [&](std::span<const char> bytes) {
        sizes.push_back(bytes.size());
        if (!bytes.empty()) {
            throw Vulkan::vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
        }
    };

    SECTION("the driver rejects the saved bytes") {
        Vulkan::CreateStaticPipelineCache(data, rejecting);
        CHECK(sizes == std::vector<size_t>{data.size(), 0});
    }
    SECTION("the driver takes the saved bytes") {
        Vulkan::CreateStaticPipelineCache(data, [&](std::span<const char> bytes) {
            sizes.push_back(bytes.size());
        });
        CHECK(sizes == std::vector<size_t>{data.size()});
    }
    SECTION("no saved bytes: a failure is not retried") {
        CHECK_THROWS_AS(Vulkan::CreateStaticPipelineCache({}, [&](std::span<const char> bytes) {
                            sizes.push_back(bytes.size());
                            throw Vulkan::vk::Exception(VK_ERROR_OUT_OF_HOST_MEMORY);
                        }),
                        Vulkan::vk::Exception);
        CHECK(sizes == std::vector<size_t>{0});
    }
}
