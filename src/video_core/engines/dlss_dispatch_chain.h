// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Tegra::Engines {

class DlssDispatchChain {
public:
    void NoteProgram(u64 native_key) {
        if (finished) {
            *this = {};
        }
        if (native_key == fortunes_weave_prepass_key || native_key == star_fox_prepass_key) {
            active = true;
            finished = false;
            dispatches = 0;
            output_key = native_key == fortunes_weave_prepass_key ? fortunes_weave_output_key
                                                                  : star_fox_output_key;
            return;
        }
        if (!active) {
            return;
        }
        if (++dispatches > max_dispatches) {
            *this = {};
            return;
        }
        finished = native_key == output_key;
    }

    bool active{};

private:
    static constexpr u64 fortunes_weave_prepass_key = 0x4cbf6eff32525aecULL;
    static constexpr u64 fortunes_weave_output_key = 0x14b4222bbe23abeeULL;
    static constexpr u64 star_fox_prepass_key = 0xd686ca0b9fad9e6dULL;
    static constexpr u64 star_fox_output_key = 0x7d3764ff101bec27ULL;
    static constexpr u32 max_dispatches = 64;

    bool finished{};
    u64 output_key{};
    u32 dispatches{};
};

}
