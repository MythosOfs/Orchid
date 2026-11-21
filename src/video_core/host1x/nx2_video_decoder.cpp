// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/host1x/ffmpeg.h"
#include "video_core/host1x/h264_packet.h"
#include "video_core/host1x/nx2_video_decoder.h"

extern "C" {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif

#include <libswscale/swscale.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

namespace Tegra::Host1x {

namespace {

std::optional<NvdecCommon::VideoCodec> CodecFromMime(std::string_view mime) {
    if (mime.find("vp9") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::VP9;
    }
    if (mime.find("vp8") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::VP8;
    }
    if (mime.find("av01") != std::string_view::npos || mime.find("av1") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::AV1;
    }
    if (mime.find("avc") != std::string_view::npos || mime.find("264") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::H264;
    }
    return std::nullopt;
}

}

std::optional<Nx2VideoConfiguration> ParseNx2VideoConfiguration(std::span<const u8> configuration) {
    if (configuration.empty()) {
        return std::nullopt;
    }
    const auto name_end = std::find(configuration.begin(), configuration.end(), u8{});
    const std::string_view mime{reinterpret_cast<const char*>(configuration.data()),
                               static_cast<size_t>(name_end - configuration.begin())};
    if (const auto codec = CodecFromMime(mime)) {
        return Nx2VideoConfiguration{*codec, {}};
    }
    const auto prefix_size = [&](size_t offset) -> size_t {
        const auto remaining = configuration.subspan(offset);
        if (remaining.size() >= 4 && remaining[0] == 0 && remaining[1] == 0 &&
            remaining[2] == 0 && remaining[3] == 1) {
            return 4;
        }
        if (remaining.size() >= 3 && remaining[0] == 0 && remaining[1] == 0 && remaining[2] == 1) {
            return 3;
        }
        return 0;
    };
    bool has_sps{};
    bool has_pps{};
    for (size_t offset = 0; offset < configuration.size();) {
        const size_t prefix = prefix_size(offset);
        if (prefix == 0 || offset + prefix == configuration.size()) {
            return std::nullopt;
        }
        const size_t start = offset + prefix;
        size_t end = start + 1;
        while (end < configuration.size() && prefix_size(end) == 0) {
            ++end;
        }
        const u8 header = configuration[start];
        const u8 type = header & 0x1f;
        if ((header & 0x80) != 0 || (type != 7 && type != 8) ||
            end - start < (type == 7 ? 4U : 2U)) {
            return std::nullopt;
        }
        has_sps |= type == 7;
        has_pps |= type == 8;
        offset = end;
    }
    if (!has_sps || !has_pps) {
        return std::nullopt;
    }
    return Nx2VideoConfiguration{NvdecCommon::VideoCodec::H264, configuration};
}

bool IsH264Stream(std::span<const u8> data) {
    if (data.size() >= 4) {
        if (data[0] == 0 && data[1] == 0 && data[2] == 1) {
            const u8 nal_type = data[3] & 0x1F;
            if ((data[3] & 0x80) == 0 && nal_type >= 1 && nal_type <= 23) {
                return true;
            }
        }
    }
    if (data.size() >= 5) {
        if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
            const u8 nal_type = data[4] & 0x1F;
            if ((data[4] & 0x80) == 0 && nal_type >= 1 && nal_type <= 23) {
                return true;
            }
        }
        const u32 nalu_len = (static_cast<u32>(data[0]) << 24) |
                             (static_cast<u32>(data[1]) << 16) |
                             (static_cast<u32>(data[2]) << 8) |
                             static_cast<u32>(data[3]);
        if (nalu_len > 0 && nalu_len <= data.size() - 4) {
            const u8 nal_type = data[4] & 0x1F;
            if ((data[4] & 0x80) == 0 && nal_type >= 1 && nal_type <= 23) {
                return true;
            }
        }
    }
    if (data.size() >= 7) {
        if (data[0] == 1 && (data[4] & 0xFC) == 0xFC && (data[5] & 0xE0) == 0xE0 && (data[5] & 0x1F) > 0) {
            return true;
        }
    }
    return false;
}

struct Nx2VideoDecoder::Impl {
    std::vector<u8> configuration;
    NvdecCommon::VideoCodec codec{};
    std::optional<FFmpeg::Decoder> decoder;
    std::optional<FFmpeg::HardwareContext> hardware;
    std::unique_ptr<FFmpeg::DecoderContext> context;
    SwsContext* scaler{};
    bool open{};
    bool on_gpu{};
    bool received{};
    Nx2GpuDecodeFault gpu_fault{};
    H264PacketConverter h264_packets;
    std::multiset<u64> pending_pts;
    std::vector<std::pair<std::vector<u8>, u64>> gpu_packets;
    bool gpu_packets_lost{};
    std::vector<u8> packet;

    ~Impl() {
        sws_freeContext(scaler);
        context.reset();
        hardware.reset();
    }

    bool OpenGpu() {
        context = std::make_unique<FFmpeg::DecoderContext>(*decoder);
        hardware.emplace();
        on_gpu = gpu_fault != Nx2GpuDecodeFault::None ||
                 hardware->InitializeForDecoder(*context, *decoder);
        return !(on_gpu && gpu_fault == Nx2GpuDecodeFault::Open) &&
               context->OpenContext(*decoder, ParseNx2VideoConfiguration(configuration)->extradata);
    }

    std::unique_ptr<FFmpeg::DecoderContext> OpenCpu() const {
        auto cpu = std::make_unique<FFmpeg::DecoderContext>(*decoder);
        if (gpu_fault == Nx2GpuDecodeFault::NoCpu ||
            !cpu->OpenContext(*decoder, ParseNx2VideoConfiguration(configuration)->extradata)) {
            return {};
        }
        return cpu;
    }

    void UseCpu(std::unique_ptr<FFmpeg::DecoderContext> cpu) {
        context = std::move(cpu);
        hardware.reset();
        on_gpu = false;
    }

    static bool Rejected(int ret) {
        return ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN);
    }

    int Send(FFmpeg::DecoderContext& target, const FFmpeg::Packet& packet, bool gpu) const {
        const int ret = gpu && gpu_fault == Nx2GpuDecodeFault::Busy   ? AVERROR(EAGAIN)
                        : gpu && gpu_fault == Nx2GpuDecodeFault::Hold ? 0
                        : gpu && (gpu_fault == Nx2GpuDecodeFault::Packet ||
                                  gpu_fault == Nx2GpuDecodeFault::NoCpu ||
                                  gpu_fault == Nx2GpuDecodeFault::Replay ||
                                  gpu_fault == Nx2GpuDecodeFault::ReplayReceive ||
                                  gpu_fault == Nx2GpuDecodeFault::SwitchReceive)
                            ? AVERROR_INVALIDDATA
                            : avcodec_send_packet(target.GetCodecContext(), packet.GetPacket());
        if (Rejected(ret)) {
            LOG_ERROR(HW_GPU, "nx2 video: avcodec_send_packet error {}", ret);
        }
        return ret;
    }

    bool Replay(const FFmpeg::Packet& packet, u64 pts, std::vector<Nx2Picture>& pictures) {
        for (const auto& [data, data_pts] : gpu_packets) {
            FFmpeg::Packet replay{
                std::span<const u8>{data.data(), data.size() - AV_INPUT_BUFFER_PADDING_SIZE}};
            replay.GetPacket()->pts = static_cast<s64>(data_pts);
            pending_pts.insert(data_pts);
            const bool last = &data == &gpu_packets.back().first;
            if ((gpu_fault == Nx2GpuDecodeFault::Replay && last) ||
                Rejected(Send(*context, replay, false))) {
                return false;
            }
            Receive(pictures);
            if (context->ReceiveFailed() ||
                (gpu_fault == Nx2GpuDecodeFault::ReplayReceive && last)) {
                return false;
            }
        }
        pending_pts.insert(pts);
        if (Rejected(Send(*context, packet, false))) {
            return false;
        }
        Receive(pictures);
        return !context->ReceiveFailed() && gpu_fault != Nx2GpuDecodeFault::SwitchReceive;
    }

    void FallBackToCpu(const FFmpeg::Packet& packet, u64 pts, std::vector<Nx2Picture>& pictures) {
        auto cpu = gpu_packets_lost ? nullptr : OpenCpu();
        if (!cpu) {
            return;
        }
        auto gpu = std::exchange(context, std::move(cpu));
        auto gpu_pts = std::exchange(pending_pts, {});
        const size_t count = pictures.size();
        if (Replay(packet, pts, pictures)) {
            LOG_WARNING(HW_GPU, "nx2 video: host GPU decoder rejected the stream, decoding on the CPU");
            gpu.reset();
            UseCpu(std::move(context));
            return;
        }
        context = std::move(gpu);
        pending_pts = std::move(gpu_pts);
        pictures.resize(count);
        received = false;
    }

    void Receive(std::vector<Nx2Picture>& pictures) {
        while (auto frame = context->ReceiveFrame()) {
            const AVFrame* f = frame->GetFrame();
            const s64 stamp = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp
                                                                         : f->pts;
            u64 pts{};
            if (stamp != AV_NOPTS_VALUE) {
                pts = static_cast<u64>(stamp);
                if (const auto it = pending_pts.find(pts); it != pending_pts.end()) {
                    pending_pts.erase(it);
                }
            } else if (!pending_pts.empty()) {
                pts = *pending_pts.begin();
                pending_pts.erase(pending_pts.begin());
            }
            pictures.push_back({static_cast<u32>(f->width), static_cast<u32>(f->height), pts,
                                std::move(frame)});
            received = true;
        }
    }
};

Nx2VideoDecoder::Nx2VideoDecoder(std::span<const u8> configuration, Nx2GpuDecodeFault fault)
    : impl{std::make_unique<Impl>()} {
    impl->gpu_fault = fault;
    impl->configuration.assign(configuration.begin(), configuration.end());
    const auto parsed = ParseNx2VideoConfiguration(impl->configuration);
    if (!parsed) {
        LOG_ERROR(HW_GPU, "nx2 video: unsupported {}-byte codec configuration", configuration.size());
        return;
    }
    impl->codec = parsed->codec;
    impl->decoder.emplace(parsed->codec);
    if (impl->decoder->GetCodec() == nullptr) {
        LOG_ERROR(HW_GPU, "nx2 video: FFmpeg has no decoder for codec {}",
                  static_cast<u64>(parsed->codec));
        return;
    }
    if (Settings::values.nvdec_emulation.GetValue() == Settings::NvdecEmulation::Gpu) {
        impl->open = impl->OpenGpu();
    }
    if (!impl->open) {
        if (impl->on_gpu) {
            LOG_WARNING(HW_GPU, "nx2 video: host GPU decoder failed to open, decoding on the CPU");
        }
        impl->UseCpu(impl->OpenCpu());
        impl->open = impl->context != nullptr;
    }
    LOG_INFO(HW_GPU, "nx2 video: decoder for codec {} ({} configuration bytes) {}",
             static_cast<u64>(parsed->codec), parsed->extradata.size(),
             impl->open ? "open" : "failed to open");
}

Nx2VideoDecoder::~Nx2VideoDecoder() = default;

void Nx2VideoDecoder::SetGpuDecodeFault(Nx2GpuDecodeFault fault) {
    impl->gpu_fault = fault;
}

bool Nx2VideoDecoder::IsOpen() const {
    return impl->open;
}

std::span<const u8> Nx2VideoDecoder::Configuration() const {
    return impl->configuration;
}

void Nx2VideoDecoder::Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures) {
    if (!impl->open || data.empty()) {
        return;
    }
    impl->packet.assign(data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    std::memcpy(impl->packet.data(), data.data(), data.size());

    if (impl->codec == NvdecCommon::VideoCodec::H264) {
        impl->h264_packets.Convert(std::span<u8>{impl->packet.data(), data.size()});
    }

    FFmpeg::Packet packet{std::span<const u8>{impl->packet.data(), data.size()}};
    packet.GetPacket()->pts = static_cast<s64>(pts);
    if (impl->pending_pts.size() >= 64) {
        impl->pending_pts.erase(impl->pending_pts.begin());
    }
    impl->pending_pts.insert(pts);
    const bool waiting = impl->on_gpu && !impl->received;
    const int ret = impl->Send(*impl->context, packet, impl->on_gpu);
    if (Impl::Rejected(ret)) {
        if (waiting) {
            impl->FallBackToCpu(packet, pts, pictures);
        }
    } else if (ret == 0 && waiting && impl->gpu_packets.size() == 64) {
        impl->gpu_packets.clear();
        impl->gpu_packets_lost = true;
    } else if (ret == 0 && waiting && !impl->gpu_packets_lost) {
        impl->gpu_packets.emplace_back(impl->packet, pts);
    }
    impl->Receive(pictures);
    if (impl->received || !impl->on_gpu) {
        impl->gpu_packets.clear();
    }
}

void Nx2VideoDecoder::Drain(std::vector<Nx2Picture>& pictures) {
    if (!impl->open) {
        return;
    }
    avcodec_send_packet(impl->context->GetCodecContext(), nullptr);
    impl->Receive(pictures);
    Reset();
}

void Nx2VideoDecoder::Reset() {
    if (impl->open) {
        avcodec_flush_buffers(impl->context->GetCodecContext());
    }
    impl->pending_pts.clear();
    impl->gpu_packets.clear();
    impl->gpu_packets_lost = false;
    impl->received = false;
    impl->h264_packets.Reset();
}

void Nx2VideoDecoder::Render(const Nx2Picture& picture, Nx2PictureFormat format, u32 width,
                             u32 height, u64 pitch, u64 chroma_offset, std::span<u8> out) {
    const AVFrame* f = picture.frame->GetFrame();
    const u64 needed = format == Nx2PictureFormat::Nv12
                           ? chroma_offset + pitch * ((u64{height} + 1) / 2)
                           : pitch * height;
    const u64 min_pitch = format == Nx2PictureFormat::Nv12 ? u64{(width + 1) / 2} * 2 : 0;
    if (width == 0 || height == 0 || pitch < min_pitch || out.size() < needed) {
        return;
    }
    const AVPixelFormat target = format == Nx2PictureFormat::Nv12    ? AV_PIX_FMT_NV12
                                 : format == Nx2PictureFormat::Rgba8 ? AV_PIX_FMT_RGBA
                                                                     : AV_PIX_FMT_BGRA;
    if (target == AV_PIX_FMT_NV12 && f->format == AV_PIX_FMT_NV12 &&
        static_cast<u32>(f->width) == width && static_cast<u32>(f->height) == height) {
        for (u32 y = 0; y < height; ++y) {
            std::memcpy(out.data() + y * pitch, f->data[0] + y * f->linesize[0], width);
        }
        for (u32 y = 0; y < (height + 1) / 2; ++y) {
            std::memcpy(out.data() + chroma_offset + y * pitch, f->data[1] + y * f->linesize[1],
                        min_pitch);
        }
        return;
    }
    if (target == AV_PIX_FMT_NV12 && f->format == AV_PIX_FMT_YUV420P &&
        f->color_range != AVCOL_RANGE_JPEG && static_cast<u32>(f->width) == width &&
        static_cast<u32>(f->height) == height) {
        for (u32 y = 0; y < height; ++y) {
            std::memcpy(out.data() + y * pitch, f->data[0] + y * f->linesize[0], width);
        }
        const u32 chroma_width = (width + 1) / 2;
        for (u32 y = 0; y < (height + 1) / 2; ++y) {
            const u8* u = f->data[1] + y * f->linesize[1];
            const u8* v = f->data[2] + y * f->linesize[2];
            u8* dst = out.data() + chroma_offset + y * pitch;
            for (u32 x = 0; x < chroma_width; ++x) {
                dst[x * 2] = u[x];
                dst[x * 2 + 1] = v[x];
            }
        }
        return;
    }
    impl->scaler = sws_getCachedContext(impl->scaler, f->width, f->height,
                                        static_cast<AVPixelFormat>(f->format),
                                        static_cast<int>(width), static_cast<int>(height), target,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (impl->scaler == nullptr) {
        LOG_ERROR(HW_GPU, "nx2 video: cannot convert {} to {}",
                  av_get_pix_fmt_name(static_cast<AVPixelFormat>(f->format)),
                  av_get_pix_fmt_name(target));
        return;
    }
    const bool full_range_source =
        f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P ||
        f->format == AV_PIX_FMT_YUVJ422P || f->format == AV_PIX_FMT_YUVJ444P ||
        f->format == AV_PIX_FMT_YUVJ440P || f->format == AV_PIX_FMT_YUVJ411P;
    int space = SWS_CS_ITU601;
    if (f->colorspace == AVCOL_SPC_BT709 ||
        (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720)) {
        space = SWS_CS_ITU709;
    } else if (f->colorspace == AVCOL_SPC_BT2020_NCL || f->colorspace == AVCOL_SPC_BT2020_CL) {
        space = SWS_CS_BT2020;
    }
    const int* coefficients = sws_getCoefficients(space);
    sws_setColorspaceDetails(impl->scaler, coefficients, full_range_source, coefficients,
                             format != Nx2PictureFormat::Nv12, 0, 1 << 16, 1 << 16);
    std::array<u8*, 4> planes{out.data(), nullptr, nullptr, nullptr};
    std::array<int, 4> strides{static_cast<int>(pitch), 0, 0, 0};
    if (format == Nx2PictureFormat::Nv12) {
        planes[1] = out.data() + chroma_offset;
        strides[1] = static_cast<int>(pitch);
    }
    sws_scale(impl->scaler, f->data, f->linesize, 0, f->height, planes.data(), strides.data());
}

}
