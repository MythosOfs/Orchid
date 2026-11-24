// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace {
namespace IR = Shader::IR;

std::vector<u32> SignedNanWidths(const std::vector<u32>& spirv) {
    std::vector<u32> widths;
    for (std::size_t i = 5; i < spirv.size();) {
        const u32 word_count = spirv[i] >> 16;
        const u32 opcode = spirv[i] & 0xFFFFU;
        if (word_count == 0 || i + word_count > spirv.size()) {
            break;
        }
        if (opcode == static_cast<u32>(spv::Op::OpExecutionMode) && word_count >= 4 &&
            spirv[i + 2] == static_cast<u32>(spv::ExecutionMode::SignedZeroInfNanPreserve)) {
            widths.push_back(spirv[i + 3]);
        }
        i += word_count;
    }
    std::ranges::sort(widths);
    return widths;
}

std::vector<u32> EmitWithSignedNan(bool uses_fp16, bool uses_fp64, bool broken_fp16,
                                   bool support_fp16 = true) {
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = Shader::Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    program.info.uses_fp16 = uses_fp16;
    program.info.uses_fp64 = uses_fp64;

    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.support_float_controls = true;
    profile.support_fp16_signed_zero_nan_preserve = support_fp16;
    profile.support_fp32_signed_zero_nan_preserve = true;
    profile.support_fp64_signed_zero_nan_preserve = true;
    profile.has_broken_fp16_float_controls = broken_fp16;

    return SignedNanWidths(Shader::Backend::SPIRV::EmitSPIRV(profile, program));
}

}

TEST_CASE("Broken fp16 float controls omit only fp16 SignedZeroInfNanPreserve",
          "[video_core][spirv]") {
    CHECK(EmitWithSignedNan(true, false, true) == std::vector<u32>{32U});
    CHECK(EmitWithSignedNan(true, true, true) == std::vector<u32>{32U, 64U});
    CHECK(EmitWithSignedNan(true, false, false) == std::vector<u32>{16U, 32U});
    CHECK(EmitWithSignedNan(true, false, false, false) == std::vector<u32>{32U});
    CHECK(EmitWithSignedNan(false, false, false) == std::vector<u32>{32U});
}
