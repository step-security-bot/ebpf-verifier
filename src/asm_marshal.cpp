// Copyright (c) Prevail Verifier contributors.
// SPDX-License-Identifier: MIT
#include <cassert>
#include <map>
#include <variant>
#include <vector>

#include "asm_marshal.hpp"
#include "asm_ostream.hpp"

using std::vector;

static uint8_t op(Condition::Op op) {
    using Op = Condition::Op;
    switch (op) {
    case Op::EQ: return 0x1;
    case Op::GT: return 0x2;
    case Op::GE: return 0x3;
    case Op::SET: return 0x4;
    case Op::NSET: assert(false);
    case Op::NE: return 0x5;
    case Op::SGT: return 0x6;
    case Op::SGE: return 0x7;
    case Op::LT: return 0xa;
    case Op::LE: return 0xb;
    case Op::SLT: return 0xc;
    case Op::SLE: return 0xd;
    }
    assert(false);
    return {};
}

static uint8_t op(Bin::Op op) {
    using Op = Bin::Op;
    switch (op) {
    case Op::ADD: return 0x0;
    case Op::SUB: return 0x1;
    case Op::MUL: return 0x2;
    case Op::SDIV:
    case Op::UDIV: return 0x3;
    case Op::OR: return 0x4;
    case Op::AND: return 0x5;
    case Op::LSH: return 0x6;
    case Op::RSH: return 0x7;
    case Op::SMOD:
    case Op::UMOD: return 0x9;
    case Op::XOR: return 0xa;
    case Op::MOV:
    case Op::MOVSX8:
    case Op::MOVSX16:
    case Op::MOVSX32: return 0xb;
    case Op::ARSH: return 0xc;
    }
    assert(false);
    return {};
}

static int16_t offset(Bin::Op op) {
    using Op = Bin::Op;
    switch (op) {
    case Op::SDIV:
    case Op::SMOD: return 1;
    case Op::MOVSX8: return 8;
    case Op::MOVSX16: return 16;
    case Op::MOVSX32: return 32;
    }
    return 0;
}

static uint8_t imm(Un::Op op) {
    using Op = Un::Op;
    switch (op) {
    case Op::NEG: return 0;
    case Op::BE16:
    case Op::LE16:
    case Op::SWAP16: return 16;
    case Op::BE32:
    case Op::LE32:
    case Op::SWAP32: return 32;
    case Op::BE64:
    case Op::LE64:
    case Op::SWAP64: return 64;
    }
    assert(false);
    return {};
}

struct MarshalVisitor {
  private:
    static vector<ebpf_inst> makeLddw(Reg dst, bool isFd, int32_t imm, int32_t next_imm) {
        return {ebpf_inst{.opcode = static_cast<uint8_t>(INST_CLS_LD | width_to_opcode(8)),
                          .dst = dst.v,
                          .src = static_cast<uint8_t>(isFd ? 1 : 0),
                          .offset = 0,
                          .imm = imm},
                ebpf_inst{.opcode = 0, .dst = 0, .src = 0, .offset = 0, .imm = next_imm}};
    }

  public:
    std::function<auto(label_t)->int16_t> label_to_offset16;
    std::function<auto(label_t)->int32_t> label_to_offset32;

    vector<ebpf_inst> operator()(Undefined const& a) {
        assert(false);
        return {};
    }

    vector<ebpf_inst> operator()(LoadMapFd const& b) { return makeLddw(b.dst, true, b.mapfd, 0); }

    vector<ebpf_inst> operator()(Bin const& b) {
        if (b.lddw) {
            assert(std::holds_alternative<Imm>(b.v));
            auto [imm, next_imm] = split(std::get<Imm>(b.v).v);
            return makeLddw(b.dst, false, imm, next_imm);
        }

        ebpf_inst res{.opcode = static_cast<uint8_t>((b.is64 ? INST_CLS_ALU64 : INST_CLS_ALU) | (op(b.op) << 4)),
                      .dst = b.dst.v,
                      .src = 0,
                      .offset = offset(b.op),
                      .imm = 0};
        std::visit(overloaded{[&](Reg right) {
                                  res.opcode |= INST_SRC_REG;
                                  res.src = right.v;
                              },
                              [&](Imm right) { res.imm = static_cast<int32_t>(right.v); }},
                   b.v);
        return {res};
    }

    vector<ebpf_inst> operator()(Un const& b) {
        switch (b.op) {
        case Un::Op::NEG:
            return {ebpf_inst{
                // FIX: should be INST_CLS_ALU / INST_CLS_ALU64
                .opcode = static_cast<uint8_t>(INST_CLS_ALU | 0x3 | INST_ALU_OP_NEG),
                .dst = b.dst.v,
                .src = 0,
                .offset = 0,
                .imm = imm(b.op),
            }};
        case Un::Op::LE16:
        case Un::Op::LE32:
        case Un::Op::LE64:
            return {ebpf_inst{
                .opcode = static_cast<uint8_t>(INST_CLS_ALU | INST_ALU_OP_END),
                .dst = b.dst.v,
                .src = 0,
                .offset = 0,
                .imm = imm(b.op),
            }};
        case Un::Op::BE16:
        case Un::Op::BE32:
        case Un::Op::BE64:
            return {ebpf_inst{
                .opcode = static_cast<uint8_t>(INST_CLS_ALU | INST_END_BE | INST_ALU_OP_END),
                .dst = b.dst.v,
                .src = 0,
                .offset = 0,
                .imm = imm(b.op),
            }};
        case Un::Op::SWAP16:
        case Un::Op::SWAP32:
        case Un::Op::SWAP64:
            return {ebpf_inst{
                .opcode = static_cast<uint8_t>(INST_CLS_ALU64 | INST_ALU_OP_END),
                .dst = b.dst.v,
                .src = 0,
                .offset = 0,
                .imm = imm(b.op),
            }};
        }
        assert(false);
        return {};
    }

    vector<ebpf_inst> operator()(Call const& b) {
        return {
            ebpf_inst{.opcode = static_cast<uint8_t>(INST_OP_CALL), .dst = 0, .src = 0, .offset = 0, .imm = b.func}};
    }

    vector<ebpf_inst> operator()(Exit const& b) {
        return {ebpf_inst{.opcode = INST_OP_EXIT, .dst = 0, .src = 0, .offset = 0, .imm = 0}};
    }

    vector<ebpf_inst> operator()(Assume const& b) { throw std::invalid_argument("Cannot marshal assumptions"); }

    vector<ebpf_inst> operator()(Assert const& b) { throw std::invalid_argument("Cannot marshal assertions"); }

    vector<ebpf_inst> operator()(Jmp const& b) {
        if (b.cond) {
            ebpf_inst res{
                .opcode = static_cast<uint8_t>(INST_CLS_JMP | (op(b.cond->op) << 4)),
                .dst = b.cond->left.v,
                .src = 0,
                .offset = label_to_offset16(b.target),
            };
            visit(overloaded{[&](Reg right) {
                                 res.opcode |= INST_SRC_REG;
                                 res.src = right.v;
                             },
                             [&](Imm right) { res.imm = static_cast<int32_t>(right.v); }},
                  b.cond->right);
            return {res};
        } else {
            int32_t imm = label_to_offset32(b.target);
            if (imm != 0)
                return {ebpf_inst{.opcode = INST_OP_JA32, .imm = imm}};
            else
                return {ebpf_inst{.opcode = INST_OP_JA16, .offset = label_to_offset16(b.target)}};
        }
    }

    vector<ebpf_inst> operator()(Mem const& b) {
        Deref access = b.access;
        ebpf_inst res{
            .opcode = static_cast<uint8_t>((INST_MEM << 5) | width_to_opcode(access.width)),
            .dst = 0,
            .src = 0,
            .offset = static_cast<int16_t>(access.offset),
        };
        if (b.is_load) {
            if (!std::holds_alternative<Reg>(b.value))
                throw std::runtime_error(std::string("LD IMM: ") + to_string(b));
            res.opcode |= INST_CLS_LD | 0x1;
            res.dst = static_cast<uint8_t>(std::get<Reg>(b.value).v);
            res.src = static_cast<uint8_t>(access.basereg.v);
        } else {
            res.opcode |= INST_CLS_ST;
            res.dst = access.basereg.v;
            if (std::holds_alternative<Reg>(b.value)) {
                res.opcode |= 0x1;
                res.src = std::get<Reg>(b.value).v;
            } else {
                res.opcode |= 0x0;
                res.imm = static_cast<int32_t>(std::get<Imm>(b.value).v);
            }
        }
        return {res};
    }

    vector<ebpf_inst> operator()(Packet const& b) {
        ebpf_inst res{
            .opcode = static_cast<uint8_t>(INST_CLS_LD | width_to_opcode(b.width)),
            .dst = 0,
            .src = 0,
            .offset = 0,
            .imm = static_cast<int32_t>(b.offset),
        };
        if (b.regoffset) {
            res.opcode |= (INST_IND << 5);
            res.src = b.regoffset->v;
        } else {
            res.opcode |= (INST_ABS << 5);
        }
        return {res};
    }

    vector<ebpf_inst> operator()(LockAdd const& b) {
        return {ebpf_inst{
            .opcode = static_cast<uint8_t>(INST_CLS_ST | 0x1 | (INST_XADD << 5) | width_to_opcode(b.access.width)),
            .dst = b.access.basereg.v,
            .src = b.valreg.v,
            .offset = static_cast<int16_t>(b.access.offset),
            .imm = 0}};
    }

    vector<ebpf_inst> operator()(IncrementLoopCounter const& ins) {
        return {};
    }
};

vector<ebpf_inst> marshal(const Instruction& ins, pc_t pc) {
    return std::visit(MarshalVisitor{label_to_offset16(pc), label_to_offset32(pc)}, ins);
}

vector<ebpf_inst> marshal(const vector<Instruction>& insts) {
    vector<ebpf_inst> res;
    pc_t pc = 0;
    for (const auto& ins : insts) {
        for (auto e : marshal(ins, pc)) {
            pc++;
            res.push_back(e);
        }
    }
    return res;
}

static int size(Instruction inst) {
    if (std::holds_alternative<Bin>(inst)) {
        if (std::get<Bin>(inst).lddw)
            return 2;
    }
    if (std::holds_alternative<LoadMapFd>(inst)) {
        return 2;
    }
    return 1;
}

static auto get_labels(const InstructionSeq& insts) {
    pc_t pc = 0;
    std::map<label_t, pc_t> pc_of_label;
    for (auto [label, inst, _] : insts) {
        pc_of_label[label] = pc;
        pc += size(inst);
    }
    return pc_of_label;
}

vector<ebpf_inst> marshal(const InstructionSeq& insts) {
    vector<ebpf_inst> res;
    auto pc_of_label = get_labels(insts);
    pc_t pc = 0;
    for (auto [label, ins, _] : insts) {
        (void)label; // unused
        if (std::holds_alternative<Jmp>(ins)) {
            Jmp& jmp = std::get<Jmp>(ins);
            jmp.target = label_t(pc_of_label.at(jmp.target));
        }
        for (auto e : marshal(ins, pc)) {
            pc++;
            res.push_back(e);
        }
    }
    return res;
}
