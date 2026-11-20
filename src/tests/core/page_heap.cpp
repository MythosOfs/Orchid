// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "common/literals.h"
#include "core/hle/kernel/k_page_heap.h"

using namespace Common::Literals;

TEST_CASE("Linear graphics backing allocation reuses released pages", "[kernel][heap][page_reuse]") {
    constexpr size_t HeapSize = 256_MiB;
    constexpr size_t BlockSize = 4_MiB;
    constexpr auto HeapStart = Kernel::KPhysicalAddress{0x80000000};
    constexpr auto BlockIndex = Kernel::KPageHeap::GetBlockIndex(BlockSize / Kernel::PageSize);
    static_assert(Kernel::KPageHeap::GetBlockSize(BlockIndex) == BlockSize);

    Kernel::KPageHeap heap;
    const auto management_size = Kernel::KPageHeap::CalculateManagementOverheadSize(HeapSize);
    heap.Initialize(HeapStart, HeapSize,
                    Kernel::KVirtualAddress{std::numeric_limits<uintptr_t>::max() - management_size},
                    management_size);
    heap.Free(HeapStart, HeapSize / Kernel::PageSize);

    std::array<Kernel::KPhysicalAddress, 8> live;
    std::set<uintptr_t> touched;
    for (auto& address : live) {
        address = heap.AllocateBlock(BlockIndex, false);
        REQUIRE(address != 0);
        REQUIRE(GetInteger(address) % BlockSize == 0);
        REQUIRE(touched.insert(GetInteger(address)).second);
    }
    REQUIRE(heap.GetFreeSize() == HeapSize - live.size() * BlockSize);

    for (size_t iteration = 0; iteration < 2048; ++iteration) {
        auto& address = live[iteration % live.size()];
        const auto released = address;
        heap.Free(released, BlockSize / Kernel::PageSize);
        address = heap.AllocateBlock(BlockIndex, false);
        REQUIRE(address == released);
        touched.insert(GetInteger(address));
        REQUIRE(heap.GetFreeSize() == HeapSize - live.size() * BlockSize);
    }
    REQUIRE(touched.size() == live.size());

    for (const auto address : live) {
        heap.Free(address, BlockSize / Kernel::PageSize);
    }
    REQUIRE(heap.GetFreeSize() == HeapSize);
}

TEST_CASE("Page heap visits every free page once and keeps it free", "[kernel][heap][free_blocks]") {
    constexpr size_t HeapSize = 64_MiB;
    constexpr auto HeapStart = Kernel::KPhysicalAddress{0x80000000};
    Kernel::KPageHeap heap;
    const auto management_size = Kernel::KPageHeap::CalculateManagementOverheadSize(HeapSize);
    heap.Initialize(HeapStart, HeapSize,
                    Kernel::KVirtualAddress{std::numeric_limits<uintptr_t>::max() - management_size},
                    management_size);
    heap.Free(HeapStart, HeapSize / Kernel::PageSize);

    // Leave used pages of several block sizes between the free ones.
    const auto big = heap.AllocateBlock(Kernel::KPageHeap::GetBlockIndex(512), false);
    const auto mid = heap.AllocateBlock(Kernel::KPageHeap::GetBlockIndex(16), false);
    const auto small = heap.AllocateBlock(0, false);
    REQUIRE((big != 0 && mid != 0 && small != 0));
    const size_t free_size = heap.GetFreeSize();

    std::set<uintptr_t> visited;
    size_t visited_size = 0;
    heap.ForEachFreeBlock([&](Kernel::KPhysicalAddress block, size_t num_pages) {
        for (size_t page = 0; page < num_pages; ++page) {
            const uintptr_t address = GetInteger(block) + page * Kernel::PageSize;
            REQUIRE(visited.insert(address).second);
            REQUIRE(!(address >= GetInteger(big) && address < GetInteger(big) + 2_MiB));
            REQUIRE(!(address >= GetInteger(mid) && address < GetInteger(mid) + 64_KiB));
            REQUIRE(address != GetInteger(small));
        }
        visited_size += num_pages * Kernel::PageSize;
    });
    REQUIRE(visited_size == free_size);
    REQUIRE(heap.GetFreeSize() == free_size);

    // Still one coalesced heap after giving the used pages back.
    heap.Free(big, 512);
    heap.Free(mid, 16);
    heap.Free(small, 1);
    REQUIRE(heap.GetFreeSize() == HeapSize);
    REQUIRE(heap.AllocateBlock(Kernel::KPageHeap::GetBlockIndex(HeapSize / 2 / Kernel::PageSize),
                               false) != 0);
}
