// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include <vulkan/vulkan.h>

#include "shader_recompiler/frontend/ir/program.h"

namespace Vulkan {

struct MeshShaderLimitViolation {
    const char* name{};
    u64 declared{};
    u32 limit{};
};

MeshShaderLimitViolation CheckMeshShaderLimits(
    const Shader::IR::Program& program, const VkPhysicalDeviceMeshShaderPropertiesEXT& limits);

template <typename Reject, typename Build>
auto BuildWithMeshShaderLimits(std::span<const Shader::IR::Program> programs,
                               const VkPhysicalDeviceMeshShaderPropertiesEXT& limits,
                               Reject&& reject, Build&& build) -> decltype(build()) {
    for (size_t index = 0; index < programs.size(); ++index) {
        const auto violation = CheckMeshShaderLimits(programs[index], limits);
        if (violation.name) {
            reject(index, violation);
            return {};
        }
    }
    return build();
}

}
