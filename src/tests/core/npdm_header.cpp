// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/program_metadata.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"

namespace {
FileSys::VirtualFile MakeFile(std::vector<u8> bytes) {
    return std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "main.npdm");
}
} // namespace

TEST_CASE("NPDM checks META before interpreting its section offsets", "[core][file_sys]") {
    std::vector<u8> npdm(0x80, 0);
    std::memcpy(npdm.data(), "META", 4);

    FileSys::ProgramMetadata metadata;
    REQUIRE(metadata.Load(nullptr) == Loader::ResultStatus::ErrorBadNPDMHeader);
    // A valid signature advances to the next check: the ACID header is absent.
    REQUIRE(metadata.Load(MakeFile(npdm)) == Loader::ResultStatus::ErrorBadACIDHeader);

    std::memcpy(npdm.data(), "NX2!", 4);
    REQUIRE(metadata.Load(MakeFile(npdm)) == Loader::ResultStatus::ErrorBadNPDMHeader);
    REQUIRE(metadata.Reload(MakeFile(npdm)) == Loader::ResultStatus::ErrorBadNPDMHeader);

    npdm.resize(0x7f);
    REQUIRE(metadata.Load(MakeFile(std::move(npdm))) ==
            Loader::ResultStatus::ErrorBadNPDMHeader);
}
