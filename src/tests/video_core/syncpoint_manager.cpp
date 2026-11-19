// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/host1x/syncpoint_manager.h"

using Tegra::Host1x::SyncpointManager;

TEST_CASE("Syncpoint ids past the table are dropped, valid ones still run their actions",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    constexpr u32 Last = SyncpointManager::NUM_MAX_SYNCPOINTS - 1;
    bool fired = false;
    manager->RegisterGuestAction(Last, 1, [&fired] { fired = true; });

    // 24-bit ids as the puller's fence action can carry them.
    manager->IncrementGuest(SyncpointManager::NUM_MAX_SYNCPOINTS);
    manager->IncrementHost(0xFFFFFF);
    REQUIRE_FALSE(fired);

    manager->IncrementGuest(Last);
    REQUIRE(fired);
    REQUIRE(manager->GetGuestSyncpointValue(Last) == 1);
}

TEST_CASE("Syncpoint actions run outside the guard, in value order",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    std::vector<u32> order;
    bool nested = false;
    // An action that registers another one takes the guard itself (the composite action can block
    // on the display while other threads need it); under the guard this deadlocked.
    manager->RegisterGuestAction(0, 2, [&] {
        order.push_back(2);
        manager->RegisterGuestAction(1, 1, [&nested] { nested = true; });
    });
    manager->RegisterGuestAction(0, 1, [&order] { order.push_back(1); });
    manager->RegisterGuestAction(0, 3, [&order] { order.push_back(3); });
    manager->IncrementGuest(0);
    manager->IncrementGuest(0);
    REQUIRE(order == std::vector<u32>{1, 2});
    manager->IncrementGuest(1);
    REQUIRE(nested);
    manager->IncrementGuest(0);
    REQUIRE(order == std::vector<u32>{1, 2, 3});
}

TEST_CASE("A syncpoint action can cancel another due action from inside",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    bool second = false;
    SyncpointManager::ActionHandle handle{};
    manager->RegisterGuestAction(0, 1, [&] { manager->DeregisterGuestAction(0, handle); });
    handle = manager->RegisterGuestAction(0, 1, [&second] { second = true; });
    manager->IncrementGuest(0); // must not wait for itself
    REQUIRE(second); // already taken out when the first one ran
}

TEST_CASE("A cancel from another thread returns only after a due action ran",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    std::atomic<bool> release{false};
    std::atomic<bool> entered{false};
    std::atomic<bool> second{false};
    manager->RegisterGuestAction(0, 1, [&] {
        entered = true;
        while (!release) {
            std::this_thread::yield();
        }
    });
    const auto handle = manager->RegisterGuestAction(0, 1, [&second] { second = true; });
    std::thread incrementer{[&] { manager->IncrementGuest(0); }};
    while (!entered) {
        std::this_thread::yield();
    }
    // The guard is free while the first action blocks (registering does not wait).
    manager->RegisterGuestAction(1, 1, [] {});
    std::thread releaser{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        release = true;
    }};
    manager->DeregisterGuestAction(0, handle);
    REQUIRE(second);
    releaser.join();
    incrementer.join();
}

TEST_CASE("Cancelling an action that already ran leaves later actions registered",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    const auto ran = manager->RegisterGuestAction(0, 1, [] {});
    manager->IncrementGuest(0);
    bool later = false;
    manager->RegisterGuestAction(0, 2, [&later] { later = true; });
    manager->DeregisterGuestAction(0, ran); // its list node may be reused by the later action
    manager->IncrementGuest(0);
    REQUIRE(later);
}

TEST_CASE("The guest mirror gets every value, and moved syncpoints are reported once",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    manager->IncrementGuest(70);
    REQUIRE(manager->TakeMovedGuest(1) == u64{1} << 6); // also without a mirror
    REQUIRE(manager->TakeMovedGuest(1) == 0);

    std::vector<u32> page(SyncpointManager::NUM_MAX_SYNCPOINTS, 0xFFFF);
    manager->SetGuestMirror([&page](u32 id, u32 value) { page[id] = value; });
    // Setting the mirror fills the page: nothing else writes it.
    REQUIRE(page[70] == 1);
    REQUIRE(page[0] == 0);
    REQUIRE(page[SyncpointManager::NUM_MAX_SYNCPOINTS - 1] == 0);

    // The page already holds the new value when an action runs and when the bit is taken, so a
    // poll cannot tell from the page that an armed wait has to be woken.
    u32 seen_by_action = 0;
    manager->RegisterGuestAction(70, 2, [&] { seen_by_action = page[70]; });
    manager->IncrementGuest(70);
    manager->IncrementGuest(191);
    manager->IncrementHost(3);
    REQUIRE(seen_by_action == 2);
    REQUIRE(page[70] == 2);
    REQUIRE(manager->TakeMovedGuest(0) == 0); // host increments do not count
    REQUIRE(manager->TakeMovedGuest(1) == u64{1} << 6);
    REQUIRE(manager->TakeMovedGuest(2) == u64{1} << 63);
    REQUIRE(manager->TakeMovedGuest(2) == 0);

    manager->SetGuestMirror({});
    manager->IncrementGuest(70);
    REQUIRE(page[70] == 2);
}

TEST_CASE("Actions and waits on a syncpoint past the table count as reached",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    constexpr u32 Past = SyncpointManager::NUM_MAX_SYNCPOINTS;
    bool guest = false;
    bool host = false;
    // A fence id from the guest's parcel: the composite waits for this action, so it must run.
    REQUIRE(manager->RegisterGuestAction(Past, 1, [&guest] { guest = true; }) == 0);
    REQUIRE(guest);
    REQUIRE(manager->RegisterHostAction(Past + 1, 1, [&host] { host = true; }) == 0);
    REQUIRE(host);
    manager->DeregisterGuestAction(Past, 1);
    manager->DeregisterHostAction(Past + 1, 1);
    manager->WaitGuest(Past, 1); // returns instead of waiting forever
    manager->WaitHost(Past + 1, 1);
    REQUIRE(manager->IsReadyGuest(Past, 1));
    REQUIRE(manager->IsReadyHost(Past + 1, 1));

    // Valid ids keep waiting for their value.
    bool valid = false;
    const auto guest_handle =
        manager->RegisterGuestAction(Past - 1, 1, [&valid] { valid = true; });
    REQUIRE(guest_handle != 0);
    REQUIRE_FALSE(valid);
    REQUIRE_FALSE(manager->IsReadyGuest(Past - 1, 1));
    const auto host_handle = manager->RegisterHostAction(Past - 1, 1, [&valid] { valid = true; });
    REQUIRE(host_handle != 0);
    REQUIRE_FALSE(manager->IsReadyHost(Past - 1, 1));
    manager->DeregisterGuestAction(Past - 1, guest_handle);
    manager->DeregisterHostAction(Past - 1, host_handle);
    manager->IncrementGuest(Past - 1);
    manager->IncrementHost(Past - 1);
    REQUIRE_FALSE(valid);
    REQUIRE(manager->IsReadyGuest(Past - 1, 1));
    REQUIRE(manager->IsReadyHost(Past - 1, 1));
}
