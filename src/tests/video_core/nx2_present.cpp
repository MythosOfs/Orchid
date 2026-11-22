// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/nx2_present_queue.h"
#include "video_core/host1x/host1x.h"

#include "core/hle/service/nx2_present.h"
#include "video_core/engines/native_upscale.h"

TEST_CASE("Nx2 presentation crops preserve the native allocation extent", "[nx2_video]") {
    const auto crop = Service::Nx2PresentCrop(0, 1279 | (719 << 16), 2, 3840, 2160);
    CHECK(crop.left == 0);
    CHECK(crop.top == 0);
    CHECK(crop.right == 1280);
    CHECK(crop.bottom == 720);
    const auto disabled = Service::Nx2PresentCrop(0, 1279 | (719 << 16), 1, 3840, 2160);
    CHECK(disabled.right == 3840);
    CHECK(disabled.bottom == 2160);
}

TEST_CASE("Nx2 presentation crops decode inclusive coordinates and clip to the image", "[nx2_video]") {
    const auto crop = Service::Nx2PresentCrop(16 | (32 << 16), 4095 | (4095 << 16), 2, 3840, 2160);
    CHECK(crop.left == 16);
    CHECK(crop.top == 32);
    CHECK(crop.right == 3840);
    CHECK(crop.bottom == 2160);
    const auto pixel = Service::Nx2PresentCrop(16 | (32 << 16), 16 | (32 << 16), 2, 3840, 2160);
    CHECK(pixel.GetWidth() == 1);
    CHECK(pixel.GetHeight() == 1);
    const auto maximum = Service::Nx2PresentCrop(0, 0xffffffff, 2, 65536, 65536);
    CHECK(maximum.right == 65536);
    CHECK(maximum.bottom == 65536);
}

TEST_CASE("Nx2 presentation ignores empty or reversed crop metadata", "[nx2_video]") {
    for (const u32 origin : {3840U, 2160U << 16, 32U | (32U << 16)}) {
        const auto crop = Service::Nx2PresentCrop(origin, 15 | (15 << 16), 2, 3840, 2160);
        CHECK(crop.left == 0);
        CHECK(crop.top == 0);
        CHECK(crop.right == 3840);
        CHECK(crop.bottom == 2160);
    }
}

TEST_CASE("Nx2 presentation honors vertical inversion independently of crop flags", "[nx2_video]") {
    using Service::android::BufferTransformFlags;
    CHECK(Service::Nx2PresentTransform(0x00040003) == BufferTransformFlags::FlipV);
    CHECK(Service::Nx2PresentTransform(0x00040000) == BufferTransformFlags::FlipV);
    CHECK(Service::Nx2PresentTransform(0x00000003) == BufferTransformFlags::Unset);
    CHECK(Service::Nx2PresentTransform(0) == BufferTransformFlags::Unset);
}

TEST_CASE("Native upscale uses active regions inside larger texture allocations", "[nx2_video]") {
    const std::array<u32, 12> parameters{128, 40, 23, 1, 0, 0, 959, 539, 0, 0, 1919, 1079};
    const auto regions = Tegra::Engines::DecodeNativeUpscaleRegions(
        parameters, {128, 1, 1}, {40, 23, 1}, {960, 540}, {3840, 2160});
    REQUIRE(regions);
    CHECK(regions->source == std::array<u32, 4>{0, 0, 960, 540});
    CHECK(regions->destination == std::array<u32, 4>{0, 0, 1920, 1080});

    auto offset = parameters;
    offset[4] = 16;
    offset[5] = 8;
    offset[8] = 32;
    offset[9] = 16;
    const auto subregion = Tegra::Engines::DecodeNativeUpscaleRegions(
        offset, {128, 1, 1}, {40, 23, 1}, {960, 540}, {3840, 2160});
    REQUIRE(subregion);
    CHECK(subregion->source[0] == 16);
    CHECK(subregion->destination[1] == 16);

    const std::array<u32, 12> xenoblade{160, 40, 15, 1, 0, 0, 1279, 719, 0, 0, 1919, 1079};
    const auto handheld = Tegra::Engines::DecodeNativeUpscaleRegions(
        xenoblade, {128, 1, 1}, {40, 15, 1}, {1280, 720}, {3840, 2160});
    REQUIRE(handheld);
    CHECK(handheld->source == std::array<u32, 4>{0, 0, 1280, 720});
    CHECK(handheld->destination == std::array<u32, 4>{0, 0, 1920, 1080});
}

TEST_CASE("Native upscale rejects incompatible or invalid region parameters", "[nx2_video]") {
    const std::array<u32, 12> parameters{128, 40, 23, 1, 0, 0, 959, 539, 0, 0, 1919, 1079};
    CHECK_FALSE(Tegra::Engines::DecodeNativeUpscaleRegions(
        parameters, {128, 1, 1}, {41, 23, 1}, {960, 540}, {3840, 2160}));
    CHECK_FALSE(Tegra::Engines::DecodeNativeUpscaleRegions(
        parameters, {64, 2, 1}, {40, 23, 1}, {960, 540}, {3840, 2160}));
    CHECK_FALSE(Tegra::Engines::DecodeNativeUpscaleRegions(
        parameters, {64, 1, 2}, {40, 23, 1}, {960, 540}, {3840, 2160}));
    CHECK_FALSE(Tegra::Engines::DecodeNativeUpscaleRegions(
        std::span{parameters}.first(11), {128, 1, 1}, {40, 23, 1}, {960, 540}, {3840, 2160}));
    for (const std::pair<std::size_t, u32> invalid : {
             std::pair<std::size_t, u32>{6, 960}, {7, 540}, {10, 3840}, {11, 2160},
             {10, 0xffffffff}, {4, 960}, {8, 1920}, {10, 100}}) {
        auto malformed = parameters;
        malformed[invalid.first] = invalid.second;
        CHECK_FALSE(Tegra::Engines::DecodeNativeUpscaleRegions(
            malformed, {128, 1, 1}, {40, 23, 1}, {960, 540}, {3840, 2160}));
    }
}

TEST_CASE("Native upscale accepts only the formats it can copy", "[nx2_video]") {
    using Tegra::Engines::IsNativeUpscaleFormat;
    using Tegra::Texture::TextureFormat;
    CHECK(IsNativeUpscaleFormat(TextureFormat::A2B10G10R10));
    CHECK(IsNativeUpscaleFormat(TextureFormat::A8B8G8R8));
    CHECK(IsNativeUpscaleFormat(TextureFormat::B10G11R11));
    CHECK(IsNativeUpscaleFormat(TextureFormat::R16G16B16A16));
    CHECK_FALSE(IsNativeUpscaleFormat(TextureFormat::BC7U));
    CHECK_FALSE(IsNativeUpscaleFormat(TextureFormat::DXN1));
    CHECK_FALSE(IsNativeUpscaleFormat(TextureFormat::R16G16));
}

TEST_CASE("Bananza fixed-refresh pacing caps long swap intervals", "[nx2_present_pacing]") {
    using Service::Nx2PresentSwapInterval;
    for (u32 refresh : {30U, 40U, 60U, 120U}) {
        for (u32 requested : {0U, 1U, 2U, 3U, 4U, 5U}) {
            const u32 timing = (requested << 16) | refresh;
            CAPTURE(timing);
            CHECK(Nx2PresentSwapInterval(timing, true, 0, true) ==
                  std::clamp(requested, 1U, 2U));
            CHECK(Nx2PresentSwapInterval(timing, true, 0, false) ==
                  std::clamp(requested, 1U, 4U));
            CHECK(Nx2PresentSwapInterval(timing, false, 0, true) == 1);
            CHECK(Nx2PresentSwapInterval(timing, true, 3, true) == 3);
        }
    }
}

TEST_CASE("NX2 present releases wait for composition and deadline", "[nx2_present_queue]") {
    using namespace std::chrono_literals;
    Core::Timing::CoreTiming timing;
    timing.SetMulticore(false);
    timing.Initialize([] {});
    Tegra::Host1x::SyncpointManager syncpoints;
    constexpr u32 id = 12;
    const auto queue = std::make_shared<Service::Nx2PresentReleaseQueue>(timing, syncpoints, id);
    const auto release = queue->Queue(5ms);
    REQUIRE(release);
    CHECK(release->value == 1);
    CHECK(release->syncpoint == id);
    CHECK(queue->RunAhead() > 0ns);
    SECTION("deadline first") {
        timing.AddTicks(1'000'000'000);
        timing.Advance();
        CHECK(syncpoints.GetGuestSyncpointValue(id) == 0);
        release->on_composited();
    }
    SECTION("composition first") {
        release->on_composited();
        timing.Advance();
        CHECK(syncpoints.GetGuestSyncpointValue(id) == 0);
        timing.AddTicks(1'000'000'000);
    }
    timing.Advance();
    CHECK(syncpoints.GetGuestSyncpointValue(id) == 1);
    CHECK(queue->RunAhead() == 0ns);
}

TEST_CASE("NX2 present queue bounds releases, signals in order and drains on stop",
          "[nx2_present_queue]") {
    using namespace std::chrono_literals;
    Core::Timing::CoreTiming timing;
    timing.SetMulticore(false);
    timing.Initialize([] {});
    Tegra::Host1x::SyncpointManager syncpoints;
    constexpr u32 id = 12;
    const auto queue = std::make_shared<Service::Nx2PresentReleaseQueue>(timing, syncpoints, id);
    syncpoints.IncrementGuest(id + 1);
    const auto first = queue->Queue(0ns);
    REQUIRE(first);
    std::vector<u32> order;
    for (u32 value = 1; value <= Service::Nx2PresentReleaseQueue::Capacity; ++value) {
        syncpoints.RegisterGuestAction(id, value, [&, value] { order.push_back(value); });
        if (value > 1) {
            const auto later = queue->Queue(0ns);
            REQUIRE(later);
            CHECK(later->value == value);
            later->on_composited();
        }
    }
    CHECK_FALSE(queue->Queue(0ns));
    timing.Advance();
    CHECK(order.empty());
    first->on_composited();
    timing.Advance();
    REQUIRE(order.size() == Service::Nx2PresentReleaseQueue::Capacity);
    CHECK(std::ranges::is_sorted(order));
    const auto last = queue->Queue(120ms);
    REQUIRE(last);
    CHECK(last->value == order.size() + 1);
    queue->Stop();
    CHECK(syncpoints.GetGuestSyncpointValue(id) == last->value);
    last->on_composited();
    CHECK_FALSE(queue->Queue(0ns));
    CHECK_FALSE(timing.Advance());
    CHECK(syncpoints.GetGuestSyncpointValue(id + 1) == 1);
}

TEST_CASE("NX2 release syncpoint exhaustion falls back and permits safe reuse",
          "[nx2_present_queue]") {
    const bool saved_multi_core = Settings::values.use_multi_core.GetValue();
    SCOPE_EXIT { Settings::values.use_multi_core.SetValue(saved_multi_core); };
    Settings::values.use_multi_core.SetValue(false);
    Core::System system;
    system.Initialize();
    auto host = std::make_unique<Tegra::Host1x::Host1x>(system);
    Service::Nvidia::NvCore::SyncpointManager allocator{*host};
    std::vector<u32> ids;
    while (const auto id = allocator.TryAllocateSyncpoint(true)) {
        REQUIRE(*id != 0);
        REQUIRE(ids.size() < Tegra::Host1x::SyncpointManager::NUM_MAX_SYNCPOINTS);
        CHECK(allocator.IsSyncpointAllocated(*id));
        ids.push_back(*id);
    }
    REQUIRE_FALSE(ids.empty());
    CHECK_FALSE(allocator.TryAllocateSyncpoint(true));
    CHECK_FALSE(allocator.IsSyncpointAllocated(0));
    allocator.FreeSyncpoint(ids.back());
    CHECK(allocator.TryAllocateSyncpoint(true) == ids.back());
    CHECK_FALSE(allocator.TryAllocateSyncpoint(true));
}

TEST_CASE("NX2 present continues a reused syncpoint and ignores late completions",
          "[nx2_present_queue]") {
    using namespace std::chrono_literals;
    Core::Timing::CoreTiming timing;
    timing.SetMulticore(false);
    timing.Initialize([] {});
    Tegra::Host1x::SyncpointManager syncpoints;
    constexpr u32 id = 12;
    syncpoints.IncrementGuest(id);
    std::function<void()> late;
    {
        const auto queue = std::make_shared<Service::Nx2PresentReleaseQueue>(timing, syncpoints, id);
        const auto release = queue->Queue(-5ms);
        REQUIRE(release);
        CHECK(release->value == 2);
        CHECK(queue->RunAhead() == 0ns);
        late = release->on_composited;
        syncpoints.IncrementGuest(id);
        syncpoints.IncrementGuest(id);
    }
    late();
    CHECK(syncpoints.GetGuestSyncpointValue(id) == 3);
    CHECK_FALSE(timing.Advance());
}
