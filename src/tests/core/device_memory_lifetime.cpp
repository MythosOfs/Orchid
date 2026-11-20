// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef __linux__

#include <cerrno>
#include <fstream>
#include <sys/mman.h>

#include <catch2/catch_test_macros.hpp>

#include "common/cpu_features.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/device_memory.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/am/applet_manager.h"
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
    SCOPE_EXIT {
        Settings::SetCurrentProgramIsSwitch2(previous_switch2);
        Common::g_wall_clock.SetGuestCNTFRQ(previous_cntfrq);
    };
    Common::g_wall_clock.SetGuestCNTFRQ(Common::WallClock::CNTFRQ);

    Core::System system;
    system.Initialize();
    system.Kernel().Initialize();
    system.PushGeneralChannelData({1, 2, 3});
    system.CoreTiming().AddTicks(100'000);
    const auto ticks = system.CoreTiming().GetClockTicks();
    CHECK(ticks > 0);
    auto* const old_backing = system.DeviceMemory().buffer.BackingBasePointer();
    unsigned char status{};
    CHECK(mincore(old_backing, 1, &status) == 0);

    Settings::SetCurrentProgramIsSwitch2(true);
    system.ShutdownMainProcess();
    const int mincore_result = mincore(old_backing, 1, &status);
    const int mincore_error = errno;
    REQUIRE(mincore_result == -1);
    CHECK(mincore_error == ENOMEM);
    CHECK(system.GetGeneralChannel().size() == 1);

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
    CHECK(system.CoreTiming().GetClockTicks() == ticks);
    REQUIRE_FALSE(Settings::CurrentProgramIsSwitch2());
    // The Switch 1 loader-failure shutdown leaves its freshly allocated backing in place.
    CHECK(mincore(system.DeviceMemory().buffer.BackingBasePointer(), 1, &status) == 0);
}

#endif
