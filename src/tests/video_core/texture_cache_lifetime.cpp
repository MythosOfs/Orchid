#include <algorithm>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "video_core/control/channel_state.h"
#include "video_core/gpu.h"
#include "video_core/renderer_null/null_rasterizer.h"
#include "video_core/texture_cache/texture_cache.h"

namespace {

struct TestRuntime {};

struct TestImage : VideoCommon::ImageBase {
    explicit TestImage(const VideoCommon::NullImageParams& params) : ImageBase(params) {}
    TestImage(TestRuntime&, const VideoCommon::ImageInfo& info, GPUVAddr gpu_addr, VAddr cpu_addr)
        : ImageBase(info, gpu_addr, cpu_addr) {}
};

struct TestImageView : VideoCommon::ImageViewBase {
    TestImageView(TestRuntime&, const VideoCommon::NullImageViewParams& params)
        : ImageViewBase(params) {}
    template <class Images>
    TestImageView(TestRuntime&, const VideoCommon::ImageViewInfo& info, VideoCommon::ImageId id,
                  VideoCommon::ImageBase& image, Images&)
        : ImageViewBase(info, image.info, id, image.gpu_addr) {}
};

struct TestSampler {
    TestSampler(TestRuntime&, const Tegra::Texture::TSCEntry&) {}
};

struct Policy {
    using Runtime = TestRuntime;
    using Image = TestImage;
    using ImageAlloc = VideoCommon::ImageAllocBase;
    using ImageView = TestImageView;
    using Sampler = TestSampler;
    struct Framebuffer {};
    struct AsyncBuffer {};
    struct BufferType {};
    static constexpr bool ENABLE_VALIDATION = false;
    static constexpr bool FRAMEBUFFER_BLITS = false;
    static constexpr bool HAS_EMULATED_COPIES = false;
    static constexpr bool HAS_DEVICE_MEMORY_INFO = false;
    static constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS = false;
    static constexpr bool HAS_MSAA_DOWNLOADS = false;
};

using Cache = VideoCommon::TextureCache<Policy>;

VideoCommon::ImageInfo MakeImageInfo() {
    VideoCommon::ImageInfo info{};
    info.type = VideoCommon::ImageType::e2D;
    info.format = VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;
    info.size = {64, 32, 1};
    info.resources = {1, 1};
    info.num_samples = 1;
    return info;
}

struct Fixture {
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device{physical};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    TestRuntime runtime;
    Tegra::MemoryManager memory{system, device, 32, 1ULL << 31, 16};
    Tegra::MemoryManager other_memory{system, device, 32, 1ULL << 31, 16};
    Cache cache{runtime, device};
    Tegra::Control::ChannelState channel{1};
    Tegra::Control::ChannelState other_channel{2};

    Fixture() {
        memory.BindRasterizer(&rasterizer);
        other_memory.BindRasterizer(&rasterizer);
    }

    void CreateChannels() {
        channel.memory_manager = &memory;
        other_channel.memory_manager = &other_memory;
        channel.Init(system, 0);
        other_channel.Init(system, 0);
        cache.CreateChannel(channel);
        cache.CreateChannel(other_channel);
    }
};

}

namespace VideoCommon {

struct TextureCacheTestAccess {
    template <typename F>
    static void ForEachSparseSegment(Cache& cache, Tegra::MemoryManager& memory, ImageBase& image,
                                     F&& visit) {
        cache.gpu_memory = &memory;
        cache.ForEachSparseSegment(image, std::forward<F>(visit));
    }

    static ImageId Register(Cache& cache, const ImageInfo& info, GPUVAddr gpu_addr, DAddr cpu_addr) {
        const ImageId image_id = cache.slot_images.insert(cache.runtime, info, gpu_addr, cpu_addr);
        cache.RegisterImage(image_id);
        return image_id;
    }

    static void Unregister(Cache& cache, ImageId image_id) {
        cache.UnregisterImage(image_id);
    }

    static bool IsFullClear(Cache& cache, ImageId image_id) {
        ImageBase& image = cache.slot_images[image_id];
        const ImageViewInfo info{ImageViewType::e2D, image.info.format};
        return cache.IsFullClear(cache.slot_image_views.insert(cache.runtime, info, image_id, image,
                                                               cache.slot_images));
    }

    static bool WriteBack(Cache& cache, ImageId image_id, const std::vector<u8>& data) {
        const ImageBase& image = cache.slot_images[image_id];
        return cache.WriteBackImage(image, FullDownloadCopies(image.info), data);
    }

    static size_t PagedImages(Cache& cache, size_t address_space_index) {
        size_t count{};
        for (const auto& [page, images] : cache.gpu_page_table_storage[address_space_index * 2]) {
            count += images.size();
        }
        return count;
    }
};

}

TEST_CASE("Sparse texture iteration skips stale translations and visits later segments",
          "[video_core][texture_cache][sparse_segments]") {
    Fixture fixture;
    constexpr GPUVAddr base = 0x200000;
    constexpr size_t page_size = 0x1000;
    constexpr DAddr invalid = DAddr{1} << Tegra::MaxwellDeviceMemoryManager::AS_BITS;
    constexpr DAddr valid = 0x20000;
    fixture.memory.Map(base, invalid, page_size, Tegra::PTEKind::PITCH, false);
    fixture.memory.Map(base + page_size, valid, page_size, Tegra::PTEKind::PITCH, false);

    VideoCommon::ImageBase image{MakeImageInfo(), base, valid};
    REQUIRE(fixture.memory.GetSubmappedRange(base, image.guest_size_bytes).size() == 2);
    REQUIRE_FALSE(fixture.memory.GpuToCpuAddress(base));

    std::vector<GPUVAddr> visited;
    VideoCommon::TextureCacheTestAccess::ForEachSparseSegment(
        fixture.cache, fixture.memory, image,
        [&](GPUVAddr address, DAddr device, size_t size) {
            visited.push_back(address);
            CHECK(device == valid);
            CHECK(size == page_size);
        });
    CHECK(visited == std::vector<GPUVAddr>{base + page_size});
}

TEST_CASE("UnregisterImage clears the page table of the address space the image was registered in",
          "[video_core][texture_cache][address_space]") {
    Fixture fixture;
    fixture.CreateChannels();
    constexpr GPUVAddr gpu_addr = 0x200000;
    constexpr DAddr cpu_addr = 0x20000;

    fixture.cache.BindToChannel(1);
    const auto image_id =
        VideoCommon::TextureCacheTestAccess::Register(fixture.cache, MakeImageInfo(), gpu_addr, cpu_addr);
    REQUIRE(VideoCommon::TextureCacheTestAccess::PagedImages(fixture.cache, 0) == 1);

    fixture.cache.BindToChannel(2);
    VideoCommon::TextureCacheTestAccess::Unregister(fixture.cache, image_id);
    CHECK(VideoCommon::TextureCacheTestAccess::PagedImages(fixture.cache, 0) == 0);
    CHECK(VideoCommon::TextureCacheTestAccess::PagedImages(fixture.cache, 1) == 0);
}

TEST_CASE("WriteBackImage writes through the memory manager of the image's own address space",
          "[video_core][texture_cache][address_space]") {
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
    constexpr u64 phys = 0x80000, guest = 0x200000, extent = 0x20000;
    constexpr DAddr first_device = 0x100000, second_device = 0x110000;
    system.DeviceMemory().buffer.ClearBackingRegion(phys, extent, 0);
    cpu.MapMemoryRegion(table, Common::ProcessAddress{guest}, extent,
                        Common::PhysicalAddress{Core::DramMemoryMap::Base + phys},
                        Common::MemoryPermission::ReadWrite, false);
    Tegra::MaxwellDeviceMemoryManager device(system.DeviceMemory());
    const auto asid = device.RegisterProcess(&cpu);
    device.Map(first_device, guest, 0x10000, asid);
    device.Map(second_device, guest + 0x10000, 0x10000, asid);
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    TestRuntime runtime;
    Cache cache{runtime, device};

    const auto first = std::make_shared<Tegra::MemoryManager>(system, device);
    auto second = std::make_shared<Tegra::MemoryManager>(system, device);
    first->BindRasterizer(&rasterizer);
    second->BindRasterizer(&rasterizer);
    constexpr GPUVAddr gpu_addr = 0x400000;
    first->Map(gpu_addr, first_device, 0x10000, Tegra::PTEKind::PITCH, false);
    second->Map(gpu_addr, second_device, 0x10000, Tegra::PTEKind::PITCH, false);
    Tegra::Control::ChannelState first_channel{1};
    Tegra::Control::ChannelState second_channel{2};
    first_channel.memory_manager = first.get();
    second_channel.memory_manager = second.get();
    first_channel.Init(system, 0);
    second_channel.Init(system, 0);
    cache.CreateChannel(first_channel);
    cache.CreateChannel(second_channel);

    cache.BindToChannel(2);
    const auto image_id = VideoCommon::TextureCacheTestAccess::Register(
        cache, MakeImageInfo(), gpu_addr, second_device);
    cache.BindToChannel(1);

    const auto data_of = [](u8 value) { return std::vector<u8>(64 * 32 * 4, value); };
    const auto contains = [&](DAddr address, u8 value) {
        std::vector<u8> bytes(64 * 32 * 4);
        device.ReadBlockUnsafe(address, bytes.data(), bytes.size());
        return std::ranges::find(bytes, value) != bytes.end();
    };
    CHECK(VideoCommon::TextureCacheTestAccess::WriteBack(cache, image_id, data_of(0xAB)));
    CHECK(contains(second_device, 0xAB));
    CHECK_FALSE(contains(first_device, 0xAB));

    second.reset();
    CHECK_FALSE(VideoCommon::TextureCacheTestAccess::WriteBack(cache, image_id, data_of(0xEF)));
    CHECK_FALSE(contains(second_device, 0xEF));
    CHECK_FALSE(contains(first_device, 0xEF));

    first->Unmap(gpu_addr, 0x10000);
    first->Map(gpu_addr, second_device, 0x10000, Tegra::PTEKind::PITCH, false);
    CHECK(VideoCommon::TextureCacheTestAccess::WriteBack(cache, image_id, data_of(0xCD)));
    CHECK(contains(second_device, 0xCD));
    CHECK_FALSE(contains(first_device, 0xCD));
}

TEST_CASE("IsFullClear needs the render area to reach the whole image",
          "[video_core][texture_cache][clear]") {
    Fixture fixture;
    fixture.CreateChannels();
    fixture.cache.BindToChannel(1);
    const auto image_id = VideoCommon::TextureCacheTestAccess::Register(
        fixture.cache, MakeImageInfo(), 0x200000, 0x20000);
    auto& regs = fixture.channel.payload->maxwell_3d.regs;
    regs.clear_control.use_scissor.Assign(0);
    regs.surface_clip.width.Assign(64);
    regs.surface_clip.height.Assign(32);
    CHECK(VideoCommon::TextureCacheTestAccess::IsFullClear(fixture.cache, image_id));

    regs.surface_clip.width.Assign(32);
    regs.surface_clip.height.Assign(16);
    CHECK_FALSE(VideoCommon::TextureCacheTestAccess::IsFullClear(fixture.cache, image_id));
}
