// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "input_common/drivers/silent_controller.h"

namespace fs = std::filesystem;
using InputCommon::SilentConnection;
using InputCommon::SilentConnectionTimeout;

namespace {

struct FakeHidTree {
    fs::path root = MakeRoot();
    fs::path sys_class_input = root / "sys/class/input";
    fs::path dev = root / "dev";

    static fs::path MakeRoot() {
        std::string pattern = (fs::temp_directory_path() / "orchid-hidraw-XXXXXX").string();
        REQUIRE(mkdtemp(pattern.data()) != nullptr);
        return fs::absolute(pattern);
    }

    explicit FakeHidTree(fs::perms hidraw_perms) {
        const fs::path hid = root / "sys/devices/uhid/0003:F00D:0170.0018";
        fs::create_directories(hid / "hidraw/hidraw9");
        fs::create_directories(sys_class_input / "event7/device");
        fs::create_directory_symlink(hid, sys_class_input / "event7/device/device");
        fs::create_directories(dev);
        std::ofstream{dev / "hidraw9"};
        fs::permissions(dev / "hidraw9", hidraw_perms);
    }
    ~FakeHidTree() {
        fs::remove_all(root);
    }

    std::string Check(SilentConnection& connection, std::chrono::steady_clock::time_point now) {
        return connection.Check(now, sys_class_input, dev);
    }
};

const auto connected = std::chrono::steady_clock::time_point{} + std::chrono::hours{1};

} // namespace

TEST_CASE("Silent controller warns once when its hidraw node can't be opened",
          "[input_common][silent_controller]") {
    if (geteuid() == 0) {
        SKIP("root can open every node");
    }
    FakeHidTree tree{fs::perms::none};
    SilentConnection connection;
    connection.Start("Test Pad", "/dev/input/event7", connected);

    CHECK(tree.Check(connection, connected + SilentConnectionTimeout - std::chrono::seconds{1})
              .empty());
    CHECK(connection.Warning().empty());

    const auto warning = tree.Check(connection, connected + SilentConnectionTimeout);
    CHECK(warning.starts_with("Test Pad has sent no input in the 10 s"));
    CHECK(warning.find((tree.dev / "hidraw9").string() + " can't be opened (Permission denied)") !=
          std::string::npos);
    CHECK(warning.find("KERNELS==\"0003:F00D:0170.*\"") != std::string::npos);
    CHECK(connection.Warning() == warning);
    CHECK(tree.Check(connection, connected + 2 * SilentConnectionTimeout).empty());

    SECTION("input clears it") {
        connection.MarkInput();
        CHECK(connection.Warning().empty());
    }
    SECTION("unplugging clears it") {
        connection.Stop();
        CHECK(connection.Warning().empty());
    }
    SECTION("a reconnect starts a new check") {
        connection.MarkInput();
        connection.Start("Test Pad", "/dev/input/event7", connected);
        CHECK(connection.Warning().empty());
        CHECK(tree.Check(connection, connected + SilentConnectionTimeout) == warning);
    }
}

TEST_CASE("Silent controller stays quiet when it sends input or its hidraw node opens",
          "[input_common][silent_controller]") {
    SECTION("accessible node") {
        FakeHidTree tree{fs::perms::owner_read | fs::perms::owner_write};
        SilentConnection connection;
        connection.Start("Test Pad", "/dev/input/event7", connected);
        errno = EACCES;
        CHECK(tree.Check(connection, connected + SilentConnectionTimeout).empty());
        CHECK(connection.Warning().empty());
    }
    SECTION("input before the timeout") {
        FakeHidTree tree{fs::perms::none};
        SilentConnection connection;
        connection.Start("Test Pad", "/dev/input/event7", connected);
        connection.MarkInput();
        CHECK(tree.Check(connection, connected + SilentConnectionTimeout).empty());
    }
    SECTION("missing node") {
        FakeHidTree tree{fs::perms::none};
        fs::remove(tree.dev / "hidraw9");
        SilentConnection connection;
        connection.Start("Test Pad", "/dev/input/event7", connected);
        CHECK(tree.Check(connection, connected + SilentConnectionTimeout).empty());
    }
    SECTION("not an evdev node") {
        FakeHidTree tree{fs::perms::none};
        SilentConnection connection;
        connection.Start("Test Pad", "/dev/hidraw9", connected);
        CHECK(tree.Check(connection, connected + SilentConnectionTimeout).empty());
    }
    SECTION("never connected") {
        FakeHidTree tree{fs::perms::none};
        SilentConnection connection;
        CHECK(tree.Check(connection, connected + SilentConnectionTimeout).empty());
    }
}
