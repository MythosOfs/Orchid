// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

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

    // The first transition (local mean time -7:52:58 to PST) set the clock back by 7:02.
    REQUIRE(ToPosix(time_zone, out, {1883, 11, 18, 12, 5, 0}) == 2);
    CHECK(std::min(out[0], out[1]) == -2717640122);
    CHECK(std::max(out[0], out[1]) == -2717639700);
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

TEST_CASE("ToPosixTime returns both times when the first transition turns the clock back",
          "[core][time]") {
    // UTC+3600 by default, UTC+0 from 1000 and UTC+3600 again from 10000.
    auto rule = std::make_unique<Tz::Rule>();
    rule->timecnt = 2;
    rule->typecnt = 2;
    rule->charcnt = 8;
    rule->ats[0] = 1000;
    rule->ats[1] = 10000;
    rule->types[0] = 0;
    rule->types[1] = 1;
    rule->ttis[0] = {.tt_utoff = 0, .tt_desigidx = 0};
    rule->ttis[1] = {.tt_utoff = 3600, .tt_desigidx = 4};
    std::memcpy(rule->chars.data(), "UTC\0AAA", 8);
    rule->defaulttype = 1;

    TimeZone time_zone;
    std::array<s64, 2> out{};
    u32 count{};
    // 00:59:59 local is -1 under the default UTC+3600 and 3599 under UTC+0.
    REQUIRE(time_zone.ToPosixTime(count, out, out.size(), {1970, 1, 1, 0, 59, 59}, *rule) ==
            ResultSuccess);
    REQUIRE(count == 2);
    CHECK(std::min(out[0], out[1]) == -1);
    CHECK(std::max(out[0], out[1]) == 3599);

    // 01:30:00 local is 5400 under UTC+0 only: under UTC+3600 it would be 1800, after 1000.
    REQUIRE(time_zone.ToPosixTime(count, out, out.size(), {1970, 1, 1, 1, 30, 0}, *rule) ==
            ResultSuccess);
    REQUIRE(count == 1);
    CHECK(out[0] == 5400);
}

TEST_CASE("ToPosixTime of a rule without transitions returns one time", "[core][time]") {
    // UTC+3600 by default; type 0 (UTC+0) is never in effect.
    auto rule = std::make_unique<Tz::Rule>();
    rule->typecnt = 2;
    rule->charcnt = 8;
    rule->ttis[0] = {.tt_utoff = 0, .tt_desigidx = 0};
    rule->ttis[1] = {.tt_utoff = 3600, .tt_desigidx = 4};
    std::memcpy(rule->chars.data(), "UTC\0AAA", 8);
    rule->defaulttype = 1;

    TimeZone time_zone;
    std::array<s64, 2> out{};
    u32 count{};
    REQUIRE(time_zone.ToPosixTime(count, out, out.size(), {1970, 1, 1, 1, 0, 0}, *rule) ==
            ResultSuccess);
    REQUIRE(count == 1);
    CHECK(out[0] == 0);
}

TEST_CASE("ToPosixTime pairs the default interval only with the offset of transition 0",
          "[core][time]") {
    // UTC+3600 by default, UTC+0 from 1000, UTC+1800 from 10000 and UTC+0 from 20000.
    auto rule = std::make_unique<Tz::Rule>();
    rule->timecnt = 3;
    rule->typecnt = 3;
    rule->charcnt = 12;
    rule->ats[0] = 1000;
    rule->ats[1] = 10000;
    rule->ats[2] = 20000;
    rule->types[0] = 0;
    rule->types[1] = 2;
    rule->types[2] = 0;
    rule->ttis[0] = {.tt_utoff = 0, .tt_desigidx = 0};
    rule->ttis[1] = {.tt_utoff = 3600, .tt_desigidx = 4};
    rule->ttis[2] = {.tt_utoff = 1800, .tt_desigidx = 8};
    std::memcpy(rule->chars.data(), "UTC\0AAA\0BBB", 12);
    rule->defaulttype = 1;

    TimeZone time_zone;
    std::array<s64, 2> out{};
    u32 count{};
    // 00:59:59 local is -1 under the default UTC+3600 and 3599 under UTC+0, not 1799 under the
    // UTC+1800 of transition 1.
    REQUIRE(time_zone.ToPosixTime(count, out, out.size(), {1970, 1, 1, 0, 59, 59}, *rule) ==
            ResultSuccess);
    REQUIRE(count == 2);
    CHECK(out[0] == -1);
    CHECK(out[1] == 3599);
}

TEST_CASE("ToPosixTime finds no second time before a first transition that sets the clock back "
          "only later",
          "[core][time]") {
    // UTC+0 by default, UTC-3600 from 5000 and UTC+0 again from 9000.
    auto rule = std::make_unique<Tz::Rule>();
    rule->timecnt = 2;
    rule->typecnt = 2;
    rule->charcnt = 8;
    rule->ats[0] = 5000;
    rule->ats[1] = 9000;
    rule->types[0] = 1;
    rule->types[1] = 0;
    rule->ttis[0] = {.tt_utoff = 0, .tt_desigidx = 0};
    rule->ttis[1] = {.tt_utoff = -3600, .tt_desigidx = 4};
    std::memcpy(rule->chars.data(), "UTC\0AAA", 8);
    rule->defaulttype = 0;

    TimeZone time_zone;
    std::array<s64, 2> out{};
    u32 count{};
    // 00:08:20 local is 500 under UTC+0; under UTC-3600 it would be 4100, still before 5000.
    REQUIRE(time_zone.ToPosixTime(count, out, out.size(), {1970, 1, 1, 0, 8, 20}, *rule) ==
            ResultSuccess);
    REQUIRE(count == 1);
    CHECK(out[0] == 500);
}

namespace {
constexpr s64 FourHundredYears = 12622780800;

std::unique_ptr<Tz::Rule> MakeRule(std::initializer_list<s32> offsets,
                                   std::initializer_list<std::pair<s64, u8>> transitions) {
    auto rule = std::make_unique<Tz::Rule>();
    for (const s32 offset : offsets) {
        rule->ttis[rule->typecnt++] = {.tt_utoff = offset, .tt_desigidx = 0};
    }
    for (const auto& [at, type] : transitions) {
        rule->ats[rule->timecnt] = at;
        rule->types[rule->timecnt++] = type;
    }
    rule->charcnt = 4;
    std::memcpy(rule->chars.data(), "UTC", 4);
    return rule;
}

std::vector<s64> Times(const Tz::Rule& rule, const CalendarTime& calendar) {
    TimeZone time_zone;
    std::array<s64, 2> out{};
    u32 count{};
    REQUIRE(time_zone.ToPosixTime(count, out, out.size(), calendar, rule) == ResultSuccess);
    std::vector<s64> times(out.begin(), out.begin() + count);
    std::sort(times.begin(), times.end());
    return times;
}
} // namespace

TEST_CASE("ToPosixTime returns a second time only where it maps back to the local time",
          "[core][time]") {
    constexpr s64 P = FourHundredYears;
    SECTION("extrapolated after the last transition") {
        auto rule =
            MakeRule({0, 3600, 7200}, {{0, 1}, {10000, 0}, {10500, 2}, {20000, 1}, {P + 10000, 0}});
        rule->goahead = true;
        // P + 11000 under UTC+7200; P + 7400 would be 03:03:20 under UTC+3600.
        CHECK(Times(*rule, {2370, 1, 1, 5, 3, 20}) == std::vector<s64>{P + 11000});
    }
    SECTION("extrapolated before the first transition") {
        auto rule = MakeRule({3600, 0}, {{0, 0}, {1000, 1}, {P - 10000, 0}, {P, 0}});
        rule->goback = true;
        CHECK(Times(*rule, {1970, 1, 1, 0, 59, 59}) == std::vector<s64>{-1, 3599});
    }
    SECTION("a transition inside the repeated hour") {
        auto rule = MakeRule({0, 0, 1800, 0, 3600},
                             {{1000, 0}, {2000, 4}, {3000, 2}, {4000, 0}, {20000, 4}});
        CHECK(Times(*rule, {1970, 1, 1, 1, 40, 0}) == std::vector<s64>{2400, 6000});
    }
}

TEST_CASE("ToPosixTime rejects a rule whose abbreviation lies outside its text", "[core][time]") {
    TimeZone time_zone;
    std::array<s64, 2> out{};
    u32 count{};
    auto rule = MakeRule({0}, {});
    REQUIRE(time_zone.ToPosixTime(count, out, out.size(), {1970, 1, 1, 0, 0, 0}, *rule) ==
            ResultSuccess);
    rule->ttis[0].tt_desigidx = -1;
    CHECK(time_zone.ToPosixTime(count, out, out.size(), {1970, 1, 1, 0, 0, 0}, *rule) ==
          ResultTimeZoneOutOfRange);
    rule->ttis[0].tt_desigidx = static_cast<s32>(rule->chars.size());
    CHECK(time_zone.ToPosixTime(count, out, out.size(), {1970, 1, 1, 0, 0, 0}, *rule) ==
          ResultTimeZoneOutOfRange);
    // The last index localsub accepts, first without and then with a NUL after it.
    const size_t last = rule->chars.size() - Tz::CHARS_EXTRA;
    rule->ttis[0].tt_desigidx = static_cast<s32>(last);
    rule->chars.fill('A');
    CHECK(time_zone.ToPosixTime(count, out, out.size(), {1970, 1, 1, 0, 0, 0}, *rule) ==
          ResultTimeZoneOutOfRange);
    rule->chars[last + 1] = '\0';
    REQUIRE(time_zone.ToPosixTime(count, out, out.size(), {1970, 1, 1, 0, 0, 0}, *rule) ==
            ResultSuccess);
    REQUIRE(count == 1);
    CHECK(out[0] == 0);
}

TEST_CASE("ToPosixTime rejects a rule whose default type or transition count is out of range",
          "[core][time]") {
    TimeZone time_zone;
    std::array<s64, 2> out{};
    u32 count{};
    auto rule = MakeRule({0, 3600}, {{1000, 1}});
    rule->defaulttype = 1;
    REQUIRE(time_zone.ToPosixTime(count, out, out.size(), {1970, 1, 1, 0, 0, 0}, *rule) ==
            ResultSuccess);
    for (const s32 type : {-1, 2}) {
        rule->defaulttype = type;
        CHECK(time_zone.ToPosixTime(count, out, out.size(), {1970, 1, 1, 0, 0, 0}, *rule) ==
              ResultTimeZoneOutOfRange);
    }
    rule->defaulttype = 0;
    rule->timecnt = -1;
    CHECK(time_zone.ToPosixTime(count, out, out.size(), {1970, 1, 1, 0, 0, 0}, *rule) ==
          ResultTimeZoneOutOfRange);
}
