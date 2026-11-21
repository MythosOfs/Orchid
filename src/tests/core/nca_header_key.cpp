// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "core/core.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/fssystem/fssystem_nca_header.h"
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

std::vector<u8> SingleFileNsp(const std::string& name, const std::vector<u8>& contents) {
    std::vector<u8> bytes(0x28);
    const u32 header[]{Common::MakeMagic('P', 'F', 'S', '0'), 1, static_cast<u32>(name.size() + 1), 0};
    const u64 entry[]{0, contents.size()};
    std::memcpy(bytes.data(), header, sizeof(header));
    std::memcpy(bytes.data() + 0x10, entry, sizeof(entry));
    bytes.insert(bytes.end(), name.begin(), name.end());
    bytes.push_back(0);
    bytes.insert(bytes.end(), contents.begin(), contents.end());
    return bytes;
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
    const auto file = std::make_shared<VectorVfsFile>(
        SingleFileNsp("0123456789abcdef0123456789abcdef.cnmt.nca", std::vector<u8>(0xC00, 0)),
        "test.nsp");

    ScopedSystemTestContext environment;
    Core::System system;
    ContentProviderUnion content_provider;
    Kernel::KProcess process{system.Kernel()};
    Loader::AppLoader_NSP loader{file, system.GetFileSystemController(), content_provider, 0, 0};
    CHECK(loader.Load(process, system).first == Loader::ResultStatus::ErrorMissingHeaderKey);
}
