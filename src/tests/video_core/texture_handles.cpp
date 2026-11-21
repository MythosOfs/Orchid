// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <spirv/unified1/spirv.hpp11>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "tests/expect.h"

namespace {
namespace IR = Shader::IR;
using IR::Opcode;
using IR::Value;

class HandleEnvironment final : public Shader::Environment {
public:
    bool integer_texture{};
    u32 format_reads{};
    u32 expected_handle{0xabc};
    std::map<std::pair<u32, u32>, u32> cbuf_values{{{0, 0}, 0xabc}};

    u64 ReadInstruction(u32) override { throw std::logic_error("Unexpected instruction read"); }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        const auto it = cbuf_values.find({index, offset});
        REQUIRE(it != cbuf_values.end());
        return it->second;
    }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        REQUIRE(handle == expected_handle);
        ++format_reads;
        return integer_texture;
    }
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

struct HandleProgram {
    Shader::ObjectPool<IR::Inst> pool{256};
    IR::Block block{pool};
    IR::Block left{pool};
    IR::Block right{pool};
    IR::Block merge{pool};
    IR::Block* insertion_block{&block};
    IR::Program program;
    HandleEnvironment env;

    HandleProgram() {
        program.stage = Shader::Stage::Fragment;
        program.blocks = {&block};
        program.post_order_blocks = {&block};
    }

    Value Emit(Opcode opcode, std::initializer_list<Value> args = {}) {
        insertion_block->AppendNewInst(opcode, args);
        return Value{&insertion_block->back()};
    }

    Value Storage(u32 offset) {
        return Emit(Opcode::LoadStorage32, {Value{0U}, Value{offset}});
    }

    Value Add(Value lhs, Value rhs) {
        return Emit(Opcode::IAdd32, {lhs, rhs});
    }

    Value Phi(Value first, Value second) {
        block.AddBranch(&left);
        block.AddBranch(&right);
        left.AddBranch(&merge);
        right.AddBranch(&merge);
        program.blocks = {&block, &left, &right, &merge};
        program.post_order_blocks = {&merge, &right, &left, &block};
        insertion_block = &merge;
        const Value value{Emit(Opcode::Phi)};
        value.Inst()->SetFlags(IR::Type::U32);
        value.Inst()->AddPhiOperand(&left, first);
        value.Inst()->AddPhiOperand(&right, second);
        return value;
    }

    IR::Inst& Sample(Value handle) {
        IR::IREmitter ir{*insertion_block};
        IR::TextureInstInfo flags{};
        flags.type.Assign(Shader::TextureType::Color2D);
        return *ir.ImageSampleExplicitLod(handle,
                                         ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(0.0f)),
                                         ir.Imm32(0.0f), {}, flags).Inst();
    }

    void Run() {
        Shader::HostTranslateInfo host;
        host.ApplyDescriptorLimitPolicy();
        Shader::Optimization::TexturePass(env, program, host);
    }

    const Shader::TextureDescriptor& Descriptor(const IR::Inst& sample) const {
        REQUIRE(sample.GetOpcode() == Opcode::ImageSampleExplicitLod);
        const u32 index{sample.Flags<IR::TextureInstInfo>().descriptor_index};
        REQUIRE(index < program.info.texture_descriptors.size());
        return program.info.texture_descriptors[index];
    }

    u32 Evaluate(const IR::Inst& sample, u32 first = 0x1234, u32 second = 0x50) const {
        const u32 index{Descriptor(sample).handle_program};
        REQUIRE(index < program.info.handle_programs.size());
        const auto result{Shader::EvaluateHandleProgram(
            program.info.handle_programs[index],
            [](u32, u32) -> u32 { throw std::logic_error("Unexpected evaluator cbuf read"); },
            [=](u32 binding, u32 offset) {
                REQUIRE(binding == 0);
                REQUIRE((offset == 0 || offset == 4));
                return offset == 0 ? first : second;
            })};
        REQUIRE(result.has_value());
        return *result;
    }
};
}

TEST_CASE("Sampled descriptors distinguish static and computed handles",
          "[video_core][texture_handles]") {
    HandleProgram shader;
    auto& first{shader.Sample(shader.Storage(0))};
    auto& second{shader.Sample(shader.Storage(4))};
    auto& repeated{shader.Sample(shader.Storage(0))};
    auto& fixed{shader.Sample(shader.Emit(Opcode::GetCbufU32, {Value{0U}, Value{0U}}))};
    shader.Run();

    CHECK(shader.program.info.handle_programs.size() == 2);
    CHECK(shader.program.info.texture_descriptors.size() == 3);
    CHECK(shader.Descriptor(first).handle_program != shader.Descriptor(second).handle_program);
    CHECK(shader.Descriptor(first).handle_program == shader.Descriptor(repeated).handle_program);
    CHECK(shader.Descriptor(fixed).handle_program == ~0U);
    CHECK(first.Flags<IR::TextureInstInfo>().descriptor_index ==
          repeated.Flags<IR::TextureInstInfo>().descriptor_index);
    CHECK(first.Flags<IR::TextureInstInfo>().descriptor_index !=
          fixed.Flags<IR::TextureInstInfo>().descriptor_index);
    CHECK(shader.Evaluate(first) == 0x1234);
    CHECK(shader.Evaluate(second) == 0x50);
    CHECK(shader.Evaluate(repeated, 0x5678) == 0x5678);
}

TEST_CASE("Computed handles do not share the static texture format cache",
          "[video_core][texture_handles]") {
    for (const bool dynamic_first : {false, true}) {
        CAPTURE(dynamic_first);
        HandleProgram shader;
        shader.env.integer_texture = true;
        const Value dynamic{shader.Storage(0)};
        const Value fixed{shader.Emit(Opcode::GetCbufU32, {Value{0U}, Value{0U}})};
        auto& first{shader.Sample(dynamic_first ? dynamic : fixed)};
        auto& second{shader.Sample(dynamic_first ? fixed : dynamic)};
        auto& third{shader.Sample(fixed)};
        shader.Run();

        const auto& static_sample{dynamic_first ? second : first};
        const auto& dynamic_sample{dynamic_first ? first : second};
        CHECK(shader.program.info.texture_descriptors.size() == 2);
        CHECK(static_sample.Flags<IR::TextureInstInfo>().is_integer == 1);
        CHECK(third.Flags<IR::TextureInstInfo>().is_integer == 1);
        CHECK(dynamic_sample.Flags<IR::TextureInstInfo>().is_integer == 0);
        CHECK(shader.Descriptor(static_sample).is_integer);
        CHECK_FALSE(shader.Descriptor(dynamic_sample).is_integer);
        CHECK(shader.env.format_reads == 1);
    }
}

TEST_CASE("A sampled handle keeps a storage-indexed cbuf texture with its sampler",
          "[video_core][texture_handles]") {
    for (const bool sampler_first : {false, true}) {
        HandleProgram shader;
        shader.env.cbuf_values = {{{6, 0}, 0x123}, {{2, 0x570}, 0x24000000}};
        shader.env.expected_handle = 0x24000123;
        const Value offset{shader.Emit(Opcode::ShiftLeftLogical32, {shader.Storage(0), Value{4U}})};
        const Value texture{shader.Emit(Opcode::GetCbufU32, {Value{6U}, offset})};
        const Value sampler{shader.Emit(Opcode::GetCbufU32, {Value{2U}, Value{0x570U}})};
        auto& sample{shader.Sample(shader.Emit(Opcode::BitwiseOr32,
                                              {sampler_first ? sampler : texture,
                                               sampler_first ? texture : sampler}))};
        shader.Run();
        const auto& descriptor = shader.Descriptor(sample);
        CHECK(descriptor.handle_program == ~0U);
        CHECK(descriptor.count > 1);
        CHECK(descriptor.size_shift == 4);
        CHECK(descriptor.cbuf_index == 6);
        CHECK(descriptor.cbuf_offset == 0);
        CHECK(descriptor.has_secondary);
        CHECK(descriptor.secondary_cbuf_index == 2);
        CHECK(descriptor.secondary_cbuf_offset == 0x570);
        CHECK(shader.program.info.handle_programs.empty());
        REQUIRE_FALSE(sample.Arg(0).IsEmpty());
        const auto* selection = sample.Arg(0).InstRecursive();
        REQUIRE(selection->GetOpcode() == Opcode::UMin32);
        const auto* index = selection->Arg(0).InstRecursive();
        REQUIRE(index->GetOpcode() == Opcode::ShiftRightLogical32);
        CHECK(index->Arg(0).Resolve() == offset.Resolve());
        CHECK(index->Arg(1).U32() == 4);
    }
}

TEST_CASE("Combining static cbuf handles keeps the static descriptor path",
          "[video_core][texture_handles]") {
    HandleProgram shader;
    const Value texture{shader.Emit(Opcode::GetCbufU32, {Value{0U}, Value{0U}})};
    const Value sampler{shader.Emit(Opcode::GetCbufU32, {Value{0U}, Value{0U}})};
    auto& sample{shader.Sample(shader.Emit(Opcode::BitwiseOr32, {texture, sampler}))};
    shader.Run();
    CHECK(shader.Descriptor(sample).handle_program == ~0U);
    CHECK(shader.Descriptor(sample).has_secondary);
    CHECK(shader.program.info.handle_programs.empty());
}

TEST_CASE("Texture arrays reserve the final static and buffer resources",
          "[video_core][texture_handles]") {
    for (const u32 arrays : {1U, 2U}) {
        HandleProgram shader;
        const Value offset{shader.Emit(Opcode::ShiftLeftLogical32, {shader.Storage(0), Value{4U}})};
        auto& sample{shader.Sample(shader.Emit(Opcode::GetCbufU32, {Value{0U}, offset}))};
        shader.Run();
        auto& info = shader.program.info;
        const auto descriptor = info.texture_descriptors.front();
        for (u32 i = 1; i < arrays; ++i) {
            info.texture_descriptors.push_back(descriptor);
        }
        for (u32 i = 0; i < 3; ++i) {
            auto fixed = descriptor;
            fixed.count = 1;
            info.texture_descriptors.push_back(fixed);
        }
        for (u32 i = 0; i < 5; ++i) {
            info.constant_buffer_descriptors.push_back({.index = i, .count = 1});
        }
        for (u32 i = 0; i < 2; ++i) {
            info.storage_buffers_descriptors.push_back({.count = 1});
        }
        Shader::HostTranslateInfo host;
        host.ApplyDescriptorLimitPolicy();
        host.max_per_stage_resources = 16;
        host.max_per_stage_descriptor_sampled_images = 16;
        host.max_descriptor_set_sampled_images = 16;
        Shader::Optimization::LimitSampledTextureArrays(shader.program, host);
        for (u32 i = 0; i < arrays; ++i) {
            CHECK(info.texture_descriptors[i].count == 6 / arrays);
        }
        CHECK(Shader::NumDescriptors(info.texture_descriptors) == 9);
        CHECK(sample.Arg(0).InstRecursive()->Arg(1).U32() == 6 / arrays - 1);
        if (arrays == 2) {
            info.texture_descriptors[0].count = 2;
            info.texture_descriptors[1].count = 12;
            host.max_per_stage_resources = 24;
            host.max_per_stage_descriptor_sampled_images = 24;
            host.max_descriptor_set_sampled_images = 24;
            Shader::Optimization::LimitSampledTextureArrays(shader.program, host);
            CHECK(info.texture_descriptors[0].count == 2);
            CHECK(info.texture_descriptors[1].count == 12);
        }
        host.max_per_stage_resources = 10;
        REQUIRE_THROWS(Shader::Optimization::LimitSampledTextureArrays(shader.program, host));
    }
}

TEST_CASE("Failed Phi handle subtrees discard their cached values",
          "[video_core][texture_handles]") {
    for (const bool nested : {false, true}) {
        for (const bool retained_slot : {false, true}) {
            CAPTURE(nested, retained_slot);
            HandleProgram shader;
            const Value shared{shader.Storage(0)};
            const Value retained{shader.Storage(4)};
            const Value unsupported{shader.Emit(Opcode::GetRegister, {Value{IR::Reg::R0}})};
            const Value subtree{nested ? shader.Add(shared, retained) : shared};
            const Value failed{shader.Add(subtree, unsupported)};
            Value fallback{shader.Add(subtree, Value{7U})};
            if (retained_slot) {
                fallback = shader.Add(fallback, retained);
            }
            Value handle{shader.Phi(failed, fallback)};
            if (retained_slot) {
                handle = shader.Add(retained, handle);
            }
            auto& sample{shader.Sample(handle)};
            shader.Run();

            REQUIRE(shader.program.info.handle_programs.size() == 1);
            CHECK(shader.Evaluate(sample) ==
                  0x1234U + 7U + (nested ? 0x50 : 0) + (retained_slot ? 2 * 0x50 : 0));
            CHECK(shader.Evaluate(sample, 0x5678, 0x90) ==
                  0x5678U + 7U + (nested ? 0x90 : 0) + (retained_slot ? 2 * 0x90 : 0));
        }
    }
}

TEST_CASE("Each word of a 64-bit constant buffer read binds its own texture",
          "[video_core][texture_handles][cbuf_word]") {
    const u32 word = GENERATE(0U, 1U);
    HandleProgram shader;
    const u32 offset = 0x298 + word * 4;
    shader.env.cbuf_values = {{{0, 0x298}, 0x20020b}, {{0, 0x29c}, 0x20034a}};
    shader.env.expected_handle = shader.env.cbuf_values.at({0, offset});
    const Value pair{shader.Emit(Opcode::GetCbufU32x2, {Value{0U}, Value{0x298U}})};
    const Value handle{shader.Emit(Opcode::CompositeExtractU32x2, {pair, Value{word}})};
    auto& sample{shader.Sample(handle)};
    shader.Run();

    REQUIRE(shader.program.info.texture_descriptors.size() == 1);
    CHECK(shader.Descriptor(sample).cbuf_index == 0);
    CHECK(shader.Descriptor(sample).cbuf_offset == offset);
    CHECK(shader.Descriptor(sample).handle_program == ~0U);
}

TEST_CASE("The high word of a dynamically indexed cbuf read keeps its descriptor range",
          "[video_core][texture_handles][cbuf_word]") {
    const u32 word = GENERATE(0U, 1U);
    constexpr u32 base = 16 * Shader::HostTranslateInfo::DEFAULT_DESCRIPTOR_LIMIT - 16;
    HandleProgram shader;
    const u32 offset = base + word * 4;
    shader.env.cbuf_values = {{{0, base}, 0x20020b}, {{0, base + 4}, 0x20034a}};
    shader.env.expected_handle = shader.env.cbuf_values.at({0, offset});
    const Value stride{shader.Emit(Opcode::ShiftLeftLogical32, {shader.Storage(0), Value{3U}})};
    const Value pair{shader.Emit(Opcode::GetCbufU32x2, {Value{0U}, shader.Add(Value{base}, stride)})};
    const Value handle{shader.Emit(Opcode::CompositeExtractU32x2, {pair, Value{word}})};
    auto& sample{shader.Sample(handle)};
    shader.Run();

    const auto& descriptor = shader.Descriptor(sample);
    CHECK(descriptor.cbuf_offset == offset);
    CHECK(descriptor.count == 2U - word);
    CHECK(descriptor.handle_program == ~0U);
}

TEST_CASE("Phi operands resolve immediate identities before dead code elimination",
          "[video_core][shader][phi_identity][expect]") {
    const bool replace_operand = GENERATE(false, true);
    HandleProgram shader;
    using Node = IR::AbstractSyntaxNode;
    IR::IREmitter entry{shader.block};
    const IR::U32 lane{entry.LocalInvocationIdX()};
    const IR::U1 cond{entry.ConditionRef(entry.IEqual(lane, entry.Imm32(0)))};
    const Value identity{shader.Emit(Opcode::Identity, {Value{7U}})};
    shader.insertion_block = &shader.left;
    const Value other{shader.Emit(Opcode::IAdd32, {lane, Value{1U}})};

    shader.block.AddBranch(&shader.left);
    shader.block.AddBranch(&shader.merge);
    shader.left.AddBranch(&shader.merge);
    shader.insertion_block = &shader.merge;
    const Value phi{shader.Emit(Opcode::Phi)};
    phi.Inst()->SetFlags(IR::Type::U32);
    phi.Inst()->AddPhiOperand(&shader.block, replace_operand ? Value{3U} : identity);
    phi.Inst()->AddPhiOperand(&shader.left, other);
    if (replace_operand) {
        phi.Inst()->SetArg(0, identity);
    }
    IR::IREmitter merge{shader.merge};
    merge.WriteShared(32, merge.Imm32(0), IR::U32{phi});

    auto& program = shader.program;
    program.stage = Shader::Stage::Compute;
    program.workgroup_size = {32, 1, 1};
    program.shared_memory_size = 4;
    program.blocks = {&shader.block, &shader.left, &shader.merge};
    program.post_order_blocks = {&shader.merge, &shader.left, &shader.block};
    program.syntax_list = {
        {{.block = &shader.block}, Node::Type::Block},
        {{.if_node{cond, &shader.left, &shader.merge}}, Node::Type::If},
        {{.block = &shader.left}, Node::Type::Block},
        {{.end_if{&shader.merge}}, Node::Type::EndIf},
        {{.block = &shader.merge}, Node::Type::Block},
        {{}, Node::Type::Return},
    };
    Shader::Optimization::DeadCodeEliminationPass(program);

    const auto first = phi.Inst()->Arg(0);
    REQUIRE(first.IsImmediate());
    REQUIRE_FALSE(first.IsIdentity());
    CHECK(first.U32() == 7);
    CHECK(phi.Inst()->Arg(1).Inst() == other.Inst());
    CHECK(other.Inst()->UseCount() == 1);

    Shader::Optimization::CollectShaderInfoPass(shader.env, program);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010500;
    const auto spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
    u32 seven_id{};
    std::array<u32, 2> phi_values{};
    for (size_t i = 5; i < spirv.size();) {
        const size_t size = spirv[i] >> 16;
        REQUIRE(size != 0);
        switch (static_cast<spv::Op>(spirv[i] & 0xffffU)) {
        case spv::Op::OpConstant:
            if (size == 4 && spirv[i + 3] == 7) {
                seven_id = spirv[i + 2];
            }
            break;
        case spv::Op::OpPhi:
            REQUIRE(size == 7);
            phi_values = {spirv[i + 3], spirv[i + 5]};
            break;
        default:
            break;
        }
        i += size;
    }
    REQUIRE(seven_id != 0);
    Tests::CheckExpect("phi_immediate_identity.ir", Tests::StripIrAddresses(IR::DumpProgram(program)));
    CHECK(phi_values[0] != 0);
    CHECK(phi_values[1] != 0);
    CHECK((phi_values[0] == seven_id || phi_values[1] == seven_id));
}
