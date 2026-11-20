// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/display_hdr.h"

using Common::DisplayHdrInfo;
using Common::ResolveHdrOutput;
using Settings::HdrMode;

TEST_CASE("HDR Auto follows the monitor's HDR state", "[display_hdr]") {
    const DisplayHdrInfo hdr{
        .hdr_enabled = true, .max_luminance = 1000.0f, .sdr_white_level = 240.0f};
    const DisplayHdrInfo sdr{
        .hdr_enabled = false, .max_luminance = 270.0f, .sdr_white_level = 80.0f};
    REQUIRE(ResolveHdrOutput(hdr, HdrMode::Auto, 0, 0).enabled);
    REQUIRE_FALSE(ResolveHdrOutput(sdr, HdrMode::Auto, 0, 0).enabled);
    // A host that cannot tell gets SDR.
    REQUIRE_FALSE(ResolveHdrOutput(std::nullopt, HdrMode::Auto, 0, 0).enabled);
}

TEST_CASE("HDR On and Off ignore the monitor", "[display_hdr]") {
    const DisplayHdrInfo hdr{
        .hdr_enabled = true, .max_luminance = 1000.0f, .sdr_white_level = 240.0f};
    const DisplayHdrInfo sdr{
        .hdr_enabled = false, .max_luminance = 270.0f, .sdr_white_level = 80.0f};
    REQUIRE(ResolveHdrOutput(sdr, HdrMode::On, 0, 0).enabled);
    REQUIRE(ResolveHdrOutput(std::nullopt, HdrMode::On, 0, 0).enabled);
    REQUIRE_FALSE(ResolveHdrOutput(hdr, HdrMode::Off, 0, 0).enabled);
    REQUIRE_FALSE(ResolveHdrOutput(std::nullopt, HdrMode::Off, 0, 0).enabled);
}

TEST_CASE("HDR luminance of 0 follows the monitor", "[display_hdr]") {
    const DisplayHdrInfo display{
        .hdr_enabled = true, .max_luminance = 1000.4f, .sdr_white_level = 240.0f};
    const auto output = ResolveHdrOutput(display, HdrMode::Auto, 0, 0);
    REQUIRE(output.peak_luminance == 1000);
    REQUIRE(output.reference_white == 240);
}

TEST_CASE("HDR luminance settings override the monitor", "[display_hdr]") {
    const DisplayHdrInfo display{
        .hdr_enabled = true, .max_luminance = 1000.0f, .sdr_white_level = 240.0f};
    const auto output = ResolveHdrOutput(display, HdrMode::Auto, 600, 900);
    REQUIRE(output.peak_luminance == 600);
    REQUIRE(output.reference_white == 600); // capped at peak
}

TEST_CASE("HDR luminance without a monitor uses the Switch 2 calibration", "[display_hdr]") {
    const auto output = ResolveHdrOutput(std::nullopt, HdrMode::Auto, 0, 0);
    REQUIRE(output.peak_luminance == 300);
    REQUIRE(output.reference_white == 200);
    REQUIRE(ResolveHdrOutput(std::nullopt, HdrMode::Auto, 30, 0).peak_luminance == 80);
    REQUIRE(ResolveHdrOutput(std::nullopt, HdrMode::Auto, 1000, 0).reference_white == 300);
}

TEST_CASE("HDR luminance the monitor does not report uses the Switch 2 calibration",
          "[display_hdr]") {
    const DisplayHdrInfo unknown{
        .hdr_enabled = true, .max_luminance = 0.0f, .sdr_white_level = 0.0f};
    const auto output = ResolveHdrOutput(unknown, HdrMode::Auto, 0, 0);
    REQUIRE(output.peak_luminance == 300);
    REQUIRE(output.reference_white == 200);
}

TEST_CASE("HDR peak luminance stays within 80 to 10000 nits", "[display_hdr]") {
    const DisplayHdrInfo bright{
        .hdr_enabled = true, .max_luminance = 20000.0f, .sdr_white_level = 240.0f};
    REQUIRE(ResolveHdrOutput(bright, HdrMode::Auto, 0, 0).peak_luminance == 10000);
    const DisplayHdrInfo dim{
        .hdr_enabled = true, .max_luminance = 1.0f, .sdr_white_level = 240.0f};
    REQUIRE(ResolveHdrOutput(dim, HdrMode::Auto, 0, 0).peak_luminance == 80);
    REQUIRE(ResolveHdrOutput(std::nullopt, HdrMode::On, 79, 0).peak_luminance == 80);
    REQUIRE(ResolveHdrOutput(std::nullopt, HdrMode::On, 80, 0).peak_luminance == 80);
    REQUIRE(ResolveHdrOutput(std::nullopt, HdrMode::On, 10000, 0).peak_luminance == 10000);
}

TEST_CASE("HDR reference white defaults to the peak divided by 1.5, rounded up",
          "[display_hdr]") {
    REQUIRE(ResolveHdrOutput(std::nullopt, HdrMode::On, 100, 0).reference_white == 67);
    REQUIRE(ResolveHdrOutput(std::nullopt, HdrMode::On, 80, 0).reference_white == 54);
    REQUIRE(ResolveHdrOutput(std::nullopt, HdrMode::On, 450, 0).reference_white == 300);
    REQUIRE(ResolveHdrOutput(std::nullopt, HdrMode::On, 451, 0).reference_white == 300);
}

TEST_CASE("The HDR monitor counts only real changes", "[display_hdr]") {
    Common::DisplayHdrMonitor monitor;
    REQUIRE_FALSE(monitor.Current().has_value());
    REQUIRE(monitor.Generation() == 0);
    const DisplayHdrInfo hdr{
        .hdr_enabled = true, .max_luminance = 1000.0f, .sdr_white_level = 240.0f};
    REQUIRE(monitor.Update(hdr));
    REQUIRE(monitor.Generation() == 1);
    REQUIRE_FALSE(monitor.Update(hdr)); // the same reading every 500 ms
    REQUIRE(monitor.Generation() == 1);
    // Windows HDR off, then a new calibration peak, then the window off every monitor.
    REQUIRE(monitor.Update(
        DisplayHdrInfo{.hdr_enabled = false, .max_luminance = 1000.0f, .sdr_white_level = 240.0f}));
    REQUIRE(monitor.Update(
        DisplayHdrInfo{.hdr_enabled = false, .max_luminance = 800.0f, .sdr_white_level = 240.0f}));
    REQUIRE(monitor.Update(std::nullopt));
    REQUIRE(monitor.Generation() == 4);
    REQUIRE_FALSE(monitor.Current().has_value());
}

TEST_CASE("HDR Auto switches with the monitor's reading", "[display_hdr]") {
    Common::DisplayHdrMonitor monitor;
    monitor.Update(
        DisplayHdrInfo{.hdr_enabled = true, .max_luminance = 1000.0f, .sdr_white_level = 240.0f});
    REQUIRE(ResolveHdrOutput(monitor.Current(), HdrMode::Auto, 0, 0).enabled);
    monitor.Update(
        DisplayHdrInfo{.hdr_enabled = false, .max_luminance = 1000.0f, .sdr_white_level = 240.0f});
    REQUIRE_FALSE(ResolveHdrOutput(monitor.Current(), HdrMode::Auto, 0, 0).enabled);
}

TEST_CASE("The guest is told HDR only while the swapchain presents HDR", "[display_hdr]") {
    using Common::ResolveGuestHdrOutput;
    const DisplayHdrInfo hdr{
        .hdr_enabled = true, .max_luminance = 1000.0f, .sdr_white_level = 240.0f};
    const DisplayHdrInfo sdr{
        .hdr_enabled = false, .max_luminance = 270.0f, .sdr_white_level = 80.0f};
    REQUIRE(ResolveGuestHdrOutput(hdr, HdrMode::Auto, 0, 0, true).enabled);
    // Windows HDR on, but the surface had no HDR pair: the swapchain stayed SDR.
    REQUIRE_FALSE(ResolveGuestHdrOutput(hdr, HdrMode::Auto, 0, 0, false).enabled);
    REQUIRE_FALSE(ResolveGuestHdrOutput(sdr, HdrMode::Auto, 0, 0, true).enabled);
    REQUIRE(ResolveGuestHdrOutput(sdr, HdrMode::On, 0, 0, true).enabled);
    REQUIRE_FALSE(ResolveGuestHdrOutput(sdr, HdrMode::On, 0, 0, false).enabled);
    REQUIRE_FALSE(ResolveGuestHdrOutput(hdr, HdrMode::Off, 0, 0, true).enabled);
    // An HDR answer carries the monitor's luminances.
    const auto hdr_answer = ResolveGuestHdrOutput(hdr, HdrMode::Auto, 0, 0, true);
    REQUIRE(hdr_answer.peak_luminance == 1000);
    REQUIRE(hdr_answer.reference_white == 240);
}

TEST_CASE("An SDR answer keeps the fixed SDR luminances", "[display_hdr]") {
    using Common::ResolveGuestHdrOutput;
    const DisplayHdrInfo hdr{
        .hdr_enabled = true, .max_luminance = 1000.0f, .sdr_white_level = 240.0f};
    // Off, the default: what every game was told before HDR output existed.
    const auto off = ResolveGuestHdrOutput(std::nullopt, HdrMode::Off, 0, 0, false);
    REQUIRE_FALSE(off.enabled);
    REQUIRE(off.peak_luminance == 400);
    REQUIRE(off.reference_white == 203);
    // Neither the monitor nor the luminance settings change an SDR answer.
    const auto fallback = ResolveGuestHdrOutput(hdr, HdrMode::On, 600, 150, false);
    REQUIRE_FALSE(fallback.enabled);
    REQUIRE(fallback.peak_luminance == 400);
    REQUIRE(fallback.reference_white == 203);
    const auto off_on_hdr = ResolveGuestHdrOutput(hdr, HdrMode::Off, 600, 150, true);
    REQUIRE(off_on_hdr.peak_luminance == 400);
    REQUIRE(off_on_hdr.reference_white == 203);
    // The settings reach the game once it is told HDR.
    const auto on = ResolveGuestHdrOutput(hdr, HdrMode::On, 600, 150, true);
    REQUIRE(on.enabled);
    REQUIRE(on.peak_luminance == 600);
    REQUIRE(on.reference_white == 150);
}

TEST_CASE("A swapchain HDR change counts as a display change", "[display_hdr]") {
    Common::DisplayHdrMonitor monitor;
    REQUIRE_FALSE(monitor.PresentingHdr());
    monitor.SetPresentingHdr(false);
    REQUIRE(monitor.Generation() == 0);
    monitor.SetPresentingHdr(true);
    REQUIRE(monitor.PresentingHdr());
    REQUIRE(monitor.Generation() == 1);
    monitor.SetPresentingHdr(true); // every swapchain recreation reports again
    REQUIRE(monitor.Generation() == 1);
    monitor.SetPresentingHdr(false);
    REQUIRE(monitor.Generation() == 2);
    // The monitor's own reading did not change.
    REQUIRE(monitor.DisplayGeneration() == 0);
    REQUIRE(monitor.Update(DisplayHdrInfo{.hdr_enabled = true}));
    REQUIRE(monitor.DisplayGeneration() == 1);
    REQUIRE(monitor.Generation() == 3);
    REQUIRE_FALSE(monitor.Update(DisplayHdrInfo{.hdr_enabled = true}));
    REQUIRE(monitor.DisplayGeneration() == 1);
}
