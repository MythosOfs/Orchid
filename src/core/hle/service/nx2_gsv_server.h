// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <memory>

#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/server_manager.h"

namespace Service {

// The server for the Switch 2 graphics system sessions: nx2:gsv (ISelfController 2000) and every
// session handed out from it (vi2, gpu, syncpt, nvsrv, video decoder, ...), on a host thread of
// its own. They used to be registered with AM's server, a guest service thread on emulated
// core 3: vi2's present (229) waits there for the previous frame's GPU fence and then for the
// next vblank, and for that time core 3 ran no game thread and no other graphics request (GPU
// submissions through nvsrv included) was answered. One thread, so these handlers stay
// serialized among themselves as before. EDEN_NX2_GSV_HOST_THREAD=0 keeps them on AM's server.
/// Runs the Switch 2 graphics system server on the calling host thread until emulation stops.
inline void RunNx2GsvServer(Core::System& system) {
    if (const char* value = std::getenv("EDEN_NX2_GSV_HOST_THREAD");
        value != nullptr && value[0] == '0') {
        return;
    }
    system.Kernel().RunServer(std::make_unique<ServerManager>(system), true);
}

} // namespace Service
