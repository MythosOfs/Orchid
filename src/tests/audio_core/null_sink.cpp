// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "audio_core/sink/null_sink.h"
#include "audio_core/sink/sink_stream.h"
#include "core/core.h"

TEST_CASE("Null sink plays render output at the device rate", "[audio_core][null_sink]") {
    Core::System system;
    AudioCore::Sink::NullSink null{"auto"};
    auto* stream =
        null.AcquireSinkStream(system, 2, "render", AudioCore::Sink::StreamType::Render);
    REQUIRE(stream != nullptr);
    CHECK(stream->GetSystemChannels() == 2);
    stream->Start();

    AudioCore::Sink::SinkBuffer block{240, 0, 0, false};
    std::array<s16, 480> samples{};
    constexpr u32 blocks = 12;
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < blocks; ++i) {
        stream->AppendBuffer(block, samples);
    }
    while (stream->GetQueueSize() != 0 &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds{5}) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(stream->GetQueueSize() == 0);
    REQUIRE(std::chrono::steady_clock::now() - start >=
            std::chrono::milliseconds{5 * (blocks - 2)});
    null.CloseStream(stream);
}

TEST_CASE("Null sink tracks streams per caller and propagates volume", "[audio_core][null_sink]") {
    Core::System system;
    AudioCore::Sink::NullSink null{"auto"};
    auto* first = null.AcquireSinkStream(system, 2, "first", AudioCore::Sink::StreamType::Render);
    auto* second = null.AcquireSinkStream(system, 6, "second", AudioCore::Sink::StreamType::Out);
    REQUIRE(first != second);
    CHECK(first->GetSystemChannels() == 2);
    CHECK(second->GetSystemChannels() == 6);
    null.SetDeviceVolume(0.5f);
    CHECK(null.GetDeviceVolume() == 0.5f);
    CHECK(first->GetDeviceVolume() == 0.5f);
    CHECK(second->GetDeviceVolume() == 0.5f);
    auto* third = null.AcquireSinkStream(system, 2, "third", AudioCore::Sink::StreamType::In);
    CHECK(third->GetDeviceVolume() == 0.5f);

    second->Start();
    AudioCore::Sink::SinkBuffer block{240, 0, 0, false};
    std::array<s16, 1440> samples{};
    second->AppendBuffer(block, samples);
    const auto start = std::chrono::steady_clock::now();
    while (second->GetQueueSize() != 0 &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds{2}) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(second->GetQueueSize() == 0);

    null.CloseStream(first);
    null.SetDeviceVolume(0.75f);
    CHECK(second->GetDeviceVolume() == 0.75f);
    null.CloseStreams();
}

TEST_CASE("Null sink handles concurrent stream acquire, volume updates, and close",
          "[audio_core][null_sink]") {
    Core::System system;
    AudioCore::Sink::NullSink null{"auto"};
    std::array<bool, 4> valid_volumes{};
    std::array<std::thread, 4> workers;
    for (std::size_t idx = 0; idx < workers.size(); ++idx) {
        workers[idx] = std::thread([&system, &null, &valid = valid_volumes[idx], idx] {
            const auto type = (idx % 2 == 0) ? AudioCore::Sink::StreamType::Render
                                             : AudioCore::Sink::StreamType::Out;
            valid = true;
            for (int iter = 0; iter < 8; ++iter) {
                auto* stream = null.AcquireSinkStream(system, 2, "worker", type);
                null.SetDeviceVolume(0.25f * static_cast<f32>((iter % 4) + 1));
                const f32 volume = null.GetDeviceVolume();
                valid &= stream != nullptr && volume >= 0.25f && volume <= 1.0f;
                null.CloseStream(stream);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    for (const bool valid : valid_volumes) {
        CHECK(valid);
    }
    null.CloseStreams();
}
