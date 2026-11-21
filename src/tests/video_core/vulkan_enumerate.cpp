// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace {
// Device counts the stub driver reports: one per count call, then one per fill call.
std::vector<u32> g_counts;
std::vector<u32> g_fills;
std::vector<VkResult> g_fill_results;
size_t g_count_calls;
size_t g_fill_calls;

VKAPI_ATTR VkResult VKAPI_CALL Enumerate(VkInstance, u32* count, VkPhysicalDevice* devices) {
    if (devices == nullptr) {
        *count = g_counts[(std::min)(g_count_calls++, g_counts.size() - 1)];
        return VK_SUCCESS;
    }
    const size_t call = (std::min)(g_fill_calls++, g_fills.size() - 1);
    const u32 available = g_fills[call];
    const u32 written = (std::min)(*count, available);
    for (u32 i = 0; i < written; ++i) {
        devices[i] = reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(0x100 + i));
    }
    *count = written;
    return g_fill_results[(std::min)(call, g_fill_results.size() - 1)];
}

VKAPI_ATTR void VKAPI_CALL GetProperties(VkPhysicalDevice device,
                                         VkPhysicalDeviceProperties* properties) {
    *properties = {};
    const std::string name = "GPU " + std::to_string(reinterpret_cast<uintptr_t>(device));
    std::strncpy(properties->deviceName, name.c_str(), sizeof(properties->deviceName) - 1);
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance, const VkAllocationCallbacks*) {}

std::vector<VkPhysicalDevice> Run(std::vector<u32> counts, std::vector<u32> fills,
                                  std::vector<VkResult> results) {
    g_counts = std::move(counts);
    g_fills = std::move(fills);
    g_fill_results = std::move(results);
    g_count_calls = 0;
    g_fill_calls = 0;
    Vulkan::vk::InstanceDispatch dld{};
    dld.vkEnumeratePhysicalDevices = Enumerate;
    dld.vkGetPhysicalDeviceProperties = GetProperties;
    dld.vkDestroyInstance = DestroyInstance;
    const Vulkan::vk::Instance instance(reinterpret_cast<VkInstance>(uintptr_t{1}), dld);
    return instance.EnumeratePhysicalDevices();
}

VkResult ResultOf(std::vector<u32> counts, std::vector<u32> fills, std::vector<VkResult> results) {
    try {
        Run(std::move(counts), std::move(fills), std::move(results));
    } catch (const Vulkan::vk::Exception& exception) {
        return exception.GetResult();
    }
    return VK_SUCCESS;
}
} // Anonymous namespace

TEST_CASE("A device that appears during enumeration is counted again", "[video_core][vulkan]") {
    const auto devices = Run({1, 2}, {2, 2}, {VK_INCOMPLETE, VK_SUCCESS});
    CHECK(devices.size() == 2);
    CHECK(g_count_calls == 2);
    for (const VkPhysicalDevice device : devices) {
        CHECK(device != VK_NULL_HANDLE);
    }
}

TEST_CASE("A device that disappears during enumeration leaves no null handle",
          "[video_core][vulkan]") {
    const auto devices = Run({3}, {2}, {VK_SUCCESS});
    CHECK(devices.size() == 2);
    for (const VkPhysicalDevice device : devices) {
        CHECK(device != VK_NULL_HANDLE);
    }
}

TEST_CASE("Enumeration that stays incomplete ends with VK_INCOMPLETE", "[video_core][vulkan]") {
    CHECK(ResultOf({1}, {2}, {VK_INCOMPLETE}) == VK_INCOMPLETE);
    CHECK(g_count_calls == 8);
    CHECK(g_fill_calls == 8);
}

TEST_CASE("Enumeration errors are thrown at once", "[video_core][vulkan]") {
    CHECK(ResultOf({1}, {1}, {VK_ERROR_INITIALIZATION_FAILED}) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(g_fill_calls == 1);
    CHECK(Run({0}, {0}, {VK_SUCCESS}).empty());
}

TEST_CASE("A device that appears after an empty count leaves no null handle",
          "[video_core][vulkan]") {
    // With no devices the fill call gets no array and only counts again.
    for (const VkPhysicalDevice device : Run({0, 1}, {1}, {VK_SUCCESS})) {
        CHECK(device != VK_NULL_HANDLE);
    }
}
