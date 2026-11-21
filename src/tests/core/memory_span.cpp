#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"

TEST_CASE("Guest memory spans validate every covered page", "[core][memory_span]") {
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    Kernel::KProcess process{system.Kernel()};
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };

    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, Core::Memory::YUZU_PAGEBITS);
    auto& memory = process.GetMemory();
    memory.SetCurrentPageTable(process);
    u8* const backing = system.DeviceMemory().buffer.BackingBasePointer();
    constexpr auto page_size = Core::Memory::YUZU_PAGESIZE;
    const auto map = [&](u64 page, u16 mapping_id, u64 host_page) {
        const auto host = reinterpret_cast<uintptr_t>(backing + host_page * page_size);
        table.entries.GetAndFault(page).Store(false, Common::PageType::Memory, mapping_id,
                                              host - page * page_size);
    };

    map(4, 1, 16);
    map(5, 2, 32);
    map(6, 1, 18);
    CHECK(memory.GetSpan(4 * page_size, 2 * page_size + 1) == nullptr);
    CHECK(std::as_const(memory).GetSpan(4 * page_size, 2 * page_size + 1) == nullptr);

    map(5, 3, 17);
    map(7, 4, 48);
    CHECK(memory.GetSpan(4 * page_size, 3 * page_size) == backing + 16 * page_size);
    CHECK(memory.GetSpan(4 * page_size + 1, 3 * page_size - 1) ==
          backing + 16 * page_size + 1);
    CHECK(memory.GetSpan(4 * page_size, 3 * page_size + 1) == nullptr);

    table.entries.GetAndFault(5).MarkRasterizerCached();
    CHECK(memory.GetSpan(4 * page_size, 3 * page_size) == backing + 16 * page_size);
    table.entries.GetAndFault(5).Store(false, Common::PageType::Unmapped, 1, 0);
    CHECK(memory.GetSpan(4 * page_size, 3 * page_size) == nullptr);
}
