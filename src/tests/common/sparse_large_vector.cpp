// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/sparse_large_vector.h"

#ifdef _WIN32
namespace {

using Entry = u64;

const std::size_t EntriesPerHostPage = Common::HostPageSize / sizeof(Entry);

uintptr_t PageOf(const Common::SparseLargeVector<Entry>& entries, std::size_t index) {
    return reinterpret_cast<uintptr_t>(entries.data() + index) & Common::HostPageMask;
}

} // namespace

TEST_CASE("SparseLargeVector fault handler keeps a page another thread committed writable",
          "[common]") {
    Common::SparseLargeVector<Entry> entries(EntriesPerHostPage * 2);
    const std::size_t index = EntriesPerHostPage + 3;

    entries.CommitRegion(index, index + 1);
    entries.GetUnchecked(index) = 1;

    REQUIRE(Common::CommitVectorPage(PageOf(entries, index)));

    CHECK(entries[index] == 1);
    entries.GetUnchecked(index) = 2;
    CHECK(entries[index] == 2);
}

TEST_CASE("SparseLargeVector read fault commits a zeroed page that stays writable", "[common]") {
    Common::SparseLargeVector<Entry> entries(EntriesPerHostPage * 2);
    const std::size_t index = EntriesPerHostPage + 5;

    CHECK(static_cast<volatile const Entry&>(entries.GetUnchecked(index)) == 0);
    CHECK(static_cast<volatile const Entry&>(entries.GetUnchecked(index + 1)) == 0);

    static_cast<volatile Entry&>(entries.GetUnchecked(index)) = 3;
    CHECK(static_cast<volatile const Entry&>(entries.GetUnchecked(index)) == 3);
    CHECK(static_cast<volatile const Entry&>(entries.GetUnchecked(index + 1)) == 0);
}
#endif
