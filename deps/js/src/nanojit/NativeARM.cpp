/* -*- Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil; tab-width: 4 -*- */
/* vi: set ts=4 sw=4 expandtab: (add to ~/.vimrc: set modeline modelines=5) */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is [Open Source Virtual Machine].
 *
 * The Initial Developer of the Original Code is
 * Adobe System Incorporated.
 * Portions created by the Initial Developer are Copyright (C) 2004-2007
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Adobe AS3 Team
 *   Vladimir Vukicevic <vladimir@pobox.com>
 *   Jacob Bramley <Jacob.Bramley@arm.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "nanojit.h"

#ifdef UNDER_CE
#include <cmnintrin.h>
extern "C" bool blx_lr_broken();
#endif

#if defined(FEATURE_NANOJIT) && defined(NANOJIT_ARM)

namespace nanojit
{

#ifdef NJ_VERBOSE
const char* regNames[] = {"r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r10","fp","ip","sp","lr","pc",
                          "d0","d1","d2","d3","d4","d5","d6","d7","s14"};
const char* condNames[] = {"eq","ne","cs","cc","mi","pl","vs","vc","hi","ls","ge","lt","gt","le",""/*al*/,"nv"};
const char* shiftNames[] = { "lsl", "lsl", "lsr", "lsr", "asr", "asr", "ror", "ror" };
#endif

const Register Assembler::argRegs[] = { R0, R1, R2, R3 };
const Register Assembler::retRegs[] = { R0, R1 };
const Register Assembler::savedRegs[] = { R4, R5, R6, R7, R8, R9, R10 };

// --------------------------------
// ARM-specific utility functions.
// --------------------------------

#ifdef DEBUG
// Return true if enc is a valid Operand 2 encoding and thus can be used as-is
// in an ARM arithmetic operation that accepts such encoding.
//
// This utility does not know (or determine) the actual value that the encoded
// value represents, and thus cannot be used to ensure the correct operation of
// encOp2Imm, but it does ensure that the encoded value can be used to encode a
// valid ARM instruction. decOp2Imm can be used if you also need to check that
// a literal is correctly encoded (and thus that encOp2Imm is working
// correctly).
inline bool
Assembler::isOp2Imm(uint32_t enc)
{
    return ((enc & 0xfff) == enc);
}

// Decodes operand 2 immediate values (for debug output and assertions).
inline uint32_t
Assembler::decOp2Imm(uint32_t enc)
{
    NanoAssert(isOp2Imm(enc));

    uint32_t    imm8 = enc & 0xff;
    uint32_t    rot = 32 - ((enc >> 7) & 0x1e);

    return imm8 << (rot & 0x1f);
}
#endif

// Calculate the number of leading zeroes in data.
inline uint32_t
Assembler::CountLeadingZeroes(uint32_t data)
{
    uint32_t    leading_zeroes;

    // We can't do CLZ on anything earlier than ARMv5. Architectures as early
    // as that aren't supported, but assert that we aren't running on one
    // anyway.
    // If ARMv4 support is required in the future for some reason, we can do a
    // run-time check on _config.arm_arch and fall back to the C routine, but for
    // now we can avoid the cost of the check as we don't intend to support
    // ARMv4 anyway.
    NanoAssert(_config.arm_arch >= 5);

#if defined(__ARMCC__)
    // ARMCC can do this with an intrinsic.
    leading_zeroes = __clz(data);

// current Android GCC compiler incorrectly refuses to compile 'clz' for armv5
// (even though this is a legal instruction there). Since we currently only compile for ARMv5
// for emulation, we don't care too much (but we DO care for ARMv6+ since those are "real"
// devices).
#elif defined(__GNUC__) && !(defined(ANDROID) && __ARM_ARCH__ <= 5)
    // GCC can use inline assembler to insert a CLZ instruction.
    __asm (
        "   clz     %0, %1  \n"
        :   "=r"    (leading_zeroes)
        :   "r"     (data)
    );
#elif defined(UNDER_CE)
    // WinCE can do this with an intrinsic.
    leading_zeroes = _CountLeadingZeros(data);
#else
    // Other platforms must fall back to a C routine. This won't be as
    // efficient as the CLZ instruction, but it is functional.
    uint32_t    try_shift;

    leading_zeroes = 0;

    // This loop does a bisection search rather than the obvious rotation loop.
    // This should be faster, though it will still be no match for CLZ.
    for (try_shift = 16; try_shift != 0; try_shift /= 2) {
        uint32_t    shift = leading_zeroes + try_shift;
        if (((data << shift) >> shift) == data) {
            leading_zeroes = shift;
        }
    }
#endif

    // Assert that the operation worked!
    NanoAssert(((0xffffffff >> leading_zeroes) & data) == data);

    return leading_zeroes;
}

// The ARM instruction set allows some flexibility to the second operand of
// most arithmetic operations. When operand 2 is an immediate value, it takes
// the form of an 8-bit value rotated by an even value in the range 0-30.
//
// Some values that can be encoded this scheme — such as 0xf000000f — are
// probably fairly rare in practice and require extra code to detect, so this
// function implements a fast CLZ-based heuristic to detect any value that can
// be encoded using just a shift, and not a full rotation. For example,
// 0xff000000 and 0x000000ff are both detected, but 0xf000000f is not.
//
// This function will return true to indicate that the encoding was successful,
// or false to indicate that the literal could not be encoded as an operand 2
// immediate. If successful, the encoded value will be written to *enc.
inline bool
Assembler::encOp2Imm(uint32_t literal, uint32_t * enc)
{
    // The number of leading zeroes in the literal. This is used to calculate
    // the rotation component of the encoding.
    uint32_t    leading_zeroes;

    // Components of the operand 2 encoding.
    int32_t    rot;
    uint32_t    imm8;

    // Check the literal to see if it is a simple 8-bit value. I suspect that
    // most literals are in fact small values, so doing this check early should
    // give a decent speed-up.
    if (literal < 256)
    {
        *enc = literal;
        return true;
    }

    // Determine the number of leading zeroes in the literal. This is used to
    // calculate the required rotation.
    leading_zeroes = CountLeadingZeroes(literal);

    // We've already done a check to see if the literal is an 8-bit value, so
    // leading_zeroes must be less than (and not equal to) (32-8)=24. However,
    // if it is greater than 24, this algorithm will break, so debug code
    // should use an assertion here to check that we have a value that we
    // expect.
    NanoAssert(leading_zeroes < 24);

    // Assuming that we have a field of no more than 8 bits for a valid
    // literal, we can calculate the required rotation by subtracting
    // leading_zeroes from (32-8):
    //
    // Example:
    //      0: Known to be zero.
    //      1: Known to be one.
    //      X: Either zero or one.
    //      .: Zero in a valid operand 2 literal.
    //
    //  Literal:     [ 1XXXXXXX ........ ........ ........ ]
    //  leading_zeroes = 0
    //  Therefore rot (left) = 24.
    //  Encoded 8-bit literal:                  [ 1XXXXXXX ]
    //
    //  Literal:     [ ........ ..1XXXXX XX...... ........ ]
    //  leading_zeroes = 10
    //  Therefore rot (left) = 14.
    //  Encoded 8-bit literal:                  [ 1XXXXXXX ]
    //
    // Note, however, that we can only encode even shifts, and so
    // "rot=24-leading_zeroes" is not sufficient by itself. By ignoring
    // zero-bits in odd bit positions, we can ensure that we get a valid
    // encoding.
    //
    // Example:
    //  Literal:     [ 01XXXXXX ........ ........ ........ ]
    //  leading_zeroes = 1
    //  Therefore rot (left) = round_up(23) = 24.
    //  Encoded 8-bit literal:                  [ 01XXXXXX ]
    rot = 24 - (leading_zeroes & ~1);

    // The imm8 component of the operand 2 encoding can be calculated from the
    // rot value.
    imm8 = literal >> rot;

    // The validity of the literal can be checked by reversing the
    // calculation. It is much easier to decode the immediate than it is to
    // encode it!
    if (literal != (imm8 << rot)) {
        // The encoding is not valid, so report the failure. Calling code
        // should use some other method of loading the value (such as LDR).
        return false;
    }

    // The operand is valid, so encode it.
    // Note that the ARM encoding is actually described by a rotate to the
    // _right_, so rot must be negated here. Calculating a left shift (rather
    // than calculating a right rotation) simplifies the above code.
    *enc = ((-rot << 7) & 0xf00) | imm8;

    // Assert that the operand was properly encoded.
    NanoAssert(decOp2Imm(*enc) == literal);

    return true;
}

// Encode "rd = rn + imm" using an appropriate instruction sequence.
// Set stat to 1 to update the status flags. Otherwise, set it to 0 or omit it.
// (The declaration in NativeARM.h defines the default value of stat as 0.)
//
// It is not valid to call this function if:
//   (rd == IP) AND (rn == IP) AND !encOp2Imm(imm) AND !encOp2Imm(-imm)
// Where: if (encOp2Imm(imm)), imm can be encoded as an ARM operand 2 using the
// encOp2Imm method.
void
Assembler::asm_add_imm(Register rd, Register rn, int32_t imm, int stat /* =0 */)
{
    // Operand 2 encoding of the immediate.
    uint32_t    op2imm;

    NanoAssert(IsGpReg(rd));
    NanoAssert(IsGpReg(rn));
    NanoAssert((stat & 1) == stat);

    // As a special case to simplify code elsewhere, emit nothing where we
    // don't want to update the flags (stat == 0), the second operand is 0 and
    // (rd == rn). Such instructions are effectively NOPs.
    if ((imm == 0) && (stat == 0) && (rd == rn)) {
        return;
    }

    // Try to encode the value directly as an operand 2 immediate value, then
    // fall back to loading the value into a register.
    if (encOp2Imm(imm, &op2imm)) {
        ADDis(rd, rn, op2imm, stat);
    } else if (encOp2Imm(-imm, &op2imm)) {
        // We could not encode the value for ADD, so try to encode it for SUB.
        // Note that this is valid even if stat is set, _unless_ imm is 0, but
        // that case is caught above.
        NanoAssert(imm != 0);
        SUBis(rd, rn, op2imm, stat);
    } else {
        // We couldn't encode the value directly, so use an intermediate
        // register to encode the value. We will use IP to do this unless rn is
        // IP; in that case we can reuse rd. This allows every case other than
        // "ADD IP, IP, =#imm".
        Register    rm = (rn == IP) ? (rd) : (IP);
        NanoAssert(rn != rm);

        ADDs(rd, rn, rm, stat);
        asm_ld_imm(rm, imm);
    }
}

// Encode "rd = rn - imm" using an appropriate instruction sequence.
// Set stat to 1 to update the status flags. Otherwise, set it to 0 or omit it.
// (The declaration in NativeARM.h defines the default value of stat as 0.)
//
// It is not valid to call this function if:
//   (rd == IP) AND (rn == IP) AND !encOp2Imm(imm) AND !encOp2Imm(-imm)
// Where: if (encOp2Imm(imm)), imm can be encoded as an ARM operand 2 using the
// encOp2Imm method.
void
Assembler::asm_sub_imm(Register rd, Register rn, int32_t imm, int stat /* =0 */)
{
    // Operand 2 encoding of the immediate.
    uint32_t    op2imm;

    NanoAssert(IsGpReg(rd));
    NanoAssert(IsGpReg(rn));
    NanoAssert((stat & 1) == stat);
    
    // As a special case to simplify code elsewhere, emit nothing where we
    // don't want to update the flags (stat == 0), the second operand is 0 and
    // (rd == rn). Such instructions are effectively NOPs.
    if ((imm == 0) && (stat == 0) && (rd == rn)) {
        return;
    }

    // Try to encode the value directly as an operand 2 immediate value, then
    // fall back to loading the value into a register.
    if (encOp2Imm(imm, &op2imm)) {
        SUBis(rd, rn, op2imm, stat);
    } else if (encOp2Imm(-imm, &op2imm)) {
        // We could not encode the value for SUB, so try to encode it for ADD.
        // Note that this is valid even if stat is set, _unless_ imm is 0, but
        // that case is caught above.
        NanoAssert(imm != 0);
        ADDis(rd, rn, op2imm, stat);
    } else {
        // We couldn't encode the value directly, so use an intermediate
        // register to encode the value. We will use IP to do this unless rn is
        // IP; in that case we can reuse rd. This allows every case other than
        // "SUB IP, IP, =#imm".
        Register    rm = (rn == IP) ? (rd) : (IP);
        NanoAssert(rn != rm);

        SUBs(rd, rn, rm, stat);
        asm_ld_imm(rm, imm);
    }
}

// Encode "rd = rn & imm" using an appropriate instruction sequence.
// Set stat to 1 to update the status flags. Otherwise, set it to 0 or omit it.
// (The declaration in NativeARM.h defines the default value of stat as 0.)
//
// It is not valid to call this function if:
//   (rd == IP) AND (rn == IP) AND !encOp2Imm(imm) AND !encOp2Imm(~imm)
// Where: if (encOp2Imm(imm)), imm can be encoded as an ARM operand 2 using the
// encOp2Imm method.
void
Assembler::asm_and_imm(Register rd, Register rn, int32_t imm, int stat /* =0 */)
{
    // Operand 2 encoding of the immediate.
    uint32_t    op2imm;

    NanoAssert(IsGpReg(rd));
    NanoAssert(IsGpReg(rn));
    NanoAssert((stat & 1) == stat);

    // Try to encode the value directly as an operand 2 immediate value, then
    // fall back to loading the value into a register.
    if (encOp2Imm(imm, &op2imm)) {
        ANDis(rd, rn, op2imm, stat);
    } else if (encOp2Imm(~imm, &op2imm)) {
        // Use BIC with the inverted immediate.
        BICis(rd, rn, op2imm, stat);
    } else {
        // We couldn't encode the value directly, so use an intermediate
        // register to encode the value. We will use IP to do this unless rn is
        // IP; in that case we can reuse rd. This allows every case other than
        // "AND IP, IP, =#imm".
        Register    rm = (rn == IP) ? (rd) : (IP);
        NanoAssert(rn != rm);

        ANDs(rd, rn, rm, stat);
        asm_ld_imm(rm, imm);
    }
}

// Encode "rd = rn | imm" using an appropriate instruction sequence.
// Set stat to 1 to update the status flags. Otherwise, set it to 0 or omit it.
// (The declaration in NativeARM.h defines the default value of stat as 0.)
//
// It is not valid to call this function if:
//   (rd == IP) AND (rn == IP) AND !encOp2Imm(imm)
// Where: if (encOp2Imm(imm)), imm can be encoded as an ARM operand 2 using the
// encOp2Imm method.
void
Assembler::asm_orr_imm(Register rd, Register rn, int32_t imm, int stat /* =0 */)
{
    // Operand 2 encoding of the immediate.
    uint32_t    op2imm;

    NanoAssert(IsGpReg(rd));
    NanoAssert(IsGpReg(rn));
    NanoAssert((stat & 1) == stat);

    // Try to encode the value directly as an operand 2 immediate value, then
    // fall back to loading the value into a register.
    if (encOp2Imm(imm, &op2imm)) {
        ORRis(rd, rn, op2imm, stat);
    } else {
        // We couldn't encode the value directly, so use an intermediate
        // register to encode the value. We will use IP to do this unless rn is
        // IP; in that case we can reuse rd. This allows every case other than
        // "ORR IP, IP, =#imm".
        Register    rm = (rn == IP) ? (rd) : (IP);
        NanoAssert(rn != rm);

        ORRs(rd, rn, rm, stat);
        asm_ld_imm(rm, imm);
    }
}

// Encode "rd = rn ^ imm" using an appropriate instruction sequence.
// Set stat to 1 to update the status flags. Otherwise, set it to 0 or omit it.
// (The declaration in NativeARM.h defines the default value of stat as 0.)
//
// It is not valid to call this function if:
//   (rd == IP) AND (rn == IP) AND !encOp2Imm(imm)
// Where: if (encOp2Imm(imm)), imm can be encoded as an ARM operand 2 using the
// encOp2Imm method.
void
Assembler::asm_eor_imm(Register rd, Register rn, int32_t imm, int stat /* =0 */)
{
    // Operand 2 encoding of the immediate.
    uint32_t    op2imm;

    NanoAssert(IsGpReg(rd));
    NanoAssert(IsGpReg(rn));
    NanoAssert((stat & 1) == stat);

    // Try to encode the value directly as an operand 2 immediate value, then
    // fall back to loading the value into a register.
    if (encOp2Imm(imm, &op2imm)) {
        EORis(rd, rn, op2imm, stat);
    } else {
        // We couldn't encoder the value directly, so use an intermediate
        // register to encode the value. We will use IP to do this unless rn is
        // IP; in that case we can reuse rd. This allows every case other than
        // "EOR IP, IP, =#imm".
        Register    rm = (rn == IP) ? (rd) : (IP);
        NanoAssert(rn != rm);

        EORs(rd, rn, rm, stat);
        asm_ld_imm(rm, imm);
    }
}

// --------------------------------
// Assembler functions.
// --------------------------------

void
Assembler::nInit(AvmCore*)
{
#ifdef UNDER_CE
    blx_lr_bug = blx_lr_broken();
#else
    blx_lr_bug = 0;
#endif
    nHints[LIR_calli]  = rmask(retRegs[0]);
    nHints[LIR_hcalli] = rmask(retRegs[1]);
    nHints[LIR_paramp] = PREFER_SPECIAL;
}

void Assembler::nBeginAssembly()
{
    max_out_args = 0;
}

NIns*
Assembler::genPrologue()
{
    /**
     * Prologue
     */

    // NJ_RESV_OFFSET is space at the top of the stack for us
    // to use for parameter passing (8 bytes at the moment)
    uint32_t stackNeeded = max_out_args + STACK_GRANULARITY * _activation.stackSlotsNeeded();
    uint32_t savingCount = 2;

    uint32_t savingMask = rmask(FP) | rmask(LR);

    // so for alignment purposes we've pushed return addr and fp
    uint32_t stackPushed = STACK_GRANULARITY * savingCount;
    uint32_t aligned = alignUp(stackNeeded + stackPushed, NJ_ALIGN_STACK);
    int32_t amt = aligned - stackPushed;

    // Make room on stack for what we are doing
    if (amt)
        asm_sub_imm(SP, SP, amt);

    verbose_only( asm_output("## %p:",(void*)_nIns); )
    verbose_only( asm_output("## patch entry"); )
    NIns *patchEntry = _nIns;

    MOV(FP, SP);
    PUSH_mask(savingMask);
    return patchEntry;
}

void
Assembler::nFragExit(LIns* guard)
{
    SideExit *  exit = guard->record()->exit;
    Fragment *  frag = exit->target;

    bool        target_is_known = frag && frag->fragEntry;

    if (target_is_known) {
        // The target exists so we can simply emit a branch to its location.
        JMP_far(frag->fragEntry);
    } else {
        // The target doesn't exit yet, so emit a jump to the epilogue. If the
        // target is created later on, the jump will be patched.

        GuardRecord *gr = guard->record();

        if (!_epilogue)
            _epilogue = genEpilogue();

        // Jump to the epilogue. This may get patched later, but JMP_far always
        // emits two instructions even when only one is required, so patching
        // will work correctly.
        JMP_far(_epilogue);

        // In the future you may want to move this further down so that we can
        // overwrite the r0 guard record load during a patch to a different
        // fragment with some assumed input-register state. Not today though.
        gr->jmp = _nIns;

        // NB: this is a workaround for the fact that, by patching a
        // fragment-exit jump, we could be changing the *meaning* of the R0
        // register we're passing to the jump target. If we jump to the
        // epilogue, ideally R0 means "return value when exiting fragment".
        // If we patch this to jump to another fragment however, R0 means
        // "incoming 0th parameter". This is just a quirk of ARM ABI. So
        // we compromise by passing "return value" to the epilogue in IP,
        // not R0, and have the epilogue MOV(R0, IP) first thing.

        asm_ld_imm(IP, int(gr));
    }

#ifdef NJ_VERBOSE
    if (_config.arm_show_stats) {
        // load R1 with Fragment *fromFrag, target fragment
        // will make use of this when calling fragenter().
        int fromfrag = int((Fragment*)_thisfrag);
        asm_ld_imm(argRegs[1], fromfrag);
    }
#endif

    // profiling for the exit
    verbose_only(
       if (_logc->lcbits & LC_FragProfile) {
           asm_inc_m32( &guard->record()->profCount );
       }
    )

    // Pop the stack frame.
    MOV(SP, FP);
}

NIns*
Assembler::genEpilogue()
{
    // On ARMv5+, loading directly to PC correctly handles interworking.
    // Note that we don't support anything older than ARMv5.
    NanoAssert(_config.arm_arch >= 5);

    RegisterMask savingMask = rmask(FP) | rmask(PC);

    POP_mask(savingMask); // regs

    // NB: this is the later half of the dual-nature patchable exit branch
    // workaround noted above in nFragExit. IP has the "return value"
    // incoming, we need to move it to R0.
    MOV(R0, IP);

    return _nIns;
}

/*
 * asm_arg will encode the specified argument according to the current ABI, and
 * will update r and stkd as appropriate so that the next argument can be
 * encoded.
 *
 * Linux has used ARM's EABI for some time. Windows CE uses the legacy ABI.
 *
 * Under EABI:
 * - doubles are 64-bit aligned both in registers and on the stack.
 *   If the next available argument register is R1, it is skipped
 *   and the double is placed in R2:R3.  If R0:R1 or R2:R3 are not
 *   available, the double is placed on the stack, 64-bit aligned.
 * - 32-bit arguments are placed in registers and 32-bit aligned
 *   on the stack.
 *
 * Under legacy ABI:
 * - doubles are placed in subsequent arg registers; if the next
 *   available register is r3, the low order word goes into r3
 *   and the high order goes on the stack.
 * - 32-bit arguments are placed in the next available arg register,
 * - both doubles and 32-bit arguments are placed on stack with 32-bit
 *   alignment.
 */
void
Assembler::asm_arg(ArgType ty, LIns* arg, Register& r, int& stkd)
{
    // The stack pointer must always be at least aligned to 4 bytes.
    NanoAssert((stkd & 3) == 0);

    if (ty == ARGTYPE_D) {
        // This task is fairly complex and so is delegated to asm_arg_64.
        asm_arg_64(arg, r, stkd);
    } else {
        NanoAssert(ty == ARGTYPE_I || ty == ARGTYPE_UI);
        // pre-assign registers R0-R3 for arguments (if they fit)
        if (r < R4) {
            asm_regarg(ty, arg, r);
            r = nextreg(r);
        } else {
            asm_stkarg(arg, stkd);
            stkd += 4;
        }
    }
}

// Encode a 64-bit floating-point argument using the appropriate ABI.
// This function operates in the same way as asm_arg, except that it will only
// handle arguments where (ArgType)ty == ARGTYPE_D.
void
Assembler::asm_arg_64(LIns* arg, Register& r, int& stkd)
{
    // The stack pointer must always be at least aligned to 4 bytes.
    NanoAssert((stkd & 3) == 0);
    // The only use for this function when we are using soft floating-point
    // is for LIR_ii2d.
    NanoAssert(_config.arm_vfp || arg->isop(LIR_ii2d));

    Register    fp_reg = deprecated_UnknownReg;

    if (_config.arm_vfp) {
        fp_reg = findRegFor(arg, FpRegs);
        NanoAssert(deprecated_isKnownReg(fp_reg));
    }

#ifdef NJ_ARM_EABI
    // EABI requires that 64-bit arguments are aligned on even-numbered
    // registers, as R0:R1 or R2:R3. If the register base is at an
    // odd-numbered register, advance it. Note that this will push r past
    // R3 if r is R3 to start with, and will force the argument to go on
    // the stack.
    if ((r == R1) || (r == R3)) {
        r = nextreg(r);
    }
#endif

    if (r < R3) {
        Register    ra = r;
        Register    rb = nextreg(r);
        r = nextreg(rb);

#ifdef NJ_ARM_EABI
        // EABI requires that 64-bit arguments are aligned on even-numbered
        // registers, as R0:R1 or R2:R3.
        NanoAssert( ((ra == R0) && (rb == R1)) || ((ra == R2) && (rb == R3)) );
#endif

        // Put the argument in ra and rb. If the argument is in a VFP register,
        // use FMRRD to move it to ra and rb. Otherwise, let asm_regarg deal
        // with the argument as if it were two 32-bit arguments.
        if (_config.arm_vfp) {
            FMRRD(ra, rb, fp_reg);
        } else {
            asm_regarg(ARGTYPE_I, arg->oprnd1(), ra);
            asm_regarg(ARGTYPE_I, arg->oprnd2(), rb);
        }

#ifndef NJ_ARM_EABI
    } else if (r == R3) {
        // We only have one register left, but the legacy ABI requires that we
        // put 32 bits of the argument in the register (R3) and the remaining
        // 32 bits on the stack.
        Register    ra = r;
        r = nextreg(r);

        // This really just checks that nextreg() works properly, as we know
        // that r was previously R3.
        NanoAssert(r == R4);

        // We're splitting the argument between registers and the stack.  This
        // must be the first time that the stack is used, so stkd must be at 0.
        NanoAssert(stkd == 0);

        if (_config.arm_vfp) {
            // TODO: We could optimize the this to store directly from
            // the VFP register to memory using "FMRRD ra, fp_reg[31:0]" and
            // "STR fp_reg[63:32], [SP, #stkd]".

            // Load from the floating-point register as usual, but use IP
            // as a swap register.
            STR(IP, SP, 0);
            stkd += 4;
            FMRRD(ra, IP, fp_reg);
        } else {
            // Without VFP, we can simply use asm_regarg and asm_stkarg to
            // encode the two 32-bit words as we don't need to load from a VFP
            // register.
            asm_regarg(ARGTYPE_I, arg->oprnd1(), ra);
            asm_stkarg(arg->oprnd2(), 0);
            stkd += 4;
        }
#endif
    } else {
        // The argument won't fit in registers, so pass on to asm_stkarg.
#ifdef NJ_ARM_EABI
        // EABI requires that 64-bit arguments are 64-bit aligned.
        if ((stkd & 7) != 0) {
            // stkd will always be aligned to at least 4 bytes; this was
            // asserted on entry to this function.
            stkd += 4;
        }
#endif
        asm_stkarg(arg, stkd);
        stkd += 8;
    }
}

void
Assembler::asm_regarg(ArgType ty, LIns* p, Register r)
{
    NanoAssert(deprecated_isKnownReg(r));
    if (ty == ARGTYPE_I || ty == ARGTYPE_UI)
    {
        // arg goes in specific register
        if (p->isImmI()) {
            asm_ld_imm(r, p->immI());
        } else {
            if (p->isExtant()) {
                if (!p->deprecated_hasKnownReg()) {
                    // load it into the arg reg
                    int d = findMemFor(p);
                    if (p->isop(LIR_allocp)) {
                        asm_add_imm(r, FP, d, 0);
                    } else {
                        LDR(r, FP, d);
                    }
                } else {
                    // it must be in a saved reg
                    MOV(r, p->deprecated_getReg());
                }
            }
            else {
                // this is the last use, so fine to assign it
                // to the scratch reg, it's dead after this point.
                findSpecificRegFor(p, r);
            }
        }
    }
    else
    {
        NanoAssert(ty == ARGTYPE_D);
        // fpu argument in register - should never happen since FPU
        // args are converted to two 32-bit ints on ARM
        NanoAssert(false);
    }
}

void
Assembler::asm_stkarg(LIns* arg, int stkd)
{
    bool isF64 = arg->isD();

    Register rr;
    if (arg->isExtant() && (rr = arg->deprecated_getReg(), deprecated_isKnownReg(rr))) {
        // The argument resides somewhere in registers, so we simply need to
        // push it onto the stack.
        if (!_config.arm_vfp || !isF64) {
            NanoAssert(IsGpReg(rr));

            asm_str(rr, SP, stkd);
        } else {
            // According to the comments in asm_arg_64, LIR_ii2d
            // can have a 64-bit argument even if VFP is disabled. However,
            // asm_arg_64 will split the argument and issue two 32-bit
            // arguments to asm_stkarg so we can ignore that case here and
            // assert that we will never get 64-bit arguments unless VFP is
            // available.
            NanoAssert(_config.arm_vfp);
            NanoAssert(IsFpReg(rr));

#ifdef NJ_ARM_EABI
            // EABI requires that 64-bit arguments are 64-bit aligned.
            NanoAssert((stkd & 7) == 0);
#endif

            FSTD(rr, SP, stkd);
        }
    } else {
        // The argument does not reside in registers, so we need to get some
        // memory for it and then copy it onto the stack.
        int d = findMemFor(arg);
        if (!isF64) {
            asm_str(IP, SP, stkd);
            if (arg->isop(LIR_allocp)) {
                asm_add_imm(IP, FP, d);
            } else {
                LDR(IP, FP, d);
            }
        } else {
#ifdef NJ_ARM_EABI
            // EABI requires that 64-bit arguments are 64-bit aligned.
            NanoAssert((stkd & 7) == 0);
#endif

            asm_str(IP, SP, stkd+4);
            LDR(IP, FP, d+4);
            asm_str(IP, SP, stkd);
            LDR(IP, FP, d);
        }
    }
}

void
Assembler::asm_call(LIns* ins)
{
    if (_config.arm_vfp && ins->isop(LIR_calld)) {
        /* Because ARM actually returns the result in (R0,R1), and not in a
         * floating point register, the code to move the result into a correct
         * register is below.  We do nothing here.
         *
         * The reason being that if we did something here, the final code
         * sequence we'd get would be something like:
         *     MOV {R0-R3},params        [from below]
         *     BL function               [from below]
         *     MOV {R0-R3},spilled data  [from evictScratchRegsExcept()]
         *     MOV Dx,{R0,R1}            [from here]
         * which is clearly broken.
         *
         * This is not a problem for non-floating point calls, because the
         * restoring of spilled data into R0 is done via a call to
         * deprecated_prepResultReg(R0) in the other branch of this if-then-else,
         * meaning that evictScratchRegsExcept() will not modify R0. However,
         * deprecated_prepResultReg is not aware of the concept of using a register pair
         * (R0,R1) for the result of a single operation, so it can only be
         * used here with the ultimate VFP register, and not R0/R1, which
         * potentially allows for R0/R1 to get corrupted as described.
         */
    } else {
        deprecated_prepResultReg(ins, rmask(retRegs[0]));
    }

    // Do this after we've handled the call result, so we don't
    // force the call result to be spilled unnecessarily.

    evictScratchRegsExcept(0);

    const CallInfo* ci = ins->callInfo();
    ArgType argTypes[MAXARGS];
    uint32_t argc = ci->getArgTypes(argTypes);
    bool indirect = ci->isIndirect();

    // If we aren't using VFP, assert that the LIR operation is an integer
    // function call.
    NanoAssert(_config.arm_vfp || ins->isop(LIR_calli));

    // If we're using VFP, and the return type is a double, it'll come back in
    // R0/R1. We need to either place it in the result fp reg, or store it.
    // See comments above for more details as to why this is necessary here
    // for floating point calls, but not for integer calls.
    if (_config.arm_vfp && ins->isExtant()) {
        // If the result size is a floating-point value, treat the result
        // specially, as described previously.
        if (ci->returnType() == ARGTYPE_D) {
            Register rr = ins->deprecated_getReg();

            NanoAssert(ins->opcode() == LIR_calld);

            if (!deprecated_isKnownReg(rr)) {
                int d = deprecated_disp(ins);
                NanoAssert(d != 0);
                deprecated_freeRsrcOf(ins);

                // The result doesn't have a register allocated, so store the
                // result (in R0,R1) directly to its stack slot.
                asm_str(R0, FP, d+0);
                asm_str(R1, FP, d+4);
            } else {
                NanoAssert(IsFpReg(rr));

                // Copy the result to the (VFP) result register.
                deprecated_prepResultReg(ins, rmask(rr));
                FMDRR(rr, R0, R1);
            }
        }
    }

    // Emit the branch.
    if (!indirect) {
        verbose_only(if (_logc->lcbits & LC_Native)
            outputf("        %p:", _nIns);
        )

        // Direct call: on v5 and above (where the calling sequence doesn't
        // corrupt LR until the actual branch instruction), we can avoid an
        // interlock in the "long" branch sequence by manually loading the
        // target address into LR ourselves before setting up the parameters
        // in other registers.
        BranchWithLink((NIns*)ci->_address);
    } else {
        // Indirect call: we assign the address arg to LR since it's not
        // used for regular arguments, and is otherwise scratch since it's
        // clobberred by the call. On v4/v4T, where we have to manually do
        // the equivalent of a BLX, move LR into IP before corrupting LR
        // with the return address.
        if (blx_lr_bug) {
            // workaround for msft device emulator bug (blx lr emulated as no-op)
            underrunProtect(8);
            BLX(IP);
            MOV(IP,LR);
        } else {
            BLX(LR);
        }
        asm_regarg(ARGTYPE_I, ins->arg(--argc), LR);
    }

    // Encode the arguments, starting at R0 and with an empty argument stack.
    Register    r = R0;
    int         stkd = 0;

    // Iterate through the argument list and encode each argument according to
    // the ABI.
    // Note that we loop through the arguments backwards as LIR specifies them
    // in reverse order.
    uint32_t    i = argc;
    while(i--) {
        asm_arg(argTypes[i], ins->arg(i), r, stkd);
    }

    if (stkd > max_out_args) {
        max_out_args = stkd;
    }
}

Register
Assembler::nRegisterAllocFromSet(RegisterMask set)
{
    NanoAssert(set != 0);

    // The CountLeadingZeroes function will use the CLZ instruction where
    // available. In other cases, it will fall back to a (slower) C
    // implementation.
    Register r = (Register)(31-CountLeadingZeroes(set));
    _allocator.free &= ~rmask(r);

    NanoAssert(IsGpReg(r) || IsFpReg(r));
    NanoAssert((rmask(r) & set) == rmask(r));

    return r;
}

void
Assembler::nRegisterResetAll(RegAlloc& a)
{
    // add scratch registers to our free list for the allocator
    a.clear();
    a.free =
        rmask(R0) | rmask(R1) | rmask(R2) | rmask(R3) | rmask(R4) |
        rmask(R5) | rmask(R6) | rmask(R7) | rmask(R8) | rmask(R9) |
        rmask(R10) | rmask(LR);
    if (_config.arm_vfp)
        a.free |= FpRegs;

    debug_only(a.managed = a.free);
}

static inline ConditionCode
get_cc(NIns *ins)
{
    return ConditionCode((*ins >> 28) & 0xF);
}

static inline bool
branch_is_B(NIns* branch)
{
    return (*branch & 0x0E000000) == 0x0A000000;
}

static inline bool
branch_is_LDR_PC(NIns* branch)
{
    return (*branch & 0x0F7FF000) == 0x051FF000;
}

// Is this an instruction of the form  ldr/str reg, [fp, #-imm] ?
static inline bool
is_ldstr_reg_fp_minus_imm(/*OUT*/uint32_t* isLoad, /*OUT*/uint32_t* rX,
                          /*OUT*/uint32_t* immX, NIns i1)
{
    if ((i1 & 0xFFEF0000) != 0xE50B0000)
        return false;
    *isLoad = (i1 >> 20) & 1;
    *rX     = (i1 >> 12) & 0xF;
    *immX   = i1 & 0xFFF;
    return true;
}

// Is this an instruction of the form  ldmdb/stmdb fp, regset ?
static inline bool
is_ldstmdb_fp(/*OUT*/uint32_t* isLoad, /*OUT*/uint32_t* regSet, NIns i1)
{
    if ((i1 & 0xFFEF0000) != 0xE90B0000)
        return false;
    *isLoad = (i1 >> 20) & 1;
    *regSet = i1 & 0xFFFF;
    return true;
}

// Make an instruction of the form ldmdb/stmdb fp, regset
static inline NIns
mk_ldstmdb_fp(uint32_t isLoad, uint32_t regSet)
{
    return 0xE90B0000 | (regSet & 0xFFFF) | ((isLoad & 1) << 20);
}

// Compute the number of 1 bits in the lowest 16 bits of regSet
static inline uint32_t
size_of_regSet(uint32_t regSet)
{
   uint32_t x = regSet;
   x = (x & 0x5555) + ((x >> 1) & 0x5555);
   x = (x & 0x3333) + ((x >> 2) & 0x3333);
   x = (x & 0x0F0F) + ((x >> 4) & 0x0F0F);
   x = (x & 0x00FF) + ((x >> 8) & 0x00FF);
   return x;
}

// See if two ARM instructions, i1 and i2, can be combined into one
static bool
do_peep_2_1(/*OUT*/NIns* merged, NIns i1, NIns i2)
{
    uint32_t rX, rY, immX, immY, isLoadX, isLoadY, regSet;
    /*   ld/str rX, [fp, #-8]
         ld/str rY, [fp, #-4]
         ==>
         ld/stmdb fp, {rX, rY}
         when
         X < Y and X != fp and Y != fp and X != 15 and Y != 15
    */
    if (is_ldstr_reg_fp_minus_imm(&isLoadX, &rX, &immX, i1) &&
        is_ldstr_reg_fp_minus_imm(&isLoadY, &rY, &immY, i2) &&
        immX == 8 && immY == 4 && rX < rY &&
        isLoadX == isLoadY &&
        rX != FP && rY != FP &&
         rX != 15 && rY != 15) {
        *merged = mk_ldstmdb_fp(isLoadX, (1 << rX) | (1<<rY));
        return true;
    }
    /*   ld/str   rX, [fp, #-N]
         ld/stmdb fp, regset
         ==>
         ld/stmdb fp, union(regset,{rX})
         when
         regset is nonempty
         X < all elements of regset
         N == 4 * (1 + card(regset))
         X != fp and X != 15
    */
    if (is_ldstr_reg_fp_minus_imm(&isLoadX, &rX, &immX, i1) &&
        is_ldstmdb_fp(&isLoadY, &regSet, i2) &&
        regSet != 0 &&
        (regSet & ((1 << (rX + 1)) - 1)) == 0 &&
        immX == 4 * (1 + size_of_regSet(regSet)) &&
        isLoadX == isLoadY &&
        rX != FP && rX != 15) {
        *merged = mk_ldstmdb_fp(isLoadX, regSet | (1 << rX));
        return true;
    }
    return false;
}

// Determine whether or not it's safe to look at _nIns[1].
// Necessary condition for safe peepholing with do_peep_2_1.
static inline bool
does_next_instruction_exist(NIns* _nIns, NIns* codeStart, NIns* codeEnd,
                            NIns* exitStart, NIns* exitEnd)
{
    return (exitStart <= _nIns && _nIns+1 < exitEnd) ||
           (codeStart <= _nIns && _nIns+1 < codeEnd);
}

void
Assembler::nPatchBranch(NIns* branch, NIns* target)
{
    // Patch the jump in a loop

    //
    // There are two feasible cases here, the first of which has 2 sub-cases:
    //
    //   (1) We are patching a patchable unconditional jump emitted by
    //       JMP_far.  All possible encodings we may be looking at with
    //       involve 2 words, though we *may* have to change from 1 word to
    //       2 or vice verse.
    //
    //          1a:  B ±32MB ; BKPT
    //          1b:  LDR PC [PC, #-4] ; $imm
    //
    //   (2) We are patching a patchable conditional jump emitted by
    //       B_cond_chk.  Short conditional jumps are non-patchable, so we
    //       won't have one here; will only ever have an instruction of the
    //       following form:
    //
    //          LDRcc PC [PC, #lit] ...
    //
    //       We don't actually know whether the lit-address is in the
    //       constant pool or in-line of the instruction stream, following
    //       the insn (with a jump over it) and we don't need to. For our
    //       purposes here, cases 2, 3 and 4 all look the same.
    //
    // For purposes of handling our patching task, we group cases 1b and 2
    // together, and handle case 1a on its own as it might require expanding
    // from a short-jump to a long-jump.
    //
    // We do not handle contracting from a long-jump to a short-jump, though
    // this is a possible future optimisation for case 1b. For now it seems
    // not worth the trouble.
    //

    if (branch_is_B(branch)) {
        // Case 1a
        // A short B branch, must be unconditional.
        NanoAssert(get_cc(branch) == AL);

        int32_t offset = PC_OFFSET_FROM(target, branch);
        if (isS24(offset>>2)) {
            // We can preserve the existing form, just rewrite its offset.
            NIns cond = *branch & 0xF0000000;
            *branch = (NIns)( cond | (0xA<<24) | ((offset>>2) & 0xFFFFFF) );
        } else {
            // We need to expand the existing branch to a long jump.
            // make sure the next instruction is a dummy BKPT
            NanoAssert(*(branch+1) == BKPT_insn);

            // Set the branch instruction to   LDRcc pc, [pc, #-4]
            NIns cond = *branch & 0xF0000000;
            *branch++ = (NIns)( cond | (0x51<<20) | (PC<<16) | (PC<<12) | (4));
            *branch++ = (NIns)target;
        }
    } else {
        // Case 1b & 2
        // Not a B branch, must be LDR, might be any kind of condition.
        NanoAssert(branch_is_LDR_PC(branch));

        NIns *addr = branch+2;
        int offset = (*branch & 0xFFF) / sizeof(NIns);
        if (*branch & (1<<23)) {
            addr += offset;
        } else {
            addr -= offset;
        }

        // Just redirect the jump target, leave the insn alone.
        *addr = (NIns) target;
    }
}

RegisterMask
Assembler::nHint(LIns* ins)
{
    NanoAssert(ins->isop(LIR_paramp)); 
    RegisterMask prefer = 0;
    if (ins->paramKind() == 0)
        if (ins->paramArg() < 4)
            prefer = rmask(argRegs[ins->paramArg()]);
    return prefer;
}

void
Assembler::asm_qjoin(LIns *ins)
{
    int d = findMemFor(ins);
    NanoAssert(d);
    LIns* lo = ins->oprnd1();
    LIns* hi = ins->oprnd2();

    Register r = findRegFor(hi, GpRegs);
    asm_str(r, FP, d+4);

    // okay if r gets recycled.
    r = findRegFor(lo, GpRegs);
    asm_str(r, FP, d);
    deprecated_freeRsrcOf(ins);     // if we had a reg in use, emit a ST to flush it to mem
}

void
Assembler::asm_store32(LOpcode op, LIns *value, int dr, LIns *base)
{
    Register ra, rb;
    getBaseReg2(GpRegs, value, ra, GpRegs, base, rb, dr);

    switch (op) {
        case LIR_sti:
            if (isU12(-dr) || isU12(dr)) {
                STR(ra, rb, dr);
            } else {
                STR(ra, IP, 0);
                asm_add_imm(IP, rb, dr);
            }
            return;
        case LIR_sti2c:
            if (isU12(-dr) || isU12(dr)) {
                STRB(ra, rb, dr);
            } else {
                STRB(ra, IP, 0);
                asm_add_imm(IP, rb, dr);
            }
            return;
        case LIR_sti2s:
            // Similar to the sti/stb case, but the max offset is smaller.
            if (isU8(-dr) || isU8(dr)) {
                STRH(ra, rb, dr);
            } else {
                STRH(ra, IP, 0);
                asm_add_imm(IP, rb, dr);
            }
            return;
        default:
            NanoAssertMsg(0, "asm_store32 should never receive this LIR opcode");
            return;
    }
}

bool
canRematALU(LIns *ins)
{
    // Return true if we can generate code for this instruction that neither
    // sets CCs, clobbers an input register, nor requires allocating a register.
    switch (ins->opcode()) {
    case LIR_addi:
    case LIR_subi:
    case LIR_andi:
    case LIR_ori:
    case LIR_xori:
        return ins->oprnd1()->isInReg() && ins->oprnd2()->isImmI();
    default:
        ;
    }
    return false;
}

bool
Assembler::canRemat(LIns* ins)
{
    return ins->isImmI() || ins->isop(LIR_allocp) || canRematALU(ins);
}

void
Assembler::asm_restore(LIns* i, Register r)
{
    // The following registers should never be restored:
    NanoAssert(r != PC);
    NanoAssert(r != IP);
    NanoAssert(r != SP);

    if (i->isop(LIR_allocp)) {
        asm_add_imm(r, FP, deprecated_disp(i));
    } else if (i->isImmI()) {
        asm_ld_imm(r, i->immI());
    } else if (canRematALU(i)) {
        Register rn = i->oprnd1()->getReg();
        int32_t imm = i->oprnd2()->immI();
        switch (i->opcode()) {
        case LIR_addi: asm_add_imm(r, rn, imm, /*stat=*/ 0); break;
        case LIR_subi: asm_sub_imm(r, rn, imm, /*stat=*/ 0); break;
        case LIR_andi: asm_and_imm(r, rn, imm, /*stat=*/ 0); break;
        case LIR_ori:  asm_orr_imm(r, rn, imm, /*stat=*/ 0); break;
        case LIR_xori: asm_eor_imm(r, rn, imm, /*stat=*/ 0); break;
        default:       NanoAssert(0);                        break;
        }
    } else {
        // We can't easily load immediate values directly into FP registers, so
        // ensure that memory is allocated for the constant and load it from
        // memory.
        int d = findMemFor(i);
        if (_config.arm_vfp && IsFpReg(r)) {
            if (isS8(d >> 2)) {
                FLDD(r, FP, d);
            } else {
                FLDD(r, IP, 0);
                asm_add_imm(IP, FP, d);
            }
        } else {
            NIns merged;
            LDR(r, FP, d);
            // See if we can merge this load into an immediately following
            // one, by creating or extending an LDM instruction.
            if (/* is it safe to poke _nIns[1] ? */
                does_next_instruction_exist(_nIns, codeStart, codeEnd,
                                                   exitStart, exitEnd)
                && /* can we merge _nIns[0] into _nIns[1] ? */
                   do_peep_2_1(&merged, _nIns[0], _nIns[1])) {
                _nIns[1] = merged;
                _nIns++;
                verbose_only( asm_output("merge next into LDMDB"); )
            }
        }
    }
}

void
Assembler::asm_spill(Register rr, int d, bool pop, bool quad)
{
    (void) pop;
    (void) quad;
    NanoAssert(d);
    // The following registers should never be spilled:
    NanoAssert(rr != PC);
    NanoAssert(rr != IP);
    NanoAssert(rr != SP);
    if (_config.arm_vfp && IsFpReg(rr)) {
        if (isS8(d >> 2)) {
            FSTD(rr, FP, d);
        } else {
            FSTD(rr, IP, 0);
            asm_add_imm(IP, FP, d);
        }
    } else {
        NIns merged;
        // asm_str always succeeds, but returns '1' to indicate that it emitted
        // a simple, easy-to-merge STR.
        if (asm_str(rr, FP, d)) {
            // See if we can merge this store into an immediately following one,
            // one, by creating or extending a STM instruction.
            if (/* is it safe to poke _nIns[1] ? */
                    does_next_instruction_exist(_nIns, codeStart, codeEnd,
                        exitStart, exitEnd)
                    && /* can we merge _nIns[0] into _nIns[1] ? */
                    do_peep_2_1(&merged, _nIns[0], _nIns[1])) {
                _nIns[1] = merged;
                _nIns++;
                verbose_only( asm_output("merge next into STMDB"); )
            }
        }
    }
}

void
Assembler::asm_load64(LIns* ins)
{
    //asm_output("<<< load64");

    NanoAssert(ins->isD());

    LIns* base = ins->oprnd1();
    int offset = ins->disp();

    Register rr = ins->deprecated_getReg();
    int d = deprecated_disp(ins);

    Register rb = findRegFor(base, GpRegs);
    NanoAssert(IsGpReg(rb));
    deprecated_freeRsrcOf(ins);

    //outputf("--- load64: Finished register allocation.");

    switch (ins->opcode()) {
        case LIR_ldd:
            if (_config.arm_vfp && deprecated_isKnownReg(rr)) {
                // VFP is enabled and the result will go into a register.
                NanoAssert(IsFpReg(rr));

                if (!isS8(offset >> 2) || (offset&3) != 0) {
                    FLDD(rr,IP,0);
                    asm_add_imm(IP, rb, offset);
                } else {
                    FLDD(rr,rb,offset);
                }
            } else {
                // Either VFP is not available or the result needs to go into memory;
                // in either case, VFP instructions are not required. Note that the
                // result will never be loaded into registers if VFP is not available.
                NanoAssert(!deprecated_isKnownReg(rr));
                NanoAssert(d != 0);

                // Check that the offset is 8-byte (64-bit) aligned.
                NanoAssert((d & 0x7) == 0);

                // *(uint64_t*)(FP+d) = *(uint64_t*)(rb+offset)
                asm_mmq(FP, d, rb, offset);
            }
            return;

        case LIR_ldf2d:
            if (_config.arm_vfp) {
                if (deprecated_isKnownReg(rr)) {
                    NanoAssert(IsFpReg(rr));
                    FCVTDS(rr, S14);
                } else {
                    // Normally D7 isn't allowed to be used as an FP reg.
                    // In this case we make an explicit exception.
                    if (isS8(d)) {
                        FSTD_allowD7(D7, FP, d, true);
                    } else {
                        FSTD_allowD7(D7, IP, 0, true);
                        asm_add_imm(IP, FP, d);
                    }
                    FCVTDS_allowD7(D7, S14, true);
                }

                // always load into a VFP reg to do the conversion, and always use
                // our S14 scratch reg
                if (!isS8(offset >> 2) || (offset&3) != 0) {
                    FLDS(S14, IP, 0);
                    asm_add_imm(IP, rb, offset);
                } else {
                    FLDS(S14, rb, offset);
                }
            } else {
                NanoAssertMsg(0, "ld32f not supported with non-VFP, fix me");
            }
            return;

        default:
            NanoAssertMsg(0, "asm_load64 should never receive this LIR opcode");
            return;
    }

    //asm_output(">>> load64");
}

void
Assembler::asm_store64(LOpcode op, LIns* value, int dr, LIns* base)
{
    //asm_output("<<< store64 (dr: %d)", dr);

    switch (op) {
        case LIR_std:
            if (_config.arm_vfp) {
                Register rb = findRegFor(base, GpRegs);

                if (value->isImmD()) {
                    underrunProtect(LD32_size*2 + 8);

                    // XXX use another reg, get rid of dependency
                    asm_str(IP, rb, dr);
                    asm_ld_imm(IP, value->immDlo(), false);
                    asm_str(IP, rb, dr+4);
                    asm_ld_imm(IP, value->immDhi(), false);

                    return;
                }

                Register rv = findRegFor(value, FpRegs);

                NanoAssert(deprecated_isKnownReg(rb));
                NanoAssert(deprecated_isKnownReg(rv));

                Register baseReg = rb;
                intptr_t baseOffset = dr;

                // VFP cannot do non-word-aligned accesses, even with a
                // register base.
                NanoAssert((dr%4) == 0);

                if (!isS8(dr/4)) {
                    baseReg = IP;
                    baseOffset = 0;
                }

                FSTD(rv, baseReg, baseOffset);

                if (!isS8(dr/4)) {
                    asm_add_imm(IP, rb, dr);
                }

                // if it's a constant, make sure our baseReg/baseOffset location
                // has the right value
                if (value->isImmD()) {
                    underrunProtect(4*4);
                    asm_immd_nochk(rv, value->immDlo(), value->immDhi());
                }
            } else {
                int da = findMemFor(value);
                Register rb = findRegFor(base, GpRegs);
                // *(uint64_t*)(rb+dr) = *(uint64_t*)(FP+da)
                asm_mmq(rb, dr, FP, da);
            }
            return;

        case LIR_std2f:
            if (_config.arm_vfp) {
                Register rb = findRegFor(base, GpRegs);

                if (value->isImmD()) {
                    union {
                        float       f;
                        uint32_t    i;
                    } imm;
                    imm.f = (float)(value->immD());
                    asm_str(IP, rb, dr);
                    asm_ld_imm(IP, imm.i);
                    return;
                }

                Register rv = findRegFor(value, FpRegs);

                NanoAssert(deprecated_isKnownReg(rb));
                NanoAssert(deprecated_isKnownReg(rv));

                Register baseReg = rb;
                intptr_t baseOffset = dr;

                // VFP cannot do non-word-aligned accesses, even with a
                // register base.
                NanoAssert((dr%4) == 0);

                if (!isS8(dr/4)) {
                    baseReg = IP;
                    baseOffset = 0;
                }

                FSTS(S14, baseReg, baseOffset);

                if (!isS8(dr/4)) {
                    asm_add_imm(IP, rb, dr);
                }

                FCVTSD(S14, rv);

                // if it's a constant, make sure our baseReg/baseOffset location
                // has the right value
                if (value->isImmD()) {
                    underrunProtect(4*4);
                    asm_immd_nochk(rv, value->immDlo(), value->immDhi());
                }
            } else {
                NanoAssertMsg(0, "st32f not supported with non-VFP, fix me");
            }
            return;
        default:
            NanoAssertMsg(0, "asm_store64 should never receive this LIR opcode");
            return;
    }

    //asm_output(">>> store64");
}

// Stick a float into register rr, where p points to the two
// 32-bit parts of the quad, optinally also storing at FP+d
void
Assembler::asm_immd_nochk(Register rr, int32_t immDlo, int32_t immDhi)
{
    // We're not going to use a slot, because it might be too far
    // away.  Instead, we're going to stick a branch in the stream to
    // jump over the constants, and then load from a short PC relative
    // offset.

    // stream should look like:
    //    branch A
    //    immDlo
    //    immDhi
    // A: FLDD PC-16

    FLDD(rr, PC, -16);

    *(--_nIns) = (NIns) immDhi;
    *(--_nIns) = (NIns) immDlo;

    B_nochk(_nIns+2);
}

void
Assembler::asm_immd(LIns* ins)
{
    int d = deprecated_disp(ins);
    Register rr = ins->deprecated_getReg();

    deprecated_freeRsrcOf(ins);

    if (_config.arm_vfp && deprecated_isKnownReg(rr)) {
        if (d)
            asm_spill(rr, d, false, true);

        underrunProtect(4*4);
        asm_immd_nochk(rr, ins->immDlo(), ins->immDhi());
    } else {
        NanoAssert(d);

        asm_str(IP, FP, d+4);
        asm_ld_imm(IP, ins->immDhi());
        asm_str(IP, FP, d);
        asm_ld_imm(IP, ins->immDlo());
    }
}

void
Assembler::asm_nongp_copy(Register r, Register s)
{
    if (_config.arm_vfp && IsFpReg(r) && IsFpReg(s)) {
        // fp->fp
        FCPYD(r, s);
    } else {
        // We can't move a double-precision FP register into a 32-bit GP
        // register, so assert that no calling code is trying to do that.
        NanoAssert(0);
    }
}

Register
Assembler::asm_binop_rhs_reg(LIns*)
{
    return deprecated_UnknownReg;
}

/**
 * copy 64 bits: (rd+dd) <- (rs+ds)
 */
void
Assembler::asm_mmq(Register rd, int dd, Register rs, int ds)
{
    // The value is either a 64bit struct or maybe a float that isn't live in
    // an FPU reg.  Either way, don't put it in an FPU reg just to load & store
    // it.
    // This operation becomes a simple 64-bit memcpy.

    // In order to make the operation optimal, we will require two GP
    // registers. We can't allocate a register here because the caller may have
    // called deprecated_freeRsrcOf, and allocating a register here may cause something
    // else to spill onto the stack which has just be conveniently freed by
    // deprecated_freeRsrcOf (resulting in stack corruption).
    //
    // Falling back to a single-register implementation of asm_mmq is better
    // than adjusting the callers' behaviour (to allow us to allocate another
    // register here) because spilling a register will end up being slower than
    // just using the same register twice anyway.
    //
    // Thus, if there is a free register which we can borrow, we will emit the
    // following code:
    //  LDR rr, [rs, #ds]
    //  LDR ip, [rs, #(ds+4)]
    //  STR rr, [rd, #dd]
    //  STR ip, [rd, #(dd+4)]
    // (Where rr is the borrowed register.)
    //
    // If there is no free register, don't spill an existing allocation. Just
    // do the following:
    //  LDR ip, [rs, #ds]
    //  STR ip, [rd, #dd]
    //  LDR ip, [rs, #(ds+4)]
    //  STR ip, [rd, #(dd+4)]
    //
    // Note that if rs+4 or rd+4 is outside the LDR or STR range, extra
    // instructions will be emitted as required to make the code work.

    // Ensure that the PC is not used as either base register. The instruction
    // generation macros call underrunProtect, and a side effect of this is
    // that we may be pushed onto another page, so the PC is not a reliable
    // base register.
    NanoAssert(rs != PC);
    NanoAssert(rd != PC);

    // We use IP as a swap register, so check that it isn't used for something
    // else by the caller.
    NanoAssert(rs != IP);
    NanoAssert(rd != IP);

    // Find the list of free registers from the allocator's free list and the
    // GpRegs mask. This excludes any floating-point registers that may be on
    // the free list.
    RegisterMask    free = _allocator.free & AllowableFlagRegs;

    // Ensure that ds and dd are within the +/-4095 offset range of STR and
    // LDR. If either is out of range, adjust and modify rd or rs so that the
    // load works correctly.
    // The modification here is performed after the LDR/STR block (because code
    // is emitted backwards), so this one is the reverse operation.

    int32_t dd_adj = 0;
    int32_t ds_adj = 0;

    if ((dd+4) >= 0x1000) {
        dd_adj = ((dd+4) & ~0xfff);
    } else if (dd <= -0x1000) {
        dd_adj = -((-dd) & ~0xfff);
    }
    if ((ds+4) >= 0x1000) {
        ds_adj = ((ds+4) & ~0xfff);
    } else if (ds <= -0x1000) {
        ds_adj = -((-ds) & ~0xfff);
    }

    // These will emit no code if d*_adj is 0.
    asm_sub_imm(rd, rd, dd_adj);
    asm_sub_imm(rs, rs, ds_adj);

    ds -= ds_adj;
    dd -= dd_adj;

    if (free) {
        // There is at least one register on the free list, so grab one for
        // temporary use. There is no need to allocate it explicitly because
        // we won't need it after this function returns.

        // The CountLeadingZeroes utility can be used to quickly find a set bit
        // in the free mask.
        Register    rr = (Register)(31-CountLeadingZeroes(free));

        // Note: Not every register in GpRegs is usable here. However, these
        // registers will never appear on the free list.
        NanoAssert((free & rmask(PC)) == 0);
        NanoAssert((free & rmask(LR)) == 0);
        NanoAssert((free & rmask(SP)) == 0);
        NanoAssert((free & rmask(IP)) == 0);
        NanoAssert((free & rmask(FP)) == 0);

        // Emit the actual instruction sequence.
        STR(IP, rd, dd+4);
        STR(rr, rd, dd);
        LDR(IP, rs, ds+4);
        LDR(rr, rs, ds);
    } else {
        // There are no free registers, so fall back to using IP twice.
        STR(IP, rd, dd+4);
        LDR(IP, rs, ds+4);
        STR(IP, rd, dd);
        LDR(IP, rs, ds);
    }

    // Re-adjust the base registers. (These will emit no code if d*_adj is 0.
    asm_add_imm(rd, rd, dd_adj);
    asm_add_imm(rs, rs, ds_adj);
}

// Increment the 32-bit profiling counter at pCtr, without
// changing any registers.
verbose_only(
void Assembler::asm_inc_m32(uint32_t* pCtr)
{
    // We need to temporarily free up two registers to do this, so
    // just push r0 and r1 on the stack.  This assumes that the area
    // at r13 - 8 .. r13 - 1 isn't being used for anything else at
    // this point.  This guaranteed us by the EABI; although the
    // situation with the legacy ABI I'm not sure of.
    //
    // Plan: emit the following bit of code.  It's not efficient, but
    // this is for profiling debug builds only, and is self contained,
    // except for above comment re stack use.
    //
    // E92D0003                 push    {r0,r1}
    // E59F0000                 ldr     r0, [r15]   ; pCtr
    // EA000000                 b       .+8         ; jump over imm
    // 12345678                 .word   0x12345678  ; pCtr
    // E5901000                 ldr     r1, [r0]
    // E2811001                 add     r1, r1, #1
    // E5801000                 str     r1, [r0]
    // E8BD0003                 pop     {r0,r1}

    // We need keep the 4 words beginning at "ldr r0, [r15]"
    // together.  Simplest to underrunProtect the whole thing.
    underrunProtect(8*4);
    IMM32(0xE8BD0003);       //  pop     {r0,r1}
    IMM32(0xE5801000);       //  str     r1, [r0]
    IMM32(0xE2811001);       //  add     r1, r1, #1
    IMM32(0xE5901000);       //  ldr     r1, [r0]
    IMM32((uint32_t)pCtr);   //  .word   pCtr
    IMM32(0xEA000000);       //  b       .+8
    IMM32(0xE59F0000);       //  ldr     r0, [r15]
    IMM32(0xE92D0003);       //  push    {r0,r1}
}
)

void
Assembler::nativePageReset()
{
    _nSlot = 0;
    _nExitSlot = 0;
}

void
Assembler::nativePageSetup()
{
    NanoAssert(!_inExit);
    if (!_nIns)
        codeAlloc(codeStart, codeEnd, _nIns verbose_only(, codeBytes));

    // constpool starts at top of page and goes down,
    // code starts at bottom of page and moves up
    if (!_nSlot)
        _nSlot = codeStart;
}


void
Assembler::underrunProtect(int bytes)
{
    NanoAssertMsg(bytes<=LARGEST_UNDERRUN_PROT, "constant LARGEST_UNDERRUN_PROT is too small");
    NanoAssert(_nSlot != 0 && int(_nIns)-int(_nSlot) <= 4096);
    uintptr_t top = uintptr_t(_nSlot);
    uintptr_t pc = uintptr_t(_nIns);
    if (pc - bytes < top)
    {
        verbose_only(verbose_outputf("        %p:", _nIns);)
        NIns* target = _nIns;
        // This may be in a normal code chunk or an exit code chunk.
        codeAlloc(codeStart, codeEnd, _nIns verbose_only(, codeBytes));

        _nSlot = codeStart;

        // _nSlot points to the first empty position in the new code block
        // _nIns points just past the last empty position.
        // Assume B_nochk won't ever try to write to _nSlot. See B_cond_chk macro.
        B_nochk(target);
    }
}

void
Assembler::JMP_far(NIns* addr)
{
    // Even if a simple branch is all that is required, this function must emit
    // two words so that the branch can be arbitrarily patched later on.
    underrunProtect(8);

    intptr_t offs = PC_OFFSET_FROM(addr,_nIns-2);

    if (isS24(offs>>2)) {
        // Emit a BKPT to ensure that we reserve enough space for a full 32-bit
        // branch patch later on. The BKPT should never be executed.
        BKPT_nochk();

        asm_output("bkpt");

        // B [PC+offs]
        *(--_nIns) = (NIns)( COND_AL | (0xA<<24) | ((offs>>2) & 0xFFFFFF) );

        asm_output("b %p", (void*)addr);
    } else {
        // Insert the target address as a constant in the instruction stream.
        *(--_nIns) = (NIns)((addr));
        // ldr pc, [pc, #-4] // load the address into pc, reading it from [pc-4] (e.g.,
        // the next instruction)
        *(--_nIns) = (NIns)( COND_AL | (0x51<<20) | (PC<<16) | (PC<<12) | (4));

        asm_output("ldr pc, =%p", (void*)addr);
    }
}

// Perform a branch with link, and ARM/Thumb exchange if necessary. The actual
// BLX instruction is only available from ARMv5 onwards, but as we don't
// support anything older than that this function will not attempt to output
// pre-ARMv5 sequences.
//
// Note: This function is not designed to be used with branches which will be
// patched later, though it will work if the patcher knows how to patch the
// generated instruction sequence.
void
Assembler::BranchWithLink(NIns* addr)
{
    // Most branches emitted by TM are loaded through a register, so always
    // reserve enough space for the LDR sequence. This should give us a slight
    // net gain over reserving the exact amount required for shorter branches.
    // This _must_ be called before PC_OFFSET_FROM as it can move _nIns!
    underrunProtect(4+LD32_size);

    // Calculate the offset from the instruction that is about to be
    // written (at _nIns-1) to the target.
    intptr_t offs = PC_OFFSET_FROM(addr,_nIns-1);

    // ARMv5 and above can use BLX <imm> for branches within ±32MB of the
    // PC and BLX Rm for long branches.
    if (isS24(offs>>2)) {
        // the value we need to stick in the instruction; masked,
        // because it will be sign-extended back to 32 bits.
        intptr_t offs2 = (offs>>2) & 0xffffff;

        if (((intptr_t)addr & 1) == 0) {
            // The target is ARM, so just emit a BL.

            // BL target
            *(--_nIns) = (NIns)( (COND_AL) | (0xB<<24) | (offs2) );
            asm_output("bl %p", (void*)addr);
        } else {
            // The target is Thumb, so emit a BLX.

            // We need to emit an ARMv5+ instruction, so assert that we have a
            // suitable processor. Note that we don't support ARMv4(T), but
            // this serves as a useful sanity check.
            NanoAssert(_config.arm_arch >= 5);

            // The (pre-shifted) value of the "H" bit in the BLX encoding.
            uint32_t    H = (offs & 0x2) << 23;

            // BLX addr
            *(--_nIns) = (NIns)( (0xF << 28) | (0x5<<25) | (H) | (offs2) );
            asm_output("blx %p", (void*)addr);
        }
    } else {
        // Load the target address into IP and branch to that. We've already
        // done underrunProtect, so we can skip that here.
        BLX(IP, false);

        // LDR IP, =addr
        asm_ld_imm(IP, (int32_t)addr, false);
    }
}

// This is identical to BranchWithLink(NIns*) but emits a branch to an address
// held in a register rather than a literal address.
inline void
Assembler::BLX(Register addr, bool chk /* = true */)
{
    // We need to emit an ARMv5+ instruction, so assert that we have a suitable
    // processor. Note that we don't support ARMv4(T), but this serves as a
    // useful sanity check.
    NanoAssert(_config.arm_arch >= 5);

    NanoAssert(IsGpReg(addr));
    // There is a bug in the WinCE device emulator which stops "BLX LR" from
    // working as expected. Assert that we never do that!
    if (blx_lr_bug) { NanoAssert(addr != LR); }

    if (chk) {
        underrunProtect(4);
    }

    // BLX IP
    *(--_nIns) = (NIns)( (COND_AL) | (0x12<<20) | (0xFFF<<8) | (0x3<<4) | (addr) );
    asm_output("blx ip");
}

// Emit the code required to load a memory address into a register as follows:
// d = *(b+off)
// underrunProtect calls from this function can be disabled by setting chk to
// false. However, this function can use more than LD32_size bytes of space if
// the offset is out of the range of a LDR instruction; the maximum space this
// function requires for underrunProtect is 4+LD32_size.
void
Assembler::asm_ldr_chk(Register d, Register b, int32_t off, bool chk)
{
    if (_config.arm_vfp && IsFpReg(d)) {
        FLDD_chk(d,b,off,chk);
        return;
    }

    NanoAssert(IsGpReg(d));
    NanoAssert(IsGpReg(b));

    // We can't use underrunProtect if the base register is the PC because
    // underrunProtect might move the PC if there isn't enough space on the
    // current page.
    NanoAssert((b != PC) || (!chk));

    if (isU12(off)) {
        // LDR d, b, #+off
        if (chk) underrunProtect(4);
        *(--_nIns) = (NIns)( COND_AL | (0x59<<20) | (b<<16) | (d<<12) | off );
    } else if (isU12(-off)) {
        // LDR d, b, #-off
        if (chk) underrunProtect(4);
        *(--_nIns) = (NIns)( COND_AL | (0x51<<20) | (b<<16) | (d<<12) | -off );
    } else {
        // The offset is over 4096 (and outside the range of LDR), so we need
        // to add a level of indirection to get the address into IP.

        // Because of that, we can't do a PC-relative load unless it fits within
        // the single-instruction forms above.

        NanoAssert(b != PC);
        NanoAssert(b != IP);

        if (chk) underrunProtect(4+LD32_size);

        *(--_nIns) = (NIns)( COND_AL | (0x79<<20) | (b<<16) | (d<<12) | IP );
        asm_ld_imm(IP, off, false);
    }

    asm_output("ldr %s, [%s, #%d]",gpn(d),gpn(b),(off));
}

// Emit a store, using a register base and an arbitrary immediate offset. This
// behaves like a STR instruction, but doesn't care about the offset range, and
// emits one of the following instruction sequences:
//
// ----
// STR  rt, [rr, #offset]
// ----
// asm_add_imm  ip, rr, #(offset & ~0xfff)
// STR  rt, [ip, #(offset & 0xfff)]
// ----
// # This one's fairly horrible, but should be rare.
// asm_add_imm  rr, rr, #(offset & ~0xfff)
// STR  rt, [ip, #(offset & 0xfff)]
// asm_sub_imm  rr, rr, #(offset & ~0xfff)
// ----
// SUB-based variants (for negative offsets) are also supported.
// ----
//
// The return value is 1 if a simple STR could be emitted, or 0 if the required
// sequence was more complex.
int32_t
Assembler::asm_str(Register rt, Register rr, int32_t offset)
{
    // We can't do PC-relative stores, and we can't store the PC value, because
    // we use macros (such as STR) which call underrunProtect, and this can
    // push _nIns to a new page, thus making any PC value impractical to
    // predict.
    NanoAssert(rr != PC);
    NanoAssert(rt != PC);
    if (offset >= 0) {
        // The offset is positive, so use ADD (and variants).
        if (isU12(offset)) {
            STR(rt, rr, offset);
            return 1;
        }

        if (rt != IP) {
            STR(rt, IP, offset & 0xfff);
            asm_add_imm(IP, rr, offset & ~0xfff);
        } else {
            int32_t adj = offset & ~0xfff;
            asm_sub_imm(rr, rr, adj);
            STR(rt, rr, offset-adj);
            asm_add_imm(rr, rr, adj);
        }
    } else {
        // The offset is negative, so use SUB (and variants).
        if (isU12(-offset)) {
            STR(rt, rr, offset);
            return 1;
        }

        if (rt != IP) {
            STR(rt, IP, -((-offset) & 0xfff));
            asm_sub_imm(IP, rr, (-offset) & ~0xfff);
        } else {
            int32_t adj = ((-offset) & ~0xfff);
            asm_add_imm(rr, rr, adj);
            STR(rt, rr, offset+adj);
            asm_sub_imm(rr, rr, adj);
        }
    }

    return 0;
}

// Emit the code required to load an immediate value (imm) into general-purpose
// register d. Optimal (MOV-based) mechanisms are used if the immediate can be
// encoded using ARM's operand 2 encoding. Otherwise, a slot is used on the
// literal pool and LDR is used to load the value.
//
// chk can be explicitly set to false in order to disable underrunProtect calls
// from this function; this allows the caller to perform the check manually.
// This function guarantees not to use more than LD32_size bytes of space.
void
Assembler::asm_ld_imm(Register d, int32_t imm, bool chk /* = true */)
{
    uint32_t    op2imm;

    NanoAssert(IsGpReg(d));

    // Attempt to encode the immediate using the second operand of MOV or MVN.
    // This is the simplest solution and generates the shortest and fastest
    // code, but can only encode a limited set of values.

    if (encOp2Imm(imm, &op2imm)) {
        // Use MOV to encode the literal.
        MOVis(d, op2imm, 0);
        return;
    }

    if (encOp2Imm(~imm, &op2imm)) {
        // Use MVN to encode the inverted literal.
        MVNis(d, op2imm, 0);
        return;
    }

    // Try to use simple MOV, MVN or MOV(W|T) instructions to load the
    // immediate. If this isn't possible, load it from memory.
    //  - We cannot use MOV(W|T) on cores older than the introduction of
    //    Thumb-2 or if the target register is the PC.
    //
    // (Note that we use Thumb-2 if arm_arch is ARMv7 or later; the only earlier
    // ARM core that provided Thumb-2 is ARMv6T2/ARM1156, which is a real-time
    // core that nanojit is unlikely to ever target.)
    if (_config.arm_arch >= 7 && (d != PC)) {
        // ARMv6T2 and above have MOVW and MOVT.
        uint32_t    high_h = (uint32_t)imm >> 16;
        uint32_t    low_h = imm & 0xffff;

        if (high_h != 0) {
            // Load the high half-word (if necessary).
            MOVTi_chk(d, high_h, chk);
        }
        // Load the low half-word. This also zeroes the high half-word, and
        // thus must execute _before_ MOVT, and is necessary even if low_h is 0
        // because MOVT will not change the existing low half-word.
        MOVWi_chk(d, low_h, chk);

        return;
    }

    // We couldn't encode the literal in the instruction stream, so load it
    // from memory.

    // Because the literal pool is on the same page as the generated code, it
    // will almost always be within the ±4096 range of a LDR. However, this may
    // not be the case if _nSlot is at the start of the page and _nIns is at
    // the end because the PC is 8 bytes ahead of _nIns. This is unlikely to
    // happen, but if it does occur we can simply waste a word or two of
    // literal space.

    // We must do the underrunProtect before PC_OFFSET_FROM as underrunProtect
    // can move the PC if there isn't enough space on the current page!
    if (chk) {
        underrunProtect(LD32_size);
    }

    int offset = PC_OFFSET_FROM(_nSlot, _nIns-1);
    // If the offset is out of range, waste literal space until it is in range.
    while (offset <= -4096) {
        ++_nSlot;
        offset += sizeof(_nSlot);
    }
    NanoAssert((isU12(-offset) || isU12(offset)) && (offset <= -8));

    // Write the literal.
    *(_nSlot++) = imm;
    asm_output("## imm= 0x%x", imm);

    // Load the literal.
    LDR_nochk(d,PC,offset);
    NanoAssert(uintptr_t(_nIns) + 8 + offset == uintptr_t(_nSlot-1));
    NanoAssert(*((int32_t*)_nSlot-1) == imm);
}

// Branch to target address _t with condition _c, doing underrun
// checks (_chk == 1) or skipping them (_chk == 0).
//
// Set the target address (_t) to 0 if the target is not yet known and the
// branch will be patched up later.
//
// If the jump is to a known address (with _t != 0) and it fits in a relative
// jump (±32MB), emit that.
// If the jump is unconditional, emit the dest address inline in
// the instruction stream and load it into pc.
// If the jump has a condition, but noone's mucked with _nIns and our _nSlot
// pointer is valid, stick the constant in the slot and emit a conditional
// load into pc.
// Otherwise, emit the conditional load into pc from a nearby constant,
// and emit a jump to jump over it it in case the condition fails.
//
// NB: B_nochk depends on this not calling samepage() when _c == AL
void
Assembler::B_cond_chk(ConditionCode _c, NIns* _t, bool _chk)
{
    int32_t offs = PC_OFFSET_FROM(_t,_nIns-1);
    //nj_dprintf("B_cond_chk target: 0x%08x offset: %d @0x%08x\n", _t, offs, _nIns-1);

    // optimistically check if this will fit in 24 bits
    if (_chk && isS24(offs>>2) && (_t != 0)) {
        underrunProtect(4);
        // recalculate the offset, because underrunProtect may have
        // moved _nIns to a new page
        offs = PC_OFFSET_FROM(_t,_nIns-1);
    }

    // Emit one of the following patterns:
    //
    //  --- Short branch. This can never be emitted if the branch target is not
    //      known.
    //          B(cc)   ±32MB
    //
    //  --- Long unconditional branch.
    //          LDR     PC, #lit
    //  lit:    #target
    //
    //  --- Long conditional branch. Note that conditional branches will never
    //      be patched, so the nPatchBranch function doesn't need to know where
    //      the literal pool is located.
    //          LDRcc   PC, #lit
    //          ; #lit is in the literal pool at _nSlot
    //
    //  --- Long conditional branch (if the slot isn't on the same page as the instruction).
    //          LDRcc   PC, #lit
    //          B       skip        ; Jump over the literal data.
    //  lit:    #target
    //  skip:   [...]

    if (isS24(offs>>2) && (_t != 0)) {
        // The underrunProtect for this was done above (if required by _chk).
        *(--_nIns) = (NIns)( ((_c)<<28) | (0xA<<24) | (((offs)>>2) & 0xFFFFFF) );
        asm_output("b%s %p", _c == AL ? "" : condNames[_c], (void*)(_t));
    } else if (_c == AL) {
        if(_chk) underrunProtect(8);
        *(--_nIns) = (NIns)(_t);
        *(--_nIns) = (NIns)( COND_AL | (0x51<<20) | (PC<<16) | (PC<<12) | 0x4 );
        asm_output("b%s %p", _c == AL ? "" : condNames[_c], (void*)(_t));
    } else if (PC_OFFSET_FROM(_nSlot, _nIns-1) > -0x1000) {
        if(_chk) underrunProtect(8);
        *(_nSlot++) = (NIns)(_t);
        offs = PC_OFFSET_FROM(_nSlot-1,_nIns-1);
        NanoAssert(offs < 0);
        *(--_nIns) = (NIns)( ((_c)<<28) | (0x51<<20) | (PC<<16) | (PC<<12) | ((-offs) & 0xFFF) );
        asm_output("ldr%s %s, [%s, #-%d]", condNames[_c], gpn(PC), gpn(PC), -offs);
        NanoAssert(uintptr_t(_nIns)+8+offs == uintptr_t(_nSlot-1));
    } else {
        if(_chk) underrunProtect(12);
        // Emit a pointer to the target as a literal in the instruction stream.
        *(--_nIns) = (NIns)(_t);
        // Emit a branch to skip over the literal. The PC value is 8 bytes
        // ahead of the executing instruction, so to branch two instructions
        // forward this must branch 8-8=0 bytes.
        *(--_nIns) = (NIns)( COND_AL | (0xA<<24) | 0x0 );
        // Emit the conditional branch.
        *(--_nIns) = (NIns)( ((_c)<<28) | (0x51<<20) | (PC<<16) | (PC<<12) | 0x0 );
        asm_output("b%s %p", _c == AL ? "" : condNames[_c], (void*)(_t));
    }
}

/*
 * VFP
 */

void
Assembler::asm_i2d(LIns* ins)
{
    Register rr = deprecated_prepResultReg(ins, FpRegs);
    Register srcr = findRegFor(ins->oprnd1(), GpRegs);

    // todo: support int value in memory, as per x86
    NanoAssert(deprecated_isKnownReg(srcr));

    FSITOD(rr, S14);
    FMSR(S14, srcr);
}

void
Assembler::asm_ui2d(LIns* ins)
{
    Register rr = deprecated_prepResultReg(ins, FpRegs);
    Register sr = findRegFor(ins->oprnd1(), GpRegs);

    // todo: support int value in memory, as per x86
    NanoAssert(deprecated_isKnownReg(sr));

    FUITOD(rr, S14);
    FMSR(S14, sr);
}

void Assembler::asm_d2i(LIns* ins)
{
    // where our result goes
    Register rr = deprecated_prepResultReg(ins, GpRegs);
    Register sr = findRegFor(ins->oprnd1(), FpRegs);

    FMRS(rr, S14);
    FTOSID(S14, sr);
}

void
Assembler::asm_fneg(LIns* ins)
{
    LIns* lhs = ins->oprnd1();
    Register rr = deprecated_prepResultReg(ins, FpRegs);

    Register sr = ( !lhs->isInReg()
                  ? findRegFor(lhs, FpRegs)
                  : lhs->deprecated_getReg() );

    FNEGD(rr, sr);
}

void
Assembler::asm_fop(LIns* ins)
{
    LIns* lhs = ins->oprnd1();
    LIns* rhs = ins->oprnd2();
    LOpcode op = ins->opcode();

    // rr = ra OP rb

    Register rr = deprecated_prepResultReg(ins, FpRegs);

    Register ra = findRegFor(lhs, FpRegs);
    Register rb = (rhs == lhs) ? ra : findRegFor(rhs, FpRegs & ~rmask(ra));

    // XXX special-case 1.0 and 0.0

    switch (op)
    {
        case LIR_addd:      FADDD(rr,ra,rb);    break;
        case LIR_subd:      FSUBD(rr,ra,rb);    break;
        case LIR_muld:      FMULD(rr,ra,rb);    break;
        case LIR_divd:      FDIVD(rr,ra,rb);    break;
        default:            NanoAssert(0);      break;
    }
}

void
Assembler::asm_cmpd(LIns* ins)
{
    LIns* lhs = ins->oprnd1();
    LIns* rhs = ins->oprnd2();
    LOpcode op = ins->opcode();

    NanoAssert(isCmpDOpcode(op));

    Register ra, rb;
    findRegFor2(FpRegs, lhs, ra, FpRegs, rhs, rb);

    int e_bit = (op != LIR_eqd);

    // do the comparison and get results loaded in ARM status register
    FMSTAT();
    FCMPD(ra, rb, e_bit);
}

/* Call this with targ set to 0 if the target is not yet known and the branch
 * will be patched up later.
 */
NIns*
Assembler::asm_branch(bool branchOnFalse, LIns* cond, NIns* targ)
{
    LOpcode condop = cond->opcode();
    NanoAssert(cond->isCmp());
    NanoAssert(_config.arm_vfp || !isCmpDOpcode(condop));

    // The old "never" condition code has special meaning on newer ARM cores,
    // so use "always" as a sensible default code.
    ConditionCode cc = AL;

    // Detect whether or not this is a floating-point comparison.
    bool    fp_cond;

    // Select the appropriate ARM condition code to match the LIR instruction.
    switch (condop)
    {
        // Floating-point conditions. Note that the VFP LT/LE conditions
        // require use of the unsigned condition codes, even though
        // float-point comparisons are always signed.
        case LIR_eqd:   cc = EQ;    fp_cond = true;     break;
        case LIR_ltd:   cc = LO;    fp_cond = true;     break;
        case LIR_led:   cc = LS;    fp_cond = true;     break;
        case LIR_ged:   cc = GE;    fp_cond = true;     break;
        case LIR_gtd:   cc = GT;    fp_cond = true;     break;

        // Standard signed and unsigned integer comparisons.
        case LIR_eqi:   cc = EQ;    fp_cond = false;    break;
        case LIR_lti:   cc = LT;    fp_cond = false;    break;
        case LIR_lei:   cc = LE;    fp_cond = false;    break;
        case LIR_gti:   cc = GT;    fp_cond = false;    break;
        case LIR_gei:   cc = GE;    fp_cond = false;    break;
        case LIR_ltui:  cc = LO;    fp_cond = false;    break;
        case LIR_leui:  cc = LS;    fp_cond = false;    break;
        case LIR_gtui:  cc = HI;    fp_cond = false;    break;
        case LIR_geui:  cc = HS;    fp_cond = false;    break;

        // Default case for invalid or unexpected LIR instructions.
        default:        cc = AL;    fp_cond = false;    break;
    }

    // Invert the condition if required.
    if (branchOnFalse)
        cc = OppositeCond(cc);

    // Ensure that we got a sensible condition code.
    NanoAssert((cc != AL) && (cc != NV));

    // Ensure that we don't hit floating-point LIR codes if VFP is disabled.
    NanoAssert(_config.arm_vfp || !fp_cond);

    // Emit a suitable branch instruction.
    B_cond(cc, targ);

    // Store the address of the branch instruction so that we can return it.
    // asm_[f]cmp will move _nIns so we must do this now.
    NIns *at = _nIns;

    if (_config.arm_vfp && fp_cond)
        asm_cmpd(cond);
    else
        asm_cmp(cond);

    return at;
}

NIns* Assembler::asm_branch_ov(LOpcode op, NIns* target)
{
    // Because MUL can't set the V flag, we use SMULL and CMP to set the Z flag
    // to detect overflow on multiply. Thus, if we have a LIR_mulxovi, we must
    // be conditional on !Z, not V.
    ConditionCode cc = ( (op == LIR_mulxovi) || (op == LIR_muljovi) ? NE : VS );

    // Emit a suitable branch instruction.
    B_cond(cc, target);
    return _nIns;
}

void
Assembler::asm_cmp(LIns *cond)
{
    LIns* lhs = cond->oprnd1();
    LIns* rhs = cond->oprnd2();

    NanoAssert(lhs->isI() && rhs->isI());

    // ready to issue the compare
    if (rhs->isImmI()) {
        int c = rhs->immI();
        Register r = findRegFor(lhs, GpRegs);
        if (c == 0 && cond->isop(LIR_eqi)) {
            TST(r, r);
        } else {
            asm_cmpi(r, c);
        }
    } else {
        Register ra, rb;
        findRegFor2(GpRegs, lhs, ra, GpRegs, rhs, rb);
        CMP(ra, rb);
    }
}

void
Assembler::asm_cmpi(Register r, int32_t imm)
{
    if (imm < 0) {
        if (imm > -256) {
            ALUi(AL, cmn, 1, 0, r, -imm);
        } else {
            underrunProtect(4 + LD32_size);
            CMP(r, IP);
            asm_ld_imm(IP, imm);
        }
    } else {
        if (imm < 256) {
            ALUi(AL, cmp, 1, 0, r, imm);
        } else {
            underrunProtect(4 + LD32_size);
            CMP(r, IP);
            asm_ld_imm(IP, imm);
        }
    }
}

void
Assembler::asm_condd(LIns* ins)
{
    // only want certain regs
    Register r = deprecated_prepResultReg(ins, AllowableFlagRegs);

    switch (ins->opcode()) {
        case LIR_eqd: SETEQ(r); break;
        case LIR_ltd: SETLO(r); break; // } note: VFP LT/LE operations require use of
        case LIR_led: SETLS(r); break; // } unsigned LO/LS condition codes!
        case LIR_ged: SETGE(r); break;
        case LIR_gtd: SETGT(r); break;
        default: NanoAssert(0); break;
    }

    asm_cmpd(ins);
}

void
Assembler::asm_cond(LIns* ins)
{
    Register r = deprecated_prepResultReg(ins, AllowableFlagRegs);
    LOpcode op = ins->opcode();

    switch(op)
    {
        case LIR_eqi:  SETEQ(r); break;
        case LIR_lti:  SETLT(r); break;
        case LIR_lei:  SETLE(r); break;
        case LIR_gti:  SETGT(r); break;
        case LIR_gei:  SETGE(r); break;
        case LIR_ltui: SETLO(r); break;
        case LIR_leui: SETLS(r); break;
        case LIR_gtui: SETHI(r); break;
        case LIR_geui: SETHS(r); break;
        default:      NanoAssert(0);  break;
    }
    asm_cmp(ins);
}

void
Assembler::asm_arith(LIns* ins)
{
    LOpcode op = ins->opcode();
    LIns*   lhs = ins->oprnd1();
    LIns*   rhs = ins->oprnd2();

    RegisterMask    allow = GpRegs;

    // We always need the result register and the first operand register.
    Register        rr = deprecated_prepResultReg(ins, allow);

    // If this is the last use of lhs in reg, we can re-use the result reg.
    // Else, lhs already has a register assigned.
    Register        ra = ( !lhs->isInReg()
                         ? findSpecificRegFor(lhs, rr)
                         : lhs->deprecated_getReg() );

    // Don't re-use the registers we've already allocated.
    NanoAssert(deprecated_isKnownReg(rr));
    NanoAssert(deprecated_isKnownReg(ra));
    allow &= ~rmask(rr);
    allow &= ~rmask(ra);

    // If the rhs is constant, we can use the instruction-specific code to
    // determine if the value can be encoded in an ARM instruction. If the
    // value cannot be encoded, it will be loaded into a register.
    //
    // Note that the MUL instruction can never take an immediate argument so
    // even if the argument is constant, we must allocate a register for it.
    //
    // Note: It is possible to use a combination of the barrel shifter and the
    // basic arithmetic instructions to generate constant multiplications.
    // However, LIR_muli is never invoked with a constant during
    // trace-tests.js so it is very unlikely to be worthwhile implementing it.
    if (rhs->isImmI() && (op != LIR_muli) && (op != LIR_mulxovi) && (op != LIR_muljovi))
    {
        if ((op == LIR_addi || op == LIR_addxovi) && lhs->isop(LIR_allocp)) {
            // Add alloc+const. The result should be the address of the
            // allocated space plus a constant.
            Register    rs = deprecated_prepResultReg(ins, allow);
            int         d = findMemFor(lhs) + rhs->immI();

            NanoAssert(deprecated_isKnownReg(rs));
            asm_add_imm(rs, FP, d);
        }

        int32_t immI = rhs->immI();

        switch (op)
        {
            case LIR_addi:       asm_add_imm(rr, ra, immI);     break;
            case LIR_addjovi:
            case LIR_addxovi:    asm_add_imm(rr, ra, immI, 1);  break;
            case LIR_subi:       asm_sub_imm(rr, ra, immI);     break;
            case LIR_subjovi:
            case LIR_subxovi:    asm_sub_imm(rr, ra, immI, 1);  break;
            case LIR_andi:       asm_and_imm(rr, ra, immI);     break;
            case LIR_ori:        asm_orr_imm(rr, ra, immI);     break;
            case LIR_xori:       asm_eor_imm(rr, ra, immI);     break;
            case LIR_lshi:       LSLi(rr, ra, immI);            break;
            case LIR_rshi:       ASRi(rr, ra, immI);            break;
            case LIR_rshui:      LSRi(rr, ra, immI);            break;

            default:
                NanoAssertMsg(0, "Unsupported");
                break;
        }

        // We've already emitted an instruction, so return now.
        return;
    }

    // The rhs is either a register or cannot be encoded as a constant.

    Register rb;
    if (lhs == rhs) {
        rb = ra;
    } else {
        rb = asm_binop_rhs_reg(ins);
        if (!deprecated_isKnownReg(rb))
            rb = findRegFor(rhs, allow);
        allow &= ~rmask(rb);
    }
    NanoAssert(deprecated_isKnownReg(rb));

    const Register SBZ = (Register)0;
    switch (op)
    {
        case LIR_addi:       ADDs(rr, ra, rb, 0);    break;
        case LIR_addjovi:
        case LIR_addxovi:    ADDs(rr, ra, rb, 1);    break;
        case LIR_subi:       SUBs(rr, ra, rb, 0);    break;
        case LIR_subjovi:
        case LIR_subxovi:    SUBs(rr, ra, rb, 1);    break;
        case LIR_andi:       ANDs(rr, ra, rb, 0);    break;
        case LIR_ori:        ORRs(rr, ra, rb, 0);    break;
        case LIR_xori:       EORs(rr, ra, rb, 0);    break;

        // XXX: LIR_muli can be done more efficiently than LIR_mulxovi.  See bug 542629.
        case LIR_muli:
        case LIR_muljovi:
        case LIR_mulxovi:
            // ARMv5 and earlier cores cannot do a MUL where the first operand
            // is also the result, so we need a special case to handle that.
            //
            // We try to use rb as the first operand by default because it is
            // common for (rr == ra) and is thus likely to be the most
            // efficient method.

            if ((_config.arm_arch > 5) || (rr != rb)) {
                // IP is used to temporarily store the high word of the result from
                // SMULL, so we make use of this to perform an overflow check, as
                // ARM's MUL instruction can't set the overflow flag by itself.
                // We can check for overflow using the following:
                //   SMULL  rr, ip, ra, rb
                //   CMP    ip, rr, ASR #31
                // An explanation can be found in bug 521161. This sets Z if we did
                // _not_ overflow, and clears it if we did.
                ALUr_shi(AL, cmp, 1, SBZ, IP, rr, ASR_imm, 31);
                SMULL(rr, IP, rb, ra);
            } else {
                // _config.arm_arch is ARMv5 (or below) and rr == rb, so we must
                // find a different way to encode the instruction.

                // If possible, swap the arguments to avoid the restriction.
                if (rr != ra) {
                    // We know that rr == rb, so this will be something like
                    // rX = rY * rX.
                    // Other than swapping ra and rb, this works in the same as
                    // as the ARMv6+ case, above.
                    ALUr_shi(AL, cmp, 1, SBZ, IP, rr, ASR_imm, 31);
                    SMULL(rr, IP, ra, rb);
                } else {
                    // We're trying to do rX = rX * rX, but we also need to
                    // check for overflow so we would need two extra registers
                    // on ARMv5 and below. We achieve this by observing the
                    // following:
                    //   - abs(rX)*abs(rX) = rX*rX, so we force the input to be
                    //     positive to simplify the detection logic.
                    //   - Any argument greater than 0xffff will _always_
                    //     overflow, and we can easily check that the top 16
                    //     bits are zero.
                    //   - Any argument lower than (or equal to) 0xffff that
                    //     also overflows is guaranteed to set output bit 31.
                    //
                    // Thus, we know we have _not_ overflowed if:
                    //   abs(rX)&0xffff0000 == 0 AND result[31] == 0
                    //
                    // The following instruction sequence will be emitted:
                    // MOVS     IP, rX      // Put abs(rX) into IP.
                    // RSBMI    IP, IP, #0  // ...
                    // MUL      rX, IP, IP  // Do the actual multiplication.
                    // MOVS     IP, IP, LSR #16 // Check that abs(arg)<=0xffff
                    // CMPEQ    IP, rX, ASR #31 // Check that result[31] == 0

                    NanoAssert(rr != IP);

                    ALUr_shi(AL, cmp, 1, SBZ, IP, rr, ASR_imm, 31);
                    ALUr_shi(AL, mov, 1, IP, SBZ, IP, LSR_imm, 16);
                    MUL(rr, IP, IP);
                    ALUi(MI, rsb, 0, IP, IP, 0);
                    ALUr(AL, mov, 1, IP, ra, ra);
                }
            }
            break;

        // The shift operations need a mask to match the JavaScript
        // specification because the ARM architecture allows a greater shift
        // range than JavaScript.
        case LIR_lshi:
            LSL(rr, ra, IP);
            ANDi(IP, rb, 0x1f);
            break;
        case LIR_rshi:
            ASR(rr, ra, IP);
            ANDi(IP, rb, 0x1f);
            break;
        case LIR_rshui:
            LSR(rr, ra, IP);
            ANDi(IP, rb, 0x1f);
            break;
        default:
            NanoAssertMsg(0, "Unsupported");
            break;
    }
}

void
Assembler::asm_neg_not(LIns* ins)
{
    LOpcode op = ins->opcode();
    Register rr = deprecated_prepResultReg(ins, GpRegs);

    LIns* lhs = ins->oprnd1();
    // If this is the last use of lhs in reg, we can re-use result reg.
    // Else, lhs already has a register assigned.
    Register ra = ( !lhs->isInReg()
                  ? findSpecificRegFor(lhs, rr)
                  : lhs->deprecated_getReg() );
    NanoAssert(deprecated_isKnownReg(ra));

    if (op == LIR_noti)
        MVN(rr, ra);
    else
        RSBS(rr, ra);
}

void
Assembler::asm_load32(LIns* ins)
{
    LOpcode op = ins->opcode();
    LIns* base = ins->oprnd1();
    int d = ins->disp();

    Register rr = deprecated_prepResultReg(ins, GpRegs);
    Register ra = getBaseReg(base, d, GpRegs);

    switch (op) {
        case LIR_lduc2ui:
            if (isU12(-d) || isU12(d)) {
                LDRB(rr, ra, d);
            } else {
                LDRB(rr, IP, 0);
                asm_add_imm(IP, ra, d);
            }
            return;
        case LIR_ldus2ui:
            // Some ARM machines require 2-byte alignment here.
            // Similar to the ldcb/ldzb case, but the max offset is smaller.
            if (isU8(-d) || isU8(d)) {
                LDRH(rr, ra, d);
            } else {
                LDRH(rr, IP, 0);
                asm_add_imm(IP, ra, d);
            }
            return;
        case LIR_ldi:
            // Some ARM machines require 4-byte alignment here.
            if (isU12(-d) || isU12(d)) {
                LDR(rr, ra, d);
            } else {
                LDR(rr, IP, 0);
                asm_add_imm(IP, ra, d);
            }
            return;
        case LIR_ldc2i:
            if (isU8(-d) || isU8(d)) {
                LDRSB(rr, ra, d);
            } else {
                LDRSB(rr, IP, 0);
                asm_add_imm(IP, ra, d);
            }
            return;
        case LIR_lds2i:
            if (isU8(-d) || isU8(d)) {
                LDRSH(rr, ra, d);
            } else {
                LDRSH(rr, IP, 0);
                asm_add_imm(IP, ra, d);
            }
            return;
        default:
            NanoAssertMsg(0, "asm_load32 should never receive this LIR opcode");
            return;
    }
}

void
Assembler::asm_cmov(LIns* ins)
{
    LIns* condval = ins->oprnd1();
    LIns* iftrue  = ins->oprnd2();
    LIns* iffalse = ins->oprnd3();

    NanoAssert(condval->isCmp());
    NanoAssert(ins->opcode() == LIR_cmovi && iftrue->isI() && iffalse->isI());

    const Register rr = deprecated_prepResultReg(ins, GpRegs);

    // this code assumes that neither LD nor MR nor MRcc set any of the condition flags.
    // (This is true on Intel, is it true on all architectures?)
    const Register iffalsereg = findRegFor(iffalse, GpRegs & ~rmask(rr));
    switch (condval->opcode()) {
        // note that these are all opposites...
        case LIR_eqi:    MOVNE(rr, iffalsereg);  break;
        case LIR_lti:    MOVGE(rr, iffalsereg);  break;
        case LIR_lei:    MOVGT(rr, iffalsereg);  break;
        case LIR_gti:    MOVLE(rr, iffalsereg);  break;
        case LIR_gei:    MOVLT(rr, iffalsereg);  break;
        case LIR_ltui:   MOVHS(rr, iffalsereg);  break;
        case LIR_leui:   MOVHI(rr, iffalsereg);  break;
        case LIR_gtui:   MOVLS(rr, iffalsereg);  break;
        case LIR_geui:   MOVLO(rr, iffalsereg);  break;
        default: debug_only( NanoAssert(0) );    break;
    }
    /*const Register iftruereg =*/ findSpecificRegFor(iftrue, rr);
    asm_cmp(condval);
}

void
Assembler::asm_qhi(LIns* ins)
{
    Register rr = deprecated_prepResultReg(ins, GpRegs);
    LIns *q = ins->oprnd1();
    int d = findMemFor(q);
    LDR(rr, FP, d+4);
}

void
Assembler::asm_qlo(LIns* ins)
{
    Register rr = deprecated_prepResultReg(ins, GpRegs);
    LIns *q = ins->oprnd1();
    int d = findMemFor(q);
    LDR(rr, FP, d);
}

void
Assembler::asm_param(LIns* ins)
{
    uint32_t a = ins->paramArg();
    uint32_t kind = ins->paramKind();
    if (kind == 0) {
        // ordinary param
        AbiKind abi = _thisfrag->lirbuf->abi;
        uint32_t abi_regcount = abi == ABI_CDECL ? 4 : abi == ABI_FASTCALL ? 2 : abi == ABI_THISCALL ? 1 : 0;
        if (a < abi_regcount) {
            // incoming arg in register
            deprecated_prepResultReg(ins, rmask(argRegs[a]));
        } else {
            // incoming arg is on stack, and EBP points nearby (see genPrologue)
            Register r = deprecated_prepResultReg(ins, GpRegs);
            int d = (a - abi_regcount) * sizeof(intptr_t) + 8;
            LDR(r, FP, d);
        }
    } else {
        // saved param
        deprecated_prepResultReg(ins, rmask(savedRegs[a]));
    }
}

void
Assembler::asm_immi(LIns* ins)
{
    Register rr = deprecated_prepResultReg(ins, GpRegs);
    asm_ld_imm(rr, ins->immI());
}

void
Assembler::asm_ret(LIns *ins)
{
    genEpilogue();

    // NB: our contract with genEpilogue is actually that the return value
    // we are intending for R0 is currently IP, not R0. This has to do with
    // the strange dual-nature of the patchable jump in a side-exit. See
    // nPatchBranch.

    MOV(IP, R0);

    // Pop the stack frame.
    MOV(SP,FP);

    releaseRegisters();
    assignSavedRegs();
    LIns *value = ins->oprnd1();
    if (ins->isop(LIR_reti)) {
        findSpecificRegFor(value, R0);
    }
    else {
        NanoAssert(ins->isop(LIR_retd));
        if (_config.arm_vfp) {
            Register reg = findRegFor(value, FpRegs);
            FMRRD(R0, R1, reg);
        } else {
            NanoAssert(value->isop(LIR_ii2d));
            findSpecificRegFor(value->oprnd1(), R0); // lo
            findSpecificRegFor(value->oprnd2(), R1); // hi
        }
    }
}

void
Assembler::asm_jtbl(LIns* ins, NIns** table)
{
    Register indexreg = findRegFor(ins->oprnd1(), GpRegs);
    Register tmp = registerAllocTmp(GpRegs & ~rmask(indexreg));
    LDR_scaled(PC, tmp, indexreg, 2);      // LDR PC, [tmp + index*4]
    asm_ld_imm(tmp, (int32_t)table);       // tmp = #table
}

void Assembler::swapCodeChunks() {
    if (!_nExitIns)
        codeAlloc(exitStart, exitEnd, _nExitIns verbose_only(, exitBytes));
    if (!_nExitSlot)
        _nExitSlot = exitStart;
    SWAP(NIns*, _nIns, _nExitIns);
    SWAP(NIns*, _nSlot, _nExitSlot);        // this one is ARM-specific
    SWAP(NIns*, codeStart, exitStart);
    SWAP(NIns*, codeEnd, exitEnd);
    verbose_only( SWAP(size_t, codeBytes, exitBytes); )
}

}
#endif /* FEATURE_NANOJIT */
