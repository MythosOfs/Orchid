// SPDX-FileCopyrightText: Copyright 2026 Orchid Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Per-title overrides in Core::GameSettings::LoadOverrides. Only the renderer's vendor string
// is read there, so minimal fakes stand in for the window and renderer.

#include <memory>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "common/settings.h"
#include "common/scope_exit.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/game_settings.h"
#include "video_core/renderer_base.h"

namespace {

template <typename T, bool ranged>
struct SettingSnapshot {
    Settings::SwitchableSetting<T, ranged>& setting;
    bool global;
    T global_value, custom_value;
    explicit SettingSnapshot(Settings::SwitchableSetting<T, ranged>& value)
        : setting{value}, global{value.UsingGlobal()}, global_value{value.GetValue(true)} {
        setting.SetGlobal(false);
        custom_value = setting.GetValue();
        setting.SetGlobal(global);
    }
    ~SettingSnapshot() {
        setting.SetGlobal(true);
        setting.SetValue(global_value);
        setting.SetGlobal(false);
        setting.SetValue(custom_value);
        setting.SetGlobal(global);
    }
};

class FakeWindow final : public Core::Frontend::EmuWindow {
public:
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return nullptr;
    }
    bool IsShown() const override {
        return true;
    }
};

class FakeRenderer final : public VideoCore::RendererBase {
public:
    explicit FakeRenderer(Core::Frontend::EmuWindow& window,
                          std::string vendor_ = "AMD Radeon RX 7600 (RADV NAVI33)",
                          bool nvidia_turing_or_newer_ = false)
        : RendererBase(window, nullptr), vendor{std::move(vendor_)},
          nvidia_turing_or_newer{nvidia_turing_or_newer_} {}
    void Composite(std::span<const Tegra::FramebufferConfig>) override {}
    std::vector<u8> GetAppletCaptureBuffer() override {
        return {};
    }
    VideoCore::RasterizerInterface* ReadRasterizer() override {
        return nullptr;
    }
    std::string GetDeviceVendor() const override {
        return vendor;
    }
    bool IsNvidiaTuringOrNewer() const override {
        return nvidia_turing_or_newer;
    }

private:
    std::string vendor;
    bool nvidia_turing_or_newer;
};

bool PreemptiveReadsFor(u64 program_id) {
    FakeWindow window;
    FakeRenderer renderer{window};
    Core::GameSettings::LoadOverrides(program_id, renderer);
    return Settings::values.preemptive_cpu_reads;
}

} // namespace

TEST_CASE("GameSettings: Xenoblade DE Switch 2 Edition skips ordered CPU reads", "[game_settings]") {
    // Title ID as the game reports it (not the enum), so a wrong ID in the table fails here.
    // The game reads a GPU-written texture every frame; ordered reads drained the host GPU.
    REQUIRE(PreemptiveReadsFor(0x0400FF500E34A001ULL));
    // The Switch 1 release keeps ordered reads.
    REQUIRE_FALSE(PreemptiveReadsFor(0x0100FF500E34A000ULL));
}

TEST_CASE("Orbitals uses coherent native defaults while preserving custom settings", "[game_settings]") {
    auto& accuracy = Settings::values.gpu_accuracy;
    auto& fences = Settings::values.gpu_fence_behavior;
    auto& reactive = Settings::values.use_reactive_flushing;
    SettingSnapshot saved_accuracy{accuracy};
    SettingSnapshot saved_fences{fences};
    SettingSnapshot saved_reactive{reactive};
    const bool old_preemptive = Settings::values.preemptive_cpu_reads;
    const auto old_runtime_accuracy = Settings::values.current_gpu_accuracy;
    SCOPE_EXIT {
        Settings::values.preemptive_cpu_reads = old_preemptive;
        Settings::values.current_gpu_accuracy = old_runtime_accuracy;
    };
    Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::High;
    accuracy.SetGlobal(true);
    accuracy.SetValue(Settings::GpuAccuracy::High);
    fences.SetGlobal(true);
    fences.SetValue(Settings::GpuFenceBehavior::Default);
    reactive.SetGlobal(true);
    reactive.SetValue(true);
    FakeWindow window;
    FakeRenderer renderer{window};

    SECTION("Inherited defaults select read traps and delayed native fences") {
        Core::GameSettings::LoadEarlyOverrides(0x040004F027560000ULL);
        CHECK(accuracy.GetValue() == Settings::GpuAccuracy::Low);
        CHECK(Settings::values.current_gpu_accuracy == Settings::GpuAccuracy::Low);
        CHECK(fences.GetValue() == Settings::GpuFenceBehavior::Balanced);
        CHECK_FALSE(reactive.GetValue());
        Core::GameSettings::LoadOverrides(0x040004F027560000ULL, renderer);
        CHECK(accuracy.GetValue() == Settings::GpuAccuracy::Low);
        CHECK(fences.GetValue() == Settings::GpuFenceBehavior::Balanced);
        CHECK_FALSE(reactive.GetValue());
        CHECK_FALSE(accuracy.UsingGlobal());
        CHECK_FALSE(fences.UsingGlobal());
        CHECK_FALSE(reactive.UsingGlobal());
        CHECK(accuracy.GetValue(true) == Settings::GpuAccuracy::High);
        CHECK(fences.GetValue(true) == Settings::GpuFenceBehavior::Default);
        CHECK(reactive.GetValue(true));
        CHECK_FALSE(Settings::values.preemptive_cpu_reads);
    }
    SECTION("Explicit per-game choices remain authoritative") {
        accuracy.SetGlobal(false);
        accuracy.SetValue(Settings::GpuAccuracy::High);
        fences.SetGlobal(false);
        fences.SetValue(Settings::GpuFenceBehavior::Accurate);
        reactive.SetGlobal(false);
        reactive.SetValue(true);
        Core::GameSettings::LoadEarlyOverrides(0x040004F027560000ULL);
        Core::GameSettings::LoadOverrides(0x040004F027560000ULL, renderer);
        CHECK(accuracy.GetValue() == Settings::GpuAccuracy::High);
        CHECK(Settings::values.current_gpu_accuracy == Settings::GpuAccuracy::High);
        CHECK(fences.GetValue() == Settings::GpuFenceBehavior::Accurate);
        CHECK(reactive.GetValue());
        CHECK_FALSE(Settings::values.preemptive_cpu_reads);
    }
    SECTION("A different title retains its inherited settings") {
        Core::GameSettings::LoadEarlyOverrides(0x040004F027570000ULL);
        Core::GameSettings::LoadOverrides(0x040004F027570000ULL, renderer);
        CHECK(accuracy.GetValue() == Settings::GpuAccuracy::High);
        CHECK(fences.GetValue() == Settings::GpuFenceBehavior::Default);
        CHECK(reactive.GetValue());
        CHECK(accuracy.UsingGlobal());
        CHECK(fences.UsingGlobal());
        CHECK(reactive.UsingGlobal());
    }
}

TEST_CASE("Yoshi keeps reactive flushing unless the game's own setting turns it off",
          "[game_settings]") {
    auto& reactive = Settings::values.use_reactive_flushing;
    SettingSnapshot saved_reactive{reactive};
    const bool old_preemptive = Settings::values.preemptive_cpu_reads;
    SCOPE_EXIT {
        Settings::values.preemptive_cpu_reads = old_preemptive;
    };
    reactive.SetGlobal(true);
    reactive.SetValue(false);
    FakeWindow window;
    FakeRenderer renderer{window};
    constexpr u64 yoshi = 0x0400245021D2C000ULL;

    SECTION("An inherited global off is overridden at boot and after the properties dialog") {
        Core::GameSettings::LoadEarlyOverrides(yoshi);
        CHECK(reactive.GetValue());
        CHECK_FALSE(reactive.UsingGlobal());
        CHECK_FALSE(reactive.GetValue(true));
        reactive.SetGlobal(true);
        Core::GameSettings::LoadOverrides(yoshi, renderer);
        CHECK(reactive.GetValue());
        CHECK_FALSE(reactive.GetValue(true));
    }
    SECTION("An explicit per-game choice remains authoritative") {
        reactive.SetGlobal(false);
        reactive.SetValue(false);
        Core::GameSettings::LoadEarlyOverrides(yoshi);
        Core::GameSettings::LoadOverrides(yoshi, renderer);
        CHECK_FALSE(reactive.GetValue());
    }
    SECTION("A different title retains its inherited setting") {
        Core::GameSettings::LoadEarlyOverrides(yoshi + 0x1000);
        CHECK_FALSE(reactive.GetValue());
        CHECK(reactive.UsingGlobal());
    }
}

TEST_CASE("Wonder and Kirby Switch 2 Editions get the split-friendly GPU defaults", "[game_settings]") {
    auto& accuracy = Settings::values.gpu_accuracy;
    auto& fences = Settings::values.gpu_fence_behavior;
    auto& reactive = Settings::values.use_reactive_flushing;
    SettingSnapshot saved_accuracy{accuracy};
    SettingSnapshot saved_fences{fences};
    SettingSnapshot saved_reactive{reactive};
    const bool old_preemptive = Settings::values.preemptive_cpu_reads;
    const auto old_runtime_accuracy = Settings::values.current_gpu_accuracy;
    SCOPE_EXIT {
        Settings::values.preemptive_cpu_reads = old_preemptive;
        Settings::values.current_gpu_accuracy = old_runtime_accuracy;
    };
    Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::High;
    accuracy.SetGlobal(true);
    accuracy.SetValue(Settings::GpuAccuracy::High);
    fences.SetGlobal(true);
    fences.SetValue(Settings::GpuFenceBehavior::Default);
    reactive.SetGlobal(true);
    reactive.SetValue(true);
    FakeWindow window;
    FakeRenderer renderer{window};
    // Wonder, Kirby and the Forgotten Land: the Switch 2 Edition is the Switch 1 ID + 0x0300...1.
    const std::uint64_t switch1 = GENERATE(0x010015100B514000ULL, 0x01004D300C5AE000ULL);
    const std::uint64_t switch2 = switch1 + 0x0300000000000001ULL;

    SECTION("Before the renderer exists") {
        Core::GameSettings::LoadEarlyOverrides(switch2);
        CHECK(accuracy.GetValue() == Settings::GpuAccuracy::Low);
        CHECK(Settings::values.current_gpu_accuracy == Settings::GpuAccuracy::Low);
        CHECK(fences.GetValue() == Settings::GpuFenceBehavior::Balanced);
        CHECK_FALSE(reactive.GetValue());
        CHECK(accuracy.GetValue(true) == Settings::GpuAccuracy::High);
    }
    SECTION("Again after the configuration dialog reset them, which calls LoadOverrides only") {
        Core::GameSettings::LoadOverrides(switch2, renderer);
        CHECK(accuracy.GetValue() == Settings::GpuAccuracy::Low);
        CHECK(Settings::values.current_gpu_accuracy == Settings::GpuAccuracy::Low);
        CHECK(fences.GetValue() == Settings::GpuFenceBehavior::Balanced);
        CHECK_FALSE(reactive.GetValue());
    }
    SECTION("The Switch 1 release keeps the inherited settings") {
        Core::GameSettings::LoadEarlyOverrides(switch1);
        Core::GameSettings::LoadOverrides(switch1, renderer);
        CHECK(accuracy.GetValue() == Settings::GpuAccuracy::High);
        CHECK(fences.GetValue() == Settings::GpuFenceBehavior::Default);
        CHECK(reactive.GetValue());
    }
}

TEST_CASE("GameSettings: Kirby fog workaround preserves fog on Turing and newer", "[game_settings]") {
    SettingSnapshot saved_accuracy{Settings::values.gpu_accuracy};
    SettingSnapshot saved_fences{Settings::values.gpu_fence_behavior};
    SettingSnapshot saved_reactive{Settings::values.use_reactive_flushing};
    SettingSnapshot saved_backend{Settings::values.renderer_backend};
    const auto old_runtime_accuracy = Settings::values.current_gpu_accuracy;
    const bool old_preemptive = Settings::values.preemptive_cpu_reads;
    SCOPE_EXIT {
        Settings::values.kirby_fog_workaround = false;
        Settings::values.current_gpu_accuracy = old_runtime_accuracy;
        Settings::values.preemptive_cpu_reads = old_preemptive;
    };
    constexpr u64 kirby = 0x04004D300C5AE001ULL;
    const auto workaround = [](u64 program_id, std::string vendor, Settings::RendererBackend backend,
                               bool nvidia_turing_or_newer = false) {
        Settings::values.renderer_backend.SetValue(backend);
        FakeWindow window;
        FakeRenderer renderer{window, std::move(vendor), nvidia_turing_or_newer};
        Core::GameSettings::LoadOverrides(program_id, renderer);
        return Settings::values.kirby_fog_workaround;
    };
    // Driver names as RendererVulkan::GetDeviceVendor reports them (vk::GetDriverName).
    const std::string nvidia = "Nvidia";
    CHECK(workaround(kirby, nvidia, Settings::RendererBackend::Vulkan));
    CHECK_FALSE(workaround(kirby, nvidia, Settings::RendererBackend::Vulkan, true));
    CHECK_FALSE(workaround(kirby, "RADV", Settings::RendererBackend::Vulkan));
    CHECK_FALSE(workaround(kirby, nvidia, Settings::RendererBackend::OpenGL_GLSL));
    // Another title after Kirby clears it again.
    CHECK(workaround(kirby, nvidia, Settings::RendererBackend::Vulkan));
    CHECK_FALSE(workaround(0x0400FF500E34A001ULL, nvidia, Settings::RendererBackend::Vulkan));
}

TEST_CASE("GameSettings: Donkey Kong Bananza and Star Fox keep the normal CPU clock when inherited",
          "[game_settings]") {
    auto& cpu_clock = Settings::values.cpu_clock;
    SettingSnapshot saved_clock{cpu_clock};
    SettingSnapshot saved_accuracy{Settings::values.gpu_accuracy};
    SettingSnapshot saved_reactive{Settings::values.use_reactive_flushing};
    SettingSnapshot saved_gpu_clock{Settings::values.gpu_clock};
    const bool old_preemptive = Settings::values.preemptive_cpu_reads;
    SCOPE_EXIT {
        Settings::values.preemptive_cpu_reads = old_preemptive;
    };
    cpu_clock.SetGlobal(true);
    cpu_clock.SetValue(Settings::CpuClock::Boost);
    FakeWindow window;
    FakeRenderer renderer{window};
    const std::uint64_t title_id = GENERATE(0x040074A01BF12000ULL, 0x0400E88025B1A000ULL);

    SECTION("Global Boost is overridden to Normal for the session") {
        Core::GameSettings::LoadOverrides(title_id, renderer);
        CHECK(cpu_clock.GetValue() == Settings::CpuClock::Normal);
        CHECK_FALSE(cpu_clock.UsingGlobal());
        CHECK(cpu_clock.GetValue(true) == Settings::CpuClock::Boost);
    }
    SECTION("An explicit per-game CPU clock stays authoritative") {
        cpu_clock.SetGlobal(false);
        cpu_clock.SetValue(Settings::CpuClock::Overclock);
        Core::GameSettings::LoadOverrides(title_id, renderer);
        CHECK(cpu_clock.GetValue() == Settings::CpuClock::Overclock);
    }
    SECTION("Another title keeps the inherited Boost CPU clock") {
        Core::GameSettings::LoadOverrides(title_id + 1, renderer);
        CHECK(cpu_clock.GetValue() == Settings::CpuClock::Boost);
        CHECK(cpu_clock.UsingGlobal());
    }
}
