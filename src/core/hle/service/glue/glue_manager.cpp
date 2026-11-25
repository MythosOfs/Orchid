// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/glue/errors.h"
#include "core/hle/service/glue/glue_manager.h"

namespace Service::Glue {

struct ARPManager::MapEntry {
    ApplicationLaunchProperty launch;
    std::vector<u8> control;
    u64 owner_pid;
    bool has_control;
};

ARPManager::ARPManager() = default;

ARPManager::~ARPManager() = default;

Result ARPManager::GetLaunchProperty(ApplicationLaunchProperty* out_launch_property,
                                     u64 title_id) const {
    if (title_id == 0) {
        return Glue::ResultInvalidProcessId;
    }

    std::scoped_lock lock{entries_mutex};
    const auto iter = entries.find(title_id);
    if (iter == entries.end()) {
        return Glue::ResultProcessIdNotRegistered;
    }

    *out_launch_property = iter->second.launch;
    return ResultSuccess;
}

Result ARPManager::GetControlProperty(std::vector<u8>* out_control_property, u64 title_id) const {
    if (title_id == 0) {
        return Glue::ResultInvalidProcessId;
    }

    std::scoped_lock lock{entries_mutex};
    const auto iter = entries.find(title_id);
    if (iter == entries.end()) {
        return Glue::ResultProcessIdNotRegistered;
    }

    *out_control_property = iter->second.control;
    return ResultSuccess;
}

Result ARPManager::Register(u64 title_id, ApplicationLaunchProperty launch,
                            std::vector<u8> control, u64 owner_pid, bool has_control) {
    if (title_id == 0 && owner_pid == 0) {
        return Glue::ResultInvalidProcessId;
    }

    std::scoped_lock lock{entries_mutex};
    const auto iter = entries.find(title_id);
    if (iter != entries.end()) {
        return Glue::ResultAlreadyBound;
    }

    entries.insert_or_assign(title_id, MapEntry{launch, std::move(control), owner_pid, has_control});
    return ResultSuccess;
}

Result ARPManager::Unregister(u64 title_id) {
    if (title_id == 0) {
        return Glue::ResultInvalidProcessId;
    }

    std::scoped_lock lock{entries_mutex};
    const auto iter = entries.find(title_id);
    if (iter == entries.end()) {
        return Glue::ResultProcessIdNotRegistered;
    }

    entries.erase(iter);
    return ResultSuccess;
}

Result ARPManager::UnregisterProcess(u64 title_id, u64 owner_pid) {
    std::scoped_lock lock{entries_mutex};
    const auto it = entries.find(title_id);
    if (it != entries.end() && it->second.owner_pid == owner_pid) {
        entries.erase(it);
        return ResultSuccess;
    }
    return Glue::ResultProcessIdNotRegistered;
}

Result ARPManager::GetProcessLaunchProperty(ApplicationLaunchProperty* launch, u64 title_id,
                                           u64 owner_pid) const {
    std::scoped_lock lock{entries_mutex};
    const auto it = entries.find(title_id);
    if (it == entries.end() || it->second.owner_pid != owner_pid) {
        return Glue::ResultProcessIdNotRegistered;
    }
    *launch = it->second.launch;
    return ResultSuccess;
}

Result ARPManager::GetProcessControlProperty(std::vector<u8>* control, u64 title_id,
                                            u64 owner_pid, bool require_actual_control) const {
    std::scoped_lock lock{entries_mutex};
    const auto it = entries.find(title_id);
    if (it == entries.end() || it->second.owner_pid != owner_pid ||
        (require_actual_control && !it->second.has_control)) {
        return Glue::ResultProcessIdNotRegistered;
    }
    *control = it->second.control;
    return ResultSuccess;
}

void ARPManager::ResetAll() {
    std::scoped_lock lock{entries_mutex};
    entries.clear();
}

} // namespace Service::Glue
