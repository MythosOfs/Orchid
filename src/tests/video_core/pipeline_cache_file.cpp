// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/stage.h"
#include "video_core/shader_environment.h"

namespace {

namespace fs = std::filesystem;
constexpr u32 Version = 0x7e570001;

struct CacheFile {
    fs::path path = fs::temp_directory_path() /
                    ("pipeline-cache-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~CacheFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

template <typename T>
void Put(std::vector<char>& out, T value) {
    const auto* bytes = reinterpret_cast<const char*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

// One compute environment as FileEnvironment::Deserialize reads it, with no table entries.
std::vector<char> ComputeRecord(u64 code_size, u32 read_lowest, u32 read_highest,
                                const std::vector<u64>& code) {
    std::vector<char> out;
    Put<u32>(out, 1);              // num_envs
    Put<u64>(out, code_size);
    for (int i = 0; i < 5; ++i) {
        Put<u64>(out, 0);          // texture types, pixel formats, cbuf values, pointers, replacements
    }
    Put<u32>(out, 0);              // local_memory_size
    Put<u32>(out, 0);              // texture_bound
    Put<u32>(out, ~0U);            // surface_bound
    Put<u32>(out, read_lowest);    // start_address
    Put<u32>(out, read_lowest);
    Put<u32>(out, read_highest);
    Put<u32>(out, 1);              // viewport_transform_state
    Put<u32>(out, static_cast<u32>(Shader::Stage::Compute));
    for (const u64 word : code) {
        Put<u64>(out, word);
    }
    for (int i = 0; i < 4; ++i) {
        Put<u32>(out, 1);          // workgroup_size x, y, z and shared_memory_size
    }
    return out;
}

void Write(const fs::path& path, const std::vector<char>& record) {
    std::ofstream file{path, std::ios::binary};
    file.write("yuzucach", 8);
    const u32 version = Version;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(record.data(), static_cast<std::streamsize>(record.size()));
}

struct LoadResult {
    int compute = 0;
    int graphics = 0;
    u64 last_word = 0;
};

LoadResult Load(const fs::path& path) {
    LoadResult result;
    VideoCommon::LoadPipelines(
        std::stop_token{}, path, Version,
        [&result](std::ifstream&, VideoCommon::FileEnvironment env) {
            ++result.compute;
            result.last_word = env.ReadInstruction(0x18);
        },
        [&result](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) { ++result.graphics; });
    return result;
}

} // Anonymous namespace

TEST_CASE("A pipeline cache record this build writes loads", "[video_core][pipeline_cache_file]") {
    CacheFile t;
    Write(t.path, ComputeRecord(16, 0x10, 0x18, {0x1111, 0x2222}));
    const LoadResult result = Load(t.path);
    REQUIRE(result.compute == 1);
    REQUIRE(result.graphics == 0);
    REQUIRE(result.last_word == 0x2222);
    REQUIRE(fs::exists(t.path));
}

TEST_CASE("A pipeline cache record with no environments deletes the file",
          "[video_core][pipeline_cache_file]") {
    CacheFile t;
    std::vector<char> record;
    Put<u32>(record, 0);
    Write(t.path, record);
    const LoadResult result = Load(t.path);
    REQUIRE(result.compute == 0);
    REQUIRE(result.graphics == 0);
    REQUIRE(!fs::exists(t.path));
}

TEST_CASE("A pipeline cache record with a huge code size deletes the file",
          "[video_core][pipeline_cache_file]") {
    CacheFile t;
    Write(t.path, ComputeRecord(u64{1} << 60, 0x10, 0x18, {0x1111, 0x2222}));
    LoadResult result;
    REQUIRE_NOTHROW(result = Load(t.path));
    REQUIRE(result.compute == 0);
    REQUIRE(!fs::exists(t.path));
}

TEST_CASE("A pipeline cache record whose read range exceeds its code deletes the file",
          "[video_core][pipeline_cache_file]") {
    CacheFile t;
    Write(t.path, ComputeRecord(16, 0x10, 0x10000, {0x1111, 0x2222}));
    const LoadResult result = Load(t.path);
    REQUIRE(result.compute == 0);
    REQUIRE(!fs::exists(t.path));
}
