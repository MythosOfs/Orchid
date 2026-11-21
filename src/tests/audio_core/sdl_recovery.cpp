// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <thread>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include "audio_core/sink/sdl3_sink.h"
#include "audio_core/sink/sink_stream.h"
#include "core/core.h"

namespace {
using namespace std::chrono_literals;

struct SdlDevice {
    std::atomic<SDL_AudioDeviceID> named{2};
    std::atomic<SDL_AudioDeviceID> opened{};
    std::atomic<unsigned> resumes{};
    std::atomic<unsigned> closes{};
    SDL_EventFilter watch{};
    void* watch_data{};
};

SdlDevice* device{};
SDL_AudioStream* const fake_stream = reinterpret_cast<SDL_AudioStream*>(1);

struct Fixture {
    Fixture() {
        device = &mock;
    }
    ~Fixture() {
        device = nullptr;
    }
    void Event(SDL_EventType type, SDL_AudioDeviceID id, bool capture = false) {
        SDL_Event event{};
        event.adevice.type = type;
        event.adevice.which = id;
        event.adevice.recording = capture;
        mock.watch(mock.watch_data, &event);
    }
    SdlDevice mock;
};

template <typename Predicate>
bool WaitFor(Predicate condition) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    return condition();
}
}

extern "C" {
SDL_InitFlags __real_SDL_WasInit(SDL_InitFlags);
SDL_AudioDeviceID* __real_SDL_GetAudioPlaybackDevices(int*);
const char* __real_SDL_GetAudioDeviceName(SDL_AudioDeviceID);
SDL_AudioStream* __real_SDL_OpenAudioDeviceStream(SDL_AudioDeviceID, const SDL_AudioSpec*,
                                                 SDL_AudioStreamCallback, void*);
bool __real_SDL_AddEventWatch(SDL_EventFilter, void*);
void __real_SDL_RemoveEventWatch(SDL_EventFilter, void*);
void __real_SDL_DestroyAudioStream(SDL_AudioStream*);
bool __real_SDL_ResumeAudioStreamDevice(SDL_AudioStream*);
bool __real_SDL_PauseAudioStreamDevice(SDL_AudioStream*);
bool __real_SDL_GetAudioStreamFormat(SDL_AudioStream*, SDL_AudioSpec*, SDL_AudioSpec*);

SDL_InitFlags __wrap_SDL_WasInit(SDL_InitFlags flags) {
    return device ? flags : __real_SDL_WasInit(flags);
}

SDL_AudioDeviceID* __wrap_SDL_GetAudioPlaybackDevices(int* count) {
    if (!device) {
        return __real_SDL_GetAudioPlaybackDevices(count);
    }
    const auto named = device->named.load();
    *count = named ? 1 : 0;
    auto* ids = static_cast<SDL_AudioDeviceID*>(SDL_malloc(2 * sizeof(SDL_AudioDeviceID)));
    ids[0] = named;
    ids[1] = 0;
    return ids;
}

const char* __wrap_SDL_GetAudioDeviceName(SDL_AudioDeviceID id) {
    if (!device) {
        return __real_SDL_GetAudioDeviceName(id);
    }
    return id == device->named ? "fixture device" : nullptr;
}

SDL_AudioStream* __wrap_SDL_OpenAudioDeviceStream(SDL_AudioDeviceID id, const SDL_AudioSpec* spec,
                                                 SDL_AudioStreamCallback callback, void* user) {
    if (!device) {
        return __real_SDL_OpenAudioDeviceStream(id, spec, callback, user);
    }
    device->opened = id;
    return fake_stream;
}

bool __wrap_SDL_AddEventWatch(SDL_EventFilter filter, void* user) {
    if (!device) {
        return __real_SDL_AddEventWatch(filter, user);
    }
    device->watch = filter;
    device->watch_data = user;
    return true;
}

void __wrap_SDL_RemoveEventWatch(SDL_EventFilter filter, void* user) {
    if (!device) {
        __real_SDL_RemoveEventWatch(filter, user);
    } else {
        device->watch = nullptr;
    }
}

void __wrap_SDL_DestroyAudioStream(SDL_AudioStream* stream) {
    if (stream != fake_stream) {
        __real_SDL_DestroyAudioStream(stream);
    } else {
        ++device->closes;
    }
}

bool __wrap_SDL_ResumeAudioStreamDevice(SDL_AudioStream* stream) {
    if (stream != fake_stream) {
        return __real_SDL_ResumeAudioStreamDevice(stream);
    }
    ++device->resumes;
    return true;
}

bool __wrap_SDL_PauseAudioStreamDevice(SDL_AudioStream* stream) {
    return stream == fake_stream || __real_SDL_PauseAudioStreamDevice(stream);
}

bool __wrap_SDL_GetAudioStreamFormat(SDL_AudioStream* stream, SDL_AudioSpec* in, SDL_AudioSpec* out) {
    return stream == fake_stream || __real_SDL_GetAudioStreamFormat(stream, in, out);
}
}

TEST_CASE("SDL recovers named playback after loss and returns to the device",
          "[audio_core][sdl_recovery]") {
    Fixture fixture;
    Core::System system;
    AudioCore::Sink::SDLSink sink("fixture device");
    auto* stream = sink.AcquireSinkStream(system, 2, "recovery-fixture",
                                          AudioCore::Sink::StreamType::Out);
    REQUIRE(fixture.mock.opened == 2);
    REQUIRE(fixture.mock.watch);
    stream->Start();
    REQUIRE(fixture.mock.resumes == 1);

    fixture.mock.named = 0;
    fixture.Event(SDL_EVENT_AUDIO_DEVICE_REMOVED, 2);
    REQUIRE(WaitFor([&] { return fixture.mock.opened == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK; }));
    REQUIRE(WaitFor([&] { return fixture.mock.resumes == 2; }));
    CHECK(fixture.mock.closes == 1);

    stream->Stop();
    fixture.mock.named = 4;
    fixture.Event(SDL_EVENT_AUDIO_DEVICE_ADDED, 4);
    REQUIRE(WaitFor([&] { return fixture.mock.opened == 4; }));
    stream->Finalize();
    stream->Finalize();
    CHECK(stream->IsPaused());
    CHECK(fixture.mock.resumes == 2);
    CHECK(fixture.mock.closes == 3);
    sink.CloseStream(stream);
    CHECK_FALSE(fixture.mock.watch);
    CHECK(fixture.mock.closes == 3);
}

TEST_CASE("SDL ignores removal of other devices and of its own default device",
          "[audio_core][sdl_recovery]") {
    Fixture fixture;
    Core::System system;
    SECTION("a named stream keeps its device when another one disappears") {
        AudioCore::Sink::SDLSink sink("fixture device");
        auto* stream = sink.AcquireSinkStream(system, 2, "other-removed",
                                              AudioCore::Sink::StreamType::Out);
        REQUIRE(fixture.mock.opened == 2);
        fixture.Event(SDL_EVENT_AUDIO_DEVICE_REMOVED, 9);
        std::this_thread::sleep_for(300ms);
        CHECK(fixture.mock.opened == 2);
        CHECK(fixture.mock.closes == 0);
        sink.CloseStream(stream);
    }
    SECTION("a stream on the default device is migrated by SDL itself") {
        AudioCore::Sink::SDLSink sink("auto");
        auto* stream = sink.AcquireSinkStream(system, 2, "default-removed",
                                              AudioCore::Sink::StreamType::Out);
        REQUIRE(fixture.mock.opened == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
        fixture.Event(SDL_EVENT_AUDIO_DEVICE_REMOVED, SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
        std::this_thread::sleep_for(300ms);
        CHECK(fixture.mock.closes == 0);
        sink.CloseStream(stream);
    }
}
