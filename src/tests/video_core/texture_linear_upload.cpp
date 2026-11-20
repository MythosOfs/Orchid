// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"

namespace {
std::vector<u8> RandomBytes(size_t size) {
    std::vector<u8> bytes(size);
    u32 random = 0x51f15e;
    for (auto& byte : bytes) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        byte = static_cast<u8>(random);
    }
    return bytes;
}
} // namespace

TEST_CASE("A pitch-linear image is read once, straight into the upload buffer",
          "[texture_linear_upload]") {
    const bool native = GENERATE(false, true);
    VideoCommon::ImageInfo info;
    info.type = VideoCommon::ImageType::Linear;
    info.format = VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;
    info.size = {37, 19, 1};
    info.pitch = 160; // 40 texels a row, three of them padding
    const std::vector<u8> guest = RandomBytes(VideoCommon::CalculateGuestSizeInBytes(info));
    REQUIRE(guest.size() == 160 * 19);
    constexpr GPUVAddr address = 0x123450000;
    constexpr size_t guard = 32;
    std::vector<u8> storage(guest.size() + 2 * guard, 0xa5);
    size_t reads = 0;
    // No input: the caller does not read the image as well.
    const auto copies = VideoCommon::UnswizzleImage(
        [&](GPUVAddr source, std::span<u8> bytes) {
            REQUIRE(source == address);
            REQUIRE(bytes.size() == guest.size());
            std::memcpy(bytes.data(), guest.data(), bytes.size());
            ++reads;
        },
        address, info, {}, std::span<u8>{storage}.subspan(guard, guest.size()), native);
    CHECK(reads == 1);
    CHECK(std::equal(guest.begin(), guest.end(), storage.begin() + guard));
    CHECK(std::ranges::all_of(std::span{storage}.first(guard), [](u8 b) { return b == 0xa5; }));
    CHECK(std::ranges::all_of(std::span{storage}.last(guard), [](u8 b) { return b == 0xa5; }));
    REQUIRE(copies.size() == 1);
    CHECK(copies[0].buffer_offset == 0);
    CHECK(copies[0].buffer_size == guest.size());
    CHECK(copies[0].buffer_row_length == 40);
    CHECK(copies[0].buffer_image_height == 19);
    CHECK(copies[0].image_subresource.base_level == 0);
    CHECK(copies[0].image_subresource.base_layer == 0);
    CHECK(copies[0].image_subresource.num_layers == 1);
    CHECK(copies[0].image_offset == VideoCommon::Offset3D{0, 0, 0});
    CHECK(copies[0].image_extent == info.size);
}

TEST_CASE("A block-linear image is unswizzled from the bytes it is given, without a read",
          "[texture_linear_upload]") {
    const bool native = GENERATE(false, true);
    VideoCommon::ImageInfo info;
    info.type = VideoCommon::ImageType::e2D;
    info.format = VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;
    info.size = {70, 33, 1};
    info.resources = {2, 1};
    info.block.height = 2;
    info.layer_stride = VideoCommon::CalculateLayerStride(info);
    const std::vector<u8> guest = RandomBytes(VideoCommon::CalculateGuestSizeInBytes(info));
    std::vector<u8> expected(VideoCommon::CalculateUnswizzledSizeBytes(info));
    const auto reference = VideoCommon::UnswizzleBlockLinearImage(info, guest, expected, native);
    std::vector<u8> output(expected.size());
    const auto copies = VideoCommon::UnswizzleImage(
        [](GPUVAddr, std::span<u8>) { FAIL("Unexpected read"); }, 0, info, guest, output, native);
    CHECK(output == expected);
    REQUIRE(copies.size() == reference.size());
    for (size_t i = 0; i < copies.size(); ++i) {
        CHECK(copies[i].buffer_offset == reference[i].buffer_offset);
        CHECK(copies[i].buffer_size == reference[i].buffer_size);
        CHECK(copies[i].image_extent == reference[i].image_extent);
    }
}
