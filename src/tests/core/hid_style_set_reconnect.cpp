// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_types.h"

using Core::HID::ControllerTriggerType;
using Core::HID::EmulatedController;
using Core::HID::NpadIdType;
using Core::HID::NpadStyleIndex;
using Core::HID::NpadStyleSet;
using Core::HID::NpadStyleTag;

TEST_CASE("Setting the same style set again does not reconnect a fallback controller", "[hid]") {
    // Star Fox (issue 55) supports 0x407 and sets it again and again. A GameCube pad is not in
    // that set and runs as a Pro Controller.
    const NpadStyleTag StarFox{static_cast<NpadStyleSet>(0x407)};
    const NpadStyleTag WithGameCube{static_cast<NpadStyleSet>(0x3f)};

    EmulatedController controller{NpadIdType::Player1};
    // The configured ("original") type is set through the configuration mode.
    controller.EnableConfiguration();
    controller.SetNpadStyleIndex(NpadStyleIndex::GameCube);
    controller.DisableConfiguration();
    controller.SetSupportedNpadStyleTag(WithGameCube);
    controller.Connect();
    REQUIRE(controller.IsConnected());
    REQUIRE(controller.GetNpadStyleIndex() == NpadStyleIndex::GameCube);

    int connects = 0;
    int disconnects = 0;
    controller.SetCallback({
        .on_change =
            [&](ControllerTriggerType type) {
                connects += type == ControllerTriggerType::Connected ? 1 : 0;
                disconnects += type == ControllerTriggerType::Disconnected ? 1 : 0;
            },
        .is_npad_service = true,
    });

    // First call: one reconnect, as the fallback type.
    controller.SetSupportedNpadStyleTag(StarFox);
    REQUIRE(controller.IsConnected());
    REQUIRE(controller.GetNpadStyleIndex() == NpadStyleIndex::Fullkey);
    REQUIRE(connects == 1);
    REQUIRE(disconnects == 1);

    // Repeated calls leave the controller alone.
    controller.SetSupportedNpadStyleTag(StarFox);
    controller.SetSupportedNpadStyleTag(StarFox);
    REQUIRE(controller.IsConnected());
    REQUIRE(controller.GetNpadStyleIndex() == NpadStyleIndex::Fullkey);
    REQUIRE(connects == 1);
    REQUIRE(disconnects == 1);

    // A set that supports the configured type brings it back.
    controller.SetSupportedNpadStyleTag(WithGameCube);
    REQUIRE(controller.IsConnected());
    REQUIRE(controller.GetNpadStyleIndex() == NpadStyleIndex::GameCube);
    REQUIRE(connects == 2);
    REQUIRE(disconnects == 2);
}
