// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <cstddef>
#include <utility>
#include <boost/container/small_vector.hpp>
#include "common/common_types.h"
#include "common/slot_vector.h"

namespace VideoCommon {

template <class Image>
struct PendingImageDownload {
    bool is_swizzle;
    size_t async_buffer_id;
    Common::SlotId object_id;
    std::shared_ptr<Image> retired_image{};

    Image& GetImage(Common::SlotVector<Image>& images) const {
        return retired_image ? *retired_image : images[object_id];
    }
};

// Whole-image copies are recorded together at commit time. Repeated requests
// for the same live image therefore observe the same contents. Keep the first
// occurrence so reverse CPU writeback order remains unchanged for aliases.
template <class Image, class Downloads>
size_t DeduplicatePendingImageDownloads(Downloads& downloads, Common::SlotVector<Image>& images) {
    std::unordered_set<const Image*> seen;
    const size_t previous_size = downloads.size();
    const auto end = std::remove_if(downloads.begin(), downloads.end(), [&](const auto& download) {
        return download.is_swizzle && !seen.insert(&download.GetImage(images)).second;
    });
    downloads.erase(end, downloads.end());
    return previous_size - downloads.size();
}

// Cache slots may be recycled before queued GPU downloads are submitted or
// consumed. Keep the actual image, including its metadata, until those finish.
template <class Image, class Downloads, class Batches>
bool RetainPendingImageDownloads(Common::SlotId id, Image& image, Downloads& pending,
                                 Batches& submitted) {
    std::shared_ptr<Image> retained;
    const auto retain = [&](auto& downloads) {
        for (auto& download : downloads) {
            if (!download.is_swizzle || download.object_id != id || download.retired_image) continue;
            if (!retained) retained = std::make_shared<Image>(std::move(image));
            download.retired_image = retained;
        }
    };
    retain(pending);
    for (auto& downloads : submitted) retain(downloads);
    return retained != nullptr;
}

/// Host memory behind the destination of a queued image-to-buffer download, in address order.
/// Gathered when the copy is queued: the guest can unmap the GPU and device addresses before the
/// download lands, and the bytes still belong in its memory (Pokemon Pokopia unmaps its photo
/// buffer right after the copy and saved corrupt photos, issue 58). nullptr runs are unmapped.
using HostRuns = boost::container::small_vector<std::pair<u8*, size_t>, 1>;

template <class GetPointer>
HostRuns GatherHostRuns(u64 address, size_t size, size_t page_size, GetPointer&& get_pointer) {
    HostRuns runs;
    for (size_t offset = 0; offset < size;) {
        const u64 current = address + offset;
        const size_t chunk = (std::min)(size - offset, page_size - (current & (page_size - 1)));
        u8* const pointer = get_pointer(current);
        if (!runs.empty() && (runs.back().first == nullptr) == (pointer == nullptr) &&
            (pointer == nullptr || runs.back().first + runs.back().second == pointer)) {
            runs.back().second += chunk;
        } else {
            runs.emplace_back(pointer, chunk);
        }
        offset += chunk;
    }
    return runs;
}

inline void WriteHostRuns(const HostRuns& runs, const u8* data) {
    for (const auto& [pointer, size] : runs) {
        if (pointer != nullptr) {
            std::memcpy(pointer, data, size);
        }
        data += size;
    }
}

} // namespace VideoCommon
