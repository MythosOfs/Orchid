// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdlib>
#include <deque>
#include <optional>
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/engines/dlss_dispatch_chain.h"

namespace Tegra::Engines {

class DlssDispatchTrace {
public:
    struct SsboRecord {
        u32 ssbo_index{};
        u32 cbuf_index{};
        u32 cbuf_offset{};
        u64 declared_word{};
        u32 chosen_size{};
        bool native_compute{};
        bool dlss_dispatch{};
    };

    bool Enabled() const {
        static const bool enabled = std::getenv("EDEN_NX2_TRACE_DLSS_CHAIN") != nullptr;
        return enabled;
    }

    void BeginDispatch() {
        if (!Enabled())
            return;
        if (entries.size() >= max_entries)
            entries.pop_front();
        entries.push_back(Entry{.serial = next_serial++});
    }

    void EndDispatch() {
        if (!Enabled() || emitted || entries.empty() || !output_serial ||
            entries.back().serial <= *output_serial) {
            return;
        }
        Emit();
    }

    void NoteProgram(u64 native_key, bool cache_hit) {
        if (!Enabled() || entries.empty())
            return;
        Entry& entry = entries.back();
        entry.native_key = native_key;
        entry.cache_warm = cache_hit;
        if (native_key == DlssDispatchChain::fortunes_weave_prepass_key ||
            native_key == DlssDispatchChain::star_fox_prepass_key) {
            prepass_serial = entry.serial;
        }
        if (native_key == DlssDispatchChain::fortunes_weave_output_key ||
            native_key == DlssDispatchChain::star_fox_output_key) {
            output_serial = entry.serial;
        }
    }

    void NoteSsbo(SsboRecord record) {
        if (!Enabled() || entries.empty())
            return;
        Entry& entry = entries.back();
        if (entry.ssbo_count < entry.ssbos.size()) {
            entry.ssbos[entry.ssbo_count++] = record;
        }
    }

private:
    struct Entry {
        u64 serial;
        u64 native_key{};
        std::optional<bool> cache_warm;
        std::array<SsboRecord, 16> ssbos;
        u32 ssbo_count{};
    };

    void Emit() {
        emitted = true;
        for (const Entry& entry : entries) {
            if (!prepass_serial || entry.serial < *prepass_serial)
                continue;
            LOG_INFO(HW_GPU, "DLSS chain trace: dispatch #{} key={:#016x} cache={}",
                     entry.serial, entry.native_key,
                     entry.cache_warm ? (*entry.cache_warm ? "warm" : "cold") : "none");
            for (u32 i = 0; i < entry.ssbo_count; ++i) {
                const SsboRecord& ssbo = entry.ssbos[i];
                LOG_INFO(HW_GPU,
                         "DLSS chain trace:   ssbo[{}] cbuf{}+{:#x} mode={} declared={:#x} "
                         "size={:#x}",
                         ssbo.ssbo_index, ssbo.cbuf_index, ssbo.cbuf_offset,
                         ssbo.dlss_dispatch ? "dlss"
                         : ssbo.native_compute ? "native"
                                               : "graphics",
                         ssbo.declared_word, ssbo.chosen_size);
            }
        }
    }

    static constexpr size_t max_entries = 16;
    std::deque<Entry> entries;
    u64 next_serial = 1;
    std::optional<u64> prepass_serial, output_serial;
    bool emitted{};
};

}
