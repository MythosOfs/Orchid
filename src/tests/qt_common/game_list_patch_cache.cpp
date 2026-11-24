// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "qt_common/game_list/patch_cache.h"

TEST_CASE("GameListPatchCache caches per scan and drops stale entries on Clear",
          "[qt_common][game_list]") {
    for (const quint64 title_id : {0x04004D300C5AE001ULL, 0x040015100B514001ULL}) {
        GameListPatchCache cache;
        QString installed;
        unsigned calls{};
        const auto generate = [&] {
            ++calls;
            return installed;
        };
        const auto read = [&] { return cache.Get(title_id, true, generate); };

        CHECK(read().isEmpty());
        installed = QStringLiteral("DLC (1)");
        CHECK(read() == installed);
        CHECK(calls == 2);
        CHECK(read() == installed);
        CHECK(calls == 2);

        installed = QStringLiteral("DLC (1, 2)");
        cache.Clear();
        CHECK(read() == installed);
        installed = QStringLiteral("DLC (2)");
        cache.Clear();
        CHECK(read() == installed);
        installed.clear();
        cache.Clear();
        CHECK(read().isEmpty());
        CHECK(read().isEmpty());
        CHECK(calls == 6);

        installed = QStringLiteral("Update (NAND) (1.0.0)");
        CHECK(read() == installed);
        installed = QStringLiteral("Update (NAND) (1.1.0)\nUpdate (SDMC) (1.2.0)");
        cache.Clear();
        CHECK(read() == installed);

        installed = QStringLiteral("[D] DLC (1)");
        CHECK(cache.Get(title_id, false, generate) == installed);
        CHECK(cache.Get(0, true, generate) == installed);
        installed.clear();
        CHECK(cache.Get(0, true, generate).isEmpty());

        GameListPatchCache next_scan;
        CHECK(next_scan.Get(title_id, true, generate).isEmpty());
    }
}
