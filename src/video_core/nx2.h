// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace Tegra {

inline std::atomic<bool> g_nx2_ampere{false};

inline constexpr u64 Nx2WrittenPageBits = 12;
inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenPageBits)) / 64> g_nx2_gpu_written_pages{};
inline std::atomic<u64> g_nx2_gpu_written_generation{};

inline void Nx2MarkGpuWritten(u64 device_address, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 last = std::min<u64>((device_address + size - 1) >> Nx2WrittenPageBits,
                                   g_nx2_gpu_written_pages.size() * 64 - 1);
    const u64 first = device_address >> Nx2WrittenPageBits;
    for (u64 index = first / 64; first <= last && index <= last / 64; ++index) {
        const u64 low = index == first / 64 ? first % 64 : 0;
        const u64 high = index == last / 64 ? last % 64 : 63;
        const u64 bits = (~u64{0} >> (63 - high)) & (~u64{0} << low);
        auto& word = g_nx2_gpu_written_pages[index];
        if ((word.load(std::memory_order_relaxed) & bits) != bits) {
            if ((word.fetch_or(bits, std::memory_order_relaxed) & bits) != bits) {
                g_nx2_gpu_written_generation.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenPageBits)) / 64> g_nx2_command_list_pages{};
inline std::atomic<bool> g_nx2_has_command_list_pages{false};

inline void Nx2MarkCommandListPage(u64 device_address) {
    const u64 page = device_address >> Nx2WrittenPageBits;
    if (page >= g_nx2_command_list_pages.size() * 64) {
        return;
    }
    auto& word = g_nx2_command_list_pages[page / 64];
    const u64 bit = u64{1} << (page % 64);
    if ((word.load(std::memory_order_relaxed) & bit) == 0) {
        word.fetch_or(bit, std::memory_order_relaxed);
        g_nx2_has_command_list_pages.store(true, std::memory_order_relaxed);
    }
}

[[nodiscard]] inline bool Nx2IsCommandListPage(u64 device_address) {
    const u64 page = device_address >> Nx2WrittenPageBits;
    if (page >= g_nx2_command_list_pages.size() * 64) {
        return false;
    }
    return (g_nx2_command_list_pages[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1;
}

inline void Nx2ClearGpuWrittenPages() {
    for (auto& word : g_nx2_gpu_written_pages) {
        word.store(0, std::memory_order_relaxed);
    }
    for (auto& word : g_nx2_command_list_pages) {
        word.store(0, std::memory_order_relaxed);
    }
    g_nx2_has_command_list_pages.store(false, std::memory_order_relaxed);
}

[[nodiscard]] inline bool Nx2IsGpuWritten(u64 device_address) {
    const u64 page = device_address >> Nx2WrittenPageBits;
    if (page >= g_nx2_gpu_written_pages.size() * 64) {
        return false;
    }
    return (g_nx2_gpu_written_pages[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1;
}

template <typename Translate>
[[nodiscard]] bool Nx2HasGpuWrittenPage(GPUVAddr address, size_t size, Translate&& translate) {
    if (size == 0) {
        return false;
    }
    const GPUVAddr last = address + size - 1;
    if (last < address) {
        return true;
    }
    constexpr GPUVAddr mask = (GPUVAddr{1} << Nx2WrittenPageBits) - 1;
    for (GPUVAddr page = address & ~mask;; page += mask + 1) {
        if (const auto device = translate(page); device && Nx2IsGpuWritten(*device)) {
            return true;
        }
        if (page == (last & ~mask)) {
            return false;
        }
    }
}

template <typename Drain, typename Translate, typename Flush>
void Nx2SynchronizeGpuWrittenCommands(GPUVAddr address, size_t size, Drain&& drain,
                                     Translate&& translate, Flush&& flush) {
    if (size == 0) return;
    drain();
    if (Nx2HasGpuWrittenPage(address, size, translate)) flush();
}

inline bool Nx2CheckCommandPagesFirst() {
    static const bool drain_first = [] {
        const char* value = std::getenv("EDEN_NX2_CMD_DRAIN");
        return value != nullptr && value[0] == '1';
    }();
    return !drain_first;
}

[[nodiscard]] inline bool Nx2AcquireQueuedEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_ACQUIRE_QUEUED");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

class Nx2QueuedMaps {
public:
    void Add(GPUVAddr address, u64 size) {
        std::scoped_lock lk{mutex};
        ranges.emplace_back(address, size);
    }

    void Remove(GPUVAddr address, u64 size) {
        std::scoped_lock lk{mutex};
        if (const auto it = std::ranges::find(ranges, std::pair{address, size});
            it != ranges.end()) {
            ranges.erase(it);
        }
    }

    [[nodiscard]] bool Overlaps(GPUVAddr address, u64 size) const {
        std::scoped_lock lk{mutex};
        return std::ranges::any_of(ranges, [&](const auto& range) {
            return address < range.first + range.second && range.first < address + size;
        });
    }

private:
    mutable std::mutex mutex;
    std::vector<std::pair<GPUVAddr, u64>> ranges;
};

using Nx2SparseBackingHandler = std::function<void(u64 gpu_address, u64 size, const u8* data)>;
inline std::recursive_mutex g_nx2_sparse_backing_mutex;
inline Nx2SparseBackingHandler g_nx2_sparse_backing;
inline std::atomic<u64> g_nx2_sparse_low{~u64{0}};
inline std::atomic<u64> g_nx2_sparse_high{0};

inline void Nx2SetSparseBacking(Nx2SparseBackingHandler handler) {
    std::scoped_lock lk{g_nx2_sparse_backing_mutex};
    g_nx2_sparse_backing = std::move(handler);
    g_nx2_sparse_low.store(~u64{0});
    g_nx2_sparse_high.store(0);
}

inline void Nx2AddSparseSpan(u64 gpu_address, u64 size) {
    u64 low = g_nx2_sparse_low.load();
    while (gpu_address < low && !g_nx2_sparse_low.compare_exchange_weak(low, gpu_address)) {
    }
    u64 high = g_nx2_sparse_high.load();
    while (gpu_address + size > high &&
           !g_nx2_sparse_high.compare_exchange_weak(high, gpu_address + size)) {
    }
}

inline void Nx2BackSparseWrite(u64 gpu_address, u64 size, const void* data) {
    if (gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }
    std::scoped_lock lk{g_nx2_sparse_backing_mutex};
    if (g_nx2_sparse_backing) {
        g_nx2_sparse_backing(gpu_address, size, static_cast<const u8*>(data));
    }
}

struct Nx2SparseRegionState {
    u64 size;
    u64 block_size;
    std::vector<bool> written;
    std::vector<bool> mapped_pages;
};

inline constexpr u64 Nx2SparseMappingPageSize = 4ULL << 10;
inline std::mutex g_nx2_sparse_state_mutex;
inline std::map<u64, Nx2SparseRegionState> g_nx2_sparse_state;

inline void Nx2SparseRegionCreated(u64 gpu_address, u64 size, u64 block_size) {
    if (size == 0 || block_size == 0 || size > (std::numeric_limits<u64>::max)() - gpu_address) {
        return;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state[gpu_address] = {
        size, block_size, std::vector<bool>(size / block_size + (size % block_size != 0))};
}

inline void Nx2SparseRegionDestroyed(u64 gpu_address) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state.erase(gpu_address);
}

inline void Nx2ClearSparseRegions() {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state.clear();
}

inline void Nx2SparseBlockWritten(u64 region_address, u64 block) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    if (const auto it = g_nx2_sparse_state.find(region_address);
        it != g_nx2_sparse_state.end() && block < it->second.written.size()) {
        it->second.written[block] = true;
    }
}

inline bool Nx2SparseBlockIsWritten(u64 region_address, u64 block) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    const auto it = g_nx2_sparse_state.find(region_address);
    return it == g_nx2_sparse_state.end() || block >= it->second.written.size() ||
           it->second.written[block];
}

inline void Nx2SparseTilesMapped(u64 region_address, u64 offset, u64 size, bool mapped) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    const auto it = g_nx2_sparse_state.find(region_address);
    if (it == g_nx2_sparse_state.end() || size == 0) {
        return;
    }
    auto& region = it->second;
    if (offset >= region.size || size > region.size - offset) {
        return;
    }
    if (region.mapped_pages.empty()) {
        region.mapped_pages.resize(region.size / Nx2SparseMappingPageSize +
                                   (region.size % Nx2SparseMappingPageSize != 0));
    }
    const u64 last = (offset + size - 1) / Nx2SparseMappingPageSize;
    for (u64 tile = offset / Nx2SparseMappingPageSize; tile <= last; ++tile) {
        region.mapped_pages[tile] = mapped;
    }
}

inline std::vector<std::pair<u64, u64>> Nx2SparseDataRanges(u64 gpu_address, u64 size) {
    std::vector<std::pair<u64, u64>> ranges;
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    auto it = g_nx2_sparse_state.upper_bound(gpu_address);
    if (it == g_nx2_sparse_state.begin()) {
        return ranges;
    }
    --it;
    const auto& [start, region] = *it;
    if (size == 0 || gpu_address - start >= region.size ||
        size > region.size - (gpu_address - start)) {
        return ranges;
    }
    const bool by_tile = !region.mapped_pages.empty();
    const u64 unit = by_tile ? Nx2SparseMappingPageSize : region.block_size;
    const std::vector<bool>& present = by_tile ? region.mapped_pages : region.written;
    const u64 begin = gpu_address - start;
    const u64 end = begin + size;
    for (u64 index = begin / unit; index * unit < end && index < present.size(); ++index) {
        if (!present[index]) {
            continue;
        }
        const u64 run_begin = (std::max)(index * unit, begin) - begin;
        const u64 run_end = index * unit + (std::min)(unit, end - index * unit) - begin;
        if (!ranges.empty() && ranges.back().first + ranges.back().second == run_begin) {
            ranges.back().second += run_end - run_begin;
        } else {
            ranges.emplace_back(run_begin, run_end - run_begin);
        }
    }
    return ranges;
}

inline bool Nx2InSparseRegion(u64 gpu_address, u64 size) {
    if (size == 0 || gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return false;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    auto it = g_nx2_sparse_state.upper_bound(gpu_address);
    if (it == g_nx2_sparse_state.begin()) {
        return false;
    }
    --it;
    return gpu_address + size <= it->first + it->second.size;
}

inline void Nx2SparseMarkWritten(u64 gpu_address, u64 size) {
    if (size == 0 || gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    for (auto& [start, region] : g_nx2_sparse_state) {
        const u64 end = start + region.size;
        if (gpu_address >= end || gpu_address + size <= start) {
            continue;
        }
        const u64 first = ((std::max)(gpu_address, start) - start) / region.block_size;
        const u64 last = ((std::min)(gpu_address + size, end) - 1 - start) / region.block_size;
        for (u64 block = first; block <= last; ++block) {
            region.written[block] = true;
        }
    }
}

inline bool Nx2SparseUnwritten(u64 gpu_address, u64 size) {
    if (size == 0 || gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return false;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    auto it = g_nx2_sparse_state.upper_bound(gpu_address);
    if (it == g_nx2_sparse_state.begin()) {
        return false;
    }
    --it;
    const auto& [start, region] = *it;
    if (gpu_address + size > start + region.size) {
        return false;
    }
    const u64 last = (gpu_address + size - 1 - start) / region.block_size;
    for (u64 block = (gpu_address - start) / region.block_size; block <= last; ++block) {
        if (region.written[block]) {
            return false;
        }
    }
    return true;
}

class Nx2SemaphoreWaiters {
public:
    static constexpr u32 AcquireEqual = 0x1;
    static constexpr u32 AcquireGequal = 0x4;
    static constexpr u32 AcquireMask = 0x8;

    static Nx2SemaphoreWaiters& Get() {
        static Nx2SemaphoreWaiters waiters;
        return waiters;
    }

    void Add(const void* owner, u64 device_address, u32 value, u32 operation) {
        std::scoped_lock lk{mutex};
        std::erase_if(waiters, [owner](const Waiter& w) { return w.owner == owner; });
        waiters.push_back({owner, device_address, value, operation, false});
        count.store(waiters.size(), std::memory_order_release);
    }

    void Remove(const void* owner) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::scoped_lock lk{mutex};
        std::erase_if(waiters, [owner](const Waiter& w) { return w.owner == owner; });
        count.store(waiters.size(), std::memory_order_release);
    }

    [[nodiscard]] bool Latched(const void* owner) {
        if (count.load(std::memory_order_acquire) == 0) {
            return false;
        }
        std::scoped_lock lk{mutex};
        return std::ranges::any_of(waiters,
                                   [owner](const Waiter& w) { return w.owner == owner && w.latched; });
    }

    void OnWrite(u64 device_address, u32 value) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::scoped_lock lk{mutex};
        for (auto& w : waiters) {
            if (w.address == device_address && !w.latched && Matches(w, value)) {
                w.latched = true;
            }
        }
    }

private:
    struct Waiter {
        const void* owner;
        u64 address;
        u32 value;
        u32 operation;
        bool latched;
    };

    static bool Matches(const Waiter& w, u32 current) {
        switch (w.operation) {
        case AcquireEqual:
            return current == w.value;
        case AcquireGequal:
            return current >= w.value;
        case AcquireMask:
            return (current & w.value) != 0;
        default:
            return false;
        }
    }

    std::mutex mutex;
    std::vector<Waiter> waiters;
    std::atomic<size_t> count{};
};

}
