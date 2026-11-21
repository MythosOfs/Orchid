// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "video_core/gpu.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_null/null_rasterizer.h"
#include "video_core/texture_cache/util.h"

namespace {

struct RecompressionRestore {
    Settings::AstcRecompression configured = Settings::values.astc_recompression.GetValue();
    ~RecompressionRestore() {
        Settings::values.astc_recompression.SetValue(configured);
        Settings::SetAstcRecompressionOverride(std::nullopt);
    }
};

} // Anonymous namespace

TEST_CASE("ASTC recompression override wins until cleared", "[video_core][astc]") {
    RecompressionRestore restore;
    Settings::values.astc_recompression.SetValue(Settings::AstcRecompression::Uncompressed);
    Settings::SetAstcRecompressionOverride(std::nullopt);
    REQUIRE(Settings::GetAstcRecompression() == Settings::AstcRecompression::Uncompressed);

    Settings::SetAstcRecompressionOverride(Settings::AstcRecompression::Bc3);
    REQUIRE(Settings::GetAstcRecompression() == Settings::AstcRecompression::Bc3);
    REQUIRE(Settings::values.astc_recompression.GetValue() ==
            Settings::AstcRecompression::Uncompressed);

    Settings::SetAstcRecompressionOverride(std::nullopt);
    REQUIRE(Settings::GetAstcRecompression() == Settings::AstcRecompression::Uncompressed);
}

TEST_CASE("ASTC recompression follows the configuration without an override",
          "[video_core][astc]") {
    RecompressionRestore restore;
    Settings::SetAstcRecompressionOverride(std::nullopt);
    Settings::values.astc_recompression.SetValue(Settings::AstcRecompression::Bc1);
    REQUIRE(Settings::GetAstcRecompression() == Settings::AstcRecompression::Bc1);
}

TEST_CASE("Switch 2 program flag is per load", "[video_core][astc]") {
    Settings::SetCurrentProgramIsSwitch2(true);
    REQUIRE(Settings::CurrentProgramIsSwitch2());
    Settings::SetCurrentProgramIsSwitch2(false);
    REQUIRE_FALSE(Settings::CurrentProgramIsSwitch2());
}

TEST_CASE("ASTC opacity is bit exact on mapped and fragmented guest pages",
          "[video_core][astc_guest_scan]") {
    const bool saved_multi_core = Settings::values.use_multi_core.GetValue();
    SCOPE_EXIT { Settings::values.use_multi_core.SetValue(saved_multi_core); };
    Settings::values.use_multi_core.SetValue(false);
    Core::System system;
    system.Initialize();
    Kernel::KProcess process(system.Kernel());
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& cpu = process.GetMemory();
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(32, 12);
    cpu.SetCurrentPageTable(process);
    constexpr u64 physical = 0x80000, guest = 0x200000, device = 0x100000;
    constexpr size_t page = 4096;
    cpu.MapMemoryRegion(table, Common::ProcessAddress{guest}, 3 * page,
                        Common::PhysicalAddress{Core::DramMemoryMap::Base + physical},
                        Common::MemoryPermission::ReadWrite, false);
    system.DeviceMemory().buffer.ClearBackingRegion(physical, 3 * page, 0);
    Tegra::MaxwellDeviceMemoryManager manager(system.DeviceMemory());
    manager.Map(device, guest, 3 * page, manager.RegisterProcess(&cpu));
    Tegra::GPU gpu(system, false, false);
    Null::RasterizerNull rasterizer(gpu);
    Tegra::MemoryManager address_space(system, manager);
    address_space.BindRasterizer(&rasterizer);
    constexpr GPUVAddr contiguous = 0x300000, fragmented = 0x400000;
    address_space.Map(contiguous, device, 2 * page, Tegra::PTEKind::PITCH, false);
    address_space.Map(fragmented, device, page, Tegra::PTEKind::PITCH, false);
    address_space.Map(fragmented + page, device + 2 * page, page, Tegra::PTEKind::PITCH, false);
    std::array<u8, 32> astc{};
    astc[16] = 0xfc;
    astc[17] = 0x0d;
    const auto verify = [&](GPUVAddr start, size_t scratch_size) {
        Common::ScratchBuffer<u8> scratch;
        for (const bool opaque : {true, false}) {
            astc[30] = astc[31] = opaque ? 0xff : 0;
            address_space.WriteBlockUnsafe(start, astc.data(), astc.size());
            std::vector<u8> copied(astc.size());
            address_space.ReadBlockUnsafe(start, copied.data(), copied.size());
            REQUIRE(VideoCommon::AstcDataLooksOpaque(copied) == opaque);
            CHECK(VideoCommon::AstcDataLooksOpaque(address_space, start, astc.size(), scratch) ==
                  opaque);
            CHECK(scratch.size() == scratch_size);
        }
    };
    REQUIRE(address_space.GetSpan(contiguous, astc.size()));
    REQUIRE_FALSE(address_space.GetSpan(fragmented + page - 16, astc.size()));
    verify(contiguous, 0);
    verify(fragmented + page - 16, astc.size());
}
