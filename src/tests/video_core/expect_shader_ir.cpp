// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Expect tests: the IR of captured native code after lowering and the optimization passes is
// compared with a reference text in src/tests/expect/. Any change to lowering, translation or the
// passes that reaches this code shows up as a reviewable diff. Semantics stay covered by the
// machine tests (e.g. ampere_kirby_fsel.cpp); these only make changes visible.

#include <array>
#include <initializer_list>
#include <limits>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/object_pool.h"
#include "tests/expect.h"

namespace {
using Shader::Ampere::Instruction;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

// Serves a lowered program at the addresses LowerToMaxwell places it (a scheduling word before
// every third instruction), like the renderer's environment for native programs. A fragment
// stage writing R0-R31 to its eight render targets keeps every register result alive; without
// an output, dead code elimination would leave an empty program.
class LoweredEnvironment final : public Shader::Environment {
public:
    explicit LoweredEnvironment(std::span<const u64> code_) : code{code_} {
        stage = Shader::Stage::Fragment;
        sph.ps.omap.target = ~0U;
    }
    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < code.size(); ++i) {
            if (address == 0x50 + i * 8 + ((i + 1) / 3) * 8)
                return code[i];
        }
        FAIL("Read outside the lowered program at " << address);
        return 0;
    }
    bool HasNativeInstructions() const override {
        return true;
    }
    bool IsNativeProgram() const override {
        return true;
    }
    u32 ReadCbufValue(u32, u32) override {
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 0;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {1, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> code;
};

std::string LoweredIr(std::span<const Instruction> native) {
    const auto lowered = Shader::Ampere::LowerToMaxwell(native);
    INFO(lowered.error << " at " << lowered.error_offset);
    REQUIRE(lowered);
    LoweredEnvironment env{lowered.instructions};
    Shader::ObjectPool<Shader::IR::Inst> inst_pool;
    Shader::ObjectPool<Shader::IR::Block> block_pool;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, 0x50};
    const Shader::HostTranslateInfo host_info{};
    const auto program =
        Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host_info);
    return Tests::StripIrAddresses(Shader::IR::DumpProgram(program));
}
struct BitfieldProgram {
    Shader::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block block{pool};
    Shader::IR::Program program;
    LoweredEnvironment env{{}};

    BitfieldProgram() {
        using Node = Shader::IR::AbstractSyntaxNode;
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, Node::Type::Block}};
    }

    Shader::IR::Value Emit(Shader::IR::Opcode opcode,
                           std::initializer_list<Shader::IR::Value> args) {
        block.AppendNewInst(opcode, args);
        return Shader::IR::Value{&block.back()};
    }

    void Fold() {
        Shader::Optimization::ConstantPropagationPass(env, program);
    }
};

} // Anonymous namespace

TEST_CASE("Kirby captured uniform FSEL pair lowers to the reference IR",
          "[video_core][ampere][kirby][expect]") {
    // The two FSEL forms logged from Kirby and the Forgotten Land (also in ampere_kirby_fsel.cpp).
    constexpr std::array<Instruction, 3> code{{
        {{0x0c0d2c08, 0x00000007, 0x0d800000, 0x000fca00}},
        {{0x0e154c08, 0x00000007, 0x0e800000, 0x000fe200}},
        Exit,
    }};
    Tests::CheckExpect("kirby_fsel_pair.ir", LoweredIr(code));
}

TEST_CASE("Unsigned bitfield folding handles every valid field within a word",
          "[video_core][shader][bitfield_fold]") {
    using Shader::IR::Opcode;
    using Shader::IR::Value;
    struct Field {
        u32 offset;
        u32 width;
        Value extract;
    };

    BitfieldProgram shader;
    constexpr u32 input = 0x91e7a53c;
    std::vector<Field> fields;
    for (u32 offset = 0; offset <= 32; ++offset) {
        for (u32 width = 0; width <= 32 - offset; ++width) {
            fields.push_back({offset, width, shader.Emit(
                Opcode::BitFieldUExtract, {Value{input}, Value{offset}, Value{width}})});
        }
    }
    shader.Fold();

    for (const auto& [offset, width, extract] : fields) {
        CAPTURE(offset, width);
        const u64 mask = (u64{1} << width) - 1;
        REQUIRE(extract.IsImmediate());
        CHECK(extract.U32() == ((u64{input} >> offset) & mask));
    }
}

TEST_CASE("Unsigned bitfield folding rejects fields outside a word",
          "[video_core][shader][bitfield_fold]") {
    using Shader::IR::Opcode;
    using Shader::IR::Value;
    constexpr u32 max = std::numeric_limits<u32>::max();
    for (const auto& [offset, width] :
         std::array{std::pair{32U, 1U}, std::pair{33U, 0U}, std::pair{33U, max},
                    std::pair{max, max}}) {
        CAPTURE(offset, width);
        BitfieldProgram shader;
        shader.Emit(Opcode::BitFieldUExtract, {Value{0U}, Value{offset}, Value{width}});
        CHECK_THROWS_AS(shader.Fold(), Shader::LogicError);
    }
}

TEST_CASE("Unsigned bitfield fold reference keeps all bits for a full-width extract",
          "[video_core][shader][bitfield_fold][expect]") {
    using Shader::IR::Opcode;
    using Shader::IR::Value;
    BitfieldProgram shader;
    constexpr u32 input = 0x91e7a53c;
    const Value empty{shader.Emit(Opcode::BitFieldUExtract,
                                  {Value{input}, Value{32U}, Value{0U}})};
    const Value full{shader.Emit(Opcode::BitFieldUExtract,
                                 {Value{input}, Value{0U}, Value{32U}})};
    shader.Emit(Opcode::WriteGlobal32, {Value{u64{0}}, empty});
    shader.Emit(Opcode::WriteGlobal32, {Value{u64{4}}, full});
    shader.Fold();
    Tests::CheckExpect("unsigned_bitfield_edges.ir",
                       Tests::StripIrAddresses(Shader::IR::DumpProgram(shader.program)));
}
