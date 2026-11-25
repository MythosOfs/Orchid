// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdlib>
#include <deque>
#include <optional>
#include <string>
#include "common/common_types.h"
#include "common/logging.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/engines/dlss_dispatch_chain.h"

namespace Tegra::Engines {

class DlssDispatchTrace {
public:
    struct SsboRecord {
        u32 ssbo_index{};
        u32 cbuf_index{};
        u32 cbuf_offset{};
        u64 declared_word{};
        std::optional<u64> next_word;
        u32 chosen_size{};
        bool bound{};
        bool native_compute{};
        bool dlss_dispatch{};
    };

    struct Entry {
        u64 serial{};
        u64 native_key{};
        std::optional<bool> cache_warm;
        std::array<SsboRecord, Shader::Info::MAX_SSBOS> ssbos;
        u32 ssbo_count{};
        u32 ssbo_overflow{};
        bool is_prepass{};
        bool is_output{};
        [[nodiscard]] bool Ordinary() const {
            return !is_prepass && !is_output;
        }
    };

    bool Enabled() const {
        return test_enabled.value_or([] {
            static const bool enabled = std::getenv("EDEN_NX2_TRACE_DLSS_CHAIN") != nullptr;
            return enabled;
        }());
    }

    void SetTestEnabled(bool enabled) {
        test_enabled = enabled;
    }

    void BeginDispatch() {
        if (!Enabled() || emitted) {
            has_pending = false;
            return;
        }
        pending = Entry{.serial = next_serial++};
        has_pending = true;
        if (capturing && entries.size() >= max_entries) {
            truncated = true;
        }
    }

    void EndDispatch() {
        if (!Enabled() || !has_pending || emitted) {
            return;
        }
        has_pending = false;
        if (!capturing) {
            return;
        }
        if (!truncated) {
            entries.push_back(pending);
        }
        if (truncated || (output_seen && pending.Ordinary())) {
            Emit();
        }
    }

    void NoteProgram(u64 native_key, bool cache_hit) {
        if (!Enabled() || !has_pending || emitted) {
            return;
        }
        pending.native_key = native_key;
        pending.cache_warm = cache_hit;
        if (native_key == DlssDispatchChain::fortunes_weave_prepass_key ||
            native_key == DlssDispatchChain::star_fox_prepass_key) {
            capturing = true;
            output_seen = false;
            entries.clear();
            pending.is_prepass = true;
        }
        if (native_key == DlssDispatchChain::fortunes_weave_output_key ||
            native_key == DlssDispatchChain::star_fox_output_key) {
            if (capturing) {
                output_seen = true;
                pending.is_output = true;
            }
        }
    }

    void NoteSsbo(SsboRecord record) {
        if (!Enabled() || !has_pending || emitted) {
            return;
        }
        if (pending.ssbo_count < pending.ssbos.size()) {
            pending.ssbos[pending.ssbo_count++] = record;
        } else {
            ++pending.ssbo_overflow;
        }
    }

    [[nodiscard]] bool Capturing() const {
        return capturing;
    }
    [[nodiscard]] bool Emitted() const {
        return emitted;
    }
    [[nodiscard]] bool Truncated() const {
        return truncated;
    }
    [[nodiscard]] const std::deque<Entry>& Entries() const {
        return entries;
    }

private:
    void Emit() {
        emitted = true;
        capturing = false;
        LOG_INFO(HW_GPU, "DLSS chain trace: captured {} dispatches truncated={}", entries.size(),
                 truncated);
        for (const Entry& entry : entries) {
            LOG_INFO(HW_GPU, "DLSS chain trace: dispatch #{} key={:#016x} cache={}",
                     entry.serial, entry.native_key,
                     entry.cache_warm ? (*entry.cache_warm ? "warm" : "cold") : "none");
            for (u32 i = 0; i < entry.ssbo_count; ++i) {
                const SsboRecord& ssbo = entry.ssbos[i];
                LOG_INFO(HW_GPU,
                         "DLSS chain trace:   ssbo[{}] cbuf{}+{:#x} mode={} declared={:#x} "
                         "next={} chosen={:#x} bound={}",
                         ssbo.ssbo_index, ssbo.cbuf_index, ssbo.cbuf_offset,
                         ssbo.dlss_dispatch ? "dlss"
                         : ssbo.native_compute ? "native"
                                               : "graphics",
                         ssbo.declared_word,
                         ssbo.next_word ? fmt::format("{:#x}", *ssbo.next_word)
                                        : std::string{"unavailable"},
                         ssbo.chosen_size, ssbo.bound);
            }
            if (entry.ssbo_overflow) {
                LOG_INFO(HW_GPU, "DLSS chain trace:   {} further SSBOs not recorded",
                         entry.ssbo_overflow);
            }
        }
    }

    static constexpr size_t max_entries = DlssDispatchChain::max_dispatches + 2;
    Entry pending{};
    bool has_pending{};
    std::deque<Entry> entries;
    u64 next_serial = 1;
    bool capturing{};
    bool output_seen{};
    bool truncated{};
    bool emitted{};
    std::optional<bool> test_enabled;
};

}
