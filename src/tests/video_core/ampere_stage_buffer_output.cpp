// SPDX-FileCopyrightText: Copyright 2026 Orchid Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>

#include <catch2/catch_test_macros.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell_upstream.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "video_core/renderer_vulkan/task_payload_limit.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwellUpstream;
namespace IR = Shader::IR;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};
constexpr Instruction OutputRead{{0x1e237923, 0x00002400, 0x0000b800, 0x000e2400}};
constexpr Instruction ClearAddress{{0x001e7202, 0x000000ff, 0x00000f00, 0}};
constexpr Instruction StoreFirst{{0x1eff7927, 0x0000241e, 0x0000b800, 0}};
constexpr Instruction StoreRead{{0x1eff7927, 0x00002823, 0x0000b800, 0}};
constexpr std::array<u32, 5> SportsHeader{0x000c0481, 0, 0, 0, 0xff000000};

class StageEnvironment final : public Shader::Environment {
public:
    StageEnvironment(Shader::Stage stage_, std::span<const u64> code_, u32 max_vertices)
        : code{code_} {
        stage = stage_;
        is_proprietary_driver = true;
        std::memcpy(&sph, SportsHeader.data(), sizeof(SportsHeader));
        if (stage == Shader::Stage::Mesh) {
            sph.vtg.omap_systemb.raw = 0xF0;
            sph.vtg.omap_generic_vector[0] = 0x1;
            mesh_registers = {4U | (max_vertices << 4) | (16U << 13), 32U << 20, 32U | (1U << 12)};
        } else {
            mesh_registers = {0, 0, 32 | (1U << 12)};
        }
    }
    u64 ReadInstruction(u32 address) override {
        for (u32 index = 0; index < code.size(); ++index) {
            if (address == 0x50 + index * 8 + ((index + 1) / 3) * 8) {
                return code[index];
            }
        }
        FAIL("Read outside the lowered program at " << address);
        return 0;
    }
    u32 ReadCbufValue(u32 bank, u32 offset) override { return (bank << 16) | offset; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 15; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {32, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    bool IsNativeProgram() const override { return true; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> code;
};
}

TEST_CASE("Ampere ISBERD.ATTR.32.O lowers to an output stage buffer read",
          "[video_core][ampere][mesh]") {
    const auto lowered = LowerToMaxwellUpstream(std::array{OutputRead, Exit});
    INFO(lowered.error << " at " << lowered.error_offset);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions[0] == 0xefc0b00002471e23ULL);

    auto skew = OutputRead;
    skew.words[2] |= 1U << 14;
    auto byte = OutputRead;
    byte.words[2] &= ~(1U << 11);
    auto map = OutputRead;
    map.words[2] &= ~(3U << 12);
    for (const Instruction rejected : {skew, byte, map}) {
        REQUIRE_FALSE(LowerToMaxwellUpstream(std::array{rejected, Exit}));
    }
}

TEST_CASE("Task and mesh programs read back the output stage buffer word they wrote",
          "[video_core][ampere][mesh]") {
    const std::array code{ClearAddress, StoreFirst, OutputRead, StoreRead, Exit};
    const auto lowered = LowerToMaxwellUpstream(code);
    INFO(lowered.error << " at " << lowered.error_offset);
    REQUIRE(lowered);

    for (const auto& [stage, max_vertices] : {std::pair{Shader::Stage::Task, 0U},
                                             std::pair{Shader::Stage::Mesh, 32U},
                                             std::pair{Shader::Stage::Mesh, 33U}}) {
        CAPTURE(stage, max_vertices);
        StageEnvironment env{stage, lowered.instructions, max_vertices};
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
        Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
        Shader::ObjectPool<IR::Inst> inst_pool;
        Shader::ObjectPool<IR::Block> block_pool;
        auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg,
                                                         Shader::HostTranslateInfo{});
        REQUIRE(program.stage == stage);
        REQUIRE(program.mesh.max_vertices == max_vertices);
        REQUIRE(program.workgroup_size[0] == 32);
        u32 output_reads{}, input_reads{}, writes{};
        for (const auto* block : program.blocks) {
            for (const auto& inst : *block) {
                switch (inst.GetOpcode()) {
                case IR::Opcode::IsbeReadOutput:
                    ++output_reads;
                    break;
                case IR::Opcode::IsbeRead:
                    ++input_reads;
                    break;
                case IR::Opcode::IsbeWriteAttribute:
                    ++writes;
                    break;
                default:
                    break;
                }
            }
        }
        REQUIRE(output_reads == 1);
        REQUIRE(input_reads == 0);
        REQUIRE(writes == 2);

        Shader::Profile profile{};
        profile.supported_spirv = 0x00010500;
        Shader::RuntimeInfo runtime{};
        Shader::Backend::Bindings bindings{};
        const auto spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
        REQUIRE(spirv.size() > 5);
        REQUIRE(spirv[0] == 0x07230203);
        u32 nan_id = 0;
        u32 limit_id = 0;
        u32 stride_id = 0;
        u32 invocation_id = 0;
        u32 tail_vertex = 0;
        u32 limit_check = 0;
        u32 store_label = 0;
        u32 merge_label = 0;
        u32 current_label = 0;
        u32 nan_stores = 0;
        u32 guarded_stores = 0;
        bool found_barrier = false;
        for (std::size_t i = 5; i < spirv.size();) {
            const u32 wc = spirv[i] >> 16;
            const auto op = static_cast<spv::Op>(spirv[i] & 0xFFFFU);
            REQUIRE(wc != 0);
            REQUIRE(i + wc <= spirv.size());
            if (op == spv::Op::OpConstant && wc == 4) {
                if (spirv[i + 3] == 0x7fc00000U) {
                    nan_id = spirv[i + 2];
                }
                if (spirv[i + 3] == max_vertices) {
                    limit_id = spirv[i + 2];
                }
                if (spirv[i + 3] == 32U) {
                    stride_id = spirv[i + 2];
                }
            } else if (op == spv::Op::OpCompositeExtract) {
                invocation_id = spirv[i + 2];
            } else if (op == spv::Op::OpIAdd && spirv[i + 3] == invocation_id &&
                       spirv[i + 4] == stride_id) {
                tail_vertex = spirv[i + 2];
            } else if (op == spv::Op::OpULessThan && spirv[i + 3] == tail_vertex &&
                       spirv[i + 4] == limit_id) {
                limit_check = spirv[i + 2];
            } else if (op == spv::Op::OpBranchConditional && spirv[i + 1] == limit_check) {
                store_label = spirv[i + 2];
                merge_label = spirv[i + 3];
            } else if (op == spv::Op::OpLabel) {
                current_label = spirv[i + 1];
            } else if (op == spv::Op::OpStore && nan_id != 0 && spirv[i + 2] == nan_id) {
                ++nan_stores;
                guarded_stores += store_label != 0 && current_label == store_label;
            } else if (op == spv::Op::OpControlBarrier) {
                found_barrier = true;
                if (max_vertices == 33) {
                    CHECK(current_label == merge_label);
                }
                break;
            }
            i += wc;
        }
        CHECK(found_barrier);
        CHECK(nan_stores == (max_vertices == 33 ? 8U : max_vertices == 32 ? 4U : 0U));
        CHECK(guarded_stores == (max_vertices == 33 ? 4U : 0U));
    }
}

TEST_CASE("Task payload declarations respect the device byte limit", "[shader][task_payload]") {
    constexpr u32 limit = 16 * 1024;
    CHECK_FALSE(Vulkan::TaskPayloadExceedsLimit(32, limit));
    CHECK_FALSE(Vulkan::TaskPayloadExceedsLimit(4096, limit));
    CHECK(Vulkan::TaskPayloadExceedsLimit(4097, limit));
    CHECK(Vulkan::TaskPayloadExceedsLimit(6976, limit));
    CHECK(Vulkan::TaskPayloadExceedsLimit(4096, limit - 1));
}
