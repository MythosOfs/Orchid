// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "hid_core/hid_util.h"
#include "hid_core/resources/vibration/vibration_device.h"

using Core::HID::DeviceIndex;
using Core::HID::NpadStyleIndex;
using Core::HID::VibrationDeviceHandle;
using Core::HID::VibrationValue;
using Service::HID::GetActualVibrationValueOrDefault;
using Service::HID::IsVibrationHandleValid;
using Service::HID::NpadVibrationDevice;

namespace {
// VibrationValue::operator== ignores the frequencies at zero amplitude; the SDK does not.
void RequireDefault(const VibrationValue& value) {
    REQUIRE(value.low_amplitude == 0.0f);
    REQUIRE(value.low_frequency == 160.0f);
    REQUIRE(value.high_amplitude == 0.0f);
    REQUIRE(value.high_frequency == 320.0f);
}
} // namespace

TEST_CASE("Actual vibration value of a handle without a device is the default", "[hid]") {
    // Kirby Air Riders (issue 111): the SDK aborts when hid 202 fails, so a handle that resolves
    // to no device reads as "not vibrating".
    RequireDefault(GetActualVibrationValueOrDefault(nullptr));
}

TEST_CASE("Actual vibration value of an inactive or unmounted device is the default", "[hid]") {
    NpadVibrationDevice device;
    // Never activated: the device itself reports ResultVibrationNotInitialized.
    VibrationValue raw{};
    REQUIRE(device.GetActualVibrationValue(raw).IsError());
    RequireDefault(GetActualVibrationValueOrDefault(&device));

    // Activated but no controller mounted.
    REQUIRE(device.Activate().IsSuccess());
    RequireDefault(GetActualVibrationValueOrDefault(&device));
}

TEST_CASE("Vibration handles of styles without a vibration device are invalid", "[hid]") {
    // These are the handles the hid server answers with defaults instead of an error.
    const auto handle = [](NpadStyleIndex type, u8 npad_id, DeviceIndex index) {
        VibrationDeviceHandle result{};
        result.npad_type = type;
        result.npad_id = npad_id;
        result.device_index = index;
        return result;
    };
    REQUIRE(IsVibrationHandleValid(handle(NpadStyleIndex::Fullkey, 0, DeviceIndex::Left))
                .IsSuccess());
    REQUIRE(IsVibrationHandleValid(handle(NpadStyleIndex::None, 0, DeviceIndex::Left)).IsError());
    REQUIRE(IsVibrationHandleValid(handle(static_cast<NpadStyleIndex>(15), 0, DeviceIndex::Left))
                .IsError());
    REQUIRE(IsVibrationHandleValid(handle(NpadStyleIndex::Fullkey, 8, DeviceIndex::Left))
                .IsError());
    REQUIRE(IsVibrationHandleValid(
                handle(NpadStyleIndex::Fullkey, 0, DeviceIndex::MaxDeviceIndex))
                .IsError());
}
