// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "video_core/host1x/nvdec_common.h"

namespace FFmpeg {
class Frame;
}

namespace Tegra::Host1x {

struct Nx2VideoConfiguration {
    NvdecCommon::VideoCodec codec;
    std::span<const u8> extradata;
};

std::optional<Nx2VideoConfiguration> ParseNx2VideoConfiguration(std::span<const u8> configuration);

struct Nx2Picture {
    u32 width{};
    u32 height{};
    u64 pts{};
    std::shared_ptr<FFmpeg::Frame> frame;
};

enum class Nx2PictureFormat : u32 {
    Nv12,
    Rgba8,
    Bgra8,
};

enum class Nx2GpuDecodeFault {
    None,
    Open,
    Packet,
    Busy,
    NoCpu,
    Replay,
    Hold,
    ReplayReceive,
    SwitchReceive,
};

bool IsH264Stream(std::span<const u8> data);

class Nx2VideoDecoder {
public:
    explicit Nx2VideoDecoder(std::span<const u8> configuration,
                             Nx2GpuDecodeFault fault = Nx2GpuDecodeFault::None);
    ~Nx2VideoDecoder();

    void SetGpuDecodeFault(Nx2GpuDecodeFault fault);

    bool IsOpen() const;

    std::span<const u8> Configuration() const;

    void Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures);

    void Drain(std::vector<Nx2Picture>& pictures);

    void Reset();

    void Render(const Nx2Picture& picture, Nx2PictureFormat format, u32 width, u32 height,
                u64 pitch, u64 chroma_offset, std::span<u8> out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
