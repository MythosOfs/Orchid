// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"

namespace {

using VideoCommon::ImageInfo;
using VideoCommon::ImageType;
using VideoCommon::ScaledImageFits;
using VideoCore::Surface::PixelFormat;

ImageInfo Image(u32 width, u32 height, ImageType type = ImageType::e2D) {
    ImageInfo info;
    info.format = PixelFormat::A8B8G8R8_UNORM;
    info.type = type;
    info.size = {width, height, 1};
    return info;
}

Settings::ResolutionScalingInfo Scale(u32 up_scale, u32 down_shift) {
    Settings::ResolutionScalingInfo resolution;
    resolution.up_scale = up_scale;
    resolution.down_shift = down_shift;
    resolution.active = up_scale != 1 || down_shift != 0;
    return resolution;
}

} // namespace

TEST_CASE("A 1080p target at 8x stays native on an 8192 device and scales on a 16384 one",
          "[video_core][texture_cache]") {
    const ImageInfo target = Image(1920, 1080);
    REQUIRE_FALSE(ScaledImageFits(target, Scale(8, 0), {8192, 8192})); // 15360x8640
    REQUIRE(ScaledImageFits(target, Scale(4, 0), {8192, 8192}));       // 7680x4320
    REQUIRE(ScaledImageFits(target, Scale(8, 0), {16384, 16384}));
}

TEST_CASE("Scaled image limits", "[video_core][texture_cache]") {
    // Framebuffer width and height limits may differ.
    REQUIRE(ScaledImageFits(Image(3072, 1024), Scale(2, 0), {8192, 4096}));
    REQUIRE_FALSE(ScaledImageFits(Image(1024, 3072), Scale(2, 0), {8192, 4096}));
    REQUIRE(ScaledImageFits(Image(4096, 4096), Scale(2, 0), {8192, 8192}));
    REQUIRE_FALSE(ScaledImageFits(Image(4097, 2048), Scale(2, 0), {8192, 8192}));
    REQUIRE_FALSE(ScaledImageFits(Image(2048, 4097), Scale(2, 0), {8192, 8192}));
    REQUIRE(ScaledImageFits(Image(5461, 5461), Scale(3, 1), {8192, 8192}));
    REQUIRE_FALSE(ScaledImageFits(Image(5462, 5462), Scale(3, 1), {8192, 8192}));
    // Only 2D images scale their height.
    REQUIRE(ScaledImageFits(Image(4096, 16384, ImageType::e1D), Scale(2, 0), {16384, 16384}));
    REQUIRE_FALSE(ScaledImageFits(Image(4096, 16384, ImageType::e2D), Scale(2, 0), {16384, 16384}));
    // A 4x MSAA 1920x1080 target is a 3840x2160 sample grid in a 1920x1080 Vulkan image.
    ImageInfo msaa = Image(3840, 2160);
    msaa.num_samples = 4;
    REQUIRE(ScaledImageFits(msaa, Scale(2, 0), {4096, 4096}));
    REQUIRE_FALSE(ScaledImageFits(msaa, Scale(3, 0), {4096, 4096}));
    ImageInfo msaa_2x = Image(4096, 1024);
    msaa_2x.num_samples = 2;
    REQUIRE(ScaledImageFits(msaa_2x, Scale(2, 0), {4096, 4096}));
    REQUIRE_FALSE(ScaledImageFits(msaa_2x, Scale(2, 0), {4095, 4095}));
    ImageInfo tall_2x = Image(1024, 4096);
    tall_2x.num_samples = 2;
    REQUIRE_FALSE(ScaledImageFits(tall_2x, Scale(2, 0), {4096, 4096}));
    // Whatever the device reports, the sample grid stays within 16384 per side.
    REQUIRE(ScaledImageFits(Image(8192, 8192), Scale(2, 0), {65536, 65536}));
    REQUIRE_FALSE(ScaledImageFits(Image(8193, 8192), Scale(2, 0), {65536, 65536}));
    REQUIRE_FALSE(ScaledImageFits(Image(8192, 8193), Scale(2, 0), {65536, 65536}));
    REQUIRE_FALSE(ScaledImageFits(msaa, Scale(5, 0), {65536, 65536}));
}

TEST_CASE("Downscaling never shrinks an MSAA image below one texel", "[video_core][texture_cache]") {
    ImageInfo msaa = Image(4, 4);
    msaa.num_samples = 4;
    REQUIRE(ScaledImageFits(msaa, Scale(1, 1), {16384, 16384})); // 2x2 sample grid, 1x1 texels
    msaa.size = {2, 4, 1};
    REQUIRE_FALSE(ScaledImageFits(msaa, Scale(1, 1), {16384, 16384})); // 0 texels wide
    msaa.size = {4, 2, 1};
    REQUIRE_FALSE(ScaledImageFits(msaa, Scale(1, 1), {16384, 16384})); // 0 texels high
}
