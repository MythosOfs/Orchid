// SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "common/common_funcs.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/device_memory.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_file.h"
#include "core/hle/service/server_manager.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"

namespace {

class ShortOutput final : public Service::ServiceFramework<ShortOutput> {
public:
    ShortOutput(Core::System& system, u32 bytes_to_write)
        : ServiceFramework{system, "short-output"}, bytes_to_write{bytes_to_write} {
        static const FunctionInfo functions[] = {
            {0, D<&ShortOutput::Read>, "Read"},
        };
        RegisterHandlers(functions);
    }

private:
    Result Read(Service::OutBuffer<Service::BufferAttr_HipcAutoSelect> output) {
        REQUIRE(output.size() >= bytes_to_write);
        std::fill_n(output.data(), bytes_to_write, u8{0x11});
        return ResultSuccess;
    }

    u32 bytes_to_write;
};

}

TEST_CASE("CMIF out buffers keep the guest bytes a short read leaves alone", "[cmif][ipc]") {
    const u32 buffer_size = GENERATE(5U, 64U);
    const u32 file_size = std::min(40U, buffer_size - 2);
    const bool map_alias = GENERATE(true, false);
    CAPTURE(map_alias, buffer_size);

    ScopedSystemTestContext test_context;
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
        process.UnregisterThread(kernel, &client);
        process.Close(kernel);
        kernel.UnregisterKernelObject(&client);
    };
    std::optional<std::jthread> server_thread;
    Service::ServerManager server{system};
    server_thread.emplace(
        kernel.RunOnHostCoreProcess("cmif-out-buffer-test", [&] { server.LoopProcess(); }));
    auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);

    constexpr u64 Base = 0x4000;
    constexpr u64 BufferOffset = 0x100;
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    auto* page = system.DeviceMemory().buffer.BackingBasePointer() + 0x10000;
    table.entries.GetAndFault(Base >> 12)
        .Store(false, Common::PageType::Memory, 1, reinterpret_cast<uintptr_t>(page) - Base);
    auto& memory = process.GetMemory();
    memory.SetCurrentPageTable(process);

    Service::FileSystem::IFile file{system, std::make_shared<FileSys::VectorVfsFile>(
                                                std::vector<u8>(file_size, 0x11), "save.bin")};
    std::fill_n(page + BufferOffset, buffer_size, u8{0xa5});

    std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
    IPC::CommandHeader header{};
    header.type.Assign(IPC::CommandType::Request);
    if (map_alias) {
        header.num_buf_b_descriptors.Assign(1);
        header.data_size.Assign(3 + 4 + 6);
        IPC::BufferDescriptorABW buffer{};
        buffer.size_bits_0_31 = buffer_size;
        buffer.address_bits_0_31 = static_cast<u32>(Base + BufferOffset);
        std::memcpy(words.data() + 2, &buffer, sizeof(buffer));
        words[8] = Common::MakeMagic('S', 'F', 'C', 'I');
        words[16] = buffer_size;
    } else {
        header.buf_c_descriptor_flags.Assign(
            IPC::CommandHeader::BufferDescriptorCFlag::OneDescriptor);
        header.data_size.Assign(6);
        IPC::BufferDescriptorC buffer{};
        buffer.address_bits_0_31 = Base + BufferOffset;
        buffer.size.Assign(buffer_size);
        std::memcpy(words.data() + 8, &buffer, sizeof(buffer));
        words[4] = Common::MakeMagic('S', 'F', 'C', 'I');
    }
    std::memcpy(words.data(), &header, sizeof(header));

    Service::HLERequestContext request{kernel, memory, nullptr, &client};
    request.SetSessionRequestManager(manager);
    REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
    if (map_alias) {
        file.InvokeRequest(request);
        CHECK(request.CommandBuffer()[8] == file_size);
    } else {
        ShortOutput output{system, file_size};
        output.InvokeRequest(request);
    }
    REQUIRE(request.CommandBuffer()[4] == Common::MakeMagic('S', 'F', 'C', 'O'));
    REQUIRE(request.CommandBuffer()[6] == ResultSuccess.raw);

    const u8* out = page + BufferOffset;
    CHECK(std::all_of(out, out + file_size, [](u8 value) { return value == 0x11; }));
    CHECK(std::all_of(out + file_size, out + buffer_size, [](u8 value) { return value == 0xa5; }));
}
