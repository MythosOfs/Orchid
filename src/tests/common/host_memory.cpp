// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <sys/mman.h>
#endif

#include "common/host_memory.h"
#include "common/literals.h"

using Common::HostMemory;
using namespace Common::Literals;

static constexpr size_t VIRTUAL_SIZE = 1ULL << 39;
static constexpr size_t BACKING_SIZE = 4_GiB;
static constexpr auto PERMS = Common::MemoryPermission::ReadWrite;
static constexpr auto HEAP = false;

TEST_CASE("HostMemory: Simple map", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x8000, 0x1000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[0] = 50;
    REQUIRE(data[0] == 50);
}

TEST_CASE("HostMemory: Simple mirror map", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);
    mem.Map(0x8000, 0x4000, 0x1000, PERMS, HEAP);

    volatile u8* const mirror_a = mem.VirtualBasePointer() + 0x5000;
    volatile u8* const mirror_b = mem.VirtualBasePointer() + 0x8000;
    mirror_b[0] = 76;
    REQUIRE(mirror_a[0x1000] == 76);
}

TEST_CASE("HostMemory: Simple unmap and remap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[0] = 50;
    REQUIRE(data[0] == 50);

    mem.Unmap(0x5000, 0x2000, HEAP);

    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);
    REQUIRE(data[0] == 50);

    mem.Map(0x7000, 0x2000, 0x5000, PERMS, HEAP);
    REQUIRE(data[0x3000] == 50);
}

TEST_CASE("HostMemory: Nieche allocation", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x20000, PERMS, HEAP);
    mem.Unmap(0x0000, 0x4000, HEAP);
    mem.Map(0x1000, 0, 0x2000, PERMS, HEAP);
    mem.Map(0x3000, 0, 0x1000, PERMS, HEAP);
    mem.Map(0, 0, 0x1000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Full unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x8000, 0x4000, HEAP);
    mem.Map(0x6000, 0, 0x16000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Right out of bounds unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x2000, 0x4000, HEAP);
    mem.Map(0x2000, 0x80000, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Left out of bounds unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);

    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x6000, 0x4000, HEAP);
    mem.Map(0x8000, 0, 0x2000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Multiple placeholder unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x1b000, PERMS, HEAP);
    mem.Unmap(0x3000, 0x1c000, HEAP);
    mem.Map(0x3000, 0, 0x20000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap between placeholders", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x2000, 0x4000, HEAP);
    mem.Map(0x2000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap to origin", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x4000, 0x4000, HEAP);
    mem.Map(0, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap to right", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x8000, 0x4000, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Partial right unmap check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x1000] = 17;

    mem.Unmap(0x6000, 0x2000, HEAP);

    REQUIRE(ptr[0x1000] == 17);
}

TEST_CASE("HostMemory: Partial left unmap check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x3000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x4000, 0x2000, HEAP);

    REQUIRE(ptr[0x3000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("HostMemory: Partial middle unmap check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x0000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x1000, 0x2000, HEAP);

    REQUIRE(ptr[0x0000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("HostMemory: Partial sparse middle unmap and check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x2000, PERMS, HEAP);
    mem.Map(0x6000, 0x20000, 0x2000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x0000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x5000, 0x2000, HEAP);

    REQUIRE(ptr[0x0000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

#ifdef __linux__
static bool Resident(const u8* address) {
    unsigned char vec = 0;
    REQUIRE(mincore(const_cast<u8*>(address), 0x1000, &vec) == 0);
    return (vec & 1) != 0;
}

TEST_CASE("HostMemory: Discard gives the backing back and keeps it dirty", "[common][discard]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    u8* const backing = mem.BackingBasePointer();
    mem.Map(0x40000, 0x20000, 0x3000, PERMS, HEAP);
    volatile u8* const view = mem.VirtualBasePointer() + 0x40000;
    mem.MarkUsed(0x20000, 0x3000);
    mem.MarkDirty(0x20000, 0x3000);
    view[0x0000] = 11;
    view[0x1000] = 22;
    view[0x2000] = 33;
    REQUIRE(Resident(backing + 0x21000));

    mem.Discard(0x21000, 0x1000);
    REQUIRE(!Resident(backing + 0x21000));
    REQUIRE(view[0x1000] == 0);
    REQUIRE(view[0x0000] == 11);
    REQUIRE(view[0x2000] == 33);

    // Written again after the discard: the next allocation must still clear it.
    view[0x1000] = 44;
    mem.ClearBackingRegion(0x21000, 0x1000, 0);
    REQUIRE(view[0x1000] == 0);
}
#endif

static std::string PageProtection([[maybe_unused]] const volatile u8* address) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    REQUIRE(VirtualQuery(const_cast<const u8*>(address), &info, sizeof(info)) == sizeof(info));
    switch (info.Protect) {
    case PAGE_READWRITE:
        return "rw";
    case PAGE_READONLY:
        return "r-";
    case PAGE_NOACCESS:
        return "--";
    default:
        return std::to_string(info.Protect);
    }
#elif defined(__linux__)
    const auto page = reinterpret_cast<uintptr_t>(address);
    std::ifstream maps{"/proc/self/maps"};
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long long begin{};
        unsigned long long end{};
        char perms[5]{};
        if (std::sscanf(line.c_str(), "%llx-%llx %4s", &begin, &end, perms) == 3 &&
            page >= begin && page < end) {
            return std::string(perms, 2);
        }
    }
    return "unmapped";
#else
    return {};
#endif
}

TEST_CASE("HostMemory: Partial unmaps keep the protection of the rest of the view", "[common]") {
#if !defined(_WIN32) && !defined(__linux__)
    SKIP("Page protection is read with VirtualQuery or from /proc/self/maps");
#endif
    constexpr size_t Page = 0x1000;
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    volatile u8* const base = mem.VirtualBasePointer();

    mem.Map(0x40000, 0x10000, 4 * Page, PERMS, HEAP);
    base[0x40000] = 1;
    base[0x43000] = 4;
    mem.Protect(0x40000, Page, Common::MemoryPermission::Read);
    mem.Protect(0x43000, Page, Common::MemoryPermission::Read);
    mem.Unmap(0x41000, 2 * Page, HEAP);
    REQUIRE(PageProtection(base + 0x40000) == "r-");
    REQUIRE(PageProtection(base + 0x43000) == "r-");
    REQUIRE(base[0x40000] == 1);
    REQUIRE(base[0x43000] == 4);
    mem.Protect(0x40000, Page, PERMS);
    mem.Protect(0x43000, Page, PERMS);
    REQUIRE(PageProtection(base + 0x40000) == "rw");
    REQUIRE(PageProtection(base + 0x43000) == "rw");
    base[0x43000] = 0x44;
    REQUIRE(mem.BackingBasePointer()[0x13000] == 0x44);

    mem.Map(0x80000, 0x20000, 6 * Page, PERMS, HEAP);
    for (size_t page = 0; page < 6; ++page) {
        base[0x80000 + page * Page] = static_cast<u8>(0x10 + page);
    }
    mem.Protect(0x80000, Page, Common::MemoryPermission::Read);
    mem.Protect(0x84000, Page, Common::MemoryPermission{});
    mem.Protect(0x85000, Page, Common::MemoryPermission::Read);
    mem.Unmap(0x82000, 2 * Page, HEAP);
    REQUIRE(PageProtection(base + 0x80000) == "r-");
    REQUIRE(PageProtection(base + 0x81000) == "rw");
    REQUIRE(PageProtection(base + 0x84000) == "--");
    REQUIRE(PageProtection(base + 0x85000) == "r-");
    REQUIRE(base[0x80000] == 0x10);
    REQUIRE(base[0x81000] == 0x11);
    REQUIRE(mem.BackingBasePointer()[0x24000] == 0x14);
    REQUIRE(base[0x85000] == 0x15);
    base[0x81000] = 0x21;
    REQUIRE(mem.BackingBasePointer()[0x21000] == 0x21);
    mem.Protect(0x84000, 2 * Page, PERMS);
    REQUIRE(PageProtection(base + 0x84000) == "rw");
    REQUIRE(PageProtection(base + 0x85000) == "rw");
    base[0x84000] = 0x24;
    REQUIRE(mem.BackingBasePointer()[0x24000] == 0x24);
}
