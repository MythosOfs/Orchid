// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <bitset>

#include "shader_recompiler/backend/spirv/mesh_declarations.h"
#include "video_core/renderer_vulkan/mesh_shader_limits.h"

namespace Vulkan {
namespace {
using Shader::Backend::SPIRV::MeshOutputAttribute;
using Shader::Backend::SPIRV::MeshWorkgroupLayout;
using Shader::Backend::SPIRV::RoundMeshCount;
using Shader::IR::Attribute;

struct MeshOutputs {
    u64 components{};
    u64 location_components{};
};

MeshOutputs CountMeshOutputs(std::span<const u32> lines, bool per_primitive) {
    std::bitset<static_cast<u32>(Attribute::Generic31W) + 1> declared;
    MeshOutputs outputs;
    for (const u32 line : lines) {
        const auto attribute = MeshOutputAttribute(static_cast<Attribute>(line), per_primitive);
        if (!attribute || declared.test(static_cast<u32>(*attribute))) {
            continue;
        }
        declared.set(static_cast<u32>(*attribute));
        if (Shader::IR::IsGeneric(*attribute)) {
            outputs.components += 4;
            const u64 location = Shader::IR::GenericAttributeIndex(*attribute);
            outputs.location_components =
                (std::max)(outputs.location_components, (location + 1) * 4);
        } else {
            outputs.components += *attribute == Attribute::PositionX ? 4 : 1;
        }
    }
    return outputs;
}
}

MeshShaderLimitViolation CheckMeshShaderLimits(
    const Shader::IR::Program& program, const VkPhysicalDeviceMeshShaderPropertiesEXT& limits) {
    const auto& mesh = program.mesh;
    const u64 payload = u64{mesh.payload_words} * sizeof(u32);
    if (program.stage == Shader::Stage::Task) {
        constexpr u64 shared = 0;
        if (payload > limits.maxTaskPayloadSize) {
            return {"maxTaskPayloadSize", payload, limits.maxTaskPayloadSize};
        }
        if (shared > limits.maxTaskSharedMemorySize) {
            return {"maxTaskSharedMemorySize", shared, limits.maxTaskSharedMemorySize};
        }
        if (payload + shared > limits.maxTaskPayloadAndSharedMemorySize) {
            return {"maxTaskPayloadAndSharedMemorySize", payload + shared,
                    limits.maxTaskPayloadAndSharedMemorySize};
        }
        return {};
    }
    if (program.stage != Shader::Stage::Mesh) {
        return {};
    }

    const u64 shared = MeshWorkgroupLayout{mesh}.SizeBytes();
    if (shared > limits.maxMeshSharedMemorySize) {
        return {"maxMeshSharedMemorySize", shared, limits.maxMeshSharedMemorySize};
    }
    if (payload + shared > limits.maxMeshPayloadAndSharedMemorySize) {
        return {"maxMeshPayloadAndSharedMemorySize", payload + shared,
                limits.maxMeshPayloadAndSharedMemorySize};
    }
    if (mesh.max_vertices > limits.maxMeshOutputVertices) {
        return {"maxMeshOutputVertices", mesh.max_vertices, limits.maxMeshOutputVertices};
    }
    if (mesh.max_primitives > limits.maxMeshOutputPrimitives) {
        return {"maxMeshOutputPrimitives", mesh.max_primitives, limits.maxMeshOutputPrimitives};
    }
    const auto vertex = CountMeshOutputs(mesh.vertex_lines, false);
    const auto primitive = CountMeshOutputs(mesh.primitive_lines, true);
    const u64 components = (std::max)({vertex.components + primitive.components,
                                       vertex.location_components, primitive.location_components});
    if (components > limits.maxMeshOutputComponents) {
        return {"maxMeshOutputComponents", components, limits.maxMeshOutputComponents};
    }
    const u64 output_bytes =
        sizeof(u32) *
        (vertex.components *
             RoundMeshCount(mesh.max_vertices, limits.meshOutputPerVertexGranularity) +
         primitive.components *
             RoundMeshCount(mesh.max_primitives, limits.meshOutputPerPrimitiveGranularity));
    if (output_bytes > limits.maxMeshOutputMemorySize) {
        return {"maxMeshOutputMemorySize", output_bytes, limits.maxMeshOutputMemorySize};
    }
    if (payload + output_bytes > limits.maxMeshPayloadAndOutputMemorySize) {
        return {"maxMeshPayloadAndOutputMemorySize", payload + output_bytes,
                limits.maxMeshPayloadAndOutputMemorySize};
    }
    return {};
}

}
