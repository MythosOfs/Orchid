// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <mutex>
#include <span>
#include <vector>

#include <SDL3/SDL.h>

#include "audio_core/common/common.h"
#include "audio_core/sink/sdl3_sink.h"
#include "audio_core/sink/sink_stream.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/core.h"

namespace AudioCore::Sink {

namespace {

[[nodiscard]] bool InitializeAudio() {
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        // See https://github.com/PCSX2/pcsx2/pull/12312
        // "SDL and cubeb backends previously resulted in different names for the output which
        // caused them be identified as different applications by the OS."
        //
        // Keep in sync with cubeb_sink.cpp name.
        SDL_SetHint("SDL_AUDIO_DEVICE_APP_NAME", "yuzu Latency Getter");
#ifdef __ANDROID__
        SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "openslES", SDL_HINT_OVERRIDE);
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            LOG_WARNING(Audio_Sink, "OpenSL ES audio initialization failed: {}; retrying default drivers", SDL_GetError());
            SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
        }
#endif
        if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            LOG_CRITICAL(Audio_Sink, "SDL_InitSubSystem audio failed: {}", SDL_GetError());
            return false;
        }
        LOG_INFO(Audio_Sink, "SDL audio driver: {}", SDL_GetCurrentAudioDriver());
    }
    return true;
}

SDL_AudioDeviceID FindAudioDeviceByName(const std::string& device_name, bool capture) {
    int device_count = 0;
    SDL_AudioDeviceID* devices = capture ? SDL_GetAudioRecordingDevices(&device_count)
                                         : SDL_GetAudioPlaybackDevices(&device_count);
    if (devices == nullptr) {
        return capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    }

    SDL_AudioDeviceID selected = capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING
                                         : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    for (int i = 0; i < device_count; ++i) {
        const char* current_name = SDL_GetAudioDeviceName(devices[i]);
        if (current_name != nullptr && device_name == current_name) {
            selected = devices[i];
            break;
        }
    }
    SDL_free(devices);
    return selected;
}
} // Anonymous namespace

/**
 * SDL sink stream, responsible for sinking samples to hardware.
 */
class SDLSinkStream final : public SinkStream {
public:
    /**
     * Create a new sink stream.
     *
     * @param device_channels_ - Number of channels supported by the hardware.
     * @param system_channels_ - Number of channels the audio systems expect.
     * @param output_device    - Name of the output device to use for this stream.
     * @param input_device     - Name of the input device to use for this stream.
     * @param type_            - Type of this stream.
     * @param system_          - Core system.
     * @param event            - Event used only for audio renderer, signalled on buffer consume.
     */
    SDLSinkStream(u32 device_channels_, u32 system_channels_, const std::string& output_device,
                  const std::string& input_device, StreamType type_, Core::System& system_)
        : SinkStream{system_, type_},
          configured_device{type_ == StreamType::In ? input_device : output_device},
          default_device{type_ == StreamType::In ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING
                                                 : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK} {
        system_channels = system_channels_;
        device_channels = device_channels_;
        OpenBackend(DesiredDevice());
        watching = SDL_AddEventWatch(&SDLSinkStream::OnDeviceEvent, this);
        if (watching) {
            recovery = std::jthread([this](std::stop_token stop) {
                const std::stop_callback wake_on_stop{stop, [this] { RequestReopen(); }};
                while (!stop.stop_requested()) {
                    reopen.wait(false);
                    if (stop.stop_requested()) {
                        break;
                    }
                    reopen = false;
                    std::scoped_lock lock{backend_mutex};
                    if (!finalized) {
                        const auto desired = DesiredDevice();
                        if (force_reopen.exchange(false) || !stream || desired != bound_device) {
                            OpenBackend(desired);
                        }
                    }
                }
            });
        }
    }

    ~SDLSinkStream() override {
        if (watching) {
            SDL_RemoveEventWatch(&SDLSinkStream::OnDeviceEvent, this);
            recovery.request_stop();
            recovery.join();
        }
        Finalize();
    }

    void Finalize() override {
        std::scoped_lock lock{backend_mutex};
        finalized = true;
        SignalPause();
        CloseBackend();
        StopSilentDevice();
    }

    void Start(bool resume = false) override {
        std::scoped_lock lock{backend_mutex};
        if (finalized || !paused) {
            return;
        }
        paused = false;
        if (stream && !SDL_ResumeAudioStreamDevice(stream)) {
            force_reopen = true;
            RequestReopen();
        }
    }

    void Stop() override {
        std::scoped_lock lock{backend_mutex};
        if (paused) {
            return;
        }
        SignalPause();
        if (stream) {
            static_cast<void>(SDL_PauseAudioStreamDevice(stream));
        }
    }

private:
    void RequestReopen() {
        reopen = true;
        reopen.notify_one();
    }

    static bool SDLCALL OnDeviceEvent(void* user, SDL_Event* event) {
        if (event->type != SDL_EVENT_AUDIO_DEVICE_ADDED &&
            event->type != SDL_EVENT_AUDIO_DEVICE_REMOVED) {
            return true;
        }
        auto& self = *static_cast<SDLSinkStream*>(user);
        if (event->adevice.recording != (self.type == StreamType::In)) {
            return true;
        }
        const auto bound = self.bound_device.load();
        if (event->type == SDL_EVENT_AUDIO_DEVICE_REMOVED) {
            if (bound != self.default_device && bound == event->adevice.which) {
                self.force_reopen = true;
                self.RequestReopen();
            }
        } else if (bound == 0 || (bound == self.default_device && !self.configured_device.empty())) {
            self.RequestReopen();
        }
        return true;
    }

    void CloseBackend() {
        if (stream) {
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
        }
    }

    SDL_AudioDeviceID DesiredDevice() const {
        return configured_device.empty()
                   ? default_device
                   : FindAudioDeviceByName(configured_device, type == StreamType::In);
    }

    void OpenBackend(SDL_AudioDeviceID requested) {
        CloseBackend();
        StopSilentDevice();
        const SDL_AudioSpec spec{SDL_AUDIO_S16, static_cast<int>(device_channels),
                                 TargetSampleRate};
        for (const auto candidate : {requested, default_device}) {
            bound_device = candidate;
            stream = SDL_OpenAudioDeviceStream(candidate, &spec, &SDLSinkStream::DataCallback, this);
            if (stream && (paused || SDL_ResumeAudioStreamDevice(stream))) {
                SDL_AudioSpec stream_out{};
                static_cast<void>(SDL_GetAudioStreamFormat(stream, nullptr, &stream_out));
                LOG_INFO(Service_Audio,
                         "Opening SDL stream {} with: rate {} channels {} (system channels {}) "
                         "format {}",
                         static_cast<const void*>(stream), stream_out.freq, stream_out.channels,
                         system_channels, static_cast<int>(stream_out.format));
                return;
            }
            CloseBackend();
            if (candidate == default_device) {
                break;
            }
        }
        bound_device = 0;
        LOG_WARNING(Audio_Sink, "Error opening SDL audio device: {}, playing silence instead",
                    SDL_GetError());
        StartSilentDevice();
    }

    /**
     * Main callback from SDL. Either expects samples from us (audio render/audio out), or will
     * provide samples to be copied (audio in).
     *
     * @param userdata - Custom data pointer passed along, points to a SDLSinkStream.
     * @param stream   - Buffer of samples to be filled or read.
     * @param len      - Length of the stream in bytes.
     */
    static void DataCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                             int total_amount) {
        auto* impl = static_cast<SDLSinkStream*>(userdata);

        if (!impl) {
            return;
        }

        const std::size_t num_channels = impl->GetDeviceChannels();
        const std::size_t frame_size = num_channels;

        if (impl->type == StreamType::In) {
            const int bytes_available = SDL_GetAudioStreamAvailable(stream);
            if (bytes_available <= 0) {
                return;
            }

            std::vector<s16> input(bytes_available / static_cast<int>(sizeof(s16)));
            const int bytes_read = SDL_GetAudioStreamData(stream, input.data(), bytes_available);
            if (bytes_read <= 0) {
                return;
            }

            const std::size_t num_frames =
                static_cast<std::size_t>(bytes_read) / sizeof(s16) / frame_size;
            std::span<const s16> input_buffer{input.data(),
                                              static_cast<std::size_t>(bytes_read) / sizeof(s16)};
            impl->ProcessAudioIn(input_buffer, num_frames);
        } else {
            if (additional_amount <= 0 && total_amount <= 0) {
                return;
            }

            const int bytes_requested = additional_amount > 0 ? additional_amount : total_amount;
            std::vector<s16> output(bytes_requested / static_cast<int>(sizeof(s16)));
            const std::size_t num_frames =
                static_cast<std::size_t>(bytes_requested) / sizeof(s16) / frame_size;
            std::span<s16> output_buffer{output.data(), output.size()};
            impl->ProcessAudioOutAndRender(output_buffer, num_frames);
            static_cast<void>(SDL_PutAudioStreamData(stream, output.data(), bytes_requested));
        }
    }

    const std::string configured_device;
    const SDL_AudioDeviceID default_device;
    SDL_AudioStream* stream{};
    std::atomic<SDL_AudioDeviceID> bound_device{};
    std::atomic<bool> reopen{};
    std::atomic<bool> force_reopen{};
    bool watching{};
    bool finalized{};
    std::mutex backend_mutex;
    std::jthread recovery;
};

SDLSink::SDLSink(std::string_view target_device_name) {
    if (InitializeAudio()) {
        if (target_device_name != auto_device_name && !target_device_name.empty()) {
            output_device = target_device_name;
        } else {
            output_device.clear();
        }
        device_channels = 2;
    }
}

SDLSink::~SDLSink() = default;

SinkStream* SDLSink::AcquireSinkStream(Core::System& system, u32 system_channels_,
                                       const std::string&, StreamType type) {
    system_channels = system_channels_;
    SinkStreamPtr& stream = sink_streams.emplace_back(std::make_unique<SDLSinkStream>(
        device_channels, system_channels, output_device, input_device, type, system));
    stream->SetDeviceVolume(device_volume);
    return stream.get();
}

void SDLSink::CloseStream(SinkStream* stream) {
    for (size_t i = 0; i < sink_streams.size(); i++) {
        if (sink_streams[i].get() == stream) {
            sink_streams[i].reset();
            sink_streams.erase(sink_streams.begin() + i);
            break;
        }
    }
}

void SDLSink::CloseStreams() {
    sink_streams.clear();
}

f32 SDLSink::GetDeviceVolume() const {
    return device_volume;
}

void SDLSink::SetDeviceVolume(f32 volume) {
    device_volume = volume;
    for (auto& stream : sink_streams) {
        stream->SetDeviceVolume(volume);
    }
}

void SDLSink::SetSystemVolume(f32 volume) {
    for (auto& stream : sink_streams) {
        stream->SetSystemVolume(volume);
    }
}

std::vector<std::string> ListSDLSinkDevices(bool capture) {
    if (!InitializeAudio())
        return {}; //no devices

    std::vector<std::string> device_list;
    int device_count = 0;
    SDL_AudioDeviceID* devices =
        capture ? SDL_GetAudioRecordingDevices(&device_count)
                : SDL_GetAudioPlaybackDevices(&device_count);
    if (devices == nullptr) {
        return device_list;
    }

    for (int i = 0; i < device_count; ++i) {
        if (const char* name = SDL_GetAudioDeviceName(devices[i])) {
            device_list.emplace_back(name);
        }
    }
    SDL_free(devices);

    return device_list;
}

/* REVERSION to 3833 - function GetSDLLatency() REINTRODUCED FROM 3833 - DIABLO 3 FIX */
u32 GetSDLLatency() {
    return TargetSampleCount * 2;
}

// REVERTED back to 3833 - Below function IsSDLSuitable() removed, reverting to GetSDLLatency() above. - DIABLO 3 FIX
/*
bool IsSDLSuitable() {
#if !defined(HAVE_SDL3)
    return false;
#else
    // Check SDL can init
    if (!InitializeAudio()!
        return false;

    // We can set any latency frequency we want with SDL, so no need to check that.

    // Check we can open a device with standard parameters
    SDL_AudioSpec spec;
    spec.freq = TargetSampleRate;
    spec.channels = 2u;
    spec.format = AUDIO_S16SYS;
    spec.samples = TargetSampleCount * 2;
    spec.callback = nullptr;
    spec.userdata = nullptr;

    SDL_AudioSpec obtained;
    auto device = SDL_OpenAudioDevice(nullptr, false, &spec, &obtained, false);

    if (device == 0) {
        LOG_ERROR(Audio_Sink, "SDL failed to open a device, it is not suitable. Error: {}",
                  SDL_GetError());
        return false;
    }

    SDL_CloseAudioDevice(device);
    return true;
#endif
}
*/

} // namespace AudioCore::Sink
