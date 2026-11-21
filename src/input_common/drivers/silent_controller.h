// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace InputCommon {

inline constexpr std::chrono::seconds SilentConnectionTimeout{10};

/// The warning for a controller that sent no input since it connected, when the raw HID device
/// behind its evdev node (the path SDL_GetJoystickPath returns) can't be opened. Empty otherwise.
std::string DescribeUnreadableHidraw(const std::string& name, const std::string& joystick_path,
                                     const std::filesystem::path& sys_class_input = "/sys/class/input",
                                     const std::filesystem::path& dev = "/dev");

/// One connection of a controller, which warns once when it stays silent and its raw HID device
/// can't be opened.
class SilentConnection {
public:
    void Start(std::string name, std::string joystick_path,
               std::chrono::steady_clock::time_point now);

    /// The controller was unplugged.
    void Stop();

    void MarkInput();

    /// Once per connection, SilentConnectionTimeout after it connected without input: returns
    /// the warning when its raw HID device can't be opened, empty otherwise.
    std::string Check(std::chrono::steady_clock::time_point now,
                      const std::filesystem::path& sys_class_input = "/sys/class/input",
                      const std::filesystem::path& dev = "/dev");

    /// The warning of the current connection, until the controller sends input.
    std::string Warning() const;

private:
    struct State {
        std::string name;
        std::string path;
        std::chrono::steady_clock::time_point connected_at;
        bool checked{};
        std::string warning;
    };
    mutable std::mutex mutex;
    std::optional<State> state;
    std::atomic<bool> input_seen{};
};

} // namespace InputCommon
