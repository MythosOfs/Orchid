// SPDX-License-Identifier: GPL-3.0-or-later

// Screen-linear varyings are passed times w and read back times FragCoord.w (issue 53).

#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;

class TestEnvironment final : public Shader::Environment {
public:
    explicit TestEnvironment(Shader::Stage stage_) { stage = stage_; }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

// Fragment program that outputs generic 0, or vertex program that writes Position and generic 0.
struct Program {
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    Shader::Profile profile{};
    Shader::RuntimeInfo runtime{};

    explicit Program(Shader::Stage stage) {
        program.stage = stage;
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        program.info.interpolation[0] = Shader::Interpolation::NoPerspective;
        profile.supported_spirv = 0x00010300;
        runtime.previous_stage_stores.mask.set();

        IR::IREmitter ir{block};
        ir.Prologue();
        if (stage == Shader::Stage::Fragment) {
            ir.SetFragColor(0, 0, ir.GetAttribute(IR::Attribute::Generic0X));
        } else {
            for (u32 element = 0; element < 4; ++element) {
                ir.SetAttribute(IR::Attribute::PositionX + element, ir.Imm32(2.0f), ir.Imm32(0));
                ir.SetAttribute(IR::Attribute::Generic0X + element, ir.Imm32(3.0f), ir.Imm32(0));
            }
        }
        ir.Epilogue();
        TestEnvironment env{stage};
        Shader::Optimization::CollectShaderInfoPass(env, program);
    }

    std::vector<u32> Emit(bool screen_linear) {
        runtime.screen_linear_generics[0] = screen_linear;
        Shader::Backend::Bindings bindings;
        return Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    }
};

struct Counts {
    u32 no_perspective{};
    u32 frag_coord{};
    u32 fmul{};
    u32 vector_times_scalar{};
};

Counts Count(const std::vector<u32>& code) {
    Counts counts;
    for (size_t offset = 5; offset < code.size();) {
        const u32 count = code[offset] >> 16;
        REQUIRE(count > 0);
        REQUIRE(offset + count <= code.size());
        const auto op = static_cast<spv::Op>(code[offset] & 0xffff);
        if (op == spv::Op::OpDecorate && count >= 3) {
            const auto decoration = static_cast<spv::Decoration>(code[offset + 2]);
            counts.no_perspective += decoration == spv::Decoration::NoPerspective;
            counts.frag_coord += decoration == spv::Decoration::BuiltIn && count == 4 &&
                                 code[offset + 3] == static_cast<u32>(spv::BuiltIn::FragCoord);
        }
        counts.fmul += op == spv::Op::OpFMul;
        counts.vector_times_scalar += op == spv::Op::OpVectorTimesScalar;
        offset += count;
    }
    return counts;
}
} // namespace

TEST_CASE("Fragment stage reads screen-linear generics perspective-correct times FragCoord.w",
          "[video_core][spirv][screen_linear]") {
    Program fragment{Shader::Stage::Fragment};
    const Counts plain = Count(fragment.Emit(false));
    CHECK(plain.no_perspective == 1);
    CHECK(plain.frag_coord == 0);

    const Counts rewritten = Count(fragment.Emit(true));
    CHECK(rewritten.no_perspective == 0);
    CHECK(rewritten.frag_coord == 1);
    CHECK(rewritten.fmul == plain.fmul + 1);
}

TEST_CASE("Last vertex stage writes screen-linear generics times Position.w",
          "[video_core][spirv][screen_linear]") {
    for (const auto stage : {Shader::Stage::VertexB, Shader::Stage::TessellationEval}) {
        CAPTURE(stage);
        Program vertex{stage};
        const Counts plain = Count(vertex.Emit(false));
        const Counts rewritten = Count(vertex.Emit(true));
        CHECK(rewritten.vector_times_scalar == plain.vector_times_scalar + 1);
    }
    // A geometry stage may emit a written output more than once: it is never rescaled.
    Program geometry{Shader::Stage::Geometry};
    geometry.program.output_vertices = 1;
    CHECK(Count(geometry.Emit(true)).vector_times_scalar ==
          Count(geometry.Emit(false)).vector_times_scalar);
}
