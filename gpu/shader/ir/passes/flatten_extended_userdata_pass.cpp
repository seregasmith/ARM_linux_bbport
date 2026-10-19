// SPDX-License-Identifier: GPL-2.0-or-later
// Shaders read descriptors and constants through pointers in their user data (s_load_dword from
// shader resource tables, possibly several levels deep, with computed offsets). The host shader
// reads one flat buffer instead: this pass gives every such read (ReadConst, and constant buffer
// reads that produce sharps) a slot in it after the user data registers, and generates the x86
// "SRT walker" that fills the buffer at draw time: it follows the same pointers from the user
// data, recomputing dynamic offsets from slots already copied. bbport: on other hosts (AArch64)
// the walker is an "SRT program" of the same steps, run by RunSrtProgram.
//
// Reads that compute the same value (global value numbering over the pointer and offset
// arithmetic) share a slot. A walker load from unmapped memory (a stale pointer) faults; the
// fault handler supplies 0 for that invocation and resumes without changing shared code.
#include <atomic>
#include <mutex>
#include <optional>
#include <queue>
#include <unordered_map>

#include <boost/container/flat_map.hpp>
#include <boost/container/small_vector.hpp>
#include <magic_enum/magic_enum.hpp>

#include "common/arch.h"
#ifdef ARCH_X86_64
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>
#else
#include <algorithm>
#include <cstring>
#include <vector>
#include "bbport_toggles.h"
#endif
#include "common/assert.h"
#include "common/decoder.h"
#include "common/hash.h"
#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/signal_context.h"
#include "core/emulator_settings.h"
#include "core/signals.h"
#include "shader/ir/ir_emitter.h"
#include "shader/ir/passes/ir_passes.h"
#include "shader/ir/passes/srt.h"
#include "shader/ir/program.h"
#include "shader/info.h"

#ifdef ARCH_X86_64

namespace Shader {

namespace {

using namespace Xbyak::util;

/// All walkers live in one executable buffer. Published instructions are immutable.
struct WalkerCode {
    std::mutex mutex;
    Xbyak::CodeGenerator code{32_MB};
    const u8* start{};
    std::atomic<size_t> published_size{0};
};

WalkerCode& Walkers() {
    static WalkerCode walkers;
    return walkers;
}

/// Supply zero for this failed load only. Patching the shared MOV raced with other threads'
/// fault handlers and permanently discarded valid data on later draws using the same walker.
bool HandleFaultingWalkerLoad(void* context, void* fault_address) {
    WalkerCode& walkers = Walkers();
    const u8* const code = static_cast<const u8*>(Common::GetRip(context));
    const size_t published_size = walkers.published_size.load(std::memory_order_acquire);
    if (!published_size || code < walkers.start || code >= walkers.start + published_size) {
        return false;
    }
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const ZyanStatus status = Common::Decoder::Instance()->decodeInstruction(
        instruction, operands, const_cast<u8*>(code), 15);
    if (!ZYAN_SUCCESS(status) || instruction.mnemonic != ZYDIS_MNEMONIC_MOV ||
        operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
        operands[1].type != ZYDIS_OPERAND_TYPE_MEMORY) {
        return false;
    }
    auto& registers = static_cast<ucontext_t*>(context)->uc_mcontext.gregs;
    switch (operands[0].reg.value) {
    case ZYDIS_REGISTER_RDI:
        registers[REG_RDI] = 0;
        break;
    case ZYDIS_REGISTER_R10D:
        registers[REG_R10] = 0; // a 32-bit MOV also clears the upper half
        break;
    default:
        return false;
    }
    registers[REG_RIP] += instruction.length;
    static std::atomic<u64> faults{0};
    const u64 count = faults.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 5 || (count & (count - 1)) == 0) {
        LOG_WARNING(Render_Recompiler, "SRT walker read unavailable memory at {} (instruction {}, "
                    "{} reads so far); supplied zero for this draw", fault_address,
                    static_cast<const void*>(code), count);
    }
    return true;
}

/// The walker buffer, with the fault handler registered on first use (lock held).
Xbyak::CodeGenerator& WalkerBuffer(WalkerCode& walkers) {
    if (!walkers.start) {
        walkers.start = walkers.code.getCurr();
        // After the memory tracking handler.
        constexpr u32 Priority = 2;
        Core::Signals::Instance()->RegisterAccessViolationHandler(HandleFaultingWalkerLoad,
                                                                  Priority);
    }
    return walkers.code;
}

void DumpWalker(const Info& info, const u8* code, size_t size) {
    using namespace Common::FS;
    const auto dir = GetUserPath(PathType::ShaderDir) / "dumps";
    std::filesystem::create_directories(dir);
    const auto name = fmt::format("{}_{:#018x}.srtprogram.txt", info.hw_stage, info.pgm_hash);
    const IOFile file{dir / name, FileAccessMode::Create, FileType::TextFile};
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    for (u64 address = u64(code); address < u64(code) + size; address += instruction.length) {
        auto* const decoder = Common::Decoder::Instance();
        if (!ZYAN_SUCCESS(decoder->decodeInstruction(instruction, operands,
                                                     reinterpret_cast<void*>(address)))) {
            break;
        }
        file.WriteString(decoder->disassembleInst(instruction, operands, address) + "\n");
    }
}

} // namespace

PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    WalkerCode& walkers = Walkers();
    std::scoped_lock lock{walkers.mutex};
    Xbyak::CodeGenerator& c = WalkerBuffer(walkers);
    const auto func = c.getCurr<PFN_SrtWalker>();
    c.db(ptr, size);
    c.ready();
    walkers.published_size.store(c.getSize(), std::memory_order_release);
    return func;
}

} // namespace Shader

#else

namespace {
/// bbport: the SRT program (hosts other than x86-64): the walk the x86-64 builder emits, as
/// words. Expression ops push a value; PushDyn and CopyDyn take one (a dword offset).
enum SrtOp : u32 {
    OpEnd,
    OpPushImm, ///< offset: push the pointer, load the next one at pointer + offset
    OpPushDyn,
    OpPop,
    OpCopyImm, ///< source, destination: flat[destination] = pointer[source]
    OpCopyDyn, ///< destination
    OpUserData, ///< register: flat[register]
    OpFlat,     ///< offset: flat[offset]
    OpImm,      ///< value
    OpAdd,
    OpSub,
    OpMul,
    OpShl,
    OpShr,
    OpAnd,
    OpOr,
    OpXor,
    OpNot,
    OpUMin,
    OpUMax,
    OpBitFieldUExtract,
};
constexpr u32 SrtMagic = 0x42545253u; // "SRTB"
constexpr u32 MaxDepth = 64, MaxValues = 32;
/// Loads of one walk that faulted: they read zero for this walk only, like the x86-64 walker.
constexpr u32 MaxZeroed = 16;
} // namespace

namespace Shader {

PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    u32 magic = 0;
    if (size < 8 || size % 4 != 0 || (std::memcpy(&magic, ptr, 4), magic != SrtMagic)) {
        LOG_ERROR(Render_Recompiler, "SRT walker in the shader cache is not an SRT program");
        return {};
    }
    // Kept for the process: pipelines refer to their walker for as long as they exist.
    auto* program = new u32[size / 4];
    std::memcpy(program, ptr, size);
    return SrtWalker{program};
}

void RunSrtProgram(const u32* program, const u32* user_data, u32* flat) {
    // A load from unmapped memory (a stale pointer) faults; the fault handler returns here
    // (runtime_fault_recover) and the walk starts over with that load reading zero. Writes go to
    // `flat` only, so a walk can start over.
    sigjmp_buf* const previous = runtime_fault_recover;
    sigjmp_buf recover;
    // Changed between sigsetjmp and the jump back: volatile, so they keep their values.
    volatile u32 faulted = 0;
    volatile u32 zeroed[MaxZeroed] = {};
    volatile u32 num_zeroed = 0;
    if (sigsetjmp(recover, 0)) {
        if (num_zeroed == MaxZeroed) {
            runtime_fault_recover = previous;
            LOG_ERROR(Render_Recompiler, "SRT walker: {} loads faulted, the walk gives up", MaxZeroed);
            return;
        }
        zeroed[num_zeroed] = faulted;
        num_zeroed = num_zeroed + 1;
        static std::atomic<u64> faults{0};
        const u64 count = faults.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 5 || (count & (count - 1)) == 0) {
            LOG_WARNING(Render_Recompiler, "SRT walker read unavailable memory ({} reads so far); "
                        "supplied zero for this draw", count);
        }
    }
    runtime_fault_recover = &recover;
    const auto is_zeroed = [&](u32 at) {
        for (u32 i = 0, n = num_zeroed; i < n; ++i) {
            if (zeroed[i] == at) {
                return true;
            }
        }
        return false;
    };
    const u8* stack[MaxDepth];
    u32 values[MaxValues];
    u32 depth = 0, count = 0;
    const u8* pointer = reinterpret_cast<const u8*>(user_data);
    for (u32 pc = 1;;) {
        const u32 at = pc++;
        const u32 word = program[at];
        if (count >= MaxValues || depth >= MaxDepth) {
            runtime_fault_recover = previous;
            LOG_ERROR(Render_Recompiler, "SRT walker: program exceeds its stack");
            return;
        }
        switch (word) {
        case OpEnd:
            runtime_fault_recover = previous;
            return;
        case OpPushImm:
        case OpPushDyn: {
            const u32 offset = (word == OpPushImm ? program[pc++] : values[--count]) << 2;
            u64 next = 0;
            if (!is_zeroed(at)) {
                faulted = at;
                std::memcpy(&next, pointer + offset, sizeof(next));
            }
            stack[depth++] = pointer;
            pointer = reinterpret_cast<const u8*>(next & 0xFFFFFFFFFFFFull);
            break;
        }
        case OpPop:
            pointer = stack[--depth];
            break;
        case OpCopyImm:
        case OpCopyDyn: {
            const u32 offset = (word == OpCopyImm ? program[pc++] : values[--count]) << 2;
            u32 value = 0;
            if (!is_zeroed(at)) {
                faulted = at;
                std::memcpy(&value, pointer + offset, sizeof(value));
            }
            flat[program[pc++]] = value;
            break;
        }
        case OpUserData:
        case OpFlat:
            values[count++] = flat[program[pc++]];
            break;
        case OpImm:
            values[count++] = program[pc++];
            break;
        case OpNot:
            values[count - 1] = ~values[count - 1];
            break;
        case OpBitFieldUExtract: {
            const u32 bits = values[--count], offset = values[--count];
            u32& base = values[count - 1];
            base = (base >> (offset & 31)) & ((1u << (bits & 31)) - 1);
            break;
        }
        default: {
            const u32 b = values[--count];
            u32& a = values[count - 1];
            switch (word) {
            case OpAdd: a += b; break;
            case OpSub: a -= b; break;
            case OpMul: a *= b; break;
            case OpShl: a <<= b & 31; break;
            case OpShr: a >>= b & 31; break;
            case OpAnd: a &= b; break;
            case OpOr: a |= b; break;
            case OpXor: a ^= b; break;
            case OpUMin: a = std::min(a, b); break;
            case OpUMax: a = std::max(a, b); break;
            default: UNREACHABLE_MSG("Bad SRT program op {}", word);
            }
        }
        }
    }
}

} // namespace Shader

#endif

namespace Shader::Optimization {

namespace {

using IR::Opcode;
#ifdef ARCH_X86_64
using namespace Xbyak::util;
#endif

/// Value numbers of the pointer and offset arithmetic: equal numbers, equal values.
class ValueNumbering {
public:
    u32 Number(const IR::Value& value) {
        if (const auto it = numbers.find(value); it != numbers.end()) {
            return it->second;
        }
        IR::Inst* const inst = value.TryInst();
        if (!inst || !IsPure(inst->GetOpcode())) {
            return New(value);
        }
        Key key;
        key.reserve(2 + inst->NumArgs());
        key.push_back(static_cast<u32>(inst->GetOpcode()));
        key.push_back(inst->Flags<u32>());
        for (size_t i = 0; i < inst->NumArgs(); i++) {
            key.push_back(Number(inst->Arg(i)));
        }
        if (const auto it = by_key.find(key); it != by_key.end()) {
            numbers[value] = it->second;
            return it->second;
        }
        const u32 number = New(value);
        by_key.emplace(std::move(key), number);
        return number;
    }

private:
    using Key = boost::container::small_vector<u32, 8>;
    struct KeyHash {
        size_t operator()(const Key& key) const {
            u64 hash = 0;
            for (const u32 element : key) {
                hash = HashCombine(hash, u64(element));
            }
            return hash;
        }
    };

    static bool IsPure(Opcode op) {
        switch (op) {
        case Opcode::GetUserData:
        case Opcode::CompositeConstructU32x2:
        case Opcode::ReadConst:
        case Opcode::ReadConstBuffer:
        case Opcode::IAdd32:
        case Opcode::ISub32:
        case Opcode::IMul32:
        case Opcode::ShiftLeftLogical32:
        case Opcode::ShiftRightLogical32:
        case Opcode::BitwiseAnd32:
        case Opcode::BitwiseOr32:
        case Opcode::BitwiseXor32:
        case Opcode::BitwiseNot32:
        case Opcode::UMin32:
        case Opcode::UMax32:
        case Opcode::BitFieldUExtract:
            return true;
        default:
            return false;
        }
    }

    u32 New(const IR::Value& value) {
        const u32 number = next++;
        numbers[value] = number;
        return number;
    }

    std::unordered_map<IR::Value, u32> numbers;
    std::unordered_map<Key, u32, KeyHash> by_key;
    u32 next{};
};

/// Offsets read through one pointer, ordered: immediates by value, then instructions in
/// program order (blocks by address).
struct OffsetOrder {
    bool operator()(const IR::Value& a, const IR::Value& b) const {
        if (a.IsImmediate() != b.IsImmediate()) {
            return a.IsImmediate();
        }
        if (a.IsImmediate()) {
            return a.U32() < b.U32();
        }
        const IR::Inst* const a_inst = a.Inst();
        const IR::Inst* const b_inst = b.Inst();
        const IR::Block* const block = a_inst->GetParent();
        if (block != b_inst->GetParent()) {
            return block < b_inst->GetParent();
        }
        const auto& list = block->Instructions();
        for (auto it = list.iterator_to(*a_inst); it != list.end(); ++it) {
            if (&*it == b_inst) {
                return true;
            }
        }
        return false;
    }
};

using OffsetReads = boost::container::flat_map<IR::Value, IR::Inst*, OffsetOrder>;

u16 FlatbufOffset(const IR::Inst* inst) {
    switch (inst->GetOpcode()) {
    case Opcode::ReadConstBuffer:
        return inst->Flags<IR::BufferInstInfo>().flatbuf_off_dw;
    case Opcode::ReadConst:
        return inst->Flags<u16>();
    default:
        UNREACHABLE_MSG("Instruction not supported");
    }
}

void SetFlatbufOffset(IR::Inst* inst, u16 offset) {
    switch (inst->GetOpcode()) {
    case Opcode::ReadConstBuffer: {
        auto flags = inst->Flags<IR::BufferInstInfo>();
        flags.flatbuf_off_dw.Assign(offset);
        inst->SetFlags(flags);
        break;
    }
    case Opcode::ReadConst:
        inst->SetFlags(offset);
        break;
    default:
        UNREACHABLE_MSG("Instruction not supported");
    }
}

bool IsConstantMemoryValue(const IR::Value& value) {
    const IR::Inst* const inst = value.TryInst();
    if (!inst) {
        return false;
    }
    const Opcode op = inst->GetOpcode();
    return op == Opcode::GetUserData || op == Opcode::ReadConst || op == Opcode::ReadConstBuffer;
}

/// Operations a walker can recompute an offset with.
bool IsOffsetArithmetic(Opcode op) {
    switch (op) {
    case Opcode::GetUserData:
    case Opcode::ReadConst:
    case Opcode::ReadConstBuffer:
    case Opcode::IAdd32:
    case Opcode::ISub32:
    case Opcode::IMul32:
    case Opcode::ShiftLeftLogical32:
    case Opcode::ShiftRightLogical32:
    case Opcode::BitwiseAnd32:
    case Opcode::BitwiseOr32:
    case Opcode::BitwiseXor32:
    case Opcode::BitwiseNot32:
    case Opcode::UMin32:
    case Opcode::UMax32:
    case Opcode::BitFieldUExtract:
        return true;
    default:
        return false;
    }
}

#ifdef ARCH_X86_64
/// Builds one walker. Calling convention: rdi the user data (then the current table), rsi the
/// flat buffer; offsets are computed in r10d with ecx and edx as scratch.
class WalkerBuilder {
public:
    explicit WalkerBuilder(Xbyak::CodeGenerator& c_) : c{c_} {}

    ValueNumbering numbering;
    /// The reads through each pointer (by its value number's first instruction).
    std::unordered_map<IR::Inst*, OffsetReads> pointer_reads;
    /// User data pointers the walk starts from, by register.
    boost::container::small_flat_map<IR::ScalarReg, IR::Inst*, 1> roots;
    /// The first instruction of each value number.
    std::unordered_map<u32, IR::Inst*> first_of;
    u16 next_slot{};

    IR::Inst* Canonical(IR::Inst* inst) {
        return first_of.try_emplace(numbering.Number(IR::Value{inst}), inst).first->second;
    }

    /// Copies everything read through `table` (loaded from `table_off_dw` of the current table),
    /// then walks the tables among them.
    void Visit(const IR::Value& table_off_dw, IR::Inst* table) {
        if (table->GetOpcode() != Opcode::GetUserData && FlatbufOffset(table) == 0) {
            // The pointer itself was not copied.
            return;
        }
        if (!PushTable(table_off_dw)) {
            LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
            return;
        }
        OffsetReads* const reads = ReadsThrough(table);
        ASSERT(reads);
        // Contiguous in the guest table, contiguous in the flat buffer.
        for (const auto& [off_dw, read] : *reads) {
            if (off_dw.IsImmediate()) {
                c.mov(r10d, ptr[rdi + (off_dw.U32() << 2)]);
            } else {
                if (!Compute(r10d, off_dw)) {
                    LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
                    continue;
                }
                c.shl(r10d, 2);
                c.mov(r10d, r10d);
                c.mov(r10d, dword[rdi + r10]);
            }
            c.mov(ptr[rsi + (next_slot << 2)], r10d);
            SetFlatbufOffset(read, next_slot++);
        }
        for (const auto& [off_dw, read] : *reads) {
            if (ReadsThrough(read)) {
                Visit(off_dw, read);
            }
        }
        c.pop(rdi);
    }

private:
    OffsetReads* ReadsThrough(IR::Inst* inst) {
        const auto it = pointer_reads.find(inst);
        return it != pointer_reads.end() ? &it->second : nullptr;
    }

    /// Saves the current table and loads the 48-bit pointer at `off_dw` of it.
    bool PushTable(const IR::Value& off_dw) {
        c.push(rdi);
        if (off_dw.IsImmediate()) {
            c.mov(rdi, ptr[rdi + (off_dw.U32() << 2)]);
        } else {
            if (!Compute(r10d, off_dw)) {
                c.pop(rdi);
                return false;
            }
            c.shl(r10d, 2);
            c.mov(r10d, r10d);
            c.mov(rdi, ptr[rdi + r10]);
        }
        c.mov(r10, 0xFFFFFFFFFFFFULL);
        c.and_(rdi, r10);
        return true;
    }

    /// reg = a op b. `op_imm` takes an immediate, `op_mem` the other operand on the stack.
    template <typename OpImm, typename OpMem>
    bool Binary(Xbyak::Reg32 reg, const IR::Inst* inst, OpImm&& op_imm, OpMem&& op_mem) {
        const IR::Value a = inst->Arg(0);
        const IR::Value b = inst->Arg(1);
        if (a.IsImmediate()) {
            return Compute(reg, b) && (op_imm(reg, a.U32()), true);
        }
        if (b.IsImmediate()) {
            return Compute(reg, a) && (op_imm(reg, b.U32()), true);
        }
        if (!Compute(reg, a)) {
            return false;
        }
        c.push(reg.cvt64());
        if (!Compute(reg, b)) {
            c.add(rsp, 8);
            return false;
        }
        op_mem(reg, dword[rsp]);
        c.add(rsp, 8);
        return true;
    }

    /// reg = a shift b: `by_imm` shifts reg by an immediate, `by_cl` by cl.
    template <typename ByImm, typename ByCl>
    bool ShiftOp(Xbyak::Reg32 reg, const IR::Inst* inst, ByImm&& by_imm, ByCl&& by_cl) {
        const IR::Value a = inst->Arg(0);
        const IR::Value b = inst->Arg(1);
        if (b.IsImmediate()) {
            return Compute(reg, a) && (by_imm(reg, b.U32()), true);
        }
        if (a.IsImmediate()) {
            if (!Compute(reg, b)) {
                return false;
            }
            c.mov(ecx, reg);
            c.mov(reg, a.U32());
            by_cl(reg);
            return true;
        }
        if (!Compute(reg, b)) {
            return false;
        }
        c.push(reg.cvt64());
        if (!Compute(reg, a)) {
            c.add(rsp, 8);
            return false;
        }
        c.pop(rcx);
        by_cl(reg);
        return true;
    }

    bool BitFieldUExtract(Xbyak::Reg32 reg, const IR::Inst* inst) {
        const IR::Value base = inst->Arg(0);
        const IR::Value offset = inst->Arg(1);
        const IR::Value count = inst->Arg(2);
        if (base.IsImmediate()) {
            c.mov(reg, base.U32());
        } else if (!Compute(reg, base)) {
            return false;
        }
        bool on_stack = false;
        if (offset.IsImmediate()) {
            c.shr(reg, offset.U32());
        } else {
            c.push(reg.cvt64());
            on_stack = true;
            if (!Compute(reg, offset)) {
                c.add(rsp, 8);
                return false;
            }
            c.mov(ecx, reg);
            c.shr(dword[rsp], cl);
        }
        if (count.IsImmediate()) {
            if (on_stack) {
                c.pop(reg.cvt64());
            }
            if (count.U32() < 32) {
                c.and_(reg, (1U << count.U32()) - 1);
            }
            return true;
        }
        if (!on_stack) {
            c.push(reg.cvt64());
        }
        if (!Compute(reg, count)) {
            c.add(rsp, 8);
            return false;
        }
        c.mov(ecx, reg);
        c.mov(edx, 1);
        c.shl(edx, cl);
        c.dec(edx);
        c.and_(dword[rsp], edx);
        c.pop(reg.cvt64());
        return true;
    }

    /// reg = the value of `off_dw` at draw time (from user data and slots already copied).
    bool Compute(Xbyak::Reg32 reg, const IR::Value& off_dw) {
        ASSERT(reg != ecx && reg != edx);
        IR::Inst* const inst = off_dw.Inst();
        switch (inst->GetOpcode()) {
        case Opcode::GetUserData:
            c.mov(reg, ptr[rsi + (static_cast<u32>(inst->Arg(0).ScalarReg()) << 2)]);
            return true;
        case Opcode::ReadConst:
        case Opcode::ReadConstBuffer: {
            const u16 slot = FlatbufOffset(Canonical(inst));
            if (slot == 0) {
                return false;
            }
            c.mov(reg, ptr[rsi + (slot << 2)]);
            return true;
        }
        default:
            break;
        }
        if (!IsOffsetArithmetic(inst->GetOpcode())) {
            LOG_ERROR(Render_Recompiler, "Unexpected instruction for offset computation, {}",
                      magic_enum::enum_name(inst->GetOpcode()));
            return false;
        }
        ASSERT(!inst->AreAllArgsImmediates());
        switch (inst->GetOpcode()) {
        case Opcode::IAdd32:
            return Binary(reg, inst, [&](auto r, u32 imm) { c.add(r, imm); },
                          [&](auto r, const auto& m) { c.add(r, m); });
        case Opcode::ISub32: {
            const IR::Value a = inst->Arg(0);
            const IR::Value b = inst->Arg(1);
            if (b.IsImmediate()) {
                return Compute(reg, a) && (c.sub(reg, b.U32()), true);
            }
            // a - b = -b + a
            if (a.IsImmediate()) {
                return Compute(reg, b) && (c.neg(reg), c.add(reg, a.U32()), true);
            }
            if (!Compute(reg, a)) {
                return false;
            }
            c.push(reg.cvt64());
            if (!Compute(reg, b)) {
                c.add(rsp, 8);
                return false;
            }
            c.neg(reg);
            c.add(reg, dword[rsp]);
            c.add(rsp, 8);
            return true;
        }
        case Opcode::IMul32:
            return Binary(reg, inst, [&](auto r, u32 imm) { c.imul(r, r, imm); },
                          [&](auto r, const auto& m) { c.imul(r, m); });
        case Opcode::BitwiseAnd32:
            return Binary(reg, inst, [&](auto r, u32 imm) { c.and_(r, imm); },
                          [&](auto r, const auto& m) { c.and_(r, m); });
        case Opcode::BitwiseOr32:
            return Binary(reg, inst, [&](auto r, u32 imm) { c.or_(r, imm); },
                          [&](auto r, const auto& m) { c.or_(r, m); });
        case Opcode::BitwiseXor32:
            return Binary(reg, inst, [&](auto r, u32 imm) { c.xor_(r, imm); },
                          [&](auto r, const auto& m) { c.xor_(r, m); });
        case Opcode::UMin32:
            return Binary(
                reg, inst,
                [&](auto r, u32 imm) {
                    c.mov(ecx, imm);
                    c.cmp(r, ecx);
                    c.cmova(r, ecx);
                },
                [&](auto r, const auto& m) {
                    c.cmp(r, m);
                    c.cmova(r, m);
                });
        case Opcode::UMax32:
            return Binary(
                reg, inst,
                [&](auto r, u32 imm) {
                    c.mov(ecx, imm);
                    c.cmp(r, ecx);
                    c.cmovb(r, ecx);
                },
                [&](auto r, const auto& m) {
                    c.cmp(r, m);
                    c.cmovb(r, m);
                });
        case Opcode::ShiftLeftLogical32:
            return ShiftOp(reg, inst, [&](auto r, u32 imm) { c.shl(r, imm); },
                           [&](auto r) { c.shl(r, cl); });
        case Opcode::ShiftRightLogical32:
            return ShiftOp(reg, inst, [&](auto r, u32 imm) { c.shr(r, imm); },
                           [&](auto r) { c.shr(r, cl); });
        case Opcode::BitwiseNot32:
            return Compute(reg, inst->Arg(0)) && (c.not_(reg), true);
        case Opcode::BitFieldUExtract:
            return BitFieldUExtract(reg, inst);
        default:
            UNREACHABLE();
        }
    }

    Xbyak::CodeGenerator& c;
};
#else
/// bbport: builds one SRT program (hosts other than x86-64), the same walk as the x86-64 builder:
/// offsets are computed on RunSrtProgram's value stack.
class WalkerBuilder {
public:
    explicit WalkerBuilder(std::vector<u32>& p_) : p{p_} {}

    ValueNumbering numbering;
    /// The reads through each pointer (by its value number's first instruction).
    std::unordered_map<IR::Inst*, OffsetReads> pointer_reads;
    /// User data pointers the walk starts from, by register.
    boost::container::small_flat_map<IR::ScalarReg, IR::Inst*, 1> roots;
    /// The first instruction of each value number.
    std::unordered_map<u32, IR::Inst*> first_of;
    u16 next_slot{};

    IR::Inst* Canonical(IR::Inst* inst) {
        return first_of.try_emplace(numbering.Number(IR::Value{inst}), inst).first->second;
    }

    /// Copies everything read through `table` (loaded from `table_off_dw` of the current table),
    /// then walks the tables among them.
    void Visit(const IR::Value& table_off_dw, IR::Inst* table) {
        if (table->GetOpcode() != Opcode::GetUserData && FlatbufOffset(table) == 0) {
            // The pointer itself was not copied.
            return;
        }
        if (!PushTable(table_off_dw)) {
            LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
            return;
        }
        OffsetReads* const reads = ReadsThrough(table);
        ASSERT(reads);
        // Contiguous in the guest table, contiguous in the flat buffer.
        for (const auto& [off_dw, read] : *reads) {
            if (off_dw.IsImmediate()) {
                Emit(OpCopyImm, off_dw.U32(), next_slot);
            } else {
                const size_t mark = p.size();
                if (!Compute(off_dw)) {
                    p.resize(mark);
                    LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
                    continue;
                }
                Emit(OpCopyDyn, next_slot);
            }
            SetFlatbufOffset(read, next_slot++);
        }
        for (const auto& [off_dw, read] : *reads) {
            if (ReadsThrough(read)) {
                Visit(off_dw, read);
            }
        }
        Emit(OpPop);
    }

private:
    template <typename... Words>
    void Emit(Words... words) {
        (p.push_back(static_cast<u32>(words)), ...);
    }

    OffsetReads* ReadsThrough(IR::Inst* inst) {
        const auto it = pointer_reads.find(inst);
        return it != pointer_reads.end() ? &it->second : nullptr;
    }

    /// Saves the current table and loads the 48-bit pointer at `off_dw` of it.
    bool PushTable(const IR::Value& off_dw) {
        if (off_dw.IsImmediate()) {
            Emit(OpPushImm, off_dw.U32());
            return true;
        }
        const size_t mark = p.size();
        if (!Compute(off_dw)) {
            p.resize(mark);
            return false;
        }
        Emit(OpPushDyn);
        return true;
    }

    static u32 BinaryOp(Opcode op) {
        switch (op) {
        case Opcode::IAdd32: return OpAdd;
        case Opcode::ISub32: return OpSub;
        case Opcode::IMul32: return OpMul;
        case Opcode::ShiftLeftLogical32: return OpShl;
        case Opcode::ShiftRightLogical32: return OpShr;
        case Opcode::BitwiseAnd32: return OpAnd;
        case Opcode::BitwiseOr32: return OpOr;
        case Opcode::BitwiseXor32: return OpXor;
        case Opcode::UMin32: return OpUMin;
        case Opcode::UMax32: return OpUMax;
        default: UNREACHABLE();
        }
    }

    /// Pushes the value of `off_dw` at draw time (from user data and slots already copied).
    bool Compute(const IR::Value& off_dw) {
        if (off_dw.IsImmediate()) {
            Emit(OpImm, off_dw.U32());
            return true;
        }
        IR::Inst* const inst = off_dw.Inst();
        switch (inst->GetOpcode()) {
        case Opcode::GetUserData:
            Emit(OpUserData, static_cast<u32>(inst->Arg(0).ScalarReg()));
            return true;
        case Opcode::ReadConst:
        case Opcode::ReadConstBuffer: {
            const u16 slot = FlatbufOffset(Canonical(inst));
            if (slot == 0) {
                return false;
            }
            Emit(OpFlat, slot);
            return true;
        }
        default:
            break;
        }
        if (!IsOffsetArithmetic(inst->GetOpcode())) {
            LOG_ERROR(Render_Recompiler, "Unexpected instruction for offset computation, {}",
                      magic_enum::enum_name(inst->GetOpcode()));
            return false;
        }
        switch (inst->GetOpcode()) {
        case Opcode::BitwiseNot32:
            return Compute(inst->Arg(0)) && (Emit(OpNot), true);
        case Opcode::BitFieldUExtract:
            return Compute(inst->Arg(0)) && Compute(inst->Arg(1)) && Compute(inst->Arg(2)) &&
                   (Emit(OpBitFieldUExtract), true);
        default:
            return Compute(inst->Arg(0)) && Compute(inst->Arg(1)) &&
                   (Emit(BinaryOp(inst->GetOpcode())), true);
        }
    }

    std::vector<u32>& p;
};
#endif

/// s_add_u32 s12, s2, off; s_addc_u32 s13, s3, 0; s_load_dword s16, s[12:13], 0: the pointer
/// becomes s[2:3] and the byte offset joins the read's dword offset.
void FoldPointerAdd(IR::Inst& inst) {
    if (inst.GetOpcode() != Opcode::ReadConst) {
        return;
    }
    IR::Inst* const pointer = inst.Arg(0).Inst();
    const IR::Inst* const lo = pointer->Arg(0).TryInst();
    const IR::Inst* const hi = pointer->Arg(1).TryInst();
    if (!lo || !hi || lo->GetOpcode() != Opcode::IAdd32 || hi->GetOpcode() != Opcode::IAdd32) {
        return;
    }
    IR::Value base;
    IR::Value byte_offset;
    if (IsConstantMemoryValue(lo->Arg(0))) {
        base = lo->Arg(0);
        byte_offset = lo->Arg(1);
    } else if (IsConstantMemoryValue(lo->Arg(1))) {
        base = lo->Arg(1);
        byte_offset = lo->Arg(0);
    } else {
        return;
    }
    // The high half adds the carry: select(carry, 1, 0).
    const IR::Inst* const carry = hi->Arg(1).TryInst();
    if (!carry || carry->GetOpcode() != Opcode::SelectU32 || !carry->Arg(1).IsImmediate() ||
        !carry->Arg(2).IsImmediate() || carry->Arg(1).U32() != 1 || carry->Arg(2).U32() != 0) {
        return;
    }
    pointer->SetArg(0, base);
    pointer->SetArg(1, hi->Arg(0));
    for (const auto& [user, operand] : pointer->Uses()) {
        ASSERT(user->GetOpcode() == Opcode::ReadConst && operand == 0);
        IR::IREmitter ir{*user->GetParent(), IR::Block::InstructionList::s_iterator_to(*user)};
        const IR::U32 offset{user->Arg(1)};
        const IR::U32 add = byte_offset.IsImmediate()
                                ? ir.Imm32(byte_offset.U32() >> 2u)
                                : IR::U32{ir.ShiftRightLogical(IR::U32{byte_offset}, ir.Imm32(2u))};
        if (offset.IsImmediate() && add.IsImmediate()) {
            user->SetArg(1, ir.Imm32(offset.U32() + add.U32()));
        } else {
            user->SetArg(1, ir.IAdd(offset, add));
        }
    }
}

using InstVector = boost::container::small_vector<IR::Inst*, 32>;

/// The reads to flatten, in reverse post order: ReadConsts and sharp-producing constant buffer
/// reads, and the other constant buffer reads their offsets are computed from.
InstVector CollectReads(IR::Program& program) {
    InstVector reads;
    InstVector visited;
    std::queue<IR::Inst*> queue;
    const auto enqueue = [&](IR::Inst* inst) {
        if (std::ranges::find(visited, inst) == visited.end()) {
            visited.push_back(inst);
            queue.push(inst);
        }
    };
    for (auto it = program.post_order_blocks.rbegin(); it != program.post_order_blocks.rend();
         ++it) {
        for (IR::Inst& inst : **it) {
            const Opcode op = inst.GetOpcode();
            if (op != Opcode::ReadConst && op != Opcode::ReadConstBuffer) {
                continue;
            }
            if (op == Opcode::ReadConstBuffer && !inst.Flags<IR::BufferInstInfo>().sharp_source) {
                continue;
            }
            FoldPointerAdd(inst);
            reads.push_back(&inst);
            const IR::Value offset = inst.Arg(1);
            if (offset.IsImmediate()) {
                continue;
            }
            queue.push(offset.Inst());
            while (!queue.empty()) {
                IR::Inst* const term = queue.front();
                queue.pop();
                const Opcode term_op = term->GetOpcode();
                if (term_op == Opcode::ReadConstBuffer) {
                    if (!term->Flags<IR::BufferInstInfo>().sharp_source.Value()) {
                        reads.push_back(term);
                        if (!term->Arg(1).IsImmediate()) {
                            enqueue(term->Arg(1).Inst());
                        }
                    }
                    continue;
                }
                if (term_op == Opcode::GetUserData || term_op == Opcode::ReadConst ||
                    !IsOffsetArithmetic(term_op)) {
                    continue;
                }
                for (size_t arg = term->NumArgs(); arg--;) {
                    if (!term->Arg(arg).IsImmediate()) {
                        enqueue(term->Arg(arg).Inst());
                    }
                }
            }
        }
    }
    return reads;
}

} // namespace

void FlattenExtendedUserdataPass(IR::Program& program) {
#ifdef ARCH_X86_64
    WalkerCode& walkers = Walkers();
    std::scoped_lock lock{walkers.mutex};
    WalkerBuilder builder{walkers.code};
#else
    std::vector<u32> words;
    WalkerBuilder builder{words};
#endif

    const InstVector reads = CollectReads(program);
    for (IR::Inst* const read : reads) {
        if (builder.Canonical(read) != read) {
            continue;
        }
        const IR::Inst* const pointer = read->Arg(0).Inst();
        const IR::Inst* const lo = pointer->Arg(0).TryInst();
        if (lo && lo->GetOpcode() == Opcode::ReadFirstLane) {
            continue;
        }
        ASSERT_MSG(IsConstantMemoryValue(pointer->Arg(0)),
                   "ReadConst base low not from constant memory");
        ASSERT_MSG(IsConstantMemoryValue(pointer->Arg(1)),
                   "ReadConst base high not from constant memory");
        IR::Inst* const table = builder.Canonical(pointer->Arg(0).Inst());
        builder.pointer_reads[table][read->Arg(1)] = read;
        if (table->GetOpcode() == Opcode::GetUserData) {
            builder.roots[table->Arg(0).ScalarReg()] = table;
        }
    }

    Info& info = program.info;
    if (!builder.roots.empty()) {
#ifdef ARCH_X86_64
        Xbyak::CodeGenerator& c = WalkerBuffer(walkers);
        info.srt_info.walker_func = c.getCurr<PFN_SrtWalker>();
#else
        words.push_back(SrtMagic);
#endif
        builder.next_slot = NUM_USER_DATA_REGS;
        ASSERT(builder.next_slot == info.srt_info.flattened_bufsize_dw);
        for (const auto& [sgpr, root] : builder.roots) {
            builder.Visit(IR::Value(static_cast<u32>(sgpr)), root);
        }
#ifdef ARCH_X86_64
        c.ret();
        c.ready();
        walkers.published_size.store(c.getSize(), std::memory_order_release);
        const auto* const code = reinterpret_cast<const u8*>(info.srt_info.walker_func);
        info.srt_info.walker_func_size = c.getCurr() - code;
        if (EmulatorSettings.IsDumpShaders()) {
            DumpWalker(info, code, info.srt_info.walker_func_size);
        }
#else
        words.push_back(OpEnd);
        // Kept for the process: pipelines refer to their walker for as long as they exist.
        u32* const program_words = new u32[words.size()];
        std::memcpy(program_words, words.data(), words.size() * sizeof(u32));
        info.srt_info.walker_func = SrtWalker{program_words};
        info.srt_info.walker_func_size = words.size() * sizeof(u32);
#endif
        info.srt_info.flattened_bufsize_dw = builder.next_slot;
    }

    // Duplicates take the slot of their value number's first read.
    for (IR::Inst* const read : reads) {
        ASSERT(builder.first_of.contains(builder.numbering.Number(IR::Value{read})));
        SetFlatbufOffset(read, FlatbufOffset(builder.Canonical(read)));
    }
    info.RefreshFlatBuf();
}

} // namespace Shader::Optimization
