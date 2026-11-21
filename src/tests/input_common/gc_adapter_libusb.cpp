// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <libusb.h>

#include <catch2/catch_test_macros.hpp>

#include "input_common/drivers/gc_adapter.h"

namespace {
int injected_init_error = LIBUSB_SUCCESS;
int exit_calls = 0;
libusb_context* exited_context = nullptr;
int fake_context_storage;
libusb_context* const fake_context = reinterpret_cast<libusb_context*>(&fake_context_storage);
} // namespace

// The tests target links with --wrap for these, so GCAdapter's libusb calls land here.
extern "C" {
int __wrap_libusb_init(libusb_context** ctx);
void __wrap_libusb_exit(libusb_context* ctx);
libusb_device_handle* __wrap_libusb_open_device_with_vid_pid(libusb_context*, uint16_t, uint16_t);
}

extern "C" int __wrap_libusb_init(libusb_context** ctx) {
    if (injected_init_error == LIBUSB_SUCCESS) {
        *ctx = fake_context;
    }
    return injected_init_error;
}

extern "C" void __wrap_libusb_exit(libusb_context* ctx) {
    ++exit_calls;
    exited_context = ctx;
}

extern "C" libusb_device_handle* __wrap_libusb_open_device_with_vid_pid(libusb_context*, uint16_t,
                                                                         uint16_t) {
    return nullptr;
}

TEST_CASE("GCAdapter exits libusb only after a successful init", "[input_common][gc_adapter]") {
    exit_calls = 0;
    exited_context = nullptr;
    SECTION("failed init") {
        injected_init_error = LIBUSB_ERROR_OTHER;
        { InputCommon::GCAdapter adapter{"gcpad"}; }
        CHECK(exit_calls == 0);
    }
    SECTION("healthy init") {
        injected_init_error = LIBUSB_SUCCESS;
        { InputCommon::GCAdapter adapter{"gcpad"}; }
        CHECK(exit_calls == 1);
        CHECK(exited_context == fake_context);
    }
    injected_init_error = LIBUSB_SUCCESS;
}
