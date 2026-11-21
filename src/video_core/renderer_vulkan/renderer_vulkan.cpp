// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/ranges.h>

#include "common/display_hdr.h"
#include "common/logging.h"
#include <ranges>
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core_timing.h"
#include "core/frontend/graphics_context.h"
#include "video_core/capture.h"
#include "video_core/gpu.h"
#include "video_core/present.h"
#include "video_core/renderer_vulkan/present/util.h"
#ifdef HAS_RESHADE
#include "video_core/post_processing/fx_chain.h"
#endif
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_hitch_diagnostic.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/textures/decoders.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"
#include "video_core/vulkan_common/vulkan_debug_callback.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#ifdef __ANDROID__
#include <jni.h>
#endif
namespace Vulkan {
namespace {

constexpr VkExtent2D CaptureImageSize{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
};

#ifdef HAS_LSFG
[[nodiscard]] VkExtent2D GuestExtent(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (framebuffers.empty()) {
        return VkExtent2D{};
    }

    const auto& framebuffer = framebuffers.front();
    if (framebuffer.crop_rect.IsEmpty()) {
        return VkExtent2D{.width = framebuffer.width, .height = framebuffer.height};
    }
    return VkExtent2D{
        .width = static_cast<u32>(framebuffer.crop_rect.GetWidth()),
        .height = static_cast<u32>(framebuffer.crop_rect.GetHeight()),
    };
}
#endif

constexpr VkExtent3D CaptureImageExtent{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
    .depth = VideoCore::Capture::LinearDepth,
};

constexpr VkFormat CaptureFormat = VK_FORMAT_A8B8G8R8_UNORM_PACK32;

std::string GetReadableVersion(u32 version) {
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

std::string GetDriverVersion(const Device& device) {
    // Extracted from
    // https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5dddea46ea1120b0df14eef8f15ff8e318e35462/functions.php#L308-L314
    const u32 version = device.GetDriverVersion();

    if (device.GetDriverID() == VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (device.GetDriverID() == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }
    return GetReadableVersion(version);
}

std::string BuildCommaSeparatedExtensions(
    const std::set<std::string, std::less<>>& available_extensions) {
    return fmt::format("{}", fmt::join(available_extensions, ","));
}

// The swapchain format and colour space to present in: an HDR pair the surface lists while HDR
// output is on (Off/On/Auto with the monitor's state), else SDR. wanted_hdr, when given, tells
// whether HDR output is on, whatever the surface lists.
#ifdef __ANDROID__
// Always SDR: the present thread replaces the surface and its window there, so neither can be
// asked from here.
VkSurfaceFormatKHR ChooseOutputFormat(const vk::PhysicalDevice&, const vk::SurfaceKHR&,
                                      const Core::Frontend::EmuWindow&, bool,
                                      bool* wanted_hdr = nullptr, bool = false) {
    if (wanted_hdr) {
        *wanted_hdr = false;
    }
    return {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
}
#else
VkSurfaceFormatKHR ChooseOutputFormat(const vk::PhysicalDevice& physical,
                                      const vk::SurfaceKHR& surface,
                                      [[maybe_unused]] const Core::Frontend::EmuWindow& window,
                                      bool warn, bool* wanted_hdr = nullptr,
                                      bool sdr_only = false) {
    auto& monitor = Common::DisplayHdrMonitor::Instance();
#ifdef _WIN32
    if (Settings::values.hdr_mode.GetValue() != Settings::HdrMode::Off) {
        monitor.Watch(window.GetWindowInfo().render_surface);
    }
#endif
    const auto output = Common::ResolveHdrOutput(
        monitor.Current(), Settings::values.hdr_mode.GetValue(),
        Settings::values.hdr_peak_luminance.GetValue(),
        Settings::values.hdr_reference_white.GetValue());
    if (wanted_hdr) {
        *wanted_hdr = output.enabled;
    }
    if (output.enabled && !sdr_only) {
        try {
            const auto formats = physical.GetSurfaceFormatsKHR(*surface);
            const auto hdr =
                ChooseHdrSurfaceFormat(formats, Settings::values.hdr_swapchain.GetValue());
            if (hdr) {
                return *hdr;
            }
            if (warn) {
                LOG_WARNING(Render_Vulkan,
                            "HDR output is on, but the surface lists no HDR10 or scRGB format");
            }
        } catch (const vk::Exception& exception) {
            // A lost surface is the present thread's to recover; present SDR until it has.
            if (warn) {
                LOG_WARNING(Render_Vulkan, "HDR output is on, but the surface formats could not "
                                           "be read ({})",
                            exception.what());
            }
        }
    }
    return {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
}
#endif

} // Anonymous namespace

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld, VkSurfaceKHR surface) {
    const std::vector<VkPhysicalDevice> devices = instance.EnumeratePhysicalDevices();
    if (devices.empty()) {
        LOG_ERROR(Render_Vulkan, "No Vulkan devices found!");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    const auto is_cpu = [&](u32 index) {
        const auto type = vk::PhysicalDevice(devices[index], dld).GetProperties().deviceType;
        return type == VK_PHYSICAL_DEVICE_TYPE_CPU || type == VK_PHYSICAL_DEVICE_TYPE_OTHER;
    };
    // First real GPU, so a saved index never lands on a software renderer (llvmpipe) by
    // accident: the order of devices can change between runs and loaders.
    const auto first_gpu = [&]() -> std::optional<u32> {
        for (u32 i = 0; i < u32(devices.size()); ++i) {
            if (!is_cpu(i)) {
                return i;
            }
        }
        return std::nullopt;
    }();
    u32 device_index = Settings::values.vulkan_device.GetValue();
    if (device_index >= u32(devices.size())) {
        const u32 fallback = first_gpu.value_or(0);
        LOG_WARNING(Render_Vulkan, "Invalid device index {} ({} device(s) found), using device {}",
                    device_index, devices.size(), fallback);
        device_index = fallback;
    } else if (is_cpu(device_index) && first_gpu) {
        LOG_WARNING(Render_Vulkan,
                    "Device {} is a software renderer, using GPU device {} instead",
                    device_index, *first_gpu);
        device_index = *first_gpu;
    }
    const vk::PhysicalDevice physical_device(devices[device_index], dld);
    return Device(*instance, physical_device, surface, dld);
}

RendererVulkan::RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                               Tegra::MaxwellDeviceMemoryManager& device_memory_,
                               Tegra::GPU& gpu_,
                               std::unique_ptr<Core::Frontend::GraphicsContext> context_)
try
    : RendererBase(emu_window, std::move(context_))
    , device_memory(device_memory_)
    , gpu(gpu_)
    , library(OpenLibrary(context.get()))
    , dld()
    // Create raw Vulkan instance first
    , instance(CreateInstance(*library,
                            dld,
                            VK_API_VERSION_1_1,
                            render_window.GetWindowInfo().type,
                            Settings::values.renderer_debug.GetValue()))
    // Create debug messenger if debug is enabled
    , debug_messenger(Settings::values.renderer_debug ? CreateDebugUtilsCallback(instance)
                                                    : vk::DebugUtilsMessenger{})
    // Create surface
    , surface(CreateSurface(instance, render_window.GetWindowInfo()))
    , device(CreateDevice(instance, dld, *surface))
    , memory_allocator(device)
    , state_tracker()
    , scheduler(device, state_tracker)
    , swapchain(*surface,
                device,
                scheduler,
               render_window.GetFramebufferLayout().width,
               render_window.GetFramebufferLayout().height,
               ChooseOutputFormat(device.GetPhysical(), surface, render_window, true))
    , present_manager(instance,
                      render_window,
                      device,
                      memory_allocator,
                      scheduler,
                      swapchain,
                      surface)
    , blit_swapchain(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_capture(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_applet(device_memory,
                  device,
                  memory_allocator,
                  present_manager,
                  scheduler,
                  PresentFiltersForAppletCapture)
    , rasterizer(render_window, gpu, device_memory, device, memory_allocator, state_tracker, scheduler)
#ifdef HAS_LSFG
    , frame_gen(memory_allocator, scheduler)
#endif
{

    if (Settings::values.renderer_force_max_clock.GetValue() && device.ShouldBoostClocks()) {
        turbo_mode.emplace(instance, dld);
        scheduler.RegisterOnSubmit([this] { turbo_mode->QueueSubmitted(); });
    }

#ifdef HAS_RESHADE
    VideoCore::FxChain::Instance().LoadFromSettings();
#endif

    Report();
} catch (const vk::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "Vulkan initialization failed with error: {}", exception.what());
    throw std::runtime_error{fmt::format("Vulkan initialization error {}", exception.what())};
}

RendererVulkan::~RendererVulkan() {
    scheduler.RegisterOnSubmit([] {});
    void(device.GetLogical().WaitIdle());
}

void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {
    HitchTrace::Scope trace{HitchTrace::Event::Composite, framebuffers.size()};
    SCOPE_EXIT {
        render_window.OnFrameDisplayed();
    };

    RenderAppletCaptureLayer(framebuffers);

    if (!render_window.IsShown()) {
        return;
    }

    RenderScreenshot(framebuffers);
    Frame* frame = present_manager.GetRenderFrame();

    const HdrPresent output = UpdateHdrPresent();
    frame->color_space = output.color_space;

    scheduler.RequestOutsideRenderPassOperationContext();
    blit_swapchain.DrawToFrame(device, rasterizer, frame, framebuffers,
                              render_window.GetFramebufferLayout(),
                              present_manager.SwapchainImageCount(), output_format.format, output);

#ifdef HAS_LSFG
    if (output.IsHdr()) {
        frame_gen.Pause();
    } else {
        void(frame_gen.WantedGenerations(present_manager.MaxExtraFrames()));

        frame_gen.Process(device, frame, frame->image_format, GuestExtent(framebuffers));

        const size_t generated_frames = frame_gen.GeneratedFrameCount();
        for (size_t generation = 0; generation < generated_frames; ++generation) {
            Frame* generated = present_manager.GetRenderFrame();
            blit_swapchain.PrepareFrame(device, generated, render_window.GetFramebufferLayout());
            frame_gen.GenerateInto(device, generated, generation);
            scheduler.Flush(*generated->render_ready);
            present_manager.Present(generated);
        }
    }
#endif

    scheduler.Flush(*frame->render_ready);

    present_manager.Present(frame);
#ifdef HAS_LSFG
    scheduler.DispatchWork();
#endif

    gpu.RendererFrameEndNotify();
    rasterizer.TickFrame();
}

HdrPresent RendererVulkan::UpdateHdrPresent() {
    // The monitor's reading, not its swapchain state: the presenter's own switch between HDR and
    // SDR is no reason to ask the surface again.
    const auto& monitor = Common::DisplayHdrMonitor::Instance();
    const u64 key = (monitor.DisplayGeneration() << 8) |
                    (u64{static_cast<u8>(Settings::values.hdr_mode.GetValue())} << 4) |
                    static_cast<u8>(Settings::values.hdr_swapchain.GetValue());
    const u32 luminance_key = (u32{Settings::values.hdr_peak_luminance.GetValue()} << 16) |
                              Settings::values.hdr_reference_white.GetValue();
    const bool first = output_format_key == ~u64{};
    const bool settings_changed =
        !first && (key != output_format_key || luminance_key != output_luminance_key);
    // While HDR output is on but the surface offered no pair, ask again every few seconds: a
    // surface can gain one without a new swapchain (the compositor turned colour management on).
    constexpr u32 RetryFrames = 240;
    const bool retry = output_hdr_missing && ++output_retry_frames >= RetryFrames;
    // When the presenter had to create an SDR swapchain for an HDR frame, stay with SDR until the
    // next retry or a changed setting, whatever the surface lists meanwhile: a surface that keeps
    // changing its mind then costs a swapchain per retry, not one per frame.
    const u64 fallbacks = swapchain.GetHdrFallbackCount();
    const bool fell_back = fallbacks != output_fallback_count;
    if (fell_back) {
        output_fallback_count = fallbacks;
        output_sdr_only = true;
        output_retry_frames = 0;
    } else if (settings_changed || retry) {
        output_sdr_only = false;
    }
    // A new swapchain may sit on a surface that lists other formats: pick again after each.
    const u64 swapchain_count = swapchain.GetCreationCount();
    if (!first && !settings_changed && !retry && !fell_back &&
        swapchain_count == output_swapchain_count) {
        return {output_format.colorSpace, output_reference_white, output_peak};
    }
    output_format_key = key;
    output_luminance_key = luminance_key;
    output_swapchain_count = swapchain_count;
    if (retry) {
        output_retry_frames = 0;
    }
    const auto previous = output_format;
    bool wanted_hdr{};
    output_format = ChooseOutputFormat(device.GetPhysical(), surface, render_window,
                                       settings_changed, &wanted_hdr, output_sdr_only);
    output_hdr_missing = wanted_hdr && output_format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    const auto told = CaptureHdrPresent();
    output_reference_white = told.reference_white;
    output_peak = told.peak;
    if (previous.format != output_format.format ||
        previous.colorSpace != output_format.colorSpace) {
        if (output_format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            LOG_INFO(Render_Vulkan,
                     "HDR output: {} {}, reference white {} nits, peak {} nits. Anti-aliasing, "
                     "post-processing, upscaling filters and frame generation are off",
                     string_VkFormat(output_format.format),
                     string_VkColorSpaceKHR(output_format.colorSpace), output_reference_white,
                     output_peak);
        } else {
            LOG_INFO(Render_Vulkan, "HDR output: SDR {}", string_VkFormat(output_format.format));
        }
    }
    return {output_format.colorSpace, output_reference_white, output_peak};
}

HdrPresent RendererVulkan::CaptureHdrPresent() const {
    // The luminances the game was told (nx2:vi2 QueryDisplayHdrStatus / PaperWhite).
    const auto told = Common::ResolveHdrOutput(Common::DisplayHdrMonitor::Instance().Current(),
                                               Settings::HdrMode::On,
                                               Settings::values.hdr_peak_luminance.GetValue(),
                                               Settings::values.hdr_reference_white.GetValue());
    return {VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, static_cast<f32>(told.reference_white),
            static_cast<f32>(told.peak_luminance)};
}

void RendererVulkan::Report() const {
    using namespace Common::Literals;
    const std::string vendor_name{device.GetVendorName()};
    const std::string model_name{device.GetModelName()};
    const std::string driver_version = GetDriverVersion(device);
    const std::string driver_name = fmt::format("{} {}", vendor_name, driver_version);

    const std::string api_version = GetReadableVersion(device.ApiVersion());

    const std::string extensions = BuildCommaSeparatedExtensions(device.GetAvailableExtensions());

    const auto available_vram = static_cast<f64>(device.GetDeviceLocalMemory()) / f64{1_GiB};

    LOG_INFO(Render_Vulkan, "Driver: {}", driver_name);
    LOG_INFO(Render_Vulkan, "Device: {}", model_name);
    LOG_INFO(Render_Vulkan, "Vulkan: {}", api_version);
    LOG_INFO(Render_Vulkan, "VRAM limit for caches: {:.2f} GiB (driver budget minus a reserve, "
                            "capped unless VRAM usage is Aggressive)", available_vram);
}

vk::Buffer RendererVulkan::RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                                          const Layout::FramebufferLayout& layout, VkFormat format,
                                          VkDeviceSize buffer_size) {
    auto frame = [&]() {
        Frame f{};
        // Without the size, DrawToFrame takes every capture for a resize: a device wait-idle and
        // a rebuilt window adapt pass, layers and frame image each time.
        f.width = layout.width;
        f.height = layout.height;
        f.image =
            CreateWrappedImage(memory_allocator, VkExtent2D{layout.width, layout.height}, format);
        f.image_view = CreateWrappedImageView(device, f.image, format);
        f.framebuffer = blit_capture.CreateFramebuffer(device, layout, *f.image_view, format);
        return f;
    }();

    auto dst_buffer = CreateWrappedBuffer(memory_allocator, buffer_size, MemoryUsage::Download);
    blit_capture.DrawToFrame(device, rasterizer, &frame, framebuffers, layout, 1, format,
                             CaptureHdrPresent());

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *frame.image, *dst_buffer,
                           VkExtent3D{layout.width, layout.height, 1});
    });

    // Ensure the copy is fully completed before saving the capture
    scheduler.Finish();

    // Copy backing image data to the capture buffer
    dst_buffer.Invalidate();
    return dst_buffer;
}

void RendererVulkan::RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (!renderer_settings.screenshot_requested) {
        return;
    }

    const auto screenshot_layers = Tegra::FilterLayerStack(
        framebuffers, renderer_settings.screenshot_layer_stack, screenshot_layer_scratch);

    const auto& layout{renderer_settings.screenshot_framebuffer_layout};
    const auto dst_buffer = RenderToBuffer(screenshot_layers, layout, VK_FORMAT_B8G8R8A8_UNORM,
                                           layout.width * layout.height * 4);

    std::memcpy(renderer_settings.screenshot_bits, dst_buffer.Mapped().data(),
                dst_buffer.Mapped().size());
    renderer_settings.screenshot_complete_callback(false);
    renderer_settings.screenshot_requested = false;
}

std::vector<u8> RendererVulkan::GetAppletCaptureBuffer() {
    using namespace VideoCore::Capture;

    std::vector<u8> out(VideoCore::Capture::TiledSize);

    if (!applet_frame.image) {
        return out;
    }

    const auto dst_buffer =
        CreateWrappedBuffer(memory_allocator, VideoCore::Capture::TiledSize, MemoryUsage::Download);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *applet_frame.image, *dst_buffer, CaptureImageExtent);
    });

    // Ensure the copy is fully completed before writing the capture
    scheduler.Finish();

    // Swizzle image data to the capture buffer
    dst_buffer.Invalidate();
    Tegra::Texture::SwizzleTexture(out, dst_buffer.Mapped(), BytesPerPixel, LinearWidth,
                                   LinearHeight, LinearDepth, BlockHeight, BlockDepth);

    return out;
}

void RendererVulkan::RenderAppletCaptureLayer(
    std::span<const Tegra::FramebufferConfig> framebuffers) {
    const auto capture_layers = Tegra::FilterLayerStack(
        framebuffers, Service::Nvnflinger::LayerStackId::LastFrame, applet_capture_layers);

    if (capture_layers.empty())
        return;

    // Switch 2 titles present through nx2:vi2, and only the Switch 1 vi path and applets read
    // this capture (GetAppletCaptureBuffer): drawing it for every frame cost a framebuffer
    // lookup, a render pass break and a 1280x720 draw on the GPU thread. Keep it at most half a
    // second old for them instead. The first frame is always drawn.
    const bool switch2_layers = std::ranges::all_of(
        capture_layers, [](const Tegra::FramebufferConfig& layer) { return layer.native_tiling; });
    if (switch2_layers && applet_frame.image && (++native_capture_frames % 30) != 0) {
        return;
    }

    if (!applet_frame.image) {
        applet_frame.image = CreateWrappedImage(memory_allocator, CaptureImageSize, CaptureFormat);
        applet_frame.image_view = CreateWrappedImageView(device, applet_frame.image, CaptureFormat);
        applet_frame.framebuffer = blit_applet.CreateFramebuffer(device,
            VideoCore::Capture::Layout, *applet_frame.image_view, CaptureFormat);
    }

    // The capture is drawn every frame. With one set of layer resources each frame waited for
    // the host GPU to finish the previous one; rotating through three lets the GPU thread stay
    // ahead like the swapchain blit does.
    constexpr size_t CaptureResourceSets = 3;
    scheduler.RequestOutsideRenderPassOperationContext();
    blit_applet.DrawToFrame(device, rasterizer, &applet_frame, capture_layers, VideoCore::Capture::Layout,
                            CaptureResourceSets, CaptureFormat, CaptureHdrPresent());
}

} // namespace Vulkan
