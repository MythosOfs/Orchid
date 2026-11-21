// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "core/core.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/fssystem/fssystem_nca_header.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/loader.h"
#include "core/loader/nsp.h"
#include "tests/core/system_test_context.h"

namespace {

using namespace FileSys;

class UnreadableFile final : public VectorVfsFile {
public:
    using VectorVfsFile::VectorVfsFile;
    std::size_t Read(u8*, std::size_t, std::size_t) const override {
        return 0;
    }
};

Loader::ResultStatus StatusOf(std::vector<u8> bytes) {
    return NCA{std::make_shared<VectorVfsFile>(std::move(bytes))}.GetStatus();
}

std::vector<u8> PlaintextHeader(u32 magic, u32 sdk_addon_version) {
    std::vector<u8> bytes(0xC00);
    NcaHeader header{};
    header.magic = magic;
    header.sdk_addon_version = sdk_addon_version;
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

using Files = std::vector<std::pair<std::string, std::vector<u8>>>;

std::vector<u8> MakePartition(const Files& files) {
    std::vector<u8> bytes(0x10 + files.size() * 0x18);
    std::string names;
    u64 offset = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const u64 entry[]{offset, files[i].second.size(), names.size()};
        std::memcpy(bytes.data() + 0x10 + i * 0x18, entry, sizeof(entry));
        names += files[i].first + '\0';
        offset += files[i].second.size();
    }
    const u32 header[]{Common::MakeMagic('P', 'F', 'S', '0'), static_cast<u32>(files.size()),
                       static_cast<u32>(names.size()), 0};
    std::memcpy(bytes.data(), header, sizeof(header));
    bytes.insert(bytes.end(), names.begin(), names.end());
    for (const auto& file : files) {
        bytes.insert(bytes.end(), file.second.begin(), file.second.end());
    }
    return bytes;
}

std::vector<u8> MakeNca(u64 title_id, NcaHeader::ContentType type, const Files& contents) {
    const auto pfs = MakePartition(contents);
    std::vector<u8> partition(0x1000);
    partition.insert(partition.end(), pfs.begin(), pfs.end());
    NcaHeader header{};
    header.magic = NcaHeader::MagicDecrypted;
    header.content_type = type;
    header.program_id = title_id;
    header.sdk_addon_version = 0x000C0000;
    header.fs_info[0].start_sector = 6;
    header.fs_info[0].end_sector = 6 + static_cast<u32>((partition.size() + 0x1FF) / 0x200);
    NcaFsHeader fs{};
    fs.version = 2;
    fs.fs_type = NcaFsHeader::FsType::PartitionFs;
    fs.hash_type = NcaFsHeader::HashType::HierarchicalSha256Hash;
    fs.encryption_type = NcaFsHeader::EncryptionType::None;
    auto& sha = fs.hash_data.hierarchical_sha256_data;
    sha.hash_block_size = 0x1000;
    sha.hash_layer_count = 2;
    sha.hash_layer_region[0].size = 0x20;
    sha.hash_layer_region[1].offset = 0x1000;
    sha.hash_layer_region[1].size = pfs.size();
    std::vector<u8> bytes(0xC00);
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), &fs, sizeof(fs));
    bytes.insert(bytes.end(), partition.begin(), partition.end());
    bytes.resize(std::size_t{header.fs_info[0].end_sector} * 0x200);
    return bytes;
}

Loader::ResultStatus LoadNsp(const Files& files) {
    const auto file = std::make_shared<VectorVfsFile>(MakePartition(files), "test.nsp");
    ScopedSystemTestContext environment;
    Core::System system;
    ContentProviderUnion content_provider;
    Kernel::KProcess process{system.Kernel()};
    Loader::AppLoader_NSP loader{file, system.GetFileSystemController(), content_provider, 0, 0};
    return loader.Load(process, system).first;
}

} // namespace

TEST_CASE("NCA asks for the header key only when the header needs it", "[core][file_sys][nca_header_key]") {
    const auto& keys = Core::Crypto::KeyManager::Instance();
    if (keys.HasKey(Core::Crypto::S256KeyType::Header) ||
        keys.HasKey(Core::Crypto::S128KeyType::KeyArea, 0, 0)) {
        SKIP("Console keys are installed");
    }

    SECTION("A full-size header that is not plaintext needs the header key") {
        CHECK(StatusOf(std::vector<u8>(0xC00, 0xA5)) == Loader::ResultStatus::ErrorMissingHeaderKey);
    }

    SECTION("A file shorter than a header is damaged") {
        CHECK(StatusOf(std::vector<u8>(0x100, 0xA5)) == Loader::ResultStatus::ErrorBadNCAHeader);
    }

    SECTION("A file that ends inside the FS headers is damaged") {
        CHECK(StatusOf(std::vector<u8>(0x400, 0xA5)) == Loader::ResultStatus::ErrorBadNCAHeader);
        CHECK(StatusOf(std::vector<u8>(0xBFF, 0xA5)) == Loader::ResultStatus::ErrorBadNCAHeader);
    }

    SECTION("A header that cannot be read is damaged") {
        const NCA nca{std::make_shared<UnreadableFile>(std::vector<u8>(0xC00, 0xA5))};
        CHECK(nca.GetStatus() == Loader::ResultStatus::ErrorBadNCAHeader);
    }

    SECTION("A rejected plaintext header does not ask for keys") {
        for (const u32 magic : {NcaHeader::Magic0, NcaHeader::Magic1, NcaHeader::Magic2,
                                NcaHeader::Magic3, NcaHeader::MagicDecrypted}) {
            INFO("magic " << magic);
            CHECK(StatusOf(PlaintextHeader(magic, 0)) == Loader::ResultStatus::ErrorBadNCAHeader);
        }
    }

    SECTION("Readable plaintext headers keep their own diagnostics") {
        CHECK(StatusOf(PlaintextHeader(NcaHeader::Magic3, 0x000C0000)) ==
              Loader::ResultStatus::ErrorMissingKeyAreaKey);
        CHECK(StatusOf(PlaintextHeader(NcaHeader::MagicDecrypted, 0x000C0000)) ==
              Loader::ResultStatus::Success);
    }
}

TEST_CASE("NSP launch reports the missing header key of its metadata NCA",
          "[core][loader][nca_header_key]") {
    const auto& keys = Core::Crypto::KeyManager::Instance();
    if (keys.HasKey(Core::Crypto::S256KeyType::Header)) {
        SKIP("Console keys are installed");
    }
    CHECK(LoadNsp({{"0123456789abcdef0123456789abcdef.cnmt.nca", std::vector<u8>(0xC00, 0)}}) ==
          Loader::ResultStatus::ErrorMissingHeaderKey);
}

TEST_CASE("NSP launch rejects a title ID 0 program listed under another title",
          "[core][loader][nca_header_key]") {
    if (Core::Crypto::KeyManager::KeyFileExists(false)) {
        SKIP("A key file is installed");
    }
    // The metadata lists the program under another title, so only the title ID check rejects it.
    CNMTHeader header{};
    header.title_id = 0x0100000000010000;
    header.type = TitleType::Application;
    header.table_offset = sizeof(OptionalHeader);
    header.number_content_entries = 1;
    ContentRecord record{};
    record.nca_id.fill(0x11);
    record.type = ContentRecordType::Program;
    const CNMT cnmt{header, {}, {record}, {}};
    CHECK(LoadNsp({{"meta.cnmt.nca", MakeNca(header.title_id, NcaHeader::ContentType::Meta,
                                              {{"test.cnmt", cnmt.Serialize()}})},
                   {"11111111111111111111111111111111.nca",
                    MakeNca(0, NcaHeader::ContentType::Program, {{"main", {0}}})}}) ==
          Loader::ResultStatus::ErrorNSPMissingProgramNCA);
}
