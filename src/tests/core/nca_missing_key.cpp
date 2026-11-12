// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/crypto/key_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"

TEST_CASE("NCA without a header key reports the missing key", "[core][file_sys]") {
    if (Core::Crypto::KeyManager::Instance().HasKey(Core::Crypto::S256KeyType::Header)) {
        SKIP("A header key is installed");
    }
    // No NCA magic, encrypted or plain: the header is unreadable either way.
    const FileSys::NCA nca{
        std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(0xC00, 0xA5), "test.nca")};
    CHECK(nca.GetStatus() == Loader::ResultStatus::ErrorMissingHeaderKey);
}
