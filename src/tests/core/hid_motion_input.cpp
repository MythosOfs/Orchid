// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "hid_core/frontend/motion_input.h"

TEST_CASE("Stationary motion feedback does not grow with sample count", "[hid][motion]") {
    Core::HID::MotionInput motion;
    motion.SetAcceleration({0.6f, 0.0f, -0.8f});
    motion.UpdateOrientation(0);
    const auto correction = motion.GetGyroscope();
    REQUIRE(correction.Length() > 0.0f);

    for (int sample = 0; sample < 500; ++sample) {
        motion.UpdateOrientation(0);
    }
    CHECK((motion.GetGyroscope() - correction).Length() < 0.001f);
}
