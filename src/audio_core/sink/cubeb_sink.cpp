// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <condition_variable>
#include <span>
#include <vector>

#include "audio_core/common/common.h"
#include "audio_core/sink/cubeb_sink.h"
#include "audio_core/sink/sink_stream.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/core.h"

#ifdef _WIN32
#include <objbase.h>
#undef CreateEvent
#endif

namespace AudioCore::Sink {
/**
 * Cubeb sink stream, responsible for sinking samples to hardware.
 */
class CubebSinkStream final : public SinkStream {
public:
    /**
     * Create a new sink stream.
     *
     * @param ctx_             - Cubeb context to create this stream with.
     * @param device_channels_ - Number of channels supported by the hardware.
     * @param system_channels_ - Number of channels the audio systems expect.
     * @param output_device    - Cubeb output device id.
     * @param input_device     - Cubeb input device id.
     * @param name_            - Name of this stream.
     * @param type_            - Type of this stream.
     * @param system_          - Core system.
     * @param event            - Event used only for audio renderer, signalled on buffer consume.
     */
    CubebSinkStream(cubeb* ctx_, u32 device_channels_, u32 system_channels_,
                    cubeb_devid output_device_, cubeb_devid input_device_, const std::string& name_,
                    StreamType type_, Core::System& system_)
        : SinkStream(system_, type_), ctx{ctx_}, output_device{output_device_},
          input_device{input_device_} {
#ifdef _WIN32
        com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
        name = name_;
        device_channels = device_channels_;
        system_channels = system_channels_;

        if (ctx && !OpenBackend()) {
            device_lost = true;
            LOG_WARNING(Audio_Sink, "Cubeb stream {} failed to open, retrying", name);
        }
    }

    /**
     * Destroy the sink stream.
     */
    ~CubebSinkStream() override {
        LOG_DEBUG(Service_Audio, "Destructing cubeb stream {}", name);

        Finalize();

#ifdef _WIN32
        if (SUCCEEDED(com_init_result)) {
            CoUninitialize();
        }
#endif
    }

    /**
     * Finalize the sink stream.
     */
    void Finalize() override {
        std::scoped_lock lock{backend_mutex};
        finalized = true;
        SignalPause();
        CloseBackend();
    }

    void Start(bool resume = false) override {
        std::scoped_lock lock{backend_mutex};
        if (!ctx || finalized || !paused) {
            return;
        }
        paused = false;
        StartBackend();
    }

    void Stop() override {
        std::scoped_lock lock{backend_mutex};
        if (paused) {
            return;
        }
        SignalPause();
        if (stream_backend && cubeb_stream_stop(stream_backend) != CUBEB_OK) {
            device_lost = true;
        }
    }

    void Watch() {
        std::scoped_lock lock{backend_mutex};
        if (finalized) {
            return;
        }
        const auto now = Clock::now();
        if (!paused && stream_backend &&
            now - Clock::time_point{Clock::duration{last_callback.load()}} > std::chrono::seconds{5}) {
            if (++stalled_checks >= 2) {
                device_lost = true;
            }
        } else {
            stalled_checks = 0;
        }
        if (!device_lost) {
            return;
        }
        CloseBackend();
        device_lost = false;
        if (!OpenBackend()) {
            device_lost = true;
            return;
        }
        if (!paused) {
            StartBackend();
        }
        if (!device_lost) {
            LOG_INFO(Audio_Sink, "Reopened Cubeb stream {}", name);
        }
    }

private:
    using Clock = std::chrono::steady_clock;

    void StartBackend() {
        last_callback = Clock::now().time_since_epoch().count();
        stalled_checks = 0;
        if (stream_backend && cubeb_stream_start(stream_backend) != CUBEB_OK) {
            device_lost = true;
        }
    }

    void CloseBackend() {
        if (stream_backend) {
            cubeb_stream_stop(stream_backend);
            cubeb_stream_destroy(stream_backend);
            stream_backend = nullptr;
        }
    }

    bool OpenBackend() {
        cubeb_stream_params params{};
        params.rate = TargetSampleRate;
        params.channels = device_channels;
        params.format = CUBEB_SAMPLE_S16LE;
        params.prefs = CUBEB_STREAM_PREF_NONE;
        switch (params.channels) {
        case 1:
            params.layout = CUBEB_LAYOUT_MONO;
            break;
        case 2:
            params.layout = CUBEB_LAYOUT_STEREO;
            break;
        case 6:
            params.layout = CUBEB_LAYOUT_3F2_LFE;
            break;
        }

        u32 minimum_latency{0};
        const auto latency_error = cubeb_get_min_latency(ctx, &params, &minimum_latency);
        if (latency_error != CUBEB_OK) {
            LOG_CRITICAL(Audio_Sink, "Error getting minimum latency, error: {}", latency_error);
            minimum_latency = TargetSampleCount * 2;
        }

        minimum_latency = (std::max)(minimum_latency, TargetSampleCount * 2);

        LOG_INFO(Service_Audio,
                 "Opening cubeb stream {} type {} with: rate {} channels {} (system channels {}) "
                 "latency {}",
                 name, type, params.rate, params.channels, system_channels, minimum_latency);

        int init_error;
        cubeb_stream* opened{};
        if (type == StreamType::In) {
            init_error = cubeb_stream_init(ctx, &opened, name.c_str(), input_device,
                                           &params, output_device, nullptr, minimum_latency,
                                           &CubebSinkStream::DataCallback,
                                           &CubebSinkStream::StateCallback, this);
        } else {
            init_error = cubeb_stream_init(ctx, &opened, name.c_str(), input_device,
                                           nullptr, output_device, &params, minimum_latency,
                                           &CubebSinkStream::DataCallback,
                                           &CubebSinkStream::StateCallback, this);
        }

        if (init_error != CUBEB_OK) {
            LOG_DEBUG(Audio_Sink, "Error initializing cubeb stream {}, error: {}", name, init_error);
            return false;
        }
        stream_backend = opened;
        return true;
    }

    /**
     * Main callback from Cubeb. Either expects samples from us (audio render/audio out), or will
     * provide samples to be copied (audio in).
     *
     * @param stream      - Cubeb-specific data about the stream.
     * @param user_data   - Custom data pointer passed along, points to a CubebSinkStream.
     * @param in_buff     - Input buffer to be used if the stream is an input type.
     * @param out_buff    - Output buffer to be used if the stream is an output type.
     * @param num_frames_ - Number of frames of audio in the buffers. Note: Not number of samples.
     */
    static long DataCallback([[maybe_unused]] cubeb_stream* stream, void* user_data,
                             [[maybe_unused]] const void* in_buff, void* out_buff,
                             long num_frames_) {
        auto* impl = static_cast<CubebSinkStream*>(user_data);
        if (!impl) {
            return -1;
        }

        impl->last_callback.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
        const std::size_t num_channels = impl->GetDeviceChannels();
        const std::size_t frame_size = num_channels;
        const std::size_t num_frames{static_cast<size_t>(num_frames_)};

        if (impl->type == StreamType::In) {
            std::span<const s16> input_buffer{reinterpret_cast<const s16*>(in_buff),
                                              num_frames * frame_size};
            impl->ProcessAudioIn(input_buffer, num_frames);
        } else {
            std::span<s16> output_buffer{reinterpret_cast<s16*>(out_buff), num_frames * frame_size};
            impl->ProcessAudioOutAndRender(output_buffer, num_frames);
        }

        return num_frames_;
    }

    static void StateCallback(cubeb_stream*, void* user_data, cubeb_state state) {
        if (state == CUBEB_STATE_ERROR) {
            static_cast<CubebSinkStream*>(user_data)->device_lost = true;
        }
    }

    cubeb* ctx{};
    cubeb_stream* stream_backend{};
    cubeb_devid output_device{};
    cubeb_devid input_device{};
    std::mutex backend_mutex;
    std::atomic<bool> device_lost{};
    std::atomic<Clock::duration::rep> last_callback{};
    u32 stalled_checks{};
    bool finalized{};
#ifdef _WIN32
    HRESULT com_init_result{};
#endif
};

CubebSink::CubebSink(std::string_view target_device_name) {
    // Cubeb requires COM to be initialized on the thread calling cubeb_init on Windows
#ifdef _WIN32
    com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    if (cubeb_init(&ctx, "Eden", nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "cubeb_init failed");
        return;
    }

    if (target_device_name != auto_device_name && !target_device_name.empty()) {
        cubeb_device_collection collection;
        if (cubeb_enumerate_devices(ctx, CUBEB_DEVICE_TYPE_OUTPUT, &collection) != CUBEB_OK) {
            LOG_WARNING(Audio_Sink, "Audio output device enumeration not supported");
        } else {
            const auto collection_end{collection.device + collection.count};
            const auto device{
                std::find_if(collection.device, collection_end, [&](const cubeb_device_info& info) {
                    return info.friendly_name != nullptr &&
                           target_device_name == std::string(info.friendly_name);
                })};
            if (device != collection_end) {
                output_device = device->devid;
            }
            cubeb_device_collection_destroy(ctx, &collection);
        }
    }

    cubeb_get_max_channel_count(ctx, &device_channels);
    device_channels = device_channels >= 6U ? 6U : 2U;
    watchdog = std::jthread([this](std::stop_token stop) {
#ifdef _WIN32
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SCOPE_EXIT {
            if (SUCCEEDED(com)) {
                CoUninitialize();
            }
        };
#endif
        std::condition_variable_any wake;
        std::unique_lock lock{streams_mutex};
        while (!stop.stop_requested()) {
            wake.wait_for(lock, stop, std::chrono::seconds{1}, [] { return false; });
            if (stop.stop_requested()) {
                break;
            }
            for (auto& stream : sink_streams) {
                static_cast<CubebSinkStream*>(stream.get())->Watch();
            }
        }
    });
}

CubebSink::~CubebSink() {
    if (!ctx) {
        return;
    }

    watchdog.request_stop();
    watchdog.join();
    CloseStreams();
    cubeb_destroy(ctx);

#ifdef _WIN32
    if (SUCCEEDED(com_init_result)) {
        CoUninitialize();
    }
#endif
}

SinkStream* CubebSink::AcquireSinkStream(Core::System& system, u32 system_channels_,
                                         const std::string& name, StreamType type) {
    std::scoped_lock lock{streams_mutex};
    system_channels = system_channels_;
    SinkStreamPtr& stream = sink_streams.emplace_back(std::make_unique<CubebSinkStream>(
        ctx, device_channels, system_channels, output_device, input_device, name, type, system));
    stream->SetDeviceVolume(device_volume);

    return stream.get();
}

void CubebSink::CloseStream(SinkStream* stream) {
    std::scoped_lock lock{streams_mutex};
    for (size_t i = 0; i < sink_streams.size(); i++) {
        if (sink_streams[i].get() == stream) {
            sink_streams[i].reset();
            sink_streams.erase(sink_streams.begin() + i);
            break;
        }
    }
}

void CubebSink::CloseStreams() {
    std::scoped_lock lock{streams_mutex};
    sink_streams.clear();
}

f32 CubebSink::GetDeviceVolume() const {
    std::scoped_lock lock{streams_mutex};
    return device_volume;
}

void CubebSink::SetDeviceVolume(f32 volume) {
    std::scoped_lock lock{streams_mutex};
    device_volume = volume;
    for (auto& stream : sink_streams) {
        stream->SetDeviceVolume(volume);
    }
}

void CubebSink::SetSystemVolume(f32 volume) {
    std::scoped_lock lock{streams_mutex};
    for (auto& stream : sink_streams) {
        stream->SetSystemVolume(volume);
    }
}

std::vector<std::string> ListCubebSinkDevices(bool capture) {
    std::vector<std::string> device_list;
    cubeb* ctx;

#ifdef _WIN32
    auto com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    if (cubeb_init(&ctx, "Eden Device Enumerator", nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "cubeb_init failed");
        return {};
    }

#ifdef _WIN32
    if (SUCCEEDED(com_init_result)) {
        CoUninitialize();
    }
#endif

    auto type{capture ? CUBEB_DEVICE_TYPE_INPUT : CUBEB_DEVICE_TYPE_OUTPUT};
    cubeb_device_collection collection;
    if (cubeb_enumerate_devices(ctx, type, &collection) != CUBEB_OK) {
        LOG_WARNING(Audio_Sink, "Audio output device enumeration not supported");
    } else {
        for (std::size_t i = 0; i < collection.count; i++) {
            const cubeb_device_info& device = collection.device[i];
            if (device.friendly_name && device.friendly_name[0] != '\0' &&
                device.state == CUBEB_DEVICE_STATE_ENABLED) {
                device_list.emplace_back(device.friendly_name);
            }
        }
        cubeb_device_collection_destroy(ctx, &collection);
    }

    cubeb_destroy(ctx);
    return device_list;
}

/* REVERSION TO 3833 - function GetCubebLatency REINTRODUCED FROM 3833 - DIABLO 3 FIX */
u32 GetCubebLatency() {
    cubeb* ctx;

#ifdef _WIN32
    auto com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    // Init cubeb
    if (cubeb_init(&ctx, "yuzu Latency Getter", nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "cubeb_init failed");
        // Return a large latency so we choose SDL instead.
        return 10000u;
    }

#ifdef _WIN32
    if (SUCCEEDED(com_init_result)) {
        CoUninitialize();
    }
#endif

    // Get min latency
    cubeb_stream_params params{};
    params.rate = TargetSampleRate;
    params.channels = 2;
    params.format = CUBEB_SAMPLE_S16LE;
    params.prefs = CUBEB_STREAM_PREF_NONE;
    params.layout = CUBEB_LAYOUT_STEREO;

    u32 latency{0};
    const auto latency_error = cubeb_get_min_latency(ctx, &params, &latency);
    if (latency_error != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "Error getting minimum latency, error: {}", latency_error);
        latency = TargetSampleCount * 2;
    }
    latency = (std::max)(latency, TargetSampleCount * 2);
    cubeb_destroy(ctx);
    return latency;
}

// REVERTED back to 3833 - Below namespace section and function IsCubebSuitable() removed, reverting to GetCubebLatency() above. - DIABLO 3 FIX
/*
namespace {
static long TmpDataCallback(cubeb_stream*, void*, const void*, void*, long) {
    return TargetSampleCount;
}
static void TmpStateCallback(cubeb_stream*, void*, cubeb_state) {}
} // namespace

bool IsCubebSuitable() {
#if !defined(HAVE_CUBEB)
    return false;
#else
    cubeb* ctx{nullptr};

#ifdef _WIN32
    auto com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    // Init cubeb
    if (cubeb_init(&ctx, "Eden Latency Getter", nullptr) != CUBEB_OK) {
        LOG_ERROR(Audio_Sink, "Cubeb failed to init, it is not suitable.");
        return false;
    }

    SCOPE_EXIT {
        cubeb_destroy(ctx);
    };

#ifdef _WIN32
    if (SUCCEEDED(com_init_result)) {
        CoUninitialize();
    }
#endif

    // Get min latency
    cubeb_stream_params params{};
    params.rate = TargetSampleRate;
    params.channels = 2;
    params.format = CUBEB_SAMPLE_S16LE;
    params.prefs = CUBEB_STREAM_PREF_NONE;
    params.layout = CUBEB_LAYOUT_STEREO;

    u32 latency{0};
    const auto latency_error = cubeb_get_min_latency(ctx, &params, &latency);
    if (latency_error != CUBEB_OK) {
        LOG_ERROR(Audio_Sink, "Cubeb could not get min latency, it is not suitable.");
        return false;
    }
    latency = (std::max)(latency, TargetSampleCount * 2);

    // Test opening a device with standard parameters
    cubeb_devid output_device{0};
    cubeb_devid input_device{0};
    std::string name{"Eden test"};
    cubeb_stream* stream{nullptr};

    if (cubeb_stream_init(ctx, &stream, name.c_str(), input_device, nullptr, output_device, &params,
                          latency, &TmpDataCallback, &TmpStateCallback, nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "Cubeb could not open a device, it is not suitable.");
        return false;
    }

    cubeb_stream_stop(stream);
    cubeb_stream_destroy(stream);
    return true;
#endif
}
*/

} // namespace AudioCore::Sink
