// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/present/hdr_convert.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

using Settings::HdrSwapchain;
using Vulkan::ChooseHdrSurfaceFormat;

namespace {

constexpr VkSurfaceFormatKHR Sdr{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
constexpr VkSurfaceFormatKHR Hdr10{VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                   VK_COLOR_SPACE_HDR10_ST2084_EXT};
constexpr VkSurfaceFormatKHR Scrgb{VK_FORMAT_R16G16B16A16_SFLOAT,
                                   VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT};

bool Same(const std::optional<VkSurfaceFormatKHR>& a, const VkSurfaceFormatKHR& b) {
    return a && a->format == b.format && a->colorSpace == b.colorSpace;
}

} // Anonymous namespace

TEST_CASE("HDR output takes the preferred pair the surface lists", "[hdr_output]") {
    const std::array both{Sdr, Scrgb, Hdr10};
    REQUIRE(Same(ChooseHdrSurfaceFormat(both, HdrSwapchain::Hdr10), Hdr10));
    REQUIRE(Same(ChooseHdrSurfaceFormat(both, HdrSwapchain::Scrgb), Scrgb));
}

TEST_CASE("HDR output falls back to the other listed pair", "[hdr_output]") {
    const std::array scrgb_only{Sdr, Scrgb};
    REQUIRE(Same(ChooseHdrSurfaceFormat(scrgb_only, HdrSwapchain::Hdr10), Scrgb));
    const std::array hdr10_only{Sdr, Hdr10};
    REQUIRE(Same(ChooseHdrSurfaceFormat(hdr10_only, HdrSwapchain::Scrgb), Hdr10));
}

TEST_CASE("HDR output needs the exact format and colour space pair", "[hdr_output]") {
    // A 10-bit SDR format or an HDR10 colour space on another format is no HDR10 pair.
    const std::array mismatched{
        Sdr,
        VkSurfaceFormatKHR{VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_HDR10_ST2084_EXT},
    };
    REQUIRE_FALSE(ChooseHdrSurfaceFormat(mismatched, HdrSwapchain::Hdr10));
    REQUIRE_FALSE(ChooseHdrSurfaceFormat(std::array{Sdr}, HdrSwapchain::Scrgb));
}

TEST_CASE("Only the HDR10 and scRGB pairs count as an HDR swapchain", "[hdr_output]") {
    using Vulkan::IsHdrSurfaceFormat;
    REQUIRE(IsHdrSurfaceFormat(Hdr10));
    REQUIRE(IsHdrSurfaceFormat(Scrgb));
    REQUIRE_FALSE(IsHdrSurfaceFormat(Sdr));
    // Another wide colour space, or an HDR colour space on another format, is not presented as
    // HDR, so the guest must not be told HDR for it.
    REQUIRE_FALSE(IsHdrSurfaceFormat(
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT}));
    REQUIRE_FALSE(IsHdrSurfaceFormat(
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}));
    REQUIRE_FALSE(
        IsHdrSurfaceFormat({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_HDR10_ST2084_EXT}));
    REQUIRE_FALSE(IsHdrSurfaceFormat(
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_HDR10_ST2084_EXT}));
    REQUIRE_FALSE(IsHdrSurfaceFormat(
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT}));
}

TEST_CASE("An HDR frame is only shown on a swapchain of its own colour space", "[hdr_output]") {
    using Vulkan::FrameFitsSwapchain;
    constexpr auto sdr = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    constexpr auto hdr10 = VK_COLOR_SPACE_HDR10_ST2084_EXT;
    constexpr auto scrgb = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
    REQUIRE(FrameFitsSwapchain(sdr, sdr));
    REQUIRE(FrameFitsSwapchain(hdr10, hdr10));
    REQUIRE(FrameFitsSwapchain(scrgb, scrgb));
    // The surface stopped listing the pair and the swapchain was created as SDR.
    REQUIRE_FALSE(FrameFitsSwapchain(hdr10, sdr));
    REQUIRE_FALSE(FrameFitsSwapchain(scrgb, sdr));
    REQUIRE_FALSE(FrameFitsSwapchain(hdr10, scrgb));
    REQUIRE_FALSE(FrameFitsSwapchain(scrgb, hdr10));
}

TEST_CASE("Layers are converted only where their encoding differs from the frame's",
          "[hdr_output]") {
    using Vulkan::HdrEncoding;
    using Vulkan::HdrPresent;
    Tegra::FramebufferConfig layer{};
    const HdrPresent sdr{};
    const HdrPresent hdr10{.color_space = VK_COLOR_SPACE_HDR10_ST2084_EXT};
    const HdrPresent scrgb{.color_space = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT};

    layer.color_space = Tegra::ColorSpace::RGB709;
    REQUIRE_FALSE(Vulkan::NeedsHdrConvert(Vulkan::MakeHdrConvertParams(layer, sdr)));
    const auto sdr_on_hdr10 = Vulkan::MakeHdrConvertParams(layer, hdr10);
    REQUIRE(Vulkan::NeedsHdrConvert(sdr_on_hdr10));
    REQUIRE(sdr_on_hdr10.input == HdrEncoding::Srgb);
    REQUIRE(sdr_on_hdr10.output == HdrEncoding::Pq);

    layer.color_space = Tegra::ColorSpace::RGB2020_PQ;
    REQUIRE_FALSE(Vulkan::NeedsHdrConvert(Vulkan::MakeHdrConvertParams(layer, hdr10)));
    REQUIRE(Vulkan::NeedsHdrConvert(Vulkan::MakeHdrConvertParams(layer, sdr)));
    REQUIRE(Vulkan::NeedsHdrConvert(Vulkan::MakeHdrConvertParams(layer, scrgb)));

    layer.color_space = Tegra::ColorSpace::RGB709_Linear;
    layer.linear_unit_luminance = 0; // unknown: the scRGB 80 nits
    REQUIRE_FALSE(Vulkan::NeedsHdrConvert(Vulkan::MakeHdrConvertParams(layer, scrgb)));
    layer.linear_unit_luminance = 100;
    const auto rescaled = Vulkan::MakeHdrConvertParams(layer, scrgb);
    REQUIRE(Vulkan::NeedsHdrConvert(rescaled));
    REQUIRE(rescaled.nits_per_unit == 100.0f);
}

TEST_CASE("Layer conversions carry the luminances the game was told", "[hdr_output]") {
    using Vulkan::HdrEncoding;
    Tegra::FramebufferConfig layer{};
    layer.color_space = Tegra::ColorSpace::RGB2020_PQ;
    const Vulkan::HdrPresent sdr{.reference_white = 240.0f, .peak = 800.0f};
    const auto tone_mapped = Vulkan::MakeHdrConvertParams(layer, sdr);
    REQUIRE(tone_mapped.input == HdrEncoding::Pq);
    REQUIRE(tone_mapped.output == HdrEncoding::Srgb);
    REQUIRE(tone_mapped.reference_white == 240.0f);
    REQUIRE(tone_mapped.peak == 800.0f);
    REQUIRE(tone_mapped.nits_per_unit == 80.0f);
    REQUIRE_FALSE(tone_mapped.premultiplied);
}

TEST_CASE("Premultiplied layers stay premultiplied across the conversion", "[hdr_output]") {
    Tegra::FramebufferConfig layer{};
    const Vulkan::HdrPresent hdr10{.color_space = VK_COLOR_SPACE_HDR10_ST2084_EXT};
    layer.blending = Tegra::BlendMode::Premultiplied;
    REQUIRE(Vulkan::MakeHdrConvertParams(layer, hdr10).premultiplied);
    layer.blending = Tegra::BlendMode::Coverage;
    REQUIRE_FALSE(Vulkan::MakeHdrConvertParams(layer, hdr10).premultiplied);
    layer.blending = Tegra::BlendMode::Opaque;
    REQUIRE_FALSE(Vulkan::MakeHdrConvertParams(layer, hdr10).premultiplied);
}

TEST_CASE("Frame and layer colour spaces map to the shader's encodings", "[hdr_output]") {
    using Vulkan::EncodingOf;
    using Vulkan::HdrEncoding;
    REQUIRE(EncodingOf(VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) == HdrEncoding::Srgb);
    REQUIRE(EncodingOf(VK_COLOR_SPACE_HDR10_ST2084_EXT) == HdrEncoding::Pq);
    REQUIRE(EncodingOf(VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) == HdrEncoding::Scrgb);
    REQUIRE(EncodingOf(VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT) == HdrEncoding::Srgb);
    REQUIRE(EncodingOf(Tegra::ColorSpace::RGB709) == HdrEncoding::Srgb);
    REQUIRE(EncodingOf(Tegra::ColorSpace::RGB2020_PQ) == HdrEncoding::Pq);
    REQUIRE(EncodingOf(Tegra::ColorSpace::RGB709_Linear) == HdrEncoding::Scrgb);
    REQUIRE_FALSE(Vulkan::HdrPresent{}.IsHdr());
    REQUIRE(Vulkan::HdrPresent{.color_space = VK_COLOR_SPACE_HDR10_ST2084_EXT}.IsHdr());
    REQUIRE(Vulkan::HdrPresent{.color_space = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT}.IsHdr());
}

TEST_CASE("The background colour sits at the reference white on a linear HDR target",
          "[hdr_output]") {
    using Vulkan::BackgroundClearColor;
    const std::array<f32, 3> white{1.0f, 1.0f, 1.0f};
    const std::array<f32, 3> mixed{0.0f, 0.5f, 0.03f};
    // SDR and PQ targets take the colour as it is (an HDR10 frame is composited in scRGB).
    REQUIRE(BackgroundClearColor(Vulkan::HdrPresent{}, mixed) == mixed);
    REQUIRE(BackgroundClearColor({.color_space = VK_COLOR_SPACE_HDR10_ST2084_EXT}, mixed) ==
            mixed);
    const Vulkan::HdrPresent scrgb{.color_space = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT,
                                   .reference_white = 200.0f};
    // 200 nits over scRGB's 80 nits per 1.0.
    REQUIRE(BackgroundClearColor(scrgb, white) == std::array<f32, 3>{2.5f, 2.5f, 2.5f});
    const auto linear = BackgroundClearColor(scrgb, mixed);
    REQUIRE(linear[0] == 0.0f);
    REQUIRE(std::abs(linear[1] - 0.214041f * 2.5f) < 1e-4f);
    REQUIRE(std::abs(linear[2] - 0.03f / 12.92f * 2.5f) < 1e-6f);
}

TEST_CASE("A native FP16 image is detiled only when its blocks fit its allocation",
          "[hdr_output]") {
    using Tegra::Fp16DetileBlockHeight;
    Tegra::FramebufferConfig image{};
    image.width = 256;
    image.height = 64;
    image.stride = 256;
    image.pixel_format = Service::android::PixelFormat::RgbaFp16;
    // 16 rows per block: 2048 bytes a row, 64 rows.
    image.block_height_log2 = 1;
    image.allocation_size = 2048 * 64;
    REQUIRE(Fp16DetileBlockHeight(image) == 1U);
    // One byte short of its rows.
    image.allocation_size = 2048 * 64 - 1;
    REQUIRE_FALSE(Fp16DetileBlockHeight(image));
    // 128 rows per block round the 64 rows up to 128.
    image.block_height_log2 = 4;
    image.allocation_size = 2048 * 64;
    REQUIRE_FALSE(Fp16DetileBlockHeight(image));
    image.allocation_size = 2048 * 128;
    REQUIRE(Fp16DetileBlockHeight(image) == 4U);
    // An unknown allocation is not checked; 256 rows per block is the largest.
    image.allocation_size = 0;
    image.block_height_log2 = 5;
    REQUIRE(Fp16DetileBlockHeight(image) == 5U);
    image.block_height_log2 = 6;
    REQUIRE_FALSE(Fp16DetileBlockHeight(image));
    // A block height below one GOB has no value.
    image.block_height_log2 = std::nullopt;
    REQUIRE_FALSE(Fp16DetileBlockHeight(image));
    // Rows are padded to whole 64-byte GOBs: 250 pixels need 2000 bytes, 2048 with padding.
    image.block_height_log2 = 0;
    image.stride = 250;
    image.height = 8;
    image.allocation_size = 2048 * 8;
    REQUIRE(Fp16DetileBlockHeight(image) == 0U);
    image.allocation_size = 2000 * 8;
    REQUIRE_FALSE(Fp16DetileBlockHeight(image));
    // Sizes are 64-bit and capped below 4 GiB (CalculateSize computes in 32 bits): 65536 pixels
    // are 2^19 bytes a row.
    image.stride = 65536;
    image.height = 8184;
    image.allocation_size = u64{1} << 35;
    REQUIRE(Fp16DetileBlockHeight(image) == 0U);
    image.allocation_size = (u64{1} << 19) * 8184 - 1;
    REQUIRE_FALSE(Fp16DetileBlockHeight(image));
    image.allocation_size = 0;
    image.height = 8192;
    REQUIRE_FALSE(Fp16DetileBlockHeight(image));
    image.height = 65536;
    image.allocation_size = u64{1} << 35;
    REQUIRE_FALSE(Fp16DetileBlockHeight(image));
}
