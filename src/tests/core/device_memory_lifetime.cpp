// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef __linux__

#include <cerrno>
#include <fstream>
#include <future>
#include <sys/mman.h>

#include <catch2/catch_test_macros.hpp>

#include "common/cpu_features.h"
#include "common/game_fps.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/device_memory.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_scheduler_lock.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/os/event.h"
#include "core/hle/service/sm/sm.h"
#include "core/loader/loader.h"
#include "tests/core/system_test_context.h"

namespace {

class HeadlessWindow final : public Core::Frontend::EmuWindow {
public:
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return nullptr;
    }

    bool IsShown() const override {
        return false;
    }
};

} // namespace

TEST_CASE("Switch 2 shutdown releases guest RAM and permits a same-layout reload",
          "[device_memory_lifetime]") {
    ScopedSystemTestContext context;
    const bool previous_switch2 = Settings::CurrentProgramIsSwitch2();
    const auto previous_cntfrq = Common::g_wall_clock.GetGuestCNTFRQ();
    const auto previous_astc = Settings::GetAstcRecompressionOverride();
    const auto previous_wonder_fps = Settings::wonder_fps_active.load();
    const auto previous_timed_fps = Settings::timed_fps_active.load();
    const auto previous_frame_step = Settings::frame_step_address.load();
    const auto previous_dynamic_fps = Settings::wonder_dynamic_fps_active.load();
    const bool previous_squashed_blend = Settings::values.use_squashed_iterated_blend;
    SCOPE_EXIT {
        Settings::SetAstcRecompressionOverride(previous_astc);
        Settings::wonder_fps_active.store(previous_wonder_fps);
        Settings::timed_fps_active.store(previous_timed_fps);
        Settings::frame_step_address.store(previous_frame_step);
        Settings::wonder_dynamic_fps_active.store(previous_dynamic_fps);
        Settings::values.use_squashed_iterated_blend = previous_squashed_blend;
        Settings::SetCurrentProgramIsSwitch2(previous_switch2);
        Common::g_wall_clock.SetGuestCNTFRQ(previous_cntfrq);
    };
    Settings::SetCurrentProgramIsSwitch2(true);
    Settings::values.memory_layout_mode.SetValue(Settings::MemoryLayout::Memory_12Gb);
    Common::g_wall_clock.SetGuestCNTFRQ(Common::WallClock::CNTFRQ);

    Core::System system;
    system.Initialize();
    system.Kernel().Initialize();
    system.CoreTiming().AddTicks(100'000);
    const auto time = system.CoreTiming().GetGlobalTimeNs();
    CHECK(time.count() > 0);
    auto* const old_backing = system.DeviceMemory().buffer.BackingBasePointer();
    unsigned char status{};
    CHECK(mincore(old_backing, 1, &status) == 0);

    system.ShutdownMainProcess();
    const int mincore_result = mincore(old_backing, 1, &status);
    const int mincore_error = errno;
    REQUIRE(mincore_result == -1);
    CHECK(mincore_error == ENOMEM);

    // Reach kernel initialization and the ordinary loader-error shutdown without a GPU or ROM.
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "invalid.nro";
    std::ofstream{path}.put('\0');
    HeadlessWindow window;
    Service::AM::FrontendAppletParameters params{
        .launch_type = Service::AM::LaunchType::ApplicationInitiated,
    };
    const auto result = system.Load(window, path.string(), params);
    CHECK(result == static_cast<Core::SystemResultStatus>(
                        static_cast<u32>(Core::SystemResultStatus::ErrorLoader) +
                        static_cast<u32>(Loader::ResultStatus::ErrorLoadingNRO)));
    CHECK(system.CoreTiming().GetGlobalTimeNs() == time);
    REQUIRE_FALSE(Settings::CurrentProgramIsSwitch2());
    // The Switch 1 loader-failure shutdown leaves its freshly allocated backing in place.
    CHECK(mincore(system.DeviceMemory().buffer.BackingBasePointer(), 1, &status) == 0);
}

TEST_CASE("General channel survives shutdown regardless of the next IPC order",
          "[device_memory_lifetime]") {
    ScopedSystemTestContext context;
    const bool previous_switch2 = Settings::CurrentProgramIsSwitch2();
    const bool previous_squashed_blend = Settings::values.use_squashed_iterated_blend;
    SCOPE_EXIT {
        Settings::SetCurrentProgramIsSwitch2(previous_switch2);
        Settings::values.use_squashed_iterated_blend = previous_squashed_blend;
    };
    Settings::SetCurrentProgramIsSwitch2(false);

    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    kernel.Initialize();
    system.PushGeneralChannelData({1, 2, 3});
    system.PushGeneralChannelData({4, 5, 6});
    system.ShutdownMainProcess();

    kernel.Initialize();
    SCOPE_EXIT { system.ShutdownMainProcess(); };
    const auto is_signaled = [&] {
        auto* event = system.GetGeneralChannelEvent().GetHandle();
        Kernel::KScopedSchedulerLock lock{kernel};
        return event->IsSignaled(kernel);
    };
    std::vector<u8> data;
    SECTION("Pop before getting the event") {
        CHECK(system.TryPopGeneralChannel(data));
        CHECK(data == std::vector<u8>{4, 5, 6});
        CHECK(system.TryPopGeneralChannel(data));
        CHECK(data == std::vector<u8>{1, 2, 3});
        CHECK_FALSE(is_signaled());
    }
    SECTION("Get the event before popping") {
        CHECK(is_signaled());
        CHECK(system.TryPopGeneralChannel(data));
        CHECK(data == std::vector<u8>{4, 5, 6});
        CHECK(is_signaled());
        CHECK(system.TryPopGeneralChannel(data));
        CHECK(data == std::vector<u8>{1, 2, 3});
        CHECK_FALSE(is_signaled());
    }
    CHECK_FALSE(system.TryPopGeneralChannel(data));
}

TEST_CASE("Host service startup can stop while a dependency is missing",
          "[device_memory_lifetime]") {
    using namespace std::chrono_literals;

    ScopedSystemTestContext context;
    const bool previous_switch2 = Settings::CurrentProgramIsSwitch2();
    SCOPE_EXIT { Settings::SetCurrentProgramIsSwitch2(previous_switch2); };
    Settings::SetCurrentProgramIsSwitch2(false);

    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    kernel.Initialize();
    SCOPE_EXIT { kernel.Shutdown(); };

    Service::SM::ServiceManager services{kernel};
    auto handler = std::make_shared<Service::SM::SM>(services, system);
    // If cancellation regresses, register the dependency before joining so the test can fail.
    const auto register_dependency = [&] {
        Kernel::KServerPort* port{};
        REQUIRE(services.RegisterService(&port, "dep", 1, [handler] { return handler; }).IsSuccess());
        port->Close(kernel);
    };
    std::promise<std::shared_ptr<Service::SM::SM>> dependency;
    auto result = dependency.get_future();
    std::jthread startup([&](std::stop_token stop) {
        dependency.set_value(services.WaitForService<Service::SM::SM>("dep", stop));
    });
    CHECK(result.wait_for(20ms) == std::future_status::timeout);
    SECTION("Stop before registration") {
        startup.request_stop();
        const auto status = result.wait_for(1s);
        CHECK(status == std::future_status::ready);
        register_dependency();
        CHECK(result.get() == nullptr);
        CHECK(services.WaitForService<Service::SM::SM>("dep", startup.get_stop_token()) == nullptr);
    }
    SECTION("Registration wakes startup") {
        register_dependency();
        const auto status = result.wait_for(1s);
        CHECK(status == std::future_status::ready);
        if (status != std::future_status::ready) {
            startup.request_stop();
        }
        CHECK(result.get() == handler);
    }
}

#endif
