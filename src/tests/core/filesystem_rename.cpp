// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "core/core.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/fssrv/fssrv_sf_path.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/fsp/fs_i_filesystem.h"
#include "tests/core/system_test_context.h"

TEST_CASE("RenameFile reports a missing source in another directory", "[core][filesystem_rename]") {
    ScopedSystemTestContext context;
    Core::System system;
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "rename-test";
    REQUIRE(std::filesystem::create_directories(path / "a"));
    REQUIRE(std::filesystem::create_directory(path / "b"));
    FileSys::RealVfsFilesystem vfs;
    const auto root =
        vfs.OpenDirectory(Common::FS::PathToUTF8String(path), FileSys::OpenMode::ReadWrite);
    REQUIRE(root != nullptr);
    Service::FileSystem::IFileSystem filesystem{system, root, {}};
    const auto old_path = FileSys::Sf::Path::Encode("/a/missing.bin");
    const auto new_path = FileSys::Sf::Path::Encode("/b/moved.bin");

    CHECK(filesystem.RenameFile(old_path, new_path) == FileSys::ResultPathNotFound);
    CHECK_FALSE(std::filesystem::exists(path / "b" / "moved.bin"));

#ifndef _WIN32
    // The same handler moves an existing file. Not checked on Windows: the move still holds the
    // source open when it deletes it, and Windows does not delete open files without
    // FILE_SHARE_DELETE.
    std::ofstream{path / "a" / "present.bin", std::ios::binary} << "orchid";
    CHECK(filesystem.RenameFile(FileSys::Sf::Path::Encode("/a/present.bin"), new_path) ==
          ResultSuccess);
    CHECK_FALSE(std::filesystem::exists(path / "a" / "present.bin"));
    std::ifstream moved{path / "b" / "moved.bin", std::ios::binary};
    CHECK(std::string{std::istreambuf_iterator<char>{moved}, {}} == "orchid");
#endif
}
