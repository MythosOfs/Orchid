// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"

namespace {
namespace IR = Shader::IR;

constexpr u64 ExitInsn = 0xe30000000007000fULL;

constexpr u64 MakeLocalInsn(u64 base_opcode, u32 reg, u32 addr_reg, u32 offset) {
    return base_opcode | (u64{reg} & 0xFFULL) | ((u64{addr_reg} & 0xFFULL) << 8) |
           ((u64{offset} & 0xFFFFFFULL) << 20) | (u64{4} << 48);
}

constexpr u64 MakeStl32(u32 reg, u32 addr_reg, u32 offset) {
    return MakeLocalInsn(0xef50000000000000ULL, reg, addr_reg, offset);
}

constexpr u64 MakeLdl32(u32 reg, u32 addr_reg, u32 offset) {
    return MakeLocalInsn(0xef40000000000000ULL, reg, addr_reg, offset);
}

class LocalEnv final : public Shader::Environment {
public:
    LocalEnv(std::span<const u64> code_, u32 local_size_, bool native_)
        : code{code_}, local_size{local_size_}, native{native_} {
        stage = Shader::Stage::Compute;
    }
    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < code.size(); ++i) {
            if (address == i * 8 + ((i + 1) / 3) * 8) {
                return code[i];
            }
        }
        return ExitInsn;
    }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return local_size; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    bool IsNativeProgram() const override { return native; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> code;
    u32 local_size;
    bool native;
};

IR::Program Translate(LocalEnv& env) {
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0}};
    Shader::ObjectPool<IR::Inst> inst_pool;
    Shader::ObjectPool<IR::Block> block_pool;
    return Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg,
                                             Shader::HostTranslateInfo{});
}

}

TEST_CASE("Native programs reserve a local stack region and map top-of-window accesses",
          "[video_core][native_local_stack]") {
    constexpr u32 declared = 0x10;
    constexpr u32 stack_top_word = Shader::Environment::LOCAL_WINDOW_BYTES - 4U;
    constexpr u32 oob_offset = 0x1000;
    const std::array code{
        MakeStl32(1, 255, stack_top_word),
        MakeStl32(2, 255, oob_offset),
        MakeLdl32(3, 255, stack_top_word),
        ExitInsn,
    };

    LocalEnv native_env{code, declared, true};
    CHECK(native_env.LocalArraySize() == declared + Shader::Environment::NATIVE_STACK_BYTES + 4U);
    const auto native_prog = Translate(native_env);
    CHECK(native_prog.local_memory_size ==
          declared + Shader::Environment::NATIVE_STACK_BYTES + 4U);

    u32 write_stack_word{};
    u32 write_spare_word{};
    const u32 expected_stack_word = (declared + Shader::Environment::NATIVE_STACK_BYTES - 4U) / 4U;
    const u32 expected_spare_word = (declared + Shader::Environment::NATIVE_STACK_BYTES) / 4U;
    for (const auto* block : native_prog.blocks) {
        for (const auto& inst : *block) {
            if (inst.GetOpcode() == IR::Opcode::WriteLocal && inst.Arg(0).IsImmediate()) {
                const u32 w = inst.Arg(0).U32();
                if (w == expected_stack_word) {
                    ++write_stack_word;
                } else if (w == expected_spare_word) {
                    ++write_spare_word;
                }
            }
        }
    }
    CHECK(write_stack_word == 1);
    CHECK(write_spare_word == 1);

    LocalEnv legacy_env{code, declared, false};
    CHECK(legacy_env.LocalArraySize() == declared);
    const auto legacy_prog = Translate(legacy_env);
    CHECK(legacy_prog.local_memory_size == declared);
}
