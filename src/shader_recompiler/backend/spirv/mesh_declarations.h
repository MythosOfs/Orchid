// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <optional>

#include "shader_recompiler/frontend/ir/attribute.h"
#include "shader_recompiler/shader_info.h"

namespace Shader::Backend::SPIRV {

constexpr u64 RoundMeshCount(u32 count, u32 granularity) {
    return ((u64{count} + granularity - 1) / granularity) * granularity;
}

struct MeshWorkgroupLayout {
    explicit MeshWorkgroupLayout(const MeshInfo& mesh)
        : primitive_base{RoundMeshCount(mesh.max_vertices, 32) * mesh.vertex_lines.size()},
          attribute_words{std::max<u64>(primitive_base + RoundMeshCount(mesh.max_primitives, 32) *
                                                             mesh.primitive_lines.size(),
                                        1)},
          map_words{4 + 3 * u64{mesh.max_primitives}} {}

    u64 primitive_base;
    u64 attribute_words;
    u64 map_words;

    u64 SizeBytes() const {
        return (attribute_words + map_words + 1) * sizeof(u32);
    }
};

inline std::optional<IR::Attribute> MeshOutputAttribute(IR::Attribute line, bool per_primitive) {
    if (IR::IsGeneric(line)) {
        return static_cast<IR::Attribute>(static_cast<u32>(IR::Attribute::Generic0X) +
                                          4 * IR::GenericAttributeIndex(line));
    }
    if (per_primitive) {
        switch (line) {
        case IR::Attribute::PrimitiveId:
        case IR::Attribute::Layer:
        case IR::Attribute::ViewportIndex:
            return line;
        default:
            return std::nullopt;
        }
    }
    if (line >= IR::Attribute::PositionX && line <= IR::Attribute::PositionW) {
        return IR::Attribute::PositionX;
    }
    if (line == IR::Attribute::PointSize) {
        return line;
    }
    return std::nullopt;
}

}
