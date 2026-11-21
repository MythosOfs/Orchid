// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN32
#include <windows.h>
#include <catch2/catch_test_macros.hpp>

#include "common/host_memory.h"

TEST_CASE("HostMemory: Fallback allocation is released", "[common][host_memory]") {
    u8* backing{};
    {
        Common::HostMemory memory{0x1000, 1ULL << 63};
        REQUIRE(memory.VirtualBasePointer() == nullptr);
        backing = memory.BackingBasePointer();
        REQUIRE(backing != nullptr);
        MEMORY_BASIC_INFORMATION info{};
        REQUIRE(VirtualQuery(backing, &info, sizeof(info)) == sizeof(info));
        REQUIRE(info.State == MEM_COMMIT);
    }
    MEMORY_BASIC_INFORMATION info{};
    REQUIRE(VirtualQuery(backing, &info, sizeof(info)) == sizeof(info));
    REQUIRE(info.State == MEM_FREE);
}
#endif

#ifdef __linux__
#include <cstring>
#include <optional>
#include <sys/resource.h>
#include <catch2/catch_test_macros.hpp>

#include "common/host_memory.h"

TEST_CASE("HostMemory: No memfd leads to the plain buffer", "[common][host_memory]") {
    rlimit old_limit{};
    REQUIRE(getrlimit(RLIMIT_NOFILE, &old_limit) == 0);
    rlimit no_files = old_limit;
    no_files.rlim_cur = 0; // memfd_create now fails with EMFILE
    REQUIRE(setrlimit(RLIMIT_NOFILE, &no_files) == 0);
    std::optional<Common::HostMemory> memory;
    memory.emplace(0x10000, 0x10000);
    REQUIRE(setrlimit(RLIMIT_NOFILE, &old_limit) == 0);
    CHECK(memory->VirtualBasePointer() == nullptr);
    REQUIRE(memory->BackingBasePointer() != nullptr);
    std::memset(memory->BackingBasePointer(), 0xAB, 0x10000);
    CHECK(memory->BackingBasePointer()[0xFFFF] == 0xAB);
    memory->Discard(0, 0x10000); // no memfd to punch: the plain buffer keeps its contents
    CHECK(memory->BackingBasePointer()[0xFFFF] == 0xAB);
}
#endif
