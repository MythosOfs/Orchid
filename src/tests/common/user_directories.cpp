// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "common/fs/path_util.h"

namespace {
namespace fs = std::filesystem;
using Common::FS::SelectUserDirectories;
using Common::FS::UserDirectoryKind;
using Common::FS::UserDirectoryRoots;

// Laid out like the Linux defaults: three separate roots per name.
const fs::path portable_dir{"/work/user"};
const UserDirectoryRoots orchid_roots{"/home/u/.local/share/orchid", "/home/u/.config/orchid",
                                      "/home/u/.cache/orchid"};
const UserDirectoryRoots legacy_roots{"/home/u/.local/share/eden", "/home/u/.config/eden",
                                      "/home/u/.cache/eden"};

/// Runs the selection against a made-up set of existing directories.
Common::FS::UserDirectorySelection Select(std::vector<fs::path> existing) {
    return SelectUserDirectories(
        portable_dir, orchid_roots, legacy_roots, [&existing](const fs::path& path) {
            return std::find(existing.begin(), existing.end(), path) != existing.end();
        });
}
} // namespace

TEST_CASE("UserDirectories: a new install gets the orchid directories", "[common]") {
    const auto selection = Select({});
    CHECK(selection.kind == UserDirectoryKind::Orchid);
    CHECK(selection.roots == orchid_roots);
}

TEST_CASE("UserDirectories: an existing eden install keeps being used", "[common]") {
    const auto selection = Select({legacy_roots.data, legacy_roots.config, legacy_roots.cache});
    CHECK(selection.kind == UserDirectoryKind::Legacy);
    CHECK(selection.roots == legacy_roots);
}

TEST_CASE("UserDirectories: orchid wins when both installs exist", "[common]") {
    const auto selection = Select({orchid_roots.data, orchid_roots.config, orchid_roots.cache,
                                   legacy_roots.data, legacy_roots.config, legacy_roots.cache});
    CHECK(selection.kind == UserDirectoryKind::Orchid);
    CHECK(selection.roots == orchid_roots);
}

TEST_CASE("UserDirectories: data or config alone marks an install", "[common]") {
    // All three roots follow the one decision, whichever root gave the install away.
    CHECK(Select({legacy_roots.data}).roots == legacy_roots);
    CHECK(Select({legacy_roots.config}).roots == legacy_roots);
    CHECK(Select({orchid_roots.data, legacy_roots.data, legacy_roots.config}).roots ==
          orchid_roots);
    CHECK(Select({orchid_roots.config, legacy_roots.data, legacy_roots.config}).roots ==
          orchid_roots);
}

TEST_CASE("UserDirectories: a cache directory alone marks nothing", "[common]") {
    // A leftover eden cache does not pull a new install into the eden directories ...
    CHECK(Select({legacy_roots.cache}).kind == UserDirectoryKind::Orchid);
    CHECK(Select({legacy_roots.cache}).roots == orchid_roots);
    // ... and a foreign ~/.cache/orchid does not hide an existing eden install.
    CHECK(Select({orchid_roots.cache, legacy_roots.data, legacy_roots.config}).kind ==
          UserDirectoryKind::Legacy);
    CHECK(Select({orchid_roots.cache, legacy_roots.data, legacy_roots.config}).roots ==
          legacy_roots);
}

TEST_CASE("UserDirectories: the portable directory wins over everything", "[common]") {
    const UserDirectoryRoots portable_roots{portable_dir, portable_dir / "config",
                                            portable_dir / "cache"};
    for (const auto& existing :
         {std::vector<fs::path>{portable_dir},
          std::vector<fs::path>{portable_dir, legacy_roots.data, legacy_roots.config},
          std::vector<fs::path>{portable_dir, orchid_roots.data, legacy_roots.data}}) {
        const auto selection = Select(existing);
        CHECK(selection.kind == UserDirectoryKind::Portable);
        CHECK(selection.roots == portable_roots);
    }
}

TEST_CASE("UserDirectories: no portable candidate falls through to the global ones", "[common]") {
    // An empty candidate must not match, even for a predicate that accepts everything.
    const auto selection =
        SelectUserDirectories({}, orchid_roots, legacy_roots, [](const fs::path&) { return true; });
    CHECK(selection.kind == UserDirectoryKind::Orchid);
    CHECK(selection.roots == orchid_roots);
}

TEST_CASE("UserDirectories: selection on the real filesystem", "[common]") {
    const fs::path base =
        fs::temp_directory_path() /
        ("orchid-user-dirs-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path portable = base / "user";
    const UserDirectoryRoots orchid{base / "data/orchid", base / "config/orchid",
                                    base / "cache/orchid"};
    const UserDirectoryRoots legacy{base / "data/eden", base / "config/eden", base / "cache/eden"};
    fs::create_directories(base);

    // Nothing there yet.
    CHECK(SelectUserDirectories(portable, orchid, legacy).roots == orchid);

    // A regular file of that name is not an install.
    fs::create_directories(legacy.data.parent_path());
    std::ofstream{legacy.data}.close();
    CHECK(SelectUserDirectories(portable, orchid, legacy).kind == UserDirectoryKind::Orchid);
    fs::remove(legacy.data);

    fs::create_directories(legacy.data);
    CHECK(SelectUserDirectories(portable, orchid, legacy).kind == UserDirectoryKind::Legacy);
    CHECK(SelectUserDirectories(portable, orchid, legacy).roots == legacy);

    fs::create_directories(orchid.data);
    CHECK(SelectUserDirectories(portable, orchid, legacy).kind == UserDirectoryKind::Orchid);

    fs::create_directories(portable);
    CHECK(SelectUserDirectories(portable, orchid, legacy).kind == UserDirectoryKind::Portable);
    CHECK(SelectUserDirectories(portable, orchid, legacy).roots.data == portable);

    fs::remove_all(base);
}
