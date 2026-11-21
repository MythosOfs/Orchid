// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/ips_layer.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {
FileSys::VirtualFile Text(std::string_view text) {
    return std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(text.begin(), text.end()),
                                                    "patch.pchtxt");
}

std::vector<u8> Patched(const FileSys::IPSwitchCompiler& compiler) {
    return compiler.Apply(std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(16, 0)))
        ->ReadAllBytes();
}
} // namespace

TEST_CASE("A pchtxt patch applies its build id, shift and string record", "[core][ipswitch]") {
    const FileSys::IPSwitchCompiler compiler{
        Text("@nsobid-0123\n@flag offset_shift 0x4\n@enabled\n00000002 \"hi\"\n")};
    const auto build_id = compiler.GetBuildID();
    REQUIRE(build_id[0] == 0x01);
    REQUIRE(build_id[1] == 0x23);
    REQUIRE(build_id[2] == 0);
    const auto bytes = Patched(compiler);
    REQUIRE(bytes.size() == 16);
    REQUIRE(bytes[6] == 'h');
    REQUIRE(bytes[7] == 'i');
    for (std::size_t i = 0; i < 16; ++i) {
        if (i != 6 && i != 7) {
            REQUIRE(bytes[i] == 0);
        }
    }
}

TEST_CASE("A short @n line does not throw", "[core][ipswitch]") {
    REQUIRE_NOTHROW(FileSys::IPSwitchCompiler{Text("@nsob\n")});
    const FileSys::IPSwitchCompiler compiler{Text("@nsob\n")};
    for (const u8 byte : compiler.GetBuildID()) {
        REQUIRE(byte == 0);
    }
}

TEST_CASE("A bare offset_shift flag does not read the next line", "[core][ipswitch]") {
    const FileSys::IPSwitchCompiler compiler{
        Text("@enabled\n@flag offset_shift\n00000002 \"hi\"\n")};
    const auto bytes = Patched(compiler);
    REQUIRE(bytes[2] == 'h');
    REQUIRE(bytes[3] == 'i');
}

TEST_CASE("A patch line at the end of the file without a newline is applied", "[core][ipswitch]") {
    const FileSys::IPSwitchCompiler compiler{Text("@enabled\n0000000f \"y\"\n00000003 \"x\"")};
    const auto bytes = Patched(compiler);
    REQUIRE(bytes[15] == 'y');
    REQUIRE(bytes[3] == 'x');
}

TEST_CASE("Escapes in a quoted replacement give their control bytes", "[core][ipswitch]") {
    const FileSys::IPSwitchCompiler compiler{Text("@enabled\n00000004 \"\\a\\e\\f\\q\"\n")};
    const auto bytes = Patched(compiler);
    REQUIRE(bytes[4] == 0x07);
    REQUIRE(bytes[5] == 0x1b);
    REQUIRE(bytes[6] == 0x0c);
    REQUIRE(bytes[7] == 'q');
    REQUIRE(bytes[8] == 0);
}
