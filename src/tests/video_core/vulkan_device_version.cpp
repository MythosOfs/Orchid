// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/vulkan_device.h"

TEST_CASE("HasBrokenVertexInputDynamicState decodes NVIDIA proprietary driver versions",
          "[video_core][vulkan]") {
    using Vulkan::Device;

    REQUIRE(Device::MakeNvidiaDriverVersion(580, 119, 2, 0) ==
            ((580u << 22) | (119u << 14) | (2u << 6)));
    REQUIRE(Device::MakeNvidiaDriverVersion(621, 24, 0, 5) ==
            ((621u << 22) | (24u << 14) | 5u));

    CHECK(Device::HasBrokenVertexInputDynamicState(
        VK_DRIVER_ID_NVIDIA_PROPRIETARY, Device::MakeNvidiaDriverVersion(575, 64, 0, 0)));
    CHECK(Device::HasBrokenVertexInputDynamicState(
        VK_DRIVER_ID_NVIDIA_PROPRIETARY, Device::MakeNvidiaDriverVersion(580, 118, 99, 0)));
    CHECK(Device::HasBrokenVertexInputDynamicState(
        VK_DRIVER_ID_NVIDIA_PROPRIETARY, Device::MakeNvidiaDriverVersion(580, 119, 1, 63)));

    CHECK_FALSE(Device::HasBrokenVertexInputDynamicState(
        VK_DRIVER_ID_NVIDIA_PROPRIETARY, Device::MakeNvidiaDriverVersion(580, 119, 2, 0)));
    CHECK_FALSE(Device::HasBrokenVertexInputDynamicState(
        VK_DRIVER_ID_NVIDIA_PROPRIETARY, Device::MakeNvidiaDriverVersion(580, 119, 2, 1)));
    CHECK_FALSE(Device::HasBrokenVertexInputDynamicState(
        VK_DRIVER_ID_NVIDIA_PROPRIETARY, Device::MakeNvidiaDriverVersion(621, 24, 0, 0)));

    CHECK(Device::HasBrokenVertexInputDynamicState(
        VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, VK_MAKE_API_VERSION(0, 20, 99, 0)));
    CHECK_FALSE(Device::HasBrokenVertexInputDynamicState(VK_DRIVER_ID_MESA_RADV, 0));
}
