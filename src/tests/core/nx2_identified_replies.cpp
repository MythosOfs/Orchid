// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/ipc.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nx2_gsv.h"
#include "core/hle/service/server_manager.h"
#include "tests/core/system_test_context.h"

namespace {

struct TipcReply {
    u32 data_words;
    u32 result;
};

}

TEST_CASE("nx2 TSG ioctls the SDK issues succeed, others stay unsupported", "[nx2_replies]") {
    Core::System system;
    Service::Nvidia::Devices::nx2_stub_device tsg{system, "/dev/nvhost-tsg-gpu"};
    std::array<u8, 0x18> input{};
    std::array<u8, 0x18> output{};
    const auto ioctl = [&](u32 raw, std::size_t size) {
        Service::Nvidia::Ioctl command{};
        command.raw = raw;
        return tsg.Ioctl1(1, command, std::span<const u8>{input.data(), size},
                          std::span<u8>{output.data(), size});
    };
    CHECK(ioctl(0xc0045401, 4) == Service::Nvidia::NvResult::Success);
    CHECK(ioctl(0xc0045402, 4) == Service::Nvidia::NvResult::Success);
    CHECK(ioctl(0xc0045408, 4) == Service::Nvidia::NvResult::Success);
    CHECK(ioctl(0xc018540b, 0x18) == Service::Nvidia::NvResult::Success);
    CHECK(ioctl(0xc0045403, 4) == Service::Nvidia::NvResult::NotImplemented);
    CHECK(ioctl(0xc0085408, 8) == Service::Nvidia::NvResult::NotImplemented);
}

TEST_CASE("nx2 commands identified as result-only reply with the result word alone",
          "[nx2_replies]") {
    ScopedSystemTestContext environment;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    Kernel::KThread bootstrap{kernel};
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &bootstrap, nullptr).IsSuccess());
    kernel.SetCurrentEmuThread(&bootstrap);
    kernel.Initialize();
    SCOPE_EXIT {
        system.CoreTiming().ClearPendingEvents();
        kernel.Shutdown();
        kernel.SetCurrentEmuThread(nullptr);
        kernel.UnregisterKernelObject(&bootstrap);
    };
    Kernel::KProcess process{kernel};
    Kernel::KAutoObject::Create(&process);
    SCOPE_EXIT {
        kernel.UnregisterKernelObject(&process);
    };
    Kernel::KThread client{kernel};
    Kernel::KAutoObject::Create(&client);
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &client, &process).IsSuccess());
    SCOPE_EXIT {
        kernel.UnregisterKernelObject(&client);
    };
    std::optional<std::jthread> server_thread;
    Service::ServerManager server{system};
    server_thread.emplace(kernel.RunOnHostCoreProcess("nx2-replies-test", [&] { server.LoopProcess(); }));
    auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);

    const auto invoke = [&](Service::ServiceFrameworkBase& service, u32 command,
                            std::vector<u32> data) {
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
        words[0] = command + 16;
        words[1] = static_cast<u32>(data.size());
        std::copy(data.begin(), data.end(), words.begin() + 2);
        Service::HLERequestContext request{kernel, process.GetMemory(), nullptr, &client};
        request.SetSessionRequestManager(manager);
        REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
        service.InvokeRequestTipc(request);
        const u32* reply = request.CommandBuffer();
        return TipcReply{reply[1] & 0x3ff, reply[2]};
    };
    const auto check_result_only = [&](Service::ServiceFrameworkBase& service, u32 command,
                                       std::vector<u32> data) {
        INFO("command " << command);
        const auto reply = invoke(service, command, std::move(data));
        CHECK(reply.data_words == 1);
        CHECK(reply.result == 0);
    };

    Service::INx2SyncptSession syncpt{system};
    check_result_only(syncpt, 21, {32});

    Service::INx2Vi2Session vi2{system, std::make_shared<Service::Nx2NativeObjects>()};
    check_result_only(vi2, 51, {7});
    check_result_only(vi2, 111, {0x1000, 0, 0x2000, 0, 0x3000, 0});
    check_result_only(vi2, 112, {0x1000, 0, 0x2000, 0});
    check_result_only(vi2, 251, {0x3000, 0, 0x1000, 0});
    check_result_only(vi2, 252, {0x3000, 0});
    check_result_only(vi2, 301, {0x1000, 0, 1, 0, 1, 0});
    check_result_only(vi2, 351, {0x3000, 0, 1, 0});

    Service::INx2GraphicsSystemSession gsv{system};
    check_result_only(gsv, 603, {});
    check_result_only(gsv, 1003, {});
}
