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

namespace nanojit
{
    using namespace avmplus;
    #ifdef FEATURE_NANOJIT

    const int8_t operandCount[] = {
#define OPDEF(op, number, operands, repkind) \
        operands,
#define OPDEF64(op, number, operands, repkind) \
        operands,
#include "LIRopcode.tbl"
#undef OPDEF
#undef OPDEF64
        0
    };

    const uint8_t repKinds[] = {
#define OPDEF(op, number, operands, repkind) \
        LRK_##repkind,
#define OPDEF64(op, number, operands, repkind) \
        OPDEF(op, number, operands, repkind)
#include "LIRopcode.tbl"
#undef OPDEF
#undef OPDEF64
        0
    };

    extern const uint8_t insSizes[] = {
#define OPDEF(op, number, operands, repkind) \
            sizeof(LIns##repkind),
#define OPDEF64(op, number, operands, repkind) \
            OPDEF(op, number, operands, repkind)
#include "LIRopcode.tbl"
#undef OPDEF
#undef OPDEF64
            0
    };

    // LIR verbose specific
    #ifdef NJ_VERBOSE

    const char* lirNames[] = {
#define OPDEF(op, number, operands, repkind) \
        #op,
#define OPDEF64(op, number, operands, repkind) \
        #op,
#include "LIRopcode.tbl"
#undef OPDEF
#undef OPDEF64
        NULL
    };

    #endif /* NANOJIT_VEBROSE */

    // implementation

#ifdef NJ_PROFILE
    // @todo fixup move to nanojit.h
    #undef counter_value
    #define counter_value(x)        x
#endif /* NJ_PROFILE */

    // LCompressedBuffer
    LirBuffer::LirBuffer(Allocator& alloc) :
#ifdef NJ_VERBOSE
          names(NULL),
#endif
          abi(ABI_FASTCALL), state(NULL), param1(NULL), sp(NULL), rp(NULL),
          _allocator(alloc)
    {
        clear();
    }

    void LirBuffer::clear()
    {
        // clear the stats, etc
        _unused = 0;
        _limit = 0;
        _bytesAllocated = 0;
        _stats.lir = 0;
        for (int i = 0; i < NumSavedRegs; ++i)
            savedRegs[i] = NULL;
        chunkAlloc();
    }

    void LirBuffer::chunkAlloc()
    {
        _unused = (uintptr_t) _allocator.alloc(CHUNK_SZB);
        NanoAssert(_unused != 0); // Allocator.alloc() never returns null. See Allocator.h
        _limit = _unused + CHUNK_SZB;
    }

    int32_t LirBuffer::insCount()
    {
        // A LIR_skip payload is considered part of the LIR_skip, and LIR_call
        // arg slots are considered part of the LIR_call.
        return _stats.lir;
    }

    size_t LirBuffer::byteCount()
    {
        return _bytesAllocated - (_limit - _unused);
    }

    // Allocate a new page, and write the first instruction to it -- a skip
    // linking to last instruction of the previous page.
    void LirBuffer::moveToNewChunk(uintptr_t addrOfLastLInsOnCurrentChunk)
    {
        chunkAlloc();
        // Link LIR stream back to prior instruction.
        // Unlike all the ins*() functions, we don't call makeRoom() here
        // because we know we have enough space, having just started a new
        // page.
        LInsSk* insSk = (LInsSk*)_unused;
        LIns*   ins   = insSk->getLIns();
        ins->initLInsSk((LInsp)addrOfLastLInsOnCurrentChunk);
        _unused += sizeof(LInsSk);
        verbose_only(_stats.lir++);
    }

    // Make room for a single instruction.
    uintptr_t LirBuffer::makeRoom(size_t szB)
    {
        // Make sure the size is ok
        NanoAssert(0 == szB % sizeof(void*));
        NanoAssert(sizeof(LIns) <= szB && szB <= MAX_LINS_SZB);
        NanoAssert(_unused < _limit);

        // If the instruction won't fit on the current chunk, get a new chunk
        if (_unused + szB > _limit) {
            uintptr_t addrOfLastLInsOnChunk = _unused - sizeof(LIns);
            moveToNewChunk(addrOfLastLInsOnChunk);
        }

        // We now know that we are on a chunk that has the requested amount of
        // room: record the starting address of the requested space and bump
        // the pointer.
        uintptr_t startOfRoom = _unused;
        _unused += szB;
        verbose_only(_stats.lir++);             // count the instruction

        // If there's no more space on this chunk, move to a new one.
        // (This will only occur if the asked-for size filled up exactly to
        // the end of the chunk.)  This ensures that next time we enter this
        // function, _unused won't be pointing one byte past the end of
        // the chunk, which would break everything.
        if (_unused >= _limit) {
            // Check we used exactly the remaining space
            NanoAssert(_unused == _limit);
            uintptr_t addrOfLastLInsOnChunk = _unused - sizeof(LIns);
            moveToNewChunk(addrOfLastLInsOnChunk);
        }

        // Make sure it's word-aligned.
        NanoAssert(0 == startOfRoom % sizeof(void*));
        return startOfRoom;
    }

    LInsp LirBufWriter::insStorei(LInsp val, LInsp base, int32_t d)
    {
        LOpcode op = val->isQuad() ? LIR_stqi : LIR_sti;
        LInsSti* insSti = (LInsSti*)_buf->makeRoom(sizeof(LInsSti));
        LIns*    ins    = insSti->getLIns();
        ins->initLInsSti(op, val, base, d);
        return ins;
    }

    LInsp LirBufWriter::ins0(LOpcode op)
    {
        LInsOp0* insOp0 = (LInsOp0*)_buf->makeRoom(sizeof(LInsOp0));
        LIns*    ins    = insOp0->getLIns();
        ins->initLInsOp0(op);
        return ins;
    }

    LInsp LirBufWriter::ins1(LOpcode op, LInsp o1)
    {
        LInsOp1* insOp1 = (LInsOp1*)_buf->makeRoom(sizeof(LInsOp1));
        LIns*    ins    = insOp1->getLIns();
        ins->initLInsOp1(op, o1);
        return ins;
    }

    LInsp LirBufWriter::ins2(LOpcode op, LInsp o1, LInsp o2)
    {
        LInsOp2* insOp2 = (LInsOp2*)_buf->makeRoom(sizeof(LInsOp2));
        LIns*    ins    = insOp2->getLIns();
        ins->initLInsOp2(op, o1, o2);
        return ins;
    }

    LInsp LirBufWriter::ins3(LOpcode op, LInsp o1, LInsp o2, LInsp o3)
    {
        LInsOp3* insOp3 = (LInsOp3*)_buf->makeRoom(sizeof(LInsOp3));
        LIns*    ins    = insOp3->getLIns();
        ins->initLInsOp3(op, o1, o2, o3);
        return ins;
    }

    LInsp LirBufWriter::insLoad(LOpcode op, LInsp base, int32_t d)
    {
        LInsLd* insLd = (LInsLd*)_buf->makeRoom(sizeof(LInsLd));
        LIns*   ins   = insLd->getLIns();
        ins->initLInsLd(op, base, d);
        return ins;
    }

    LInsp LirBufWriter::insGuard(LOpcode op, LInsp c, GuardRecord *gr)
    {
        debug_only( if (LIR_x == op || LIR_xbarrier == op) NanoAssert(!c); )
        return ins2(op, c, (LIns*)gr);
    }

    LInsp LirBufWriter::insBranch(LOpcode op, LInsp condition, LInsp toLabel)
    {
        NanoAssert((op == LIR_j && !condition) ||
                   ((op == LIR_jf || op == LIR_jt) && condition));
        return ins2(op, condition, toLabel);
    }

    LInsp LirBufWriter::insAlloc(int32_t size)
    {
        size = (size+3)>>2; // # of required 32bit words
        LInsI* insI = (LInsI*)_buf->makeRoom(sizeof(LInsI));
        LIns*  ins  = insI->getLIns();
        ins->initLInsI(LIR_alloc, size);
        return ins;
    }

    LInsp LirBufWriter::insParam(int32_t arg, int32_t kind)
    {
        LInsP* insP = (LInsP*)_buf->makeRoom(sizeof(LInsP));
        LIns*  ins  = insP->getLIns();
        ins->initLInsP(arg, kind);
        if (kind) {
            NanoAssert(arg < NumSavedRegs);
            _buf->savedRegs[arg] = ins;
        }
        return ins;
    }

    LInsp LirBufWriter::insImm(int32_t imm)
    {
        LInsI* insI = (LInsI*)_buf->makeRoom(sizeof(LInsI));
        LIns*  ins  = insI->getLIns();
        ins->initLInsI(LIR_int, imm);
        return ins;
    }

    LInsp LirBufWriter::insImmq(uint64_t imm)
    {
        LInsI64* insI64 = (LInsI64*)_buf->makeRoom(sizeof(LInsI64));
        LIns*    ins    = insI64->getLIns();
        ins->initLInsI64(LIR_quad, imm);
        return ins;
    }

    LInsp LirBufWriter::insImmf(double d)
    {
        LInsI64* insI64 = (LInsI64*)_buf->makeRoom(sizeof(LInsI64));
        LIns*    ins    = insI64->getLIns();
        union {
            double d;
            uint64_t q;
        } u;
        u.d = d;
        ins->initLInsI64(LIR_float, u.q);
        return ins;
    }

    LInsp LirBufWriter::insSkip(size_t payload_szB)
    {
        // First, round up payload_szB to a multiple of the word size.  To
        // ensure that the rounding up won't cause it to exceed
        // NJ_MAX_SKIP_PAYLOAD_SZB, NJ_MAX_SKIP_PAYLOAD_SZB must also be a
        // multiple of the word size, which we check.
        payload_szB = alignUp(payload_szB, sizeof(void*));
        NanoAssert(0 == LirBuffer::MAX_SKIP_PAYLOAD_SZB % sizeof(void*));
        NanoAssert(sizeof(void*) <= payload_szB && payload_szB <= LirBuffer::MAX_SKIP_PAYLOAD_SZB);

        uintptr_t payload = _buf->makeRoom(payload_szB + sizeof(LInsSk));
        uintptr_t prevLInsAddr = payload - sizeof(LIns);
        LInsSk* insSk = (LInsSk*)(payload + payload_szB);
        LIns*   ins   = insSk->getLIns();
        // not sure what we want to assert here since chunks aren't pages anymore
        //NanoAssert(prevLInsAddr >= pageDataStart(prevLInsAddr));
        //NanoAssert(samepage(prevLInsAddr, insSk));
        ins->initLInsSk((LInsp)prevLInsAddr);
        return ins;
    }

    // Reads the next non-skip instruction.
    LInsp LirReader::read()
    {
        NanoAssert(_i);
        LInsp cur = _i;
        uintptr_t i = uintptr_t(cur);
        LOpcode iop = ((LInsp)i)->opcode();

        // We pass over skip instructions below.  Also, the last instruction
        // for a fragment shouldn't be a skip(*).  Therefore we shouldn't see
        // a skip here.
        //
        // (*) Actually, if the last *inserted* instruction exactly fills up a
        // page, a new page will be created, and thus the last *written*
        // instruction will be a skip -- the one needed for the cross-page
        // link.  But the last *inserted* instruction is what is recorded and
        // used to initialise each LirReader, and that is what is seen here,
        // and therefore this assertion holds.
        NanoAssert(iop != LIR_skip);

        do
        {
            // Nb: this switch is table-driven (because sizeof_LInsXYZ() is
            // table-driven) in most cases to avoid branch mispredictions --
            // if we do a vanilla switch on the iop or LInsRepKind the extra
            // branch mispredictions cause a small but noticeable slowdown.
            switch (iop)
            {
                default:
                    i -= insSizes[((LInsp)i)->opcode()];
                    break;

                case LIR_icall:
                case LIR_fcall:
                case LIR_qcall: {
                    int argc = ((LInsp)i)->argc();
                    i -= sizeof(LInsC);         // step over the instruction
                    i -= argc*sizeof(LInsp);    // step over the arguments
                    break;
                }

                case LIR_skip:
                    // Ignore the skip, move onto its predecessor.
                    NanoAssert(((LInsp)i)->prevLIns() != (LInsp)i);
                    i = uintptr_t(((LInsp)i)->prevLIns());
                    break;

                case LIR_start:
                    // Once we hit here, this method shouldn't be called again.
                    // The assertion at the top of this method checks this.
                    _i = 0;
                    return cur;
            }
            iop = ((LInsp)i)->opcode();
        }
        while (LIR_skip == iop);
        _i = (LInsp)i;
        return cur;
    }

    // This is never called, but that's ok because it contains only static
    // assertions.
    void LIns::staticSanityCheck()
    {
        // LIns must be word-sized.
        NanoStaticAssert(sizeof(LIns) == 1*sizeof(void*));

        // LInsXYZ have expected sizes too.
        NanoStaticAssert(sizeof(LInsOp0) == 1*sizeof(void*));
        NanoStaticAssert(sizeof(LInsOp1) == 2*sizeof(void*));
        NanoStaticAssert(sizeof(LInsOp2) == 3*sizeof(void*));
        NanoStaticAssert(sizeof(LInsOp3) == 4*sizeof(void*));
        NanoStaticAssert(sizeof(LInsLd)  == 3*sizeof(void*));
        NanoStaticAssert(sizeof(LInsSti) == 4*sizeof(void*));
        NanoStaticAssert(sizeof(LInsSk)  == 2*sizeof(void*));
        NanoStaticAssert(sizeof(LInsC)   == 3*sizeof(void*));
        NanoStaticAssert(sizeof(LInsP)   == 2*sizeof(void*));
        NanoStaticAssert(sizeof(LInsI)   == 2*sizeof(void*));
    #if defined NANOJIT_64BIT
        NanoStaticAssert(sizeof(LInsI64) == 2*sizeof(void*));
    #else
        NanoStaticAssert(sizeof(LInsI64) == 3*sizeof(void*));
    #endif

        // oprnd_1 must be in the same position in LIns{Op1,Op2,Op3,Ld,Sti}
        // because oprnd1() is used for all of them.
        NanoStaticAssert( (offsetof(LInsOp1, ins) - offsetof(LInsOp1, oprnd_1)) ==
                          (offsetof(LInsOp2, ins) - offsetof(LInsOp2, oprnd_1)) );
        NanoStaticAssert( (offsetof(LInsOp2, ins) - offsetof(LInsOp2, oprnd_1)) ==
                          (offsetof(LInsOp3, ins) - offsetof(LInsOp3, oprnd_1)) );
        NanoStaticAssert( (offsetof(LInsOp3, ins) - offsetof(LInsOp3, oprnd_1)) ==
                          (offsetof(LInsLd,  ins) - offsetof(LInsLd,  oprnd_1)) );
        NanoStaticAssert( (offsetof(LInsLd,  ins) - offsetof(LInsLd,  oprnd_1)) ==
                          (offsetof(LInsSti, ins) - offsetof(LInsSti, oprnd_1)) );

        // oprnd_2 must be in the same position in LIns{Op2,Op3,Sti}
        // because oprnd2() is used for both of them.
        NanoStaticAssert( (offsetof(LInsOp2, ins) - offsetof(LInsOp2, oprnd_2)) ==
                          (offsetof(LInsOp3, ins) - offsetof(LInsOp3, oprnd_2)) );
        NanoStaticAssert( (offsetof(LInsOp3, ins) - offsetof(LInsOp3, oprnd_2)) ==
                          (offsetof(LInsSti, ins) - offsetof(LInsSti, oprnd_2)) );
    }

    bool LIns::isFloat() const {
        switch (opcode()) {
            default:
                return false;
            case LIR_fadd:
            case LIR_fsub:
            case LIR_fmul:
            case LIR_fdiv:
            case LIR_fneg:
            case LIR_fcall:
            case LIR_i2f:
            case LIR_u2f:
                return true;
        }
    }

    LIns* LirWriter::ins2i(LOpcode v, LIns* oprnd1, int32_t imm)
    {
        return ins2(v, oprnd1, insImm(imm));
    }

    bool insIsS16(LInsp i)
    {
        if (i->isconst()) {
            int c = i->imm32();
            return isS16(c);
        }
        if (i->isop(LIR_cmov) || i->isop(LIR_qcmov)) {
            return insIsS16(i->oprnd2()) && insIsS16(i->oprnd3());
        }
        if (i->isCmp())
            return true;
        // many other possibilities too.
        return false;
    }

    LIns* ExprFilter::ins1(LOpcode v, LIns* i)
    {
        switch (v) {
        case LIR_qlo:
            if (i->isconstq())
                return insImm(i->imm64_0());
            if (i->isop(LIR_qjoin))
                return i->oprnd1();
            break;
        case LIR_qhi:
            if (i->isconstq())
                return insImm(i->imm64_1());
            if (i->isop(LIR_qjoin))
                return i->oprnd2();
            break;
        case LIR_not:
            if (i->isconst())
                return insImm(~i->imm32());
        involution:
            if (v == i->opcode())
                return i->oprnd1();
            break;
        case LIR_neg:
            if (i->isconst())
                return insImm(-i->imm32());
            if (i->isop(LIR_sub)) // -(a-b) = b-a
                return out->ins2(LIR_sub, i->oprnd2(), i->oprnd1());
            goto involution;
        case LIR_fneg:
            if (i->isconstq())
                return insImmf(-i->imm64f());
            if (i->isop(LIR_fsub))
                return out->ins2(LIR_fsub, i->oprnd2(), i->oprnd1());
            goto involution;
        case LIR_i2f:
            if (i->isconst())
                return insImmf(i->imm32());
            break;
        case LIR_u2f:
            if (i->isconst())
                return insImmf(uint32_t(i->imm32()));
            break;
        default:
            ;
        }

        return out->ins1(v, i);
    }

    LIns* ExprFilter::ins2(LOpcode v, LIns* oprnd1, LIns* oprnd2)
    {
        NanoAssert(oprnd1 && oprnd2);
        if (oprnd1 == oprnd2)
        {
            switch (v) {
            case LIR_xor:
            case LIR_sub:
            case LIR_ult:
            case LIR_ugt:
            case LIR_gt:
            case LIR_lt:
                return insImm(0);
            case LIR_or:
            case LIR_and:
                return oprnd1;
            case LIR_le:
            case LIR_ule:
            case LIR_ge:
            case LIR_uge:
                // x <= x == 1; x >= x == 1
                return insImm(1);
            default:
                ;
            }
        }
        if (oprnd1->isconst() && oprnd2->isconst())
        {
            int32_t c1 = oprnd1->imm32();
            int32_t c2 = oprnd2->imm32();
            double d;
            int32_t r;
            uint64_t q;

            switch (v) {
            case LIR_qjoin:
                q = c1 | uint64_t(c2)<<32;
                return insImmq(q);
            case LIR_eq:
                return insImm(c1 == c2);
            case LIR_ov:
                return insImm((c2 != 0) && ((c1 + c2) <= c1));
            case LIR_lt:
                return insImm(c1 < c2);
            case LIR_gt:
                return insImm(c1 > c2);
            case LIR_le:
                return insImm(c1 <= c2);
            case LIR_ge:
                return insImm(c1 >= c2);
            case LIR_ult:
                return insImm(uint32_t(c1) < uint32_t(c2));
            case LIR_ugt:
                return insImm(uint32_t(c1) > uint32_t(c2));
            case LIR_ule:
                return insImm(uint32_t(c1) <= uint32_t(c2));
            case LIR_uge:
                return insImm(uint32_t(c1) >= uint32_t(c2));
            case LIR_rsh:
                return insImm(int32_t(c1) >> int32_t(c2));
            case LIR_lsh:
                return insImm(int32_t(c1) << int32_t(c2));
            case LIR_ush:
                return insImm(uint32_t(c1) >> int32_t(c2));
            case LIR_or:
                return insImm(uint32_t(c1) | int32_t(c2));
            case LIR_and:
                return insImm(uint32_t(c1) & int32_t(c2));
            case LIR_xor:
                return insImm(uint32_t(c1) ^ int32_t(c2));
            case LIR_add:
                d = double(c1) + double(c2);
            fold:
                r = int32_t(d);
                if (r == d)
                    return insImm(r);
                break;
            case LIR_sub:
                d = double(c1) - double(c2);
                goto fold;
            case LIR_mul:
                d = double(c1) * double(c2);
                goto fold;
            case LIR_div:
            case LIR_mod:
                // We can't easily fold div and mod, since folding div makes it
                // impossible to calculate the mod that refers to it. The
                // frontend shouldn't emit div and mod with constant operands.
                NanoAssert(0);
            default:
                ;
            }
        }
        else if (oprnd1->isconstq() && oprnd2->isconstq())
        {
            double c1 = oprnd1->imm64f();
            double c2 = oprnd2->imm64f();
            switch (v) {
            case LIR_feq:
                return insImm(c1 == c2);
            case LIR_flt:
                return insImm(c1 < c2);
            case LIR_fgt:
                return insImm(c1 > c2);
            case LIR_fle:
                return insImm(c1 <= c2);
            case LIR_fge:
                return insImm(c1 >= c2);
            case LIR_fadd:
                return insImmf(c1 + c2);
            case LIR_fsub:
                return insImmf(c1 - c2);
            case LIR_fmul:
                return insImmf(c1 * c2);
            case LIR_fdiv:
                return insImmf(c1 / c2);
            default:
                ;
            }
        }
        else if (oprnd1->isconst() && !oprnd2->isconst())
        {
            LIns* t;
            switch (v) {
            case LIR_add:
            case LIR_iaddp:
            case LIR_qaddp:
            case LIR_mul:
            case LIR_fadd:
            case LIR_fmul:
            case LIR_xor:
            case LIR_or:
            case LIR_and:
            case LIR_eq:
                // move const to rhs
                t = oprnd2;
                oprnd2 = oprnd1;
                oprnd1 = t;
                break;
            default:
                if (v >= LIR_lt && v <= LIR_uge) {
                    NanoStaticAssert((LIR_lt ^ 1) == LIR_gt);
                    NanoStaticAssert((LIR_le ^ 1) == LIR_ge);
                    NanoStaticAssert((LIR_ult ^ 1) == LIR_ugt);
                    NanoStaticAssert((LIR_ule ^ 1) == LIR_uge);

                    // move const to rhs, swap the operator
                    LIns *t = oprnd2;
                    oprnd2 = oprnd1;
                    oprnd1 = t;
                    v = LOpcode(v^1);
                }
                break;
            }
        }

        if (oprnd2->isconst())
        {
            int c = oprnd2->imm32();
            switch (v) {
            case LIR_add:
                if (oprnd1->isop(LIR_add) && oprnd1->oprnd2()->isconst()) {
                    // add(add(x,c1),c2) => add(x,c1+c2)
                    c += oprnd1->oprnd2()->imm32();
                    oprnd2 = insImm(c);
                    oprnd1 = oprnd1->oprnd1();
                }
                break;
            case LIR_sub:
                if (oprnd1->isop(LIR_add) && oprnd1->oprnd2()->isconst()) {
                    // sub(add(x,c1),c2) => add(x,c1-c2)
                    c = oprnd1->oprnd2()->imm32() - c;
                    oprnd2 = insImm(c);
                    oprnd1 = oprnd1->oprnd1();
                    v = LIR_add;
                }
                break;
            case LIR_rsh:
                if (c == 16 && oprnd1->isop(LIR_lsh) &&
                    oprnd1->oprnd2()->isconstval(16) &&
                    insIsS16(oprnd1->oprnd1())) {
                    // rsh(lhs(x,16),16) == x, if x is S16
                    return oprnd1->oprnd1();
                }
                break;
            default:
                ;
            }

            if (c == 0) {
                switch (v) {
                case LIR_add:
                case LIR_iaddp:
                case LIR_or:
                case LIR_xor:
                case LIR_sub:
                case LIR_lsh:
                case LIR_rsh:
                case LIR_ush:
                    return oprnd1;
                case LIR_and:
                case LIR_mul:
                    return oprnd2;
                case LIR_eq:
                    if (oprnd1->isop(LIR_or) &&
                        oprnd1->oprnd2()->isconst() &&
                        oprnd1->oprnd2()->imm32() != 0) {
                        // (x or c) != 0 if c != 0
                        return insImm(0);
                    }
                default:
                    ;
                }
            } else if (c == -1 || (c == 1 && oprnd1->isCmp())) {
                switch (v) {
                case LIR_or:
                    // x | -1 = -1, cmp | 1 = 1
                    return oprnd2;
                case LIR_and:
                    // x & -1 = x, cmp & 1 = cmp
                    return oprnd1;
                default:
                    ;
                }
            }
        }

        LInsp i;
        if (v == LIR_qjoin && oprnd1->isop(LIR_qlo) && oprnd2->isop(LIR_qhi) &&
            (i = oprnd1->oprnd1()) == oprnd2->oprnd1()) {
            // qjoin(qlo(x),qhi(x)) == x
            return i;
        }

        return out->ins2(v, oprnd1, oprnd2);
    }

    LIns* ExprFilter::ins3(LOpcode v, LIns* oprnd1, LIns* oprnd2, LIns* oprnd3)
    {
        NanoAssert(oprnd1 && oprnd2 && oprnd3);
        NanoAssert(v == LIR_cmov || v == LIR_qcmov);
        if (oprnd2 == oprnd3) {
            // c ? a : a => a
            return oprnd2;
        }
        if (oprnd1->isconst()) {
            // const ? x : y => return x or y depending on const
            return oprnd1->imm32() ? oprnd2 : oprnd3;
        }
        if (oprnd1->isop(LIR_eq) &&
            ((oprnd1->oprnd2() == oprnd2 && oprnd1->oprnd1() == oprnd3) ||
             (oprnd1->oprnd1() == oprnd2 && oprnd1->oprnd2() == oprnd3))) {
            // (y == x) ? x : y  =>  y
            // (x == y) ? x : y  =>  y
            return oprnd3;
        }

        return out->ins3(v, oprnd1, oprnd2, oprnd3);
    }

    LIns* ExprFilter::insGuard(LOpcode v, LInsp c, GuardRecord *gr)
    {
        if (v == LIR_xt || v == LIR_xf) {
            if (c->isconst()) {
                if ((v == LIR_xt && !c->imm32()) || (v == LIR_xf && c->imm32())) {
                    return 0; // no guard needed
                }
                else {
#ifdef JS_TRACER
                    // We're emitting a guard that will always fail. Any code
                    // emitted after this guard is dead code. We could
                    // silently optimize out the rest of the emitted code, but
                    // this could indicate a performance problem or other bug,
                    // so assert in debug builds.
                    NanoAssertMsg(0, "Constantly false guard detected");
#endif
                    return out->insGuard(LIR_x, NULL, gr);
                }
            }
            else {
                NanoStaticAssert((LIR_xt ^ 1) == LIR_xf);
                while (c->isop(LIR_eq) && c->oprnd1()->isCmp() &&
                    c->oprnd2()->isconstval(0)) {
                    // xt(eq(cmp,0)) => xf(cmp)   or   xf(eq(cmp,0)) => xt(cmp)
                    v = LOpcode(v^1);
                    c = c->oprnd1();
                }
            }
        }
        return out->insGuard(v, c, gr);
    }

    LIns* ExprFilter::insBranch(LOpcode v, LIns *c, LIns *t)
    {
        switch (v) {
        case LIR_jt:
        case LIR_jf:
            while (c->isop(LIR_eq) && c->oprnd1()->isCmp() && c->oprnd2()->isconstval(0)) {
                // jt(eq(cmp,0)) => jf(cmp)   or   jf(eq(cmp,0)) => jt(cmp)
                v = LOpcode(v ^ 1);
                c = c->oprnd1();
            }
            break;
        default:
            ;
        }
        return out->insBranch(v, c, t);
    }

    LIns* ExprFilter::insLoad(LOpcode op, LIns* base, int32_t off) {
        if (base->isconstp() && !isS8(off)) {
            // if the effective address is constant, then transform:
            // ld const[bigconst] => ld (const+bigconst)[0]
            // note: we don't do this optimization for <8bit field offsets,
            // under the assumption that we're more likely to CSE-match the
            // constant base address if we dont const-fold small offsets.
            uintptr_t p = (uintptr_t)base->constvalp() + off;
            return out->insLoad(op, insImmPtr((void*)p), 0);
        }
        return out->insLoad(op, base, off);
    }

    LIns* LirWriter::ins_eq0(LIns* oprnd1)
    {
        return ins2i(LIR_eq, oprnd1, 0);
    }

    LIns* LirWriter::ins_peq0(LIns* oprnd1)
    {
        return ins2(LIR_peq, oprnd1, insImmWord(0));
    }

    LIns* LirWriter::ins_i2p(LIns* intIns)
    {
#ifdef NANOJIT_64BIT
        return ins1(LIR_i2q, intIns);
#else
        return intIns;
#endif
    }

    LIns* LirWriter::ins_u2p(LIns* uintIns)
    {
#ifdef NANOJIT_64BIT
        return ins1(LIR_u2q, uintIns);
#else
        return uintIns;
#endif
    }

    LIns* LirWriter::qjoin(LInsp lo, LInsp hi)
    {
        return ins2(LIR_qjoin, lo, hi);
    }

    LIns* LirWriter::insImmWord(intptr_t value)
    {
#ifdef NANOJIT_64BIT
        return insImmq(value);
#else
        return insImm(value);
#endif
    }

    LIns* LirWriter::insImmPtr(const void *ptr)
    {
#ifdef NANOJIT_64BIT
        return insImmq((uint64_t)ptr);
#else
        return insImm((int32_t)ptr);
#endif
    }

    LIns* LirWriter::ins_choose(LIns* cond, LIns* iftrue, LIns* iffalse)
    {
        // if not a conditional, make it implicitly an ==0 test (then flop results)
        if (!cond->isCmp())
        {
            cond = ins_eq0(cond);
            LInsp tmp = iftrue;
            iftrue = iffalse;
            iffalse = tmp;
        }

        if (avmplus::AvmCore::use_cmov())
            return ins3((iftrue->isQuad() || iffalse->isQuad()) ? LIR_qcmov : LIR_cmov, cond, iftrue, iffalse);

        LInsp ncond = ins1(LIR_neg, cond); // cond ? -1 : 0
        return ins2(LIR_or,
                    ins2(LIR_and, iftrue, ncond),
                    ins2(LIR_and, iffalse, ins1(LIR_not, ncond)));
    }

    LIns* LirBufWriter::insCall(const CallInfo *ci, LInsp args[])
    {
        static const LOpcode k_callmap[] = {
        //  ARGSIZE_NONE  ARGSIZE_F  ARGSIZE_LO  ARGSIZE_Q  (4)        (5)        ARGSIZE_U  (7)
            LIR_pcall,    LIR_fcall, LIR_icall,  LIR_qcall, LIR_pcall, LIR_pcall, LIR_icall, LIR_pcall
        };

        uint32_t argt = ci->_argtypes;
        LOpcode op = k_callmap[argt & ARGSIZE_MASK_ANY];
        NanoAssert(op != LIR_skip); // LIR_skip here is just an error condition

        ArgSize sizes[MAXARGS];
        int32_t argc = ci->get_sizes(sizes);

        if (AvmCore::config.soft_float) {
            if (op == LIR_fcall)
                op = LIR_callh;
        }

        NanoAssert(argc <= (int)MAXARGS);

        // Lay the call parameters out (in reverse order).
        // Nb: this must be kept in sync with arg().
        LInsp* newargs = (LInsp*)_buf->makeRoom(argc*sizeof(LInsp) + sizeof(LInsC)); // args + call
        for (int32_t i = 0; i < argc; i++)
            newargs[argc - i - 1] = args[i];

        // Write the call instruction itself.
        LInsC* insC = (LInsC*)(uintptr_t(newargs) + argc*sizeof(LInsp));
        LIns*  ins  = insC->getLIns();
#ifndef NANOJIT_64BIT
        ins->initLInsC(op==LIR_callh ? LIR_icall : op, argc, ci);
#else
        ins->initLInsC(op, argc, ci);
#endif
        return ins;
    }

    using namespace avmplus;

    StackFilter::StackFilter(LirFilter *in, Allocator& alloc, LirBuffer *lirbuf, LInsp sp, LInsp rp)
        : LirFilter(in), lirbuf(lirbuf), sp(sp), rp(rp), spStk(alloc), rpStk(alloc),
          spTop(0), rpTop(0)
    {}

    bool StackFilter::ignoreStore(LInsp ins, int top, BitSet* stk)
    {
        bool ignore = false;
        int d = ins->disp() >> 2;
        if (d >= top) {
            ignore = true;
        } else {
            d = top - d;
            if (ins->oprnd1()->isQuad()) {
                // storing 8 bytes
                if (stk->get(d) && stk->get(d-1)) {
                    ignore = true;
                } else {
                    stk->set(d);
                    stk->set(d-1);
                }
            }
            else {
                // storing 4 bytes
                if (stk->get(d)) {
                    ignore = true;
                } else {
                    stk->set(d);
                }
            }
        }
        return ignore;
    }

    LInsp StackFilter::read()
    {
        for (;;)
        {
            LInsp i = in->read();
            if (i->isStore())
            {
                LInsp base = i->oprnd2();

                if (base == sp) {
                    if (ignoreStore(i, spTop, &spStk))
                        continue;

                } else if (base == rp) {
                    if (ignoreStore(i, rpTop, &rpStk))
                        continue;
                }
            }
            /*
             * NB: If there is a backward branch other than the loop-restart branch, this is
             * going to be wrong. Unfortunately there doesn't seem to be an easy way to detect
             * such branches. Just do not create any.
             */
            else if (i->isGuard())
            {
                spStk.reset();
                rpStk.reset();
                getTops(i, spTop, rpTop);
                spTop >>= 2;
                rpTop >>= 2;
            }
            
            return i;
        }
    }

    //
    // inlined/separated version of SuperFastHash
    // This content is copyrighted by Paul Hsieh, For reference see : http://www.azillionmonkeys.com/qed/hash.html
    //
    inline uint32_t _hash8(uint32_t hash, const uint8_t data)
    {
        hash += data;
        hash ^= hash << 10;
        hash += hash >> 1;
        return hash;
    }

    inline uint32_t _hash32(uint32_t hash, const uint32_t data)
    {
        const uint32_t dlo = data & 0xffff;
        const uint32_t dhi = data >> 16;
        hash += dlo;
        const uint32_t tmp = (dhi << 11) ^ hash;
        hash = (hash << 16) ^ tmp;
        hash += hash >> 11;
        return hash;
    }

    inline uint32_t _hashptr(uint32_t hash, const void* data)
    {
#ifdef NANOJIT_64BIT
        hash = _hash32(hash, uint32_t(uintptr_t(data) >> 32));
        hash = _hash32(hash, uint32_t(uintptr_t(data)));
        return hash;
#else
        return _hash32(hash, uint32_t(data));
#endif
    }

    inline uint32_t _hashfinish(uint32_t hash)
    {
        /* Force "avalanching" of final 127 bits */
        hash ^= hash << 3;
        hash += hash >> 5;
        hash ^= hash << 4;
        hash += hash >> 17;
        hash ^= hash << 25;
        hash += hash >> 6;
        return hash;
    }

    LInsHashSet::LInsHashSet(Allocator& alloc) :
            m_cap(kInitialCap), alloc(alloc)
    {
        m_list = new (alloc) LInsp[m_cap];
        clear();
    }

    void LInsHashSet::clear() {
        VMPI_memset(m_list, 0, sizeof(LInsp)*m_cap);
        m_used = 0;
    }


    inline uint32_t LInsHashSet::hashimm(int32_t a) {
        return _hashfinish(_hash32(0,a));
    }

    inline uint32_t LInsHashSet::hashimmq(uint64_t a) {
        uint32_t hash = _hash32(0, uint32_t(a >> 32));
        return _hashfinish(_hash32(hash, uint32_t(a)));
    }

    inline uint32_t LInsHashSet::hash1(LOpcode op, LInsp a) {
        uint32_t hash = _hash8(0,uint8_t(op));
        return _hashfinish(_hashptr(hash, a));
    }

    inline uint32_t LInsHashSet::hash2(LOpcode op, LInsp a, LInsp b) {
        uint32_t hash = _hash8(0,uint8_t(op));
        hash = _hashptr(hash, a);
        return _hashfinish(_hashptr(hash, b));
    }

    inline uint32_t LInsHashSet::hash3(LOpcode op, LInsp a, LInsp b, LInsp c) {
        uint32_t hash = _hash8(0,uint8_t(op));
        hash = _hashptr(hash, a);
        hash = _hashptr(hash, b);
        return _hashfinish(_hashptr(hash, c));
    }

    inline uint32_t LInsHashSet::hashLoad(LOpcode op, LInsp a, int32_t d) {
        uint32_t hash = _hash8(0,uint8_t(op));
        hash = _hashptr(hash, a);
        return _hashfinish(_hash32(hash, d));
    }

    inline uint32_t LInsHashSet::hashcall(const CallInfo *ci, uint32_t argc, LInsp args[]) {
        uint32_t hash = _hashptr(0, ci);
        for (int32_t j=argc-1; j >= 0; j--)
            hash = _hashptr(hash,args[j]);
        return _hashfinish(hash);
    }

    uint32_t LInsHashSet::hashcode(LInsp i)
    {
        const LOpcode op = i->opcode();
        uint32_t operands = operandCount[i->opcode()];

        switch (operands) {
        case 0:
            switch (op) {
            case LIR_int:
                return hashimm(i->imm32());
            case LIR_float:
            case LIR_quad:
                return hashimmq(i->imm64());
            default:
                NanoAssert(i->isCall());
                LInsp args[MAXARGS];
                uint32_t argc = i->argc();
                NanoAssert(argc < MAXARGS);
                for (uint32_t j=0; j < argc; j++)
                    args[j] = i->arg(j);
                return hashcall(i->callInfo(), argc, args);
            }
        case 1:
            return (repKinds[op] == LRK_Ld)
                ? hashLoad(op, i->oprnd1(), i->disp())
                : hash1(op, i->oprnd1());
        case 2:
            return hash2(op, i->oprnd1(), i->oprnd2());
        default:
            NanoAssert(i->isLInsOp3());
            return hash3(op, i->oprnd1(), i->oprnd2(), i->oprnd3());
        }
    }

    inline bool LInsHashSet::equals(LInsp a, LInsp b)
    {
        if (a == b)
            return true;

        const LOpcode op = a->opcode();
        if (op != b->opcode())
            return false;

        const uint32_t operands = operandCount[op];
        switch (operands) {
        case 0:
            switch (op) {
            case LIR_int:
                return a->imm32() == b->imm32();
            case LIR_float:
            case LIR_quad:
                return a->imm64() == b->imm64();
            default:
                NanoAssert(a->isCall());
                if (a->callInfo() != b->callInfo())
                    return false;
                uint32_t argc = a->argc();
                NanoAssert(argc == b->argc());
                for (uint32_t i = 0; i < argc; i++)
                    if (a->arg(i) != b->arg(i))
                        return false;
                return true;
            }
        case 1:
            if (repKinds[op] == LRK_Ld)
                return (a->oprnd1() == b->oprnd1() && a->disp() == b->disp());
            return a->oprnd1() == b->oprnd1();
        case 2:
            return a->oprnd1() == b->oprnd1() && a->oprnd2() == b->oprnd2();
        default:
            NanoAssert(a->isLInsOp3());
            return a->oprnd1() == b->oprnd1() && a->oprnd2() == b->oprnd2() && a->oprnd3() == b->oprnd3();
        }
    }

    void LInsHashSet::grow()
    {
        const uint32_t newcap = m_cap << 1;
        LInsp *newlist = new (alloc) LInsp[newcap];
        VMPI_memset(newlist, 0, newcap * sizeof(LInsp));
        LInsp *list = m_list;
        for (uint32_t i=0, n=m_cap; i < n; i++) {
            LInsp name = list[i];
            if (!name) continue;
            uint32_t j = find(name, hashcode(name), newlist, newcap);
            newlist[j] = name;
        }
        m_cap = newcap;
        m_list = newlist;
    }

    uint32_t LInsHashSet::find(LInsp name, uint32_t hash, const LInsp *list, uint32_t cap)
    {
        const uint32_t bitmask = (cap - 1) & ~0x1;

        uint32_t n = 7 << 1;
        hash &= bitmask;
        LInsp k;
        while ((k = list[hash]) != NULL && !equals(k, name))
        {
            hash = (hash + (n += 2)) & bitmask;        // quadratic probe
        }
        return hash;
    }

    LInsp LInsHashSet::add(LInsp name, uint32_t k)
    {
        // this is relatively short-lived so let's try a more aggressive load factor
        // in the interest of improving performance
        if (((m_used+1)<<1) >= m_cap) // 0.50
        {
            grow();
            k = find(name, hashcode(name), m_list, m_cap);
        }
        NanoAssert(!m_list[k]);
        m_used++;
        return m_list[k] = name;
    }

    LInsp LInsHashSet::find32(int32_t a, uint32_t &i)
    {
        uint32_t cap = m_cap;
        const LInsp *list = m_list;
        const uint32_t bitmask = (cap - 1) & ~0x1;
        uint32_t hash = hashimm(a) & bitmask;
        uint32_t n = 7 << 1;
        LInsp k;
        while ((k = list[hash]) != NULL &&
            (!k->isconst() || k->imm32() != a))
        {
            hash = (hash + (n += 2)) & bitmask;        // quadratic probe
        }
        i = hash;
        return k;
    }

    LInsp LInsHashSet::find64(LOpcode v, uint64_t a, uint32_t &i)
    {
        uint32_t cap = m_cap;
        const LInsp *list = m_list;
        const uint32_t bitmask = (cap - 1) & ~0x1;
        uint32_t hash = hashimmq(a) & bitmask;
        uint32_t n = 7 << 1;
        LInsp k;
        while ((k = list[hash]) != NULL &&
            (k->opcode() != v || k->imm64() != a))
        {
            hash = (hash + (n += 2)) & bitmask;        // quadratic probe
        }
        i = hash;
        return k;
    }

    LInsp LInsHashSet::find1(LOpcode op, LInsp a, uint32_t &i)
    {
        uint32_t cap = m_cap;
        const LInsp *list = m_list;
        const uint32_t bitmask = (cap - 1) & ~0x1;
        uint32_t hash = hash1(op,a) & bitmask;
        uint32_t n = 7 << 1;
        LInsp k;
        while ((k = list[hash]) != NULL &&
            (k->opcode() != op || k->oprnd1() != a))
        {
            hash = (hash + (n += 2)) & bitmask;        // quadratic probe
        }
        i = hash;
        return k;
    }

    LInsp LInsHashSet::find2(LOpcode op, LInsp a, LInsp b, uint32_t &i)
    {
        uint32_t cap = m_cap;
        const LInsp *list = m_list;
        const uint32_t bitmask = (cap - 1) & ~0x1;
        uint32_t hash = hash2(op,a,b) & bitmask;
        uint32_t n = 7 << 1;
        LInsp k;
        while ((k = list[hash]) != NULL &&
            (k->opcode() != op || k->oprnd1() != a || k->oprnd2() != b))
        {
            hash = (hash + (n += 2)) & bitmask;        // quadratic probe
        }
        i = hash;
        return k;
    }

    LInsp LInsHashSet::find3(LOpcode op, LInsp a, LInsp b, LInsp c, uint32_t &i)
    {
        uint32_t cap = m_cap;
        const LInsp *list = m_list;
        const uint32_t bitmask = (cap - 1) & ~0x1;
        uint32_t hash = hash3(op,a,b,c) & bitmask;
        uint32_t n = 7 << 1;
        LInsp k;
        while ((k = list[hash]) != NULL &&
            (k->opcode() != op || k->oprnd1() != a || k->oprnd2() != b || k->oprnd3() != c))
        {
            hash = (hash + (n += 2)) & bitmask;     // quadratic probe
        }
        i = hash;
        return k;
    }

    LInsp LInsHashSet::findLoad(LOpcode op, LInsp a, int32_t d, uint32_t &i)
    {
        uint32_t cap = m_cap;
        const LInsp *list = m_list;
        const uint32_t bitmask = (cap - 1) & ~0x1;
        uint32_t hash = hashLoad(op,a,d) & bitmask;
        uint32_t n = 7 << 1;
        LInsp k;
        while ((k = list[hash]) != NULL &&
            (k->opcode() != op || k->oprnd1() != a || k->disp() != d))
        {
            hash = (hash + (n += 2)) & bitmask;        // quadratic probe
        }
        i = hash;
        return k;
    }

    bool argsmatch(LInsp i, uint32_t argc, LInsp args[])
    {
        for (uint32_t j=0; j < argc; j++)
            if (i->arg(j) != args[j])
                return false;
        return true;
    }

    LInsp LInsHashSet::findcall(const CallInfo *ci, uint32_t argc, LInsp args[], uint32_t &i)
    {
        uint32_t cap = m_cap;
        const LInsp *list = m_list;
        const uint32_t bitmask = (cap - 1) & ~0x1;
        uint32_t hash = hashcall(ci, argc, args) & bitmask;
        uint32_t n = 7 << 1;
        LInsp k;
        while ((k = list[hash]) != NULL &&
            (!k->isCall() || k->callInfo() != ci || !argsmatch(k, argc, args)))
        {
            hash = (hash + (n += 2)) & bitmask;        // quadratic probe
        }
        i = hash;
        return k;
    }

#ifdef NJ_VERBOSE
    class RetiredEntry
    {
    public:
        Seq<LIns*>* live;
        LIns* i;
        RetiredEntry(): live(NULL), i(NULL) {}
    };

    class LiveTable
    {
        Allocator& alloc;
    public:
        HashMap<LIns*, LIns*> live;
        SeqBuilder<RetiredEntry*> retired;
        int retiredCount;
        int maxlive;
        LiveTable(Allocator& alloc) 
            : alloc(alloc)
            , live(alloc)
            , retired(alloc)
            , retiredCount(0)
            , maxlive(0)
        { }

        void add(LInsp i, LInsp use) {
            if (!i->isconst() && !i->isconstq() && !live.containsKey(i)) {
                NanoAssert(size_t(i->opcode()) < sizeof(lirNames) / sizeof(lirNames[0]));
                live.put(i,use);
            }
        }

        void retire(LInsp i) {
            RetiredEntry *e = new (alloc) RetiredEntry();
            e->i = i;
            SeqBuilder<LIns*> livelist(alloc);
            HashMap<LIns*, LIns*>::Iter iter(live);
            int live_count = 0;
            while (iter.next()) {
                LIns* ins = iter.key();
                if (!ins->isStore() && !ins->isGuard()) {
                    live_count++;
                    livelist.insert(ins);
                }
            }
            e->live = livelist.get();
            if (live_count > maxlive)
                maxlive = live_count;

            live.remove(i);
            retired.insert(e);
            retiredCount++;
        }

        bool contains(LInsp i) {
            return live.containsKey(i);
        }
    };

    /*
     * traverse the LIR buffer and discover which instructions are live
     * by starting from instructions with side effects (stores, calls, branches)
     * and marking instructions used by them.  Works bottom-up, in one pass.
     * if showLiveRefs == true, also print the set of live expressions next to
     * each instruction
     */
    void live(Allocator& alloc, Fragment *frag, LogControl *logc)
    {
        // traverse backwards to find live exprs and a few other stats.

        LiveTable live(alloc);
        uint32_t exits = 0;
        LirReader br(frag->lastIns);
        StackFilter sf(&br, alloc, frag->lirbuf, frag->lirbuf->sp, frag->lirbuf->rp);
        int total = 0;
        if (frag->lirbuf->state)
            live.add(frag->lirbuf->state, sf.pos());
        for (LInsp i = sf.read(); !i->isop(LIR_start); i = sf.read())
        {
            total++;

            // first handle side-effect instructions
            if (i->isStmt())
            {
                live.add(i,0);
                if (i->isGuard())
                    exits++;
            }

            // now propagate liveness
            if (live.contains(i))
            {
                live.retire(i);
                NanoAssert(size_t(i->opcode()) < sizeof(operandCount) / sizeof(operandCount[0]));
                if (i->isStore()) {
                    live.add(i->oprnd2(),i); // base
                    live.add(i->oprnd1(),i); // val
                }
                else if (i->isop(LIR_cmov) || i->isop(LIR_qcmov)) {
                    live.add(i->oprnd1(),i);
                    live.add(i->oprnd2(),i);
                    live.add(i->oprnd3(),i);
                }
                else if (operandCount[i->opcode()] == 1) {
                    live.add(i->oprnd1(),i);
                }
                else if (operandCount[i->opcode()] == 2) {
                    live.add(i->oprnd1(),i);
                    live.add(i->oprnd2(),i);
                }
                else if (i->isCall()) {
                    for (int j=0, c=i->argc(); j < c; j++)
                        live.add(i->arg(j),i);
                }
            }
        }

        logc->printf("  Live instruction count %d, total %u, max pressure %d\n",
                     live.retiredCount, total, live.maxlive);
        if (exits > 0)
            logc->printf("  Side exits %u\n", exits);
        logc->printf("  Showing LIR instructions with live-after variables\n");
        logc->printf("\n");

        // print live exprs, going forwards
        LirNameMap *names = frag->lirbuf->names;
        bool newblock = true;
        for (Seq<RetiredEntry*>* p = live.retired.get(); p != NULL; p = p->tail) {
            RetiredEntry* e = p->head;
            char livebuf[4000], *s=livebuf;
            *s = 0;
            if (!newblock && e->i->isop(LIR_label)) {
                logc->printf("\n");
            }
            newblock = false;
            for (Seq<LIns*>* p = e->live; p != NULL; p = p->tail) {
                VMPI_strcpy(s, names->formatRef(p->head));
                s += VMPI_strlen(s);
                *s++ = ' '; *s = 0;
                NanoAssert(s < livebuf+sizeof(livebuf));
            }
            /* If the LIR insn is pretty short, print it and its
               live-after set on the same line.  If not, put
               live-after set on a new line, suitably indented. */
            const char* insn_text = names->formatIns(e->i);
            if (VMPI_strlen(insn_text) >= 30-2) {
                logc->printf("  %-30s\n  %-30s %s\n", names->formatIns(e->i), "", livebuf);
            } else {
                logc->printf("  %-30s %s\n", names->formatIns(e->i), livebuf);
            }

            if (e->i->isGuard() || e->i->isBranch() || e->i->isRet()) {
                logc->printf("\n");
                newblock = true;
            }
        }
    }

    void LirNameMap::addName(LInsp i, const char* name) {
        if (!names.containsKey(i)) {
            char *copy = new (alloc) char[VMPI_strlen(name)+1];
            VMPI_strcpy(copy, name);
            Entry *e = new (alloc) Entry(copy);
            names.put(i, e);
        }
    }

    void LirNameMap::copyName(LInsp i, const char *s, int suffix) {
        char s2[200];
        if (VMPI_isdigit(s[VMPI_strlen(s)-1])) {
            // if s ends with a digit, add '_' to clarify the suffix
            VMPI_sprintf(s2,"%s_%d", s, suffix);
        } else {
            VMPI_sprintf(s2,"%s%d", s, suffix);
        }
        addName(i, s2);
    }

    void LirNameMap::formatImm(int32_t c, char *buf) {
        if (c >= 10000 || c <= -10000)
            VMPI_sprintf(buf,"#%s",labels->format((void*)c));
        else
            VMPI_sprintf(buf,"%d", c);
    }

    const char* LirNameMap::formatRef(LIns *ref)
    {
        char buffer[200], *buf=buffer;
        buf[0]=0;
        if (names.containsKey(ref)) {
            const char* name = names.get(ref)->name;
            VMPI_strcat(buf, name);
        }
        else if (ref->isconstf()) {
            VMPI_sprintf(buf, "%g", ref->imm64f());
        }
        else if (ref->isconstq()) {
            int64_t c = ref->imm64();
            if (c >= 10000 || c <= -10000)
                VMPI_sprintf(buf, "#0x%llxLL", (long long unsigned int) c);
            else
                VMPI_sprintf(buf, "%dLL", (int)c);
        }
        else if (ref->isconst()) {
            formatImm(ref->imm32(), buf);
        }
        else {
            if (ref->isCall()) {
#if !defined NANOJIT_64BIT
                if (ref->isop(LIR_callh)) {
                    // we've presumably seen the other half already
                    ref = ref->oprnd1();
                } else {
#endif
                    copyName(ref, ref->callInfo()->_name, funccounts.add(ref->callInfo()));
#if !defined NANOJIT_64BIT
                }
#endif
            } else {
                NanoAssert(size_t(ref->opcode()) < sizeof(lirNames) / sizeof(lirNames[0]));
                copyName(ref, lirNames[ref->opcode()], lircounts.add(ref->opcode()));
            }
            const char* name = names.get(ref)->name;
            VMPI_strcat(buf, name);
        }
        return labels->dup(buffer);
    }

    const char* LirNameMap::formatIns(LIns* i)
    {
        char sbuf[200];
        char *s = sbuf;
        LOpcode op = i->opcode();
        switch(op)
        {
            case LIR_int:
            {
                VMPI_sprintf(s, "%s = %s %d", formatRef(i), lirNames[op], i->imm32());
                break;
            }

            case LIR_alloc: {
                VMPI_sprintf(s, "%s = %s %d", formatRef(i), lirNames[op], i->size());
                break;
            }

            case LIR_quad:
            {
                VMPI_sprintf(s, "%s = %s #%X:%X /* %g */", formatRef(i), lirNames[op],
                             i->imm64_1(), i->imm64_0(), i->imm64f());
                break;
            }

            case LIR_float:
            {
                VMPI_sprintf(s, "%s = %s #%g", formatRef(i), lirNames[op], i->imm64f());
                break;
            }

            case LIR_start:
            case LIR_regfence:
                VMPI_sprintf(s, "%s", lirNames[op]);
                break;

            case LIR_qcall:
            case LIR_fcall:
            case LIR_icall: {
                const CallInfo* call = i->callInfo();
                int32_t argc = i->argc();
                if (call->isIndirect())
                    VMPI_sprintf(s, "%s = %s [%s] ( ", formatRef(i), lirNames[op], formatRef(i->arg(--argc)));
                else
                    VMPI_sprintf(s, "%s = %s #%s ( ", formatRef(i), lirNames[op], call->_name);
                for (int32_t j = argc - 1; j >= 0; j--) {
                    s += VMPI_strlen(s);
                    VMPI_sprintf(s, "%s ",formatRef(i->arg(j)));
                }
                s += VMPI_strlen(s);
                VMPI_sprintf(s, ")");
                break;
            }

            case LIR_param: {
                uint32_t arg = i->paramArg();
                if (!i->paramKind()) {
                    if (arg < sizeof(Assembler::argRegs)/sizeof(Assembler::argRegs[0])) {
                        VMPI_sprintf(s, "%s = %s %d %s", formatRef(i), lirNames[op],
                            arg, gpn(Assembler::argRegs[arg]));
                    } else {
                        VMPI_sprintf(s, "%s = %s %d", formatRef(i), lirNames[op], arg);
                    }
                } else {
                    VMPI_sprintf(s, "%s = %s %d %s", formatRef(i), lirNames[op],
                        arg, gpn(Assembler::savedRegs[arg]));
                }
                break;
            }

            case LIR_label:
                VMPI_sprintf(s, "%s:", formatRef(i));
                break;

            case LIR_jt:
            case LIR_jf:
                VMPI_sprintf(s, "%s %s -> %s", lirNames[op], formatRef(i->oprnd1()),
                    i->oprnd2() ? formatRef(i->oprnd2()) : "unpatched");
                break;

            case LIR_j:
                VMPI_sprintf(s, "%s -> %s", lirNames[op],
                    i->oprnd2() ? formatRef(i->oprnd2()) : "unpatched");
                break;

            case LIR_live:
            case LIR_ret:
            case LIR_fret:
                VMPI_sprintf(s, "%s %s", lirNames[op], formatRef(i->oprnd1()));
                break;

            case LIR_callh:
            case LIR_neg:
            case LIR_fneg:
            case LIR_i2f:
            case LIR_u2f:
            case LIR_qlo:
            case LIR_qhi:
            case LIR_ov:
            case LIR_not:
            case LIR_mod:
            case LIR_i2q:
            case LIR_u2q:
                VMPI_sprintf(s, "%s = %s %s", formatRef(i), lirNames[op], formatRef(i->oprnd1()));
                break;

            case LIR_x:
            case LIR_xt:
            case LIR_xf:
            case LIR_xbarrier:
            case LIR_xtbl:
                formatGuard(i, s);
                break;

            case LIR_add:       case LIR_qiadd:
            case LIR_iaddp:     case LIR_qaddp:
            case LIR_sub:
            case LIR_mul:
            case LIR_div:
            case LIR_fadd:
            case LIR_fsub:
            case LIR_fmul:
            case LIR_fdiv:
            case LIR_and:       case LIR_qiand:
            case LIR_or:        case LIR_qior:
            case LIR_xor:       case LIR_qxor:
            case LIR_lsh:       case LIR_qilsh:
            case LIR_rsh:       case LIR_qirsh:
            case LIR_ush:       case LIR_qursh:
            case LIR_eq:        case LIR_qeq:
            case LIR_lt:        case LIR_qlt:
            case LIR_le:        case LIR_qle:
            case LIR_gt:        case LIR_qgt:
            case LIR_ge:        case LIR_qge:
            case LIR_ult:       case LIR_qult:
            case LIR_ule:       case LIR_qule:
            case LIR_ugt:       case LIR_qugt:
            case LIR_uge:       case LIR_quge:
            case LIR_feq:
            case LIR_flt:
            case LIR_fle:
            case LIR_fgt:
            case LIR_fge:
                VMPI_sprintf(s, "%s = %s %s, %s", formatRef(i), lirNames[op],
                    formatRef(i->oprnd1()),
                    formatRef(i->oprnd2()));
                break;

            case LIR_qjoin:
                VMPI_sprintf(s, "%s (%s), %s", lirNames[op],
                    formatIns(i->oprnd1()),
                     formatRef(i->oprnd2()));
                 break;

            case LIR_qcmov:
            case LIR_cmov:
                VMPI_sprintf(s, "%s = %s %s ? %s : %s", formatRef(i), lirNames[op],
                    formatRef(i->oprnd1()),
                    formatRef(i->oprnd2()),
                    formatRef(i->oprnd3()));
                break;

            case LIR_ld:
            case LIR_ldc:
            case LIR_ldq:
            case LIR_ldqc:
            case LIR_ldcb:
            case LIR_ldcs:
                VMPI_sprintf(s, "%s = %s %s[%d]", formatRef(i), lirNames[op],
                    formatRef(i->oprnd1()),
                    i->disp());
                break;

            case LIR_sti:
            case LIR_stqi:
                VMPI_sprintf(s, "%s %s[%d] = %s", lirNames[op],
                    formatRef(i->oprnd2()),
                    i->disp(),
                    formatRef(i->oprnd1()));
                break;

            default:
                VMPI_sprintf(s, "?");
                break;
        }
        return labels->dup(sbuf);
    }


#endif
    CseFilter::CseFilter(LirWriter *out, Allocator& alloc)
        : LirWriter(out), exprs(alloc) {}

    LIns* CseFilter::insImm(int32_t imm)
    {
        uint32_t k;
        LInsp found = exprs.find32(imm, k);
        if (found)
            return found;
        return exprs.add(out->insImm(imm), k);
    }

    LIns* CseFilter::insImmq(uint64_t q)
    {
        uint32_t k;
        LInsp found = exprs.find64(LIR_quad, q, k);
        if (found)
            return found;
        return exprs.add(out->insImmq(q), k);
    }

    LIns* CseFilter::insImmf(double d)
    {
        uint32_t k;
        union {
            double d;
            uint64_t u64;
        } u;
        u.d = d;
        LInsp found = exprs.find64(LIR_float, u.u64, k);
        if (found)
            return found;
        return exprs.add(out->insImmf(d), k);
    }

    LIns* CseFilter::ins0(LOpcode v)
    {
        if (v == LIR_label)
            exprs.clear();
        return out->ins0(v);
    }

    LIns* CseFilter::ins1(LOpcode v, LInsp a)
    {
        if (isCseOpcode(v)) {
            NanoAssert(operandCount[v]==1);
            uint32_t k;
            LInsp found = exprs.find1(v, a, k);
            if (found)
                return found;
            return exprs.add(out->ins1(v,a), k);
        }
        return out->ins1(v,a);
    }

    LIns* CseFilter::ins2(LOpcode v, LInsp a, LInsp b)
    {
        if (isCseOpcode(v)) {
            NanoAssert(operandCount[v]==2);
            uint32_t k;
            LInsp found = exprs.find2(v, a, b, k);
            if (found)
                return found;
            return exprs.add(out->ins2(v,a,b), k);
        }
        return out->ins2(v,a,b);
    }

    LIns* CseFilter::ins3(LOpcode v, LInsp a, LInsp b, LInsp c)
    {
        NanoAssert(isCseOpcode(v));
        NanoAssert(operandCount[v]==3);
        uint32_t k;
        LInsp found = exprs.find3(v, a, b, c, k);
        if (found)
            return found;
        return exprs.add(out->ins3(v,a,b,c), k);
    }

    LIns* CseFilter::insLoad(LOpcode v, LInsp base, int32_t disp)
    {
        if (isCseOpcode(v)) {
            NanoAssert(operandCount[v]==1);
            uint32_t k;
            LInsp found = exprs.findLoad(v, base, disp, k);
            if (found)
                return found;
            return exprs.add(out->insLoad(v,base,disp), k);
        }
        return out->insLoad(v,base,disp);
    }

    LInsp CseFilter::insGuard(LOpcode v, LInsp c, GuardRecord *gr)
    {
        // LIR_xt and LIR_xf guards are CSEable.  Note that we compare the
        // opcode and condition when determining if two guards are equivalent
        // -- in find1(), hash1(), equals() and hashcode() -- but we do *not*
        // compare the GuardRecord.  This works because:
        // - If guard 1 is taken (exits) then guard 2 is never reached, so
        //   guard 2 can be removed.
        // - If guard 1 is not taken then neither is guard 2, so guard 2 can
        //   be removed.
        //
        // The underlying assumptions that are required for this to be safe:
        // - There's never a path from the side exit of guard 1 back to guard
        //   2;  for tree-shaped fragments this should be true.
        // - GuardRecords do not contain information other than what is needed
        //   to execute a successful exit.  That is currently true.
        // - The CSE algorithm will always keep guard 1 and remove guard 2
        //   (not vice versa).  The current algorithm does this.
        //
        if (isCseOpcode(v)) {
            // conditional guard
            NanoAssert(operandCount[v]==1);
            uint32_t k;
            LInsp found = exprs.find1(v, c, k);
            if (found)
                return 0;
            return exprs.add(out->insGuard(v,c,gr), k);
        }
        return out->insGuard(v, c, gr);
    }

    LInsp CseFilter::insCall(const CallInfo *ci, LInsp args[])
    {
        if (ci->_cse) {
            uint32_t k;
            uint32_t argc = ci->count_args();
            LInsp found = exprs.findcall(ci, argc, args, k);
            if (found)
                return found;
            return exprs.add(out->insCall(ci, args), k);
        }
        return out->insCall(ci, args);
    }

    void compile(Assembler* assm, Fragment* frag verbose_only(, Allocator& alloc, LabelMap* labels))
    {
        verbose_only(
        LogControl *logc = assm->_logc;
        bool anyVerb = (logc->lcbits & 0xFFFF & ~LC_FragProfile) > 0;
        bool asmVerb = (logc->lcbits & 0xFFFF & LC_Assembly) > 0;
        bool liveVerb = (logc->lcbits & 0xFFFF & LC_Liveness) > 0;
        )

        /* BEGIN decorative preamble */
        verbose_only(
        if (anyVerb) {
            logc->printf("========================================"
                         "========================================\n");
            logc->printf("=== BEGIN LIR::compile(%p, %p)\n",
                         (void*)assm, (void*)frag);
            logc->printf("===\n");
        })
        /* END decorative preamble */

        verbose_only( if (liveVerb) {
            logc->printf("\n");
            logc->printf("=== Results of liveness analysis:\n");
            logc->printf("===\n");
            live(alloc, frag, logc);
        })

        /* Set up the generic text output cache for the assembler */
        verbose_only( StringList asmOutput(alloc); )
        verbose_only( assm->_outputCache = &asmOutput; )

        assm->beginAssembly(frag);
        if (assm->error())
            return;

        //logc->printf("recompile trigger %X kind %d\n", (int)frag, frag->kind);

        verbose_only( if (anyVerb) {
            logc->printf("=== Translating LIR fragments into assembly:\n");
        })

        // now the the main trunk
        verbose_only( if (anyVerb) {
            logc->printf("=== -- Compile trunk %s: begin\n",
                         labels->format(frag));
        })
        assm->assemble(frag);
        verbose_only( if (anyVerb) {
            logc->printf("=== -- Compile trunk %s: end\n",
                         labels->format(frag));
        })

        verbose_only(
            if (asmVerb)
                assm->outputf("## compiling trunk %s",
                              labels->format(frag));
        )
        assm->endAssembly(frag);

        // reverse output so that assembly is displayed low-to-high
        // Up to this point, assm->_outputCache has been non-NULL, and so
        // has been accumulating output.  Now we set it to NULL, traverse
        // the entire list of stored strings, and hand them a second time
        // to assm->output.  Since _outputCache is now NULL, outputf just
        // hands these strings directly onwards to logc->printf.
        verbose_only( if (anyVerb) {
            logc->printf("\n");
            logc->printf("=== Aggregated assembly output: BEGIN\n");
            logc->printf("===\n");
            assm->_outputCache = 0;
            for (Seq<char*>* p = asmOutput.get(); p != NULL; p = p->tail) {
                char *str = p->head;
                assm->outputf("  %s", str);
            }
            logc->printf("===\n");
            logc->printf("=== Aggregated assembly output: END\n");
        });

        if (assm->error())
            frag->fragEntry = 0;

        verbose_only( frag->nCodeBytes += assm->codeBytes; )
        verbose_only( frag->nExitBytes += assm->exitBytes; )

        /* BEGIN decorative postamble */
        verbose_only( if (anyVerb) {
            logc->printf("\n");
            logc->printf("===\n");
            logc->printf("=== END LIR::compile(%p, %p)\n",
                         (void*)assm, (void*)frag);
            logc->printf("========================================"
                         "========================================\n");
            logc->printf("\n");
        });
        /* END decorative postamble */
    }

    LInsp LoadFilter::insLoad(LOpcode v, LInsp base, int32_t disp)
    {
        if (base != sp && base != rp && (v == LIR_ld || v == LIR_ldq)) {
            uint32_t k;
            LInsp found = exprs.findLoad(v, base, disp, k);
            if (found)
                return found;
            return exprs.add(out->insLoad(v,base,disp), k);
        }
        return out->insLoad(v, base, disp);
    }

    void LoadFilter::clear(LInsp p)
    {
        if (p != sp && p != rp)
            exprs.clear();
    }

    LInsp LoadFilter::insStorei(LInsp v, LInsp b, int32_t d)
    {
        clear(b);
        return out->insStorei(v, b, d);
    }

    LInsp LoadFilter::insCall(const CallInfo *ci, LInsp args[])
    {
        if (!ci->_cse)
            exprs.clear();
        return out->insCall(ci, args);
    }

    LInsp LoadFilter::ins0(LOpcode op)
    {
        if (op == LIR_label)
            exprs.clear();
        return out->ins0(op);
    }

    #endif /* FEATURE_NANOJIT */

#if defined(NJ_VERBOSE)
    LabelMap::LabelMap(Allocator& a, LogControl *logc)
        : allocator(a), names(a), logc(logc), end(buf)
    {}

    void LabelMap::add(const void *p, size_t size, size_t align, const char *name)
    {
        if (!this || names.containsKey(p))
            return;
        char* copy = new (allocator) char[VMPI_strlen(name)+1];
        VMPI_strcpy(copy, name);
        Entry *e = new (allocator) Entry(copy, size << align, align);
        names.put(p, e);
    }

    const char *LabelMap::format(const void *p)
    {
        char b[200];

        const void *start = names.findNear(p);
        if (start != NULL) {
            Entry *e = names.get(start);
            const void *end = (const char*)start + e->size;
            const char *name = e->name;
            if (p == start) {
                if (!(logc->lcbits & LC_NoCodeAddrs))
                    VMPI_sprintf(b,"%p %s",p,name);
                else
                    VMPI_strcpy(b, name);
                return dup(b);
            }
            else if (p > start && p < end) {
                int32_t d = int32_t(intptr_t(p)-intptr_t(start)) >> e->align;
                if (!(logc->lcbits & LC_NoCodeAddrs))
                    VMPI_sprintf(b, "%p %s+%d", p, name, d);
                else
                    VMPI_sprintf(b,"%s+%d", name, d);
                return dup(b);
            }
            else {
                VMPI_sprintf(b, "%p", p);
                return dup(b);
            }
        }
        VMPI_sprintf(b, "%p", p);
        return dup(b);
    }

    const char *LabelMap::dup(const char *b)
    {
        size_t need = VMPI_strlen(b)+1;
        NanoAssert(need <= sizeof(buf));
        char *s = end;
        end += need;
        if (end > buf+sizeof(buf)) {
            s = buf;
            end = s+need;
        }
        VMPI_strcpy(s, b);
        return s;
    }

    // ---------------------------------------------------------------
    // START debug-logging definitions
    // ---------------------------------------------------------------

    void LogControl::printf( const char* format, ... )
    {
        va_list vargs;
        va_start(vargs, format);
        vfprintf(stdout, format, vargs);
        va_end(vargs);
    }

#endif // NJ_VERBOSE


#ifdef DEBUG
    LIns* SanityFilter::ins1(LOpcode v, LIns* s0)
    {
        switch (v)
        {
            case LIR_fneg:
            case LIR_fret:
            case LIR_qlo:
            case LIR_qhi:
              NanoAssert(s0->isQuad());
              break;
            case LIR_not:
            case LIR_neg:
            case LIR_u2f:
            case LIR_i2f:
            case LIR_i2q: case LIR_u2q:
              NanoAssert(!s0->isQuad());
              break;
            default:
              break;
        }
        return out->ins1(v, s0);
    }

    LIns* SanityFilter::ins2(LOpcode v, LIns* s0, LIns* s1)
    {
        switch (v) {
          case LIR_fadd:
          case LIR_fsub:
          case LIR_fmul:
          case LIR_fdiv:
          case LIR_feq:
          case LIR_flt:
          case LIR_fgt:
          case LIR_fle:
          case LIR_fge:
          case LIR_qaddp:
          case LIR_qior:
          case LIR_qxor:
          case LIR_qiand:
          case LIR_qiadd:
          case LIR_qeq:
          case LIR_qlt: case LIR_qult:
          case LIR_qgt: case LIR_qugt:
          case LIR_qle: case LIR_quge:
            NanoAssert(s0->isQuad() && s1->isQuad());
            break;
          case LIR_add:
          case LIR_iaddp:
          case LIR_sub:
          case LIR_mul:
          case LIR_and:
          case LIR_or:
          case LIR_xor:
          case LIR_lsh:
          case LIR_rsh:
          case LIR_ush:
          case LIR_eq:
          case LIR_lt: case LIR_ult:
          case LIR_gt: case LIR_ugt:
          case LIR_le: case LIR_ule:
          case LIR_ge: case LIR_uge:
            NanoAssert(!s0->isQuad() && !s1->isQuad());
            break;
          case LIR_qilsh:
          case LIR_qirsh:
          case LIR_qursh:
            NanoAssert(s0->isQuad() && !s1->isQuad());
            break;
          default:
            break;
        }
        return out->ins2(v, s0, s1);
    }

    LIns* SanityFilter::ins3(LOpcode v, LIns* s0, LIns* s1, LIns* s2)
    {
        switch (v)
        {
          case LIR_cmov:
            NanoAssert(s0->isCond() || s0->isconst());
            NanoAssert(!s1->isQuad() && !s2->isQuad());
            break;
          case LIR_qcmov:
            NanoAssert(s0->isCond() || s0->isconst());
            NanoAssert(s1->isQuad() && s2->isQuad());
            break;
          default:
            break;
        }
        return out->ins3(v, s0, s1, s2);
    }
#endif

}
