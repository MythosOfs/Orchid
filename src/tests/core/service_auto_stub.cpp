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
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "tests/core/system_test_context.h"

namespace {

// A service with no commands, so every request is unknown to it.
class EmptyService final : public Service::ServiceFramework<EmptyService> {
public:
    EmptyService(Core::System& system_, const char* name) : ServiceFramework{system_, name} {}
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
    // One directory per process: the backend keeps writing there until the process exits.
    static const auto log_dir = std::filesystem::temp_directory_path() /
        ("orchid-auto-stub-log-" +
         std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(log_dir);
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
