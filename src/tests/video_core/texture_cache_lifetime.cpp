#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "video_core/control/channel_state.h"
#include "video_core/gpu.h"
#include "video_core/renderer_null/null_rasterizer.h"
#include "video_core/texture_cache/texture_cache.h"

namespace {

struct TestRuntime {};

struct TestImage : VideoCommon::ImageBase {
    TestImage(TestRuntime&, const VideoCommon::ImageInfo& info, GPUVAddr gpu_addr, VAddr cpu_addr)
        : ImageBase(info, gpu_addr, cpu_addr) {}
};

struct TestImageView : VideoCommon::ImageViewBase {
    TestImageView(TestRuntime&, const VideoCommon::NullImageViewParams& params)
        : ImageViewBase(params) {}
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
