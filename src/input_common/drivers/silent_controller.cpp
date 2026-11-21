// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef __linux__
#include <cerrno>
#include <cstring>
#include <unistd.h>
#endif

#include <fmt/format.h>

#include "input_common/drivers/silent_controller.h"

namespace InputCommon {

std::string DescribeUnreadableHidraw([[maybe_unused]] const std::string& name,
                                     [[maybe_unused]] const std::string& joystick_path,
                                     [[maybe_unused]] const std::filesystem::path& sys_class_input,
                                     [[maybe_unused]] const std::filesystem::path& dev) {
#ifdef __linux__
    constexpr std::string_view input_dir = "/dev/input/";
    if (!joystick_path.starts_with(input_dir)) {
        return {};
    }
    // /sys/class/input/eventN/device is the input device, and its device is the HID device
    std::error_code ec;
    const auto hid_device = std::filesystem::canonical(
        sys_class_input / joystick_path.substr(input_dir.size()) / "device" / "device", ec);
    if (ec) {
        return {};
    }
    for (const auto& entry : std::filesystem::directory_iterator{hid_device / "hidraw", ec}) {
        const std::string node = (dev / entry.path().filename()).string();
        if (access(node.c_str(), R_OK | W_OK) == 0 || (errno != EACCES && errno != EPERM)) {
            continue;
        }
        const std::string error = std::strerror(errno);
        // HID devices are named BUS:VENDOR:PRODUCT.INSTANCE, and the instance changes on reconnect
        std::string hid_name = hid_device.filename().string();
        hid_name = hid_name.substr(0, hid_name.rfind('.'));
        return fmt::format(
            "{} has sent no input in the {} s since it connected, and its raw HID device {} "
            "can't be opened ({}). Bluetooth LE controllers come up through uhid, where logind "
            "often doesn't give your user access to the hidraw node. If the controller does "
            "nothing, allow access with a udev rule, for example in "
            "/etc/udev/rules.d/72-orchid-hidraw.rules: "
            "KERNEL==\"hidraw*\", KERNELS==\"{}.*\", MODE=\"0660\", TAG+=\"uaccess\" (then run "
            "\"sudo udevadm control --reload && sudo udevadm trigger\" and reconnect the "
            "controller).",
            name, SilentConnectionTimeout.count(), node, error, hid_name);
    }
#endif
    return {};
}

void SilentConnection::Start(std::string name, std::string joystick_path,
                             std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock{mutex};
    state = State{
        .name = std::move(name),
        .path = std::move(joystick_path),
        .connected_at = now,
    };
    input_seen = false;
}

void SilentConnection::Stop() {
    std::scoped_lock lock{mutex};
    state.reset();
}

void SilentConnection::MarkInput() {
    input_seen.store(true, std::memory_order_relaxed);
}

std::string SilentConnection::Check(std::chrono::steady_clock::time_point now,
                                    const std::filesystem::path& sys_class_input,
                                    const std::filesystem::path& dev) {
    std::scoped_lock lock{mutex};
    if (!state || state->checked || input_seen ||
        now - state->connected_at < SilentConnectionTimeout) {
        return {};
    }
    state->checked = true;
    state->warning = DescribeUnreadableHidraw(state->name, state->path, sys_class_input, dev);
    return state->warning;
}

std::string SilentConnection::Warning() const {
    std::scoped_lock lock{mutex};
    if (!state || input_seen) {
        return {};
    }
    return state->warning;
}

} // namespace InputCommon
