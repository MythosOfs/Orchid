// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "core/file_sys/partition_filesystem.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"

namespace {
// A PFS0 image: header, one 0x18-byte entry per (strtab_offset, offset, size), the string
// table, then content.
std::vector<u8> Pfs0(u32 num_entries, const std::vector<std::array<u64, 3>>& entries,
                     std::string_view strtab, std::string_view content) {
    std::vector<u8> out;
    const auto put = [&out](u64 value, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i) {
            out.push_back(static_cast<u8>(value >> (8 * i)));
        }
    };
    put(Common::MakeMagic('P', 'F', 'S', '0'), 4);
    put(num_entries, 4);
    put(strtab.size(), 4);
    put(0, 4);
    for (const auto& [strtab_offset, offset, size] : entries) {
        put(offset, 8);
        put(size, 8);
        put(strtab_offset, 4);
        put(0, 4);
    }
    out.insert(out.end(), strtab.begin(), strtab.end());
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

// Records the largest ReadBytes request, to prove the header is checked before reading.
class RecordingFile final : public FileSys::VectorVfsFile {
public:
    using VectorVfsFile::VectorVfsFile;
    std::vector<u8> ReadBytes(std::size_t size, std::size_t offset) const override {
        largest_read = (std::max)(largest_read, size);
        return VectorVfsFile::ReadBytes(size, offset);
    }
    mutable std::size_t largest_read = 0;
};

TEST_CASE("A PFS0 with one file parses its name and content", "[core][pfs]") {
    const auto image = Pfs0(1, {{0, 0, 5}}, std::string_view{"a.bin\0", 6}, "hello");
    const FileSys::PartitionFilesystem pfs{std::make_shared<FileSys::VectorVfsFile>(image)};

    REQUIRE(pfs.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(pfs.GetFiles().size() == 1);
    REQUIRE(pfs.GetFiles()[0]->GetName() == "a.bin");
    REQUIRE(pfs.GetFiles()[0]->ReadAllBytes() == std::vector<u8>{'h', 'e', 'l', 'l', 'o'});
}

TEST_CASE("A PFS0 entry name offset past the string table is rejected", "[core][pfs]") {
    const auto image = Pfs0(1, {{6, 0, 5}}, std::string_view{"a.bin\0", 6}, "hello");
    const FileSys::PartitionFilesystem pfs{std::make_shared<FileSys::VectorVfsFile>(image)};

    REQUIRE(pfs.GetStatus() == Loader::ResultStatus::ErrorBadPFSHeader);
    REQUIRE(pfs.GetFiles().empty());
}

TEST_CASE("A PFS0 header larger than its file is rejected before it is read", "[core][pfs]") {
    const auto image = Pfs0(0x100000, {}, "", "");
    const auto file = std::make_shared<RecordingFile>(image);
    const FileSys::PartitionFilesystem pfs{file};

    REQUIRE(pfs.GetStatus() == Loader::ResultStatus::ErrorIncorrectPFSFileSize);
    REQUIRE(file->largest_read <= image.size());
}

TEST_CASE("A PFS0 with more than 65535 entries lists every one", "[core][pfs]") {
    const std::vector<std::array<u64, 3>> entries(65537, {0, 0, 0});
    const auto image = Pfs0(65537, entries, std::string_view{"a\0", 2}, "");
    const FileSys::PartitionFilesystem pfs{std::make_shared<FileSys::VectorVfsFile>(image)};

    REQUIRE(pfs.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(pfs.GetFiles().size() == 65537);
}
} // namespace
