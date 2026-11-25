// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "shader_recompiler/exception.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/compute_binding_diagnostic.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/gpu.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace {
using VideoCommon::StorageBufferSizeRule;

struct FixtureRuntime;
struct FixtureBuffer : VideoCommon::BufferBase {
    std::vector<u8> bytes;
    FixtureBuffer(FixtureRuntime&, VideoCommon::NullBufferParams tag) : BufferBase(tag) {}
    FixtureBuffer(FixtureRuntime&, DAddr addr, u32 size, bool sparse)
        : BufferBase(addr, size), bytes(size), sparse(sparse) {}
    explicit FixtureBuffer(size_t size)
        : BufferBase(VideoCommon::NullBufferParams{}), bytes(size) {}
    bool sparse{};
    // (offset, size) of every MarkUsage call: what the cache told the runtime its GPU work uses.
    std::vector<std::pair<u64, u64>> marked;
    void MarkUsage(u64 offset, u64 size) {
        marked.emplace_back(offset, size);
    }
};
struct FixtureStage {
    // Shared: the cache keeps a copy of a fence download's stage until its fence pops.
    std::shared_ptr<FixtureBuffer> buffer;
    size_t offset{};
    std::span<u8> mapped_span;
    explicit FixtureStage(size_t size)
        : buffer(std::make_shared<FixtureBuffer>(size)), mapped_span(buffer->bytes) {}
    explicit FixtureStage(std::shared_ptr<FixtureBuffer> buffer_)
        : buffer(std::move(buffer_)), mapped_span(buffer->bytes) {}
};
struct FixtureRuntime {
    u32 GetStorageBufferAlignment() const {
        return 256;
    }
    u64 GetMaxStorageBufferRange() const {
        ++range_queries;
        return storage_buffer_range;
    }
    u64 storage_buffer_range{std::numeric_limits<u32>::max()};
    mutable u64 range_queries{};
    u64 overlap_pages_scanned{};
    bool PrefersSparseSources() const {
        return true;
    }
    bool BindMultiRangeStorageBuffer(u64 key, bool is_written) {
        multi_range_binds.emplace_back(key, is_written);
        return !multi_range_fail;
    }
    bool multi_range_fail{};
    std::vector<std::pair<u64, bool>> multi_range_binds;
    void ResetMultiRange() {
        multi_range_sources.clear();
    }
    void PushMultiRangeSource(const FixtureBuffer& buffer, u32 offset, u32 size) {
        multi_range_sources.emplace_back(buffer.CpuAddr(), offset, size);
    }
    void InvalidateMultiRange(u64 key) {
        invalidated_multi_range.push_back(key);
    }
    std::vector<std::tuple<u64, u32, u32>> multi_range_sources;
    std::vector<u64> invalidated_multi_range;
    void BindStorageBuffer(const FixtureBuffer& buffer, u32 offset, u32 size, bool is_written) {
        storage_binds.emplace_back(buffer.CpuAddr(), offset, size, is_written);
    }
    std::vector<std::tuple<u64, u32, u32, bool>> storage_binds;
    u32 GetUniformBufferAlignment() const {
        return 256;
    }
    void BindUniformBuffer(const FixtureBuffer&, u32, u32) {}
    std::span<u8> BindMappedUniformBuffer(u32, u32, u32) {
        return {};
    }
    void BindTextureBuffer(const FixtureBuffer&, u32, u32,
                           VideoCore::Surface::PixelFormat) {}
    bool out_of_bounds{};
    std::vector<std::pair<u64, u64>> copied;
    struct DeferredStage {
        std::shared_ptr<FixtureBuffer> buffer;
        bool active{};
        u64 last_use_tick{};
    };
    u64 current_tick{};
    u64 gpu_tick{};
    std::vector<DeferredStage> deferred_stages;
    std::vector<size_t> allocation_ids;
    std::vector<size_t> freed_ids;
    FixtureStage DownloadStagingBuffer(size_t size, bool deferred = false) {
        if (!deferred) {
            return FixtureStage(size);
        }
        for (size_t i = 0; i < deferred_stages.size(); ++i) {
            auto& stage = deferred_stages[i];
            if (!stage.active && IsFree(stage.last_use_tick) && stage.buffer->bytes.size() >= size) {
                stage.active = true;
                allocation_ids.push_back(i);
                return FixtureStage(stage.buffer);
            }
        }
        FixtureStage stage(size);
        allocation_ids.push_back(deferred_stages.size());
        deferred_stages.push_back({stage.buffer, true});
        return stage;
    }
    void FreeDeferredStagingBuffer(FixtureStage& ref) {
        ReleaseDeferredStage(ref, current_tick);
    }
    void FreeCompletedDeferredStagingBuffer(FixtureStage& ref) {
        ReleaseDeferredStage(ref, 0);
    }
    void ReleaseDeferredStage(FixtureStage& ref, u64 last_use_tick) {
        const auto it = std::ranges::find_if(deferred_stages, [&](const auto& stage) {
            return stage.buffer == ref.buffer;
        });
        REQUIRE(it != deferred_stages.end());
        REQUIRE(it->active);
        it->active = false;
        it->last_use_tick = last_use_tick;
        freed_ids.push_back(static_cast<size_t>(it - deferred_stages.begin()));
    }
    FixtureStage UploadStagingBuffer(size_t size) {
        return FixtureStage(size);
    }
    bool CanReportMemoryUsage() {
        return false;
    }
    u64 GetDeviceLocalMemory() {
        return 0;
    }
    void Finish() {}
    u64 CurrentTick() {
        return current_tick;
    }
    bool IsFree(u64 tick) {
        return tick <= gpu_tick;
    }
    void Wait(u64) {}
    template <class... T>
    bool CanReorderUpload(T&&...) {
        return false;
    }
    void ClearBuffer(FixtureBuffer& b, u64 off, u64 size, u32) {
        std::fill_n(b.bytes.begin() + off, size, 0);
    }
    void PreCopyBarrier() {}
    void PostCopyBarrier() {}
    template <class C>
    void CopyBuffer(std::shared_ptr<FixtureBuffer>& dst, FixtureBuffer& src, const C& copies,
                    bool sync, bool reorder = false) {
        CopyBuffer(*dst, src, copies, sync, reorder);
    }
    template <class C>
    void CopyBuffer(FixtureBuffer& dst, std::shared_ptr<FixtureBuffer>& src, const C& copies,
                    bool sync, bool reorder = false) {
        CopyBuffer(dst, *src, copies, sync, reorder);
    }
    template <class C>
    void CopyBuffer(FixtureBuffer& dst, FixtureBuffer& src, const C& copies, bool, bool = false) {
        for (const auto& c : copies) {
            copied.emplace_back(src.CpuAddr(), c.src_offset);
            if (c.src_offset > src.bytes.size() || c.size > src.bytes.size() - c.src_offset ||
                c.dst_offset > dst.bytes.size() || c.size > dst.bytes.size() - c.dst_offset) {
                out_of_bounds = true;
                continue;
            }
            std::memcpy(dst.bytes.data() + c.dst_offset, src.bytes.data() + c.src_offset, c.size);
        }
    }
};
struct FixtureParams {
    using Buffer = FixtureBuffer;
    using Runtime = FixtureRuntime;
    using Async_Buffer = FixtureStage;
    using MemoryTracker = VideoCommon::MemoryTrackerBase<Tegra::MaxwellDeviceMemoryManager>;
    static constexpr bool IS_OPENGL = false, HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS = false,
                          HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT = false,
                          NEEDS_BIND_UNIFORM_INDEX = false, NEEDS_BIND_STORAGE_INDEX = false,
                          USE_MEMORY_MAPS = true, SEPARATE_IMAGE_BUFFER_BINDINGS = false,
                          USE_MEMORY_MAPS_FOR_UPLOADS = true;
};
// Bind just the real address-space policy needed by these memory-only cache tests. Engines
// and channel execution are not involved in ObtainCPUBuffer/GetFlushArea/CommitAsyncFlushes.
struct NativeFixtureCache : VideoCommon::BufferCache<FixtureParams> {
    using VideoCommon::BufferCache<FixtureParams>::BufferCache;
    using VideoCommon::BufferCache<FixtureParams>::StorageBufferBinding;
    void SetAddressSpace(Tegra::MemoryManager& memory) {
        gpu_memory = &memory;
    }
    void SetKeplerCompute(Tegra::Engines::KeplerCompute* kepler) {
        kepler_compute = kepler;
    }
    void EnableChannelState() {
        channel.emplace(0);
        channel_info.emplace(*channel);
        channel_state = &*channel_info;
    }
    VideoCommon::BufferCacheChannelInfo& Channel() {
        return *channel_info;
    }
    FixtureBuffer* Obtain(u64 device_addr, u32 size) {
        return ObtainCPUBuffer(device_addr, size,
                               VideoCommon::ObtainBufferSynchronize::NoSynchronize,
                               VideoCommon::ObtainBufferOperation::DoNothing)
            .first;
    }
    std::optional<Tegra::Control::ChannelState> channel;
    std::optional<VideoCommon::BufferCacheChannelInfo> channel_info;
};

struct BindingRasterizer : Null::RasterizerNull {
    using Null::RasterizerNull::RasterizerNull;
    std::function<void()> on_mapping_change;
    std::function<void()> on_dispatch;

    void DispatchCompute() override {
        if (on_dispatch) {
            on_dispatch();
        }
    }
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {
        if (on_mapping_change) {
            on_mapping_change();
        }
    }
};
} // namespace

TEST_CASE("BufferCache: storage bindings follow device remaps and guest descriptor writes",
          "[video_core][buffer_cache][storage_binding]") {
    const bool old_multi_core = Settings::values.use_multi_core.GetValue();
    SCOPE_EXIT { Settings::values.use_multi_core.SetValue(old_multi_core); };
    Settings::values.use_multi_core.SetValue(false);
    Core::System system;
    system.Initialize();
    Kernel::KProcess process(system.Kernel());
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& cpu = process.GetMemory();
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(32, 12);
    cpu.SetCurrentPageTable(process);
    constexpr u64 phys = 0x80000, guest = 0x200000, device = 0x100000;
    constexpr u64 extent = 0x60000;
    system.DeviceMemory().buffer.ClearBackingRegion(phys, extent, 0);
    cpu.MapMemoryRegion(table, Common::ProcessAddress{guest}, extent,
                        Common::PhysicalAddress{Core::DramMemoryMap::Base + phys},
                        Common::MemoryPermission::ReadWrite, false);
    Tegra::MaxwellDeviceMemoryManager manager(system.DeviceMemory());
    manager.Map(device, guest, extent, manager.RegisterProcess(&cpu));
    Tegra::GPU gpu(system, false, false);
    BindingRasterizer rasterizer(gpu);
    Tegra::MemoryManager address_space(system, manager);
    address_space.BindRasterizer(&rasterizer);
    FixtureRuntime runtime;
    auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
    cache->SetAddressSpace(address_space);

    const bool big_pages = GENERATE(false, true);
    CAPTURE(big_pages);
    const u32 page = big_pages ? 0x10000 : 0x1000;
    constexpr GPUVAddr descriptor = 0x300000, target = 0x400000;
    constexpr DAddr new_device = device + 0x30000;
    address_space.Map(descriptor, device + 0x50000, page, Tegra::PTEKind::PITCH, big_pages);
    address_space.Map(target, device, 2 * page, Tegra::PTEKind::PITCH, big_pages);
    address_space.Write<u64>(descriptor, target);
    address_space.Write<u64>(descriptor + 8, 128);
    const auto dlss = [&] { return cache->StorageBufferBinding(descriptor, 0, true, true, true); };
    const auto native = [&] { return cache->StorageBufferBinding(descriptor, 0, true, true, false); };
    const auto graphics = [&] { return cache->StorageBufferBinding(descriptor, 0, true); };
    CHECK(graphics().size == 128);
    address_space.Write<u64>(descriptor + 8, 512);
    CHECK(graphics().size == 512);
    CHECK(native().size == 512);
    CHECK(native().physical_size == 512);
    const auto initial = dlss();
    REQUIRE(initial.device_addr == device);
    REQUIRE(initial.size == 2 * page);
    REQUIRE(initial.physical_size == 2 * page);

    // The callback runs after the first page points at its new backing, before Map returns.
    // The same bare pointer now spans two adjacent GPU pages at disjoint device addresses.
    const auto initial_generation = address_space.StableMappingGeneration();
    REQUIRE(initial_generation.has_value());
    std::optional<VideoCommon::Binding> during_remap;
    rasterizer.on_mapping_change = [&] {
        CHECK_FALSE(address_space.StableMappingGeneration().has_value());
        during_remap = dlss();
    };
    address_space.Map(target, new_device, page, Tegra::PTEKind::PITCH, big_pages);
    const auto next_generation = address_space.StableMappingGeneration();
    REQUIRE(next_generation.has_value());
    CHECK(*next_generation != *initial_generation);
    REQUIRE(during_remap.has_value());
    CHECK(during_remap->device_addr == new_device);
    CHECK(during_remap->size == 2 * page);
    CHECK(during_remap->physical_size == page);
    const auto after = dlss();
    CHECK(after.device_addr == new_device);
    CHECK(after.size == 2 * page);
    CHECK(after.physical_size == page);
    address_space.Write<u64>(descriptor + 8, 2 * page);
    CHECK(native().size == 2 * page);
    CHECK(native().physical_size == page);

    VideoCommon::ComputeBindingDiagnostic::Record diag_rec;
    {
        VideoCommon::ComputeBindingDiagnostic::Scope diag_scope{diag_rec};
        CHECK(cache->StorageBufferBinding(descriptor, 0, true, true, false, true).size ==
              2 * page);
    }
    CHECK(diag_rec.layout_size == 2 * page);
    CHECK(diag_rec.selected_size == 2 * page);

    const auto native_ro = [&] {
        return cache->StorageBufferBinding(descriptor, 0, false, true, false);
    };
    address_space.Write<u64>(descriptor + 8, 512);
    CHECK(native_ro().size == 512);
    CHECK(cache->StorageBufferBinding(descriptor, 0, false).size == 0x1000U);
    address_space.Write<u64>(descriptor + 8, 2 * page);

    // Unmap writes entries before kinds: the callback sees an unmapped page with the old extent.
    std::optional<VideoCommon::Binding> unmapped_dlss, unmapped_native;
    rasterizer.on_mapping_change = [&] {
        CHECK_FALSE(address_space.StableMappingGeneration().has_value());
        unmapped_dlss = dlss();
        unmapped_native = native();
    };
    address_space.Unmap(target + page, page);
    REQUIRE(unmapped_dlss.has_value());
    CHECK(unmapped_dlss->size == page);
    CHECK(unmapped_dlss->physical_size == page);
    REQUIRE(unmapped_native.has_value());
    CHECK(unmapped_native->size == page);
    CHECK(unmapped_native->physical_size == page);
    rasterizer.on_mapping_change = nullptr;
    CHECK(dlss().size == page);
    CHECK(dlss().physical_size == page);
    CHECK(native().size == page);
    CHECK(native().physical_size == page);

    constexpr GPUVAddr big_descriptor = 0x700000, big_target = 0x800000;
    constexpr u64 big_extent = 256 * 1024 * 1024, big_limit = 128 * 1024 * 1024;
    address_space.Map(big_descriptor, device + 0x58000, page, Tegra::PTEKind::PITCH, big_pages);
    address_space.Map(big_target, device + 0x80000, big_extent, Tegra::PTEKind::PITCH, big_pages);
    address_space.Write<u64>(big_descriptor, big_target);
    address_space.Write<u64>(big_descriptor + 8, big_extent);
    runtime.storage_buffer_range = big_limit;
    CHECK_THROWS_AS(cache->StorageBufferBinding(big_descriptor, 0, true, true, false),
                    Shader::RuntimeError);
    address_space.Write<u64>(big_descriptor + 8, big_limit);
    const auto big_binding = cache->StorageBufferBinding(big_descriptor, 0, true, true, false);
    CHECK(big_binding.size == big_limit);
    CHECK(big_binding.physical_size == big_limit);
    CHECK(cache->StorageBufferBinding(big_descriptor, 0, true).size == big_limit);
    address_space.Write<u64>(big_descriptor + 8, big_extent);
    const auto gfx_big = cache->StorageBufferBinding(big_descriptor, 0, true);
    CHECK(gfx_big.size == big_extent);
    CHECK(gfx_big.physical_size == 0);
    CHECK(cache->StorageBufferBinding(big_descriptor, 0, false).size == big_extent);
    address_space.Write<u64>(big_descriptor + 8, big_limit);

    constexpr GPUVAddr glasm_descriptor = 0xa00000, glasm_target = 0xb00000;
    constexpr u64 glasm_extent = 2ULL * 1024 * 1024 * 1024;
    constexpr u64 glasm_limit = std::numeric_limits<s32>::max();
    address_space.Map(glasm_descriptor, device + 0x59000, page, Tegra::PTEKind::PITCH, big_pages);
    address_space.Map(glasm_target, device + 0x200000, glasm_extent, Tegra::PTEKind::PITCH,
                      big_pages);
    address_space.Write<u64>(glasm_descriptor, glasm_target);
    address_space.Write<u64>(glasm_descriptor + 8, glasm_extent);
    runtime.storage_buffer_range = glasm_limit;
    CHECK_THROWS_AS(cache->StorageBufferBinding(glasm_descriptor, 0, true, true, false),
                    Shader::RuntimeError);
    address_space.Write<u64>(glasm_descriptor + 8, glasm_limit);
    CHECK(cache->StorageBufferBinding(glasm_descriptor, 0, true, true, false).size == glasm_limit);

    constexpr u64 mode_extent = 192 * 1024 * 1024;
    address_space.Write<u64>(big_descriptor + 8, mode_extent);
    runtime.storage_buffer_range = std::numeric_limits<u32>::max();
    CHECK(cache->StorageBufferBinding(big_descriptor, 0, true, true, false).size == mode_extent);
    runtime.storage_buffer_range = big_limit;
    CHECK_THROWS_AS(cache->StorageBufferBinding(big_descriptor, 0, true, true, false),
                    Shader::RuntimeError);
    runtime.storage_buffer_range = std::numeric_limits<u32>::max();
    CHECK(cache->StorageBufferBinding(big_descriptor, 0, true, true, false).size == mode_extent);

    constexpr u64 small_extent = 96 * 1024 * 1024;
    address_space.Write<u64>(big_descriptor + 8, small_extent);
    runtime.storage_buffer_range = big_limit;
    CHECK(cache->StorageBufferBinding(big_descriptor, 0, true, true, false).size == small_extent);
    runtime.storage_buffer_range = std::numeric_limits<u32>::max();
    CHECK(cache->StorageBufferBinding(big_descriptor, 0, true, true, false).size == small_extent);
    runtime.storage_buffer_range = big_limit;
}

TEST_CASE("KeplerCompute: DLSS dispatch chain ticks on the native launch path",
          "[video_core][buffer_cache][storage_binding]") {
    const bool old_multi_core = Settings::values.use_multi_core.GetValue();
    SCOPE_EXIT { Settings::values.use_multi_core.SetValue(old_multi_core); };
    Settings::values.use_multi_core.SetValue(false);
    Core::System system;
    system.Initialize();
    Kernel::KProcess process(system.Kernel());
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& cpu = process.GetMemory();
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(32, 12);
    cpu.SetCurrentPageTable(process);
    constexpr u64 phys = 0x80000, guest = 0x200000, device = 0x100000;
    constexpr u64 extent = 0x60000;
    system.DeviceMemory().buffer.ClearBackingRegion(phys, extent, 0);
    cpu.MapMemoryRegion(table, Common::ProcessAddress{guest}, extent,
                        Common::PhysicalAddress{Core::DramMemoryMap::Base + phys},
                        Common::MemoryPermission::ReadWrite, false);
    Tegra::MaxwellDeviceMemoryManager manager(system.DeviceMemory());
    manager.Map(device, guest, extent, manager.RegisterProcess(&cpu));
    Tegra::GPU gpu(system, false, false);
    BindingRasterizer rasterizer(gpu);
    Tegra::MemoryManager address_space(system, manager);
    address_space.BindRasterizer(&rasterizer);

    Tegra::MacroEngine macro{false};
    Tegra::Engines::KeplerCompute kepler(address_space);
    kepler.BindRasterizer(&rasterizer);
    kepler.SetNativeMacroMode(&macro, {});
    CHECK_FALSE(kepler.IsDlssDispatch());

    constexpr GPUVAddr qmd_address = 0x500000;
    address_space.Map(qmd_address, device + 0x40000, 0x1000, Tegra::PTEKind::PITCH, false);
    Tegra::NativeQmdScheduler::Descriptor qmd{};
    qmd[12] = 1;
    qmd[13] = 1;
    qmd[14] = 1;
    qmd[18] = (1U << 16) | 0x30;
    qmd[19] = (1U << 16) | 1;
    kepler.CallMethod(system, 0xad, u32(qmd_address >> 8), true);
    const auto launch = [&] {
        address_space.WriteBlockUnsafe(qmd_address, qmd.data(), sizeof(qmd));
        kepler.CallMethod(system, 0xb0, 3, true);
    };

    SECTION("the first dispatch after the output pass ends the chain") {
        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(0x4cbf6eff32525aecULL); };
        launch();
        CHECK(kepler.IsDlssDispatch());
        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(0x14b4222bbe23abeeULL); };
        launch();
        CHECK(kepler.IsDlssDispatch());
        rasterizer.on_dispatch = nullptr;
        launch();
        CHECK_FALSE(kepler.IsDlssDispatch());
    }

    SECTION("launches whose shader is rejected still count toward the bound") {
        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(0x4cbf6eff32525aecULL); };
        launch();
        REQUIRE(kepler.IsDlssDispatch());
        rasterizer.on_dispatch = [] { throw Shader::RuntimeError{"rejected"}; };
        for (u32 i = 0; i < 64; ++i) {
            launch();
        }
        CHECK(kepler.IsDlssDispatch());
        launch();
        CHECK_FALSE(kepler.IsDlssDispatch());
    }

    SECTION("an oversized native binding rejects the launch before any enqueue") {
        FixtureRuntime runtime;
        runtime.storage_buffer_range = 128 * 1024 * 1024;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetAddressSpace(address_space);
        constexpr GPUVAddr big_descriptor = 0x700000, big_target = 0x800000;
        address_space.Map(big_descriptor, device + 0x58000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Map(big_target, device + 0x80000, 256 * 1024 * 1024, Tegra::PTEKind::PITCH,
                          false);
        address_space.Write<u64>(big_descriptor, big_target);
        address_space.Write<u64>(big_descriptor + 8, 256 * 1024 * 1024);
        bool rejected = false, enqueued = false;
        rasterizer.on_dispatch = [&] {
            try {
                const auto binding =
                    cache->StorageBufferBinding(big_descriptor, 0, true, true, false);
                enqueued = binding.buffer_id != VideoCommon::NULL_BUFFER_ID;
            } catch (const Shader::RuntimeError&) {
                rejected = true;
            }
        };
        launch();
        CHECK(rejected);
        CHECK_FALSE(enqueued);
    }

    SECTION("unrepresentable native buffer allocations reject before mutating the cache") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetKeplerCompute(&kepler);
        cache->EnableChannelState();
        constexpr u64 base = 0x80000000;
        auto& channel = cache->Channel();
        channel.enabled_compute_storage_buffers = 1;
        channel.compute_storage_buffers[0] = VideoCommon::Binding{
            .device_addr = base + 0x1000,
            .size = std::numeric_limits<u32>::max(),
            .native_compute = true,
        };
        CHECK_THROWS_AS(cache->UpdateComputeBuffers(), Shader::RuntimeError);
        FixtureBuffer* const small = cache->Obtain(base, 0x1000);
        REQUIRE(small);
        const u64 small_addr = small->CpuAddr();
        CHECK(cache->Obtain(base + 0x80000000, 0x80000000));
        CHECK(cache->Obtain(base + 0x100000000, 0x80000000));
        channel.compute_storage_buffers[0] = VideoCommon::Binding{
            .device_addr = base + 0xc0000000,
            .size = 0x80000000,
            .native_compute = true,
        };
        CHECK_THROWS_AS(cache->UpdateComputeBuffers(), Shader::RuntimeError);
        CHECK(cache->Obtain(base + 0x100, 0x100)->CpuAddr() == small_addr);
        CHECK(cache->Obtain(base + 0x80000100, 0x100));
        CHECK(cache->Obtain(base + 0x2000, 0x1000));
        const size_t copies_before = runtime.copied.size();
        CHECK(cache->Obtain(base + 0x8000, 0x10000));
        CHECK(runtime.copied.size() > copies_before);
    }

    SECTION("the native reject policy reaches host resolution through the compute binding") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetKeplerCompute(&kepler);
        cache->EnableChannelState();
        constexpr u64 base = 0x80000000;
        CHECK(cache->Obtain(base + 0x80000000, 0x80000000));
        CHECK(cache->Obtain(base + 0x100000000, 0x80000000));
        auto& channel = cache->Channel();
        channel.enabled_compute_storage_buffers = 1;
        channel.compute_storage_buffers[0] = VideoCommon::Binding{
            .device_addr = base + 0xc0000000,
            .size = 0x80000000,
            .native_compute = true,
        };
        CHECK_THROWS_AS(cache->UpdateComputeBuffers(), Shader::RuntimeError);
        CHECK(cache->Obtain(base + 0x80000000, 0x1000));
        channel.enabled_compute_storage_buffers = 1;
        channel.compute_storage_buffers[0] = VideoCommon::Binding{
            .device_addr = base + 0xc0000000,
            .size = 0x80000000,
        };
        CHECK_NOTHROW(cache->UpdateComputeBuffers());
        channel.compute_storage_buffers[0] = VideoCommon::Binding{
            .device_addr = base + 0x40000,
            .size = 0x1000,
        };
        CHECK_NOTHROW(cache->UpdateComputeBuffers());
        CHECK_FALSE(cache->Obtain(base + 0x40000, 0x100)->sparse);
    }

    SECTION("unrepresentable native buffer allocations stay silent for graphics callers") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetKeplerCompute(&kepler);
        cache->EnableChannelState();
        constexpr u64 base = 0x80000000;
        CHECK(cache->Obtain(base + 0x1000, std::numeric_limits<u32>::max()));
    }

    SECTION("the DLSS trace records each bound SSBO with its resolved range and raw word") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetKeplerCompute(&kepler);
        cache->SetAddressSpace(address_space);
        cache->EnableChannelState();
        kepler.DlssTrace().SetTestEnabled(true);
        constexpr GPUVAddr desc = 0x700000, target = 0x800000;
        address_space.Map(desc, device + 0x58000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Map(target, device + 0x80000, 0x8000, Tegra::PTEKind::PITCH, false);
        address_space.Write<u64>(desc, target);
        address_space.Write<u64>(desc + 8, 0x1234);
        qmd[20] = 1;
        qmd[32] = u32(desc);
        qmd[33] = (u32(desc >> 32) & 0xff) | (((0x1000 >> 4) & 0x1fff) << 19);
        const auto bind = [&] { cache->BindComputeStorageBuffer(0, 0, 0, true); };
        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(0x4cbf6eff32525aecULL); };
        launch();
        rasterizer.on_dispatch = [&] {
            bind();
            kepler.NoteComputeProgram(0x14b4222bbe23abeeULL);
        };
        launch();
        rasterizer.on_dispatch = [&] {
            namespace diag = VideoCommon::ComputeBindingDiagnostic;
            diag::Record rec;
            rec.phase = diag::Phase::Dispatch;
            diag::Scope scope{rec};
            kepler.launch_description.const_buffer_enable_mask.Assign(0);
            cache->BindComputeStorageBuffer(0, 0, 0x50, true);
            kepler.launch_description.const_buffer_enable_mask.Assign(1);
            cache->BindComputeStorageBuffer(0, 0, 0x50, true);
        };
        launch();

        const auto& sequences = kepler.DlssTrace().Sequences();
        REQUIRE(sequences.size() == 1);
        const auto& entries = sequences.front().entries;
        REQUIRE(entries.size() == 3);
        REQUIRE(entries[1].ssbo_count == 1);
        const auto& record = entries[1].ssbos[0];
        CHECK(record.ssbo_index == 0);
        CHECK(record.declared_word == 0);
        REQUIRE(record.next_word.has_value());
        CHECK(*record.next_word == 0x1234);
        CHECK(record.chosen_size == 0x8000);
        CHECK(record.size_rule == StorageBufferSizeRule::BarePointer);
        CHECK(record.bound);
        CHECK(record.native_compute);
        CHECK(record.dlss_dispatch);
        CHECK(entries[2].Ordinary());
        CHECK(entries[2].outcome == Tegra::Engines::DlssDispatchTrace::Outcome::Keyless);
        REQUIRE(entries[2].ssbo_count == 1);
        CHECK(entries[2].ssbos[0].declared_word == 0);
        CHECK_FALSE(entries[2].ssbos[0].bound);
        CHECK_FALSE(entries[2].ssbos[0].dlss_dispatch);
    }

    SECTION("the trace reports a following word only when its guest pages are mapped") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetKeplerCompute(&kepler);
        cache->SetAddressSpace(address_space);
        cache->EnableChannelState();
        kepler.DlssTrace().SetTestEnabled(true);
        constexpr GPUVAddr desc = 0x700000, desc2 = 0x900000, target = 0x800000;
        address_space.Map(desc, device + 0x58000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Map(desc + 0x1000, device + 0x59000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Map(desc2, device + 0x5a000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Map(target, device + 0x41000, 0x8000, Tegra::PTEKind::PITCH, false);
        address_space.Write<u64>(desc + 0x50, target);
        address_space.Write<u64>(desc + 0x58, 0x1357);
        address_space.Write<u64>(desc + 0xfc0, target);
        address_space.Write<u64>(desc + 0xfc8, 0x2468);
        address_space.Write<u64>(desc + 0xff4, target);
        address_space.Write<u64>(desc + 0xffc, 0xabcd000000001234ULL);
        address_space.Write<u64>(desc2 + 0xff4, target);
        qmd[20] = 1;
        qmd[32] = u32(desc);
        qmd[33] = (u32(desc >> 32) & 0xff) | (((0x1000 >> 4) & 0x1fff) << 19);
        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(0x4cbf6eff32525aecULL); };
        launch();
        rasterizer.on_dispatch = [&] {
            cache->BindComputeStorageBuffer(0, 0, 0xff4, true);
            kepler.NoteComputeProgram(0x14b4222bbe23abeeULL);
        };
        launch();
        rasterizer.on_dispatch = [&] {
            namespace diag = VideoCommon::ComputeBindingDiagnostic;
            cache->BindComputeStorageBuffer(0, 0, 0xfc0, true);
            cache->BindComputeStorageBuffer(0, 0, 0xfc0, true);
            diag::Record rec;
            rec.phase = diag::Phase::Dispatch;
            {
                diag::Scope scope{rec};
                kepler.launch_description.const_buffer_enable_mask.Assign(0);
                cache->BindComputeStorageBuffer(2, 0, 0x50, true);
                kepler.launch_description.const_buffer_enable_mask.Assign(1);
                cache->BindComputeStorageBuffer(2, 0, 0x50, true);
            }
            auto* const saved = diag::current;
            diag::current = nullptr;
            cache->BindComputeStorageBuffer(3, 0, 0x50, true);
            diag::current = saved;
            kepler.launch_description.const_buffer_config[0].address_low = u32(desc2);
            kepler.launch_description.const_buffer_config[0].address_high.Assign(u32(desc2 >> 32));
            cache->BindComputeStorageBuffer(1, 0, 0xff4, true);
        };
        launch();

        const auto& sequences = kepler.DlssTrace().Sequences();
        REQUIRE(sequences.size() == 1);
        const auto& entries = sequences.front().entries;
        REQUIRE(entries.size() == 3);
        REQUIRE(entries[1].ssbo_count == 1);
        const auto& crossing = entries[1].ssbos[0];
        CHECK(crossing.ssbo_index == 0);
        CHECK(crossing.cbuf_offset == 0xff4);
        CHECK(crossing.declared_word == 0);
        CHECK(crossing.dlss_dispatch);
        REQUIRE(crossing.next_word.has_value());
        CHECK(*crossing.next_word == 0xabcd000000001234ULL);
        REQUIRE(entries[2].ssbo_count == 5);
        CHECK(entries[2].ssbos[0].declared_word == 0x2468);
        CHECK(entries[2].ssbos[0].next_word == 0x2468);
        CHECK(entries[2].ssbos[1].declared_word == 0x2468);
        CHECK(entries[2].ssbos[2].declared_word == 0);
        CHECK(entries[2].ssbos[2].cbuf_offset == 0x50);
        CHECK(entries[2].ssbos[3].declared_word == 0x1357);
        const auto& unmapped = entries[2].ssbos[4];
        CHECK(unmapped.ssbo_index == 1);
        CHECK_FALSE(unmapped.next_word.has_value());
        CHECK(runtime.range_queries == 10);
        Tegra::Engines::KeplerCompute untraced_kepler(address_space);
        untraced_kepler.launch_description.const_buffer_enable_mask.Assign(1);
        cache->SetKeplerCompute(&untraced_kepler);
        cache->BindComputeStorageBuffer(3, 0, 0x50, true);
    }

    SECTION("the trace keeps a cold and a warm sequence and marks each launch's outcome") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetKeplerCompute(&kepler);
        cache->SetAddressSpace(address_space);
        cache->EnableChannelState();
        kepler.DlssTrace().SetTestEnabled(true);
        using Trace = Tegra::Engines::DlssDispatchTrace;
        constexpr u64 prepass = Tegra::Engines::DlssDispatchChain::fortunes_weave_prepass_key;
        constexpr u64 output = Tegra::Engines::DlssDispatchChain::fortunes_weave_output_key;

        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(prepass, false); };
        launch();
        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(output, false); };
        launch();
        rasterizer.on_dispatch = [] {};
        launch();
        REQUIRE(kepler.DlssTrace().Sequences().size() == 1);
        const auto& cold = kepler.DlssTrace().Sequences().front();
        REQUIRE(cold.entries.size() == 3);
        CHECK(cold.entries[0].cache_warm == false);
        CHECK(cold.entries[0].outcome == Trace::Outcome::Succeeded);
        CHECK(cold.entries[2].outcome == Trace::Outcome::Keyless);
        CHECK_FALSE(kepler.DlssTrace().Emitted());
        CHECK_FALSE(kepler.DlssTraceCapturing());

        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(prepass, true); };
        launch();
        CHECK(kepler.DlssTraceCapturing());
        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(output, true); };
        launch();
        rasterizer.on_dispatch = [] { throw Shader::RuntimeError{"rejected"}; };
        launch();

        const auto& sequences = kepler.DlssTrace().Sequences();
        REQUIRE(sequences.size() == 2);
        CHECK(kepler.DlssTrace().Emitted());
        const auto& warm = sequences.back();
        REQUIRE(warm.entries.size() == 3);
        CHECK(warm.entries[0].cache_warm == true);
        CHECK(warm.entries[2].outcome == Trace::Outcome::Rejected);

        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(prepass, false); };
        launch();
        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(output, false); };
        launch();
        CHECK(kepler.DlssTrace().Sequences().size() == 2);
        CHECK(kepler.DlssTrace().Entries().empty());
    }

    SECTION("the trace records the actual host span of a multi-range binding") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetKeplerCompute(&kepler);
        cache->SetAddressSpace(address_space);
        cache->EnableChannelState();
        kepler.DlssTrace().SetTestEnabled(true);
        constexpr GPUVAddr desc = 0x700000, t1 = 0x810000;
        CHECK(cache->Obtain(device + 0x40000, 0x2000));
        address_space.Map(desc, device + 0x58000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Map(t1, device + 0x41000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Map(t1 + 0x1000, device + 0x50000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Write<u64>(desc, t1);
        address_space.Write<u64>(desc + 8, 0x2000);
        qmd[20] = 1;
        qmd[32] = u32(desc);
        qmd[33] = (u32(desc >> 32) & 0xff) | (((0x1000 >> 4) & 0x1fff) << 19);
        rasterizer.on_dispatch = [&] {
            kepler.NoteComputeProgram(
                Tegra::Engines::DlssDispatchChain::fortunes_weave_prepass_key, false);
        };
        launch();
        rasterizer.on_dispatch = [&] {
            kepler.NoteComputeProgram(
                Tegra::Engines::DlssDispatchChain::fortunes_weave_output_key, false);
        };
        launch();
        rasterizer.on_dispatch = [&] {
            cache->BindComputeStorageBuffer(0, 0, 0, true);
            cache->UpdateComputeBuffers();
            cache->BindHostComputeBuffers();
        };
        launch();

        const auto& sequences = kepler.DlssTrace().Sequences();
        REQUIRE(sequences.size() == 1);
        const auto& entries = sequences.front().entries;
        REQUIRE(entries.size() == 3);
        REQUIRE(entries[2].ssbo_count == 1);
        const auto& record = entries[2].ssbos[0];
        CHECK(record.ssbo_index == 0);
        CHECK(record.size_rule == StorageBufferSizeRule::Declared);
        CHECK(record.logical_range == 0x2000);
        CHECK(record.chosen_size == 0x1000);
        CHECK(record.bound);
        CHECK(record.host_path == 2);
        CHECK(record.host_segments == 2);
        CHECK(record.host_span == 0x2000);
        CHECK(record.host_offset == 0);
        CHECK(record.source_offset == 0x1000);
        REQUIRE(runtime.multi_range_binds.size() == 1);
        REQUIRE(runtime.multi_range_sources.size() == 2);
        CHECK(std::get<1>(runtime.multi_range_sources[0]) == 0x1000);
        CHECK(std::get<1>(runtime.multi_range_sources[1]) == 0);
    }

    SECTION("the trace reports the single span after an attempted multi-range bind falls back") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetKeplerCompute(&kepler);
        cache->SetAddressSpace(address_space);
        cache->EnableChannelState();
        kepler.DlssTrace().SetTestEnabled(true);
        runtime.multi_range_fail = true;
        constexpr GPUVAddr desc = 0x700000, t1 = 0x810000;
        address_space.Map(desc, device + 0x58000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Map(t1, device + 0x40000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Map(t1 + 0x1000, device + 0x50000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Write<u64>(desc, t1);
        address_space.Write<u64>(desc + 8, 0x2000);
        qmd[20] = 1;
        qmd[32] = u32(desc);
        qmd[33] = (u32(desc >> 32) & 0xff) | (((0x1000 >> 4) & 0x1fff) << 19);
        rasterizer.on_dispatch = [&] {
            kepler.NoteComputeProgram(
                Tegra::Engines::DlssDispatchChain::fortunes_weave_prepass_key, false);
        };
        launch();
        rasterizer.on_dispatch = [&] {
            kepler.NoteComputeProgram(
                Tegra::Engines::DlssDispatchChain::fortunes_weave_output_key, false);
        };
        launch();
        rasterizer.on_dispatch = [&] {
            cache->BindComputeStorageBuffer(0, 0, 0, true);
            cache->UpdateComputeBuffers();
            auto& channel = cache->Channel();
            channel.compute_storage_buffers[0].physical_size = 0;
            channel.compute_storage_buffers[0].buffer_id = VideoCommon::BufferId{};
            cache->BindHostComputeBuffers();
        };
        launch();

        const auto& sequences = kepler.DlssTrace().Sequences();
        REQUIRE(sequences.size() == 1);
        const auto& entries = sequences.front().entries;
        REQUIRE(entries.size() == 3);
        REQUIRE(entries[2].ssbo_count == 1);
        const auto& record = entries[2].ssbos[0];
        CHECK(record.bound);
        CHECK(record.host_path == 1);
        CHECK(record.host_segments == 1);
        CHECK(record.host_span == 0x2000);
        REQUIRE(runtime.multi_range_binds.size() == 1);
        REQUIRE(runtime.storage_binds.size() == 1);
        const auto& slot = cache->Channel().compute_storage_buffers[0];
        const auto& bound_buffer = cache->BoundBufferById(slot.buffer_id);
        CHECK_FALSE(bound_buffer.sparse);
        REQUIRE_FALSE(bound_buffer.marked.empty());
        CHECK(bound_buffer.marked.back() ==
              std::pair<u64, u64>{bound_buffer.Offset(slot.device_addr),
                                  slot.physical_size ? slot.physical_size : slot.size});
    }

    SECTION("the trace reports a null path for an enabled but unbound storage slot") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetKeplerCompute(&kepler);
        cache->SetAddressSpace(address_space);
        cache->EnableChannelState();
        kepler.DlssTrace().SetTestEnabled(true);
        constexpr GPUVAddr desc = 0x700000, t1 = 0x810000;
        address_space.Map(desc, device + 0x58000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Map(t1, device + 0x40000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Write<u64>(desc, t1);
        address_space.Write<u64>(desc + 8, 0x1000);
        qmd[20] = 1;
        qmd[32] = u32(desc);
        qmd[33] = (u32(desc >> 32) & 0xff) | (((0x1000 >> 4) & 0x1fff) << 19);
        REQUIRE(cache->Obtain(device + 0x40000, 0x1000));
        rasterizer.on_dispatch = [&] {
            kepler.NoteComputeProgram(
                Tegra::Engines::DlssDispatchChain::fortunes_weave_prepass_key, false);
        };
        launch();
        rasterizer.on_dispatch = [&] {
            kepler.NoteComputeProgram(
                Tegra::Engines::DlssDispatchChain::fortunes_weave_output_key, false);
        };
        launch();
        rasterizer.on_dispatch = [&] {
            cache->BindComputeStorageBuffer(0, 0, 0, true);
            auto& channel = cache->Channel();
            channel.enabled_compute_storage_buffers = 1;
            channel.compute_storage_buffers[0] = VideoCommon::Binding{
                .device_addr = device + 0x40000,
                .size = 0,
                .buffer_id = VideoCommon::BufferId{1},
            };
            cache->BindHostComputeBuffers();
        };
        launch();

        const auto& sequences = kepler.DlssTrace().Sequences();
        REQUIRE(sequences.size() == 1);
        const auto& entries = sequences.front().entries;
        REQUIRE(entries.size() == 3);
        REQUIRE(entries[2].ssbo_count == 1);
        const auto& record = entries[2].ssbos[0];
        CHECK_FALSE(record.bound);
        CHECK(record.host_path == 0);
        CHECK(record.host_offset == 0);
        CHECK(record.host_span == 0);
        CHECK(record.host_segments == 0);
        REQUIRE(runtime.storage_binds.size() == 1);
        CHECK(std::get<2>(runtime.storage_binds[0]) == 0);
    }

    SECTION("the host bind keeps a slot resolved to the null buffer") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetKeplerCompute(&kepler);
        cache->SetAddressSpace(address_space);
        cache->EnableChannelState();
        kepler.DlssTrace().SetTestEnabled(true);
        constexpr GPUVAddr desc = 0x700000, t1 = 0x810000;
        address_space.Map(desc, device + 0x58000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Map(t1, device + 0x40000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Write<u64>(desc, t1);
        address_space.Write<u64>(desc + 8, 0x1000);
        qmd[20] = 1;
        qmd[32] = u32(desc);
        qmd[33] = (u32(desc >> 32) & 0xff) | (((0x1000 >> 4) & 0x1fff) << 19);
        rasterizer.on_dispatch = [&] {
            kepler.NoteComputeProgram(
                Tegra::Engines::DlssDispatchChain::fortunes_weave_prepass_key, false);
        };
        launch();
        rasterizer.on_dispatch = [&] {
            kepler.NoteComputeProgram(
                Tegra::Engines::DlssDispatchChain::fortunes_weave_output_key, false);
        };
        launch();
        rasterizer.on_dispatch = [&] {
            cache->BindComputeStorageBuffer(0, 0, 0, false);
            cache->Channel().compute_storage_buffers[0].buffer_id =
                VideoCommon::NULL_BUFFER_ID;
            cache->BindHostComputeBuffers();
        };
        launch();

        const auto& sequences = kepler.DlssTrace().Sequences();
        REQUIRE(sequences.size() == 1);
        const auto& entries = sequences.front().entries;
        REQUIRE(entries.size() == 3);
        REQUIRE(entries[2].ssbo_count == 1);
        const auto& record = entries[2].ssbos[0];
        CHECK_FALSE(record.bound);
        CHECK(record.host_path == 0);
        CHECK(record.host_span == 0);
        REQUIRE(runtime.storage_binds.size() == 1);
        CHECK(std::get<0>(runtime.storage_binds[0]) == 0);
    }

    SECTION("the host bind re-resolves written and unwritten slots distinctly") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetKeplerCompute(&kepler);
        cache->SetAddressSpace(address_space);
        cache->EnableChannelState();
        kepler.DlssTrace().SetTestEnabled(true);
        constexpr GPUVAddr desc = 0x700000, t1 = 0x810000;
        address_space.Map(desc, device + 0x58000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Map(t1, device + 0x40000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Write<u64>(desc, t1);
        address_space.Write<u64>(desc + 8, 0x1000);
        qmd[20] = 1;
        qmd[32] = u32(desc);
        qmd[33] = (u32(desc >> 32) & 0xff) | (((0x1000 >> 4) & 0x1fff) << 19);
        rasterizer.on_dispatch = [&] {
            kepler.NoteComputeProgram(
                Tegra::Engines::DlssDispatchChain::fortunes_weave_prepass_key, false);
        };
        launch();
        rasterizer.on_dispatch = [&] {
            kepler.NoteComputeProgram(
                Tegra::Engines::DlssDispatchChain::fortunes_weave_output_key, false);
        };
        launch();
        rasterizer.on_dispatch = [&] {
            cache->BindComputeStorageBuffer(0, 0, 0, false);
            cache->BindComputeStorageBuffer(1, 0, 0, true);
            cache->UpdateComputeBuffers();
            auto& channel = cache->Channel();
            channel.compute_storage_buffers[0].device_addr = device + 0x90000;
            channel.compute_storage_buffers[0].buffer_id = VideoCommon::BufferId{};
            channel.compute_storage_buffers[1].physical_size = 0;
            channel.compute_storage_buffers[1].size = 5;
            channel.compute_storage_buffers[1].buffer_id = VideoCommon::BufferId{};
            cache->BindHostComputeBuffers();
        };
        launch();

        const auto& sequences = kepler.DlssTrace().Sequences();
        REQUIRE(sequences.size() == 1);
        const auto& entries = sequences.front().entries;
        REQUIRE(entries.size() == 3);
        REQUIRE(entries[2].ssbo_count == 2);
        const auto& record = entries[2].ssbos[0];
        CHECK(record.bound);
        CHECK(record.host_path == 1);
        CHECK(record.host_span != 0);
        REQUIRE(runtime.storage_binds.size() == 2);
        CHECK_FALSE(std::get<3>(runtime.storage_binds[0]));
        CHECK(std::get<3>(runtime.storage_binds[1]));
        CHECK(std::get<2>(runtime.storage_binds[1]) == 5);
        const auto& channel = cache->Channel();
        const auto& slot0 = channel.compute_storage_buffers[0];
        const auto& buf0 = cache->BoundBufferById(slot0.buffer_id);
        CHECK_FALSE(buf0.sparse);
        REQUIRE_FALSE(buf0.marked.empty());
        CHECK(buf0.marked.back() ==
              std::pair<u64, u64>{buf0.Offset(slot0.device_addr),
                                  slot0.physical_size ? slot0.physical_size : slot0.size});
        const auto& slot1 = channel.compute_storage_buffers[1];
        const auto& buf1 = cache->BoundBufferById(slot1.buffer_id);
        REQUIRE_FALSE(buf1.marked.empty());
        CHECK(buf1.marked.back() ==
              std::pair<u64, u64>{buf1.Offset(slot1.device_addr),
                                  slot1.physical_size ? slot1.physical_size : slot1.size});
    }

    SECTION("a host compute bind without a compute engine does no trace work") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetAddressSpace(address_space);
        cache->EnableChannelState();
        auto& channel = cache->Channel();
        channel.enabled_compute_storage_buffers = 1;
        channel.compute_storage_buffers[0] = VideoCommon::NULL_BINDING;
        CHECK_NOTHROW(cache->BindHostComputeBuffers());
    }

    SECTION("a cached native binding still reports the capped size rule") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetKeplerCompute(&kepler);
        cache->SetAddressSpace(address_space);
        cache->EnableChannelState();
        kepler.DlssTrace().SetTestEnabled(true);
        constexpr GPUVAddr desc = 0x700000, t1 = 0x810000;
        address_space.Map(desc, device + 0x58000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Map(t1, device + 0x40000, 0x40, Tegra::PTEKind::Z16, false);
        address_space.Map(t1 + 0x40, device + 0x40040, 0x200, Tegra::PTEKind::PITCH, false);
        address_space.Write<u64>(desc, t1 + 0x40);
        address_space.Write<u64>(desc + 8, 520);
        qmd[20] = 1;
        qmd[32] = u32(desc);
        qmd[33] = (u32(desc >> 32) & 0xff) | (((0x1000 >> 4) & 0x1fff) << 19);
        const auto bind = [&] { cache->BindComputeStorageBuffer(0, 0, 0, true); };
        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(0x4cbf6eff32525aecULL); };
        launch();
        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(0x14b4222bbe23abeeULL); };
        launch();
        rasterizer.on_dispatch = [&] {
            bind();
            bind();
        };
        launch();

        const auto& sequences = kepler.DlssTrace().Sequences();
        REQUIRE(sequences.size() == 1);
        const auto& entries = sequences.front().entries;
        REQUIRE(entries.size() == 3);
        REQUIRE(entries[2].ssbo_count == 2);
        const auto& cold = entries[2].ssbos[0];
        CHECK(cold.bound);
        CHECK(cold.size_rule == StorageBufferSizeRule::FallbackCap);
        CHECK(cold.declared_word == 520);
        CHECK(cold.logical_range == 0x240);
        CHECK(cold.chosen_size == 0x240);
        const auto& warm = entries[2].ssbos[1];
        CHECK(warm.bound);
        CHECK(warm.size_rule == StorageBufferSizeRule::FallbackCap);
        CHECK(warm.declared_word == 520);
        CHECK(warm.logical_range == 0x240);
        CHECK(warm.chosen_size == 0x240);
    }

    SECTION("the trace keeps the declared rule after the mapped prefix shrinks") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetKeplerCompute(&kepler);
        cache->SetAddressSpace(address_space);
        cache->EnableChannelState();
        kepler.DlssTrace().SetTestEnabled(true);
        constexpr GPUVAddr desc = 0x700000, t1 = 0x810000;
        address_space.Map(desc, device + 0x58000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Map(t1, device + 0x40000, 0x2000, Tegra::PTEKind::PITCH, false);
        address_space.Write<u64>(desc, t1);
        address_space.Write<u64>(desc + 8, 0x2000);
        qmd[20] = 1;
        qmd[32] = u32(desc);
        qmd[33] = (u32(desc >> 32) & 0xff) | (((0x1000 >> 4) & 0x1fff) << 19);
        const auto bind = [&] { cache->BindComputeStorageBuffer(0, 0, 0, true); };
        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(0x4cbf6eff32525aecULL); };
        launch();
        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(0x14b4222bbe23abeeULL); };
        launch();
        rasterizer.on_dispatch = [&] {
            bind();
            bind();
            rasterizer.on_mapping_change = [&] { bind(); };
            address_space.Map(t1, device + 0x70000, 0x1000, Tegra::PTEKind::PITCH, false);
            rasterizer.on_mapping_change = nullptr;
        };
        launch();

        const auto& sequences = kepler.DlssTrace().Sequences();
        REQUIRE(sequences.size() == 1);
        const auto& entries = sequences.front().entries;
        REQUIRE(entries.size() == 3);
        REQUIRE(entries[2].ssbo_count == 3);
        const auto& contiguous = entries[2].ssbos[0];
        CHECK(contiguous.bound);
        CHECK(contiguous.size_rule == StorageBufferSizeRule::Declared);
        CHECK(contiguous.logical_range == 0x2000);
        CHECK(contiguous.chosen_size == 0x2000);
        const auto& cached = entries[2].ssbos[1];
        CHECK(cached.bound);
        CHECK(cached.size_rule == StorageBufferSizeRule::Declared);
        CHECK(cached.declared_word == 0x2000);
        CHECK(cached.logical_range == 0x2000);
        CHECK(cached.chosen_size == 0x2000);
        const auto& shrunk = entries[2].ssbos[2];
        CHECK(shrunk.bound);
        CHECK(shrunk.size_rule == StorageBufferSizeRule::Declared);
        CHECK(shrunk.declared_word == 0x2000);
        CHECK(shrunk.logical_range == 0x2000);
        CHECK(shrunk.chosen_size == 0x1000);
    }

    SECTION("a null SSBO inside a DLSS chain reports the none rule") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetKeplerCompute(&kepler);
        cache->SetAddressSpace(address_space);
        cache->EnableChannelState();
        kepler.DlssTrace().SetTestEnabled(true);
        constexpr GPUVAddr desc = 0x700000;
        address_space.Map(desc, device + 0x58000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Write<u64>(desc, 0);
        address_space.Write<u64>(desc + 8, 0x1000);
        qmd[20] = 1;
        qmd[32] = u32(desc);
        qmd[33] = (u32(desc >> 32) & 0xff) | (((0x1000 >> 4) & 0x1fff) << 19);
        rasterizer.on_dispatch = [&] {
            kepler.NoteComputeProgram(
                Tegra::Engines::DlssDispatchChain::fortunes_weave_prepass_key, false);
        };
        launch();
        rasterizer.on_dispatch = [&] {
            kepler.NoteComputeProgram(
                Tegra::Engines::DlssDispatchChain::fortunes_weave_output_key, false);
        };
        launch();
        rasterizer.on_dispatch = [&] {
            cache->BindComputeStorageBuffer(0, 0, 0, true);
        };
        launch();

        const auto& sequences = kepler.DlssTrace().Sequences();
        REQUIRE(sequences.size() == 1);
        const auto& entries = sequences.front().entries;
        REQUIRE(entries.size() == 3);
        REQUIRE(entries[2].ssbo_count == 1);
        const auto& record = entries[2].ssbos[0];
        CHECK_FALSE(record.bound);
        CHECK(record.size_rule == StorageBufferSizeRule::NotDetermined);
        CHECK(record.logical_range == 0);
    }

    SECTION("a declared word beyond the mapped extent reports the capped rule") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->SetKeplerCompute(&kepler);
        cache->SetAddressSpace(address_space);
        cache->EnableChannelState();
        kepler.DlssTrace().SetTestEnabled(true);
        constexpr GPUVAddr desc = 0x700000, t1 = 0x810000;
        address_space.Map(desc, device + 0x58000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Map(t1, device + 0x40000, 0x1000, Tegra::PTEKind::PITCH, false);
        address_space.Write<u64>(desc, t1);
        address_space.Write<u64>(desc + 8, 0x2000);
        qmd[20] = 1;
        qmd[32] = u32(desc);
        qmd[33] = (u32(desc >> 32) & 0xff) | (((0x1000 >> 4) & 0x1fff) << 19);
        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(0x4cbf6eff32525aecULL); };
        launch();
        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(0x14b4222bbe23abeeULL); };
        launch();
        rasterizer.on_dispatch = [&] {
            cache->BindComputeStorageBuffer(0, 0, 0, true);
        };
        launch();

        const auto& sequences = kepler.DlssTrace().Sequences();
        REQUIRE(sequences.size() == 1);
        const auto& entries = sequences.front().entries;
        REQUIRE(entries.size() == 3);
        REQUIRE(entries[2].ssbo_count == 1);
        const auto& record = entries[2].ssbos[0];
        CHECK(record.bound);
        CHECK(record.size_rule == StorageBufferSizeRule::FallbackCap);
        CHECK(record.declared_word == 0x2000);
        CHECK(record.logical_range == 0x1000);
    }

    SECTION("one overlap scan per cache miss regardless of the reject policy") {
        FixtureRuntime runtime;
        auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
        cache->EnableChannelState();
        constexpr u64 base = 0x200000;
        for (u32 i = 0; i < 8; ++i) {
            cache->Obtain(base + i * 0x10000, 0x4000);
        }
        const u64 before = runtime.overlap_pages_scanned;
        CHECK(cache->Obtain(base, 0x80000));
        CHECK(runtime.overlap_pages_scanned - before == 8);
        auto& channel = cache->Channel();
        channel.enabled_compute_storage_buffers = 1;
        channel.compute_storage_buffers[0] = VideoCommon::Binding{};
        CHECK_NOTHROW(cache->UpdateComputeBuffers());
    }

    SECTION("launches without a shader program still count toward the bound") {
        rasterizer.on_dispatch = [&] { kepler.NoteComputeProgram(0x4cbf6eff32525aecULL); };
        launch();
        REQUIRE(kepler.IsDlssDispatch());
        rasterizer.on_dispatch = nullptr;
        for (u32 i = 0; i < 64; ++i) {
            launch();
        }
        CHECK(kepler.IsDlssDispatch());
        launch();
        CHECK_FALSE(kepler.IsDlssDispatch());
    }
}

TEST_CASE("BufferCache: readback spanning separate buffers", "[video_core][buffer_cache]") {
    const bool old_multi_core = Settings::values.use_multi_core.GetValue();
    SCOPE_EXIT {
        Settings::values.use_multi_core.SetValue(old_multi_core);
    };
    Settings::values.use_multi_core.SetValue(false);
    Core::System system;
    system.Initialize();
    Kernel::KProcess process(system.Kernel());
    SCOPE_EXIT {
        system.Kernel().UnregisterKernelObject(&process);
    };
    auto& cpu = process.GetMemory();
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(32, 12);
    cpu.SetCurrentPageTable(process);
    constexpr u64 phys = 0x80000, guest = 0x200000, dev = 0x70000, gap = 0x10000;
    system.DeviceMemory().buffer.ClearBackingRegion(phys, 3 * gap, 0);
    cpu.MapMemoryRegion(table, Common::ProcessAddress{guest}, 3 * gap,
                        Common::PhysicalAddress{Core::DramMemoryMap::Base + phys},
                        Common::MemoryPermission::ReadWrite, false);
    Tegra::MaxwellDeviceMemoryManager manager(system.DeviceMemory());
    auto asid = manager.RegisterProcess(&cpu);
    manager.Map(dev, guest, 3 * gap, asid);
    FixtureRuntime runtime;
    auto cache = std::make_unique<VideoCommon::BufferCache<FixtureParams>>(manager, runtime);
    auto* first =
        cache
            ->ObtainCPUBuffer(dev, gap, VideoCommon::ObtainBufferSynchronize::FullSynchronize,
                              VideoCommon::ObtainBufferOperation::MarkAsWritten)
            .first;
    std::fill(first->bytes.begin(), first->bytes.end(), 0x31);
    auto* second = cache
                       ->ObtainCPUBuffer(dev + 2 * gap, gap,
                                         VideoCommon::ObtainBufferSynchronize::FullSynchronize,
                                         VideoCommon::ObtainBufferOperation::MarkAsWritten)
                       .first;
    std::fill(second->bytes.begin(), second->bytes.end(), 0x72);
    runtime.copied.clear();
    REQUIRE(cache->IsRegionGpuModified(dev, gap));
    REQUIRE(cache->IsRegionGpuModified(dev + 2 * gap, gap));
    cache->DownloadMemory(dev + gap, gap);
    REQUIRE(runtime.copied.empty());
    REQUIRE(cache->IsRegionGpuModified(dev, gap));
    REQUIRE(cache->IsRegionGpuModified(dev + 2 * gap, gap));
    cache->DownloadMemory(dev, 3 * gap);
    CHECK_FALSE(runtime.out_of_bounds);
    const std::vector<std::pair<u64, u64>> expected{{dev, 0}, {dev + 2 * gap, 0}};
    CHECK(runtime.copied == expected);
    std::vector<u8> out1(gap), out2(gap);
    manager.ReadBlockUnsafe(dev, out1.data(), out1.size());
    manager.ReadBlockUnsafe(dev + 2 * gap, out2.data(), out2.size());
    CHECK(std::ranges::all_of(out1, [](u8 x) { return x == 0x31; }));
    CHECK(std::ranges::all_of(out2, [](u8 x) { return x == 0x72; }));
    CHECK_FALSE(cache->IsRegionGpuModified(dev, gap));
    CHECK_FALSE(cache->IsRegionGpuModified(dev + 2 * gap, gap));
}

TEST_CASE("BufferCache: downloads keep held-back CPU writes", "[video_core][buffer_cache]") {
    // Three buffers the GPU wrote (host bytes 0x31), three pages each, one per case below. In
    // each, the guest CPU writes code into the middle page and the core holds the write back
    // (OnCPUWrite) until the GPU thread applies it (a round of CachedWriteMemory).
    const bool old_multi_core = Settings::values.use_multi_core.GetValue();
    SCOPE_EXIT {
        Settings::values.use_multi_core.SetValue(old_multi_core);
    };
    Settings::values.use_multi_core.SetValue(false);
    Core::System system;
    system.Initialize();
    Kernel::KProcess process(system.Kernel());
    SCOPE_EXIT {
        system.Kernel().UnregisterKernelObject(&process);
    };
    auto& cpu = process.GetMemory();
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(32, 12);
    cpu.SetCurrentPageTable(process);
    constexpr u64 phys = 0x80000, guest = 0x200000, dev = 0x70000, size = 0x3000;
    system.DeviceMemory().buffer.ClearBackingRegion(phys, 3 * size, 0);
    cpu.MapMemoryRegion(table, Common::ProcessAddress{guest}, 3 * size,
                        Common::PhysicalAddress{Core::DramMemoryMap::Base + phys},
                        Common::MemoryPermission::ReadWrite, false);
    Tegra::MaxwellDeviceMemoryManager manager(system.DeviceMemory());
    manager.Map(dev, guest, 3 * size, manager.RegisterProcess(&cpu));
    FixtureRuntime runtime;
    auto cache = std::make_unique<VideoCommon::BufferCache<FixtureParams>>(manager, runtime);
    const auto gpu_write = [&](u64 base, u8 value) {
        auto* buffer = cache
                           ->ObtainCPUBuffer(base, static_cast<u32>(size),
                                             VideoCommon::ObtainBufferSynchronize::FullSynchronize,
                                             VideoCommon::ObtainBufferOperation::MarkAsWritten)
                           .first;
        std::fill(buffer->bytes.begin(), buffer->bytes.end(), value);
    };
    const auto cpu_write = [&](u64 base) {
        const std::vector<u8> code(0x100, 0xab);
        manager.WriteBlockUnsafe(base + 0x1080, code.data(), code.size());
        REQUIRE(cache->OnCPUWrite(base + 0x1080, code.size()));
    };
    const auto guest_byte = [&](u64 addr) {
        u8 value{};
        manager.ReadBlockUnsafe(addr, &value, 1);
        return value;
    };
    cache->NoteCpuRead(dev, 3 * size); // GPU writes there are copied back at fences

    // A readback of the whole buffer leaves the CPU's newer bytes alone.
    const u64 sync = dev;
    gpu_write(sync, 0x31);
    cpu_write(sync);
    cache->DownloadMemory(sync, size);
    CHECK(guest_byte(sync) == 0x31);
    CHECK(guest_byte(sync + 0x1080) == 0xab);
    CHECK(guest_byte(sync + 0x2000) == 0x31);
    // The round that applies the write releases the page: what the GPU writes there later is
    // read back.
    const auto apply_round = [&](auto&& during) {
        cache->BeginCachedWrites();
        during();
        cache->CachedWriteMemory(sync + 0x1080, 0x100);
        cache->EndCachedWrites();
    };
    apply_round([] {});
    gpu_write(sync, 0x5c);
    cache->DownloadMemory(sync, size);
    CHECK(guest_byte(sync + 0x1080) == 0x5c);
    // Unless the core held the page again during the round: that write waits for the next one.
    gpu_write(sync, 0x31);
    apply_round([&] { cpu_write(sync); });
    gpu_write(sync, 0x5c);
    cache->DownloadMemory(sync, size);
    CHECK(guest_byte(sync) == 0x5c);
    CHECK(guest_byte(sync + 0x1080) == 0xab);
    apply_round([] {});
    gpu_write(sync, 0x5c);
    cache->DownloadMemory(sync, size);
    CHECK(guest_byte(sync + 0x1080) == 0x5c);
    cache->CommitAsyncFlushes();
    cache->PopAsyncFlushes();

    // A fence download taken before the CPU write does not write over it when it pops.
    const u64 before = dev + size;
    gpu_write(before, 0x31);
    REQUIRE(cache->HasUncommittedFlushes());
    cache->CommitAsyncFlushes();
    cpu_write(before);
    cache->PopAsyncFlushes();
    CHECK(guest_byte(before) == 0x31);
    CHECK(guest_byte(before + 0x1080) == 0xab);

    // Nor does one committed while the write is still held back.
    const u64 held = dev + 2 * size;
    gpu_write(held, 0x31);
    cpu_write(held);
    cache->CommitAsyncFlushes();
    cache->PopAsyncFlushes();
    CHECK(guest_byte(held + 0x2000) == 0x31);
    CHECK(guest_byte(held + 0x1080) == 0xab);
    cache.reset();
}

TEST_CASE("BufferCache: obtained buffers count as used by this command buffer",
          "[video_core][buffer_cache]") {
    // DMA buffer/image copies, indirect dispatches and query writes get their buffer through
    // ObtainCPUBuffer. Unless the bytes are marked used, CanReorderUpload lets a later upload to
    // them in the same command buffer run in front of that GPU work (Yoshi and the Mysterious
    // Book: a texture copied from a buffer took the next upload's bytes for one frame).
    const bool old_multi_core = Settings::values.use_multi_core.GetValue();
    SCOPE_EXIT {
        Settings::values.use_multi_core.SetValue(old_multi_core);
    };
    Settings::values.use_multi_core.SetValue(false);
    Core::System system;
    system.Initialize();
    Kernel::KProcess process(system.Kernel());
    SCOPE_EXIT {
        system.Kernel().UnregisterKernelObject(&process);
    };
    auto& cpu = process.GetMemory();
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(32, 12);
    cpu.SetCurrentPageTable(process);
    constexpr u64 phys = 0x80000, guest = 0x200000, dev = 0x70000, size = 0x3000;
    system.DeviceMemory().buffer.ClearBackingRegion(phys, size, 0);
    cpu.MapMemoryRegion(table, Common::ProcessAddress{guest}, size,
                        Common::PhysicalAddress{Core::DramMemoryMap::Base + phys},
                        Common::MemoryPermission::ReadWrite, false);
    Tegra::MaxwellDeviceMemoryManager manager(system.DeviceMemory());
    manager.Map(dev, guest, size, manager.RegisterProcess(&cpu));
    FixtureRuntime runtime;
    auto cache = std::make_unique<VideoCommon::BufferCache<FixtureParams>>(manager, runtime);

    constexpr u64 addr = dev + 0x1100;
    constexpr u32 bytes = 0x340;
    const auto check = [&](VideoCommon::ObtainBufferSynchronize sync,
                           VideoCommon::ObtainBufferOperation op) {
        const auto [buffer, offset] = cache->ObtainCPUBuffer(addr, bytes, sync, op);
        REQUIRE(buffer->CpuAddr() + offset == addr);
        REQUIRE_FALSE(buffer->marked.empty());
        CHECK(buffer->marked.back() == std::pair<u64, u64>{offset, bytes});
    };
    // An upload source (the copy reads it) and a download target (the copy writes it).
    check(VideoCommon::ObtainBufferSynchronize::FullSynchronize,
          VideoCommon::ObtainBufferOperation::DoNothing);
    check(VideoCommon::ObtainBufferSynchronize::FullSynchronize,
          VideoCommon::ObtainBufferOperation::MarkAsWritten);
    check(VideoCommon::ObtainBufferSynchronize::NoSynchronize,
          VideoCommon::ObtainBufferOperation::DiscardWrite);

    // Uploads mark their bytes too. One that stays in this command buffer is otherwise overtaken
    // by a later upload to the same bytes, and its older data wins.
    const auto covered = [](const FixtureBuffer& buffer, u64 offset, u64 length) {
        return std::ranges::any_of(buffer.marked, [&](const std::pair<u64, u64>& mark) {
            return mark.first <= offset && offset + length <= mark.first + mark.second;
        });
    };
    constexpr u64 inline_addr = dev + 0x2040;
    const auto [written, written_offset] =
        cache->ObtainCPUBuffer(inline_addr, 0x80, VideoCommon::ObtainBufferSynchronize::FullSynchronize,
                               VideoCommon::ObtainBufferOperation::MarkAsWritten);
    written->marked.clear();
    const std::array<u8, 0x20> words{};
    REQUIRE(cache->InlineMemory(inline_addr + 0x10, words.size(), words));
    CHECK(covered(*written, written_offset + 0x10, words.size()));

    constexpr u64 cpu_addr = dev + 0x400;
    cache->WriteMemory(cpu_addr, 0x40);
    const auto [synced, synced_offset] =
        cache->ObtainCPUBuffer(cpu_addr, 0x40, VideoCommon::ObtainBufferSynchronize::FullSynchronize,
                               VideoCommon::ObtainBufferOperation::DoNothing);
    REQUIRE(synced->marked.size() >= 2);
    synced->marked.pop_back(); // ObtainCPUBuffer's own mark
    CHECK(covered(*synced, synced_offset, 0x40));
    cache.reset();
}

TEST_CASE("BufferCache: completed downloads are reusable without a frame",
          "[video_core][buffer_cache][download_lifetime]") {
    const bool old_multi_core = Settings::values.use_multi_core.GetValue();
    SCOPE_EXIT { Settings::values.use_multi_core.SetValue(old_multi_core); };
    Settings::values.use_multi_core.SetValue(false);
    Core::System system;
    system.Initialize();
    Kernel::KProcess process(system.Kernel());
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& cpu = process.GetMemory();
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(32, 12);
    cpu.SetCurrentPageTable(process);
    constexpr u64 phys = 0x80000, guest = 0x200000, dev = 0x70000, page = 0x1000;
    system.DeviceMemory().buffer.ClearBackingRegion(phys, 3 * page, 0);
    cpu.MapMemoryRegion(table, Common::ProcessAddress{guest}, 3 * page,
                        Common::PhysicalAddress{Core::DramMemoryMap::Base + phys},
                        Common::MemoryPermission::ReadWrite, false);
    Tegra::MaxwellDeviceMemoryManager manager(system.DeviceMemory());
    manager.Map(dev, guest, 3 * page, manager.RegisterProcess(&cpu));
    FixtureRuntime runtime;
    auto cache = std::make_unique<VideoCommon::BufferCache<FixtureParams>>(manager, runtime);
    const auto write = [&](DAddr address, u8 value) {
        const auto [buffer, offset] = cache->ObtainCPUBuffer(
            address, page, VideoCommon::ObtainBufferSynchronize::FullSynchronize,
            VideoCommon::ObtainBufferOperation::MarkAsWritten);
        std::fill_n(buffer->bytes.begin() + offset, page, value);
    };
    const auto check_guest = [&](DAddr address, u8 expected) {
        std::array<u8, page> bytes{};
        manager.ReadBlockUnsafe(address, bytes.data(), bytes.size());
        CHECK(std::ranges::all_of(bytes, [expected](u8 value) { return value == expected; }));
    };

    // Two fences are outstanding. A later allocation must not reuse either one's storage.
    write(dev, 0x31);
    cache->CommitAsyncFlushes();
    write(dev + page, 0x72);
    cache->CommitAsyncFlushes();
    REQUIRE(runtime.allocation_ids.size() == 2);
    CHECK(runtime.allocation_ids[0] != runtime.allocation_ids[1]);
    CHECK(runtime.freed_ids.empty());
    // These readbacks' fences completed, but unrelated newer work is still outstanding.
    // Retagging completed buffers with the current tick would prevent their immediate reuse.
    runtime.current_tick = 20;
    runtime.gpu_tick = 10;
    cache->PopAsyncFlushes();
    check_guest(dev, 0x31);
    // Popping happens on the fencing thread: release remains a backend operation.
    CHECK(runtime.freed_ids.empty());
    write(dev + 2 * page, 0x53);
    cache->CommitAsyncFlushes();
    REQUIRE(runtime.allocation_ids.size() == 3);
    REQUIRE(runtime.freed_ids.size() == 1);
    CHECK(runtime.freed_ids[0] == runtime.allocation_ids[0]);
    CHECK(runtime.allocation_ids[2] == runtime.allocation_ids[0]);
    CHECK(runtime.deferred_stages[runtime.allocation_ids[1]].active);
    cache->PopAsyncFlushes();
    check_guest(dev + page, 0x72);
    cache->PopAsyncFlushes();
    check_guest(dev + 2 * page, 0x53);

    // More compute-release fences in the same frame reuse completed storage indefinitely.
    for (u8 value : std::array<u8, 4>{0x64, 0x75, 0x86, 0x97}) {
        write(dev + 2 * page, value);
        cache->CommitAsyncFlushes();
        CHECK(runtime.deferred_stages.size() == 2);
        cache->PopAsyncFlushes();
        check_guest(dev + 2 * page, value);
    }
    CHECK_FALSE(runtime.out_of_bounds);
}

TEST_CASE("DeviceMemoryManager: translations distinguish reused manager lifetimes on another thread",
          "[video_core][buffer_cache][translation_lifetime]") {
    const bool old_multi_core = Settings::values.use_multi_core.GetValue();
    SCOPE_EXIT { Settings::values.use_multi_core.SetValue(old_multi_core); };
    Settings::values.use_multi_core.SetValue(false);
    Core::System system;
    system.Initialize();
    Kernel::KProcess process(system.Kernel());
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& cpu = process.GetMemory();
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(32, 12);
    cpu.SetCurrentPageTable(process);
    constexpr u64 phys = 0x80000, guest = 0x200000, dev = 0x70000, page = 0x1000;
    system.DeviceMemory().buffer.ClearBackingRegion(phys, page, 0x31);
    system.DeviceMemory().buffer.ClearBackingRegion(phys + page, page, 0x72);
    cpu.MapMemoryRegion(table, Common::ProcessAddress{guest}, 2 * page,
                        Common::PhysicalAddress{Core::DramMemoryMap::Base + phys},
                        Common::MemoryPermission::ReadWrite, false);

    using Manager = Tegra::MaxwellDeviceMemoryManager;
    alignas(Manager) std::array<std::byte, sizeof(Manager)> storage;
    auto* manager = std::construct_at(reinterpret_cast<Manager*>(storage.data()),
                                      system.DeviceMemory());
    manager->Map(dev, guest, page, manager->RegisterProcess(&cpu));
    const u64 first_generation = manager->MappingGeneration();
    std::array<u8, 6> observed{};
    std::barrier gate{2};
    std::thread reader([&] {
        observed[0] = manager->Read<u8>(dev);
        manager->ReadBlockUnsafe(dev + 1, &observed[1], 1);
        gate.arrive_and_wait(); // The worker's thread-local cache now holds the first mapping.
        gate.arrive_and_wait(); // Main has reconstructed a manager at the identical address.
        observed[2] = manager->Read<u8>(dev);
        manager->ReadBlockUnsafe(dev + 1, &observed[3], 1);
        gate.arrive_and_wait(); // Retain the second instance's cached mapping.
        gate.arrive_and_wait(); // The same instance now maps different backing.
        observed[4] = manager->Read<u8>(dev);
        manager->ReadBlockUnsafe(dev + 1, &observed[5], 1);
    });
    gate.arrive_and_wait();
    std::destroy_at(manager);
    manager = std::construct_at(reinterpret_cast<Manager*>(storage.data()), system.DeviceMemory());
    const auto second_asid = manager->RegisterProcess(&cpu);
    manager->Map(dev, guest + page, page, second_asid);
    const u64 second_generation = manager->MappingGeneration();
    gate.arrive_and_wait();
    gate.arrive_and_wait();
    // Map replaces the backing and increments the generation. Unmap also calls the GPU
    // interface, which this memory-only fixture deliberately does not create.
    manager->Map(dev, guest, page, second_asid);
    const u64 remapped_generation = manager->MappingGeneration();
    gate.arrive_and_wait();
    reader.join();
    std::destroy_at(manager);

    CHECK(first_generation == second_generation);
    CHECK(remapped_generation != second_generation);
    CHECK(observed == std::array<u8, 6>{0x31, 0x31, 0x72, 0x72, 0x31, 0x31});
}

TEST_CASE("BufferCache: pending download budget waits for delivery and supports cancellation",
          "[video_core][buffer_cache][download_budget]") {
    using namespace std::chrono_literals;
    const bool old_multi_core = Settings::values.use_multi_core.GetValue();
    SCOPE_EXIT { Settings::values.use_multi_core.SetValue(old_multi_core); };
    Settings::values.use_multi_core.SetValue(false);
    Core::System system;
    system.Initialize();
    Kernel::KProcess process(system.Kernel());
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& cpu = process.GetMemory();
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(32, 12);
    cpu.SetCurrentPageTable(process);
    constexpr u64 phys = 0x80000, guest = 0x200000, dev = 0x70000, page = 0x1000;
    system.DeviceMemory().buffer.ClearBackingRegion(phys, 2 * page, 0);
    cpu.MapMemoryRegion(table, Common::ProcessAddress{guest}, 2 * page,
                        Common::PhysicalAddress{Core::DramMemoryMap::Base + phys},
                        Common::MemoryPermission::ReadWrite, false);
    Tegra::MaxwellDeviceMemoryManager manager(system.DeviceMemory());
    manager.Map(dev, guest, 2 * page, manager.RegisterProcess(&cpu));
    FixtureRuntime runtime;
    auto cache = std::make_unique<VideoCommon::BufferCache<FixtureParams>>(manager, runtime);
    const auto commit = [&](u32 bytes, u8 value) {
        const auto [buffer, offset] = cache->ObtainCPUBuffer(
            dev, bytes, VideoCommon::ObtainBufferSynchronize::FullSynchronize,
            VideoCommon::ObtainBufferOperation::MarkAsWritten);
        std::fill_n(buffer->bytes.begin() + offset, bytes, value);
        cache->CommitAsyncFlushes();
    };
    const auto check_guest = [&](u32 bytes, u8 expected) {
        std::vector<u8> data(bytes);
        manager.ReadBlockUnsafe(dev, data.data(), data.size());
        CHECK(std::ranges::all_of(data, [expected](u8 value) { return value == expected; }));
    };
    const auto wait = [&](bool should_block, bool cancel) {
        std::promise<void> entered;
        auto started = entered.get_future();
        std::promise<bool> completed;
        auto result = completed.get_future();
        std::stop_source stop;
        std::jthread waiter([&] {
            entered.set_value();
            completed.set_value(cache->WaitAsyncFlushBudget(stop.get_token(), page));
        });
        // No Catch assertions until the worker has been cancelled and joined: even a failed
        // readiness/wakeup check must leave neither a blocked worker nor a dangling cache.
        const bool did_start = started.wait_for(2s) == std::future_status::ready;
        const auto initial = result.wait_for(should_block ? 30ms : 2s);
        if (should_block) {
            if (cancel) {
                stop.request_stop();
            } else {
                std::scoped_lock lock{cache->mutex};
                cache->PopAsyncFlushes();
            }
        }
        const bool ready = result.wait_for(2s) == std::future_status::ready;
        stop.request_stop();
        waiter.join();
        const bool success = result.get();
        REQUIRE(did_start);
        CHECK(initial == (should_block ? std::future_status::timeout : std::future_status::ready));
        REQUIRE(ready);
        CHECK(success == !cancel);
    };

    SECTION("reaching the budget blocks until the completed bytes reach guest memory") {
        commit(page, 0x31);
        check_guest(page, 0);
        wait(true, false);
        check_guest(page, 0x31);
        CHECK(runtime.freed_ids.empty()); // Delivery does not release staging on this thread.
    }
    SECTION("completion before waiting cannot lose the wakeup") {
        commit(page, 0x72);
        cache->PopAsyncFlushes();
        check_guest(page, 0x72);
        wait(false, false);
    }
    SECTION("cancellation leaves the pending copy available for later delivery") {
        commit(page, 0x53);
        wait(true, true);
        CHECK(runtime.freed_ids.empty());
        REQUIRE(cache->ShouldWaitAsyncFlushes());
        check_guest(page, 0);
        // A new wait still sees the pending bytes: cancellation neither drops the copy nor
        // forgets its budget. Its eventual pop delivers the original staging contents.
        wait(true, false);
        check_guest(page, 0x53);
    }
    SECTION("one oversized batch is admitted but stalls the following producer wait") {
        wait(false, false);
        commit(2 * page, 0x64);
        REQUIRE(runtime.allocation_ids.size() == 1);
        REQUIRE(runtime.deferred_stages.front().buffer->bytes.size() >= 2 * page);
        wait(true, false);
        check_guest(2 * page, 0x64);
        wait(false, false);
    }
    CHECK_FALSE(runtime.out_of_bounds);
}

TEST_CASE("BufferCache: fence downloads follow tiling and CPU visibility",
          "[video_core][buffer_cache][selective_download]") {
    const bool native_tiling = GENERATE(false, true);
    const bool old_multi_core = Settings::values.use_multi_core.GetValue();
    SCOPE_EXIT { Settings::values.use_multi_core.SetValue(old_multi_core); };
    Settings::values.use_multi_core.SetValue(false);
    Core::System system;
    system.Initialize();
    Kernel::KProcess process(system.Kernel());
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& cpu = process.GetMemory();
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(32, 12);
    cpu.SetCurrentPageTable(process);
    constexpr u64 phys = 0x80000, guest = 0x200000, dev = 0x70000;
    constexpr u32 page = 0x1000, extent = 8 * 1024 * 1024;
    system.DeviceMemory().buffer.ClearBackingRegion(phys, extent, 0);
    cpu.MapMemoryRegion(table, Common::ProcessAddress{guest}, extent,
                        Common::PhysicalAddress{Core::DramMemoryMap::Base + phys},
                        Common::MemoryPermission::ReadWrite, false);
    Tegra::MaxwellDeviceMemoryManager manager(system.DeviceMemory());
    manager.Map(dev, guest, extent, manager.RegisterProcess(&cpu));
    Tegra::MemoryManager address_space(system, manager);
    FixtureRuntime runtime;
    auto cache = std::make_unique<NativeFixtureCache>(manager, runtime);
    cache->SetAddressSpace(address_space);
    const auto write = [&](u8 value) {
        const auto [buffer, offset] = cache->ObtainCPUBuffer(
            dev, extent, VideoCommon::ObtainBufferSynchronize::FullSynchronize,
            VideoCommon::ObtainBufferOperation::MarkAsWritten);
        std::fill_n(buffer->bytes.begin() + offset, extent, value);
    };
    const auto check_page = [&](DAddr address, u8 expected) {
        std::array<u8, page> bytes{};
        manager.ReadBlockUnsafe(address, bytes.data(), bytes.size());
        CHECK(std::ranges::all_of(bytes, [expected](u8 value) { return value == expected; }));
    };

    if (!native_tiling) {
        write(0x64);
        cache->CommitAsyncFlushes();
        REQUIRE(runtime.allocation_ids.size() == 1);
        CHECK(runtime.deferred_stages.front().buffer->bytes.size() == extent);
        cache->PopAsyncFlushes();
        check_page(dev, 0x64);
        check_page(dev + extent - page, 0x64);
        CHECK_FALSE(runtime.out_of_bounds);
        return;
    }

    address_space.EnableNativeTiling();
    write(0x31);
    // The first ordered CPU read downloads one dirty page and marks it Preflushable.
    const DAddr read_page = dev + 3 * page;
    const auto first_read = cache->GetFlushArea(read_page, page);
    REQUIRE(first_read.has_value());
    REQUIRE_FALSE(first_read->preemtive);
    cache->DownloadMemory(first_read->start_address,
                          first_read->end_address - first_read->start_address);
    check_page(read_page, 0x31);

    SECTION("one preflushable page does not expand to the full guessed compute extent") {
        write(0x72);
        cache->CommitAsyncFlushes();
        REQUIRE(runtime.allocation_ids.size() == 1);
        CHECK(runtime.deferred_stages.front().buffer->bytes.size() == page);
        cache->PopAsyncFlushes();
        check_page(read_page, 0x72);
        const DAddr unread_page = dev + 7 * page;
        check_page(unread_page, 0);
        REQUIRE(cache->IsRegionGpuModified(unread_page, page));
        // Selective fence downloads must not lose a later first CPU read elsewhere.
        const auto later_read = cache->GetFlushArea(unread_page, page);
        REQUIRE(later_read.has_value());
        REQUIRE_FALSE(later_read->preemtive);
        cache->DownloadMemory(later_read->start_address,
                              later_read->end_address - later_read->start_address);
        check_page(unread_page, 0x72);
        // Enumeration must retain both persistent read marks for subsequent GPU writes.
        write(0x86);
        cache->CommitAsyncFlushes();
        REQUIRE(runtime.allocation_ids.size() == 2);
        const auto& repeated = runtime.deferred_stages[runtime.allocation_ids.back()];
        CHECK(repeated.buffer->bytes.size() == 2 * page);
        cache->PopAsyncFlushes();
        check_page(read_page, 0x86);
        check_page(unread_page, 0x86);
    }
    SECTION("preflushable and explicit CPU-read pages form a union without duplicate copies") {
        cache->NoteCpuRead(read_page, page); // Overlaps the preflushable mark.
        cache->NoteCpuRead(dev + 5 * page, page); // Separate explicit CPU-read page.
        write(0x53);
        cache->CommitAsyncFlushes();
        REQUIRE(runtime.allocation_ids.size() == 1);
        CHECK(runtime.deferred_stages.front().buffer->bytes.size() == 2 * page);
        cache->PopAsyncFlushes();
        check_page(read_page, 0x53);
        check_page(dev + 5 * page, 0x53);
        check_page(dev + 7 * page, 0);
    }
    CHECK_FALSE(runtime.out_of_bounds);
}
