// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
#include "core/hle/service/nvnflinger/hos_binder_driver.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/vi/application_root_service.h"
#include "core/hle/service/vi/container.h"
#include "core/hle/service/vi/manager_root_service.h"
#include "core/hle/service/vi/system_root_service.h"
#include "core/hle/service/vi/vi.h"

namespace Service::VI {

void LoopProcess(Core::System& system, std::stop_token token) {
    auto binder_driver =
        system.ServiceManager().WaitForService<Nvnflinger::IHOSBinderDriver>("dispdrv", token);
    if (!binder_driver) {
        return;
    }
    const auto nvdrv = system.ServiceManager().WaitForService<Nvidia::NVDRV>("nvdrv:s", token);
    if (!nvdrv) {
        return;
    }
    const auto container =
        std::make_shared<Container>(system, std::move(binder_driver), nvdrv->GetModule());

    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("vi:m",
                                         std::make_shared<IManagerRootService>(system, container));
    server_manager->RegisterNamedService("vi:s",
                                         std::make_shared<ISystemRootService>(system, container));
    server_manager->RegisterNamedService(
        "vi:u", std::make_shared<IApplicationRootService>(system, container));

    std::stop_callback cb(token, [=] { container->OnTerminate(); });

    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::VI
