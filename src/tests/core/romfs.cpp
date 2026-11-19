// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {
using FileSys::VectorVfsDirectory;
using FileSys::VectorVfsFile;

// A real image from the writer: "a.txt" ("hi") at the root and "s/b.txt" ("yo").
std::vector<u8> Image() {
    const auto file = [](const char* name, const char* text) -> FileSys::VirtualFile {
        return std::make_shared<VectorVfsFile>(std::vector<u8>(text, text + 2), name);
    };
    const auto sub =
        std::make_shared<VectorVfsDirectory>(std::vector<FileSys::VirtualFile>{file("b.txt", "yo")},
                                             std::vector<FileSys::VirtualDir>{}, "s");
    const auto root =
        std::make_shared<VectorVfsDirectory>(std::vector<FileSys::VirtualFile>{file("a.txt", "hi")},
                                             std::vector<FileSys::VirtualDir>{sub}, "");
    return FileSys::CreateRomFS(root)->ReadAllBytes();
}

u64 Read64(const std::vector<u8>& image, size_t at) {
    u64 value;
    std::memcpy(&value, image.data() + at, sizeof(value));
    return value;
}

template <typename T>
void Write(std::vector<u8>& image, size_t at, T value) {
    std::memcpy(image.data() + at, &value, sizeof(value));
}

FileSys::VirtualDir Extract(const std::vector<u8>& image) {
    return FileSys::ExtractRomFS(std::make_shared<VectorVfsFile>(image));
}

std::string Text(const FileSys::VirtualFile& file) {
    const auto bytes = file->ReadAllBytes();
    return std::string(bytes.begin(), bytes.end());
}

TEST_CASE("A RomFS image from the writer extracts its files and folders", "[core][romfs]") {
    const auto root = Extract(Image());
    REQUIRE(root != nullptr);
    const auto a = root->GetFile("a.txt");
    REQUIRE(a != nullptr);
    CHECK(Text(a) == "hi");
    const auto sub = root->GetSubdirectory("s");
    REQUIRE(sub != nullptr);
    const auto b = sub->GetFile("b.txt");
    REQUIRE(b != nullptr);
    CHECK(Text(b) == "yo");
}

TEST_CASE("A RomFS whose table lies past the end of the image is rejected", "[core][romfs]") {
    auto image = Image();
    Write(image, 0x40, u64{1} << 62);
    CHECK(Extract(image) == nullptr);
}

TEST_CASE("A RomFS whose table ends one byte past the image is rejected", "[core][romfs]") {
    auto image = Image();
    Write(image, 0x40, u64{image.size() - Read64(image, 0x38) + 1});
    CHECK(Extract(image) == nullptr);
}

TEST_CASE("A RomFS nested deeper than the depth limit is cut at the limit", "[core][romfs]") {
    // 0x180 levels of "/d" fill the 0x300-byte path the writer allows.
    FileSys::VirtualDir dir = std::make_shared<VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{}, "d");
    for (size_t level = 1; level < 0x180; ++level) {
        dir = std::make_shared<VectorVfsDirectory>(std::vector<FileSys::VirtualFile>{},
                                                   std::vector<FileSys::VirtualDir>{dir}, "d");
    }
    const auto root = std::make_shared<VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{dir}, "");
    FileSys::VirtualDir walk = Extract(FileSys::CreateRomFS(root)->ReadAllBytes());
    REQUIRE(walk != nullptr);
    size_t levels = 0;
    while ((walk = walk->GetSubdirectory("d")) != nullptr) {
        ++levels;
    }
    CHECK(levels == 0x17F);
}

TEST_CASE("A RomFS whose directory table is too short for the root is rejected", "[core][romfs]") {
    auto image = Image();
    Write(image, 0x20, u64{0x10});
    CHECK(Extract(image) == nullptr);
}

TEST_CASE("A RomFS whose file table is cut off keeps the readable files", "[core][romfs]") {
    auto image = Image();
    Write(image, 0x40, u64{0});
    const auto root = Extract(image);
    REQUIRE(root != nullptr);
    CHECK(root->GetFile("a.txt") == nullptr);
    CHECK(root->GetSubdirectory("s") != nullptr);
}

TEST_CASE("A RomFS file chain through overlapping entries stops at the entry budget",
          "[core][romfs]") {
    auto image = Image();
    const auto dir_meta = Read64(image, 0x18);
    const auto file_meta = Read64(image, 0x38);
    const auto file_meta_size = Read64(image, 0x40);
    // Every word holds its own offset, so the entry at offset n links to the entry at n + 4.
    for (u32 at = 0; at < file_meta_size; at += 4) {
        Write(image, file_meta + at, at);
    }
    Write(image, dir_meta + 0x0C, u32{0});
    const auto root = Extract(image);
    REQUIRE(root != nullptr);
    CHECK(root->GetFiles().size() == file_meta_size / 0x20);
}

TEST_CASE("A RomFS directory chain through overlapping entries stops at the entry budget",
          "[core][romfs]") {
    auto image = Image();
    const auto dir_meta = Read64(image, 0x18);
    const auto dir_meta_size = Read64(image, 0x20);
    // Past the root, every word holds its own offset: entry n links to n + 4 and n + 8.
    for (u32 at = 0x18; at < dir_meta_size; at += 4) {
        Write(image, dir_meta + at, at);
    }
    Write(image, dir_meta + 0x08, u32{0x18});
    const auto root = Extract(image);
    REQUIRE(root != nullptr);
    const auto count = [](const auto& self, const FileSys::VirtualDir& dir) -> size_t {
        size_t total = 0;
        for (const auto& sub : dir->GetSubdirectories()) {
            total += 1 + self(self, sub);
        }
        return total;
    };
    CHECK(count(count, root) == dir_meta_size / 0x18 - 1);
}

TEST_CASE("A RomFS directory that is its own sibling ends the walk", "[core][romfs]") {
    auto image = Image();
    const auto dir_meta = Read64(image, 0x18);
    Write(image, dir_meta + 0x04, u32{0});
    const auto root = Extract(image);
    REQUIRE(root != nullptr);
    CHECK(root->GetFile("a.txt") != nullptr);
}

TEST_CASE("A RomFS file that is its own sibling ends the walk", "[core][romfs]") {
    auto image = Image();
    const auto dir_meta = Read64(image, 0x18);
    const auto file_meta = Read64(image, 0x38);
    u32 first_file;
    std::memcpy(&first_file, image.data() + dir_meta + 0x0C, 4);
    Write(image, file_meta + first_file + 0x04, first_file);
    const auto root = Extract(image);
    REQUIRE(root != nullptr);
    CHECK(root->GetFile("a.txt") != nullptr);
}
} // namespace
