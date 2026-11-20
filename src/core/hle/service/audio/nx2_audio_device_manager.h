// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include <fmt/ranges.h>

#include "audio_core/audio_core.h"
#include "audio_core/common/common.h"
#include "audio_core/sink/sink.h"
#include "audio_core/sink/sink_stream.h"
#include "common/string_util.h"
#include "core/hle/service/audio/nx2_sysmix_pcm.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_event.h"
#include "core/memory.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

namespace Service::Audio {

struct Nx2SysmixStream {
    u32 id{};
    u32 owner{};
    u64 buffer{};
    u64 size{};
    u32 sample_type{};
    u32 channels{};
    u32 sample_rate{};
    u32 frames{};
    bool open{};
    bool engaged{};
    double owed_frames{};
    u32 data_mode{};
    s32 data_threshold{};
    f32 gain{1.0f};
    AudioCore::Sink::SinkStream* host{};
    u64 host_tag{};
    Kernel::KEvent* state_event{};
    Kernel::KEvent* data_event{};
};

struct Nx2SysmixStreams {
    std::mutex mutex;
    std::map<u32, std::shared_ptr<Nx2SysmixStream>> streams;
    u32 next_id{1};
};

inline std::shared_ptr<Nx2SysmixStreams> Nx2SysmixStreamList() {
    static const auto list = std::make_shared<Nx2SysmixStreams>();
    return list;
}

inline void Nx2SysmixCloseHost(Core::System& system, Nx2SysmixStream& stream) {
    if (stream.host) {
        system.AudioCore().GetOutputSink().CloseStream(stream.host);
        stream.host = nullptr;
    }
}

class INx2AudioVirtualDeviceManager final : public ServiceFramework<INx2AudioVirtualDeviceManager> {
public:
    explicit INx2AudioVirtualDeviceManager(Core::System& system_)
        : ServiceFramework{system_, "nx2:auddmg:u:manager"},
          service_context{system_, "nx2:auddmg:u:manager"},
          state_event{service_context.CreateEvent("nx2:auddmg:u:StateEvent")} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &INx2AudioVirtualDeviceManager::ListVirtualOutputDevices, "ListVirtualOutputDevices"},
            {100, &INx2AudioVirtualDeviceManager::GetDefaultVirtualOutputDevice, "GetDefaultVirtualOutputDevice"},
            {300, &INx2AudioVirtualDeviceManager::IsStreamFormatSupported, "IsVirtualOutputDeviceStreamFormatSupported"},
            {401, &INx2AudioVirtualDeviceManager::GetVirtualOutputDeviceProperties, "GetVirtualOutputDeviceProperties"},
            {500, &INx2AudioVirtualDeviceManager::AcquireStateChangeEvent, "AcquireVirtualOutputDeviceStateEvent (guessed)"},
            {700, &INx2AudioVirtualDeviceManager::AttachStream, "AttachStream"},
            {800, &INx2AudioVirtualDeviceManager::DetachStream, "DetachStream"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

    ~INx2AudioVirtualDeviceManager() override {
        service_context.CloseEvent(state_event);
    }

private:
    static constexpr std::size_t NameSize = 0x100;
    static constexpr std::string_view DeviceName = "AudioVirtualOutput0";
    static constexpr std::string_view PhysicalDeviceName = "AudioTvOutput";

    static void CopyName(u8* out, std::string_view name) {
        std::memcpy(out, name.data(), std::min(name.size(), NameSize - 1));
    }

    void ListVirtualOutputDevices(HLERequestContext& ctx) {
        std::array<u8, NameSize> names{};
        CopyName(names.data(), DeviceName);
        const u32 count = ctx.GetWriteBufferSize() >= NameSize ? 1 : 0;
        if (count) {
            ctx.WriteBuffer(names);
        }
        LOG_DEBUG(Service_Audio, "called, {} device", count);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(count);
    }

    void GetDefaultVirtualOutputDevice(HLERequestContext& ctx) {
        std::array<u8, NameSize> name{};
        CopyName(name.data(), DeviceName);
        ctx.WriteBuffer(name);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void IsStreamFormatSupported(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Audio, "called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetVirtualOutputDeviceProperties(HLERequestContext& ctx) {
        std::array<u8, 0x228> block{};
        const auto name = ctx.ReadBuffer(0);
        std::memcpy(block.data(), name.data(), std::min(name.size(), NameSize - 1));
        const auto put32 = [&](std::size_t offset, u32 value) {
            std::memcpy(block.data() + offset, &value, sizeof(value));
        };
        put32(0x100, 3);
        put32(0x104, 2);
        put32(0x108, 2);
        put32(0x10c, 48000);
        CopyName(block.data() + 0x110, PhysicalDeviceName);
        const u64 latency_ns = 10'000'000;
        std::memcpy(block.data() + 0x218, &latency_ns, sizeof(latency_ns));
        put32(0x220, 1);
        ctx.WriteBuffer(block);
        LOG_DEBUG(Service_Audio, "called for {}", Common::StringFromBuffer(name));
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void AcquireStateChangeEvent(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, state_event->GetReadableEvent());
    }

    void AttachStream(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto stream_id = rp.Pop<u32>();
        const auto functionality = rp.Pop<u32>();
        LOG_INFO(Service_Audio, "attach stream {} to {} (functionality {})", stream_id,
                 Common::StringFromBuffer(ctx.ReadBuffer(0)), functionality);
        SetStreamEngaged(stream_id, true);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void DetachStream(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto stream_id = rp.Pop<u32>();
        LOG_INFO(Service_Audio, "detach stream {}", stream_id);
        SetStreamEngaged(stream_id, false);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetStreamEngaged(u32 stream_id, bool engaged) {
        const auto list = Nx2SysmixStreamList();
        std::scoped_lock lk{list->mutex};
        if (const auto it = list->streams.find(stream_id); it != list->streams.end()) {
            it->second->engaged = engaged;
            it->second->owed_frames = 0;
            if (it->second->state_event) {
                it->second->state_event->Signal(system.Kernel());
            }
        }
    }

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* state_event;
};

class INx2AudioDeviceManagerService final : public ServiceFramework<INx2AudioDeviceManagerService> {
public:
    explicit INx2AudioDeviceManagerService(Core::System& system_)
        : ServiceFramework{system_, "nx2:auddmg:u"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &INx2AudioDeviceManagerService::OpenVirtualDeviceManager, "OpenVirtualDeviceManager (guessed)"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    void OpenVirtualDeviceManager(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Audio, "called");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<INx2AudioVirtualDeviceManager>(ctx, system);
    }
};

class INx2SysmixStreamSession final : public ServiceFramework<INx2SysmixStreamSession> {
public:
    explicit INx2SysmixStreamSession(Core::System& system_, std::shared_ptr<Nx2SysmixStreams> streams_,
                                     std::shared_ptr<Nx2SysmixStream> stream_)
        : ServiceFramework{system_, "nx2:audsmx:u:stream"}, service_context{system_, "nx2:audsmx:u:stream"},
          streams{std::move(streams_)}, stream{std::move(stream_)} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &INx2SysmixStreamSession::OpenStream, "OpenStream"},
            {1, &INx2SysmixStreamSession::CloseStream, "CloseStream"},
            {2, &INx2SysmixStreamSession::GetStreamState, "GetStreamState"},
            {3, &INx2SysmixStreamSession::GetStreamGain, "GetStreamGain"},
            {4, &INx2SysmixStreamSession::SetStreamGain, "SetStreamGain"},
            {5, &INx2SysmixStreamSession::AcquireStateChangeEvent, "AcquireStreamStateChangeEvent"},
            {6, &INx2SysmixStreamSession::Succeed, "ReleaseStreamStateChangeEvent"},
            {7, &INx2SysmixStreamSession::AcquireDataEvent, "AcquireStreamDataEvent"},
            {8, &INx2SysmixStreamSession::Succeed, "ReleaseStreamDataEvent"},
            {9, &INx2SysmixStreamSession::Succeed, "ClockStream"},
            {10, &INx2SysmixStreamSession::EchoFrames, "PrimeStream"},
            {11, &INx2SysmixStreamSession::EchoFrames, "PurgeStream"},
            {12, &INx2SysmixStreamSession::Succeed, "RenameStream"},
        };
        // clang-format on
        RegisterHandlers(functions);
        std::scoped_lock lk{streams->mutex};
        stream->state_event = service_context.CreateEvent("nx2:audsmx:u:StreamStateEvent");
        stream->data_event = service_context.CreateEvent("nx2:audsmx:u:StreamDataEvent");
    }

    ~INx2SysmixStreamSession() override {
        {
            std::scoped_lock lk{streams->mutex};
            streams->streams.erase(stream->id);
            Nx2SysmixCloseHost(system, *stream);
            service_context.CloseEvent(stream->state_event);
            service_context.CloseEvent(stream->data_event);
            stream->state_event = nullptr;
            stream->data_event = nullptr;
        }
    }

private:
    void OpenStream(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto sample_type = rp.Pop<u32>();
        const auto channels = rp.Pop<u32>();
        const auto sample_rate = rp.Pop<u32>();
        const auto frames = rp.Pop<u32>();
        const auto prefill = rp.Pop<u32>();
        const auto flags = rp.Pop<u32>();
        LOG_INFO(Service_Audio,
                 "stream {} open: sample type {}, {} channels, {} Hz, {} frames, prefill {}, flags {:#x}",
                 stream->id, sample_type, channels, sample_rate, frames, prefill, flags);
        {
            std::scoped_lock lk{streams->mutex};
            stream->sample_type = sample_type;
            stream->channels = channels;
            stream->sample_rate = sample_rate;
            stream->frames = frames;
            stream->owed_frames = 0;
            stream->open = true;
            const bool playable = Sysmix::IsPlayable(sample_type, channels, sample_rate);
            Nx2SysmixCloseHost(system, *stream);
            if (playable) {
                stream->host = system.AudioCore().GetOutputSink().AcquireSinkStream(
                    system, channels, fmt::format("nx2-sysmix-{}", stream->id),
                    AudioCore::Sink::StreamType::Out);
                if (stream->host) {
                    stream->host->Start();
                }
            }
            stream->state_event->Signal(system.Kernel());
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void CloseStream(HLERequestContext& ctx) {
        {
            std::scoped_lock lk{streams->mutex};
            stream->open = false;
            Nx2SysmixCloseHost(system, *stream);
            stream->state_event->Signal(system.Kernel());
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetStreamState(HLERequestContext& ctx) {
        u32 state{};
        {
            std::scoped_lock lk{streams->mutex};
            state = stream->open ? (stream->engaged ? 2 : 1) : 0;
        }
        LOG_DEBUG(Service_Audio, "stream {} state {}", stream->id, state);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(state);
    }

    void GetStreamGain(HLERequestContext& ctx) {
        f32 gain{};
        {
            std::scoped_lock lk{streams->mutex};
            gain = stream->gain;
        }
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(gain);
    }

    void SetStreamGain(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto gain = rp.Pop<f32>();
        {
            std::scoped_lock lk{streams->mutex};
            stream->gain = std::isfinite(gain) ? std::clamp(gain, 0.0f, 128.0f) : 1.0f;
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void AcquireStateChangeEvent(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, stream->state_event->GetReadableEvent());
    }

    void AcquireDataEvent(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto mode = rp.Pop<u32>();
        const auto threshold = rp.Pop<s32>();
        LOG_INFO(Service_Audio, "stream {} data event: mode {}, threshold {}", stream->id, mode, threshold);
        {
            std::scoped_lock lk{streams->mutex};
            stream->data_mode = mode;
            stream->data_threshold = threshold;
            if (mode != 0 && stream->open && static_cast<s64>(stream->frames) >= threshold) {
                stream->data_event->Signal(system.Kernel());
            }
        }
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, stream->data_event->GetReadableEvent());
    }

    void EchoFrames(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto frames = rp.Pop<s32>();
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(static_cast<u32>((std::max)(frames, 0)));
    }

    void Succeed(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    KernelHelpers::ServiceContext service_context;
    std::shared_ptr<Nx2SysmixStreams> streams;
    std::shared_ptr<Nx2SysmixStream> stream;
};

class INx2SysmixUserSession final : public ServiceFramework<INx2SysmixUserSession> {
public:
    explicit INx2SysmixUserSession(Core::System& system_) : ServiceFramework{system_, "nx2:audsmx:u:session"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {1, &INx2SysmixUserSession::CreateStream, "CreateStream"},
            {2, &INx2SysmixUserSession::EngageStreams, "EngageStreams"},
            {3, &INx2SysmixUserSession::DisengageStreams, "DisengageStreams"},
        };
        // clang-format on
        RegisterHandlers(functions);
        drain_timer = Core::Timing::CreateEvent(
            "nx2:audsmx:u drain",
            [streams = streams, &system = system, owner = session_id](s64, std::chrono::nanoseconds)
                -> std::optional<std::chrono::nanoseconds> {
                Drain(system, *streams, owner);
                return std::nullopt;
            });
        system.CoreTiming().ScheduleLoopingEvent(DrainInterval, DrainInterval, drain_timer);
    }

    ~INx2SysmixUserSession() override {
        system.CoreTiming().UnscheduleEvent(drain_timer);
    }

private:
    static constexpr std::chrono::nanoseconds DrainInterval{std::chrono::milliseconds{5}};
    static constexpr u64 ReadIndexOffset = 0x44;
    static constexpr u64 WriteIndexOffset = 0x1004;
    static constexpr u64 PayloadOffset = 0x1040;
    static constexpr u32 MaxHostBlocks = 6;

    static void PlayFrames(Core::System& system, Nx2SysmixStream& stream, u32 read, u32 count,
                           u32 capacity) {
        const bool is_float = stream.sample_type == static_cast<u32>(AudioCore::SampleFormat::PcmFloat);
        const u32 sample_bytes = is_float ? sizeof(f32) : sizeof(s16);
        const u32 frame_bytes = stream.channels * sample_bytes;
        if (PayloadOffset + u64{capacity} * frame_bytes > stream.size) {
            return;
        }
        std::vector<u8> raw(std::size_t{count} * frame_bytes);
        auto& memory = system.ApplicationMemory();
        if (!Sysmix::ReadRing(raw, read, count, capacity, frame_bytes,
                             [&](u64 offset, std::span<u8> out) {
                                 return memory.ReadBlock(stream.buffer + PayloadOffset + offset,
                                                         out.data(), out.size());
                             })) {
            return;
        }
        auto samples = Sysmix::ConvertPcm(raw, is_float, stream.gain);
        AudioCore::Sink::SinkBuffer block{count, 0, stream.host_tag++, false};
        stream.host->AppendBuffer(block, samples);
    }

    static void Drain(Core::System& system, Nx2SysmixStreams& state, u32 owner) {
        std::scoped_lock lk{state.mutex};
        auto& memory = system.ApplicationMemory();
        for (auto& [id, stream] : state.streams) {
            if (stream->owner != owner || !stream->open || stream->frames == 0 || !stream->buffer) {
                continue;
            }
            const u32 capacity = stream->frames + 1;
            const u32 write = memory.Read32(stream->buffer + WriteIndexOffset);
            const u32 read = memory.Read32(stream->buffer + ReadIndexOffset);
            if (write >= capacity || read >= capacity) {
                continue;
            }
            u32 filled = (write + capacity - read) % capacity;
            if (stream->engaged) {
                stream->owed_frames += stream->sample_rate *
                                       std::chrono::duration<double>(DrainInterval).count();
                const auto owed = static_cast<u32>(stream->owed_frames);
                stream->owed_frames -= owed;
                u32 taken = (std::min)(filled, owed);
                if (stream->host && stream->host->GetQueueSize() >= MaxHostBlocks) {
                    taken = 0;
                }
                if (taken != 0) {
                    if (stream->host) {
                        PlayFrames(system, *stream, read, taken, capacity);
                    }
                    memory.Write32(stream->buffer + ReadIndexOffset, (read + taken) % capacity);
                    filled -= taken;
                }
            }
            const u32 free_frames = capacity - 1 - filled;
            if (stream->data_event && stream->data_mode != 0 &&
                static_cast<s64>(free_frames) >= stream->data_threshold) {
                stream->data_event->Signal(system.Kernel());
            }
        }
    }

    void CreateStream(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto kind = rp.Pop<u32>();
        const auto flags = rp.Pop<u32>();
        [[maybe_unused]] const auto pid = rp.Pop<u64>();
        const auto buffer = rp.Pop<u64>();
        const auto size = rp.Pop<u64>();
        const auto name = Common::StringFromBuffer(ctx.ReadBuffer(0));
        auto stream = std::make_shared<Nx2SysmixStream>();
        stream->buffer = buffer;
        stream->size = size;
        {
            std::scoped_lock lk{streams->mutex};
            stream->id = streams->next_id++;
            stream->owner = session_id;
            streams->streams.emplace(stream->id, stream);
        }
        LOG_INFO(Service_Audio, "stream {} '{}': kind {}, flags {:#x}, buffer {:#x} size {:#x}",
                 stream->id, name, kind, flags, buffer, size);
        IPC::ResponseBuilder rb{ctx, 3, 0, 1};
        rb.Push(ResultSuccess);
        rb.Push(stream->id);
        rb.PushIpcInterface<INx2SysmixStreamSession>(ctx, system, streams, stream);
    }

    void SetEngaged(HLERequestContext& ctx, bool engaged) {
        const auto buffer = ctx.ReadBuffer(0);
        std::vector<u32> ids(buffer.size() / sizeof(u32));
        std::memcpy(ids.data(), buffer.data(), ids.size() * sizeof(u32));
        {
            std::scoped_lock lk{streams->mutex};
            for (const u32 id : ids) {
                if (const auto it = streams->streams.find(id); it != streams->streams.end()) {
                    it->second->engaged = engaged;
                    it->second->owed_frames = 0;
                    if (it->second->state_event) {
                        it->second->state_event->Signal(system.Kernel());
                    }
                }
            }
        }
        LOG_INFO(Service_Audio, "{} streams {}", engaged ? "engage" : "disengage", fmt::join(ids, ","));
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void EngageStreams(HLERequestContext& ctx) {
        SetEngaged(ctx, true);
    }

    void DisengageStreams(HLERequestContext& ctx) {
        SetEngaged(ctx, false);
    }

    static inline std::atomic<u32> next_session_id{1};
    const u32 session_id = next_session_id++;
    std::shared_ptr<Nx2SysmixStreams> streams = Nx2SysmixStreamList();
    std::shared_ptr<Core::Timing::EventType> drain_timer;
};

class INx2AudioSoundMixerService final : public ServiceFramework<INx2AudioSoundMixerService> {
public:
    explicit INx2AudioSoundMixerService(Core::System& system_)
        : ServiceFramework{system_, "nx2:audsmx:u"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &INx2AudioSoundMixerService::OpenUserSession, "OpenUserSession"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    void OpenUserSession(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Audio, "called");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<INx2SysmixUserSession>(ctx, system);
    }
};

}
