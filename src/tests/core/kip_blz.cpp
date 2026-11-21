// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "core/file_sys/kernel_executable.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"

namespace {

using namespace FileSys;

void Put32(std::vector<u8>& out, std::size_t offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

std::vector<u8> MakeBlz() {
    std::vector<u8> blz{0x05, 0x50, 0x05, 0x50, 0x05, 0x50, 0xFF};
    for (const char c : std::string_view{"ABCDEFGH"}) {
        blz.push_back(static_cast<u8>(c));
    }
    blz.push_back(0x00);
    blz.resize(blz.size() + 0xC);
    Put32(blz, 16, 28);
    Put32(blz, 20, 0xC);
    Put32(blz, 24, 4);
    return blz;
}

std::vector<u8> MakeExpandingBlz(u32& out_size) {
    constexpr size_t groups = 4;
    std::vector<u8> pairs;
    u32 decoded = 8;
    for (size_t i = 0; i < groups * 8; ++i) {
        const u32 size = (std::min)(decoded, 18U);
        const u16 pair = static_cast<u16>(((size - 3) << 12) | (size - 3));
        pairs.push_back(static_cast<u8>(pair));
        pairs.push_back(static_cast<u8>(pair >> 8));
        decoded += size;
    }
    std::vector<u8> blz;
    for (size_t group = groups; group-- > 0;) {
        for (size_t pair = 8; pair-- > 0;) {
            const auto at = pairs.begin() + (group * 8 + pair) * 2;
            blz.insert(blz.end(), at, at + 2);
        }
        blz.push_back(0xFF);
    }
    for (const char c : std::string_view{"ABCDEFGH"}) {
        blz.push_back(static_cast<u8>(c));
    }
    blz.push_back(0x00);
    blz.resize(blz.size() + 0xC);
    const auto compressed_size = static_cast<u32>(blz.size());
    Put32(blz, compressed_size - 12, compressed_size);
    Put32(blz, compressed_size - 8, 0xC);
    Put32(blz, compressed_size - 4, decoded - compressed_size);
    out_size = decoded;
    return blz;
}

VirtualFile MakeKip(const std::vector<u8>& blz, u32 decompressed_size = 32) {
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('K', 'I', 'P', '1'));
    Put32(bytes, 0x24, decompressed_size);
    Put32(bytes, 0x28, static_cast<u32>(blz.size()));
    bytes.insert(bytes.end(), blz.begin(), blz.end());
    return std::make_shared<VectorVfsFile>(std::move(bytes), "test.kip");
}

}

TEST_CASE("KIP with a BLZ text section decompresses", "[core][file_sys]") {
    const KIP kip{MakeKip(MakeBlz())};
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::Success);
    const auto text = kip.GetSectionDecompressed(0);
    REQUIRE(std::string_view{reinterpret_cast<const char*>(text.data()), text.size()} ==
            "ABCDEFGHABCDEFGHABCDEFGHABCDEFGH");
}

TEST_CASE("KIP rejects a BLZ compressed size larger than the section", "[core][file_sys]") {
    auto blz = MakeBlz();
    Put32(blz, 16, 0x1000);
    const KIP kip{MakeKip(blz)};
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::ErrorBLZDecompressionFailed);
}

TEST_CASE("KIP rejects a BLZ footer size larger than the compressed size", "[core][file_sys]") {
    auto blz = MakeBlz();
    Put32(blz, 20, 29);
    const KIP kip{MakeKip(blz)};
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::ErrorBLZDecompressionFailed);
}

TEST_CASE("KIP rejects a BLZ stream that runs out of input", "[core][file_sys]") {
    auto blz = MakeBlz();
    blz[6] = 0x00;
    Put32(blz, 24, 0x1000);
    const KIP kip{MakeKip(blz)};
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::ErrorBLZDecompressionFailed);
}

TEST_CASE("KIP rejects a BLZ output size that wraps in 32 bits", "[core][file_sys]") {
    for (const u32 additional : {0xFFFFFFF0U, 0xFFFFFFE4U}) {
        auto blz = MakeBlz();
        Put32(blz, 24, additional);
        const KIP kip{MakeKip(blz)};
        REQUIRE(kip.GetStatus() == Loader::ResultStatus::ErrorBLZDecompressionFailed);
    }
}

TEST_CASE("KIP decompresses a BLZ stream that expands more than four times", "[core][file_sys]") {
    u32 out_size{};
    const auto blz = MakeExpandingBlz(out_size);
    REQUIRE(out_size > 4 * blz.size() + 144);
    const KIP kip{MakeKip(blz, out_size)};
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::Success);
    const auto text = kip.GetSectionDecompressed(0);
    REQUIRE(text.size() == out_size);
    REQUIRE(std::string_view{reinterpret_cast<const char*>(text.data()) + out_size - 8, 8} ==
            "ABCDEFGH");
}

TEST_CASE("KIP keeps the bytes in front of a BLZ stream", "[core][file_sys]") {
    const auto blz = MakeBlz();
    std::vector<u8> section(5, 0x7E);
    section.insert(section.end(), blz.begin(), blz.end());
    std::vector<u8> bytes(0x100);
    Put32(bytes, 0, Common::MakeMagic('K', 'I', 'P', '1'));
    Put32(bytes, 0x24, static_cast<u32>(5 + 32));
    Put32(bytes, 0x28, static_cast<u32>(section.size()));
    bytes.insert(bytes.end(), section.begin(), section.end());
    const KIP kip{std::make_shared<VectorVfsFile>(std::move(bytes), "test.kip")};
    REQUIRE(kip.GetStatus() == Loader::ResultStatus::Success);
    const auto text = kip.GetSectionDecompressed(0);
    REQUIRE(text.size() == 5 + 32);
    CHECK(text[0] == 0x7E);
    CHECK(text[4] == 0x7E);
    CHECK(std::string_view{reinterpret_cast<const char*>(text.data()) + 5, 8} == "ABCDEFGH");
}
