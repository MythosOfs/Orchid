// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/alignment.h"
#include "common/common_types.h"
#include "video_core/framebuffer_config.h"

namespace Service {

// Pixel format IDs in InterModuleUserImagePreference word 6.
enum class Nx2NativeImageFormat : u32 {
    Rgba8 = 1,
    Rgb10A2 = 4,
    Rgba16Float = 5,
};

// The colour space of a presented native image. FP16 is linear scRGB by its format. RGB10A2 holds
// PQ BT.2020 only while the game was told the display is HDR: Fire Emblem: Fortune's Weave
// presents SDR in RGB10A2.
inline Tegra::ColorSpace Nx2NativeImageColorSpace(Nx2NativeImageFormat format, bool told_hdr) {
    switch (format) {
    case Nx2NativeImageFormat::Rgba16Float:
        return Tegra::ColorSpace::RGB709_Linear;
    case Nx2NativeImageFormat::Rgb10A2:
        return told_hdr ? Tegra::ColorSpace::RGB2020_PQ : Tegra::ColorSpace::RGB709;
    default:
        return Tegra::ColorSpace::RGB709;
    }
}

// Block-linear layout of an InterModuleUserImagePreference (16 words): word 2 holds
// (width - 1) | (height - 1) << 16, word 4 the block height (log2 of GOB rows), word 5 the row
// pitch in 64-byte GOBs. Formula from changes.patch (native_image_layout.h); preferences outside
// it fall back to the format's bytes per pixel (word 6: 8 for FP16, else 4) and 128-row blocks.
struct Nx2ImageLayout {
    u32 width;
    u32 height;
    u64 pitch;
    u32 block_height_log2;
    u64 size;
    u64 alignment;
};

inline Nx2ImageLayout Nx2CalculateImageLayout(const u32* preference) {
    Nx2ImageLayout layout{};
    layout.width = (preference[2] & 0xFFFF) + 1;
    layout.height = (preference[2] >> 16) + 1;
    const u64 pitch_gobs = preference[5];
    const u32 block_height_log2 = preference[4];
    const u32 bytes_per_pixel =
        static_cast<Nx2NativeImageFormat>(preference[6]) == Nx2NativeImageFormat::Rgba16Float ? 8 : 4;
    if (block_height_log2 <= 8 && pitch_gobs >= (u64{layout.width} * bytes_per_pixel + 63) / 64 &&
        pitch_gobs <= 4096) {
        layout.pitch = pitch_gobs * 64;
        layout.block_height_log2 = block_height_log2;
    } else {
        layout.pitch = Common::AlignUp(u64{layout.width} * bytes_per_pixel, 64);
        layout.block_height_log2 = 7;
    }
    const u64 block_rows = u64{1} << layout.block_height_log2;
    layout.size = layout.pitch * Common::AlignUp(u64{layout.height}, block_rows);
    // 4 KiB, not one block (8 KiB at 128 rows): Star Fox places its window textures at offset
    // 0x1000 in their memory, and NativeWindow::Initialize fails on a misaligned offset, leaving
    // the NVN window without a native window (null call in NativeWindow::Acquire).
    layout.alignment = 0x1000;
    return layout;
}

} // namespace Service
