// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/crypto/key_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/fssystem/fssystem_nca_header.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"

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
