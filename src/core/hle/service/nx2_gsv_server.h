// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>
#include <memory>

#include "core/hle/service/server_manager.h"

namespace Core {
class System;
}

namespace Service {

inline std::atomic<ServerManager*> g_nx2_gsv_server{};

inline void RunNx2GsvServer(Core::System& system) {
    if (const char* value = std::getenv("EDEN_NX2_GSV_HOST_THREAD");
        value != nullptr && value[0] == '0') {
        return;
    }
    auto server_manager = std::make_unique<ServerManager>(system);
    g_nx2_gsv_server.store(server_manager.get(), std::memory_order_release);
    ServerManager::RunServer(std::move(server_manager));
}

}
