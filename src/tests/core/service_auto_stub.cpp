// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/ipc.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/nx2_stub_interface.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"

namespace {

// A service with no commands, so every request is unknown to it.
class EmptyService final : public Service::ServiceFramework<EmptyService> {
public:
    EmptyService(Core::System& system_, const char* name) : ServiceFramework{system_, name} {}
};

class StubReplyService final : public Service::ServiceFramework<StubReplyService> {
public:
    explicit StubReplyService(Core::System& system_) : ServiceFramework{system_, "test:stub-reply"} {
        RegisterHandlers({
            {11, &StubReplyService::StubReplyInterface, "GetInterface"},
            {12, &StubReplyService::GetDirectInterface, "GetDirectInterface"},
        });
        RegisterHandlersTipc({{11, &StubReplyService::StubReplyZeroed, "GetData"}});
    }

private:
    void GetDirectInterface(Service::HLERequestContext& ctx) {
        Service::ReturnNx2StubInterface(system, ctx, GetServiceName());
    }
};

size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

} // Anonymous namespace

// Hidden: reads the log, so it runs in a process of its own (add_test in CMakeLists.txt) whose
// log backend no other test made.
TEST_CASE("Unknown commands: auto stub replies every time and logs once", "[.][service]") {
    // Read once, at the first unknown command: it would force the auto stub on.
#ifdef _WIN32
    _putenv_s("EDEN_NX2_AUTO_STUB", "");
#else
    unsetenv("EDEN_NX2_AUTO_STUB");
#endif
    const auto previous_auto_stub = Settings::values.use_auto_stub.GetValue();
    const auto previous_censor = Settings::values.censor_username.GetValue();
    const auto previous_debug_asserts = Settings::values.use_debug_asserts.GetValue();
    const auto previous_log_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
    ScopedSystemTestContext environment;
    SCOPE_EXIT {
        Settings::values.use_auto_stub.SetValue(previous_auto_stub);
        Settings::values.use_debug_asserts.SetValue(previous_debug_asserts);
        Settings::values.censor_username.SetValue(previous_censor);
        Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, previous_log_dir);
    };
    // One directory per process, removed at exit: the backend keeps its file open until then.
    // shortcut: Windows cannot delete the open log, so its run directories stay in %TEMP%.
    static const auto log_dir = [] {
        namespace fs = std::filesystem;
        const auto logs = fs::temp_directory_path() / "orchid-auto-stub-logs";
        fs::create_directories(logs);
        auto count = std::chrono::system_clock::now().time_since_epoch().count();
        fs::path dir = logs / std::to_string(count);
        while (!fs::create_directory(dir)) {
            dir = logs / std::to_string(++count);
        }
        return dir;
    }();
    static const bool cleanup_registered = std::atexit([] {
        std::error_code ignored;
        std::filesystem::remove_all(log_dir, ignored);
    }) == 0;
    REQUIRE(cleanup_registered);
    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, log_dir);
    // The log would replace a user name like "test" inside the searched lines.
    Settings::values.censor_username.SetValue(false);
    Common::Log::Initialize();
    Common::Log::Start();

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
    server_thread.emplace(kernel.RunOnHostCoreProcess("auto-stub-test", [&] { server.LoopProcess(); }));
    auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);

    // A CMIF request for command 1234 without buffers or handles; the reply lands in the same words.
    const auto invoke = [&](Service::ServiceFrameworkBase& service) {
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(IPC::CommandType::Request);
        header.data_size.Assign(8);
        std::memcpy(words.data(), &header, sizeof(header));
        words[4] = Common::MakeMagic('S', 'F', 'C', 'I');
        words[6] = 1234;
        Service::HLERequestContext request{kernel, process.GetMemory(), nullptr, &client};
        request.SetSessionRequestManager(manager);
        REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
        service.InvokeRequest(request);
        std::memcpy(words.data(), request.CommandBuffer(), sizeof(words));
        return words;
    };
    // Everything logged so far: waits for a marker queued after it.
    const auto logged = [] {
        const std::string marker = "auto stub test marker " +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        LOG_CRITICAL(Debug, "{}", marker);
        std::string text;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (text.find(marker) == std::string::npos &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            Common::Log::Flush();
            std::ifstream file{Common::Log::GetLogFilePath(), std::ios::binary};
            text.assign(std::istreambuf_iterator<char>{file}, {});
        }
        REQUIRE(text.find(marker) != std::string::npos);
        return text;
    };

    SECTION("auto stub, debug asserts on") {
        // The assert broke before the reply was sent, at the first such command of every Switch 2
        // program; on the old code this stops the test binary there.
        Settings::values.use_auto_stub.SetValue(true);
        Settings::values.use_debug_asserts.SetValue(true);
        EmptyService service{system, "test:autostub"};
        const auto first = invoke(service);
        const auto repeat = invoke(service);
        // "SFCO", version, a success result, and the size of a reply with 32 zeroed words.
        REQUIRE(first[4] == Common::MakeMagic('S', 'F', 'C', 'O'));
        CHECK(first[6] == ResultSuccess.raw);
        IPC::CommandHeader reply_header{};
        std::memcpy(&reply_header, first.data(), sizeof(reply_header));
        CHECK(reply_header.data_size == 2 * (2 + 32) + 6);
        CHECK(repeat == first);
        const auto text = logged();
        CHECK(Count(text, "Unknown / unimplemented function '1234(<unknown>)': port='test:autostub'") ==
              1);
    }

    SECTION("registered stub warnings are once per transport and replies stay intact") {
        Settings::values.use_auto_stub.SetValue(true);
        auto service = std::make_shared<StubReplyService>(system);
        manager->SetSessionHandler(Service::SessionRequestHandlerPtr{service});

        for (int i = 0; i < 3; ++i) {
            std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
            words[0] = 11 + 16;
            Service::HLERequestContext request{kernel, process.GetMemory(), nullptr, &client};
            request.SetSessionRequestManager(manager);
            REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
            service->InvokeRequestTipc(request);
            CHECK((request.CommandBuffer()[1] & 0x3ff) == 33);
            CHECK(request.CommandBuffer()[2] == ResultSuccess.raw);
        }

        manager->ConvertToDomain();
        process.GetMemory().SetCurrentPageTable(process);
        for (u32 object_id = 2; object_id <= 5; ++object_id) {
            const bool direct = object_id == 5;
            std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
            IPC::CommandHeader header{};
            header.type.Assign(IPC::CommandType::Request);
            header.data_size.Assign(12);
            std::memcpy(words.data(), &header, sizeof(header));
            IPC::DomainMessageHeader domain_header{};
            domain_header.command.Assign(IPC::DomainMessageHeader::CommandType::SendMessage);
            domain_header.size.Assign(16);
            domain_header.object_id = 1;
            std::memcpy(words.data() + 4, &domain_header, sizeof(domain_header));
            words[8] = Common::MakeMagic('S', 'F', 'C', 'I');
            words[10] = direct ? 12 : 11;

            Service::HLERequestContext request{kernel, process.GetMemory(), nullptr, &client};
            request.SetSessionRequestManager(manager);
            REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
            service->InvokeRequest(request);
            REQUIRE(request.WriteToOutgoingCommandBuffer().IsSuccess());
            const auto* reply = request.CommandBuffer();
            CHECK((reply[1] & 0x3ff) == 15);
            CHECK(reply[4] == 1);
            CHECK(reply[8] == Common::MakeMagic('S', 'F', 'C', 'O'));
            CHECK(reply[10] == ResultSuccess.raw);
            CHECK(reply[12] == object_id);
            REQUIRE(manager->DomainHandlerCount() == object_id);
            const auto child = manager->DomainHandler(object_id - 1).lock();
            REQUIRE(child);
            CHECK(std::string_view{child->diag_service_name} ==
                  (direct ? "nx2:test:stub-reply::12" : "nx2:test:stub-reply::11"));
        }

        const auto text = logged();
        CHECK(Count(text, "(STUBBED) CMIF test:stub-reply::11 (GetInterface)") == 1);
        CHECK(Count(text, "(STUBBED) TIPC test:stub-reply::11 (GetData)") == 1);
        CHECK(Count(text, "Switch 2 test:stub-reply::11 -> stub interface") == 0);
        CHECK(Count(text, "Switch 2 test:stub-reply::12 -> stub interface") == 1);
    }

    SECTION("no auto stub, debug asserts off") {
        Settings::values.use_auto_stub.SetValue(false);
        Settings::values.use_debug_asserts.SetValue(false);
        EmptyService service{system, "test:noautostub"};
        const auto reply = invoke(service);
        // No reply: the request words stay.
        CHECK(reply[4] == Common::MakeMagic('S', 'F', 'C', 'I'));
        CHECK(Count(logged(), "assert Unknown / unimplemented function '1234(<unknown>)': "
                              "port='test:noautostub'") == 1);
    }
}
