// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/program_metadata.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"

namespace {

using FileSys::ProgramMetadata;
using Loader::ResultStatus;

constexpr std::size_t AcidOffset = 0x100;
constexpr std::size_t AciOffset = 0x500;

void Put32(std::vector<u8>& out, std::size_t offset, u32 value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

std::size_t AcidMagic(bool switch2) {
    return AcidOffset + (switch2 ? 0x300 : 0x200);
}

std::vector<u8> MakeNpdm(bool switch2 = false) {
    std::vector<u8> bytes(0x580);
    std::memcpy(bytes.data(), "META", 4);
    Put32(bytes, 0x70, AciOffset);
    Put32(bytes, 0x74, 0x80);
    Put32(bytes, 0x78, AcidOffset);
    Put32(bytes, 0x7C, switch2 ? 0x340 : 0x240);
    std::memcpy(bytes.data() + AcidMagic(switch2), "ACID", 4);
    Put32(bytes, AcidMagic(switch2) + 0x20, switch2 ? 0x340 : 0x240);
    Put32(bytes, AcidMagic(switch2) + 0x24, 0x2C);
    std::memcpy(bytes.data() + AciOffset, "ACI0", 4);
    Put32(bytes, AciOffset + 0x20, 0x40);
    Put32(bytes, AciOffset + 0x24, 0x1C);
    Put32(bytes, AciOffset + 0x30, 0x60);
    Put32(bytes, AciOffset + 0x34, 8);
    std::memset(bytes.data() + AciOffset + 0x60, 0x41, 0x20);
    return bytes;
}

ResultStatus Load(ProgramMetadata& npdm, std::vector<u8> bytes) {
    return npdm.Load(std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "main.npdm"));
}

ResultStatus Load(std::vector<u8> bytes) {
    ProgramMetadata npdm;
    return Load(npdm, std::move(bytes));
}

} // Anonymous namespace

TEST_CASE("NPDM in the Switch 1 and Switch 2 layouts loads", "[core][file_sys]") {
    for (const bool switch2 : {false, true}) {
        ProgramMetadata npdm;
        REQUIRE(Load(npdm, MakeNpdm(switch2)) == ResultStatus::Success);
        CHECK(npdm.IsSwitch2() == switch2);
        CHECK(npdm.GetKernelCapabilities() == std::vector<u32>{0x41414141, 0x41414141});
    }
}

TEST_CASE("Reloading an NPDM takes the platform of the reloaded file", "[core][file_sys]") {
    const auto native = std::make_shared<FileSys::VectorVfsFile>(MakeNpdm(true), "main.npdm");
    const auto legacy = std::make_shared<FileSys::VectorVfsFile>(MakeNpdm(false), "main.npdm");
    ProgramMetadata npdm;
    REQUIRE(npdm.Load(native) == ResultStatus::Success);
    REQUIRE(npdm.IsSwitch2());
    REQUIRE(npdm.Reload(legacy) == ResultStatus::Success);
    CHECK_FALSE(npdm.IsSwitch2());
    REQUIRE(npdm.Reload(native) == ResultStatus::Success);
    CHECK(npdm.IsSwitch2());
}

TEST_CASE("NPDM reads only whole kernel capability words", "[core][file_sys]") {
    auto bytes = MakeNpdm();
    Put32(bytes, AciOffset + 0x34, 5);
    ProgramMetadata npdm;
    REQUIRE(Load(npdm, std::move(bytes)) == ResultStatus::Success);
    CHECK(npdm.GetKernelCapabilities() == std::vector<u32>{0x41414141});
}

TEST_CASE("NPDM rejects a declared kernel capability size past the file", "[core][file_sys]") {
    auto bytes = MakeNpdm();
    Put32(bytes, AciOffset + 0x30, 0x7C);
    Put32(bytes, AciOffset + 0x34, 5);
    CHECK(Load(std::move(bytes)) == ResultStatus::ErrorBadKernelCapabilityDescriptors);
}

TEST_CASE("NPDM kernel capabilities may end exactly at the end of the file", "[core][file_sys]") {
    auto bytes = MakeNpdm();
    Put32(bytes, AciOffset + 0x34, 0x20);
    ProgramMetadata npdm;
    REQUIRE(Load(npdm, std::move(bytes)) == ResultStatus::Success);
    CHECK(npdm.GetKernelCapabilities().size() == 8);
}

TEST_CASE("NPDM with no kernel capabilities at the end of the file loads", "[core][file_sys]") {
    auto bytes = MakeNpdm();
    Put32(bytes, AciOffset + 0x30, 0x80);
    Put32(bytes, AciOffset + 0x34, 0);
    ProgramMetadata npdm;
    REQUIRE(Load(npdm, std::move(bytes)) == ResultStatus::Success);
    CHECK(npdm.GetKernelCapabilities().empty());
}

TEST_CASE("NPDM rejects kernel capabilities past the end of the file", "[core][file_sys]") {
    auto empty_past_end = MakeNpdm();
    Put32(empty_past_end, AciOffset + 0x30, 0x81);
    Put32(empty_past_end, AciOffset + 0x34, 0);
    CHECK(Load(std::move(empty_past_end)) == ResultStatus::ErrorBadKernelCapabilityDescriptors);

    for (const u32 size : {0x24u, 0xF0000000u}) {
        auto bytes = MakeNpdm();
        Put32(bytes, AciOffset + 0x34, size);
        CHECK(Load(std::move(bytes)) == ResultStatus::ErrorBadKernelCapabilityDescriptors);
    }
}

TEST_CASE("NPDM rejects section offsets that wrap past 4 GiB", "[core][file_sys]") {
    auto kac = MakeNpdm();
    Put32(kac, AciOffset + 0x30, static_cast<u32>(0x1'0000'0000 - AciOffset + 0x60));
    CHECK(Load(std::move(kac)) == ResultStatus::ErrorBadKernelCapabilityDescriptors);

    auto fac = MakeNpdm();
    Put32(fac, AcidMagic(false) + 0x20, static_cast<u32>(0x1'0000'0000 - AcidOffset + 0x40));
    CHECK(Load(std::move(fac)) == ResultStatus::ErrorBadFileAccessControl);

    auto fah = MakeNpdm();
    Put32(fah, AciOffset + 0x20, static_cast<u32>(0x1'0000'0000 - AciOffset + 0x40));
    CHECK(Load(std::move(fah)) == ResultStatus::ErrorBadFileAccessHeader);
}

TEST_CASE("NPDM file access blocks must fit in the file", "[core][file_sys]") {
    auto fac_end = MakeNpdm();
    Put32(fac_end, AcidMagic(false) + 0x20, 0x580 - AcidOffset - 0x2C);
    CHECK(Load(std::move(fac_end)) == ResultStatus::Success);
    auto fac = MakeNpdm();
    Put32(fac, AcidMagic(false) + 0x20, 0x580 - AcidOffset - 0x2B);
    CHECK(Load(std::move(fac)) == ResultStatus::ErrorBadFileAccessControl);
    auto fac_past = MakeNpdm();
    Put32(fac_past, AcidMagic(false) + 0x20, 0x580 - AcidOffset + 1);
    CHECK(Load(std::move(fac_past)) == ResultStatus::ErrorBadFileAccessControl);

    auto fah_end = MakeNpdm();
    Put32(fah_end, AciOffset + 0x20, 0x580 - AciOffset - 0x1C);
    CHECK(Load(std::move(fah_end)) == ResultStatus::Success);
    auto fah = MakeNpdm();
    Put32(fah, AciOffset + 0x20, 0x580 - AciOffset - 0x1B);
    CHECK(Load(std::move(fah)) == ResultStatus::ErrorBadFileAccessHeader);
    auto fah_past = MakeNpdm();
    Put32(fah_past, AciOffset + 0x20, 0x580 - AciOffset + 1);
    CHECK(Load(std::move(fah_past)) == ResultStatus::ErrorBadFileAccessHeader);
}

TEST_CASE("NPDM file access blocks past an ExeFS entry are not read from the container",
          "[core][file_sys]") {
    const auto check = [](std::size_t field, u32 relative_offset, ResultStatus expected) {
        auto bytes = MakeNpdm();
        const std::size_t npdm_size = bytes.size();
        Put32(bytes, field, relative_offset);
        bytes.resize(npdm_size + 0x100, 0x41);
        const auto container =
            std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "exefs.nsp0");
        ProgramMetadata npdm;
        CHECK(npdm.Load(std::make_shared<FileSys::OffsetVfsFile>(container, npdm_size, 0,
                                                                 "main.npdm")) == expected);
    };
    for (const s64 remaining : {2, 0, -0x10}) {
        check(AcidMagic(false) + 0x20, static_cast<u32>(0x580 - AcidOffset - remaining),
              ResultStatus::ErrorBadFileAccessControl);
        check(AciOffset + 0x20, static_cast<u32>(0x580 - AciOffset - remaining),
              ResultStatus::ErrorBadFileAccessHeader);
    }
}

TEST_CASE("NPDM ACID and ACI0 headers past an ExeFS entry are not read from the container",
          "[core][file_sys]") {
    const auto load = [](bool switch2, std::size_t entry_size) {
        const auto container =
            std::make_shared<FileSys::VectorVfsFile>(MakeNpdm(switch2), "exefs.nsp0");
        ProgramMetadata npdm;
        return npdm.Load(
            std::make_shared<FileSys::OffsetVfsFile>(container, entry_size, 0, "main.npdm"));
    };
    CHECK(load(false, 0xF0) == ResultStatus::ErrorBadACIDHeader);
    CHECK(load(true, 0x3F0) == ResultStatus::ErrorBadACIDHeader);
    CHECK(load(false, 0x4F0) == ResultStatus::ErrorBadACIHeader);

    auto bytes = MakeNpdm();
    bytes.resize(0x900);
    Put32(bytes, 0x78, 0x600);
    std::memcpy(bytes.data() + 0x800, "ACID", 4);
    const auto container = std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "exefs.nsp0");
    ProgramMetadata npdm;
    CHECK(npdm.Load(std::make_shared<FileSys::OffsetVfsFile>(container, 0x5F0, 0, "main.npdm")) ==
          ResultStatus::ErrorBadACIDHeader);
}

TEST_CASE("NPDM rejects a header without the META magic", "[core][file_sys]") {
    auto bytes = MakeNpdm();
    bytes[3] = 'X';
    CHECK(Load(std::move(bytes)) == ResultStatus::ErrorBadNPDMHeader);
}

TEST_CASE("NPDM rejects an ACI0 offset that does not point at an ACI0 block", "[core][file_sys]") {
    for (const bool switch2 : {false, true}) {
        for (const u32 offset : {0u, 1u, static_cast<u32>(AcidOffset)}) {
            auto bytes = MakeNpdm(switch2);
            Put32(bytes, 0x70, offset);
            CHECK(Load(std::move(bytes)) == ResultStatus::ErrorBadACIHeader);
        }
    }
}
