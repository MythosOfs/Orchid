// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <list>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <openssl/evp.h>

#include "common/settings.h"
#include "common/cpu_features.h"
#include "common/fs/path_util.h"
#include "core/launch_timestamp_cache.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_scheduler.h"
#include "common/scope_exit.h"
#include "core/core_timing.h"
#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/fssystem/fssystem_nca_header.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/nup.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/acc/acc.h"
#include "core/hle/service/aoc/addon_content_manager.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/window_system.h"
#include "core/hle/service/am/service/application_accessor.h"
#include "core/hle/service/am/service/application_creator.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/service/glue/arp.h"
#include "core/hle/service/am/service/application_functions.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/am/process_creation.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/romfs_controller.h"
#include "core/hle/service/glue/glue_manager.h"
#include "core/hle/service/ns/read_only_application_control_data_interface.h"
#include "core/hle/service/os/process.h"
#include "tests/core/system_test_context.h"

namespace {

namespace fs = std::filesystem;
using FileSys::NupContainer;

struct TempDir {
    fs::path path = fs::temp_directory_path() /
                    ("nup-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void Put(const fs::path& p, std::string_view value) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << value;
}

std::string ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string Read(const FileSys::VirtualFile& f) {
    std::string out(f->GetSize(), '\0');
    f->Read(reinterpret_cast<u8*>(out.data()), out.size(), 0);
    return out;
}

// Enough of an NPDM for the packer: the program ID at ACI0 + 0x10.
std::string Npdm(u64 program_id) {
    std::string npdm(0x100, '\0');
    npdm.replace(0x80, 4, "ACI0");
    for (int i = 0; i < 8; ++i) {
        npdm[0x90 + i] = static_cast<char>(program_id >> (8 * i));
    }
    return npdm;
}

std::array<u8, 32> Sha256(std::string_view data) {
    std::array<u8, 32> out{};
    unsigned int length{};
    EVP_Digest(data.data(), data.size(), out.data(), &length, EVP_sha256(), nullptr);
    return out;
}

u64 Le(const std::string& buf, size_t at, size_t bytes) {
    u64 value = 0;
    for (size_t i = 0; i < bytes; ++i) {
        value |= static_cast<u64>(static_cast<u8>(buf[at + i])) << (8 * i);
    }
    return value;
}

struct RawFile {
    std::string name;
    u64 size;
    std::array<u8, 32> sha256;
    u64 extent_offset;
    u64 extent_size;
    u8 codec;
};

// Reads the file records straight from the TOC, which NupContainer does not expose.
std::vector<RawFile> RawFiles(const std::string& nup) {
    const u64 toc = Le(nup, 0x08, 8);
    const u64 packages = Le(nup, toc + 0x04, 4);
    const u64 dirs = Le(nup, toc + 0x08, 8);
    const u64 files = Le(nup, toc + 0x10, 8);
    const u64 pool = toc + Le(nup, toc + 0x18, 8);
    std::vector<RawFile> out;
    u64 pos = toc + 0x30 + packages * 0x40 + dirs * 0x18;
    for (u64 i = 0; i < files; ++i, pos += 0x50) {
        RawFile file{nup.substr(pool + Le(nup, pos, 4), Le(nup, pos + 4, 4)),
                     Le(nup, pos + 0x08, 8),
                     {},
                     Le(nup, pos + 0x10, 8),
                     Le(nup, pos + 0x18, 8),
                     static_cast<u8>(nup[pos + 0x40])};
        std::copy_n(nup.begin() + static_cast<std::ptrdiff_t>(pos + 0x20), 32,
                    file.sha256.begin());
        out.push_back(file);
    }
    return out;
}

// Every file under `dir` with its contents, keyed by its path relative to `dir`.
std::map<std::string, std::string> SourceTree(const fs::path& dir) {
    std::map<std::string, std::string> out;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().filename() != ".listing" &&
            entry.path().filename() != ".nupmeta") {
            out[fs::relative(entry.path(), dir).generic_string()] = ReadAll(entry.path());
        }
    }
    return out;
}

void PackedTree(const FileSys::VirtualDir& dir, const std::string& prefix,
                std::map<std::string, std::string>& out) {
    if (dir == nullptr) {
        return;
    }
    for (const auto& file : dir->GetFiles()) {
        out[prefix + file->GetName()] = Read(file);
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        PackedTree(sub, prefix + sub->GetName() + "/", out);
    }
}

std::map<std::string, std::string> PackedTree(const FileSys::VirtualDir& dir) {
    std::map<std::string, std::string> out;
    PackedTree(dir, "", out);
    return out;
}

constexpr u64 TitleId = 0x0400ABCD00010000ULL;

void MakeDump(const fs::path& root) {
    Put(root / "Base/ExeFS/main", "base main");
    Put(root / "Base/ExeFS/main.npdm", Npdm(TitleId));
    Put(root / "Base/RomFS/shared.bin", std::string(5000, 's'));
    Put(root / "Base/RomFS/copy/shared.bin", std::string(5000, 's'));
    Put(root / "Base/RomFS/empty", "");
    Put(root / "Base/RomFS/.listing", "listing");
    Put(root / "Base/RomFS/System/Resource/ResourceSizeTable.Product.100.rsizetable", "rst");
    Put(root / "Base/RomFS/big.bin", std::string(64 * 1024, 'b'));
    Put(root / "Base/RomFS/random.bin", [] {
        std::string data(3000, '\0');
        u32 x = 12345;
        for (auto& c : data) {
            x = x * 1664525 + 1013904223;
            c = static_cast<char>(x >> 24);
        }
        return data;
    }());
    Put(root / "updates/65536/exefs/main", "update main");
    Put(root / "updates/65536/exefs/main.npdm", Npdm(TitleId));
    Put(root / "updates/65536/romfs/shared.bin", std::string(5000, 's'));
    Put(root / "updates/65536/romfs/System/Resource/ResourceSizeTable.Product.110.rsizetable",
        "rst");
    Put(root / "updates/65536/romfs/Zeta/a", "a");
    Put(root / "updates/65536/romfs/alpha", "alpha");
    Put(root / "updates/131072/exefs/main", "update 2 main");
    Put(root / "updates/131072/exefs/main.npdm", Npdm(TitleId));
    Put(root / "dlc/0400ABCD00011001/0/romfs/addon.bin", "addon data");
}

void SetLe(std::string& buf, size_t at, u64 value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        buf[at + i] = static_cast<char>(value >> (8 * i));
    }
}

// A structurally valid native NPDM, including the Switch 2 ACID signature area. The loader
// needs this distinction to select control/Acd_1 rather than the legacy control metadata.
std::string NativeNpdm(u64 program_id) {
    std::string npdm(0x500, '\0');
    npdm.replace(0, 4, "META");
    SetLe(npdm, 0x70, 0x80, 4); // ACI0 offset
    SetLe(npdm, 0x78, 0x100, 4); // ACID offset
    npdm.replace(0x80, 4, "ACI0");
    SetLe(npdm, 0x90, program_id, 8);
    SetLe(npdm, 0xA0, 0x40, 4); // FAH offset relative to ACI0
    npdm.replace(0x400, 4, "ACID");
    SetLe(npdm, 0x420, 0x340, 4); // FAC offset relative to ACID
    return npdm;
}

std::string BootNpdm(u64 program_id, bool switch2) {
    const u32 shift = switch2 ? 0x100 : 0;
    std::string npdm(0x480 + shift, '\0');
    npdm.replace(0, 4, "META");
    SetLe(npdm, 0x70, 0x400 + shift, 4);
    SetLe(npdm, 0x74, 0x40, 4);
    SetLe(npdm, 0x78, 0x100, 4);
    SetLe(npdm, 0x7C, 0x240 + shift, 4);
    npdm.replace(0x300 + shift, 4, "ACID");
    SetLe(npdm, 0x320 + shift, 0x240 + shift, 4);
    SetLe(npdm, 0x324 + shift, 0x2C, 4);
    npdm.replace(0x400 + shift, 4, "ACI0");
    SetLe(npdm, 0x410 + shift, program_id, 8);
    SetLe(npdm, 0x420 + shift, 0x40, 4);
    SetLe(npdm, 0x424 + shift, 0x1C, 4);
    SetLe(npdm, 0x430 + shift, 0x60, 4);
    return npdm;
}

class CountingNupFile final : public FileSys::VectorVfsFile {
public:
    using VectorVfsFile::VectorVfsFile;

    std::size_t Read(u8* out, std::size_t length, std::size_t offset) const override {
        ++reads;
        return VectorVfsFile::Read(out, length, offset);
    }

    mutable std::size_t reads{};
};

// Applies `patch` to a valid archive, fixes up the TOC hash and opens the result.
template <typename Patch>
Loader::ResultStatus StatusAfterPatch(std::string nup, Patch&& patch) {
    const u64 toc = Le(nup, 0x08, 8);
    patch(nup, toc);
    const auto hash = Sha256(std::string_view{nup}.substr(toc, Le(nup, 0x10, 8)));
    std::copy(hash.begin(), hash.end(), nup.begin() + 0x18);
    const auto data = std::vector<u8>(nup.begin(), nup.end());
    return NupContainer{std::make_shared<FileSys::VectorVfsFile>(data, "bad.nup")}.GetStatus();
}

} // namespace

TEST_CASE("NUP packs base, updates and add-ons and reads back through NupContainer", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    FileSys::Nup::PackOptions options{.raw_threshold = 16 * 1024};
    std::string error;
    REQUIRE(FileSys::Nup::PackDump(root, archive, options, {}, nullptr, &error) ==
            FileSys::Nup::PackResult::Success);
    INFO(error);
    REQUIRE_FALSE(fs::exists(archive.string() + ".part"));

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);

    const auto& packages = nup.GetPackages();
    REQUIRE(packages.size() == 4);
    REQUIRE(packages[0].kind == NupContainer::PackageKind::Base);
    REQUIRE(packages[0].title_id == TitleId);
    REQUIRE(packages[0].version == 0);
    REQUIRE(packages[0].display_version == "1.0.0");
    REQUIRE(packages[0].npdm.size() == 0x100);
    REQUIRE(packages[1].kind == NupContainer::PackageKind::Update);
    REQUIRE(packages[1].version == 65536);
    REQUIRE(packages[1].display_version == "1.1.0");
    REQUIRE(packages[1].title_id == TitleId);
    REQUIRE_FALSE(packages[1].standalone_update);
    REQUIRE(packages[2].kind == NupContainer::PackageKind::Update);
    REQUIRE(packages[2].version == 131072);
    REQUIRE(packages[2].romfs_dir == 0xFFFFFFFF);
    REQUIRE(packages[3].kind == NupContainer::PackageKind::Addon);
    REQUIRE(packages[3].title_id == 0x0400ABCD00011001ULL);
    REQUIRE(packages[3].exefs_dir == 0xFFFFFFFF);
    REQUIRE(nup.FindBestUpdatePackage() == std::size_t{2});

    REQUIRE(PackedTree(nup.GetExeFSDir(0)) == SourceTree(root / "Base/ExeFS"));
    REQUIRE(PackedTree(nup.GetRomFSDir(0)) == SourceTree(root / "Base/RomFS"));
    REQUIRE(PackedTree(nup.GetExeFSDir(1)) == SourceTree(root / "updates/65536/exefs"));
    REQUIRE(PackedTree(nup.GetRomFSDir(1)) == SourceTree(root / "updates/65536/romfs"));
    REQUIRE(PackedTree(nup.GetExeFSDir(2)) == SourceTree(root / "updates/131072/exefs"));
    REQUIRE(PackedTree(nup.GetRomFSDir(3)) == SourceTree(root / "dlc/0400ABCD00011001/0/romfs"));
    REQUIRE_FALSE(nup.GetRomFSDir(0)->GetFile(".listing"));

    const auto bytes = ReadAll(archive);
    const auto files = RawFiles(bytes);
    std::map<std::string, std::vector<RawFile>> by_name;
    for (const auto& file : files) {
        by_name[file.name].push_back(file);
        if (file.size != 0) {
            REQUIRE(file.extent_offset % 16 == 0);
        }
    }
    const auto shared = by_name.at("shared.bin");
    REQUIRE(shared.size() == 3);
    for (const auto& file : shared) {
        REQUIRE(file.sha256 == Sha256(std::string(5000, 's')));
        REQUIRE(file.codec == 1);
        REQUIRE(file.extent_offset == shared[0].extent_offset);
        REQUIRE(file.extent_size == shared[0].extent_size);
    }
    const auto& empty = by_name.at("empty").at(0);
    REQUIRE(empty.size == 0);
    REQUIRE(empty.extent_offset == 0);
    REQUIRE(empty.extent_size == 0);
    REQUIRE(empty.codec == 0);
    REQUIRE(empty.sha256 == Sha256(""));
    // Highly compressible, but above the raw threshold.
    const auto& big = by_name.at("big.bin").at(0);
    REQUIRE(big.codec == 0);
    REQUIRE(big.extent_size == 64 * 1024);
    REQUIRE(big.sha256 == Sha256(std::string(64 * 1024, 'b')));
    // Incompressible, so stored raw.
    const auto& random = by_name.at("random.bin").at(0);
    REQUIRE(random.codec == 0);
    REQUIRE(random.extent_size == 3000);
    REQUIRE_FALSE(by_name.contains(".listing"));
}

TEST_CASE("NUP reads zstd members above the memory cache through a temporary file", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    std::string large(300 * 1024, '\0');
    u32 x = 7;
    for (auto& c : large) {
        x = x * 1664525 + 1013904223;
        c = static_cast<char>('a' + (x >> 28));
    }
    Put(root / "Base/RomFS/large.bin", large);
    const auto archive = t.path / "Game.nup";
    FileSys::Nup::PackOptions options{.raw_threshold = 1024 * 1024};
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);
    for (const auto& file : RawFiles(ReadAll(archive))) {
        if (file.name == "large.bin") {
            REQUIRE(file.codec == 1);
        }
    }

    const auto spill_dir = t.path / "spill";
    FileSys::RealVfsFilesystem real;
    {
        // Members above 32 KiB do not fit the 64 KiB cache budget and are spilled to disk.
        NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read), 64 * 1024,
                         spill_dir};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        const auto file = nup.GetRomFSDir(0)->GetFile("large.bin");
        REQUIRE(file != nullptr);
        REQUIRE(file->GetSize() == large.size());

        std::string chunk(4096, '\0');
        for (size_t offset = 0; offset < large.size(); offset += chunk.size()) {
            const size_t count =
                file->Read(reinterpret_cast<u8*>(chunk.data()), chunk.size(), offset);
            REQUIRE(count == std::min(chunk.size(), large.size() - offset));
            REQUIRE(chunk.compare(0, count, large, offset, count) == 0);
        }
        for (const size_t offset : {size_t{0}, size_t{123456}, size_t{77}, large.size() - 5}) {
            const size_t count = file->Read(reinterpret_cast<u8*>(chunk.data()), 100, offset);
            REQUIRE(count == std::min<size_t>(100, large.size() - offset));
            REQUIRE(chunk.compare(0, count, large, offset, count) == 0);
        }
        REQUIRE(file->Read(reinterpret_cast<u8*>(chunk.data()), 10, large.size()) == 0);

        REQUIRE(std::distance(fs::directory_iterator(spill_dir), fs::directory_iterator{}) > 0);
        REQUIRE(PackedTree(nup.GetRomFSDir(0)) == SourceTree(root / "Base/RomFS"));
        REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    }
    REQUIRE(std::distance(fs::directory_iterator(spill_dir), fs::directory_iterator{}) == 0);
}

TEST_CASE("NUP rejects a TOC whose counts or offsets wrap", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    const auto nup = ReadAll(archive);
    const auto no_patch = [](std::string&, u64) {};
    REQUIRE(StatusAfterPatch(nup, no_patch) == Loader::ResultStatus::Success);

    // 2^61 directories wrap dir_count * 0x18 back to the real record size.
    REQUIRE(StatusAfterPatch(nup, [](std::string& buf, u64 toc) {
                SetLe(buf, toc + 0x08, Le(buf, toc + 0x08, 8) + (1ULL << 61), 8);
            }) == Loader::ResultStatus::ErrorNUPBadFormat);

    const auto first_dir = [](const std::string& buf, u64 toc) {
        return toc + 0x30 + Le(buf, toc + 0x04, 4) * 0x40;
    };
    // first_child + child_count wraps to a small value.
    REQUIRE(StatusAfterPatch(nup, [&](std::string& buf, u64 toc) {
                SetLe(buf, first_dir(buf, toc) + 0x08, 0xFFFFFFFF, 4);
                SetLe(buf, first_dir(buf, toc) + 0x0C, 2, 4);
            }) == Loader::ResultStatus::ErrorNUPBadFormat);
}

TEST_CASE("NUP rejects a directory that contains its own ancestor", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    // Give the base RomFS directory the same children as the package root, which lists it.
    REQUIRE(StatusAfterPatch(ReadAll(archive), [](std::string& buf, u64 toc) {
                const u64 dirs = toc + 0x30 + Le(buf, toc + 0x04, 4) * 0x40;
                const u64 root_dir = Le(buf, toc + 0x30 + 0x08, 4);
                const u64 romfs_dir = Le(buf, toc + 0x30 + 0x10, 4);
                const u64 root_record = dirs + root_dir * 0x18;
                const u64 romfs_record = dirs + romfs_dir * 0x18;
                SetLe(buf, romfs_record + 0x08, Le(buf, root_record + 0x08, 4), 4);
                SetLe(buf, romfs_record + 0x0C, Le(buf, root_record + 0x0C, 4), 4);
            }) == Loader::ResultStatus::ErrorNUPBadFormat);
}

TEST_CASE("NUP rejects a directory listed by more than one parent", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    // Point the base ExeFS directory at the RomFS directory's children. No cycle, but a chain of
    // such shared directories would make BuildDirTree expand exponentially.
    REQUIRE(StatusAfterPatch(ReadAll(archive), [](std::string& buf, u64 toc) {
                const u64 dirs = toc + 0x30 + Le(buf, toc + 0x04, 4) * 0x40;
                const u64 exefs_dir = Le(buf, toc + 0x30 + 0x0C, 4);
                const u64 romfs_dir = Le(buf, toc + 0x30 + 0x10, 4);
                const u64 exefs_record = dirs + exefs_dir * 0x18;
                const u64 romfs_record = dirs + romfs_dir * 0x18;
                SetLe(buf, exefs_record + 0x08, Le(buf, romfs_record + 0x08, 4), 4);
                SetLe(buf, exefs_record + 0x0C, Le(buf, romfs_record + 0x0C, 4), 4);
            }) == Loader::ResultStatus::ErrorNUPBadFormat);
}

TEST_CASE("NUP marks updates standalone when the dump has no base", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "updates/65536/exefs/main", "main");
    Put(root / "updates/65536/exefs/main.npdm", Npdm(TitleId));
    Put(root / "updates/65536/romfs/file", "rom");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages().size() == 1);
    REQUIRE(nup.GetPackages()[0].kind == NupContainer::PackageKind::Update);
    REQUIRE(nup.GetPackages()[0].standalone_update);
    Loader::AppLoader_NUP loader{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    const auto embedded = loader.ReadEmbeddedPatches();
    REQUIRE(embedded.size() == 1);
    REQUIRE(embedded[0].type == FileSys::PatchType::Update);
    REQUIRE(embedded[0].numeric_version == 65536);
    REQUIRE(nup.GetPackages()[0].title_id == TitleId);
    REQUIRE(Read(nup.GetRomFSDir(0)->GetFile("file")) == "rom");
}

TEST_CASE("NUP packs a bare exefs/romfs dump as one base", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "ExeFS/main", "main");
    Put(root / "ExeFS/main.npdm", Npdm(TitleId));
    Put(root / "RomFS/file", "rom");
    Put(root / "notes.txt", "extra");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages().size() == 1);
    REQUIRE(nup.GetPackages()[0].kind == NupContainer::PackageKind::Base);
    REQUIRE_FALSE(nup.GetPackages()[0].standalone_update);
    REQUIRE(Read(nup.GetExeFSDir(0)->GetFile("main")) == "main");
    REQUIRE(Read(nup.GetRomFSDir(0)->GetFile("file")) == "rom");
    const auto files = RawFiles(ReadAll(archive));
    REQUIRE(std::ranges::count(files, std::string{"notes.txt"}, &RawFile::name) == 1);
}

TEST_CASE("NUP low storage resumes and deletes the packed sources", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto copy = t.path / "copy" / "Game";
    fs::create_directories(copy);
    fs::copy(root, copy, fs::copy_options::recursive);
    const auto archive = t.path / "Game.nup";

    std::atomic_bool cancel = false;
    bool cancelled = false;
    FileSys::Nup::PackOptions options{.delete_sources = true, .chunk_bytes = 1};
    auto result = FileSys::Nup::PackDump(
        root, archive, options,
        [&](const auto& p) {
            if (p.files_done >= 4 && !cancelled) {
                cancel = true;
                cancelled = true;
            }
        },
        &cancel);
    REQUIRE(result == FileSys::Nup::PackResult::Cancelled);
    REQUIRE(FileSys::Nup::HasResumableJournal(archive));
    REQUIRE_FALSE(fs::exists(root / "Base/ExeFS/main"));
    REQUIRE_FALSE(fs::exists(root / "Base/ExeFS/main.npdm"));

    cancel = false;
    std::string error;
    REQUIRE(FileSys::Nup::PackDump(root, archive, options, {}, &cancel, &error) ==
            FileSys::Nup::PackResult::Success);
    INFO(error);
    REQUIRE_FALSE(fs::exists(archive.string() + ".journal"));
    REQUIRE_FALSE(fs::exists(archive.string() + ".part"));
    REQUIRE_FALSE(fs::exists(root / "Base/ExeFS"));
    REQUIRE_FALSE(fs::exists(root / "updates"));
    REQUIRE_FALSE(fs::exists(root / "dlc"));
    // The listing is not packed, so it is left behind with the folders holding it.
    REQUIRE(ReadAll(root / "Base/RomFS/.listing") == "listing");
    REQUIRE_FALSE(fs::exists(root / "Base/RomFS/shared.bin"));

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages().size() == 4);
    const auto npdm = Npdm(TitleId);
    REQUIRE(nup.GetPackages()[0].npdm == std::vector<u8>(npdm.begin(), npdm.end()));
    REQUIRE(PackedTree(nup.GetRomFSDir(0)) == SourceTree(copy / "Base/RomFS"));
    REQUIRE(PackedTree(nup.GetRomFSDir(1)) == SourceTree(copy / "updates/65536/romfs"));

    // The resumed archive matches one packed in a single pass.
    const auto again = t.path / "again.nup";
    REQUIRE(FileSys::Nup::PackDump(copy, again) == FileSys::Nup::PackResult::Success);
    REQUIRE(ReadAll(archive) == ReadAll(again));
}

TEST_CASE("NUP rejects overwrite and a dump without NPDM", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "exefs/main", "main");
    Put(root / "romfs/a", "rom");
    const auto archive = t.path / "out.nup";
    Put(archive, "existing");
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::IoError);
    REQUIRE(ReadAll(archive) == "existing");
    fs::remove(archive);
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::InvalidSource);
    REQUIRE_FALSE(fs::exists(archive.string() + ".part"));
}

// Archives are meant to be byte-identical on every platform. A change to this hash means
// packing a dump no longer reproduces archives made by earlier builds (for example after a zstd
// update), so make it deliberately.
TEST_CASE("NUP output is reproducible", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    // Large and compressible, so it covers what used to be zstd's multithreaded path.
    Put(root / "Base/RomFS/large.bin", [] {
        std::string data(20 * 1024 * 1024, '\0');
        u32 x = 1;
        for (auto& c : data) {
            x = x * 1664525 + 1013904223;
            c = static_cast<char>('a' + (x >> 28));
        }
        return data;
    }());
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    std::string hex;
    for (const u8 b : Sha256(ReadAll(archive))) {
        hex += fmt::format("{:02x}", b);
    }
    // The mixed-case Base/RomFS directory supplies base version 1.0.0 on every platform.
    REQUIRE(hex == "718231e0974ca04c7be7bd6ed4334015b777c6e5f1ed51de23f899f81ee004ab");
}

TEST_CASE("NUP writes metadata overrides to the base and the newest update", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto probed = FileSys::Nup::ProbeMetadata(root);
    REQUIRE(probed.title_name.empty());
    REQUIRE(probed.icon.empty());
    // The version comes from the RomFS resource size table of the newest update that has one.
    REQUIRE(probed.display_version == "1.1.0");
    REQUIRE(probed.version_source == "update 65536");

    FileSys::Nup::PackOptions options;
    options.metadata.title_name = "Custom Name";
    options.metadata.icon = {0xFF, 0xD8, 0xFF, 0xD9};
    options.metadata.display_version = "9.8.7";
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    const auto& packages = nup.GetPackages();
    REQUIRE(packages.size() == 4);
    REQUIRE(packages[0].title_name == "Custom Name");
    REQUIRE(packages[0].icon == options.metadata.icon);
    REQUIRE(packages[0].display_version == "9.8.7");
    REQUIRE(packages[2].version == 131072);
    REQUIRE(packages[2].display_version == "9.8.7");
    REQUIRE(packages[1].display_version == "1.1.0");
}

TEST_CASE("NUP probes the version from the game data list tag", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "base/exefs/main", "main");
    Put(root / "base/exefs/main.npdm", Npdm(TitleId));
    Put(root / "base/romfs/GameData/GameDataList.Product.100.byml.zs", "x");
    Put(root / "base/romfs/System/Resource/ResourceSizeTable.Product.rsizetable.zs", "x");
    Put(root / "updates/458752/exefs/main", "main");
    Put(root / "updates/458752/exefs/main.npdm", Npdm(TitleId));
    Put(root / "updates/458752/romfs/GameData/GameDataList.Product.320.byml.zs", "x");
    Put(root / "updates/458752/romfs/GameData/GameDataList.Product.320.revision.txt", "1");

    auto probed = FileSys::Nup::ProbeMetadata(root);
    REQUIRE(probed.display_version == "3.2.0");
    REQUIRE(probed.version_source == "update 458752");

    fs::remove_all(root / "updates");
    probed = FileSys::Nup::ProbeMetadata(root);
    REQUIRE(probed.display_version == "1.0.0");
    REQUIRE(probed.version_source == "base");
}

TEST_CASE("NUP exposes only the newest DLC RomFS for the running application", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    Put(root / "dlc/0400ABCD00011001/65536/romfs/addon.bin", "new addon");
    Put(root / "dlc/0400ABCD00011002/0/romfs/second.bin", "second addon");
    Put(root / "dlc/0400ABCD00021001/0/romfs/foreign.bin", "other game");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive, {}, {}, nullptr, nullptr) ==
            FileSys::Nup::PackResult::Success);
    FileSys::RealVfsFilesystem real;
    std::vector<std::pair<u64, FileSys::VirtualDir>> addons;
    {
        NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        const auto indices = nup.FindAddonPackages(TitleId);
        REQUIRE(indices.size() == 2);
        REQUIRE(nup.GetPackages()[indices[0]].title_id == TitleId + 0x1001);
        REQUIRE(nup.GetPackages()[indices[0]].version == 65536);
        REQUIRE(nup.FindAddonPackages(0x0400ABCD00030000ULL).empty());
        addons = nup.GetAddonRomFS(TitleId);
        REQUIRE(nup.GetAddonRomFS(0x0400ABCD00030000ULL).empty());
    }
    // Published RomFS files must retain their backing container after loader teardown.
    REQUIRE(addons.size() == 2);
    REQUIRE(addons[0].first == TitleId + 0x1001);
    REQUIRE(Read(addons[0].second->GetFile("addon.bin")) == "new addon");
    REQUIRE(addons[1].first == TitleId + 0x1002);
    REQUIRE(Read(addons[1].second->GetFile("second.bin")) == "second addon");
}

TEST_CASE("NUP packs add-ons from nested dump layouts and their newest patch", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    fs::remove_all(root / "dlc");
    Put(root / "dlc/0400ABCD00011001/0/base/program/0/romfs/a.bin", "base program");
    Put(root / "dlc/0400ABCD00011002/0/Base/RomFS/b.bin", "base");
    Put(root / "dlc/0400ABCD00011003/0/program/0/romfs/c.bin", "program");
    Put(root / "dlc/0400ABCD00011003/0/updates/1/program/0/romfs/c.bin", "low patch");
    // The newest patch replaces the add-on's base; a patch without content does not count.
    Put(root / "dlc/0400ABCD00011004/65536/base/program/0/romfs/d.bin", "unpatched");
    Put(root / "dlc/0400ABCD00011004/65536/updates/65536/program/0/romfs/d.bin", "patch 1");
    Put(root / "dlc/0400ABCD00011004/65536/updates/131072/program/0/romfs/d.bin", "patch 2");
    Put(root / "dlc/0400ABCD00011004/65536/updates/196608/program/0/meta/x.cnmt", "no content");
    Put(root / "dlc/0400ABCD00011004/65536/updates/notes/program/0/romfs/d.bin", "not a patch");
    Put(root / "dlc/0400ABCD00011005/0/meta/x.cnmt", "no content");
    Put(root / "dlc/0400ABCD00011006/unversioned/romfs/e.bin", "unversioned");
    Put(root / "dlc/0400ABCD00011007", "a file, not a title folder");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    const auto indices = nup.FindAddonPackages(TitleId);
    REQUIRE(indices.size() == 5);
    const auto addon = [&](std::size_t i) { return PackedTree(nup.GetRomFSDir(indices[i])); };
    using Tree = std::map<std::string, std::string>;
    REQUIRE(addon(0) == Tree{{"a.bin", "base program"}});
    REQUIRE(addon(1) == Tree{{"b.bin", "base"}});
    REQUIRE(addon(2) == Tree{{"c.bin", "low patch"}});
    REQUIRE(addon(3) == Tree{{"d.bin", "patch 2"}});
    REQUIRE(nup.GetPackages()[indices[3]].title_id == TitleId + 0x1004);
    REQUIRE(nup.GetPackages()[indices[3]].version == 65536);
    REQUIRE(addon(4) == Tree{{"e.bin", "unversioned"}});
    REQUIRE(nup.GetPackages()[indices[4]].version == 0);
}

TEST_CASE("NUP preserves edition control metadata and legacy DLC ownership", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    fs::rename(root / "dlc/0400ABCD00011001", root / "dlc/0100ABCD00011001");
    FileSys::NACP control;
    control.raw.save_data_owner_id = 0x0100ABCD00010000ULL;
    control.raw.dlc_base_title_id = 0x0100ABCD00011000ULL;
    control.raw.initial_program_index = 1;
    control.raw.accessible_program_index_flags = 3;
    const auto bytes = control.GetRawBytes();
    Put(root / "Base/control.nacp",
        std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    const auto archive = t.path / "Game.nup";
    FileSys::Nup::PackOptions options;
    options.metadata.title_name = "Native Edition Override";
    options.metadata.display_version = "9.8.7";
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);
    FileSys::RealVfsFilesystem real;
    Loader::AppLoader_NUP loader{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    FileSys::NACP restored;
    REQUIRE(loader.ReadControlData(restored) == Loader::ResultStatus::Success);
    REQUIRE(restored.GetRawBytes() == bytes);
    std::string title, version;
    REQUIRE(loader.ReadTitle(title) == Loader::ResultStatus::Success);
    REQUIRE(title == "Native Edition Override");
    REQUIRE(loader.ReadDisplayVersion(version) == Loader::ResultStatus::Success);
    REQUIRE(version == "9.8.7");
    const auto embedded = loader.ReadEmbeddedPatches();
    REQUIRE(std::count_if(embedded.begin(), embedded.end(), [](const auto& patch) {
                return patch.type == FileSys::PatchType::DLC &&
                       patch.title_id == 0x0100ABCD00011001ULL;
            }) == 1);
    REQUIRE(restored.GetDLCBaseTitleId() == 0x0100ABCD00011000ULL);
    REQUIRE(restored.GetTitleId() == 0x0100ABCD00010000ULL);
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    const auto addons = nup.GetAddonRomFS(0x0100ABCD00010000ULL);
    REQUIRE(addons.size() == 1);
    REQUIRE(addons[0].first == 0x0100ABCD00011001ULL);
    REQUIRE(Read(addons[0].second->GetFile("addon.bin")) == "addon data");
    // Without the legacy DLC base, the 0100 add-on still belongs to the 0400 edition program.
    const auto edition_addons = nup.GetAddonRomFS(TitleId);
    REQUIRE(edition_addons.size() == 1);
    REQUIRE(edition_addons[0].first == TitleId + 0x1001);
    REQUIRE(StatusAfterPatch(ReadAll(archive), [](std::string& bytes_, u64 toc) {
                const u64 meta = Le(bytes_, toc + 0x30 + 0x20, 8);
                REQUIRE(Le(bytes_, meta, 2) == 1); // The first TLV is the NPDM.
                SetLe(bytes_, meta, 2, 2); // A short NPDM cannot be accepted as a raw NACP.
            }) == Loader::ResultStatus::ErrorNUPBadFormat);
}

TEST_CASE("NUP packs the preferred edition program with its family DLC", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root); // A legacy base and updates must not override the edition program.
    constexpr u64 legacy_id = 0x0100ABCD00010000ULL;
    constexpr u64 native_id = TitleId + 1;
    const auto programs = root / "upgrade/65536/program";
    Put(programs / "0/exefs/main", "legacy program 0");
    Put(programs / "0/exefs/main.npdm", Npdm(legacy_id));
    Put(programs / "0/romfs/legacy.bin", "legacy data");
    Put(programs / "1/exefs/main", "native program 1");
    Put(programs / "1/exefs/main.npdm", NativeNpdm(native_id));
    Put(programs / "1/romfs/Empty", "");
    fs::rename(root / "dlc/0400ABCD00011001", root / "dlc/0100ABCD00011001");

    FileSys::NACP legacy;
    const std::string legacy_name = "Legacy game";
    std::copy(legacy_name.begin(), legacy_name.end(),
              legacy.raw.language_entries.language_entries[0].application_name.begin());
    const auto legacy_bytes = legacy.GetRawBytes();
    Put(programs / "1/control/control.nacp",
        std::string_view{reinterpret_cast<const char*>(legacy_bytes.data()), legacy_bytes.size()});
    FileSys::NACP edition;
    const std::string name = "Native edition";
    std::copy(name.begin(), name.end(),
              edition.raw.language_entries.language_entries[0].application_name.begin());
    const std::string version = "2.3.4";
    std::copy(version.begin(), version.end(), edition.raw.version_string.begin());
    edition.raw.save_data_owner_id = legacy_id;
    edition.raw.dlc_base_title_id = legacy_id + 0x1000;
    edition.raw.program_index = 1;
    edition.raw.initial_program_index = 1;
    edition.raw.acd_index = 1;
    edition.raw.apparent_platform = FileSys::ApparentPlatform::Ounce;
    const auto edition_bytes = edition.GetRawBytes();
    Put(programs / "1/control/Acd_1/control.nacp",
        std::string_view{reinterpret_cast<const char*>(edition_bytes.data()), edition_bytes.size()});

    REQUIRE(FileSys::Nup::IsLooseDump(root));
    REQUIRE(FileSys::Nup::DumpRootFromMain(programs / "1/exefs/main") == root);
    REQUIRE(FileSys::Nup::DumpRootFromMain(programs / "1/exefs") == root);
    REQUIRE(FileSys::Nup::DumpRootFromMain(programs / "0/exefs/main") == root);
    const auto metadata = FileSys::Nup::ProbeMetadata(root);
    REQUIRE(metadata.title_name == name);
    REQUIRE(metadata.display_version == version);
    REQUIRE(metadata.version_source == "program 1");

    const auto archive = t.path / "Edition.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages().size() == 3);
    REQUIRE(nup.GetPackages()[0].kind == NupContainer::PackageKind::Base);
    REQUIRE(nup.GetPackages()[0].title_id == native_id);
    REQUIRE(nup.FindBasePackage() == std::size_t{0});
    REQUIRE_FALSE(nup.FindBestUpdatePackage());
    REQUIRE(Read(nup.GetExeFSDir(0)->GetFile("main")) == "native program 1");
    REQUIRE(PackedTree(nup.GetRomFSDir(0)) == SourceTree(programs / "1/romfs"));
    // Program 0 rides along as a further base package under its program-index title ID, not
    // the legacy ID of its NPDM.
    REQUIRE(nup.GetPackages()[1].kind == NupContainer::PackageKind::Base);
    REQUIRE(nup.GetPackages()[1].title_id == TitleId);
    const auto siblings = nup.GetSiblingProgramRomFS();
    REQUIRE(siblings.size() == 1);
    REQUIRE(siblings[0].first == TitleId);
    REQUIRE(Read(siblings[0].second->GetFile("legacy.bin")) == "legacy data");
    REQUIRE(nup.GetPackages()[2].kind == NupContainer::PackageKind::Addon);
    REQUIRE(nup.GetPackages()[2].title_id == legacy_id + 0x1001);
    REQUIRE(Read(nup.GetRomFSDir(2)->GetFile("addon.bin")) == "addon data");
    Loader::AppLoader_NUP loader{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    FileSys::NACP restored;
    REQUIRE(loader.ReadControlData(restored) == Loader::ResultStatus::Success);
    REQUIRE(restored.GetRawBytes() == edition_bytes);
}

TEST_CASE("NUP falls back from incomplete edition programs", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto programs = root / "upgrade/65536/program";
    Put(programs / "1/exefs/main", "incomplete native program"); // Missing NPDM.
    Put(programs / "0/exefs/main", "fallback program 0");
    Put(programs / "0/exefs/main.npdm", Npdm(TitleId));
    Put(programs / "0/romfs/GameData/GameDataList.Product.340.byml.zs", "x");
    bool expect_program_zero = true;
    SECTION("program 0 remains usable") {}
    SECTION("legacy layouts remain available when both variants are incomplete") {
        fs::remove(programs / "0/exefs/main.npdm");
        expect_program_zero = false;
    }
    REQUIRE(FileSys::Nup::IsLooseDump(root));
    const auto archive = t.path / "Fallback.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    if (expect_program_zero) {
        REQUIRE(nup.GetPackages().size() == 2);
        REQUIRE(Read(nup.GetExeFSDir(0)->GetFile("main")) == "fallback program 0");
        const auto metadata = FileSys::Nup::ProbeMetadata(root);
        REQUIRE(metadata.display_version == "3.4.0");
        REQUIRE(metadata.version_source == "program 0");
    } else {
        REQUIRE(nup.GetPackages().size() == 4);
        const auto update = nup.FindBestUpdatePackage();
        REQUIRE(update);
        REQUIRE(Read(nup.GetExeFSDir(*update)->GetFile("main")) == "update 2 main");
        REQUIRE(FileSys::Nup::DumpRootFromMain(root / "Base/ExeFS/main") == root);
    }
}

TEST_CASE("NUP boots program1 from a standalone legacy update", "[nup][nup-program1]") {
    TempDir t;
    constexpr u64 legacy_id = 0x0100ABCD00010000ULL;
    constexpr u64 edition_id = TitleId + 1;
    const auto root = t.path / "Game";
    const auto update = root / "updates/65536";
    const auto program = update / "program1";
    Put(update / "exefs/main", "legacy executable");
    Put(update / "exefs/main.npdm", Npdm(legacy_id));
    Put(update / "romfs/shared", "legacy data");
    Put(update / "romfs/legacy", "legacy-only data");
    Put(program / "romfs/shared", "edition data");

    bool boot_edition = true;
    SECTION("Matching native program") {
        Put(program / "exefs/main", "edition executable");
        Put(program / "exefs/main.npdm", NativeNpdm(edition_id));
    }
    SECTION("Missing executable") {
        boot_edition = false;
        Put(program / "exefs/main.npdm", NativeNpdm(edition_id));
    }
    SECTION("Unrelated title") {
        boot_edition = false;
        Put(program / "exefs/main", "other executable");
        Put(program / "exefs/main.npdm", NativeNpdm(edition_id + 0x2000));
    }
    SECTION("Non-native metadata") {
        boot_edition = false;
        Put(program / "exefs/main", "other executable");
        auto npdm = NativeNpdm(edition_id);
        npdm.replace(0x300, 4, "ACID");
        Put(program / "exefs/main.npdm", npdm);
    }

    FileSys::NACP control;
    control.raw.dlc_base_title_id = legacy_id + 0x1000;
    const auto bytes = control.GetRawBytes();
    Put(program / "control/Acd_1/control.nacp",
        std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    Put(root / "dlc/0100ABCD00011001/0/romfs/addon", "addon data");

    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    FileSys::RealVfsFilesystem real;
    const auto file = real.OpenFile(archive.string(), FileSys::OpenMode::Read);
    NupContainer nup{file};
    REQUIRE_FALSE(nup.FindBasePackage());
    REQUIRE(nup.FindBestUpdatePackage() == std::size_t{0});
    REQUIRE(nup.GetPackages()[0].title_id == legacy_id);

    Loader::AppLoader_NUP loader{file};
    u64 program_id{};
    REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
    REQUIRE(program_id == (boot_edition ? edition_id : legacy_id));
    if (boot_edition) {
        FileSys::ProgramMetadata metadata;
        REQUIRE(loader.ReadProgramMetadata(metadata) == Loader::ResultStatus::Success);
        REQUIRE(metadata.IsSwitch2());
        REQUIRE(metadata.GetTitleID() == edition_id);
        FileSys::NACP restored;
        REQUIRE(loader.ReadControlData(restored) == Loader::ResultStatus::Success);
        REQUIRE(restored.GetDLCBaseTitleId() == legacy_id + 0x1000);
    }

    FileSys::VirtualFile romfs;
    REQUIRE(loader.ReadRomFS(romfs) == Loader::ResultStatus::Success);
    const auto image = FileSys::ExtractRomFS(romfs);
    REQUIRE(image);
    REQUIRE(Read(image->GetFile("legacy")) == "legacy-only data");
    REQUIRE(Read(image->GetFile("shared")) == (boot_edition ? "edition data" : "legacy data"));
    const auto patches = loader.ReadEmbeddedPatches();
    REQUIRE(std::ranges::any_of(patches, [](const auto& patch) {
        return patch.type == FileSys::PatchType::DLC && patch.title_id == legacy_id + 0x1001;
    }));
    REQUIRE(std::ranges::any_of(patches, [](const auto& patch) {
        return patch.type == FileSys::PatchType::Edition && patch.title_id == edition_id;
    }) == boot_edition);
}

TEST_CASE("NUP UI metadata belongs to each container and never invents a base update", "[nup]") {
    TempDir t;
    FileSys::RealVfsFilesystem real;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto full = t.path / "full.nup";
    REQUIRE(FileSys::Nup::PackDump(root, full, {}, {}, nullptr, nullptr) ==
            FileSys::Nup::PackResult::Success);
    Loader::AppLoader_NUP full_loader{real.OpenFile(full.string(), FileSys::OpenMode::Read)};
    const auto raw = ReadAll(full);
    auto counted =
        std::make_shared<CountingNupFile>(std::vector<u8>(raw.begin(), raw.end()), "counted.nup");
    Loader::AppLoader_NUP counted_loader{counted};
    counted->reads = 0;
    const auto counted_patches = counted_loader.ReadEmbeddedPatches();
    REQUIRE(counted->reads == 0);
    const auto patches = full_loader.ReadEmbeddedPatches();
    REQUIRE(counted_patches.size() == patches.size());
    REQUIRE(std::count_if(patches.begin(), patches.end(), [](const auto& patch) {
                return patch.type == FileSys::PatchType::Update &&
                       patch.numeric_version == 131072 &&
                       patch.source == FileSys::PatchSource::Embedded;
            }) == 1);
    fs::remove_all(root / "updates");
    fs::remove_all(root / "dlc");
    const auto base = t.path / "base.nup";
    REQUIRE(FileSys::Nup::PackDump(root, base, {}, {}, nullptr, nullptr) ==
            FileSys::Nup::PackResult::Success);
    Loader::AppLoader_NUP base_loader{real.OpenFile(base.string(), FileSys::OpenMode::Read)};
    REQUIRE(base_loader.ReadEmbeddedPatches().empty());
    REQUIRE(full_loader.ReadEmbeddedPatches().size() == patches.size());
}

TEST_CASE("NUP DLC metadata ignores an unusable newest package", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    Put(root / "dlc/0400ABCD00011001/65536/romfs/new.bin", "new");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    auto raw = ReadAll(archive);
    const auto toc = Le(raw, 0x08, 8);
    for (u64 i = 0; i < Le(raw, toc + 4, 4); ++i) {
        const auto record = toc + 0x30 + i * 0x40;
        if (Le(raw, record, 8) == TitleId + 0x1001 && Le(raw, record + 0x14, 4) == 65536) {
            SetLe(raw, record + 0x10, 0xFFFFFFFF, 4);
        }
    }
    const auto hash = Sha256(std::string_view{raw}.substr(toc, Le(raw, 0x10, 8)));
    std::copy(hash.begin(), hash.end(), raw.begin() + 0x18);
    NupContainer nup{std::make_shared<FileSys::VectorVfsFile>(
        std::vector<u8>(raw.begin(), raw.end()), "metadata.nup")};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    const auto selected = nup.FindAddonPackages(TitleId);
    REQUIRE(selected.size() == 1);
    REQUIRE(nup.GetPackages()[selected[0]].version == 0);
}

TEST_CASE("NUP embedded patch merging deduplicates DLC identities", "[nup]") {
    Core::System system;
    FileSys::ContentProviderUnion provider{};
    FileSys::PatchManager pm{TitleId, system.GetFileSystemController(), provider};
    const FileSys::Patch addon{.enabled = true,
                               .name = "DLC",
                               .version = "1",
                               .type = FileSys::PatchType::DLC,
                               .program_id = TitleId,
                               .title_id = TitleId + 0x1001,
                               .source = FileSys::PatchSource::Embedded};
    const FileSys::Patch update{.enabled = true,
                                .name = "Update",
                                .version = "2.0.0",
                                .type = FileSys::PatchType::Update,
                                .program_id = TitleId,
                                .title_id = TitleId,
                                .source = FileSys::PatchSource::Embedded,
                                .numeric_version = 131072};
    const auto patches = pm.GetPatches(nullptr, {addon, addon, update, update});
    REQUIRE(std::count_if(patches.begin(), patches.end(), [](const auto& patch) {
                return patch.type == FileSys::PatchType::Update;
            }) == 1);
    const auto dlc = std::find_if(patches.begin(), patches.end(), [](const auto& patch) {
        return patch.type == FileSys::PatchType::DLC;
    });
    REQUIRE(dlc != patches.end());
    REQUIRE(dlc->version == "1");
    REQUIRE(dlc->enabled);
    REQUIRE(dlc->source == FileSys::PatchSource::Embedded);
}

TEST_CASE("NUP metadata merges installed and embedded content without duplicate IDs", "[nup]") {
    std::vector<FileSys::Patch> patches;
    std::vector<FileSys::ContentProviderEntry> installed{
        {.title_id = TitleId + 0x1001, .type = FileSys::ContentRecordType::Data}};
    REQUIRE_FALSE(FileSys::PatchManager::AppendEmbeddedPatches(patches, installed, {}));
    REQUIRE(installed.size() == 1);
    const FileSys::Patch addon{.enabled = true,
                               .name = "DLC",
                               .version = "1",
                               .type = FileSys::PatchType::DLC,
                               .program_id = TitleId,
                               .title_id = TitleId + 0x1001,
                               .source = FileSys::PatchSource::Embedded};
    REQUIRE(FileSys::PatchManager::AppendEmbeddedPatches(patches, installed, {addon}));
    REQUIRE(installed.size() == 1);
    REQUIRE(installed[0].title_id == addon.title_id);
    const FileSys::Patch update{.enabled = true,
                                .name = "Update",
                                .version = "1.0.0",
                                .type = FileSys::PatchType::Update,
                                .program_id = TitleId,
                                .title_id = TitleId,
                                .source = FileSys::PatchSource::Embedded,
                                .numeric_version = 65536};
    patches.push_back(update);
    REQUIRE_FALSE(FileSys::PatchManager::AppendEmbeddedPatches(patches, installed, {update}));
    REQUIRE(patches.size() == 1);
    REQUIRE(patches[0].source == FileSys::PatchSource::Embedded);
}

TEST_CASE("NUP extracts every package and repacks to the same archive", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    // The repacked archive records its folder name, so extract into one of the same name.
    const auto output = t.path / "out" / "Game";
    REQUIRE(FileSys::Nup::DefaultExtractPath(archive) == root);
    FileSys::Nup::PackProgress last{};
    std::string error;
    REQUIRE(FileSys::Nup::ExtractNup(archive, output,
                                     [&](const FileSys::Nup::PackProgress& p) { last = p; },
                                     nullptr, &error) == FileSys::Nup::PackResult::Success);
    INFO(error);
    REQUIRE(last.files_done == last.files_total);
    REQUIRE(last.bytes_done == last.bytes_total);
    REQUIRE(last.files_total > 0);

    auto expected_base = SourceTree(root / "Base");
    std::map<std::string, std::string> lower_base;
    for (auto& [path, data] : expected_base) {
        auto lowered = path;
        lowered.replace(0, 5, path.starts_with("ExeFS") ? "exefs" : "romfs");
        lower_base[lowered] = data;
    }
    REQUIRE(SourceTree(output / "base") == lower_base);
    REQUIRE(SourceTree(output / "updates/65536") == SourceTree(root / "updates/65536"));
    REQUIRE(SourceTree(output / "updates/131072") == SourceTree(root / "updates/131072"));
    REQUIRE(SourceTree(output / "dlc/0400ABCD00011001/0") ==
            SourceTree(root / "dlc/0400ABCD00011001/0"));
    REQUIRE(fs::file_size(output / "base/romfs/empty") == 0);

    const auto repacked = t.path / "out" / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(output, repacked) == FileSys::Nup::PackResult::Success);
    REQUIRE(ReadAll(repacked) == ReadAll(archive));
}

TEST_CASE("NUP extracts a multi-program archive into program folders and repacks it", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root); // A legacy base and updates must not override the edition program.
    constexpr u64 legacy_id = 0x0100ABCD00010000ULL;
    constexpr u64 native_id = TitleId + 1;
    const auto programs = root / "upgrade/65536/program";
    Put(programs / "0/exefs/main", "legacy program 0");
    Put(programs / "0/exefs/main.npdm", Npdm(legacy_id));
    Put(programs / "0/romfs/legacy.bin", "legacy data");
    Put(programs / "1/exefs/main", "native program 1");
    Put(programs / "1/exefs/main.npdm", NativeNpdm(native_id));
    Put(programs / "1/romfs/Empty", "");
    fs::rename(root / "dlc/0400ABCD00011001", root / "dlc/0100ABCD00011001");

    FileSys::NACP legacy;
    const std::string legacy_name = "Legacy game";
    std::copy(legacy_name.begin(), legacy_name.end(),
              legacy.raw.language_entries.language_entries[0].application_name.begin());
    const auto legacy_bytes = legacy.GetRawBytes();
    Put(programs / "1/control/control.nacp",
        std::string_view{reinterpret_cast<const char*>(legacy_bytes.data()), legacy_bytes.size()});
    FileSys::NACP edition;
    const std::string name = "Native edition";
    std::copy(name.begin(), name.end(),
              edition.raw.language_entries.language_entries[0].application_name.begin());
    const std::string version = "2.3.4";
    std::copy(version.begin(), version.end(), edition.raw.version_string.begin());
    edition.raw.save_data_owner_id = legacy_id;
    edition.raw.dlc_base_title_id = legacy_id + 0x1000;
    edition.raw.program_index = 1;
    edition.raw.initial_program_index = 1;
    edition.raw.acd_index = 1;
    edition.raw.apparent_platform = FileSys::ApparentPlatform::Ounce;
    const auto edition_bytes = edition.GetRawBytes();
    Put(programs / "1/control/Acd_1/control.nacp",
        std::string_view{reinterpret_cast<const char*>(edition_bytes.data()), edition_bytes.size()});

    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    // The repacked archive records its folder name, so extract into one of the same name.
    const auto output = t.path / "out" / "Game";
    std::string error;
    const auto extracted = FileSys::Nup::ExtractNup(archive, output, {}, nullptr, &error);
    INFO(error);
    REQUIRE(extracted == FileSys::Nup::PackResult::Success);

    // Each program lands under its index; there is no single "base".
    REQUIRE_FALSE(fs::exists(output / "base"));
    REQUIRE(ReadAll(output / "program/1/exefs/main") == "native program 1");
    REQUIRE(ReadAll(output / "program/0/exefs/main") == "legacy program 0");
    REQUIRE(ReadAll(output / "program/0/romfs/legacy.bin") == "legacy data");
    REQUIRE(SourceTree(output / "dlc/0100ABCD00011001/0") ==
            SourceTree(root / "dlc/0100ABCD00011001/0"));

    // The extracted folder packs to the same packages again.
    const auto repacked = t.path / "out" / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(output, repacked) == FileSys::Nup::PackResult::Success);
    FileSys::RealVfsFilesystem real;
    NupContainer first{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    NupContainer second{real.OpenFile(repacked.string(), FileSys::OpenMode::Read)};
    REQUIRE(second.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(second.GetPackages().size() == first.GetPackages().size());
    for (size_t i = 0; i < first.GetPackages().size(); ++i) {
        INFO("package " << i);
        REQUIRE(second.GetPackages()[i].kind == first.GetPackages()[i].kind);
        REQUIRE(second.GetPackages()[i].title_id == first.GetPackages()[i].title_id);
        REQUIRE(second.GetPackages()[i].version == first.GetPackages()[i].version);
        REQUIRE(PackedTree(second.GetExeFSDir(i)) == PackedTree(first.GetExeFSDir(i)));
        REQUIRE(PackedTree(second.GetRomFSDir(i)) == PackedTree(first.GetRomFSDir(i)));
    }
    REQUIRE(ReadAll(repacked) == ReadAll(archive));
}

TEST_CASE("NUP round trip preserves an empty metadata record", "[nup][nup_empty_metadata]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const std::string record{"\xFE\x7F\x00\x00\x00\x00\x00\x00", 8};
    Put(root / "Base/.nupmeta", record);

    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    const auto output = t.path / "extracted" / "Game";
    REQUIRE(FileSys::Nup::ExtractNup(archive, output) == FileSys::Nup::PackResult::Success);
    CHECK(ReadAll(output / "base/.nupmeta").find(record) != std::string::npos);

    const auto repacked = t.path / "repacked.nup";
    REQUIRE(FileSys::Nup::PackDump(output, repacked) == FileSys::Nup::PackResult::Success);
    CHECK(ReadAll(repacked) == ReadAll(archive));
}

TEST_CASE("NUP repacking an extracted archive keeps its metadata", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    FileSys::Nup::PackOptions options;
    options.metadata.title_name = "Renamed";
    options.metadata.icon = {0xFF, 0xD8, 0xFF, 0xD9};
    options.metadata.display_version = "9.9.9";
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);
    const auto output = t.path / "out" / "Game";
    REQUIRE(FileSys::Nup::ExtractNup(archive, output) == FileSys::Nup::PackResult::Success);
    REQUIRE(fs::is_regular_file(output / "base/.nupmeta"));
    // A base of version 0 needs no version record.
    REQUIRE(ReadAll(output / "base/.nupmeta").find(std::string("\x00\xFF\x00\x00\x04", 5)) ==
            std::string::npos);

    // The name, icon and version are not files of the dump; they come back from .nupmeta.
    const auto repacked = t.path / "out" / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(output, repacked) == FileSys::Nup::PackResult::Success);
    REQUIRE(ReadAll(repacked) == ReadAll(archive));

    // An explicit override still wins, and what the dump supplies itself is not replaced.
    Put(output / "base/exefs/main.npdm", Npdm(TitleId) + "changed");
    FileSys::Nup::PackOptions again;
    again.metadata.title_name = "Again";
    const auto third = t.path / "Third.nup";
    REQUIRE(FileSys::Nup::PackDump(output, third, again) == FileSys::Nup::PackResult::Success);
    const auto metadata = FileSys::Nup::ReadNupMetadata(third);
    REQUIRE(metadata);
    REQUIRE(metadata->title_name == "Again");
    REQUIRE(metadata->icon == options.metadata.icon);
    REQUIRE(metadata->display_version == "9.9.9");
    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(third.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    const auto& npdm = nup.GetPackages()[*nup.FindBasePackage()].npdm;
    REQUIRE(std::string(npdm.begin(), npdm.end()) == Npdm(TitleId) + "changed");
    for (std::size_t i = 0; i < nup.GetPackages().size(); ++i) {
        REQUIRE(nup.GetPackages()[i].source_dump == "Game");
    }
    REQUIRE(ReadAll(third).find(".nupmeta") == std::string::npos);

    // The pack dialog shows the stored values, not only what the dump's own files give.
    const auto probed = FileSys::Nup::ProbeMetadata(output);
    REQUIRE(probed.title_name == "Renamed");
    REQUIRE(probed.icon == options.metadata.icon);
    REQUIRE(probed.display_version == "9.9.9");

    // A .nupmeta whose record runs past the end of the file is refused when packing; probing
    // falls back to the dump's own files.
    Put(output / "base/.nupmeta", std::string("\x05\x00\x00\x00\x09\x00\x00\x00short", 13));
    REQUIRE(FileSys::Nup::PackDump(output, t.path / "Bad.nup") ==
            FileSys::Nup::PackResult::InvalidSource);
    REQUIRE(FileSys::Nup::ProbeMetadata(output).title_name != "Renamed");
}

TEST_CASE("NUP repacking keeps a base package's version", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    // Archives of other packers give the base a version (here 1.0.0); .nupmeta carries it.
    const std::string version_record("\x00\xFF\x00\x00\x04\x00\x00\x00\x04\x03\x02\x01", 12);
    // Three-byte records; types 3, 4 and 5 are the icon, display version and title name.
    const auto record = [](char type, const std::string& text) {
        return type + std::string("\x00\x00\x00\x03\x00\x00\x00", 7) + text;
    };
    Put(root / "Base/.nupmeta",
        record(3, "ico") + record(4, "1.0") + record(5, "Abc") + version_record);
    Put(root / "updates/131072/.nupmeta", record(4, "2.0") + version_record);
    // The newest update's stored display version is shown, not the base's.
    REQUIRE(FileSys::Nup::ProbeMetadata(root).display_version == "2.0");
    REQUIRE(FileSys::Nup::ProbeMetadata(root).version_source == "update 131072");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    REQUIRE(ReadAll(archive).find(version_record) == std::string::npos);

    const auto output = t.path / "out" / "Game";
    REQUIRE(FileSys::Nup::ExtractNup(archive, output) == FileSys::Nup::PackResult::Success);
    REQUIRE(ReadAll(output / "base/.nupmeta").ends_with(version_record));
    REQUIRE(ReadAll(output / "updates/131072/.nupmeta").find(version_record.substr(0, 8)) ==
            std::string::npos);
    const auto repacked = t.path / "out" / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(output, repacked) == FileSys::Nup::PackResult::Success);
    REQUIRE(ReadAll(repacked) == ReadAll(archive));

    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(repacked.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    // Only a base takes its version from .nupmeta; an update's is its folder name.
    std::vector<u32> versions;
    for (const auto& package : nup.GetPackages()) {
        versions.push_back(package.version);
    }
    REQUIRE(versions == std::vector<u32>{0x01020304, 65536, 131072, 0});

    // Low-storage packing leaves no .nupmeta behind, so the emptied package folders go away.
    REQUIRE(FileSys::Nup::PackDump(output, t.path / "Low.nup", {.delete_sources = true}) ==
            FileSys::Nup::PackResult::Success);
    REQUIRE_FALSE(fs::exists(output / "base"));
    REQUIRE_FALSE(fs::exists(output / "updates"));
}

TEST_CASE("NUP keeps an empty RomFS folder of a program whose data is in its add-ons", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "base/exefs/main", "edition main");
    Put(root / "base/exefs/main.npdm", Npdm(TitleId));
    fs::create_directories(root / "base/romfs");
    Put(root / "dlc/0400ABCD00011001/0/romfs/data.bin", "game data");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    FileSys::RealVfsFilesystem real;
    {
        NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(nup.GetRomFSDir(*nup.FindBasePackage()) != nullptr);
    }
    Loader::AppLoader_NUP loader{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    FileSys::VirtualFile romfs;
    REQUIRE(loader.ReadRomFS(romfs) == Loader::ResultStatus::Success);
    const auto output = t.path / "out" / "Game";
    REQUIRE(FileSys::Nup::ExtractNup(archive, output) == FileSys::Nup::PackResult::Success);
    REQUIRE(fs::is_directory(output / "base/romfs"));
    const auto repacked = t.path / "out" / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(output, repacked) == FileSys::Nup::PackResult::Success);
    REQUIRE(ReadAll(repacked) == ReadAll(archive));
}

TEST_CASE("NUP extraction refuses a used folder and cleans up after damage", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    const auto used = t.path / "used";
    Put(used / "keep.txt", "mine");
    REQUIRE(FileSys::Nup::ExtractNup(archive, used) == FileSys::Nup::PackResult::InvalidSource);
    REQUIRE(ReadAll(used / "keep.txt") == "mine");
    REQUIRE(std::distance(fs::directory_iterator(used), fs::directory_iterator{}) == 1);

    // Damage one stored file; the TOC hash only covers the TOC, so the archive still opens.
    auto bytes = ReadAll(archive);
    const auto files = RawFiles(bytes);
    const auto random =
        std::ranges::find(files, std::string{"random.bin"}, &RawFile::name);
    REQUIRE(random != files.end());
    bytes[random->extent_offset + 10] ^= 0x55;
    const auto damaged = t.path / "damaged.nup";
    Put(damaged, bytes);

    const auto output = t.path / "damaged";
    std::string error;
    REQUIRE(FileSys::Nup::ExtractNup(damaged, output, {}, nullptr, &error) ==
            FileSys::Nup::PackResult::InvalidSource);
    REQUIRE(error.find("random.bin") != std::string::npos);
    REQUIRE_FALSE(fs::exists(output));

    // An empty folder that existed before stays, emptied again.
    fs::create_directories(output);
    REQUIRE(FileSys::Nup::ExtractNup(damaged, output) == FileSys::Nup::PackResult::InvalidSource);
    REQUIRE(fs::is_directory(output));
    REQUIRE(fs::is_empty(output));

    std::atomic_bool cancel{true};
    REQUIRE(FileSys::Nup::ExtractNup(archive, t.path / "cancelled", {}, &cancel) ==
            FileSys::Nup::PackResult::Cancelled);
    REQUIRE_FALSE(fs::exists(t.path / "cancelled"));
}

TEST_CASE("NUP extraction rejects names that leave the target folder", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    for (const std::string_view bad : {"..", ".", "a/b", "a\\b"}) {
        auto bytes = ReadAll(archive);
        const u64 toc = Le(bytes, 0x08, 8);
        const u64 packages = Le(bytes, toc + 0x04, 4);
        const u64 dirs = Le(bytes, toc + 0x08, 8);
        const u64 file_count = Le(bytes, toc + 0x10, 8);
        const u64 pool = toc + Le(bytes, toc + 0x18, 8);
        u64 pos = toc + 0x30 + packages * 0x40 + dirs * 0x18;
        bool patched = false;
        for (u64 i = 0; i < file_count; ++i, pos += 0x50) {
            if (bytes.substr(pool + Le(bytes, pos, 4), Le(bytes, pos + 4, 4)) == "alpha") {
                // Point the name at fresh bytes: "alpha" is shared by no other node.
                bytes.replace(pool + Le(bytes, pos, 4), bad.size(), bad);
                SetLe(bytes, pos + 4, bad.size(), 4);
                patched = true;
            }
        }
        REQUIRE(patched);
        const auto hash = Sha256(std::string_view{bytes}.substr(toc, Le(bytes, 0x10, 8)));
        std::copy(hash.begin(), hash.end(), bytes.begin() + 0x18);
        const auto evil = t.path / "evil.nup";
        Put(evil, bytes);

        const auto output = t.path / "nest" / "out";
        std::string error;
        INFO(bad);
        REQUIRE(FileSys::Nup::ExtractNup(evil, output, {}, nullptr, &error) ==
                FileSys::Nup::PackResult::InvalidSource);
        REQUIRE(error.find("unsafe name") != std::string::npos);
        REQUIRE_FALSE(fs::exists(output));
    }
}

TEST_CASE("NUP metadata edits match packing with the same overrides", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    FileSys::Nup::Metadata metadata;
    metadata.title_name = "Edited Name";
    metadata.icon = {0xFF, 0xD8, 0x01, 0x02, 0xFF, 0xD9};
    metadata.display_version = "4.5.6";

    const auto expected = t.path / "expected" / "Game.nup";
    fs::create_directories(expected.parent_path());
    REQUIRE(FileSys::Nup::PackDump(root, expected, {.metadata = metadata}) ==
            FileSys::Nup::PackResult::Success);

    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    const auto original = ReadAll(archive);

    // Nothing to change leaves the file alone.
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, {}) == FileSys::Nup::PackResult::Success);
    REQUIRE(ReadAll(archive) == original);

    std::string error;
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, metadata, {}, nullptr, &error) ==
            FileSys::Nup::PackResult::Success);
    INFO(error);
    REQUIRE_FALSE(fs::exists(archive.string() + ".part"));
    REQUIRE(ReadAll(archive) == ReadAll(expected));

    const auto read = FileSys::Nup::ReadNupMetadata(archive);
    REQUIRE(read);
    REQUIRE(read->title_name == "Edited Name");
    REQUIRE(read->icon == metadata.icon);
    REQUIRE(read->display_version == "4.5.6");

    // A second edit replaces the metadata again instead of piling it up.
    FileSys::Nup::Metadata rename;
    rename.title_name = "Other";
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, rename) == FileSys::Nup::PackResult::Success);
    REQUIRE(fs::file_size(archive) == fs::file_size(expected) - 6);
    REQUIRE(FileSys::Nup::ReadNupMetadata(archive)->title_name == "Other");
    FileSys::RealVfsFilesystem real;
    NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    bool visited = false;
    REQUIRE_FALSE(nup.ForEachMember(nup.GetPackages().size(),
                                    [&](const auto&, const auto&, const auto&, const auto&) {
                                        visited = true;
                                        return true;
                                    }));
    REQUIRE_FALSE(visited);
    // A visitor that stops early ends the walk with false.
    REQUIRE_FALSE(nup.ForEachMember(0, [](const auto&, const auto&, const auto&, const auto&) {
        return false;
    }));
    REQUIRE(nup.ForEachMember(0, [](const auto&, const auto&, const auto&, const auto&) {
        return true;
    }));

    REQUIRE_FALSE(FileSys::Nup::ReadNupMetadata(root / "Base/ExeFS/main"));
    std::atomic_bool cancel{true};
    // A reflinked copy has nothing left to cancel, so either outcome must leave a valid file.
    const auto result = FileSys::Nup::EditNupMetadata(archive, metadata, {}, &cancel);
    REQUIRE((result == FileSys::Nup::PackResult::Cancelled ||
             result == FileSys::Nup::PackResult::Success));
    REQUIRE_FALSE(fs::exists(archive.string() + ".part"));
    REQUIRE(FileSys::Nup::ReadNupMetadata(archive));
}

TEST_CASE("NUP updates and DLC can be switched off in the Add-Ons tab", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    FileSys::RealVfsFilesystem real;
    auto& disabled = Settings::values.disabled_addons[TitleId];
    const auto saved = disabled;
    struct Restore {
        std::vector<std::string>& target;
        std::vector<std::string> value;
        ~Restore() {
            target = value;
        }
    } restore{disabled, saved};

    // Booted update, the update row the loader reports as enabled, and its display version.
    const auto state = [&] {
        NupContainer nup{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
        const auto update = nup.FindEnabledUpdatePackage();
        Loader::AppLoader_NUP loader{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
        u32 enabled_update = 0;
        int update_rows = 0;
        bool dlc_enabled = false;
        for (const auto& patch : loader.ReadEmbeddedPatches()) {
            if (patch.type == FileSys::PatchType::Update) {
                ++update_rows;
                if (patch.enabled) {
                    REQUIRE(enabled_update == 0);
                    enabled_update = patch.numeric_version;
                }
            } else if (patch.type == FileSys::PatchType::DLC) {
                dlc_enabled = patch.enabled;
            }
        }
        REQUIRE(update_rows == 2);
        const u32 booted = update ? nup.GetPackages()[*update].version : 0;
        REQUIRE(booted == enabled_update);
        return std::pair{booted, dlc_enabled};
    };

    disabled.clear();
    REQUIRE(state() == std::pair{131072u, true});
    disabled = {"Update@131072"};
    REQUIRE(state() == std::pair{65536u, true});
    disabled = {"Update@131072", "Update@65536", "DLC"};
    REQUIRE(state() == std::pair{0u, false});

    // The merged DLC row follows the setting even when all DLC is embedded.
    Core::System system;
    FileSys::ContentProviderUnion provider{};
    FileSys::PatchManager pm{TitleId, system.GetFileSystemController(), provider};
    Loader::AppLoader_NUP loader{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    const auto patches = pm.GetPatches(nullptr, loader.ReadEmbeddedPatches());
    const auto dlc = std::find_if(patches.begin(), patches.end(), [](const auto& patch) {
        return patch.type == FileSys::PatchType::DLC;
    });
    REQUIRE(dlc != patches.end());
    REQUIRE_FALSE(dlc->enabled);
}

TEST_CASE("NUP reports Switch 2 Editions", "[nup]") {
    TempDir t;
    FileSys::RealVfsFilesystem real;
    const auto editions = [&](const fs::path& archive) {
        Loader::AppLoader_NUP loader{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};
        std::vector<std::string> out;
        for (const auto& patch : loader.ReadEmbeddedPatches()) {
            if (patch.name == "Switch 2 Edition") {
                out.push_back(patch.type == FileSys::PatchType::Edition ? "info" : "update");
            }
        }
        return out;
    };

    // Kirby and Wonder: only the edition program (index 1) is in the archive.
    const auto program = t.path / "Program";
    Put(program / "ExeFS/main", "main");
    Put(program / "ExeFS/main.npdm", Npdm(TitleId + 1));
    Put(program / "RomFS/file", "rom");
    REQUIRE(FileSys::Nup::PackDump(program, t.path / "program.nup") ==
            FileSys::Nup::PackResult::Success);
    REQUIRE(editions(t.path / "program.nup") == std::vector<std::string>{"info"});

    // Super Mario Galaxy 2: the Switch 2 build is the update of a Switch 1 base.
    constexpr u64 NxTitleId = 0x0100ABCD00010000ULL;
    const auto split = t.path / "Split";
    Put(split / "Base/ExeFS/main", "nx main");
    Put(split / "Base/ExeFS/main.npdm", Npdm(NxTitleId));
    Put(split / "Base/RomFS/file", "rom");
    Put(split / "updates/65536/exefs/main", "ounce main");
    Put(split / "updates/65536/exefs/main.npdm", Npdm(TitleId));
    REQUIRE(FileSys::Nup::PackDump(split, t.path / "split.nup") ==
            FileSys::Nup::PackResult::Success);
    REQUIRE(editions(t.path / "split.nup") == std::vector<std::string>{"update"});

    // A plain Switch 2 game is neither.
    const auto plain = t.path / "Plain";
    MakeDump(plain);
    REQUIRE(FileSys::Nup::PackDump(plain, t.path / "plain.nup") ==
            FileSys::Nup::PackResult::Success);
    REQUIRE(editions(t.path / "plain.nup").empty());
}

namespace {
// Reports a fixed file type and RomFS result, for the RomFS dump source selection.
class FixedRomFSLoader final : public Loader::AppLoader {
public:
    FixedRomFSLoader(Loader::FileType type_, Loader::ResultStatus status_)
        : AppLoader{nullptr}, type{type_}, status{status_} {}

    Loader::FileType GetFileType() const override {
        return type;
    }
    LoadResult Load(Kernel::KProcess&, Core::System&) override {
        return {Loader::ResultStatus::ErrorNotImplemented, {}};
    }
    Loader::ResultStatus ReadRomFS(FileSys::VirtualFile& out_file) override {
        out_file = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>{1}, "romfs");
        return status;
    }

private:
    Loader::FileType type;
    Loader::ResultStatus status;
};
} // namespace

TEST_CASE("NUP RomFS dumps come from the loader with the enabled update merged", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "Base/ExeFS/main", "base main");
    Put(root / "Base/ExeFS/main.npdm", Npdm(TitleId));
    Put(root / "Base/RomFS/base-only", "base");
    Put(root / "Base/RomFS/data/shared", "old");
    Put(root / "updates/65536/exefs/main", "update main");
    Put(root / "updates/65536/exefs/main.npdm", Npdm(TitleId));
    Put(root / "updates/65536/romfs/data/shared", "new");
    Put(root / "updates/65536/romfs/update-only", "update");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    FileSys::RealVfsFilesystem real;
    Loader::AppLoader_NUP loader{real.OpenFile(archive.string(), FileSys::OpenMode::Read)};

    const auto romfs = Loader::ReadUninstalledRomFS(loader);
    REQUIRE(romfs);
    const auto image = FileSys::ExtractRomFS(*romfs);
    REQUIRE(image);
    CHECK(PackedTree(image) == std::map<std::string, std::string>{{"base-only", "base"},
                                                                  {"data/shared", "new"},
                                                                  {"update-only", "update"}});
}

TEST_CASE("RomFS dumps of other files keep the installed content", "[nup]") {
    for (const auto type : {Loader::FileType::NSP, Loader::FileType::XCI, Loader::FileType::NCA,
                            Loader::FileType::NX}) {
        FixedRomFSLoader loader{type, Loader::ResultStatus::Success};
        CHECK_FALSE(Loader::ReadUninstalledRomFS(loader).has_value());
    }
    FixedRomFSLoader broken{Loader::FileType::NUP, Loader::ResultStatus::ErrorNoRomFS};
    const auto romfs = Loader::ReadUninstalledRomFS(broken);
    REQUIRE(romfs.has_value());
    CHECK(*romfs == nullptr);
}

TEST_CASE("NUP boots the program that matches its base", "[nup][boot-inputs]") {
    TempDir t;
    const auto root = t.path / "Game";
    constexpr u64 edition_program = TitleId + 1;
    const u64 legacy_program = FileSys::WithPlatformOf(TitleId, 0x0100000000000000ULL);
    bool base_switch2 = true;
    bool update_switch2 = false;
    bool base_exefs = true;

    SECTION("a legacy update does not replace the native base program") {}
    SECTION("a native update supersedes a legacy base") {
        base_switch2 = false;
        update_switch2 = true;
    }
    SECTION("a native update boots over a base packed without an executable") {
        base_exefs = false;
        update_switch2 = true;
    }

    const auto write_exefs = [](const fs::path& path, u64 program_id, bool switch2) {
        Put(path / "main", "main");
        Put(path / "main.npdm", BootNpdm(program_id, switch2));
    };
    if (base_exefs) {
        write_exefs(root / "base/exefs", base_switch2 ? edition_program : legacy_program,
                    base_switch2);
    } else {
        fs::create_directories(root / "base/exefs");
    }
    Put(root / "base/romfs/base.bin", "base");
    write_exefs(root / "updates/65536/exefs", update_switch2 ? edition_program : legacy_program,
                update_switch2);
    Put(root / "updates/65536/romfs/update.bin", "update");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem vfs;
    Loader::AppLoader_NUP loader{vfs.OpenFile(archive.string(), FileSys::OpenMode::Read)};
    FileSys::ProgramMetadata metadata;
    REQUIRE(loader.ReadProgramMetadata(metadata) == Loader::ResultStatus::Success);
    CHECK(metadata.IsSwitch2());
    CHECK(metadata.GetTitleID() == edition_program);
    u64 program_id{};
    REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
    CHECK(program_id == edition_program);
}

TEST_CASE("PackDump preserves control metadata owned by an update", "[nup][nup-update-control]") {
    TempDir tmp;
    const auto root = tmp.path / "Game";
    const auto update = root / "updates/65536";
    Put(root / "base/exefs/main", "base main");
    Put(root / "base/exefs/main.npdm", Npdm(TitleId));
    Put(root / "base/romfs/shared.bin", "base");
    Put(update / "romfs/shared.bin", "update");

    FileSys::NACP source;
    source.raw.save_data_owner_id = TitleId;
    source.raw.supported_languages = FileSys::SupportedLanguage::AmericanEnglish;
    const auto base = source.GetRawBytes();
    Put(root / "base/control.nacp", {reinterpret_cast<const char*>(base.data()), base.size()});

    source.raw.save_data_owner_id = TitleId + 1;
    source.raw.supported_languages = FileSys::SupportedLanguage::Japanese;
    const auto patched = source.GetRawBytes();
    const std::string_view patched_bytes{reinterpret_cast<const char*>(patched.data()),
                                         patched.size()};
    const std::vector<u8>* expected = &patched;
    SECTION("Executable update has its own control") {
        Put(update / "exefs/main", "update main");
        Put(update / "exefs/main.npdm", Npdm(TitleId));
        Put(update / "exefs/control.nacp", patched_bytes);
    }
    SECTION("RomFS-only update has its own control") {
        Put(update / "control.nacp", patched_bytes);
    }
    SECTION("An ancestor's control does not belong to the update") {
        Put(update / "exefs/main", "update main");
        Put(update / "exefs/main.npdm", Npdm(TitleId));
        Put(root / "updates/control.nacp", patched_bytes);
        expected = &base;
    }

    const auto archive = tmp.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    FileSys::RealVfsFilesystem vfs;
    const auto file = vfs.OpenFile(archive.string(), FileSys::OpenMode::Read);
    Loader::AppLoader_NUP loader{file};
    FileSys::NACP control;
    REQUIRE(loader.ReadControlData(control) == Loader::ResultStatus::Success);
    CHECK(control.GetRawBytes() == *expected);

    NupContainer parsed{file};
    REQUIRE(parsed.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(parsed.GetPackages().size() == 2);
    CHECK(parsed.GetPackages()[0].nacp == base);
    if (expected == &base) {
        CHECK(parsed.GetPackages()[1].nacp.empty());
    } else {
        CHECK(parsed.GetPackages()[1].nacp == patched);
    }
}

TEST_CASE("Home Menu selects native NUP programs and preserves registered content",
          "[home-menu][nup]") {
    TempDir t;
    constexpr u64 legacy_id = 0x0100ABCD00010000ULL;
    constexpr u64 edition_id = TitleId + 1;
    constexpr u64 system_id = 0x0400000000001000ULL;
    const auto root = t.path / "Game";
    const auto archive = t.path / "Game.nup";
    bool native = true;
    SECTION("Switch 2 edition") {
        Put(root / "exefs/main.npdm", BootNpdm(edition_id, true));
    }
    SECTION("Switch 1 dump") {
        native = false;
        Put(root / "exefs/main.npdm", BootNpdm(legacy_id, false));
    }
    Put(root / "exefs/main", "main");
    Put(root / "romfs/file", "romfs");
    FileSys::NACP control;
    control.raw.save_data_owner_id = legacy_id;
    const auto bytes = control.GetRawBytes();
    Put(root / "control/Acd_1/control.nacp",
        std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    Put(root / "control/Acd_1/icon_AmericanEnglish.dat", "native icon");
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    Core::System system;
    FileSys::ManualContentProvider manual;
    FileSys::ManualContentProvider installed;
    FileSys::ManualContentProvider sd;
    auto& content = system.GetContentProviderUnion();
    content.SetSlot(FileSys::ContentProviderUnionSlot::FrontendManual, &manual);
    content.SetSlot(FileSys::ContentProviderUnionSlot::UserNAND, &installed);
    content.SetSlot(FileSys::ContentProviderUnionSlot::SDMC, &sd);
    const auto registered = std::make_shared<FileSys::VectorVfsFile>();
    installed.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Program,
                       legacy_id, registered);
    installed.AddEntry(FileSys::TitleType::SystemProgram, FileSys::ContentRecordType::Program,
                       system_id, registered);
    manual.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Program,
                    system_id, registered);
    manual.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Program,
                    0x0100000000001000ULL, registered);
    CHECK(manual.HasEntry(system_id, FileSys::ContentRecordType::Program));
    CHECK(manual.HasEntry(0x0100000000001000ULL, FileSys::ContentRecordType::Program));
    installed.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Program,
                       legacy_id + 1, registered);
    installed.AddEntry(FileSys::TitleType::Update, FileSys::ContentRecordType::Program,
                       FileSys::GetUpdateTitleID(legacy_id), registered);
    installed.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Control,
                       TitleId, registered);
    sd.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Program,
                legacy_id, registered);

    FileSys::RealVfsFilesystem vfs;
    const auto file = vfs.OpenFile(archive.string(), FileSys::OpenMode::Read);
    REQUIRE(file);
    const auto loader = Loader::GetLoader(system, file);
    REQUIRE(loader);
    manual.RegisterApplicationFile(*loader, file);
    CHECK(content.ListApplicationIds() ==
          (native ? std::vector<u64>{legacy_id, edition_id} : std::vector<u64>{legacy_id}));
    CHECK(content.GetApplicationProgram(legacy_id).slot == FileSys::ContentProviderUnionSlot::UserNAND);
    CHECK(content.GetApplicationProgram(legacy_id).file == registered);
    CHECK_FALSE(content.HasEntry(edition_id, FileSys::ContentRecordType::Program));

    const auto selection = content.GetApplicationProgram(edition_id);
    CHECK(selection.loader_backed == native);
    if (!native) {
        CHECK_FALSE(selection.file);
        return;
    }

    CHECK(selection.slot == FileSys::ContentProviderUnionSlot::FrontendManual);
    REQUIRE(selection.file == file);
    auto selected = Loader::GetLoader(system, selection.file, edition_id);
    REQUIRE(selected);
    CHECK(selected->GetFileType() == Loader::FileType::NUP);
    FileSys::ProgramMetadata metadata;
    REQUIRE(selected->ReadProgramMetadata(metadata) == Loader::ResultStatus::Success);
    CHECK(metadata.IsSwitch2());
    CHECK(metadata.GetTitleID() == edition_id);
    const auto menu_control = FileSys::PatchManager::GetApplicationControlMetadata(system, edition_id);
    REQUIRE(menu_control.first);
    CHECK(menu_control.first->GetTitleId() == legacy_id);
    REQUIRE(menu_control.second);
    CHECK(Read(menu_control.second) == "native icon");

    sd.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Program,
                edition_id, registered);
    const auto installed_selection = content.GetApplicationProgram(edition_id);
    CHECK_FALSE(installed_selection.loader_backed);
    CHECK(installed_selection.slot == FileSys::ContentProviderUnionSlot::SDMC);
    CHECK(installed_selection.file == registered);

    FileSys::ManualContentProvider previous;
    manual.SwapEntries(previous);
    CHECK_FALSE(manual.GetApplicationFile(edition_id));
    manual.SwapEntries(previous);
    CHECK(manual.GetApplicationFile(edition_id) == file);
    manual.ClearAllEntries();
    CHECK(content.ListApplicationIds() == std::vector<u64>{legacy_id});
    sd.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Meta,
                edition_id, registered);
    CHECK((content.ListApplicationIds() == std::vector<u64>{legacy_id, edition_id}));
}

TEST_CASE("Home Menu uses every byte of each icon request title ID", "[home-menu]") {
    constexpr std::array<u64, 2> ids{0x0400D4201C6EA000ULL, 0x01007EF00011E000ULL};
    const auto bytes = std::span{reinterpret_cast<const u8*>(ids.data()), sizeof(ids)};
    CHECK((Service::NS::DecodeApplicationIds(bytes) == std::vector<u64>{ids[0], ids[1]}));
}

TEST_CASE("Home Menu launch ownership survives child and same-title relaunch", "[home-menu][nup]") {
    using namespace Service::AM;
    ScopedSystemTestContext environment;
    TempDir t;
    using Common::FS::EdenPath;
    const auto cache_dir = t.path / "cache";
    const auto missing_cache = t.path / "missing-cache";
    REQUIRE(fs::create_directory(cache_dir));
    const auto previous_cache =
        Common::FS::ExchangeEdenPathMapping(EdenPath::CacheDir, missing_cache);
    SCOPE_EXIT { Common::FS::ExchangeEdenPathMapping(EdenPath::CacheDir, previous_cache); };
    {
        const auto missing_mapping =
            Common::FS::ExchangeEdenPathMapping(EdenPath::CacheDir, cache_dir);
        SCOPE_EXIT { Common::FS::ExchangeEdenPathMapping(EdenPath::CacheDir, missing_mapping); };
        CHECK(Common::FS::GetEdenPath(EdenPath::CacheDir) == cache_dir);
    }
    CHECK(Common::FS::GetEdenPath(EdenPath::CacheDir) == missing_cache);
    CHECK_FALSE(fs::exists(missing_cache));
    Common::FS::SetEdenPath(EdenPath::CacheDir, cache_dir);
    CHECK(Core::LaunchTimestampCache::GetLaunchCount(TitleId) == 0);

    const bool previous_switch2 = Settings::CurrentProgramIsSwitch2();
    const u64 previous_counter = Common::g_wall_clock.GetGuestCNTFRQ();
    Settings::SetCurrentProgramIsSwitch2(true);
    Common::g_wall_clock.SetGuestCNTFRQ(Common::WallClock::Switch2CNTFRQ);
    SCOPE_EXIT {
        Settings::SetCurrentProgramIsSwitch2(previous_switch2);
        Common::g_wall_clock.SetGuestCNTFRQ(previous_counter);
    };
    constexpr u64 qlaunch_id = 0x0400000000001000ULL;
    constexpr u64 child_id = 0x0400000000001003ULL;
    constexpr u64 legacy_id = 0x0100ABCD00018000ULL;
    constexpr u64 addon_id = TitleId + 0x1001;
    std::string nso(0x10C, '\0');
    nso.replace(0, 4, "NSO0");
    for (u32 segment = 0; segment < 3; ++segment) {
        SetLe(nso, 0x10 + segment * 0x10, 0x100 + segment * 4, 4);
        SetLe(nso, 0x14 + segment * 0x10, segment * 0x1000, 4);
        SetLe(nso, 0x18 + segment * 0x10, 4, 4);
    }
    SetLe(nso, 0x100, 0x14000000, 4);
    const auto write_program = [&](const fs::path& path, u64 id, bool native,
                                   u32 stack = 0x4000) {
        auto npdm = BootNpdm(id, native);
        SetLe(npdm, 0xC, 7, 1);
        SetLe(npdm, 0xE, 44, 1);
        SetLe(npdm, 0x1C, stack, 4);
        const auto shift = native ? 0x100 : 0;
        SetLe(npdm, 0x43C + shift, 8, 4);
        SetLe(npdm, 0x460 + shift, 0x30043F7, 4);
        SetLe(npdm, 0x464 + shift, (64U << 16) | 0x7FFF, 4);
        Put(path / "exefs/main.npdm", npdm);
        Put(path / "exefs/main", nso);
        Put(path / "romfs/file", "romfs");
    };
    const auto write_control = [](const fs::path& path, bool locked, u64 dlc,
                                  std::string_view version) {
        FileSys::NACP control;
        control.raw.user_account_switch_lock = locked;
        control.raw.dlc_base_title_id = dlc;
        std::copy(version.begin(), version.end(), control.raw.version_string.begin());
        const auto bytes = control.GetRawBytes();
        Put(path, std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    };
    const auto game = t.path / "Game";
    const auto archive = t.path / "Game.nup";
    write_program(game / "base", TitleId, true);
    write_program(game / "updates/65536", TitleId, true);
    write_control(game / "base/control/Acd_1/control.nacp", true, TitleId + 0x1000, "1.0.0");
    write_control(game / "updates/65536/control/Acd_1/control.nacp", true, TitleId + 0x1000,
                  "2.0.0");
    Put(game / "base/control/Acd_1/icon_AmericanEnglish.dat", "nup-icon");
    Put(game / "dlc/0400ABCD00011001/0/romfs/addon", "addon");
    REQUIRE(FileSys::Nup::PackDump(game, archive) == FileSys::Nup::PackResult::Success);
    write_program(t.path / "Home", qlaunch_id, true);
    write_program(t.path / "Child", child_id, true);
    write_program(t.path / "Legacy", legacy_id, false);
    constexpr u64 failed_id = TitleId + 0x4000;
    write_program(t.path / "Failed", failed_id, true, 0xFFFFF000);
    constexpr u64 loose_id = TitleId + 0x2000;
    write_program(t.path / "Loose", loose_id, true);
    write_control(t.path / "Loose/control/Acd_1/control.nacp", false, 0, "3.0.0");
    Put(t.path / "Loose/control/Acd_1/icon_AmericanEnglish.dat", "loose-icon");

    FileSys::RealVfsFilesystem vfs;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    Kernel::KThread bootstrap{kernel};
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &bootstrap, nullptr).IsSuccess());
    kernel.SetCurrentEmuThread(&bootstrap);
    kernel.Initialize();
    SCOPE_EXIT {
        system.CoreTiming().ClearPendingEvents();
        kernel.Shutdown();
        kernel.SetCurrentEmuThread(nullptr);
        kernel.UnregisterKernelObject(&bootstrap);
    };
    FileSys::ManualContentProvider manual;
    FileSys::ManualContentProvider installed;
    system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual, &manual);
    system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::UserNAND, &installed);
    const auto open = [&](const fs::path& path) {
        return vfs.OpenFile(path.string(), FileSys::OpenMode::Read);
    };
    const auto create = [&](const FileSys::VirtualFile& file, u64 id,
                            std::optional<FileSys::ContentProviderUnionSlot> slot = std::nullopt) {
        std::vector<u8> raw;
        std::unique_ptr<Loader::AppLoader> loader;
        Loader::ResultStatus result{};
        return CreateApplicationProcess(raw, loader, result, system, file, id, 0, slot);
    };

    const auto finish_guest_threads = [&](Service::Process& process) {
        std::list<Kernel::KScopedAutoObject<Kernel::KThread>> threads;
        {
            auto& owner = *process.GetHandle();
            Kernel::KScopedLightLock lock{owner.GetListLock()};
            for (auto& thread : owner.GetThreadList()) {
                if (thread.GetState() != Kernel::ThreadState::Terminated) {
                    threads.emplace_back(kernel, &thread);
                }
            }
        }
        for (auto& thread : threads) {
            thread->SetState(kernel, Kernel::ThreadState::Terminated);
            thread->DoWorkerTaskImpl(kernel);
            process.GetHandle()->DecrementRunningThreadCount(kernel);
        }
    };

    const FrontendAppletParameters home_params{.program_id = qlaunch_id, .applet_id = AppletId::QLaunch,
                                              .applet_type = AppletType::LibraryApplet};
    CHECK_FALSE(home_params.IsGameLaunch());
    const FrontendAppletParameters game_params{.applet_type = AppletType::Application};
    CHECK(game_params.IsGameLaunch());

    const auto mixed_game_path = t.path / "Loose/exefs" / archive.filename();
    REQUIRE(fs::copy_file(archive, mixed_game_path));
    const auto host_file = Core::GetGameFileFromPath(system.GetFilesystem(), mixed_game_path.string());
    REQUIRE(host_file);
    CHECK(Loader::IdentifyFile(host_file) == Loader::FileType::NUP);
    auto loader = Loader::GetLoader(system, host_file);
    REQUIRE(loader);
    manual.RegisterApplicationFile(*loader, host_file);
    const auto source = system.GetContentProviderUnion().GetApplicationProgram(TitleId);
    REQUIRE(source.file == host_file);
    CHECK(source.loader_backed);

    const auto loose = open(t.path / "Home/exefs/main");
    REQUIRE(loose);
    auto loose_loader = Loader::GetLoader(system, loose);
    REQUIRE(loose_loader);
    REQUIRE(loose_loader->GetFileType() == Loader::FileType::DeconstructedRomDirectory);
    FileSys::ProgramMetadata metadata;
    REQUIRE(loose_loader->ReadProgramMetadata(metadata) == Loader::ResultStatus::Success);
    CHECK(metadata.IsSwitch2());
    auto home_process = create(loose, qlaunch_id);
    REQUIRE(home_process);
    REQUIRE(home_process->Run());
    auto home_applet = std::make_shared<Applet>(system, std::move(home_process), false);
    const auto& home = home_applet->process;
    SCOPE_EXIT { finish_guest_threads(*home); };
    {
        WindowSystem overlay_window{system};
        auto pending_process = create(open(t.path / "Loose/exefs/main"), loose_id);
        REQUIRE(pending_process);
        auto pending_overlay_case = std::make_shared<Applet>(system, std::move(pending_process), true);
        pending_overlay_case->applet_id = AppletId::Application;
        home_applet->applet_id = AppletId::OverlayDisplay;
        kernel.SetApplicationProcess(pending_overlay_case->process->GetHandle());
        overlay_window.TrackApplet(home_applet, false);
        REQUIRE(kernel.ApplicationProcess() == pending_overlay_case->process->GetHandle());

        overlay_window.TrackApplet(pending_overlay_case, true, true);
        REQUIRE(kernel.ApplicationProcess() == pending_overlay_case->process->GetHandle());
        REQUIRE(overlay_window.CancelPendingApplication(*pending_overlay_case));
        CHECK_FALSE(pending_overlay_case->process->IsInitialized());
        CHECK(kernel.ApplicationProcess() == home_applet->process->GetHandle());
        CHECK(overlay_window.GetOverlayDisplayApplet() == home_applet);
    }
    home_applet->applet_id = AppletId::QLaunch;
    home_applet->program_id = qlaunch_id;
    WindowSystem window{system};
    window.TrackApplet(home_applet, false);

    const auto positive_loose = open(t.path / "Loose/exefs/main");
    auto positive_loader = Loader::GetLoader(system, positive_loose);
    REQUIRE(positive_loader);
    manual.RegisterApplicationFile(*positive_loader, positive_loose);
    CHECK(system.GetContentProviderUnion().GetApplicationProgram(loose_id).file == positive_loose);

    {
        auto& owner = *home->GetHandle();
        Kernel::KThread client{kernel};
        Kernel::KAutoObject::Create(&client);
        REQUIRE(Kernel::KThread::InitializeDummyThread(system, &client, &owner).IsSuccess());
        kernel.SetCurrentEmuThread(&client);
        SCOPE_EXIT {
            kernel.SetCurrentEmuThread(&bootstrap);
            owner.UnregisterThread(kernel, &client);
            owner.Close(kernel);
            kernel.UnregisterKernelObject(&client);
        };
        Kernel::KProcessAddress heap{};
        REQUIRE(owner.GetPageTable().SetHeapSize(&heap, 0x200000).IsSuccess());
        Kernel::Handle transfer{};
        REQUIRE(Kernel::Svc::CreateTransferMemory64(system, &transfer, GetInteger(heap), 0x1000,
                                                   Kernel::Svc::MemoryPermission::ReadWrite)
                    .IsSuccess());
        SCOPE_EXIT { owner.GetHandleTable().Remove(kernel, transfer); };

        std::array<u64, 3> ids{TitleId, loose_id, child_id};
        auto& memory = owner.GetMemory();
        const auto ids_addr = heap + 0x1000;
        memory.WriteBlock(ids_addr, ids.data(), sizeof(ids));

        Service::ServerManager server{system};
        auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(IPC::CommandType::Request);
        header.enable_handle_descriptor.Assign(1);
        header.num_buf_a_descriptors.Assign(1);
        header.data_size.Assign(6);
        IPC::HandleDescriptorHeader handles{};
        handles.num_handles_to_copy.Assign(1);
        IPC::BufferDescriptorABW buffer{};
        buffer.address_bits_0_31 = static_cast<u32>(GetInteger(ids_addr));
        buffer.address_bits_32_35.Assign(static_cast<u32>(GetInteger(ids_addr) >> 32) & 0xF);
        buffer.address_bits_36_38.Assign(static_cast<u32>(GetInteger(ids_addr) >> 36) & 0x7);
        buffer.size_bits_0_31 = sizeof(ids);
        std::memcpy(words.data(), &header, sizeof(header));
        std::memcpy(words.data() + 2, &handles, sizeof(handles));
        words[3] = transfer;
        std::memcpy(words.data() + 4, &buffer, sizeof(buffer));
        words[8] = Common::MakeMagic('S', 'F', 'C', 'I');
        words[10] = 10;
        Service::HLERequestContext request{kernel, memory, nullptr, &client};
        request.SetSessionRequestManager(manager);
        REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
        manager->ConvertToDomain();
        Service::NS::IReadOnlyApplicationControlDataInterface control_data{system};
        control_data.InvokeRequest(request);

        std::array<u64, 4> sizes{};
        memory.ReadBlock(heap, sizes.data(), sizeof(sizes));
        CHECK((sizes == std::array<u64, 4>{3, 8, 10, 0}));
        std::array<char, 18> icons{};
        memory.ReadBlock(heap + sizeof(sizes), icons.data(), icons.size());
        CHECK(std::string_view(icons.data(), icons.size()) == "nup-iconloose-icon");
    }

    const auto call_accessor = [&](Service::ServiceFrameworkBase& accessor, u32 command,
                                   std::string* display = nullptr, u64 first_arg = 0,
                                   u64 second_arg = 0,
                                   std::shared_ptr<IApplicationAccessor>* created = nullptr) {
        Kernel::KThread client{kernel};
        Kernel::KAutoObject::Create(&client);
        auto& owner = *home->GetHandle();
        REQUIRE(Kernel::KThread::InitializeDummyThread(system, &client, &owner).IsSuccess());
        SCOPE_EXIT {
            owner.UnregisterThread(kernel, &client);
            owner.Close(kernel);
            kernel.UnregisterKernelObject(&client);
        };
        Service::ServerManager server{system};
        auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(IPC::CommandType::Request);
        header.data_size.Assign(10);
        std::memcpy(words.data(), &header, sizeof(header));
        words[4] = Common::MakeMagic('S', 'F', 'C', 'I');
        words[6] = command;
        std::memcpy(words.data() + 8, &first_arg, sizeof(first_arg));
        std::memcpy(words.data() + 10, &second_arg, sizeof(second_arg));
        auto* caller = &client;
        if (created) {
            for (auto& thread : owner.GetThreadList()) {
                if (thread.IsUserThread()) {
                    caller = &thread;
                    break;
                }
            }
            REQUIRE(caller != &client);
        }
        Service::HLERequestContext request{kernel, owner.GetMemory(), nullptr, caller};
        request.SetSessionRequestManager(manager);
        REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
        if (created) {
            manager->ConvertToDomain();
        }
        accessor.InvokeRequest(request);
        if (created && request.CommandBuffer()[6] == ResultSuccess.raw) {
            REQUIRE(request.WriteToOutgoingCommandBuffer().IsSuccess());
            REQUIRE(manager->DomainHandlerCount() == 2);
            *created = request.GetDomainHandler<IApplicationAccessor>(1);
            REQUIRE(*created);
        }
        if (display && request.CommandBuffer()[6] == ResultSuccess.raw) {
            const auto* value = reinterpret_cast<const char*>(request.CommandBuffer() + 8);
            *display = std::string(value);
        }
        return u32(request.CommandBuffer()[6]);
    };

    const auto add_installed_control = [&](u64 id, std::string_view version) {
        FileSys::NACP nacp;
        nacp.raw.user_account_switch_lock = true;
        std::copy(version.begin(), version.end(), nacp.raw.version_string.begin());
        auto directory = std::make_shared<FileSys::VectorVfsDirectory>(
            std::vector<FileSys::VirtualFile>{std::make_shared<FileSys::VectorVfsFile>(
                nacp.GetRawBytes(), "control.nacp")});
        const auto romfs = FileSys::CreateRomFS(directory)->ReadAllBytes();

        FileSys::NcaHeader header{};
        header.magic = FileSys::NcaHeader::MagicDecrypted;
        header.content_type = FileSys::NcaHeader::ContentType::Control;
        header.program_id = id;
        header.sdk_addon_version = 0x000C0000;
        header.fs_info[0].start_sector = 6;
        header.fs_info[0].end_sector = 6 + static_cast<u32>((0x1000 + romfs.size() + 0x1FF) / 0x200);
        FileSys::NcaFsHeader fs{};
        fs.version = 2;
        fs.fs_type = FileSys::NcaFsHeader::FsType::RomFs;
        fs.hash_type = FileSys::NcaFsHeader::HashType::HierarchicalSha256Hash;
        fs.encryption_type = FileSys::NcaFsHeader::EncryptionType::None;
        auto& sha = fs.hash_data.hierarchical_sha256_data;
        sha.hash_block_size = 0x1000;
        sha.hash_layer_count = 2;
        sha.hash_layer_region[0].size = 0x20;
        sha.hash_layer_region[1].offset = 0x1000;
        sha.hash_layer_region[1].size = romfs.size();
        std::vector<u8> nca(std::size_t{header.fs_info[0].end_sector} * 0x200);
        std::memcpy(nca.data(), &header, sizeof(header));
        std::memcpy(nca.data() + sizeof(header), &fs, sizeof(fs));
        std::memcpy(nca.data() + 0x1C00, romfs.data(), romfs.size());
        installed.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Control,
                           id, std::make_shared<FileSys::VectorVfsFile>(std::move(nca), "control.nca"));
    };

    add_installed_control(loose_id, "9.0.0");
    auto loose_process = create(positive_loose, loose_id);
    REQUIRE(loose_process);
    auto loose_applet = std::make_shared<Applet>(system, std::move(loose_process), true);
    IApplicationFunctions loose_functions{system, loose_applet};
    std::string display;
    REQUIRE(call_accessor(loose_functions, 23, &display) == ResultSuccess.raw);
    CHECK(display == "9.0.0");
    const auto loose_raw = system.GetProcessControlData(loose_applet->aruid.pid);
    REQUIRE(loose_raw);
    CHECK(loose_raw->GetVersionString() == "3.0.0");

    IApplicationCreator creator{system, window};
    const auto create_accessor = [&](u64 id, u32 expected = ResultSuccess.raw) {
        std::shared_ptr<IApplicationAccessor> result;
        CHECK(call_accessor(creator, 0, nullptr, id, 0, &result) == expected);
        return result;
    };
    const auto failed_file = open(t.path / "Failed/exefs/main");
    installed.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Program,
                       failed_id, failed_file);
    auto failed_accessor = create_accessor(failed_id);
    REQUIRE(failed_accessor);
    const auto failed = window.GetMainApplet();
    REQUIRE(failed);
    CHECK(call_accessor(*failed_accessor, 10) != ResultSuccess.raw);
    CHECK_FALSE(window.GetMainApplet());
    CHECK_FALSE(failed->process->IsInitialized());
    CHECK(system.GetProgramIdForProcessId(failed->aruid.pid) == 0);
    CHECK(call_accessor(*failed_accessor, 30) != ResultSuccess.raw);
    {
        Kernel::KScopedSchedulerLock lock{kernel};
        CHECK(failed->state_changed_event.GetHandle()->IsSignaled(kernel));
    }

    auto& fs_controller = system.GetFileSystemController();
    std::vector<std::shared_ptr<IApplicationAccessor>> cancelled_accessors;
    std::vector<u64> cancelled_pids;
    for (u32 command : {20, 25}) {
        auto pending = create_accessor(TitleId);
        REQUIRE(pending);
        const auto pending_applet = window.GetMainApplet();
        REQUIRE(pending_applet);
        CHECK_FALSE(create_accessor(failed_id, Kernel::ResultBusy.raw));
        CHECK(window.GetMainApplet() == pending_applet);
        const u64 pending_pid = pending_applet->aruid.pid;
        CHECK(call_accessor(*pending, command) == ResultSuccess.raw);
        CHECK_FALSE(window.GetMainApplet());
        CHECK_FALSE(pending_applet->process->IsInitialized());
        CHECK(system.GetProgramIdForProcessId(pending_pid) == 0);
        CHECK_FALSE(window.GetByAppletResourceUserId(pending_pid));
        CHECK(call_accessor(*pending, 30) == ResultSuccess.raw);
        {
            Kernel::KScopedSchedulerLock lock{kernel};
            CHECK(pending_applet->state_changed_event.GetHandle()->IsSignaled(kernel));
        }
        CHECK(fs_controller.ListProcessBootContent(
                  pending_pid, FileSys::ContentRecordType::Data).empty());
        CHECK_FALSE(system.GetProcessControlData(pending_pid));
        CHECK(call_accessor(*pending, 10) != ResultSuccess.raw);
        cancelled_pids.push_back(pending_pid);
        cancelled_accessors.push_back(std::move(pending));
    }

    AppletMessage message{};
    while (home_applet->lifecycle_manager.PopMessage(kernel, &message)) {}

    auto access = create_accessor(TitleId);
    REQUIRE(access);
    auto& accessor = *access;
    const auto applet = window.GetMainApplet();
    REQUIRE(applet);
    REQUIRE(call_accessor(accessor, 10) == ResultSuccess.raw);
    for (std::size_t i = 0; i < cancelled_accessors.size(); ++i) {
        CHECK(system.GetProgramIdForProcessId(cancelled_pids[i]) == 0);
        CHECK(call_accessor(*cancelled_accessors[i], 101) != ResultSuccess.raw);
    }
    CHECK(fs::is_regular_file(cache_dir / "launched.json"));
    CHECK(Core::LaunchTimestampCache::GetLaunchCount(TitleId) == 3);
    const auto other_cache = t.path / "other-cache";
    REQUIRE(fs::create_directory(other_cache));
    Common::FS::SetEdenPath(Common::FS::EdenPath::CacheDir, other_cache);
    CHECK(Core::LaunchTimestampCache::GetLaunchCount(TitleId) == 0);
    CHECK_FALSE(fs::exists(other_cache / "launched.json"));
    Common::FS::SetEdenPath(Common::FS::EdenPath::CacheDir, cache_dir);
    CHECK(Core::LaunchTimestampCache::GetLaunchCount(TitleId) == 3);
    CHECK(call_accessor(accessor, 10) != ResultSuccess.raw);
    CHECK(window.GetMainApplet() == applet);
    const auto& application = applet->process;
    SCOPE_EXIT { finish_guest_threads(*application); };
    const u64 pid = application->GetProcessId();


    CHECK(application->GetDisplayVersion() == "2.0.0");
    IApplicationFunctions application_functions{system, applet};
    REQUIRE(call_accessor(application_functions, 23, &display) == ResultSuccess.raw);
    CHECK(display == "2.0.0");
    const auto control = system.GetProcessControlData(pid);
    REQUIRE(control);
    CHECK(control->GetUserAccountSwitchLock());
    CHECK(control->GetDLCBaseTitleId() == TitleId + 0x1000);
    CHECK(control->GetVersionString() == "1.0.0");
    const auto menu_control = FileSys::PatchManager::GetApplicationControlMetadata(system, TitleId);
    REQUIRE(menu_control.first);
    CHECK(menu_control.first->GetVersionString() == "1.0.0");

    auto& filesystem = system.GetFileSystemController();
    constexpr u64 sibling_id = TitleId + 1;
    const auto sibling = vfs.OpenDirectory((game / "base/romfs").string());
    REQUIRE(sibling);
    const auto addon_dir = vfs.OpenDirectory((game / "dlc/0400ABCD00011001/0/romfs").string());
    REQUIRE(addon_dir);
    REQUIRE(filesystem.SetProcessBootContent(
        pid, {{addon_id, FileSys::ContentRecordType::Data, addon_dir},
              {sibling_id, FileSys::ContentRecordType::Program, sibling}}).IsSuccess());
    auto child = create(open(t.path / "Child/exefs/main"), child_id);
    REQUIRE(child);
    REQUIRE(child->Run());
    SCOPE_EXIT { finish_guest_threads(*child); };
    const auto child_control = system.GetProcessControlData(child->GetProcessId());
    CHECK_FALSE(child_control);
    std::vector<u8> placeholder;
    REQUIRE(system.GetARPManager().GetProcessControlProperty(
        &placeholder, child_id, child->GetProcessId()).IsSuccess());
    CHECK(placeholder.size() == sizeof(FileSys::RawNACP));

    const auto account_lock = [&](Kernel::KProcess& owner) {
        Kernel::KThread client{kernel};
        Kernel::KAutoObject::Create(&client);
        REQUIRE(Kernel::KThread::InitializeDummyThread(system, &client, &owner).IsSuccess());
        SCOPE_EXIT {
            owner.UnregisterThread(kernel, &client);
            owner.Close(kernel);
            kernel.UnregisterKernelObject(&client);
        };
        Service::ServerManager server{system};
        auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(IPC::CommandType::Request);
        header.data_size.Assign(6);
        std::memcpy(words.data(), &header, sizeof(header));
        words[4] = Common::MakeMagic('S', 'F', 'C', 'I');
        words[6] = 150;
        Service::HLERequestContext request{kernel, owner.GetMemory(), nullptr, &client};
        request.SetSessionRequestManager(manager);
        REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
        Service::Account::Module::Interface account{nullptr, nullptr, system, "acc:test"};
        account.IsUserAccountSwitchLocked(request);
        CHECK(request.CommandBuffer()[6] == ResultSuccess.raw);
        return request.CommandBuffer()[8] != 0;
    };
    CHECK(account_lock(*application->GetHandle()));
    CHECK_FALSE(account_lock(*child->GetHandle()));
    add_installed_control(child_id, "4.0.0");
    CHECK(account_lock(*child->GetHandle()));
    Service::AOC::IAddOnContentManager aoc{system};
    u64 dlc_base{};
    REQUIRE(aoc.GetAddOnContentBaseId(&dlc_base, {pid}).IsSuccess());
    CHECK(dlc_base == TitleId + 0x1000);
    REQUIRE(aoc.GetAddOnContentBaseId(&dlc_base, {child->GetProcessId()}).IsSuccess());
    CHECK(dlc_base == FileSys::GetAOCBaseTitleID(child_id));
    u32 count{};
    REQUIRE(aoc.CountAddOnContent(&count, {pid}).IsSuccess());
    CHECK(count == 1);
    REQUIRE(aoc.CountAddOnContent(&count, {child->GetProcessId()}).IsSuccess());
    CHECK(count == 0);

    CHECK(filesystem.ListProcessBootContent(pid, FileSys::ContentRecordType::Data) ==
          std::vector<u64>{addon_id});
    CHECK(filesystem.ListProcessBootContent(child->GetProcessId(),
                                            FileSys::ContentRecordType::Data).empty());
    Service::FileSystem::ProgramId fs_title{};
    std::shared_ptr<Service::FileSystem::SaveDataController> save;
    std::shared_ptr<Service::FileSystem::RomFsController> romfs;
    REQUIRE(filesystem.OpenProcess(&fs_title, &save, &romfs, pid).IsSuccess());
    CHECK(romfs->OpenPatchedRomFSWithProgramIndex(TitleId, 1, FileSys::ContentRecordType::Program));
    CHECK_FALSE(create(source.file, TitleId, source.slot));
    CHECK(system.GetProcessControlData(pid));
    std::optional<std::size_t> dispatched_index;
    system.RegisterExecuteProgramCallback([&](std::size_t index) { dispatched_index = index; });
    const auto restart_command = static_cast<u64>(ProgramSpecifyKind::RestartProgram);
    const auto execute_command = static_cast<u64>(ProgramSpecifyKind::ExecuteProgram);
    system.GetUserChannel() = {{0x42}};
    CHECK(call_accessor(application_functions, 120, nullptr, execute_command, 2) != ResultSuccess.raw);
    CHECK(call_accessor(application_functions, 120, nullptr, restart_command) != ResultSuccess.raw);
    CHECK(call_accessor(application_functions, 12, nullptr, TitleId) != ResultSuccess.raw);
    CHECK_FALSE(dispatched_index);
    const std::vector<std::vector<u8>> original_channel{{0x42}};
    CHECK(system.GetUserChannel() == original_channel);
    CHECK(application->IsRunning());
    CHECK(home->IsRunning());

    finish_guest_threads(*application);
    REQUIRE(call_accessor(accessor, 25) == ResultSuccess.raw);
    REQUIRE(application->IsTerminated());
    window.Update();
    bool application_exited = false;
    while (home_applet->lifecycle_manager.PopMessage(kernel, &message)) {
        application_exited |= message == AppletMessage::ApplicationExited;
    }
    CHECK(application_exited);
    CHECK_FALSE(system.GetProcessControlData(pid));
    CHECK(filesystem.OpenProcess(&fs_title, &save, &romfs, pid).IsError());
    CHECK_FALSE(romfs->OpenPatchedRomFSWithProgramIndex(
        TitleId, 1, FileSys::ContentRecordType::Program));
    CHECK(filesystem.ListProcessBootContent(pid, FileSys::ContentRecordType::Data).empty());
    REQUIRE(home->IsRunning());
    auto successor_access = create_accessor(TitleId);
    REQUIRE(successor_access);
    auto& successor_accessor = *successor_access;
    const auto successor = window.GetMainApplet();
    REQUIRE(successor);
    CHECK(successor->aruid.pid != pid);
    REQUIRE(call_accessor(successor_accessor, 10) == ResultSuccess.raw);
    SCOPE_EXIT { finish_guest_threads(*successor->process); };
    CHECK(call_accessor(accessor, 101) != ResultSuccess.raw);
    CHECK(call_accessor(accessor, 10) != ResultSuccess.raw);
    CHECK(call_accessor(accessor, 25) == ResultSuccess.raw);
    CHECK(successor->process->IsRunning());
    Service::Glue::ARP_R arp_reader{system, system.GetARPManager()};
    Service::Glue::ARP_W arp_writer{system, system.GetARPManager()};
    CHECK(call_accessor(arp_reader, 0, nullptr, pid) != ResultSuccess.raw);
    CHECK(call_accessor(arp_reader, 2, nullptr, pid) != ResultSuccess.raw);
    CHECK(call_accessor(arp_writer, 1, nullptr, pid) != ResultSuccess.raw);
    CHECK(call_accessor(arp_reader, 0, nullptr, successor->aruid.pid) == ResultSuccess.raw);
    CHECK(filesystem.ListProcessBootContent(successor->aruid.pid,
                                            FileSys::ContentRecordType::Program).empty());
    CHECK(filesystem.ListProcessBootContent(successor->aruid.pid,
                                            FileSys::ContentRecordType::Data) ==
          std::vector<u64>{addon_id});

    const auto legacy_file = open(t.path / "Legacy/exefs/main");
    REQUIRE(legacy_file);
    installed.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Program,
                       legacy_id, legacy_file);
    const auto legacy_source = system.GetContentProviderUnion().GetApplicationProgram(legacy_id);
    CHECK_FALSE(legacy_source.loader_backed);
    finish_guest_threads(*successor->process);
    window.Update();
    auto legacy_access = create_accessor(legacy_id);
    REQUIRE(legacy_access);
    const auto legacy_applet = window.GetMainApplet();
    REQUIRE(legacy_applet);
    REQUIRE(call_accessor(*legacy_access, 10) == ResultSuccess.raw);
    const auto& legacy = legacy_applet->process;
    SCOPE_EXIT { finish_guest_threads(*legacy); };
    Service::Glue::ApplicationLaunchProperty launch{};
    REQUIRE(system.GetARPManager().GetLaunchProperty(&launch, legacy_id).IsSuccess());
    CHECK(launch.base_game_storage_id == FileSys::StorageId::NandUser);
    CHECK_FALSE(account_lock(*legacy->GetHandle()));

    IApplicationFunctions legacy_functions{system, legacy_applet};
    CHECK(call_accessor(legacy_functions, 120, nullptr, restart_command) != ResultSuccess.raw);
    CHECK_FALSE(dispatched_index);
    finish_guest_threads(*legacy);
    window.Update();
    REQUIRE(home->IsRunning());

    loose_applet->process->Finalize();
    auto native_loose_access = create_accessor(loose_id);
    REQUIRE(native_loose_access);
    const auto native_loose = window.GetMainApplet();
    REQUIRE(native_loose);
    IApplicationFunctions native_loose_functions{system, native_loose};
    CHECK(call_accessor(native_loose_functions, 120, nullptr, restart_command) != ResultSuccess.raw);
    CHECK_FALSE(dispatched_index);
    CHECK(call_accessor(native_loose_functions, 120, nullptr, execute_command, 2) != ResultSuccess.raw);
    CHECK(call_accessor(*native_loose_access, 25) == ResultSuccess.raw);
    CHECK(filesystem.ListProcessBootContent(legacy->GetProcessId(),
                                            FileSys::ContentRecordType::Data).empty());
    REQUIRE(call_accessor(loose_functions, 120, nullptr, execute_command, 2) == ResultSuccess.raw);
    REQUIRE(dispatched_index);
    CHECK(*dispatched_index == 2);
    system.RegisterExecuteProgramCallback({});
}
