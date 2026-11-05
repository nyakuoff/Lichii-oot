// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"

namespace {
using namespace Shader;

struct GatherProgram {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    Profile profile;

    GatherProgram(bool dynamic, bool depth, bool integer, bool half, bool sparse, bool supported) {
        program.stage = Stage::Compute;
        program.workgroup_size = {1, 1, 1};
        program.shared_memory_size = 32;
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        program.info.uses_local_invocation_id = dynamic;
        program.info.uses_sparse_residency = sparse;
        program.info.texture_descriptors.push_back({.type = TextureType::Color2D,
            .is_depth = depth, .is_integer = integer, .count = 1});
        profile.supported_spirv = 0x00010400;
        profile.support_image_gather_extended = supported;
        IR::IREmitter ir{block};
        const IR::U32 packed = dynamic ? ir.LocalInvocationIdX() : ir.Imm32(0x1fe03ffU);
        const auto offsets = [&](IR::U32 word) {
            return ir.CompositeConstruct(
                ir.BitFieldExtract(word, ir.Imm32(0), ir.Imm32(6), true),
                ir.BitFieldExtract(word, ir.Imm32(8), ir.Imm32(6), true),
                ir.BitFieldExtract(word, ir.Imm32(16), ir.Imm32(6), true),
                ir.BitFieldExtract(word, ir.Imm32(24), ir.Imm32(6), true));
        };
        // Literal vectors are already folded by production constant propagation.
        const auto first = dynamic ? offsets(packed)
            : ir.CompositeConstruct(ir.Imm32(-1), ir.Imm32(3), ir.Imm32(-2), ir.Imm32(1));
        const auto second = dynamic ? offsets(ir.IAdd(packed, ir.Imm32(0x020100ffU)))
            : ir.CompositeConstruct(ir.Imm32(0), ir.Imm32(-3), ir.Imm32(2), ir.Imm32(-1));
        const auto coords = ir.CompositeConstruct(ir.Imm32(0.25f), ir.Imm32(0.75f));
        IR::TextureInstInfo info{};
        info.type.Assign(TextureType::Color2D);
        info.is_depth.Assign(depth);
        info.gather_component.Assign(2);
        info.relaxed_precision.Assign(half);
        const auto gather = depth
            ? ir.ImageGatherDref(ir.Imm32(0), coords, first, second, ir.Imm32(0.5f), info)
            : ir.ImageGather(ir.Imm32(0), coords, first, second, info);
        gather.Inst()->ReplaceOpcode(depth ? IR::Opcode::ImageGatherDref : IR::Opcode::ImageGather);
        if (sparse) {
            const auto resident = ir.GetSparseFromOp(gather);
            ir.WriteShared(32, ir.Imm32(16), ir.Select(resident, ir.Imm32(1), ir.Imm32(0)));
        }
        if (half) {
            const auto pair = ir.CompositeConstruct(IR::F32{ir.CompositeExtract(gather, 0)},
                                                    IR::F32{ir.CompositeExtract(gather, 1)});
            ir.WriteShared(32, ir.Imm32(0), ir.PackHalf2x16(pair));
        } else {
            for (u32 i = 0; i < 4; ++i) {
                ir.WriteShared(32, ir.Imm32(i * 4),
                               ir.BitCast<IR::U32>(IR::F32{ir.CompositeExtract(gather, i)}));
            }
        }
    }

    std::vector<u32> Emit(const std::string& name) {
        auto code = Backend::SPIRV::EmitSPIRV(profile, program);
        if (const char* dir = std::getenv("EDEN_GATHER_TEST_OUTPUT")) {
            std::filesystem::create_directories(dir);
            std::ofstream out{std::filesystem::path{dir} / (name + ".spv"), std::ios::binary};
            out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
            REQUIRE(out.good());
        }
        return code;
    }
};

struct Module {
    std::map<spv::Op, std::vector<std::vector<u32>>> ops;
    std::map<u32, std::vector<u32>> definitions;

    explicit Module(const std::vector<u32>& code) {
        for (size_t pos = 5; pos < code.size();) {
            const u32 count = code[pos] >> 16;
            REQUIRE(count > 0);
            REQUIRE(pos + count <= code.size());
            std::vector<u32> words(code.begin() + pos + 1, code.begin() + pos + count);
            ops[static_cast<spv::Op>(code[pos] & 0xffff)].push_back(words);
            switch (static_cast<spv::Op>(code[pos] & 0xffff)) {
            case spv::Op::OpConstant:
            case spv::Op::OpConstantComposite:
            case spv::Op::OpCompositeConstruct:
            case spv::Op::OpCompositeExtract:
            case spv::Op::OpVectorShuffle:
            case spv::Op::OpBitcast:
            case spv::Op::OpSelect:
                definitions[words[1]] = words;
                break;
            default:
                break;
            }
            pos += count;
        }
    }
};
} // namespace

TEST_CASE("SPIR-V runtime per-tap gathers preserve four signed offsets and texel order",
          "[video_core][spirv][oot][gather]") {
    for (const bool depth : {false, true}) {
        for (const bool sparse : {false, true}) {
            for (const bool half : {false, true}) {
                for (const bool integer : {false, true}) {
                    if (integer && (depth || half)) continue;
                    const auto name = std::to_string(depth) + std::to_string(sparse) +
                                      std::to_string(half) + std::to_string(integer);
                    GatherProgram fixture{true, depth, integer, half, sparse, true};
                    const Module module{fixture.Emit("dynamic-" + name)};
                    const auto opcode = sparse
                        ? (depth ? spv::Op::OpImageSparseDrefGather : spv::Op::OpImageSparseGather)
                        : (depth ? spv::Op::OpImageDrefGather : spv::Op::OpImageGather);
                    REQUIRE(module.ops.at(opcode).size() == 4);
                    CHECK(module.ops.at(spv::Op::OpBitFieldSExtract).size() == 8);
                    CHECK(module.ops.at(spv::Op::OpCapability).end() !=
                        std::find(module.ops.at(spv::Op::OpCapability).begin(),
                                  module.ops.at(spv::Op::OpCapability).end(),
                                  std::vector<u32>{static_cast<u32>(spv::Capability::ImageGatherExtended)}));
                    std::array<u32, 4> texels{};
                    for (u32 tap = 0; tap < 4; ++tap) {
                        const auto& gather = module.ops.at(opcode)[tap];
                        REQUIRE(gather.size() == 7);
                        CHECK(gather[5] == static_cast<u32>(spv::ImageOperandsMask::Offset));
                        const auto& cast = module.definitions.at(gather[6]);
                        const auto& pair = module.definitions.at(cast[2]);
                        CHECK(pair[4] == (tap % 2) * 2);
                        CHECK(pair[5] == (tap % 2) * 2 + 1);
                        const auto& vector = module.definitions.at(pair[2]);
                        CHECK(vector == module.ops.at(spv::Op::OpCompositeConstruct)[tap / 2]);
                        u32 color = gather[1];
                        for (const auto& extract : module.ops.at(spv::Op::OpCompositeExtract)) {
                            if (sparse && extract[2] == gather[1] && extract[3] == 1) {
                                color = extract[1];
                            }
                        }
                        for (const auto& extract : module.ops.at(spv::Op::OpCompositeExtract)) {
                            if (extract[2] == color && extract[3] == 3) {
                                texels[tap] = extract[1];
                            }
                        }
                        REQUIRE(texels[tap] != 0);
                    }
                    bool assembled = false;
                    for (const auto& construct : module.ops.at(spv::Op::OpCompositeConstruct)) {
                        assembled |= construct.size() == 6 &&
                            std::equal(texels.begin(), texels.end(), construct.begin() + 2);
                    }
                    CHECK(assembled); // Tap order preserved, with i0,j0 (.w) from each footprint.
                    if (sparse) {
                        CHECK(module.ops.at(spv::Op::OpImageSparseTexelsResident).size() == 4);
                        CHECK(module.ops.at(spv::Op::OpLogicalAnd).size() == 3);
                    }
                }
            }
        }
    }
}

TEST_CASE("SPIR-V literal PTP keeps ConstOffsets and unsupported runtime PTP keeps its fallback",
          "[video_core][spirv][gather]") {
    for (const bool dynamic : {false, true}) {
        GatherProgram fixture{dynamic, false, false, false, false, !dynamic};
        const Module module{fixture.Emit(dynamic ? "unsupported" : "constant")};
        const auto& gathers = module.ops.at(spv::Op::OpImageGather);
        REQUIRE(gathers.size() == 1);
        if (dynamic) {
            CHECK(gathers[0].size() == 5);
        } else {
            CHECK(gathers[0][5] == static_cast<u32>(spv::ImageOperandsMask::ConstOffsets));
            const auto& array = module.definitions.at(gathers[0][6]);
            REQUIRE(array.size() == 6);
            const std::array<s32, 8> expected{-1, 3, -2, 1, 0, -3, 2, -1};
            for (u32 tap = 0; tap < 4; ++tap) {
                const auto& pair = module.definitions.at(array[2 + tap]);
                for (u32 lane = 0; lane < 2; ++lane) {
                    CHECK(module.definitions.at(pair[2 + lane])[2] ==
                          static_cast<u32>(expected[tap * 2 + lane]));
                }
            }
        }
    }
}

TEST_CASE("SPIR-V runtime PTP never sends offsets outside the host gather limits",
          "[video_core][spirv][gather]") {
    GatherProgram fixture{true, false, false, false, false, true};
    fixture.profile.min_texel_gather_offset = -8;
    fixture.profile.max_texel_gather_offset = 7;
    const Module module{fixture.Emit("limited-range")};
    const auto& gathers = module.ops.at(spv::Op::OpImageGather);
    REQUIRE(gathers.size() == 4);
    CHECK(module.ops.at(spv::Op::OpSGreaterThanEqual).size() == 8);
    CHECK(module.ops.at(spv::Op::OpSLessThanEqual).size() == 8);
    CHECK(module.ops.at(spv::Op::OpSelect).size() == 8);
    for (const auto& gather : gathers) {
        const auto& select = module.definitions.at(gather[6]);
        CHECK(select.size() == 5);
        const auto& fallback = module.definitions.at(select[4]);
        REQUIRE(fallback.size() == 4);
        CHECK(module.definitions.at(fallback[2])[2] == 0);
        CHECK(module.definitions.at(fallback[3])[2] == 0);
    }
}
