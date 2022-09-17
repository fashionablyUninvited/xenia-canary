﻿/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// A note about vectors:
// Xenia represents vectors as xyzw pairs, with indices 0123.
// XMM registers are xyzw pairs with indices 3210, making them more like wzyx.
// This makes things somewhat confusing. It'd be nice to just shuffle the
// registers around on load/store, however certain operations require that
// data be in the right offset.
// Basically, this identity must hold:
//   shuffle(vec, b00011011) -> {x,y,z,w} => {x,y,z,w}
// All indices and operations must respect that.
//
// Memory (big endian):
// [00 01 02 03] [04 05 06 07] [08 09 0A 0B] [0C 0D 0E 0F] (x, y, z, w)
// load into xmm register:
// [0F 0E 0D 0C] [0B 0A 09 08] [07 06 05 04] [03 02 01 00] (w, z, y, x)

#include "xenia/cpu/backend/x64/x64_sequences.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/backend/x64/x64_emitter.h"
#include "xenia/cpu/backend/x64/x64_op.h"
#include "xenia/cpu/backend/x64/x64_tracers.h"
// needed for stmxcsr
#include "xenia/cpu/backend/x64/x64_stack_layout.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/processor.h"
XE_MSVC_OPTIMIZE_SMALL()
DEFINE_bool(use_fast_dot_product, false,
            "Experimental optimization, much shorter sequence on dot products, "
            "treating inf as overflow instead of using mcxsr"
            "four insn dotprod",
            "CPU");

DEFINE_bool(no_round_to_single, false,
            "Not for users, breaks games. Skip rounding double values to "
            "single precision and back",
            "CPU");
DEFINE_bool(inline_loadclock, false,
            "Directly read cached guest clock without calling the LoadClock "
            "method (it gets repeatedly updated by calls from other threads)",
            "CPU");
namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

using namespace Xbyak;

// TODO(benvanik): direct usings.
using namespace xe::cpu;
using namespace xe::cpu::hir;

using xe::cpu::hir::Instr;

typedef bool (*SequenceSelectFn)(X64Emitter&, const Instr*, InstrKeyValue ikey);
std::unordered_map<uint32_t, SequenceSelectFn> sequence_table;

// ============================================================================
// OPCODE_COMMENT
// ============================================================================
struct COMMENT : Sequence<COMMENT, I<OPCODE_COMMENT, VoidOp, OffsetOp>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    if (IsTracingInstr()) {
      auto str = reinterpret_cast<const char*>(i.src1.value);
      // TODO(benvanik): pass through.
      // TODO(benvanik): don't just leak this memory.
      auto str_copy = strdup(str);
      e.mov(e.rdx, reinterpret_cast<uint64_t>(str_copy));
      e.CallNative(reinterpret_cast<void*>(TraceString));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_COMMENT, COMMENT);

// ============================================================================
// OPCODE_NOP
// ============================================================================
struct NOP : Sequence<NOP, I<OPCODE_NOP, VoidOp>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) { e.nop(); }
};
EMITTER_OPCODE_TABLE(OPCODE_NOP, NOP);

// ============================================================================
// OPCODE_SOURCE_OFFSET
// ============================================================================
struct SOURCE_OFFSET
    : Sequence<SOURCE_OFFSET, I<OPCODE_SOURCE_OFFSET, VoidOp, OffsetOp>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.MarkSourceOffset(i.instr);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SOURCE_OFFSET, SOURCE_OFFSET);

// ============================================================================
// OPCODE_ASSIGN
// ============================================================================
struct ASSIGN_I8 : Sequence<ASSIGN_I8, I<OPCODE_ASSIGN, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.mov(i.dest, i.src1);
  }
};
struct ASSIGN_I16 : Sequence<ASSIGN_I16, I<OPCODE_ASSIGN, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.mov(i.dest, i.src1);
  }
};
struct ASSIGN_I32 : Sequence<ASSIGN_I32, I<OPCODE_ASSIGN, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.mov(i.dest, i.src1);
  }
};
struct ASSIGN_I64 : Sequence<ASSIGN_I64, I<OPCODE_ASSIGN, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.mov(i.dest, i.src1);
  }
};
struct ASSIGN_F32 : Sequence<ASSIGN_F32, I<OPCODE_ASSIGN, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.vmovaps(i.dest, i.src1);
  }
};
struct ASSIGN_F64 : Sequence<ASSIGN_F64, I<OPCODE_ASSIGN, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.vmovaps(i.dest, i.src1);
  }
};
struct ASSIGN_V128 : Sequence<ASSIGN_V128, I<OPCODE_ASSIGN, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    SimdDomain domain = e.DeduceSimdDomain(i.src1.value);
    if (domain == SimdDomain::INTEGER) {
      e.vmovdqa(i.dest, i.src1);
    } else {
      e.vmovaps(i.dest, i.src1);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ASSIGN, ASSIGN_I8, ASSIGN_I16, ASSIGN_I32,
                     ASSIGN_I64, ASSIGN_F32, ASSIGN_F64, ASSIGN_V128);

// ============================================================================
// OPCODE_CAST
// ============================================================================
struct CAST_I32_F32 : Sequence<CAST_I32_F32, I<OPCODE_CAST, I32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.vmovd(i.dest, i.src1);
  }
};
struct CAST_I64_F64 : Sequence<CAST_I64_F64, I<OPCODE_CAST, I64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.vmovq(i.dest, i.src1);
  }
};
struct CAST_F32_I32 : Sequence<CAST_F32_I32, I<OPCODE_CAST, F32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.vmovd(i.dest, i.src1);
  }
};
struct CAST_F64_I64 : Sequence<CAST_F64_I64, I<OPCODE_CAST, F64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.vmovq(i.dest, i.src1);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CAST, CAST_I32_F32, CAST_I64_F64, CAST_F32_I32,
                     CAST_F64_I64);

// ============================================================================
// OPCODE_ZERO_EXTEND
// ============================================================================
struct ZERO_EXTEND_I16_I8
    : Sequence<ZERO_EXTEND_I16_I8, I<OPCODE_ZERO_EXTEND, I16Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movzx(i.dest, i.src1);
  }
};
struct ZERO_EXTEND_I32_I8
    : Sequence<ZERO_EXTEND_I32_I8, I<OPCODE_ZERO_EXTEND, I32Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movzx(i.dest, i.src1);
  }
};
struct ZERO_EXTEND_I64_I8
    : Sequence<ZERO_EXTEND_I64_I8, I<OPCODE_ZERO_EXTEND, I64Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movzx(i.dest.reg().cvt32(), i.src1);
  }
};
struct ZERO_EXTEND_I32_I16
    : Sequence<ZERO_EXTEND_I32_I16, I<OPCODE_ZERO_EXTEND, I32Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movzx(i.dest, i.src1);
  }
};
struct ZERO_EXTEND_I64_I16
    : Sequence<ZERO_EXTEND_I64_I16, I<OPCODE_ZERO_EXTEND, I64Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movzx(i.dest.reg().cvt32(), i.src1);
  }
};
struct ZERO_EXTEND_I64_I32
    : Sequence<ZERO_EXTEND_I64_I32, I<OPCODE_ZERO_EXTEND, I64Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.mov(i.dest.reg().cvt32(), i.src1);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ZERO_EXTEND, ZERO_EXTEND_I16_I8, ZERO_EXTEND_I32_I8,
                     ZERO_EXTEND_I64_I8, ZERO_EXTEND_I32_I16,
                     ZERO_EXTEND_I64_I16, ZERO_EXTEND_I64_I32);

// ============================================================================
// OPCODE_SIGN_EXTEND
// ============================================================================
struct SIGN_EXTEND_I16_I8
    : Sequence<SIGN_EXTEND_I16_I8, I<OPCODE_SIGN_EXTEND, I16Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movsx(i.dest, i.src1);
  }
};
struct SIGN_EXTEND_I32_I8
    : Sequence<SIGN_EXTEND_I32_I8, I<OPCODE_SIGN_EXTEND, I32Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movsx(i.dest, i.src1);
  }
};
struct SIGN_EXTEND_I64_I8
    : Sequence<SIGN_EXTEND_I64_I8, I<OPCODE_SIGN_EXTEND, I64Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movsx(i.dest, i.src1);
  }
};
struct SIGN_EXTEND_I32_I16
    : Sequence<SIGN_EXTEND_I32_I16, I<OPCODE_SIGN_EXTEND, I32Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movsx(i.dest, i.src1);
  }
};
struct SIGN_EXTEND_I64_I16
    : Sequence<SIGN_EXTEND_I64_I16, I<OPCODE_SIGN_EXTEND, I64Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movsx(i.dest, i.src1);
  }
};
struct SIGN_EXTEND_I64_I32
    : Sequence<SIGN_EXTEND_I64_I32, I<OPCODE_SIGN_EXTEND, I64Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movsxd(i.dest, i.src1);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SIGN_EXTEND, SIGN_EXTEND_I16_I8, SIGN_EXTEND_I32_I8,
                     SIGN_EXTEND_I64_I8, SIGN_EXTEND_I32_I16,
                     SIGN_EXTEND_I64_I16, SIGN_EXTEND_I64_I32);

// ============================================================================
// OPCODE_TRUNCATE
// ============================================================================
struct TRUNCATE_I8_I16
    : Sequence<TRUNCATE_I8_I16, I<OPCODE_TRUNCATE, I8Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movzx(i.dest.reg().cvt32(), i.src1.reg().cvt8());
  }
};
struct TRUNCATE_I8_I32
    : Sequence<TRUNCATE_I8_I32, I<OPCODE_TRUNCATE, I8Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movzx(i.dest.reg().cvt32(), i.src1.reg().cvt8());
  }
};
struct TRUNCATE_I8_I64
    : Sequence<TRUNCATE_I8_I64, I<OPCODE_TRUNCATE, I8Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movzx(i.dest.reg().cvt32(), i.src1.reg().cvt8());
  }
};
struct TRUNCATE_I16_I32
    : Sequence<TRUNCATE_I16_I32, I<OPCODE_TRUNCATE, I16Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movzx(i.dest.reg().cvt32(), i.src1.reg().cvt16());
  }
};
struct TRUNCATE_I16_I64
    : Sequence<TRUNCATE_I16_I64, I<OPCODE_TRUNCATE, I16Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.movzx(i.dest.reg().cvt32(), i.src1.reg().cvt16());
  }
};
struct TRUNCATE_I32_I64
    : Sequence<TRUNCATE_I32_I64, I<OPCODE_TRUNCATE, I32Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.mov(i.dest, i.src1.reg().cvt32());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_TRUNCATE, TRUNCATE_I8_I16, TRUNCATE_I8_I32,
                     TRUNCATE_I8_I64, TRUNCATE_I16_I32, TRUNCATE_I16_I64,
                     TRUNCATE_I32_I64);

// ============================================================================
// OPCODE_CONVERT
// ============================================================================
struct CONVERT_I32_F32
    : Sequence<CONVERT_I32_F32, I<OPCODE_CONVERT, I32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    // TODO(benvanik): saturation check? cvtt* (trunc?)
    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    if (i.instr->flags == ROUND_TO_ZERO) {
      e.vcvttss2si(i.dest, src1);
    } else {
      e.vcvtss2si(i.dest, src1);
    }
  }
};
struct CONVERT_I32_F64
    : Sequence<CONVERT_I32_F64, I<OPCODE_CONVERT, I32Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    // Intel returns 0x80000000 if the double value does not fit within an int32
    // PPC saturates the value instead.
    // So, we can clamp the double value to (double)0x7FFFFFFF.
    e.vminsd(e.xmm1, GetInputRegOrConstant(e, i.src1, e.xmm0),
             e.GetXmmConstPtr(XMMIntMaxPD));
    if (i.instr->flags == ROUND_TO_ZERO) {
      e.vcvttsd2si(i.dest, e.xmm1);
    } else {
      e.vcvtsd2si(i.dest, e.xmm1);
    }
  }
};
struct CONVERT_I64_F64
    : Sequence<CONVERT_I64_F64, I<OPCODE_CONVERT, I64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    e.xor_(e.eax, e.eax);
    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);

    e.vcomisd(src1, e.GetXmmConstPtr(XmmConst::XMMZero));
    if (i.instr->flags == ROUND_TO_ZERO) {
      e.vcvttsd2si(i.dest, src1);
    } else {
      e.vcvtsd2si(i.dest, src1);
    }
    // cf set if less than
    e.setnc(e.cl);
    e.cmp(i.dest, -1LL);
    // if dest == 0x80000000 and not inp < 0 then dest = 0x7FFFFFFF
    e.seto(e.al);
    e.and_(e.al, e.cl);
    e.sub(i.dest, e.rax);
  }
};
struct CONVERT_F32_I32
    : Sequence<CONVERT_F32_I32, I<OPCODE_CONVERT, F32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(CONVERT_F32_I32);
  }
};
struct CONVERT_F32_F64
    : Sequence<CONVERT_F32_F64, I<OPCODE_CONVERT, F32Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    // TODO(benvanik): saturation check? cvtt* (trunc?)
    e.vcvtsd2ss(i.dest, GetInputRegOrConstant(e, i.src1, e.xmm0));
  }
};
struct CONVERT_F64_I64
    : Sequence<CONVERT_F64_I64, I<OPCODE_CONVERT, F64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);

    Reg64 input = i.src1;
    if (i.src1.is_constant) {
      input = e.rax;
      e.mov(input, (uintptr_t)i.src1.constant());
    }
    // TODO(benvanik): saturation check? cvtt* (trunc?)
    e.vcvtsi2sd(i.dest, input);
  }
};
struct CONVERT_F64_F32
    : Sequence<CONVERT_F64_F32, I<OPCODE_CONVERT, F64Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    e.vcvtss2sd(i.dest, GetInputRegOrConstant(e, i.src1, e.xmm0));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CONVERT, CONVERT_I32_F32, CONVERT_I32_F64,
                     CONVERT_I64_F64, CONVERT_F32_I32, CONVERT_F32_F64,
                     CONVERT_F64_I64, CONVERT_F64_F32);

struct TOSINGLE_F64_F64
    : Sequence<TOSINGLE_F64_F64, I<OPCODE_TO_SINGLE, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);

    Xmm srcreg = GetInputRegOrConstant(e, i.src1, e.xmm1);

    if (cvars::no_round_to_single) {
      if (i.dest != i.src1 || i.src1.is_constant) {
        e.vmovapd(i.dest, srcreg);
      }

    } else {
      /*
         i compared the results for this cvtss/cvtsd to results generated
         on actual hardware, it looks good to me
      */
      e.vcvtsd2ss(e.xmm0, srcreg);
      e.vcvtss2sd(i.dest, e.xmm0);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_TO_SINGLE, TOSINGLE_F64_F64);
// ============================================================================
// OPCODE_ROUND
// ============================================================================
struct ROUND_F32 : Sequence<ROUND_F32, I<OPCODE_ROUND, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(ROUND_F32);
  }
};
struct ROUND_F64 : Sequence<ROUND_F64, I<OPCODE_ROUND, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    switch (i.instr->flags) {
      case ROUND_TO_ZERO:
        e.vroundsd(i.dest, src1, 0b00000011);
        break;
      case ROUND_TO_NEAREST:
        e.vroundsd(i.dest, src1, 0b00000000);
        break;
      case ROUND_TO_MINUS_INFINITY:
        e.vroundsd(i.dest, src1, 0b00000001);
        break;
      case ROUND_TO_POSITIVE_INFINITY:
        e.vroundsd(i.dest, src1, 0b00000010);
        break;
    }
  }
};
struct ROUND_V128 : Sequence<ROUND_V128, I<OPCODE_ROUND, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    // likely dead code
    e.ChangeMxcsrMode(MXCSRMode::Vmx);
    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    switch (i.instr->flags) {
      case ROUND_TO_ZERO:
        e.vroundps(i.dest, src1, 0b00000011);
        break;
      case ROUND_TO_NEAREST:
        e.vroundps(i.dest, src1, 0b00000000);
        break;
      case ROUND_TO_MINUS_INFINITY:
        e.vroundps(i.dest, src1, 0b00000001);
        break;
      case ROUND_TO_POSITIVE_INFINITY:
        e.vroundps(i.dest, src1, 0b00000010);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ROUND, ROUND_F32, ROUND_F64, ROUND_V128);

// ============================================================================
// OPCODE_LOAD_CLOCK
// ============================================================================
struct LOAD_CLOCK : Sequence<LOAD_CLOCK, I<OPCODE_LOAD_CLOCK, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    if (cvars::inline_loadclock) {
      e.mov(e.rcx,
            e.GetBackendCtxPtr(offsetof(X64BackendContext, guest_tick_count)));
      e.mov(i.dest, e.qword[e.rcx]);
    } else {
      // When scaling is disabled and the raw clock source is selected, the code
      // in the Clock class is actually just forwarding tick counts after one
      // simple multiply and division. In that case we rather bake the scaling
      // in here to cut extra function calls with CPU cache misses and stack
      // frame overhead.
      if (cvars::clock_no_scaling && cvars::clock_source_raw) {
        auto ratio = Clock::guest_tick_ratio();
        // The 360 CPU is an in-order CPU, AMD64 usually isn't. Without
        // mfence/lfence magic the rdtsc instruction can be executed sooner or
        // later in the cache window. Since it's resolution however is much
        // higher than the 360's mftb instruction this can safely be ignored.

        // Read time stamp in edx (high part) and eax (low part).
        e.rdtsc();
        // Make it a 64 bit number in rax.
        e.shl(e.rdx, 32);
        e.or_(e.rax, e.rdx);
        // Apply tick frequency scaling.
        e.mov(e.rcx, ratio.first);
        e.mul(e.rcx);
        // We actually now have a 128 bit number in rdx:rax.
        e.mov(e.rcx, ratio.second);
        e.div(e.rcx);
        e.mov(i.dest, e.rax);
      } else {
        e.CallNative(LoadClock);
        e.mov(i.dest, e.rax);
      }
    }
  }
  static uint64_t LoadClock(void* raw_context) {
    return Clock::QueryGuestTickCount();
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_CLOCK, LOAD_CLOCK);

// ============================================================================
// OPCODE_CONTEXT_BARRIER
// ============================================================================
struct CONTEXT_BARRIER
    : Sequence<CONTEXT_BARRIER, I<OPCODE_CONTEXT_BARRIER, VoidOp>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {}
};
EMITTER_OPCODE_TABLE(OPCODE_CONTEXT_BARRIER, CONTEXT_BARRIER);

// ============================================================================
// OPCODE_MAX
// ============================================================================
struct MAX_F32 : Sequence<MAX_F32, I<OPCODE_MAX, F32Op, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(MAX_F32);
  }
};
struct MAX_F64 : Sequence<MAX_F64, I<OPCODE_MAX, F64Op, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    EmitCommutativeBinaryXmmOp(e, i,
                               [](X64Emitter& e, Xmm dest, Xmm src1, Xmm src2) {
                                 e.vmaxsd(dest, src1, src2);
                               });
  }
};
struct MAX_V128 : Sequence<MAX_V128, I<OPCODE_MAX, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Vmx);
    // if 0 and -0, return 0! opposite of minfp
    auto src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    auto src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    e.vmaxps(e.xmm2, src1, src2);
    e.vmaxps(e.xmm3, src2, src1);
    e.vandps(i.dest, e.xmm2, e.xmm3);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MAX, MAX_F32, MAX_F64, MAX_V128);

// ============================================================================
// OPCODE_MIN
// ============================================================================
struct MIN_I8 : Sequence<MIN_I8, I<OPCODE_MIN, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryOp(
        e, i,
        [](X64Emitter& e, const Reg8& dest_src, const Reg8& src) {
          e.cmp(dest_src, src);
          e.cmovg(dest_src.cvt32(), src.cvt32());
        },
        [](X64Emitter& e, const Reg8& dest_src, int32_t constant) {
          e.mov(e.al, constant);
          e.cmp(dest_src, e.al);
          e.cmovg(dest_src.cvt32(), e.eax);
        });
  }
};
struct MIN_I16 : Sequence<MIN_I16, I<OPCODE_MIN, I16Op, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(MIN_I16);
  }
};
struct MIN_I32 : Sequence<MIN_I32, I<OPCODE_MIN, I32Op, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(MIN_I32);
  }
};
struct MIN_I64 : Sequence<MIN_I64, I<OPCODE_MIN, I64Op, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(MIN_I64);
  }
};
struct MIN_F32 : Sequence<MIN_F32, I<OPCODE_MIN, F32Op, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(MIN_F32);
  }
};
struct MIN_F64 : Sequence<MIN_F64, I<OPCODE_MIN, F64Op, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    EmitCommutativeBinaryXmmOp(e, i,
                               [](X64Emitter& e, Xmm dest, Xmm src1, Xmm src2) {
                                 e.vminsd(dest, src1, src2);
                               });
  }
};
struct MIN_V128 : Sequence<MIN_V128, I<OPCODE_MIN, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Vmx);
    auto src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    auto src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    e.vminps(e.xmm2, src1, src2);
    e.vminps(e.xmm3, src2, src1);
    e.vorps(i.dest, e.xmm2, e.xmm3);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MIN, MIN_I8, MIN_I16, MIN_I32, MIN_I64, MIN_F32,
                     MIN_F64, MIN_V128);

// ============================================================================
// OPCODE_SELECT
// ============================================================================
// dest = src1 ? src2 : src3
// TODO(benvanik): match compare + select sequences, as often it's something
//     like SELECT(VECTOR_COMPARE_SGE(a, b), a, b)
struct SELECT_I8
    : Sequence<SELECT_I8, I<OPCODE_SELECT, I8Op, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    Reg8 src2;
    if (i.src2.is_constant) {
      src2 = e.al;
      e.mov(src2, i.src2.constant());
    } else {
      src2 = i.src2;
    }
    e.test(i.src1, i.src1);
    e.cmovnz(i.dest.reg().cvt32(), src2.cvt32());
    e.cmovz(i.dest.reg().cvt32(), i.src3.reg().cvt32());
  }
};
struct SELECT_I16
    : Sequence<SELECT_I16, I<OPCODE_SELECT, I16Op, I8Op, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    Reg16 src2;
    if (i.src2.is_constant) {
      src2 = e.ax;
      e.mov(src2, i.src2.constant());
    } else {
      src2 = i.src2;
    }
    e.test(i.src1, i.src1);
    e.cmovnz(i.dest.reg().cvt32(), src2.cvt32());
    e.cmovz(i.dest.reg().cvt32(), i.src3.reg().cvt32());
  }
};
struct SELECT_I32
    : Sequence<SELECT_I32, I<OPCODE_SELECT, I32Op, I8Op, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    Reg32 src2;
    if (i.src2.is_constant) {
      src2 = e.eax;
      e.mov(src2, i.src2.constant());
    } else {
      src2 = i.src2;
    }
    e.test(i.src1, i.src1);
    e.cmovnz(i.dest, src2);
    e.cmovz(i.dest, i.src3);
  }
};
struct SELECT_I64
    : Sequence<SELECT_I64, I<OPCODE_SELECT, I64Op, I8Op, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    Reg64 src2;
    if (i.src2.is_constant) {
      src2 = e.rax;
      e.mov(src2, i.src2.constant());
    } else {
      src2 = i.src2;
    }
    e.test(i.src1, i.src1);
    e.cmovnz(i.dest, src2);
    e.cmovz(i.dest, i.src3);
  }
};
struct SELECT_F32
    : Sequence<SELECT_F32, I<OPCODE_SELECT, F32Op, I8Op, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(SELECT_F32);
  }
};
struct SELECT_F64
    : Sequence<SELECT_F64, I<OPCODE_SELECT, F64Op, I8Op, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    // dest = src1 != 0 ? src2 : src3
    e.movzx(e.eax, i.src1);
    e.vmovd(e.xmm1, e.eax);
    e.vpxor(e.xmm0, e.xmm0);
    e.vpcmpeqq(e.xmm0, e.xmm1);

    Xmm src2 = i.src2.is_constant ? e.xmm2 : i.src2;
    if (i.src2.is_constant) {
      e.LoadConstantXmm(src2, i.src2.constant());
    }
    e.vpandn(e.xmm1, e.xmm0, src2);

    Xmm src3 = i.src3.is_constant ? e.xmm2 : i.src3;
    if (i.src3.is_constant) {
      e.LoadConstantXmm(src3, i.src3.constant());
    }
    e.vpand(i.dest, e.xmm0, src3);
    e.vpor(i.dest, e.xmm1);
  }
};
struct SELECT_V128_I8
    : Sequence<SELECT_V128_I8, I<OPCODE_SELECT, V128Op, I8Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(SELECT_V128_I8);
  }
};

enum class PermittedBlend : uint32_t { NotPermitted, Int8, Ps };
static bool IsVectorCompare(const Instr* i) {
  Opcode op = i->opcode->num;
  return op >= OPCODE_VECTOR_COMPARE_EQ && op <= OPCODE_VECTOR_COMPARE_UGE;
}
/*
    OPCODE_SELECT does a bit by bit selection, however, if the selector is the
   result of a comparison or if each element may only be 0xff or 0 we may use a
   blend instruction instead
*/
static PermittedBlend GetPermittedBlendForSelectV128(const Value* src1v) {
  const Instr* df = src1v->def;
  if (!df) {
    return PermittedBlend::NotPermitted;
  } else {
    if (!IsVectorCompare(df)) {
      return PermittedBlend::NotPermitted;  // todo: check ors, ands of
                                            // condition
    } else {
      switch (df->flags) {  // check what datatype we compared as
        case INT16_TYPE:
        case INT32_TYPE:
        case INT8_TYPE:
          return PermittedBlend::Int8;  // use vpblendvb
        case FLOAT32_TYPE:
          return PermittedBlend::Ps;  // use vblendvps
        default:                      // unknown type! just ignore
          return PermittedBlend::NotPermitted;
      }
    }
  }
}
struct SELECT_V128_V128
    : Sequence<SELECT_V128_V128,
               I<OPCODE_SELECT, V128Op, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    Xmm src1 = i.src1.is_constant ? e.xmm0 : i.src1;
    PermittedBlend mayblend = GetPermittedBlendForSelectV128(i.src1.value);
    // todo: detect whether src1 is only 0 or FFFF and use blends if so.
    // currently we only detect cmps
    if (i.src1.is_constant) {
      e.LoadConstantXmm(src1, i.src1.constant());
    }

    Xmm src2 = i.src2.is_constant ? e.xmm1 : i.src2;
    if (i.src2.is_constant) {
      e.LoadConstantXmm(src2, i.src2.constant());
    }

    Xmm src3 = i.src3.is_constant ? e.xmm2 : i.src3;
    if (i.src3.is_constant) {
      e.LoadConstantXmm(src3, i.src3.constant());
    }

    if (mayblend == PermittedBlend::Int8) {
      e.vpblendvb(i.dest, src2, src3, src1);
    } else if (mayblend == PermittedBlend::Ps) {
      e.vblendvps(i.dest, src2, src3, src1);
    } else {
      if (e.IsFeatureEnabled(kX64EmitXOP)) {
        e.vpcmov(i.dest, src3, src2, src1);
      } else {
        // src1 ? src2 : src3;

        e.vpandn(e.xmm3, src1, src2);
        e.vpand(i.dest, src1, src3);
        e.vpor(i.dest, i.dest, e.xmm3);
      }
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SELECT, SELECT_I8, SELECT_I16, SELECT_I32,
                     SELECT_I64, SELECT_F32, SELECT_F64, SELECT_V128_I8,
                     SELECT_V128_V128);

static const hir::Instr* GetFirstPrecedingInstrWithPossibleFlagEffects(
    const hir::Instr* i) {
  Opcode iop;

go_further:
  i = i->GetNonFakePrev();
  if (!i) {
    return false;
  }
  iop = i->opcode->num;
  // context/local loads are just movs from mem. we know they will not spoil the
  // flags
  switch (iop) {
    case OPCODE_LOAD_CONTEXT:
    case OPCODE_STORE_CONTEXT:
    case OPCODE_LOAD_LOCAL:
    case OPCODE_STORE_LOCAL:
    case OPCODE_ASSIGN:
      goto go_further;
    default:
      return i;
  }
}

static bool HasPrecedingCmpOfSameValues(const hir::Instr* i) {
  if (IsTracingData()) {
    return false;  // no cmp elim if tracing
  }
  auto prev = GetFirstPrecedingInstrWithPossibleFlagEffects(i);

  if (prev == nullptr) {
    return false;
  }

  Opcode num = prev->opcode->num;

  if (num < OPCODE_COMPARE_EQ || num > OPCODE_COMPARE_UGE) {
    return false;
  }

  return prev->src1.value->IsEqual(i->src1.value) &&
         prev->src2.value->IsEqual(i->src2.value);
}
static bool MayCombineSetxWithFollowingCtxStore(const hir::Instr* setx_insn,
                                                unsigned& out_offset) {
  if (IsTracingData()) {
    return false;
  }
  hir::Value* defed = setx_insn->dest;

  if (!defed->HasSingleUse()) {
    return false;
  }
  hir::Value::Use* single_use = defed->use_head;

  hir::Instr* shouldbestore = single_use->instr;

  if (!shouldbestore) {
    return false;  // probs impossible
  }

  if (shouldbestore->opcode->num == OPCODE_STORE_CONTEXT) {
    if (shouldbestore->GetNonFakePrev() == setx_insn) {
      out_offset = static_cast<unsigned>(shouldbestore->src1.offset);
      shouldbestore->backend_flags |=
          INSTR_X64_FLAGS_ELIMINATED;  // eliminate store
      return true;
    }
  }
  return false;
}

// ============================================================================
// OPCODE_IS_NAN
// ============================================================================
struct IS_NAN_F32 : Sequence<IS_NAN_F32, I<OPCODE_IS_NAN, I8Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(IS_NAN_F32);
  }
};

struct IS_NAN_F64 : Sequence<IS_NAN_F64, I<OPCODE_IS_NAN, I8Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    e.vucomisd(i.src1, i.src1);
    e.setp(i.dest);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_IS_NAN, IS_NAN_F32, IS_NAN_F64);

template <typename dest>
static void CompareEqDoSete(X64Emitter& e, const Instr* instr,
                            const dest& dst) {
  unsigned ctxoffset = 0;
  if (MayCombineSetxWithFollowingCtxStore(instr, ctxoffset)) {
    e.sete(e.byte[e.GetContextReg() + ctxoffset]);
  } else {
    e.sete(dst);
  }
}

// ============================================================================
// OPCODE_COMPARE_EQ
// ============================================================================
struct COMPARE_EQ_I8
    : Sequence<COMPARE_EQ_I8, I<OPCODE_COMPARE_EQ, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    // x86 flags already set?
    if (!HasPrecedingCmpOfSameValues(i.instr)) {
      EmitCommutativeCompareOp(
          e, i,
          [](X64Emitter& e, const Reg8& src1, const Reg8& src2) {
            e.cmp(src1, src2);
          },
          [](X64Emitter& e, const Reg8& src1, int32_t constant) {
            if (constant == 0) {
              e.test(src1, src1);
            } else
              e.cmp(src1, constant);
          });
    }
    CompareEqDoSete(e, i.instr, i.dest);
  }
};
struct COMPARE_EQ_I16
    : Sequence<COMPARE_EQ_I16, I<OPCODE_COMPARE_EQ, I8Op, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    if (!HasPrecedingCmpOfSameValues(i.instr)) {
      EmitCommutativeCompareOp(
          e, i,
          [](X64Emitter& e, const Reg16& src1, const Reg16& src2) {
            e.cmp(src1, src2);
          },
          [](X64Emitter& e, const Reg16& src1, int32_t constant) {
            if (constant == 0) {
              e.test(src1, src1);
            } else
              e.cmp(src1, constant);
          });
    }
    CompareEqDoSete(e, i.instr, i.dest);
  }
};
struct COMPARE_EQ_I32
    : Sequence<COMPARE_EQ_I32, I<OPCODE_COMPARE_EQ, I8Op, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    if (!HasPrecedingCmpOfSameValues(i.instr)) {
      EmitCommutativeCompareOp(
          e, i,
          [](X64Emitter& e, const Reg32& src1, const Reg32& src2) {
            e.cmp(src1, src2);
          },
          [](X64Emitter& e, const Reg32& src1, int32_t constant) {
            if (constant == 0) {
              e.test(src1, src1);
            } else
              e.cmp(src1, constant);
          });
    }
    CompareEqDoSete(e, i.instr, i.dest);
  }
};
struct COMPARE_EQ_I64
    : Sequence<COMPARE_EQ_I64, I<OPCODE_COMPARE_EQ, I8Op, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    if (!HasPrecedingCmpOfSameValues(i.instr)) {
      EmitCommutativeCompareOp(
          e, i,
          [](X64Emitter& e, const Reg64& src1, const Reg64& src2) {
            e.cmp(src1, src2);
          },
          [](X64Emitter& e, const Reg64& src1, int32_t constant) {
            if (constant == 0) {
              e.test(src1, src1);
            } else
              e.cmp(src1, constant);
          });
    }
    CompareEqDoSete(e, i.instr, i.dest);
  }
};
struct COMPARE_EQ_F32
    : Sequence<COMPARE_EQ_F32, I<OPCODE_COMPARE_EQ, I8Op, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    if (!HasPrecedingCmpOfSameValues(i.instr)) {
      EmitCommutativeBinaryXmmOp(
          e, i,
          [&i](X64Emitter& e, I8Op dest, const Xmm& src1, const Xmm& src2) {
            e.vcomiss(src1, src2);
          });
    }
    CompareEqDoSete(e, i.instr, i.dest);
  }
};
struct COMPARE_EQ_F64
    : Sequence<COMPARE_EQ_F64, I<OPCODE_COMPARE_EQ, I8Op, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    if (!HasPrecedingCmpOfSameValues(i.instr)) {
      EmitCommutativeBinaryXmmOp(
          e, i,
          [&i](X64Emitter& e, I8Op dest, const Xmm& src1, const Xmm& src2) {
            e.vcomisd(src1, src2);
          });
    }
    CompareEqDoSete(e, i.instr, i.dest);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_COMPARE_EQ, COMPARE_EQ_I8, COMPARE_EQ_I16,
                     COMPARE_EQ_I32, COMPARE_EQ_I64, COMPARE_EQ_F32,
                     COMPARE_EQ_F64);

template <typename dest>
static void CompareNeDoSetne(X64Emitter& e, const Instr* instr,
                             const dest& dst) {
  unsigned ctxoffset = 0;
  if (MayCombineSetxWithFollowingCtxStore(instr, ctxoffset)) {
    e.setne(e.byte[e.GetContextReg() + ctxoffset]);
  } else {
    e.setne(dst);
  }
}
// ============================================================================
// OPCODE_COMPARE_NE
// ============================================================================
struct COMPARE_NE_I8
    : Sequence<COMPARE_NE_I8, I<OPCODE_COMPARE_NE, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    if (!HasPrecedingCmpOfSameValues(i.instr)) {
      EmitCommutativeCompareOp(
          e, i,
          [](X64Emitter& e, const Reg8& src1, const Reg8& src2) {
            e.cmp(src1, src2);
          },
          [](X64Emitter& e, const Reg8& src1, int32_t constant) {
            e.cmp(src1, constant);
          });
    }
    CompareNeDoSetne(e, i.instr, i.dest);
  }
};
struct COMPARE_NE_I16
    : Sequence<COMPARE_NE_I16, I<OPCODE_COMPARE_NE, I8Op, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    if (!HasPrecedingCmpOfSameValues(i.instr)) {
      EmitCommutativeCompareOp(
          e, i,
          [](X64Emitter& e, const Reg16& src1, const Reg16& src2) {
            e.cmp(src1, src2);
          },
          [](X64Emitter& e, const Reg16& src1, int32_t constant) {
            e.cmp(src1, constant);
          });
    }
    CompareNeDoSetne(e, i.instr, i.dest);
  }
};
struct COMPARE_NE_I32
    : Sequence<COMPARE_NE_I32, I<OPCODE_COMPARE_NE, I8Op, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    if (!HasPrecedingCmpOfSameValues(i.instr)) {
      EmitCommutativeCompareOp(
          e, i,
          [](X64Emitter& e, const Reg32& src1, const Reg32& src2) {
            e.cmp(src1, src2);
          },
          [](X64Emitter& e, const Reg32& src1, int32_t constant) {
            e.cmp(src1, constant);
          });
    }
    CompareNeDoSetne(e, i.instr, i.dest);
  }
};
struct COMPARE_NE_I64
    : Sequence<COMPARE_NE_I64, I<OPCODE_COMPARE_NE, I8Op, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    if (!HasPrecedingCmpOfSameValues(i.instr)) {
      EmitCommutativeCompareOp(
          e, i,
          [](X64Emitter& e, const Reg64& src1, const Reg64& src2) {
            e.cmp(src1, src2);
          },
          [](X64Emitter& e, const Reg64& src1, int32_t constant) {
            e.cmp(src1, constant);
          });
    }
    CompareNeDoSetne(e, i.instr, i.dest);
  }
};
struct COMPARE_NE_F32
    : Sequence<COMPARE_NE_F32, I<OPCODE_COMPARE_NE, I8Op, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    if (!HasPrecedingCmpOfSameValues(i.instr)) {
      e.vcomiss(i.src1, i.src2);
    }
    CompareNeDoSetne(e, i.instr, i.dest);
  }
};
struct COMPARE_NE_F64
    : Sequence<COMPARE_NE_F64, I<OPCODE_COMPARE_NE, I8Op, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    if (!HasPrecedingCmpOfSameValues(i.instr)) {
      e.vcomisd(i.src1, i.src2);
    }
    CompareNeDoSetne(e, i.instr, i.dest);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_COMPARE_NE, COMPARE_NE_I8, COMPARE_NE_I16,
                     COMPARE_NE_I32, COMPARE_NE_I64, COMPARE_NE_F32,
                     COMPARE_NE_F64);

#define EMITTER_ASSOCIATE_CMP_INT_DO_SET(emit_instr, inverse_instr) \
  unsigned ctxoffset = 0;                                           \
  if (MayCombineSetxWithFollowingCtxStore(i.instr, ctxoffset)) {    \
    auto addr = e.byte[e.GetContextReg() + ctxoffset];              \
    if (!inverse) {                                                 \
      e.emit_instr(addr);                                           \
    } else {                                                        \
      e.inverse_instr(addr);                                        \
    }                                                               \
  } else {                                                          \
    if (!inverse) {                                                 \
      e.emit_instr(dest);                                           \
    } else {                                                        \
      e.inverse_instr(dest);                                        \
    }                                                               \
  }
// ============================================================================
// OPCODE_COMPARE_*
// ============================================================================
#define EMITTER_ASSOCIATIVE_COMPARE_INT(op, emit_instr, inverse_instr, type, \
                                        reg_type)                            \
  struct COMPARE_##op##_##type                                               \
      : Sequence<COMPARE_##op##_##type,                                      \
                 I<OPCODE_COMPARE_##op, I8Op, type, type>> {                 \
    static void Emit(X64Emitter& e, const EmitArgType& i) {                  \
      EmitAssociativeCompareOp(                                              \
          e, i,                                                              \
          [&i](X64Emitter& e, const Reg8& dest, const reg_type& src1,        \
               const reg_type& src2, bool inverse) {                         \
            if (!HasPrecedingCmpOfSameValues(i.instr)) {                     \
              e.cmp(src1, src2);                                             \
            }                                                                \
            EMITTER_ASSOCIATE_CMP_INT_DO_SET(emit_instr, inverse_instr)      \
          },                                                                 \
          [&i](X64Emitter& e, const Reg8& dest, const reg_type& src1,        \
               int32_t constant, bool inverse) {                             \
            if (!HasPrecedingCmpOfSameValues(i.instr)) {                     \
              e.cmp(src1, constant);                                         \
            }                                                                \
            EMITTER_ASSOCIATE_CMP_INT_DO_SET(emit_instr, inverse_instr)      \
          });                                                                \
    }                                                                        \
  };
#define EMITTER_ASSOCIATIVE_COMPARE_XX(op, instr, inverse_instr)           \
  EMITTER_ASSOCIATIVE_COMPARE_INT(op, instr, inverse_instr, I8Op, Reg8);   \
  EMITTER_ASSOCIATIVE_COMPARE_INT(op, instr, inverse_instr, I16Op, Reg16); \
  EMITTER_ASSOCIATIVE_COMPARE_INT(op, instr, inverse_instr, I32Op, Reg32); \
  EMITTER_ASSOCIATIVE_COMPARE_INT(op, instr, inverse_instr, I64Op, Reg64); \
  EMITTER_OPCODE_TABLE(OPCODE_COMPARE_##op, COMPARE_##op##_I8Op,           \
                       COMPARE_##op##_I16Op, COMPARE_##op##_I32Op,         \
                       COMPARE_##op##_I64Op);
EMITTER_ASSOCIATIVE_COMPARE_XX(SLT, setl, setg);
EMITTER_ASSOCIATIVE_COMPARE_XX(SLE, setle, setge);
EMITTER_ASSOCIATIVE_COMPARE_XX(SGT, setg, setl);
EMITTER_ASSOCIATIVE_COMPARE_XX(SGE, setge, setle);
EMITTER_ASSOCIATIVE_COMPARE_XX(ULT, setb, seta);
EMITTER_ASSOCIATIVE_COMPARE_XX(ULE, setbe, setae);
EMITTER_ASSOCIATIVE_COMPARE_XX(UGT, seta, setb);
EMITTER_ASSOCIATIVE_COMPARE_XX(UGE, setae, setbe);

// https://web.archive.org/web/20171129015931/https://x86.renejeschke.de/html/file_module_x86_id_288.html
// Original link: https://x86.renejeschke.de/html/file_module_x86_id_288.html
#define EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(op, emit_instr)            \
  struct COMPARE_##op##_F32                                           \
      : Sequence<COMPARE_##op##_F32,                                  \
                 I<OPCODE_COMPARE_##op, I8Op, F32Op, F32Op>> {        \
    static void Emit(X64Emitter& e, const EmitArgType& i) {           \
      e.ChangeMxcsrMode(MXCSRMode::Fpu);                              \
      if (!HasPrecedingCmpOfSameValues(i.instr)) {                    \
        e.vcomiss(i.src1, i.src2);                                    \
      }                                                               \
      unsigned ctxoffset = 0;                                         \
      if (MayCombineSetxWithFollowingCtxStore(i.instr, ctxoffset)) {  \
        e.emit_instr(e.byte[e.GetContextReg() + ctxoffset]);          \
      } else {                                                        \
        e.emit_instr(i.dest);                                         \
      }                                                               \
    }                                                                 \
  };                                                                  \
  struct COMPARE_##op##_F64                                           \
      : Sequence<COMPARE_##op##_F64,                                  \
                 I<OPCODE_COMPARE_##op, I8Op, F64Op, F64Op>> {        \
    static void Emit(X64Emitter& e, const EmitArgType& i) {           \
      e.ChangeMxcsrMode(MXCSRMode::Fpu);                              \
      if (!HasPrecedingCmpOfSameValues(i.instr)) {                    \
        if (i.src1.is_constant) {                                     \
          e.LoadConstantXmm(e.xmm0, i.src1.constant());               \
          e.vcomisd(e.xmm0, i.src2);                                  \
        } else if (i.src2.is_constant) {                              \
          e.LoadConstantXmm(e.xmm0, i.src2.constant());               \
          e.vcomisd(i.src1, e.xmm0);                                  \
        } else {                                                      \
          e.vcomisd(i.src1, i.src2);                                  \
        }                                                             \
      }                                                               \
      unsigned ctxoffset = 0;                                         \
      if (MayCombineSetxWithFollowingCtxStore(i.instr, ctxoffset)) {  \
        e.emit_instr(e.byte[e.GetContextReg() + ctxoffset]);          \
      } else {                                                        \
        e.emit_instr(i.dest);                                         \
      }                                                               \
    }                                                                 \
  };                                                                  \
  EMITTER_OPCODE_TABLE(OPCODE_COMPARE_##op##_FLT, COMPARE_##op##_F32, \
                       COMPARE_##op##_F64);
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(SLT, setb);
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(SLE, setbe);
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(SGT, seta);
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(SGE, setae);
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(ULT, setb);
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(ULE, setbe);
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(UGT, seta);
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(UGE, setae);

// ============================================================================
// OPCODE_DID_SATURATE
// ============================================================================
struct DID_SATURATE
    : Sequence<DID_SATURATE, I<OPCODE_DID_SATURATE, I8Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    // TODO(benvanik): implement saturation check (VECTOR_ADD, etc).
    e.xor_(i.dest, i.dest);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DID_SATURATE, DID_SATURATE);

// ============================================================================
// OPCODE_ADD
// ============================================================================
// TODO(benvanik): put dest/src1|2 together.
template <typename SEQ, typename REG, typename ARGS>
void EmitAddXX(X64Emitter& e, const ARGS& i) {
  SEQ::EmitCommutativeBinaryOp(
      e, i,
      [](X64Emitter& e, const REG& dest_src, const REG& src) {
        e.add(dest_src, src);
      },
      [](X64Emitter& e, const REG& dest_src, int32_t constant) {
        if (constant == 1 && e.IsFeatureEnabled(kX64FlagsIndependentVars)) {
          e.inc(dest_src);
        } else {
          e.add(dest_src, constant);
        }
      });
}
struct ADD_I8 : Sequence<ADD_I8, I<OPCODE_ADD, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitAddXX<ADD_I8, Reg8>(e, i);
  }
};
struct ADD_I16 : Sequence<ADD_I16, I<OPCODE_ADD, I16Op, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitAddXX<ADD_I16, Reg16>(e, i);
  }
};
struct ADD_I32 : Sequence<ADD_I32, I<OPCODE_ADD, I32Op, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitAddXX<ADD_I32, Reg32>(e, i);
  }
};
struct ADD_I64 : Sequence<ADD_I64, I<OPCODE_ADD, I64Op, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitAddXX<ADD_I64, Reg64>(e, i);
  }
};
struct ADD_F32 : Sequence<ADD_F32, I<OPCODE_ADD, F32Op, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(ADD_F32);
  }
};
struct ADD_F64 : Sequence<ADD_F64, I<OPCODE_ADD, F64Op, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);

    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    Xmm src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    e.vaddsd(i.dest, src1, src2);
  }
};
struct ADD_V128 : Sequence<ADD_V128, I<OPCODE_ADD, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Vmx);

    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    Xmm src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    e.vaddps(i.dest, src1, src2);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ADD, ADD_I8, ADD_I16, ADD_I32, ADD_I64, ADD_F32,
                     ADD_F64, ADD_V128);

// ============================================================================
// OPCODE_ADD_CARRY
// ============================================================================
// TODO(benvanik): put dest/src1|2 together.
template <typename SEQ, typename REG, typename ARGS>
void EmitAddCarryXX(X64Emitter& e, const ARGS& i) {
  // TODO(benvanik): faster setting? we could probably do some fun math tricks
  // here to get the carry flag set.
  if (i.src3.is_constant) {
    if (i.src3.constant()) {
      e.stc();
    } else {
      e.clc();
    }
  } else {
    e.bt(i.src3.reg().cvt32(), 0);
  }
  SEQ::EmitCommutativeBinaryOp(
      e, i,
      [](X64Emitter& e, const REG& dest_src, const REG& src) {
        e.adc(dest_src, src);
      },
      [](X64Emitter& e, const REG& dest_src, int32_t constant) {
        e.adc(dest_src, constant);
      });
}
struct ADD_CARRY_I8
    : Sequence<ADD_CARRY_I8, I<OPCODE_ADD_CARRY, I8Op, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitAddCarryXX<ADD_CARRY_I8, Reg8>(e, i);
  }
};
struct ADD_CARRY_I16
    : Sequence<ADD_CARRY_I16, I<OPCODE_ADD_CARRY, I16Op, I16Op, I16Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitAddCarryXX<ADD_CARRY_I16, Reg16>(e, i);
  }
};
struct ADD_CARRY_I32
    : Sequence<ADD_CARRY_I32, I<OPCODE_ADD_CARRY, I32Op, I32Op, I32Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitAddCarryXX<ADD_CARRY_I32, Reg32>(e, i);
  }
};
struct ADD_CARRY_I64
    : Sequence<ADD_CARRY_I64, I<OPCODE_ADD_CARRY, I64Op, I64Op, I64Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitAddCarryXX<ADD_CARRY_I64, Reg64>(e, i);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ADD_CARRY, ADD_CARRY_I8, ADD_CARRY_I16,
                     ADD_CARRY_I32, ADD_CARRY_I64);

// ============================================================================
// OPCODE_SUB
// ============================================================================
// TODO(benvanik): put dest/src1|2 together.
template <typename SEQ, typename REG, typename ARGS>
void EmitSubXX(X64Emitter& e, const ARGS& i) {
  SEQ::EmitAssociativeBinaryOp(
      e, i,
      [](X64Emitter& e, const REG& dest_src, const REG& src) {
        e.sub(dest_src, src);
      },
      [](X64Emitter& e, const REG& dest_src, int32_t constant) {
        if (constant == 1 && e.IsFeatureEnabled(kX64FlagsIndependentVars)) {
          e.dec(dest_src);
        } else {
          e.sub(dest_src, constant);
        }
      });
}
struct SUB_I8 : Sequence<SUB_I8, I<OPCODE_SUB, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitSubXX<SUB_I8, Reg8>(e, i);
  }
};
struct SUB_I16 : Sequence<SUB_I16, I<OPCODE_SUB, I16Op, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitSubXX<SUB_I16, Reg16>(e, i);
  }
};
struct SUB_I32 : Sequence<SUB_I32, I<OPCODE_SUB, I32Op, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitSubXX<SUB_I32, Reg32>(e, i);
  }
};
struct SUB_I64 : Sequence<SUB_I64, I<OPCODE_SUB, I64Op, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitSubXX<SUB_I64, Reg64>(e, i);
  }
};
struct SUB_F32 : Sequence<SUB_F32, I<OPCODE_SUB, F32Op, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(SUB_F32);
  }
};
struct SUB_F64 : Sequence<SUB_F64, I<OPCODE_SUB, F64Op, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    Xmm src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    e.vsubsd(i.dest, src1, src2);
  }
};
struct SUB_V128 : Sequence<SUB_V128, I<OPCODE_SUB, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    e.ChangeMxcsrMode(MXCSRMode::Vmx);
    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    Xmm src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    e.vsubps(i.dest, src1, src2);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SUB, SUB_I8, SUB_I16, SUB_I32, SUB_I64, SUB_F32,
                     SUB_F64, SUB_V128);

// ============================================================================
// OPCODE_MUL
// ============================================================================
// Sign doesn't matter here, as we don't use the high bits.
// We exploit mulx here to avoid creating too much register pressure.
struct MUL_I8 : Sequence<MUL_I8, I<OPCODE_MUL, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(MUL_I8);
  }
};
struct MUL_I16 : Sequence<MUL_I16, I<OPCODE_MUL, I16Op, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(MUL_I8);
  }
};
struct MUL_I32 : Sequence<MUL_I32, I<OPCODE_MUL, I32Op, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      uint32_t multiplier = i.src2.value->constant.u32;
      if (multiplier == 3 || multiplier == 5 || multiplier == 9) {
        e.lea(i.dest, e.ptr[i.src1.reg() * (multiplier - 1) + i.src1.reg()]);
        return;
      }
    }

    if (e.IsFeatureEnabled(kX64EmitBMI2)) {
      // mulx: $1:$2 = EDX * $3

      // TODO(benvanik): place src2 in edx?
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.mov(e.edx, i.src2);
        e.mov(e.eax, i.src1.constant());
        e.mulx(e.edx, i.dest, e.eax);
      } else if (i.src2.is_constant) {
        e.mov(e.edx, i.src1);
        e.mov(e.eax, i.src2.constant());
        e.mulx(e.edx, i.dest, e.eax);
      } else {
        e.mov(e.edx, i.src2);
        e.mulx(e.edx, i.dest, i.src1);
      }
    } else {
      // x86 mul instruction
      // EDX:EAX = EAX * $1;

      // is_constant AKA not a register
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);  // can't multiply 2 constants
        e.mov(e.eax, i.src1.constant());
        e.mul(i.src2);
        e.mov(i.dest, e.eax);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);  // can't multiply 2 constants
        e.mov(e.eax, i.src2.constant());
        e.mul(i.src1);
        e.mov(i.dest, e.eax);
      } else {
        e.mov(e.eax, i.src1);
        e.mul(i.src2);
        e.mov(i.dest, e.eax);
      }
    }
  }
};
struct MUL_I64 : Sequence<MUL_I64, I<OPCODE_MUL, I64Op, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      uint64_t multiplier = i.src2.value->constant.u64;
      if (multiplier == 3 || multiplier == 5 || multiplier == 9) {
        e.lea(i.dest,
              e.ptr[i.src1.reg() * ((int)multiplier - 1) + i.src1.reg()]);
        return;
      }
    }

    if (e.IsFeatureEnabled(kX64EmitBMI2)) {
      // mulx: $1:$2 = RDX * $3

      // TODO(benvanik): place src2 in edx?
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.mov(e.rdx, i.src2);
        e.mov(e.rax, i.src1.constant());
        e.mulx(e.rdx, i.dest, e.rax);
      } else if (i.src2.is_constant) {
        e.mov(e.rdx, i.src1);
        e.mov(e.rax, i.src2.constant());
        e.mulx(e.rdx, i.dest, e.rax);
      } else {
        e.mov(e.rdx, i.src2);
        e.mulx(e.rdx, i.dest, i.src1);
      }
    } else {
      // x86 mul instruction
      // RDX:RAX = RAX * $1;

      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);  // can't multiply 2 constants
        e.mov(e.rax, i.src1.constant());
        e.mul(i.src2);
        e.mov(i.dest, e.rax);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);  // can't multiply 2 constants
        e.mov(e.rax, i.src2.constant());
        e.mul(i.src1);
        e.mov(i.dest, e.rax);
      } else {
        e.mov(e.rax, i.src1);
        e.mul(i.src2);
        e.mov(i.dest, e.rax);
      }
    }
  }
};
struct MUL_F32 : Sequence<MUL_F32, I<OPCODE_MUL, F32Op, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(MUL_F32);
  }
};
struct MUL_F64 : Sequence<MUL_F64, I<OPCODE_MUL, F64Op, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    e.ChangeMxcsrMode(MXCSRMode::Fpu);

    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    Xmm src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    e.vmulsd(i.dest, src1, src2);
  }
};
struct MUL_V128 : Sequence<MUL_V128, I<OPCODE_MUL, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    e.ChangeMxcsrMode(MXCSRMode::Vmx);
    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    Xmm src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    e.vmulps(i.dest, src1, src2);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MUL, MUL_I8, MUL_I16, MUL_I32, MUL_I64, MUL_F32,
                     MUL_F64, MUL_V128);

// ============================================================================
// OPCODE_MUL_HI
// ============================================================================
struct MUL_HI_I8 : Sequence<MUL_HI_I8, I<OPCODE_MUL_HI, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(MUL_HI_I8);
  }
};
struct MUL_HI_I16
    : Sequence<MUL_HI_I16, I<OPCODE_MUL_HI, I16Op, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(MUL_HI_I8);
  }
};
struct MUL_HI_I32
    : Sequence<MUL_HI_I32, I<OPCODE_MUL_HI, I32Op, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(MUL_HI_I32);
  }
};
struct MUL_HI_I64
    : Sequence<MUL_HI_I64, I<OPCODE_MUL_HI, I64Op, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    if (i.instr->flags & ARITHMETIC_UNSIGNED) {
      if (e.IsFeatureEnabled(kX64EmitBMI2)) {
        // TODO(benvanik): place src1 in eax? still need to sign extend
        e.mov(e.rdx, i.src1);
        if (i.src2.is_constant) {
          e.mov(e.rax, i.src2.constant());
          e.mulx(i.dest, e.rdx, e.rax);
        } else {
          e.mulx(i.dest, e.rax, i.src2);
        }
      } else {
        // x86 mul instruction
        // RDX:RAX < RAX * REG(op1);
        if (i.src1.is_constant) {
          assert_true(!i.src2.is_constant);  // can't multiply 2 constants
          e.mov(e.rax, i.src1.constant());
          e.mul(i.src2);
          e.mov(i.dest, e.rdx);
        } else if (i.src2.is_constant) {
          assert_true(!i.src1.is_constant);  // can't multiply 2 constants
          e.mov(e.rax, i.src2.constant());
          e.mul(i.src1);
          e.mov(i.dest, e.rdx);
        } else {
          e.mov(e.rax, i.src1);
          e.mul(i.src2);
          e.mov(i.dest, e.rdx);
        }
      }
    } else {
      if (i.src1.is_constant) {
        e.mov(e.rax, i.src1.constant());
      } else {
        e.mov(e.rax, i.src1);
      }
      if (i.src2.is_constant) {
        e.mov(e.rdx, i.src2.constant());
        e.imul(e.rdx);
      } else {
        e.imul(i.src2);
      }
      e.mov(i.dest, e.rdx);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MUL_HI, MUL_HI_I8, MUL_HI_I16, MUL_HI_I32,
                     MUL_HI_I64);

// ============================================================================
// OPCODE_DIV
// ============================================================================
// TODO(benvanik): optimize common constant cases.
// TODO(benvanik): simplify code!
struct DIV_I8 : Sequence<DIV_I8, I<OPCODE_DIV, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(DIV_I8);
  }
};
struct DIV_I16 : Sequence<DIV_I16, I<OPCODE_DIV, I16Op, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(DIV_I16);
  }
};
/*
        TODO: hoist the overflow/zero checks into HIR
*/
struct DIV_I32 : Sequence<DIV_I32, I<OPCODE_DIV, I32Op, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    Xbyak::Label skip;
    e.inLocalLabel();
    e.xor_(e.eax,
           e.eax);  // need to make sure that we're zeroed if its divide by zero
    if (i.src2.is_constant) {
      assert_true(!i.src1.is_constant);

      if (i.instr->flags & ARITHMETIC_UNSIGNED) {
        e.mov(e.ecx, i.src2.constant());
        e.mov(e.eax, i.src1);
        // Zero upper bits.
        e.xor_(e.edx, e.edx);
        e.div(e.ecx);
      } else {
        e.mov(e.ecx, i.src2.constant());
        if (i.src2.constant() == -1) {  // we might have signed overflow, so
                                        // check src1 for 0x80000000 at runtime
          e.cmp(i.src1, 1);

          e.jo(skip, CodeGenerator::T_SHORT);
        }
        e.mov(e.eax, i.src1);

        e.cdq();  // edx:eax = sign-extend eax
        e.idiv(e.ecx);
      }

    } else {
      // Skip if src2 is zero.
      e.test(i.src2, i.src2);
      // branches are assumed not taken, so a newly executed divide instruction
      // that divides by 0 will probably end up speculatively executing the
      // divide instruction :/ hopefully no games rely on divide by zero
      // behavior
      e.jz(skip, CodeGenerator::T_SHORT);

      if (i.instr->flags & ARITHMETIC_UNSIGNED) {
        if (i.src1.is_constant) {
          e.mov(e.eax, i.src1.constant());
        } else {
          e.mov(e.eax, i.src1);
        }
        // Zero upper bits.
        e.xor_(e.edx, e.edx);
        e.div(i.src2);
      } else {
        // check for signed overflow
        if (i.src1.is_constant) {
          if (i.src1.constant() != (1 << 31)) {
            // we're good, overflow is impossible
          } else {
            e.cmp(i.src2, -1);  // otherwise, if src2 is -1 then we have
                                // overflow
            e.jz(skip, CodeGenerator::T_SHORT);
          }
        } else {
          e.xor_(e.ecx, e.ecx);
          e.cmp(i.src1, 1);  //== 0x80000000
          e.seto(e.cl);
          e.cmp(i.src2, -1);
          e.setz(e.ch);
          e.cmp(e.ecx, 0x0101);
          e.jz(skip, CodeGenerator::T_SHORT);
        }

        if (i.src1.is_constant) {
          e.mov(e.eax, i.src1.constant());
        } else {
          e.mov(e.eax, i.src1);
        }

        e.cdq();  // edx:eax = sign-extend eax
        e.idiv(i.src2);
      }
    }

    e.L(skip);
    e.outLocalLabel();
    e.mov(i.dest, e.eax);
  }
};
/*
        TODO: hoist the overflow/zero checks into HIR
*/
struct DIV_I64 : Sequence<DIV_I64, I<OPCODE_DIV, I64Op, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    Xbyak::Label skip;
    e.inLocalLabel();
    e.xor_(e.eax,
           e.eax);  // need to make sure that we're zeroed if its divide by zero
    if (i.src2.is_constant) {
      assert_true(!i.src1.is_constant);

      if (i.instr->flags & ARITHMETIC_UNSIGNED) {
        e.mov(e.rcx, i.src2.constant());
        e.mov(e.rax, i.src1);
        // Zero upper bits.
        e.xor_(e.edx, e.edx);
        e.div(e.rcx);
      } else {
        if (i.src2.constant() ==
            -1LL) {  // we might have signed overflow, so
                     // check src1 for 0x80000000 at runtime
          e.cmp(i.src1, 1);

          e.jo(skip, CodeGenerator::T_SHORT);
        }
        e.mov(e.rcx, i.src2.constant());
        e.mov(e.rax, i.src1);
        e.cqo();  // rdx:rax = sign-extend rax
        e.idiv(e.rcx);
      }
    } else {
      // Skip if src2 is zero.
      e.test(i.src2, i.src2);
      e.jz(skip, CodeGenerator::T_SHORT);

      if (i.instr->flags & ARITHMETIC_UNSIGNED) {
        if (i.src1.is_constant) {
          e.mov(e.rax, i.src1.constant());
        } else {
          e.mov(e.rax, i.src1);
        }
        // Zero upper bits.
        e.xor_(e.edx, e.edx);
        e.div(i.src2);
      } else {
        // check for signed overflow
        if (i.src1.is_constant) {
          if (i.src1.constant() != (1ll << 63)) {
            // we're good, overflow is impossible
          } else {
            e.cmp(i.src2, -1);  // otherwise, if src2 is -1 then we have
                                // overflow
            e.jz(skip, CodeGenerator::T_SHORT);
          }
        } else {
          e.xor_(e.ecx, e.ecx);
          e.cmp(i.src1, 1);  //== 0x80000000
          e.seto(e.cl);
          e.cmp(i.src2, -1);
          e.setz(e.ch);
          e.cmp(e.ecx, 0x0101);
          e.jz(skip, CodeGenerator::T_SHORT);
        }

        if (i.src1.is_constant) {
          e.mov(e.rax, i.src1.constant());
        } else {
          e.mov(e.rax, i.src1);
        }
        e.cqo();  // rdx:rax = sign-extend rax
        e.idiv(i.src2);
      }
    }

    e.L(skip);
    e.outLocalLabel();
    e.mov(i.dest, e.rax);
  }
};
struct DIV_F32 : Sequence<DIV_F32, I<OPCODE_DIV, F32Op, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(DIV_F32);
  }
};
struct DIV_F64 : Sequence<DIV_F64, I<OPCODE_DIV, F64Op, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    e.ChangeMxcsrMode(MXCSRMode::Fpu);

    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    Xmm src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    e.vdivsd(i.dest, src1, src2);
  }
};
struct DIV_V128 : Sequence<DIV_V128, I<OPCODE_DIV, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(DIV_V128);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DIV, DIV_I8, DIV_I16, DIV_I32, DIV_I64, DIV_F32,
                     DIV_F64, DIV_V128);

// ============================================================================
// OPCODE_MUL_ADD
// ============================================================================
// d = 1 * 2 + 3
// $0 = $1x$0 + $2
// Forms of vfmadd/vfmsub:
// - 132 -> $1 = $1 * $3 + $2
// - 213 -> $1 = $2 * $1 + $3
// - 231 -> $1 = $2 * $3 + $1
struct MUL_ADD_F32
    : Sequence<MUL_ADD_F32, I<OPCODE_MUL_ADD, F32Op, F32Op, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(
        MUL_ADD_F32);  // this can never happen, there are very few actual
                       // float32 instructions
  }
};
struct MUL_ADD_F64
    : Sequence<MUL_ADD_F64, I<OPCODE_MUL_ADD, F64Op, F64Op, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);

    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    Xmm src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    Xmm src3 = GetInputRegOrConstant(e, i.src3, e.xmm2);
    if (e.IsFeatureEnabled(kX64EmitFMA)) {
      // todo: this is garbage
      e.vmovapd(e.xmm3, src1);
      e.vfmadd213sd(e.xmm3, src2, src3);
      e.vmovapd(i.dest, e.xmm3);
    } else {
      // todo: might need to use x87 in this case...
      e.vmulsd(e.xmm3, src1, src2);
      e.vaddsd(i.dest, e.xmm3, src3);
    }
  }
};
struct MUL_ADD_V128
    : Sequence<MUL_ADD_V128,
               I<OPCODE_MUL_ADD, V128Op, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Vmx);

    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    Xmm src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    Xmm src3 = GetInputRegOrConstant(e, i.src3, e.xmm2);
    if (e.IsFeatureEnabled(kX64EmitFMA)) {
      // todo: this is garbage
      e.vmovaps(e.xmm3, src1);
      e.vfmadd213ps(e.xmm3, src2, src3);
      e.vmovaps(i.dest, e.xmm3);
    } else {
      // todo: might need to use x87 in this case...
      e.vmulps(e.xmm3, src1, src2);
      e.vaddps(i.dest, e.xmm3, src3);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MUL_ADD, MUL_ADD_F32, MUL_ADD_F64, MUL_ADD_V128);

struct NEGATED_MUL_ADD_F64
    : Sequence<NEGATED_MUL_ADD_F64,
               I<OPCODE_NEGATED_MUL_ADD, F64Op, F64Op, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);

    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    Xmm src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    Xmm src3 = GetInputRegOrConstant(e, i.src3, e.xmm2);
    if (e.IsFeatureEnabled(kX64EmitFMA)) {
      // todo: this is garbage
      e.vmovapd(e.xmm3, src1);
      e.vfmadd213sd(e.xmm3, src2, src3);
      e.vxorpd(i.dest, e.xmm3, e.GetXmmConstPtr(XMMSignMaskPD));
    } else {
      // todo: might need to use x87 in this case...
      e.vmulsd(e.xmm3, src1, src2);
      e.vaddsd(i.dest, e.xmm3, src3);
      e.vxorpd(i.dest, i.dest, e.GetXmmConstPtr(XMMSignMaskPD));
    }
  }
};
struct NEGATED_MUL_ADD_V128
    : Sequence<NEGATED_MUL_ADD_V128,
               I<OPCODE_NEGATED_MUL_ADD, V128Op, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Vmx);

    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    Xmm src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    Xmm src3 = GetInputRegOrConstant(e, i.src3, e.xmm2);
    if (e.IsFeatureEnabled(kX64EmitFMA)) {
      // todo: this is garbage
      e.vmovaps(e.xmm3, src1);
      e.vfmadd213ps(e.xmm3, src2, src3);
      e.vxorps(i.dest, e.xmm3, e.GetXmmConstPtr(XMMSignMaskPS));
    } else {
      // todo: might need to use x87 in this case...
      e.vmulps(e.xmm3, src1, src2);
      e.vaddps(i.dest, e.xmm3, src3);
      e.vxorps(i.dest, i.dest, e.GetXmmConstPtr(XMMSignMaskPS));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_NEGATED_MUL_ADD, NEGATED_MUL_ADD_F64,
                     NEGATED_MUL_ADD_V128);

// ============================================================================
// OPCODE_MUL_SUB
// ============================================================================
// d = 1 * 2 - 3
// $0 = $2x$0 - $3
// TODO(benvanik): use other forms (132/213/etc) to avoid register shuffling.
// dest could be src2 or src3 - need to ensure it's not before overwriting dest
// perhaps use other 132/213/etc
// Forms:
// - 132 -> $1 = $1 * $3 - $2
// - 213 -> $1 = $2 * $1 - $3
// - 231 -> $1 = $2 * $3 - $1

struct MUL_SUB_F64
    : Sequence<MUL_SUB_F64, I<OPCODE_MUL_SUB, F64Op, F64Op, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);

    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    Xmm src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    Xmm src3 = GetInputRegOrConstant(e, i.src3, e.xmm2);
    if (e.IsFeatureEnabled(kX64EmitFMA)) {
      // todo: this is garbage
      e.vmovapd(e.xmm3, src1);
      e.vfmsub213sd(e.xmm3, src2, src3);
      e.vmovapd(i.dest, e.xmm3);
    } else {
      // todo: might need to use x87 in this case...
      e.vmulsd(e.xmm3, src1, src2);
      e.vsubsd(i.dest, e.xmm3, src3);
    }
  }
};
struct MUL_SUB_V128
    : Sequence<MUL_SUB_V128,
               I<OPCODE_MUL_SUB, V128Op, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Vmx);

    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    Xmm src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    Xmm src3 = GetInputRegOrConstant(e, i.src3, e.xmm2);
    if (e.IsFeatureEnabled(kX64EmitFMA)) {
      // todo: this is garbage
      e.vmovaps(e.xmm3, src1);
      e.vfmsub213ps(e.xmm3, src2, src3);
      e.vmovaps(i.dest, e.xmm3);
    } else {
      // todo: might need to use x87 in this case...
      e.vmulps(e.xmm3, src1, src2);
      e.vsubps(i.dest, e.xmm3, src3);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MUL_SUB, MUL_SUB_F64, MUL_SUB_V128);

struct NEGATED_MUL_SUB_F64
    : Sequence<NEGATED_MUL_SUB_F64,
               I<OPCODE_NEGATED_MUL_SUB, F64Op, F64Op, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);

    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    Xmm src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    Xmm src3 = GetInputRegOrConstant(e, i.src3, e.xmm2);
    if (e.IsFeatureEnabled(kX64EmitFMA)) {
      // todo: this is garbage
      e.vmovapd(e.xmm3, src1);
      e.vfmsub213sd(e.xmm3, src2, src3);
      e.vxorpd(i.dest, e.xmm3, e.GetXmmConstPtr(XMMSignMaskPD));
    } else {
      // todo: might need to use x87 in this case...
      e.vmulsd(e.xmm3, src1, src2);
      e.vsubsd(i.dest, e.xmm3, src3);
      e.vxorpd(i.dest, i.dest, e.GetXmmConstPtr(XMMSignMaskPD));
    }
  }
};
struct NEGATED_MUL_SUB_V128
    : Sequence<NEGATED_MUL_SUB_V128,
               I<OPCODE_NEGATED_MUL_SUB, V128Op, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Vmx);

    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    Xmm src2 = GetInputRegOrConstant(e, i.src2, e.xmm1);
    Xmm src3 = GetInputRegOrConstant(e, i.src3, e.xmm2);
    if (e.IsFeatureEnabled(kX64EmitFMA)) {
      // todo: this is garbage
      e.vmovaps(e.xmm3, src1);
      e.vfmsub213ps(e.xmm3, src2, src3);
      e.vxorps(i.dest, e.xmm3, e.GetXmmConstPtr(XMMSignMaskPS));
    } else {
      // todo: might need to use x87 in this case...
      e.vmulps(e.xmm3, src1, src2);
      e.vsubps(i.dest, e.xmm3, src3);
      e.vxorps(i.dest, i.dest, e.GetXmmConstPtr(XMMSignMaskPS));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_NEGATED_MUL_SUB, NEGATED_MUL_SUB_F64,
                     NEGATED_MUL_SUB_V128);

// ============================================================================
// OPCODE_NEG
// ============================================================================
// TODO(benvanik): put dest/src1 together.
template <typename SEQ, typename REG, typename ARGS>
void EmitNegXX(X64Emitter& e, const ARGS& i) {
  SEQ::EmitUnaryOp(e, i,
                   [](X64Emitter& e, const REG& dest_src) { e.neg(dest_src); });
}
struct NEG_I8 : Sequence<NEG_I8, I<OPCODE_NEG, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitNegXX<NEG_I8, Reg8>(e, i);
  }
};
struct NEG_I16 : Sequence<NEG_I16, I<OPCODE_NEG, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitNegXX<NEG_I16, Reg16>(e, i);
  }
};
struct NEG_I32 : Sequence<NEG_I32, I<OPCODE_NEG, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitNegXX<NEG_I32, Reg32>(e, i);
  }
};
struct NEG_I64 : Sequence<NEG_I64, I<OPCODE_NEG, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitNegXX<NEG_I64, Reg64>(e, i);
  }
};
struct NEG_F32 : Sequence<NEG_F32, I<OPCODE_NEG, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    e.vxorps(i.dest, i.src1, e.GetXmmConstPtr(XMMSignMaskPS));
  }
};
struct NEG_F64 : Sequence<NEG_F64, I<OPCODE_NEG, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    e.vxorpd(i.dest, i.src1, e.GetXmmConstPtr(XMMSignMaskPD));
  }
};
struct NEG_V128 : Sequence<NEG_V128, I<OPCODE_NEG, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    e.ChangeMxcsrMode(MXCSRMode::Vmx);
    e.vxorps(i.dest, i.src1, e.GetXmmConstPtr(XMMSignMaskPS));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_NEG, NEG_I8, NEG_I16, NEG_I32, NEG_I64, NEG_F32,
                     NEG_F64, NEG_V128);

// ============================================================================
// OPCODE_ABS
// ============================================================================
struct ABS_F32 : Sequence<ABS_F32, I<OPCODE_ABS, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    e.vandps(i.dest, i.src1, e.GetXmmConstPtr(XMMAbsMaskPS));
  }
};
struct ABS_F64 : Sequence<ABS_F64, I<OPCODE_ABS, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    e.vandpd(i.dest, i.src1, e.GetXmmConstPtr(XMMAbsMaskPD));
  }
};
struct ABS_V128 : Sequence<ABS_V128, I<OPCODE_ABS, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Vmx);
    e.vandps(i.dest, i.src1, e.GetXmmConstPtr(XMMAbsMaskPS));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ABS, ABS_F32, ABS_F64, ABS_V128);

// ============================================================================
// OPCODE_SQRT
// ============================================================================
struct SQRT_F32 : Sequence<SQRT_F32, I<OPCODE_SQRT, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);

    e.vsqrtss(i.dest, GetInputRegOrConstant(e, i.src1, e.xmm0));
  }
};
struct SQRT_F64 : Sequence<SQRT_F64, I<OPCODE_SQRT, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    e.vsqrtsd(i.dest, GetInputRegOrConstant(e, i.src1, e.xmm0));
  }
};
struct SQRT_V128 : Sequence<SQRT_V128, I<OPCODE_SQRT, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Vmx);
    e.vsqrtps(i.dest, GetInputRegOrConstant(e, i.src1, e.xmm0));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SQRT, SQRT_F32, SQRT_F64, SQRT_V128);

// ============================================================================
// OPCODE_RSQRT
// ============================================================================
// Altivec guarantees an error of < 1/4096 for vrsqrtefp while AVX only gives
// < 1.5*2^-12 ≈ 1/2730 for vrsqrtps.
struct RSQRT_F32 : Sequence<RSQRT_F32, I<OPCODE_RSQRT, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm3);

    if (e.IsFeatureEnabled(kX64EmitAVX512Ortho)) {
      e.vrsqrt14ss(i.dest, src1, src1);
    } else {
      e.vmovaps(e.xmm0, e.GetXmmConstPtr(XMMOne));
      e.vsqrtss(e.xmm1, src1, src1);
      e.vdivss(i.dest, e.xmm0, e.xmm1);
    }
  }
};
struct RSQRT_F64 : Sequence<RSQRT_F64, I<OPCODE_RSQRT, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm3);
    if (e.IsFeatureEnabled(kX64EmitAVX512Ortho)) {
      e.vrsqrt14sd(i.dest, src1, src1);
    } else {
      e.vmovapd(e.xmm0, e.GetXmmConstPtr(XMMOnePD));
      e.vsqrtsd(e.xmm1, src1, src1);
      e.vdivsd(i.dest, e.xmm0, e.xmm1);
    }
  }
};
struct RSQRT_V128 : Sequence<RSQRT_V128, I<OPCODE_RSQRT, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Vmx);
    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm3);
    if (e.IsFeatureEnabled(kX64EmitAVX512Ortho)) {
      e.vrsqrt14ps(i.dest, src1);
    } else {
      e.vmovaps(e.xmm0, e.GetXmmConstPtr(XMMOne));
      e.vsqrtps(e.xmm1, src1);
      e.vdivps(i.dest, e.xmm0, e.xmm1);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_RSQRT, RSQRT_F32, RSQRT_F64, RSQRT_V128);

// ============================================================================
// OPCODE_RECIP
// ============================================================================
// Altivec guarantees an error of < 1/4096 for vrefp while AVX only gives
// < 1.5*2^-12 ≈ 1/2730 for rcpps. This breaks camp, horse and random event
// spawning, breaks cactus collision as well as flickering grass in 5454082B
struct RECIP_F32 : Sequence<RECIP_F32, I<OPCODE_RECIP, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm3);
    if (e.IsFeatureEnabled(kX64EmitAVX512Ortho)) {
      e.vrcp14ss(i.dest, src1, src1);
    } else {
      e.vmovaps(e.xmm0, e.GetXmmConstPtr(XMMOne));
      e.vdivss(i.dest, e.xmm0, src1);
    }
  }
};
struct RECIP_F64 : Sequence<RECIP_F64, I<OPCODE_RECIP, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Fpu);
    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm3);
    if (e.IsFeatureEnabled(kX64EmitAVX512Ortho)) {
      e.vrcp14sd(i.dest, src1, src1);
    } else {
      e.vmovapd(e.xmm0, e.GetXmmConstPtr(XMMOnePD));
      e.vdivsd(i.dest, e.xmm0, src1);
    }
  }
};
struct RECIP_V128 : Sequence<RECIP_V128, I<OPCODE_RECIP, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Vmx);
    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm3);
    if (e.IsFeatureEnabled(kX64EmitAVX512Ortho)) {
      e.vrcp14ps(i.dest, src1);
    } else {
      e.vmovaps(e.xmm0, e.GetXmmConstPtr(XMMOne));
      e.vdivps(i.dest, e.xmm0, src1);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_RECIP, RECIP_F32, RECIP_F64, RECIP_V128);

// ============================================================================
// OPCODE_POW2
// ============================================================================
// TODO(benvanik): use approx here:
//     https://jrfonseca.blogspot.com/2008/09/fast-sse2-pow-tables-or-polynomials.html
struct POW2_F32 : Sequence<POW2_F32, I<OPCODE_POW2, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(POW2_F32);
  }
};
struct POW2_F64 : Sequence<POW2_F64, I<OPCODE_POW2, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(POW2_F64);
  }
};
struct POW2_V128 : Sequence<POW2_V128, I<OPCODE_POW2, V128Op, V128Op>> {
  static __m128 EmulatePow2(void*, __m128 src) {
    alignas(16) float values[4];
    _mm_store_ps(values, src);
    for (size_t i = 0; i < 4; ++i) {
      values[i] = std::exp2(values[i]);
    }
    return _mm_load_ps(values);
  }
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Vmx);
    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);
    e.lea(e.GetNativeParam(0), e.StashXmm(0, src1));

    e.CallNativeSafe(reinterpret_cast<void*>(EmulatePow2));
    e.vmovaps(i.dest, e.xmm0);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_POW2, POW2_F32, POW2_F64, POW2_V128);

// ============================================================================
// OPCODE_LOG2
// ============================================================================
// TODO(benvanik): use approx here:
//     https://jrfonseca.blogspot.com/2008/09/fast-sse2-pow-tables-or-polynomials.html
// TODO(benvanik): this emulated fn destroys all xmm registers! don't do it!
struct LOG2_F32 : Sequence<LOG2_F32, I<OPCODE_LOG2, F32Op, F32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(LOG2_F32);
  }
};
struct LOG2_F64 : Sequence<LOG2_F64, I<OPCODE_LOG2, F64Op, F64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(LOG2_F64);
  }
};
struct LOG2_V128 : Sequence<LOG2_V128, I<OPCODE_LOG2, V128Op, V128Op>> {
  static __m128 EmulateLog2(void*, __m128 src) {
    alignas(16) float values[4];
    _mm_store_ps(values, src);
    for (size_t i = 0; i < 4; ++i) {
      values[i] = std::log2(values[i]);
    }
    return _mm_load_ps(values);
  }
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Vmx);
    Xmm src1 = GetInputRegOrConstant(e, i.src1, e.xmm0);

    e.lea(e.GetNativeParam(0), e.StashXmm(0, src1));

    e.CallNativeSafe(reinterpret_cast<void*>(EmulateLog2));
    e.vmovaps(i.dest, e.xmm0);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOG2, LOG2_F32, LOG2_F64, LOG2_V128);

// ============================================================================
// OPCODE_DOT_PRODUCT_3
// ============================================================================
struct DOT_PRODUCT_3_V128
    : Sequence<DOT_PRODUCT_3_V128,
               I<OPCODE_DOT_PRODUCT_3, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Vmx);
    // todo: add fast_dot_product path that just checks for infinity instead of
    // using mxcsr
    auto mxcsr_storage = e.dword[e.rsp + StackLayout::GUEST_SCRATCH];

    // this is going to hurt a bit...
    /*
    this implementation is accurate, it matches the results of xb360 vmsum3
    except that vmsum3 is often off by 1 bit, but its extremely slow. it is a
    long, unbroken chain of dependencies, and the three uses of mxcsr all cost
    about 15-20 cycles at the very least on amd zen processors. on older amd the
    figures agner has are pretty horrible. it looks like its just as bad on
    modern intel cpus also up until just recently. perhaps a better way of
    detecting overflow would be to just compare with inf. todo: test whether cmp
    with inf can replace
    */
    if (!cvars::use_fast_dot_product) {
      e.vstmxcsr(mxcsr_storage);
      e.mov(e.eax, 8);
    }
    e.vmovaps(e.xmm2, e.GetXmmConstPtr(XMMThreeFloatMask));
    bool is_lensqr = i.instr->src1.value == i.instr->src2.value;

    auto src1v = e.xmm0;
    auto src2v = e.xmm1;
    if (i.src1.is_constant) {
      src1v = e.xmm0;
      e.LoadConstantXmm(src1v, i.src1.constant());
    } else {
      src1v = i.src1.reg();
    }
    if (i.src2.is_constant) {
      src2v = e.xmm1;
      e.LoadConstantXmm(src2v, i.src2.constant());
    } else {
      src2v = i.src2.reg();
    }
    if (!cvars::use_fast_dot_product) {
      e.not_(e.eax);
    }
    // todo: maybe the top element should be cleared by the InstrEmit_ function
    // so that in the future this could be optimized away if the top is known to
    // be zero. Right now im not sure that happens often though and its
    // currently not worth it also, maybe pre-and if constant
    if (!is_lensqr) {
      e.vandps(e.xmm3, src1v, e.xmm2);

      e.vandps(e.xmm2, src2v, e.xmm2);

      if (!cvars::use_fast_dot_product) {
        e.and_(mxcsr_storage, e.eax);
        e.vldmxcsr(mxcsr_storage);  // overflow flag is cleared, now we're good
                                    // to go
      }
      e.vcvtps2pd(e.ymm0, e.xmm3);
      e.vcvtps2pd(e.ymm1, e.xmm2);

      /*
          ymm0 = src1 as doubles, ele 3 cleared
          ymm1 = src2 as doubles, ele 3 cleared
      */
      e.vmulpd(e.ymm3, e.ymm0, e.ymm1);
    } else {
      e.vandps(e.xmm3, src1v, e.xmm2);
      if (!cvars::use_fast_dot_product) {
        e.and_(mxcsr_storage, e.eax);
        e.vldmxcsr(mxcsr_storage);  // overflow flag is cleared, now we're good
                                    // to go
      }
      e.vcvtps2pd(e.ymm0, e.xmm3);
      e.vmulpd(e.ymm3, e.ymm0, e.ymm0);
    }
    e.vextractf128(e.xmm2, e.ymm3, 1);
    e.vunpckhpd(e.xmm0, e.xmm3, e.xmm3);  // get element [1] in xmm3
    e.vaddsd(e.xmm3, e.xmm3, e.xmm2);
    if (!cvars::use_fast_dot_product) {
      e.not_(e.eax);
    }
    e.vaddsd(e.xmm2, e.xmm3, e.xmm0);
    e.vcvtsd2ss(e.xmm1, e.xmm2);

    if (!cvars::use_fast_dot_product) {
      e.vstmxcsr(mxcsr_storage);

      e.test(mxcsr_storage, e.eax);

      Xbyak::Label& done = e.NewCachedLabel();
      Xbyak::Label& ret_qnan =
          e.AddToTail([i, &done](X64Emitter& e, Xbyak::Label& me) {
            e.L(me);
            e.vmovaps(i.dest, e.GetXmmConstPtr(XMMQNaN));
            e.jmp(done, X64Emitter::T_NEAR);
          });

      e.jnz(ret_qnan, X64Emitter::T_NEAR);  // reorder these jmps later, just
                                            // want to get this fix in
      e.vshufps(i.dest, e.xmm1, e.xmm1, 0);
      e.L(done);
    } else {
      e.vandps(e.xmm0, e.xmm1, e.GetXmmConstPtr(XMMAbsMaskPS));

      e.vcmpgeps(e.xmm2, e.xmm0, e.GetXmmConstPtr(XMMFloatInf));
      e.vblendvps(e.xmm1, e.xmm1, e.GetXmmConstPtr(XMMQNaN), e.xmm2);
      e.vshufps(i.dest, e.xmm1, e.xmm1, 0);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DOT_PRODUCT_3, DOT_PRODUCT_3_V128);

// ============================================================================
// OPCODE_DOT_PRODUCT_4
// ============================================================================
struct DOT_PRODUCT_4_V128
    : Sequence<DOT_PRODUCT_4_V128,
               I<OPCODE_DOT_PRODUCT_4, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    e.ChangeMxcsrMode(MXCSRMode::Vmx);
    // todo: add fast_dot_product path that just checks for infinity instead of
    // using mxcsr
    auto mxcsr_storage = e.dword[e.rsp + StackLayout::GUEST_SCRATCH];

    bool is_lensqr = i.instr->src1.value == i.instr->src2.value;

    auto src1v = e.xmm3;
    auto src2v = e.xmm2;
    if (i.src1.is_constant) {
      src1v = e.xmm3;
      e.LoadConstantXmm(src1v, i.src1.constant());
    } else {
      src1v = i.src1.reg();
    }
    if (i.src2.is_constant) {
      src2v = e.xmm2;
      e.LoadConstantXmm(src2v, i.src2.constant());
    } else {
      src2v = i.src2.reg();
    }
    if (!cvars::use_fast_dot_product) {
      e.vstmxcsr(mxcsr_storage);

      e.mov(e.eax, 8);
      e.not_(e.eax);

      e.and_(mxcsr_storage, e.eax);
      e.vldmxcsr(mxcsr_storage);
    }
    if (is_lensqr) {
      e.vcvtps2pd(e.ymm0, src1v);

      e.vmulpd(e.ymm3, e.ymm0, e.ymm0);
    } else {
      e.vcvtps2pd(e.ymm0, src1v);
      e.vcvtps2pd(e.ymm1, src2v);

      e.vmulpd(e.ymm3, e.ymm0, e.ymm1);
    }
    e.vextractf128(e.xmm2, e.ymm3, 1);
    e.vaddpd(e.xmm3, e.xmm3, e.xmm2);

    e.vunpckhpd(e.xmm0, e.xmm3, e.xmm3);
    if (!cvars::use_fast_dot_product) {
      e.not_(e.eax);
    }
    e.vaddsd(e.xmm2, e.xmm3, e.xmm0);
    e.vcvtsd2ss(e.xmm1, e.xmm2);

    if (!cvars::use_fast_dot_product) {
      e.vstmxcsr(mxcsr_storage);

      e.test(mxcsr_storage, e.eax);

      Xbyak::Label& done = e.NewCachedLabel();
      Xbyak::Label& ret_qnan =
          e.AddToTail([i, &done](X64Emitter& e, Xbyak::Label& me) {
            e.L(me);
            e.vmovaps(i.dest, e.GetXmmConstPtr(XMMQNaN));
            e.jmp(done, X64Emitter::T_NEAR);
          });

      e.jnz(ret_qnan, X64Emitter::T_NEAR);  // reorder these jmps later, just
                                            // want to get this fix in
      e.vshufps(i.dest, e.xmm1, e.xmm1, 0);
      e.L(done);
    } else {
      e.vandps(e.xmm0, e.xmm1, e.GetXmmConstPtr(XMMAbsMaskPS));

      e.vcmpgeps(e.xmm2, e.xmm0, e.GetXmmConstPtr(XMMFloatInf));
      e.vblendvps(e.xmm1, e.xmm1, e.GetXmmConstPtr(XMMQNaN), e.xmm2);
      e.vshufps(i.dest, e.xmm1, e.xmm1, 0);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DOT_PRODUCT_4, DOT_PRODUCT_4_V128);

// ============================================================================
// OPCODE_AND
// ============================================================================
// TODO(benvanik): put dest/src1|2 together.
template <typename SEQ, typename REG, typename ARGS>
void EmitAndXX(X64Emitter& e, const ARGS& i) {
  SEQ::EmitCommutativeBinaryOp(
      e, i,
      [](X64Emitter& e, const REG& dest_src, const REG& src) {
        e.and_(dest_src, src);
      },
      [](X64Emitter& e, const REG& dest_src, int32_t constant) {
        if (constant == 0xFF) {
          if (dest_src.getBit() == 16 || dest_src.getBit() == 32) {
            e.movzx(dest_src, dest_src.cvt8());
            return;
          } else if (dest_src.getBit() == 64) {
            // take advantage of automatic zeroing of upper 32 bits
            e.movzx(dest_src.cvt32(), dest_src.cvt8());
            return;
          }
        } else if (constant == 0xFFFF) {
          if (dest_src.getBit() == 32) {
            e.movzx(dest_src, dest_src.cvt16());
            return;
          } else if (dest_src.getBit() == 64) {
            e.movzx(dest_src.cvt32(), dest_src.cvt16());
            return;
          }
        } else if (constant == -1) {
          if (dest_src.getBit() == 64) {
            // todo: verify that mov eax, eax will properly zero upper 64 bits
          }
        } else if (dest_src.getBit() == 64 && constant > 0) {
          // do 32 bit and, not the full 64, because the upper 32 of the mask
          // are zero and the 32 bit op will auto clear the top, save space on
          // the immediate and avoid a rex prefix
          e.and_(dest_src.cvt32(), constant);
          return;
        }
        e.and_(dest_src, constant);
      });
}
struct AND_I8 : Sequence<AND_I8, I<OPCODE_AND, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitAndXX<AND_I8, Reg8>(e, i);
  }
};
struct AND_I16 : Sequence<AND_I16, I<OPCODE_AND, I16Op, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitAndXX<AND_I16, Reg16>(e, i);
  }
};
struct AND_I32 : Sequence<AND_I32, I<OPCODE_AND, I32Op, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitAndXX<AND_I32, Reg32>(e, i);
  }
};
struct AND_I64 : Sequence<AND_I64, I<OPCODE_AND, I64Op, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant && i.src2.constant() == 0xFFFFFFFF) {
      // special case for rlwinm codegen
      e.mov(((Reg64)i.dest).cvt32(), ((Reg64)i.src1).cvt32());
    } else {
      EmitAndXX<AND_I64, Reg64>(e, i);
    }
  }
};
struct AND_V128 : Sequence<AND_V128, I<OPCODE_AND, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    SimdDomain dom = PickDomain2(e.DeduceSimdDomain(i.src1.value),
                                 e.DeduceSimdDomain(i.src2.value));

    EmitCommutativeBinaryXmmOp(
        e, i, [dom](X64Emitter& e, Xmm dest, Xmm src1, Xmm src2) {
          if (dom == SimdDomain::FLOATING) {
            e.vandps(dest, src2, src1);
          } else {
            e.vpand(dest, src2, src1);
          }
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_AND, AND_I8, AND_I16, AND_I32, AND_I64, AND_V128);

// ============================================================================
// OPCODE_AND_NOT
// ============================================================================
template <typename SEQ, typename REG, typename ARGS>
void EmitAndNotXX(X64Emitter& e, const ARGS& i) {
  if (i.src1.is_constant) {
    // src1 constant.
    // `and` instruction only supports up to 32-bit immediate constants
    // 64-bit constants will need a temp register
    if (i.dest.reg().getBit() == 64) {
      auto temp = GetTempReg<typename decltype(i.src1)::reg_type>(e);
      e.mov(temp, i.src1.constant());

      if (e.IsFeatureEnabled(kX64EmitBMI1)) {
        if (i.dest.reg().getBit() == 64) {
          e.andn(i.dest.reg().cvt64(), i.src2.reg().cvt64(), temp.cvt64());
        } else {
          e.andn(i.dest.reg().cvt32(), i.src2.reg().cvt32(), temp.cvt32());
        }
      } else {
        e.mov(i.dest, i.src2);
        e.not_(i.dest);
        e.and_(i.dest, temp);
      }
    } else {
      e.mov(i.dest, i.src2);
      e.not_(i.dest);
      e.and_(i.dest, uint32_t(i.src1.constant()));
    }
  } else if (i.src2.is_constant) {
    // src2 constant.
    if (i.dest == i.src1) {
      auto temp = GetTempReg<typename decltype(i.src2)::reg_type>(e);
      e.mov(temp, ~i.src2.constant());
      e.and_(i.dest, temp);
    } else {
      e.mov(i.dest, i.src1);
      auto temp = GetTempReg<typename decltype(i.src2)::reg_type>(e);
      e.mov(temp, ~i.src2.constant());
      e.and_(i.dest, temp);
    }
  } else {
    // neither are constant
    if (e.IsFeatureEnabled(kX64EmitBMI1)) {
      if (i.dest.reg().getBit() == 64) {
        e.andn(i.dest.reg().cvt64(), i.src2.reg().cvt64(),
               i.src1.reg().cvt64());
      } else {
        e.andn(i.dest.reg().cvt32(), i.src2.reg().cvt32(),
               i.src1.reg().cvt32());
      }
    } else {
      if (i.dest == i.src2) {
        e.not_(i.dest);
        e.and_(i.dest, i.src1);
      } else if (i.dest == i.src1) {
        auto temp = GetTempReg<typename decltype(i.dest)::reg_type>(e);
        e.mov(temp, i.src2);
        e.not_(temp);
        e.and_(i.dest, temp);
      } else {
        e.mov(i.dest, i.src2);
        e.not_(i.dest);
        e.and_(i.dest, i.src1);
      }
    }
  }
}
struct AND_NOT_I8 : Sequence<AND_NOT_I8, I<OPCODE_AND_NOT, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitAndNotXX<AND_NOT_I8, Reg8>(e, i);
  }
};
struct AND_NOT_I16
    : Sequence<AND_NOT_I16, I<OPCODE_AND_NOT, I16Op, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitAndNotXX<AND_NOT_I16, Reg16>(e, i);
  }
};
struct AND_NOT_I32
    : Sequence<AND_NOT_I32, I<OPCODE_AND_NOT, I32Op, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitAndNotXX<AND_NOT_I32, Reg32>(e, i);
  }
};
struct AND_NOT_I64
    : Sequence<AND_NOT_I64, I<OPCODE_AND_NOT, I64Op, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitAndNotXX<AND_NOT_I64, Reg64>(e, i);
  }
};
struct AND_NOT_V128
    : Sequence<AND_NOT_V128, I<OPCODE_AND_NOT, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    SimdDomain dom = PickDomain2(e.DeduceSimdDomain(i.src1.value),
                                 e.DeduceSimdDomain(i.src2.value));

    EmitCommutativeBinaryXmmOp(
        e, i, [dom](X64Emitter& e, Xmm dest, Xmm src1, Xmm src2) {
          if (dom == SimdDomain::FLOATING) {
            e.vandnps(dest, src2, src1);
          } else {
            e.vpandn(dest, src2, src1);
          }
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_AND_NOT, AND_NOT_I8, AND_NOT_I16, AND_NOT_I32,
                     AND_NOT_I64, AND_NOT_V128);

// ============================================================================
// OPCODE_OR
// ============================================================================
// TODO(benvanik): put dest/src1|2 together.
template <typename SEQ, typename REG, typename ARGS>
void EmitOrXX(X64Emitter& e, const ARGS& i) {
  SEQ::EmitCommutativeBinaryOp(
      e, i,
      [](X64Emitter& e, const REG& dest_src, const REG& src) {
        e.or_(dest_src, src);
      },
      [](X64Emitter& e, const REG& dest_src, int32_t constant) {
        e.or_(dest_src, constant);
      });
}
struct OR_I8 : Sequence<OR_I8, I<OPCODE_OR, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitOrXX<OR_I8, Reg8>(e, i);
  }
};
struct OR_I16 : Sequence<OR_I16, I<OPCODE_OR, I16Op, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitOrXX<OR_I16, Reg16>(e, i);
  }
};
struct OR_I32 : Sequence<OR_I32, I<OPCODE_OR, I32Op, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitOrXX<OR_I32, Reg32>(e, i);
  }
};
struct OR_I64 : Sequence<OR_I64, I<OPCODE_OR, I64Op, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitOrXX<OR_I64, Reg64>(e, i);
  }
};
struct OR_V128 : Sequence<OR_V128, I<OPCODE_OR, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    SimdDomain dom = PickDomain2(e.DeduceSimdDomain(i.src1.value),
                                 e.DeduceSimdDomain(i.src2.value));

    EmitCommutativeBinaryXmmOp(
        e, i, [dom](X64Emitter& e, Xmm dest, Xmm src1, Xmm src2) {
          if (dom == SimdDomain::FLOATING) {
            e.vorps(dest, src1, src2);
          } else {
            e.vpor(dest, src1, src2);
          }
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_OR, OR_I8, OR_I16, OR_I32, OR_I64, OR_V128);

// ============================================================================
// OPCODE_XOR
// ============================================================================
// TODO(benvanik): put dest/src1|2 together.
template <typename SEQ, typename REG, typename ARGS>
void EmitXorXX(X64Emitter& e, const ARGS& i) {
  SEQ::EmitCommutativeBinaryOp(
      e, i,
      [](X64Emitter& e, const REG& dest_src, const REG& src) {
        e.xor_(dest_src, src);
      },
      [](X64Emitter& e, const REG& dest_src, int32_t constant) {
        e.xor_(dest_src, constant);
      });
}
struct XOR_I8 : Sequence<XOR_I8, I<OPCODE_XOR, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitXorXX<XOR_I8, Reg8>(e, i);
  }
};
struct XOR_I16 : Sequence<XOR_I16, I<OPCODE_XOR, I16Op, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitXorXX<XOR_I16, Reg16>(e, i);
  }
};
struct XOR_I32 : Sequence<XOR_I32, I<OPCODE_XOR, I32Op, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitXorXX<XOR_I32, Reg32>(e, i);
  }
};
struct XOR_I64 : Sequence<XOR_I64, I<OPCODE_XOR, I64Op, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitXorXX<XOR_I64, Reg64>(e, i);
  }
};
struct XOR_V128 : Sequence<XOR_V128, I<OPCODE_XOR, V128Op, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    SimdDomain dom = PickDomain2(e.DeduceSimdDomain(i.src1.value),
                                 e.DeduceSimdDomain(i.src2.value));

    EmitCommutativeBinaryXmmOp(
        e, i, [dom](X64Emitter& e, Xmm dest, Xmm src1, Xmm src2) {
          if (dom == SimdDomain::FLOATING) {
            e.vxorps(dest, src1, src2);
          } else {
            e.vpxor(dest, src1, src2);
          }
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_XOR, XOR_I8, XOR_I16, XOR_I32, XOR_I64, XOR_V128);

// ============================================================================
// OPCODE_NOT
// ============================================================================
// TODO(benvanik): put dest/src1 together.
template <typename SEQ, typename REG, typename ARGS>
void EmitNotXX(X64Emitter& e, const ARGS& i) {
  SEQ::EmitUnaryOp(
      e, i, [](X64Emitter& e, const REG& dest_src) { e.not_(dest_src); });
}
struct NOT_I8 : Sequence<NOT_I8, I<OPCODE_NOT, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitNotXX<NOT_I8, Reg8>(e, i);
  }
};
struct NOT_I16 : Sequence<NOT_I16, I<OPCODE_NOT, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitNotXX<NOT_I16, Reg16>(e, i);
  }
};
struct NOT_I32 : Sequence<NOT_I32, I<OPCODE_NOT, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitNotXX<NOT_I32, Reg32>(e, i);
  }
};
struct NOT_I64 : Sequence<NOT_I64, I<OPCODE_NOT, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitNotXX<NOT_I64, Reg64>(e, i);
  }
};
struct NOT_V128 : Sequence<NOT_V128, I<OPCODE_NOT, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    SimdDomain domain = e.DeduceSimdDomain(i.src1.value);
    if (domain == SimdDomain::FLOATING) {
      e.vxorps(i.dest, i.src1, e.GetXmmConstPtr(XMMFFFF /* FF... */));
    } else {
      // dest = src ^ 0xFFFF...
      e.vpxor(i.dest, i.src1, e.GetXmmConstPtr(XMMFFFF /* FF... */));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_NOT, NOT_I8, NOT_I16, NOT_I32, NOT_I64, NOT_V128);

// ============================================================================
// OPCODE_SHL
// ============================================================================
// TODO(benvanik): optimize common shifts.
template <typename SEQ, typename REG, typename ARGS>
void EmitShlXX(X64Emitter& e, const ARGS& i) {
  SEQ::EmitAssociativeBinaryOp(
      e, i,
      [](X64Emitter& e, const REG& dest_src, const Reg8& src) {
        // shlx: $1 = $2 << $3
        // shl: $1 = $1 << $2
        if (e.IsFeatureEnabled(kX64EmitBMI2)) {
          if (dest_src.getBit() == 64) {
            e.shlx(dest_src.cvt64(), dest_src.cvt64(), src.cvt64());
          } else {
            e.shlx(dest_src.cvt32(), dest_src.cvt32(), src.cvt32());
          }
        } else {
          e.mov(e.cl, src);
          e.shl(dest_src, e.cl);
        }
      },
      [](X64Emitter& e, const REG& dest_src, int8_t constant) {
        e.shl(dest_src, constant);
      });
}
struct SHL_I8 : Sequence<SHL_I8, I<OPCODE_SHL, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitShlXX<SHL_I8, Reg8>(e, i);
  }
};
struct SHL_I16 : Sequence<SHL_I16, I<OPCODE_SHL, I16Op, I16Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitShlXX<SHL_I16, Reg16>(e, i);
  }
};
struct SHL_I32 : Sequence<SHL_I32, I<OPCODE_SHL, I32Op, I32Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitShlXX<SHL_I32, Reg32>(e, i);
  }
};
struct SHL_I64 : Sequence<SHL_I64, I<OPCODE_SHL, I64Op, I64Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitShlXX<SHL_I64, Reg64>(e, i);
  }
};
struct SHL_V128 : Sequence<SHL_V128, I<OPCODE_SHL, V128Op, V128Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    // TODO(benvanik): native version (with shift magic).

    auto src1 = GetInputRegOrConstant(e, i.src1, e.xmm3);

    if (i.src2.is_constant) {
      e.mov(e.GetNativeParam(1), i.src2.constant());
    } else {
      e.mov(e.GetNativeParam(1), i.src2);
    }
    e.lea(e.GetNativeParam(0), e.StashXmm(0, src1));
    e.CallNativeSafe(reinterpret_cast<void*>(EmulateShlV128));
    e.vmovaps(i.dest, e.xmm0);
  }
  static __m128i EmulateShlV128(void*, __m128i src1, uint8_t src2) {
    // Almost all instances are shamt = 1, but non-constant.
    // shamt is [0,7]
    uint8_t shamt = src2 & 0x7;
    alignas(16) vec128_t value;
    _mm_store_si128(reinterpret_cast<__m128i*>(&value), src1);
    for (int i = 0; i < 15; ++i) {
      value.u8[i ^ 0x3] = (value.u8[i ^ 0x3] << shamt) |
                          (value.u8[(i + 1) ^ 0x3] >> (8 - shamt));
    }
    value.u8[15 ^ 0x3] = value.u8[15 ^ 0x3] << shamt;
    return _mm_load_si128(reinterpret_cast<__m128i*>(&value));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SHL, SHL_I8, SHL_I16, SHL_I32, SHL_I64, SHL_V128);

// ============================================================================
// OPCODE_SHR
// ============================================================================
// TODO(benvanik): optimize common shifts.
template <typename SEQ, typename REG, typename ARGS>
void EmitShrXX(X64Emitter& e, const ARGS& i) {
  SEQ::EmitAssociativeBinaryOp(
      e, i,
      [](X64Emitter& e, const REG& dest_src, const Reg8& src) {
        // shrx: op1 dest, op2 src, op3 count
        // shr: op1 src/dest, op2 count
        if (e.IsFeatureEnabled(kX64EmitBMI2)) {
          if (dest_src.getBit() == 64) {
            e.shrx(dest_src.cvt64(), dest_src.cvt64(), src.cvt64());
          } else if (dest_src.getBit() == 32) {
            e.shrx(dest_src.cvt32(), dest_src.cvt32(), src.cvt32());
          } else {
            e.movzx(dest_src.cvt32(), dest_src);
            e.shrx(dest_src.cvt32(), dest_src.cvt32(), src.cvt32());
          }
        } else {
          e.mov(e.cl, src);
          e.shr(dest_src, e.cl);
        }
      },
      [](X64Emitter& e, const REG& dest_src, int8_t constant) {
        e.shr(dest_src, constant);
      });
}
struct SHR_I8 : Sequence<SHR_I8, I<OPCODE_SHR, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitShrXX<SHR_I8, Reg8>(e, i);
  }
};
struct SHR_I16 : Sequence<SHR_I16, I<OPCODE_SHR, I16Op, I16Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitShrXX<SHR_I16, Reg16>(e, i);
  }
};
struct SHR_I32 : Sequence<SHR_I32, I<OPCODE_SHR, I32Op, I32Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitShrXX<SHR_I32, Reg32>(e, i);
  }
};
struct SHR_I64 : Sequence<SHR_I64, I<OPCODE_SHR, I64Op, I64Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitShrXX<SHR_I64, Reg64>(e, i);
  }
};
struct SHR_V128 : Sequence<SHR_V128, I<OPCODE_SHR, V128Op, V128Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    // TODO(benvanik): native version (with shift magic).
    if (i.src2.is_constant) {
      e.mov(e.GetNativeParam(1), i.src2.constant());
    } else {
      e.mov(e.GetNativeParam(1), i.src2);
    }
    e.lea(e.GetNativeParam(0), e.StashXmm(0, i.src1));
    e.CallNativeSafe(reinterpret_cast<void*>(EmulateShrV128));
    e.vmovaps(i.dest, e.xmm0);
  }
  static __m128i EmulateShrV128(void*, __m128i src1, uint8_t src2) {
    // Almost all instances are shamt = 1, but non-constant.
    // shamt is [0,7]
    uint8_t shamt = src2 & 0x7;
    alignas(16) vec128_t value;
    _mm_store_si128(reinterpret_cast<__m128i*>(&value), src1);
    for (int i = 15; i > 0; --i) {
      value.u8[i ^ 0x3] = (value.u8[i ^ 0x3] >> shamt) |
                          (value.u8[(i - 1) ^ 0x3] << (8 - shamt));
    }
    value.u8[0 ^ 0x3] = value.u8[0 ^ 0x3] >> shamt;
    return _mm_load_si128(reinterpret_cast<__m128i*>(&value));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SHR, SHR_I8, SHR_I16, SHR_I32, SHR_I64, SHR_V128);

// ============================================================================
// OPCODE_SHA
// ============================================================================
// TODO(benvanik): optimize common shifts.
template <typename SEQ, typename REG, typename ARGS>
void EmitSarXX(X64Emitter& e, const ARGS& i) {
  SEQ::EmitAssociativeBinaryOp(
      e, i,
      [](X64Emitter& e, const REG& dest_src, const Reg8& src) {
        if (e.IsFeatureEnabled(kX64EmitBMI2)) {
          if (dest_src.getBit() == 64) {
            e.sarx(dest_src.cvt64(), dest_src.cvt64(), src.cvt64());
          } else if (dest_src.getBit() == 32) {
            e.sarx(dest_src.cvt32(), dest_src.cvt32(), src.cvt32());
          } else {
            e.movsx(dest_src.cvt32(), dest_src);
            e.sarx(dest_src.cvt32(), dest_src.cvt32(), src.cvt32());
          }
        } else {
          e.mov(e.cl, src);
          e.sar(dest_src, e.cl);
        }
      },
      [](X64Emitter& e, const REG& dest_src, int8_t constant) {
        e.sar(dest_src, constant);
      });
}
struct SHA_I8 : Sequence<SHA_I8, I<OPCODE_SHA, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitSarXX<SHA_I8, Reg8>(e, i);
  }
};
struct SHA_I16 : Sequence<SHA_I16, I<OPCODE_SHA, I16Op, I16Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitSarXX<SHA_I16, Reg16>(e, i);
  }
};
struct SHA_I32 : Sequence<SHA_I32, I<OPCODE_SHA, I32Op, I32Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitSarXX<SHA_I32, Reg32>(e, i);
  }
};
struct SHA_I64 : Sequence<SHA_I64, I<OPCODE_SHA, I64Op, I64Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitSarXX<SHA_I64, Reg64>(e, i);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SHA, SHA_I8, SHA_I16, SHA_I32, SHA_I64);

// ============================================================================
// OPCODE_ROTATE_LEFT
// ============================================================================
// TODO(benvanik): put dest/src1 together, src2 in cl.
template <typename SEQ, typename REG, typename ARGS>
void EmitRotateLeftXX(X64Emitter& e, const ARGS& i) {
  if (i.src2.is_constant) {
    // Constant rotate.
    if (i.dest != i.src1) {
      if (i.src1.is_constant) {
        e.mov(i.dest, i.src1.constant());
      } else {
        e.mov(i.dest, i.src1);
      }
    }
    e.rol(i.dest, i.src2.constant());
  } else {
    // Variable rotate.
    if (i.src2.reg().getIdx() != e.cl.getIdx()) {
      e.mov(e.cl, i.src2);
    }
    if (i.dest != i.src1) {
      if (i.src1.is_constant) {
        e.mov(i.dest, i.src1.constant());
      } else {
        e.mov(i.dest, i.src1);
      }
    }
    e.rol(i.dest, e.cl);
  }
}
struct ROTATE_LEFT_I8
    : Sequence<ROTATE_LEFT_I8, I<OPCODE_ROTATE_LEFT, I8Op, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitRotateLeftXX<ROTATE_LEFT_I8, Reg8>(e, i);
  }
};
struct ROTATE_LEFT_I16
    : Sequence<ROTATE_LEFT_I16, I<OPCODE_ROTATE_LEFT, I16Op, I16Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitRotateLeftXX<ROTATE_LEFT_I16, Reg16>(e, i);
  }
};
struct ROTATE_LEFT_I32
    : Sequence<ROTATE_LEFT_I32, I<OPCODE_ROTATE_LEFT, I32Op, I32Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitRotateLeftXX<ROTATE_LEFT_I32, Reg32>(e, i);
  }
};
struct ROTATE_LEFT_I64
    : Sequence<ROTATE_LEFT_I64, I<OPCODE_ROTATE_LEFT, I64Op, I64Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitRotateLeftXX<ROTATE_LEFT_I64, Reg64>(e, i);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ROTATE_LEFT, ROTATE_LEFT_I8, ROTATE_LEFT_I16,
                     ROTATE_LEFT_I32, ROTATE_LEFT_I64);

// ============================================================================
// OPCODE_BYTE_SWAP
// ============================================================================
// TODO(benvanik): put dest/src1 together.
struct BYTE_SWAP_I16
    : Sequence<BYTE_SWAP_I16, I<OPCODE_BYTE_SWAP, I16Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitUnaryOp(
        e, i, [](X64Emitter& e, const Reg16& dest_src) { e.ror(dest_src, 8); });
  }
};
struct BYTE_SWAP_I32
    : Sequence<BYTE_SWAP_I32, I<OPCODE_BYTE_SWAP, I32Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitUnaryOp(
        e, i, [](X64Emitter& e, const Reg32& dest_src) { e.bswap(dest_src); });
  }
};
struct BYTE_SWAP_I64
    : Sequence<BYTE_SWAP_I64, I<OPCODE_BYTE_SWAP, I64Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    EmitUnaryOp(
        e, i, [](X64Emitter& e, const Reg64& dest_src) { e.bswap(dest_src); });
  }
};
struct BYTE_SWAP_V128
    : Sequence<BYTE_SWAP_V128, I<OPCODE_BYTE_SWAP, V128Op, V128Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    // TODO(benvanik): find a way to do this without the memory load.
    e.vpshufb(i.dest, i.src1, e.GetXmmConstPtr(XMMByteSwapMask));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_BYTE_SWAP, BYTE_SWAP_I16, BYTE_SWAP_I32,
                     BYTE_SWAP_I64, BYTE_SWAP_V128);

// ============================================================================
// OPCODE_CNTLZ
// Count leading zeroes
// ============================================================================
struct CNTLZ_I8 : Sequence<CNTLZ_I8, I<OPCODE_CNTLZ, I8Op, I8Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(CNTLZ_I8);
  }
};
struct CNTLZ_I16 : Sequence<CNTLZ_I16, I<OPCODE_CNTLZ, I8Op, I16Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    assert_impossible_sequence(CNTLZ_I16);
  }
};
struct CNTLZ_I32 : Sequence<CNTLZ_I32, I<OPCODE_CNTLZ, I8Op, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    if (e.IsFeatureEnabled(kX64EmitLZCNT)) {
      e.lzcnt(i.dest.reg().cvt32(), i.src1);
    } else {
      Xbyak::Label end;
      e.inLocalLabel();

      e.bsr(e.rax, i.src1);  // ZF set if i.src1 is 0
      e.mov(i.dest, 0x20);
      e.jz(end);

      e.xor_(e.rax, 0x1F);
      e.mov(i.dest, e.rax);

      e.L(end);
      e.outLocalLabel();
    }
  }
};
struct CNTLZ_I64 : Sequence<CNTLZ_I64, I<OPCODE_CNTLZ, I8Op, I64Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    if (e.IsFeatureEnabled(kX64EmitLZCNT)) {
      e.lzcnt(i.dest.reg().cvt64(), i.src1);
    } else {
      Xbyak::Label end;
      e.inLocalLabel();

      e.bsr(e.rax, i.src1);  // ZF set if i.src1 is 0
      e.mov(i.dest, 0x40);
      e.jz(end);

      e.xor_(e.rax, 0x3F);
      e.mov(i.dest, e.rax);

      e.L(end);
      e.outLocalLabel();
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CNTLZ, CNTLZ_I8, CNTLZ_I16, CNTLZ_I32, CNTLZ_I64);

// ============================================================================
// OPCODE_SET_ROUNDING_MODE
// ============================================================================
// Input: FPSCR (PPC format)

struct SET_ROUNDING_MODE_I32
    : Sequence<SET_ROUNDING_MODE_I32,
               I<OPCODE_SET_ROUNDING_MODE, VoidOp, I32Op>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    // removed the And with 7 and hoisted that and into the InstrEmit_'s that
    // generate OPCODE_SET_ROUNDING_MODE so that it can be constant folded and
    // backends dont have to worry about it
    if (i.src1.is_constant) {
      e.mov(e.eax, mxcsr_table[i.src1.constant()]);
      e.mov(e.dword[e.rsp + StackLayout::GUEST_SCRATCH], e.eax);
      e.mov(e.GetBackendCtxPtr(offsetof(X64BackendContext, mxcsr_fpu)), e.eax);
      e.vldmxcsr(e.dword[e.rsp + StackLayout::GUEST_SCRATCH]);

    } else {
      e.mov(e.ecx, i.src1);

      e.mov(e.rax, uintptr_t(mxcsr_table));
      e.mov(e.edx, e.ptr[e.rax + e.rcx * 4]);
      // this was not here
      e.mov(e.GetBackendCtxPtr(offsetof(X64BackendContext, mxcsr_fpu)), e.edx);

      e.vldmxcsr(e.GetBackendCtxPtr(offsetof(X64BackendContext, mxcsr_fpu)));
    }
    e.ChangeMxcsrMode(MXCSRMode::Fpu, true);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SET_ROUNDING_MODE, SET_ROUNDING_MODE_I32);
// ============================================================================
// OPCODE_DELAY_EXECUTION
// ============================================================================
struct DELAY_EXECUTION
    : Sequence<DELAY_EXECUTION, I<OPCODE_DELAY_EXECUTION, VoidOp>> {
  static void Emit(X64Emitter& e, const EmitArgType& i) {
    // todo: what if they dont have smt?
    e.pause();
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DELAY_EXECUTION, DELAY_EXECUTION);
// Include anchors to other sequence sources so they get included in the build.
extern volatile int anchor_control;
static int anchor_control_dest = anchor_control;

extern volatile int anchor_memory;
static int anchor_memory_dest = anchor_memory;

extern volatile int anchor_vector;
static int anchor_vector_dest = anchor_vector;

bool SelectSequence(X64Emitter* e, const Instr* i, const Instr** new_tail) {
  if ((i->backend_flags & INSTR_X64_FLAGS_ELIMINATED) != 0) {
    // skip
    *new_tail = i->next;
    return true;
  } else {
    const InstrKey key(i);

    auto it = sequence_table.find(key);
    if (it != sequence_table.end()) {
      if (it->second(*e, i, InstrKey(i))) {
        *new_tail = i->next;
        return true;
      }
    }
    XELOGE("No sequence match for variant {}", GetOpcodeName(i->opcode));
    return false;
  }
}

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
