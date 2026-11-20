// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_image_layout.h"

namespace {

constexpr std::array<u32, 16> ColorPreference{
    0, 2, (2160U - 1) << 16 | (3840U - 1), 0, 7, 240, 1, 1,
};

}

TEST_CASE("Native RGBA8 storage keeps its four-byte geometry", "[nx2_image_layout]") {
    const auto layout = Service::Nx2CalculateImageLayout(ColorPreference.data());
    REQUIRE(layout.width == 3840);
    REQUIRE(layout.height == 2160);
    REQUIRE(layout.pitch == 0x3C00);
    REQUIRE(layout.block_height_log2 == 7);
    REQUIRE(layout.size == 0x1FE0000);
    REQUIRE(layout.alignment == 0x1000);
}

TEST_CASE("Native image layout takes block heights up to 256 rows", "[nx2_image_layout]") {
    auto preference = ColorPreference;
    preference[4] = 8;
    const auto tall = Service::Nx2CalculateImageLayout(preference.data());
    REQUIRE(tall.pitch == 0x3C00);
    REQUIRE(tall.block_height_log2 == 8);
    REQUIRE(tall.size == 0x3C00 * 2304);
    preference[4] = 4;
    const auto low = Service::Nx2CalculateImageLayout(preference.data());
    REQUIRE(low.block_height_log2 == 4);
    REQUIRE(low.size == 0x3C00 * 2160);
    preference[4] = 9;
    const auto rejected = Service::Nx2CalculateImageLayout(preference.data());
    REQUIRE(rejected.block_height_log2 == 7);
    REQUIRE(rejected.size == 0x1FE0000);
}

TEST_CASE("Native image layout takes pitches from the row size up to 4096 GOBs",
          "[nx2_image_layout]") {
    auto preference = ColorPreference;
    preference[4] = 3;
    preference[5] = 239;
    REQUIRE(Service::Nx2CalculateImageLayout(preference.data()).block_height_log2 == 7);
    preference[5] = 4096;
    const auto widest = Service::Nx2CalculateImageLayout(preference.data());
    REQUIRE(widest.pitch == 4096 * 64);
    REQUIRE(widest.block_height_log2 == 3);
    preference[5] = 4097;
    const auto too_wide = Service::Nx2CalculateImageLayout(preference.data());
    REQUIRE(too_wide.pitch == 0x3C00);
    REQUIRE(too_wide.block_height_log2 == 7);
}

TEST_CASE("Native RGB10A2 storage preserves four-byte geometry", "[nx2_image_layout]") {
    auto preference = ColorPreference;
    preference[6] = 4;
    const auto layout = Service::Nx2CalculateImageLayout(preference.data());
    REQUIRE(layout.width == 3840);
    REQUIRE(layout.height == 2160);
    REQUIRE(layout.pitch == 0x3C00);
    REQUIRE(layout.block_height_log2 == 7);
    REQUIRE(layout.size == 0x1FE0000);
    REQUIRE(layout.alignment == 0x1000);
}

TEST_CASE("Native FP16 storage uses eight-byte geometry", "[nx2_image_layout]") {
    auto preference = ColorPreference;
    preference[6] = 5;
    preference[5] = 480;
    const auto layout = Service::Nx2CalculateImageLayout(preference.data());
    REQUIRE(layout.width == 3840);
    REQUIRE(layout.height == 2160);
    REQUIRE(layout.pitch == 0x7800);
    REQUIRE(layout.block_height_log2 == 7);
    REQUIRE(layout.size == 0x3FC0000);
    REQUIRE(layout.alignment == 0x1000);
}

TEST_CASE("Native FP16 rejects a pitch sized for four-byte pixels", "[nx2_image_layout]") {
    auto preference = ColorPreference;
    preference[6] = 5;
    preference[4] = 3;
    const auto layout = Service::Nx2CalculateImageLayout(preference.data());
    REQUIRE(layout.pitch == 0x7800);
    REQUIRE(layout.block_height_log2 == 7);
    REQUIRE(layout.size == 0x3FC0000);
}

TEST_CASE("Native FP16 accepts the smallest pitch that holds its row", "[nx2_image_layout]") {
    auto preference = ColorPreference;
    preference[6] = 5;
    preference[4] = 3;
    preference[5] = 480;
    const auto exact = Service::Nx2CalculateImageLayout(preference.data());
    REQUIRE(exact.pitch == 0x7800);
    REQUIRE(exact.block_height_log2 == 3);
    preference[5] = 479;
    const auto short_row = Service::Nx2CalculateImageLayout(preference.data());
    REQUIRE(short_row.pitch == 0x7800);
    REQUIRE(short_row.block_height_log2 == 7);
}

TEST_CASE("Native image colour space follows the HDR answer, not the format",
          "[nx2_image_layout]") {
    using Service::Nx2NativeImageColorSpace;
    using Service::Nx2NativeImageFormat;
    // Fortune's Weave: SDR in RGB10A2 while told SDR.
    REQUIRE(Nx2NativeImageColorSpace(Nx2NativeImageFormat::Rgb10A2, false) ==
            Tegra::ColorSpace::RGB709);
    REQUIRE(Nx2NativeImageColorSpace(Nx2NativeImageFormat::Rgb10A2, true) ==
            Tegra::ColorSpace::RGB2020_PQ);
    REQUIRE(Nx2NativeImageColorSpace(Nx2NativeImageFormat::Rgba8, true) ==
            Tegra::ColorSpace::RGB709);
    REQUIRE(Nx2NativeImageColorSpace(Nx2NativeImageFormat::Rgba8, false) ==
            Tegra::ColorSpace::RGB709);
    REQUIRE(Nx2NativeImageColorSpace(Nx2NativeImageFormat::Rgba16Float, false) ==
            Tegra::ColorSpace::RGB709_Linear);
    REQUIRE(Nx2NativeImageColorSpace(Nx2NativeImageFormat::Rgba16Float, true) ==
            Tegra::ColorSpace::RGB709_Linear);
}
