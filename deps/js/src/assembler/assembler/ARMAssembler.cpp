/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=79:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Copyright (C) 2009 University of Szeged
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY UNIVERSITY OF SZEGED ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL UNIVERSITY OF SZEGED OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * ***** END LICENSE BLOCK ***** */

#include "assembler/wtf/Platform.h"

#if ENABLE_ASSEMBLER && WTF_CPU_ARM_TRADITIONAL

#include "ARMAssembler.h"

namespace JSC {

// Patching helpers

void ARMAssembler::patchConstantPoolLoad(void* loadAddr, void* constPoolAddr)
{
    ARMWord *ldr = reinterpret_cast<ARMWord*>(loadAddr);
    ARMWord diff = reinterpret_cast<ARMWord*>(constPoolAddr) - ldr;
    ARMWord index = (*ldr & 0xfff) >> 1;

    ASSERT(diff >= 1);
    if (diff >= 2 || index > 0) {
        diff = (diff + index - 2) * sizeof(ARMWord);
        ASSERT(diff <= 0xfff);
        *ldr = (*ldr & ~0xfff) | diff;
    } else
        *ldr = (*ldr & ~(0xfff | ARMAssembler::DT_UP)) | sizeof(ARMWord);
}

// Handle immediates

ARMWord ARMAssembler::getOp2(ARMWord imm)
{
    int rol;

    if (imm <= 0xff)
        return OP2_IMM | imm;

    if ((imm & 0xff000000) == 0) {
        imm <<= 8;
        rol = 8;
    }
    else {
        imm = (imm << 24) | (imm >> 8);
        rol = 0;
    }

    if ((imm & 0xff000000) == 0) {
        imm <<= 8;
        rol += 4;
    }

    if ((imm & 0xf0000000) == 0) {
        imm <<= 4;
        rol += 2;
    }

    if ((imm & 0xc0000000) == 0) {
        imm <<= 2;
        rol += 1;
    }

    if ((imm & 0x00ffffff) == 0)
        return OP2_IMM | (imm >> 24) | (rol << 8);

    return INVALID_IMM;
}

int ARMAssembler::genInt(int reg, ARMWord imm, bool positive)
{
    // Step1: Search a non-immediate part
    ARMWord mask;
    ARMWord imm1;
    ARMWord imm2;
    int rol;

    mask = 0xff000000;
    rol = 8;
    while(1) {
        if ((imm & mask) == 0) {
            imm = (imm << rol) | (imm >> (32 - rol));
            rol = 4 + (rol >> 1);
            break;
        }
        rol += 2;
        mask >>= 2;
        if (mask & 0x3) {
            // rol 8
            imm = (imm << 8) | (imm >> 24);
            mask = 0xff00;
            rol = 24;
            while (1) {
                if ((imm & mask) == 0) {
                    imm = (imm << rol) | (imm >> (32 - rol));
                    rol = (rol >> 1) - 8;
                    break;
                }
                rol += 2;
                mask >>= 2;
                if (mask & 0x3)
                    return 0;
            }
            break;
        }
    }

    ASSERT((imm & 0xff) == 0);

    if ((imm & 0xff000000) == 0) {
        imm1 = OP2_IMM | ((imm >> 16) & 0xff) | (((rol + 4) & 0xf) << 8);
        imm2 = OP2_IMM | ((imm >> 8) & 0xff) | (((rol + 8) & 0xf) << 8);
    } else if (imm & 0xc0000000) {
        imm1 = OP2_IMM | ((imm >> 24) & 0xff) | ((rol & 0xf) << 8);
        imm <<= 8;
        rol += 4;

        if ((imm & 0xff000000) == 0) {
            imm <<= 8;
            rol += 4;
        }

        if ((imm & 0xf0000000) == 0) {
            imm <<= 4;
            rol += 2;
        }

        if ((imm & 0xc0000000) == 0) {
            imm <<= 2;
            rol += 1;
        }

        if ((imm & 0x00ffffff) == 0)
            imm2 = OP2_IMM | (imm >> 24) | ((rol & 0xf) << 8);
        else
            return 0;
    } else {
        if ((imm & 0xf0000000) == 0) {
            imm <<= 4;
            rol += 2;
        }

        if ((imm & 0xc0000000) == 0) {
            imm <<= 2;
            rol += 1;
        }

        imm1 = OP2_IMM | ((imm >> 24) & 0xff) | ((rol & 0xf) << 8);
        imm <<= 8;
        rol += 4;

        if ((imm & 0xf0000000) == 0) {
            imm <<= 4;
            rol += 2;
        }

        if ((imm & 0xc0000000) == 0) {
            imm <<= 2;
            rol += 1;
        }

        if ((imm & 0x00ffffff) == 0)
            imm2 = OP2_IMM | (imm >> 24) | ((rol & 0xf) << 8);
        else
            return 0;
    }

    if (positive) {
        mov_r(reg, imm1);
        orr_r(reg, reg, imm2);
    } else {
        mvn_r(reg, imm1);
        bic_r(reg, reg, imm2);
    }

    return 1;
}

#ifdef __GNUC__
// If the result of this function isn't used, the caller should probably be
// using movImm.
__attribute__((warn_unused_result))
#endif
ARMWord ARMAssembler::getImm(ARMWord imm, int tmpReg, bool invert)
{
    ARMWord tmp;

    // Do it by 1 instruction
    tmp = getOp2(imm);
    if (tmp != INVALID_IMM)
        return tmp;

    tmp = getOp2(~imm);
    if (tmp != INVALID_IMM) {
        if (invert)
            return tmp | OP2_INV_IMM;
        mvn_r(tmpReg, tmp);
        return tmpReg;
    }

    return encodeComplexImm(imm, tmpReg);
}

void ARMAssembler::moveImm(ARMWord imm, int dest)
{
    ARMWord tmp;

    // Do it by 1 instruction
    tmp = getOp2(imm);
    if (tmp != INVALID_IMM) {
        mov_r(dest, tmp);
        return;
    }

    tmp = getOp2(~imm);
    if (tmp != INVALID_IMM) {
        mvn_r(dest, tmp);
        return;
    }

    encodeComplexImm(imm, dest);
}

ARMWord ARMAssembler::encodeComplexImm(ARMWord imm, int dest)
{
#if WTF_ARM_ARCH_VERSION >= 7
    ARMWord tmp = getImm16Op2(imm);
    if (tmp != INVALID_IMM) {
        movw_r(dest, tmp);
        return dest;
    }
    movw_r(dest, getImm16Op2(imm & 0xffff));
    movt_r(dest, getImm16Op2(imm >> 16));
    return dest;
#else
    // Do it by 2 instruction
    if (genInt(dest, imm, true))
        return dest;
    if (genInt(dest, ~imm, false))
        return dest;

    ldr_imm(dest, imm);
    return dest;
#endif
}

// Memory load/store helpers

void ARMAssembler::dataTransfer32(bool isLoad, RegisterID srcDst, RegisterID base, int32_t offset)
{
    if (offset >= 0) {
        if (offset <= 0xfff)
            dtr_u(isLoad, srcDst, base, offset);
        else if (offset <= 0xfffff) {
            add_r(ARMRegisters::S0, base, OP2_IMM | (offset >> 12) | (10 << 8));
            dtr_u(isLoad, srcDst, ARMRegisters::S0, (offset & 0xfff));
        } else {
            moveImm(offset, ARMRegisters::S0);
            dtr_ur(isLoad, srcDst, base, ARMRegisters::S0);
        }
    } else {
        offset = -offset;
        if (offset <= 0xfff)
            dtr_d(isLoad, srcDst, base, offset);
        else if (offset <= 0xfffff) {
            sub_r(ARMRegisters::S0, base, OP2_IMM | (offset >> 12) | (10 << 8));
            dtr_d(isLoad, srcDst, ARMRegisters::S0, (offset & 0xfff));
        } else {
            moveImm(offset, ARMRegisters::S0);
            dtr_dr(isLoad, srcDst, base, ARMRegisters::S0);
        }
    }
}

void ARMAssembler::dataTransfer8(bool isLoad, RegisterID srcDst, RegisterID base, int32_t offset)
{
    if (offset >= 0) {
        if (offset <= 0xfff)
            dtrb_u(isLoad, srcDst, base, offset);
        else if (offset <= 0xfffff) {
            add_r(ARMRegisters::S0, base, OP2_IMM | (offset >> 12) | (10 << 8));
            dtrb_u(isLoad, srcDst, ARMRegisters::S0, (offset & 0xfff));
        } else {
            moveImm(offset, ARMRegisters::S0);
            dtrb_ur(isLoad, srcDst, base, ARMRegisters::S0);
        }
    } else {
        offset = -offset;
        if (offset <= 0xfff)
            dtrb_d(isLoad, srcDst, base, offset);
        else if (offset <= 0xfffff) {
            sub_r(ARMRegisters::S0, base, OP2_IMM | (offset >> 12) | (10 << 8));
            dtrb_d(isLoad, srcDst, ARMRegisters::S0, (offset & 0xfff));
        } else {
            moveImm(offset, ARMRegisters::S0);
            dtrb_dr(isLoad, srcDst, base, ARMRegisters::S0);
        }
    }
}

void ARMAssembler::baseIndexTransfer32(bool isLoad, RegisterID srcDst, RegisterID base, RegisterID index, int scale, int32_t offset)
{
    ARMWord op2;

    ASSERT(scale >= 0 && scale <= 3);
    op2 = lsl(index, scale);

    if (offset >= 0 && offset <= 0xfff) {
        add_r(ARMRegisters::S0, base, op2);
        dtr_u(isLoad, srcDst, ARMRegisters::S0, offset);
        return;
    }
    if (offset <= 0 && offset >= -0xfff) {
        add_r(ARMRegisters::S0, base, op2);
        dtr_d(isLoad, srcDst, ARMRegisters::S0, -offset);
        return;
    }

    ldr_un_imm(ARMRegisters::S0, offset);
    add_r(ARMRegisters::S0, ARMRegisters::S0, op2);
    dtr_ur(isLoad, srcDst, base, ARMRegisters::S0);
}

void ARMAssembler::doubleTransfer(bool isLoad, FPRegisterID srcDst, RegisterID base, int32_t offset)
{
    if (offset & 0x3) {
        if (offset <= 0x3ff && offset >= 0) {
            fdtr_u(isLoad, srcDst, base, offset >> 2);
            return;
        }
        if (offset <= 0x3ffff && offset >= 0) {
            add_r(ARMRegisters::S0, base, OP2_IMM | (offset >> 10) | (11 << 8));
            fdtr_u(isLoad, srcDst, ARMRegisters::S0, (offset >> 2) & 0xff);
            return;
        }
        offset = -offset;

        if (offset <= 0x3ff && offset >= 0) {
            fdtr_d(isLoad, srcDst, base, offset >> 2);
            return;
        }
        if (offset <= 0x3ffff && offset >= 0) {
            sub_r(ARMRegisters::S0, base, OP2_IMM | (offset >> 10) | (11 << 8));
            fdtr_d(isLoad, srcDst, ARMRegisters::S0, (offset >> 2) & 0xff);
            return;
        }
        offset = -offset;
    }

    // TODO: This is broken in the case that offset is unaligned. VFP can never
    // perform unaligned accesses, even from an unaligned register base. (NEON
    // can, but VFP isn't NEON. It is not advisable to interleave a NEON load
    // with VFP code, so the best solution here is probably to perform an
    // unaligned integer load, then move the result into VFP using VMOV.)
    ASSERT((offset & 0x3) == 0);

    ldr_un_imm(ARMRegisters::S0, offset);
    add_r(ARMRegisters::S0, ARMRegisters::S0, base);
    fdtr_u(isLoad, srcDst, ARMRegisters::S0, 0);
}

// Fix up the offsets and literal-pool loads in buffer. The buffer should
// already contain the code from m_buffer.
inline void ARMAssembler::fixUpOffsets(void * buffer)
{
    char * data = reinterpret_cast<char *>(buffer);
    for (Jumps::Iterator iter = m_jumps.begin(); iter != m_jumps.end(); ++iter) {
        // The last bit is set if the constant must be placed on constant pool.
        int pos = (*iter) & (~0x1);
        ARMWord* ldrAddr = reinterpret_cast<ARMWord*>(data + pos);
        ARMWord* addr = getLdrImmAddress(ldrAddr);
        if (*addr != InvalidBranchTarget) {
// The following is disabled for JM because we patch some branches after
// calling fixUpOffset, and the branch patcher doesn't know how to handle 'B'
// instructions.
#if 0
            if (!(*iter & 1)) {
                int diff = reinterpret_cast<ARMWord*>(data + *addr) - (ldrAddr + DefaultPrefetching);

                if ((diff <= BOFFSET_MAX && diff >= BOFFSET_MIN)) {
                    *ldrAddr = B | getConditionalField(*ldrAddr) | (diff & BRANCH_MASK);
                    continue;
                }
            }
#endif
            *addr = reinterpret_cast<ARMWord>(data + *addr);
        }
    }
}

void* ARMAssembler::executableCopy(ExecutablePool* allocator)
{
    // 64-bit alignment is required for next constant pool and JIT code as well
    m_buffer.flushWithoutBarrier(true);
    if (m_buffer.uncheckedSize() & 0x7)
        bkpt(0);

    void * data = m_buffer.executableCopy(allocator);
    if (data)
        fixUpOffsets(data);
    return data;
}

// This just dumps the code into the specified buffer, fixing up absolute
// offsets and literal pool loads as it goes. The buffer is assumed to be large
// enough to hold the code, and any pre-existing literal pool is assumed to
// have been flushed.
void* ARMAssembler::executableCopy(void * buffer)
{
    if (m_buffer.oom())
        return NULL;

    ASSERT(m_buffer.sizeOfConstantPool() == 0);

    memcpy(buffer, m_buffer.data(), m_buffer.size());
    fixUpOffsets(buffer);
    return buffer;
}

} // namespace JSC

#endif // ENABLE(ASSEMBLER) && CPU(ARM_TRADITIONAL)
