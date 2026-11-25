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
#include "video_core/buffer_cache/storage_buffer_size.h"
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
        VideoCommon::StorageBufferSizeRule size_rule{
            VideoCommon::StorageBufferSizeRule::NotDetermined};
        u32 logical_range{};
        u8 host_path{};
        u32 host_offset{};
        u64 host_span{};
        u32 host_segments{};
        u32 source_offset{};
    };

    enum class Outcome : u8 {
        Succeeded,
        Rejected,
        Keyless,
    };

    struct Entry {
        u64 serial{};
        u64 native_key{};
        std::optional<bool> cache_warm;
        Outcome outcome{Outcome::Succeeded};
        std::array<SsboRecord, Shader::Info::MAX_SSBOS> ssbos;
        u32 ssbo_count{};
        u32 ssbo_overflow{};
        bool is_prepass{};
        bool is_output{};
        [[nodiscard]] bool Ordinary() const {
            return !is_prepass && !is_output;
        }
    };

    struct Sequence {
        u32 index{};
        bool truncated{};
        std::deque<Entry> entries;
    };

    bool Enabled() const {
        return test_enabled.value_or(DefaultOptIn());
    }

    static bool DefaultOptIn() {
        static const bool enabled = std::getenv("EDEN_NX2_TRACE_DLSS_CHAIN") != nullptr;
        return enabled;
    }

    void SetTestEnabled(bool enabled) {
        test_enabled = enabled;
    }

    void BeginDispatch() {
        if (!Enabled() || Emitted()) {
            has_pending = false;
            return;
        }
        pending = Entry{.serial = next_serial++};
        has_pending = true;
        if (capturing && entries.size() >= max_entries) {
            truncated = true;
        }
    }

    void EndDispatch(bool succeeded) {
        if (!Enabled() || !has_pending || Emitted()) {
            return;
        }
        has_pending = false;
        if (!capturing) {
            return;
        }
        if (!succeeded) {
            pending.outcome = Outcome::Rejected;
        } else if (pending.Ordinary() && pending.native_key == 0) {
            pending.outcome = Outcome::Keyless;
        }
        if (!truncated) {
            entries.push_back(pending);
        }
        if (truncated || (output_seen && pending.Ordinary())) {
            Emit();
        }
    }

    void NoteProgram(u64 native_key, bool cache_hit) {
        if (!Enabled() || !has_pending || Emitted()) {
            return;
        }
        pending.native_key = native_key;
        pending.cache_warm = cache_hit;
        if (native_key == DlssDispatchChain::fortunes_weave_prepass_key ||
            native_key == DlssDispatchChain::star_fox_prepass_key) {
            capturing = true;
            output_seen = false;
            truncated = false;
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
        if (!Enabled() || !has_pending || !capturing || Emitted()) {
            return;
        }
        if (pending.ssbo_count < pending.ssbos.size()) {
            pending.ssbos[pending.ssbo_count++] = record;
        } else {
            ++pending.ssbo_overflow;
        }
    }

    void NoteSsboHost(u32 ssbo_index, bool bound, u8 host_path, u32 host_offset, u64 host_span,
                      u32 host_segments, u32 source_offset) {
        if (!Enabled() || !has_pending || !capturing || Emitted()) {
            return;
        }
        for (u32 i = pending.ssbo_count; i > 0; --i) {
            SsboRecord& record = pending.ssbos[i - 1];
            if (record.ssbo_index == ssbo_index) {
                record.bound = bound;
                record.host_path = host_path;
                record.host_offset = host_offset;
                record.host_span = host_span;
                record.host_segments = host_segments;
                record.source_offset = source_offset;
                return;
            }
        }
    }

    [[nodiscard]] bool Capturing() const {
        return Enabled() && capturing && !Emitted();
    }
    [[nodiscard]] bool Emitted() const {
        return sequences.size() >= max_sequences;
    }
    [[nodiscard]] bool Truncated() const {
        return truncated;
    }
    [[nodiscard]] const std::deque<Entry>& Entries() const {
        return entries;
    }
    [[nodiscard]] const std::deque<Sequence>& Sequences() const {
        return sequences;
    }

private:
    static const char* OutcomeName(Outcome outcome) {
        switch (outcome) {
        case Outcome::Succeeded:
            return "success";
        case Outcome::Rejected:
            return "rejected";
        case Outcome::Keyless:
            return "keyless";
        }
        return "unknown";
    }

    static const char* SizeRuleName(VideoCommon::StorageBufferSizeRule rule) {
        switch (rule) {
        case VideoCommon::StorageBufferSizeRule::Declared:
            return "declared";
        case VideoCommon::StorageBufferSizeRule::BarePointer:
            return "bare";
        case VideoCommon::StorageBufferSizeRule::FallbackCap:
            return "cap";
        default:
            return "none";
        }
    }

    static const char* HostPathName(u8 path) {
        switch (path) {
        case 1:
            return "single";
        case 2:
            return "multi";
        default:
            return "null";
        }
    }

    void Emit() {
        capturing = false;
        sequences.push_back(Sequence{
            .index = static_cast<u32>(sequences.size()),
            .truncated = truncated,
            .entries = entries,
        });
        const Sequence& sequence = sequences.back();
        LOG_INFO(HW_GPU, "DLSS chain trace: sequence #{} captured {} dispatches truncated={}",
                 sequence.index, sequence.entries.size(), sequence.truncated);
        for (const Entry& entry : sequence.entries) {
            LOG_INFO(HW_GPU, "DLSS chain trace: dispatch #{} key={:#016x} cache={} outcome={}",
                     entry.serial, entry.native_key,
                     entry.cache_warm ? (*entry.cache_warm ? "warm" : "cold") : "none",
                     OutcomeName(entry.outcome));
            for (u32 i = 0; i < entry.ssbo_count; ++i) {
                const SsboRecord& ssbo = entry.ssbos[i];
                LOG_INFO(HW_GPU,
                         "DLSS chain trace:   ssbo[{}] cbuf{}+{:#x} mode={} rule={} declared={:#x} "
                         "next={} logical={:#x} chosen={:#x} path={} host={:#x}+{:#x}/{} src={:#x} bound={}",
                         ssbo.ssbo_index, ssbo.cbuf_index, ssbo.cbuf_offset,
                         ssbo.dlss_dispatch ? "dlss"
                         : ssbo.native_compute ? "native"
                                               : "graphics",
                         SizeRuleName(ssbo.size_rule), ssbo.declared_word,
                         ssbo.next_word ? fmt::format("{:#x}", *ssbo.next_word)
                                        : std::string{"unavailable"},
                         ssbo.logical_range, ssbo.chosen_size, HostPathName(ssbo.host_path),
                         ssbo.host_offset, ssbo.host_span, ssbo.host_segments,
                         ssbo.source_offset, ssbo.bound);
            }
            if (entry.ssbo_overflow) {
                LOG_INFO(HW_GPU, "DLSS chain trace:   {} further SSBOs not recorded",
                         entry.ssbo_overflow);
            }
        }
        entries.clear();
    }

    static constexpr size_t max_entries = DlssDispatchChain::max_dispatches + 2;
    static constexpr size_t max_sequences = 2;
    Entry pending{};
    bool has_pending{};
    std::deque<Entry> entries;
    std::deque<Sequence> sequences;
    u64 next_serial = 1;
    bool capturing{};
    bool output_seen{};
    bool truncated{};
    std::optional<bool> test_enabled;
};

}
