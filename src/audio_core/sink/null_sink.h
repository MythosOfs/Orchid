// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "audio_core/sink/sink.h"
#include "audio_core/sink/sink_stream.h"

namespace Core {
class System;
} // namespace Core

namespace AudioCore::Sink {
class NullSinkStreamImpl final : public SinkStream {
public:
    explicit NullSinkStreamImpl(Core::System& system_, StreamType type_)
        : SinkStream{system_, type_} {
        if (type == StreamType::Render || type == StreamType::Out) {
            StartSilentDevice();
        }
    }
    ~NullSinkStreamImpl() override {
        Finalize();
    }
    void Finalize() override {
        SignalPause();
        StopSilentDevice();
    }
    void Start(bool = false) override {
        paused = false;
    }
    void Stop() override {
        SignalPause();
    }
    std::vector<s16> ReleaseBuffer(u64) override {
        return {};
    }
};

/**
 * A no-op sink for when no audio out is wanted.
 */
class NullSink final : public Sink {
public:
    explicit NullSink(std::string_view) {}
    ~NullSink() override = default;

    SinkStream* AcquireSinkStream(Core::System& system, u32 system_channels_, const std::string&,
                                  StreamType type) override {
        SinkStreamPtr& stream =
            sink_streams.emplace_back(std::make_unique<NullSinkStreamImpl>(system, type));
        stream->SetSystemChannels(system_channels_);
        stream->SetDeviceVolume(device_volume);
        return stream.get();
    }

    void CloseStream(SinkStream* stream) override {
        std::erase_if(sink_streams, [stream](const SinkStreamPtr& s) { return s.get() == stream; });
    }
    void CloseStreams() override {
        sink_streams.clear();
    }
    f32 GetDeviceVolume() const override {
        return device_volume;
    }
    void SetDeviceVolume(f32 volume) override {
        device_volume = volume;
        for (auto& stream : sink_streams) {
            stream->SetDeviceVolume(volume);
        }
    }
    void SetSystemVolume(f32 volume) override {}

private:
    std::vector<SinkStreamPtr> sink_streams{};
};

} // namespace AudioCore::Sink
