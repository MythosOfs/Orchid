// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <cubeb/cubeb.h>

#include "audio_core/sink/cubeb_sink.h"
#include "audio_core/sink/sink_stream.h"
#include "core/core.h"

namespace {
using namespace std::chrono_literals;

struct Device {
    std::atomic<unsigned> opens{};
    std::atomic<unsigned> starts{};
    std::atomic<unsigned> destroys{};
    std::atomic<bool> available{true};
    cubeb_state_callback callback{};
    void* user{};
};

Device* device{};
cubeb* const fake_context = reinterpret_cast<cubeb*>(1);
cubeb_stream* const fake_stream = reinterpret_cast<cubeb_stream*>(1);

struct Fixture {
    Fixture() {
        device = &mock;
    }
    ~Fixture() {
        device = nullptr;
    }
    Device mock;
};

template <typename Predicate>
bool WaitFor(Predicate condition, std::chrono::seconds limit = 4s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    return condition();
}
}

extern "C" {
int __real_cubeb_init(cubeb**, const char*, const char*);
void __real_cubeb_destroy(cubeb*);
int __real_cubeb_get_max_channel_count(cubeb*, uint32_t*);
int __real_cubeb_get_min_latency(cubeb*, cubeb_stream_params*, uint32_t*);
int __real_cubeb_stream_init(cubeb*, cubeb_stream**, const char*, cubeb_devid,
                            cubeb_stream_params*, cubeb_devid, cubeb_stream_params*, uint32_t,
                            cubeb_data_callback, cubeb_state_callback, void*);
void __real_cubeb_stream_destroy(cubeb_stream*);
int __real_cubeb_stream_start(cubeb_stream*);
int __real_cubeb_stream_stop(cubeb_stream*);

int __wrap_cubeb_init(cubeb** context, const char* name, const char* backend) {
    if (!device) {
        return __real_cubeb_init(context, name, backend);
    }
    *context = fake_context;
    return CUBEB_OK;
}

void __wrap_cubeb_destroy(cubeb* context) {
    if (context != fake_context) {
        __real_cubeb_destroy(context);
    }
}

int __wrap_cubeb_get_max_channel_count(cubeb* context, uint32_t* channels) {
    if (context != fake_context) {
        return __real_cubeb_get_max_channel_count(context, channels);
    }
    *channels = 2;
    return CUBEB_OK;
}

int __wrap_cubeb_get_min_latency(cubeb* context, cubeb_stream_params* params, uint32_t* latency) {
    if (context != fake_context) {
        return __real_cubeb_get_min_latency(context, params, latency);
    }
    *latency = 960;
    return CUBEB_OK;
}

int __wrap_cubeb_stream_init(cubeb* context, cubeb_stream** stream, const char* name,
                            cubeb_devid input, cubeb_stream_params* in_params, cubeb_devid output,
                            cubeb_stream_params* out_params, uint32_t latency,
                            cubeb_data_callback data, cubeb_state_callback state, void* user) {
    if (context != fake_context) {
        return __real_cubeb_stream_init(context, stream, name, input, in_params, output, out_params,
                                        latency, data, state, user);
    }
    const bool available = device->available;
    ++device->opens;
    if (!available) {
        return CUBEB_ERROR_DEVICE_UNAVAILABLE;
    }
    device->callback = state;
    device->user = user;
    *stream = fake_stream;
    return CUBEB_OK;
}

void __wrap_cubeb_stream_destroy(cubeb_stream* stream) {
    if (stream == fake_stream) {
        ++device->destroys;
    } else {
        __real_cubeb_stream_destroy(stream);
    }
}

int __wrap_cubeb_stream_start(cubeb_stream* stream) {
    if (stream != fake_stream) {
        return __real_cubeb_stream_start(stream);
    }
    ++device->starts;
    return CUBEB_OK;
}

int __wrap_cubeb_stream_stop(cubeb_stream* stream) {
    return stream == fake_stream ? CUBEB_OK : __real_cubeb_stream_stop(stream);
}
}

TEST_CASE("Cubeb retries a lost sink and honors pause and resume during reopen",
          "[audio_core][cubeb_recovery]") {
    Fixture fixture;
    Core::System system;
    AudioCore::Sink::CubebSink sink("auto");
    auto* stream = sink.AcquireSinkStream(system, 2, "recovery-fixture",
                                          AudioCore::Sink::StreamType::Out);
    REQUIRE(stream);
    REQUIRE(fixture.mock.opens == 1);
    stream->Start();
    REQUIRE(fixture.mock.starts == 1);
    fixture.mock.available = false;
    fixture.mock.callback(fake_stream, fixture.mock.user, CUBEB_STATE_ERROR);
    REQUIRE(WaitFor([&] { return fixture.mock.opens >= 2; }));
    CHECK(fixture.mock.destroys == 1);
    stream->Stop();
    CHECK(stream->IsPaused());
    unsigned expected_starts = 1;
    SECTION("a paused stream stays paused") {}
    SECTION("a resume is retained even when the backend is absent") {
        stream->Start();
        CHECK_FALSE(stream->IsPaused());
        expected_starts = 2;
    }
    fixture.mock.available = true;
    REQUIRE(WaitFor([&] { return fixture.mock.opens >= 3; }));
    if (expected_starts == 2) {
        REQUIRE(WaitFor([&] { return fixture.mock.starts >= expected_starts; }));
    }
    stream->Finalize();
    stream->Finalize();
    CHECK(fixture.mock.starts == expected_starts);
    CHECK(fixture.mock.destroys == 2);
    sink.CloseStream(stream);
    CHECK(fixture.mock.destroys == 2);
}

TEST_CASE("Cubeb reopens a started stream whose callbacks stop for more than five seconds",
          "[audio_core][cubeb_recovery]") {
    Fixture fixture;
    Core::System system;
    AudioCore::Sink::CubebSink sink("auto");
    auto* stream = sink.AcquireSinkStream(system, 2, "stall-fixture",
                                          AudioCore::Sink::StreamType::Out);
    REQUIRE(stream);
    stream->Start();
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(WaitFor([&] { return fixture.mock.opens >= 2; }, 9s));
    const auto stalled_for = std::chrono::steady_clock::now() - started;
    CHECK(stalled_for >= 6s);
    CHECK(fixture.mock.destroys == 1);
    stream->Finalize();
}
