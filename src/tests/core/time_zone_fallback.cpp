// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/system_archive/time_zone_binary.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/psc/time/time_zone.h"

using namespace Service::PSC::Time;

namespace {
u32 ToPosix(TimeZone& time_zone, std::array<s64, 2>& out, const CalendarTime& calendar) {
    u32 count{};
    out = {};
    REQUIRE(time_zone.ToPosixTimeWithMyRule(count, out, out.size(), calendar) == ResultSuccess);
    return count;
}
} // namespace

TEST_CASE("ToPosixTime returns both times in the repeated hour after a DST fall-back",
          "[core][time]") {
    const auto file =
        FileSys::SystemArchive::TimeZoneBinary()->GetFileRelative("zoneinfo/America/Los_Angeles");
    REQUIRE(file != nullptr);
    const auto binary = file->ReadAllBytes();

    TimeZone time_zone;
    REQUIRE(time_zone.ParseBinary(LocationName{}, binary) == ResultSuccess);
    time_zone.SetInitialized();

    std::array<s64, 2> out{};
    REQUIRE(ToPosix(time_zone, out, {2026, 11, 1, 1, 30, 0}) == 2);
    CHECK(out[0] == 1793521800);
    CHECK(out[1] == 1793525400);

    REQUIRE(ToPosix(time_zone, out, {2026, 11, 1, 0, 0, 0}) == 1);
    CHECK(out[0] == 1793516400);
    REQUIRE(ToPosix(time_zone, out, {2026, 11, 1, 2, 0, 0}) == 1);
    CHECK(out[0] == 1793527200);
    REQUIRE(ToPosix(time_zone, out, {2026, 7, 1, 12, 0, 0}) == 1);
    CHECK(out[0] == 1782932400);

    CHECK(ToPosix(time_zone, out, {2026, 3, 8, 2, 30, 0}) == 0);
}

TEST_CASE("ToPosixTime pairs no time before the first transition with transition 0",
          "[core][time]") {
    // UTC+0 by default, then UTC+3600 from 1000, UTC+0 from 2000 and UTC+1800 from 5000.
    auto rule = std::make_unique<Tz::Rule>();
    rule->timecnt = 3;
    rule->typecnt = 3;
    rule->charcnt = 12;
    rule->ats[0] = 1000;
    rule->ats[1] = 2000;
    rule->ats[2] = 5000;
    rule->types[0] = 1;
    rule->types[1] = 0;
    rule->types[2] = 2;
    rule->ttis[0] = {.tt_utoff = 0, .tt_desigidx = 0};
    rule->ttis[1] = {.tt_utoff = 3600, .tt_desigidx = 4};
    rule->ttis[2] = {.tt_utoff = 1800, .tt_desigidx = 8};
    std::memcpy(rule->chars.data(), "UTC\0AAA\0BBB", 12);
    rule->defaulttype = 0;

    TimeZone time_zone;
    std::array<s64, 2> out{};
    u32 count{};
    // 00:34:07 local is 2047 under UTC+0; under UTC+3600 it would be -1553, which is before 1000.
    REQUIRE(time_zone.ToPosixTime(count, out, out.size(), {1970, 1, 1, 0, 34, 7}, *rule) ==
            ResultSuccess);
    REQUIRE(count == 1);
    CHECK(out[0] == 2047);

    // 00:08:20 local is 500 under the default UTC+0; under UTC+3600 it would be -3100.
    REQUIRE(time_zone.ToPosixTime(count, out, out.size(), {1970, 1, 1, 0, 8, 20}, *rule) ==
            ResultSuccess);
    REQUIRE(count == 1);
    CHECK(out[0] == 500);
}
