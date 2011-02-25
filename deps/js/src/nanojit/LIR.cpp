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

    const uint8_t repKinds[] = {
#define OP___(op, number, repKind, retType, isCse) \
        LRK_##repKind,
#include "LIRopcode.tbl"
#undef OP___
        0
    };

    const LTy retTypes[] = {
#define OP___(op, number, repKind, retType, isCse) \
        LTy_##retType,
#include "LIRopcode.tbl"
#undef OP___
        LTy_V
    };

    const int8_t isCses[] = {
#define OP___(op, number, repKind, retType, isCse) \
        isCse,
#include "LIRopcode.tbl"
#undef OP___
        0
    };

    // LIR verbose specific
    #ifdef NJ_VERBOSE

    const char* lirNames[] = {
#define OP___(op, number, repKind, retType, isCse) \
        #op,
#include "LIRopcode.tbl"
#undef OP___
        NULL
    };

    #endif /* NANOJIT_VERBOSE */

    uint32_t CallInfo::count_args() const
    {
        uint32_t argc = 0;
        uint32_t argt = _typesig;
        argt >>= TYPESIG_FIELDSZB;      // remove retType
        while (argt) {
            argc++;
            argt >>= TYPESIG_FIELDSZB;
        }
        return argc;
    }

    uint32_t CallInfo::count_int32_args() const
    {
        uint32_t argc = 0;
        uint32_t argt = _typesig;
        argt >>= TYPESIG_FIELDSZB;      // remove retType
        while (argt) {
            ArgType a = ArgType(argt & TYPESIG_FIELDMASK);
            if (a == ARGTYPE_I || a == ARGTYPE_UI)
                argc++;
            argt >>= TYPESIG_FIELDSZB;
        }
        return argc;
    }

    uint32_t CallInfo::getArgTypes(ArgType* argTypes) const
    {
        uint32_t argc = 0;
        uint32_t argt = _typesig;
        argt >>= TYPESIG_FIELDSZB;      // remove retType
        while (argt) {
            ArgType a = ArgType(argt & TYPESIG_FIELDMASK);
            argTypes[argc] = a;
            argc++;
            argt >>= TYPESIG_FIELDSZB;
        }
        return argc;
    }

    // implementation
#ifdef NJ_VERBOSE
    void ReverseLister::finish()
    {
        _logc->printf("\n");
        _logc->printf("=== BEGIN %s ===\n", _title);
        int j = 0;
        for (Seq<char*>* p = _strs.get(); p != NULL; p = p->tail)
            _logc->printf("  %02d: %s\n", j++, p->head);
        _logc->printf("=== END %s ===\n", _title);
        _logc->printf("\n");
    }

    LIns* ReverseLister::read()
    {
        // This check is necessary to avoid printing the LIR_start multiple
        // times due to lookahead in Assembler::gen().
        if (_prevIns && _prevIns->isop(LIR_start))
            return _prevIns;
        LIns* ins = in->read();
        InsBuf b;
        const char* str = _printer->formatIns(&b, ins);
        char* cpy = new (_alloc) char[strlen(str)+1];
        VMPI_strcpy(cpy, str);
        _strs.insert(cpy);
        _prevIns = ins;
        return ins;
    }
#endif

    // LCompressedBuffer
    LirBuffer::LirBuffer(Allocator& alloc) :
#ifdef NJ_VERBOSE
          printer(NULL),
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
        ins->initLInsSk((LIns*)addrOfLastLInsOnCurrentChunk);
        _unused += sizeof(LInsSk);
        verbose_only(_stats.lir++);
    }

    // Make room for a single instruction.
    uintptr_t LirBuffer::makeRoom(size_t szB)
    {
        // Make sure the size is ok
        NanoAssert(0 == szB % sizeof(void*));
        NanoAssert(sizeof(LIns) <= szB && szB <= sizeof(LInsSt));  // LInsSt is the biggest one
        NanoAssert(_unused < _limit);

        debug_only( bool moved = false; )

        // If the instruction won't fit on the current chunk, get a new chunk
        if (_unused + szB > _limit) {
            uintptr_t addrOfLastLInsOnChunk = _unused - sizeof(LIns);
            moveToNewChunk(addrOfLastLInsOnChunk);
            debug_only( moved = true; )
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
            NanoAssert(!moved);     // shouldn't need to moveToNewChunk twice
            uintptr_t addrOfLastLInsOnChunk = _unused - sizeof(LIns);
            moveToNewChunk(addrOfLastLInsOnChunk);
        }

        // Make sure it's word-aligned.
        NanoAssert(0 == startOfRoom % sizeof(void*));
        return startOfRoom;
    }

    LIns* LirBufWriter::insStore(LOpcode op, LIns* val, LIns* base, int32_t d, AccSet accSet)
    {
        if (isS16(d)) {
            LInsSt* insSt = (LInsSt*)_buf->makeRoom(sizeof(LInsSt));
            LIns*   ins   = insSt->getLIns();
            ins->initLInsSt(op, val, base, d, accSet);
            return ins;
        } else {
            // If the displacement is more than 16 bits, put it in a separate instruction.
            return insStore(op, val, ins2(LIR_addp, base, insImmWord(d)), 0, accSet);
        }
    }

    LIns* LirBufWriter::ins0(LOpcode op)
    {
        LInsOp0* insOp0 = (LInsOp0*)_buf->makeRoom(sizeof(LInsOp0));
        LIns*    ins    = insOp0->getLIns();
        ins->initLInsOp0(op);
        return ins;
    }

    LIns* LirBufWriter::ins1(LOpcode op, LIns* o1)
    {
        LInsOp1* insOp1 = (LInsOp1*)_buf->makeRoom(sizeof(LInsOp1));
        LIns*    ins    = insOp1->getLIns();
        ins->initLInsOp1(op, o1);
        return ins;
    }

    LIns* LirBufWriter::ins2(LOpcode op, LIns* o1, LIns* o2)
    {
        LInsOp2* insOp2 = (LInsOp2*)_buf->makeRoom(sizeof(LInsOp2));
        LIns*    ins    = insOp2->getLIns();
        ins->initLInsOp2(op, o1, o2);
        return ins;
    }

    LIns* LirBufWriter::ins3(LOpcode op, LIns* o1, LIns* o2, LIns* o3)
    {
        LInsOp3* insOp3 = (LInsOp3*)_buf->makeRoom(sizeof(LInsOp3));
        LIns*    ins    = insOp3->getLIns();
        ins->initLInsOp3(op, o1, o2, o3);
        return ins;
    }

    LIns* LirBufWriter::insLoad(LOpcode op, LIns* base, int32_t d, AccSet accSet)
    {
        if (isS16(d)) {
            LInsLd* insLd = (LInsLd*)_buf->makeRoom(sizeof(LInsLd));
            LIns*   ins   = insLd->getLIns();
            ins->initLInsLd(op, base, d, accSet);
            return ins;
        } else {
            // If the displacement is more than 16 bits, put it in a separate instruction.
            // Note that CseFilter::insLoad() also does this, so this will
            // only occur if CseFilter has been removed from the pipeline.
            return insLoad(op, ins2(LIR_addp, base, insImmWord(d)), 0, accSet);
        }
    }

    LIns* LirBufWriter::insGuard(LOpcode op, LIns* c, GuardRecord *gr)
    {
        debug_only( if (LIR_x == op || LIR_xbarrier == op) NanoAssert(!c); )
        return ins2(op, c, (LIns*)gr);
    }

    LIns* LirBufWriter::insGuardXov(LOpcode op, LIns* a, LIns* b, GuardRecord *gr)
    {
        return ins3(op, a, b, (LIns*)gr);
    }

    LIns* LirBufWriter::insBranch(LOpcode op, LIns* condition, LIns* toLabel)
    {
        NanoAssert((op == LIR_j && !condition) ||
                   ((op == LIR_jf || op == LIR_jt) && condition));
        return ins2(op, condition, toLabel);
    }

    LIns* LirBufWriter::insBranchJov(LOpcode op, LIns* a, LIns* b, LIns* toLabel)
    {
        return ins3(op, a, b, toLabel);
    }

    LIns* LirBufWriter::insJtbl(LIns* index, uint32_t size)
    {
        LInsJtbl* insJtbl = (LInsJtbl*) _buf->makeRoom(sizeof(LInsJtbl));
        LIns**    table   = new (_buf->_allocator) LIns*[size];
        LIns*     ins     = insJtbl->getLIns();
        VMPI_memset(table, 0, size * sizeof(LIns*));
        ins->initLInsJtbl(index, size, table);
        return ins;
    }

    LIns* LirBufWriter::insAlloc(int32_t size)
    {
        size = (size+3)>>2; // # of required 32bit words
        LInsI* insI = (LInsI*)_buf->makeRoom(sizeof(LInsI));
        LIns*  ins  = insI->getLIns();
        ins->initLInsI(LIR_allocp, size);
        return ins;
    }

    LIns* LirBufWriter::insParam(int32_t arg, int32_t kind)
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

    LIns* LirBufWriter::insImmI(int32_t imm)
    {
        LInsI* insI = (LInsI*)_buf->makeRoom(sizeof(LInsI));
        LIns*  ins  = insI->getLIns();
        ins->initLInsI(LIR_immi, imm);
        return ins;
    }

#ifdef NANOJIT_64BIT
    LIns* LirBufWriter::insImmQ(uint64_t imm)
    {
        LInsQorD* insQorD = (LInsQorD*)_buf->makeRoom(sizeof(LInsQorD));
        LIns*     ins     = insQorD->getLIns();
        ins->initLInsQorD(LIR_immq, imm);
        return ins;
    }
#endif

    LIns* LirBufWriter::insImmD(double d)
    {
        LInsQorD* insQorD = (LInsQorD*)_buf->makeRoom(sizeof(LInsQorD));
        LIns*     ins     = insQorD->getLIns();
        union {
            double d;
            uint64_t q;
        } u;
        u.d = d;
        ins->initLInsQorD(LIR_immd, u.q);
        return ins;
    }

    // Reads the next non-skip instruction.
    LIns* LirReader::read()
    {
        static const uint8_t insSizes[] = {
        // LIR_start is treated specially -- see below.
#define OP___(op, number, repKind, retType, isCse) \
            ((number) == LIR_start ? 0 : sizeof(LIns##repKind)),
#include "LIRopcode.tbl"
#undef OP___
            0
        };

        // Check the invariant: _ins never points to a skip.
        NanoAssert(_ins && !_ins->isop(LIR_skip));

        // Step back one instruction.  Use a table lookup rather than a switch
        // to avoid branch mispredictions.  LIR_start is given a special size
        // of zero so that we don't step back past the start of the block.
        // (Callers of this function should stop once they see a LIR_start.)
        LIns* ret = _ins;
        _ins = (LIns*)(uintptr_t(_ins) - insSizes[_ins->opcode()]);

        // Ensure _ins doesn't end up pointing to a skip.
        while (_ins->isop(LIR_skip)) {
            NanoAssert(_ins->prevLIns() != _ins);
            _ins = _ins->prevLIns();
        }

        return ret;
    }

    LOpcode arithOpcodeD2I(LOpcode op)
    {
        switch (op) {
        case LIR_negd:  return LIR_negi;
        case LIR_addd:  return LIR_addi;
        case LIR_subd:  return LIR_subi;
        case LIR_muld:  return LIR_muli;
        default:        NanoAssert(0); return LIR_skip;
        }
    }

#ifdef NANOJIT_64BIT
    LOpcode cmpOpcodeI2Q(LOpcode op)
    {
        switch (op) {
        case LIR_eqi:    return LIR_eqq;
        case LIR_lti:    return LIR_ltq;
        case LIR_gti:    return LIR_gtq;
        case LIR_lei:    return LIR_leq;
        case LIR_gei:    return LIR_geq;
        case LIR_ltui:   return LIR_ltuq;
        case LIR_gtui:   return LIR_gtuq;
        case LIR_leui:   return LIR_leuq;
        case LIR_geui:   return LIR_geuq;
        default:        NanoAssert(0); return LIR_skip;
        }
    }
#endif

    LOpcode cmpOpcodeD2I(LOpcode op)
    {
        switch (op) {
        case LIR_eqd:    return LIR_eqi;
        case LIR_ltd:    return LIR_lti;
        case LIR_gtd:    return LIR_gti;
        case LIR_led:    return LIR_lei;
        case LIR_ged:    return LIR_gei;
        default:        NanoAssert(0); return LIR_skip;
        }
    }

    LOpcode cmpOpcodeD2UI(LOpcode op)
    {
        switch (op) {
        case LIR_eqd:    return LIR_eqi;
        case LIR_ltd:    return LIR_ltui;
        case LIR_gtd:    return LIR_gtui;
        case LIR_led:    return LIR_leui;
        case LIR_ged:    return LIR_geui;
        default:        NanoAssert(0); return LIR_skip;
        }
    }

    // This is never called, but that's ok because it contains only static
    // assertions.
    void LIns::staticSanityCheck()
    {
        // LIns must be word-sized.
        NanoStaticAssert(sizeof(LIns) == 1*sizeof(void*));

        // LInsXYZ have expected sizes too.
        NanoStaticAssert(sizeof(LInsOp0)  == 1*sizeof(void*));
        NanoStaticAssert(sizeof(LInsOp1)  == 2*sizeof(void*));
        NanoStaticAssert(sizeof(LInsOp2)  == 3*sizeof(void*));
        NanoStaticAssert(sizeof(LInsOp3)  == 4*sizeof(void*));
        NanoStaticAssert(sizeof(LInsLd)   == 3*sizeof(void*));
        NanoStaticAssert(sizeof(LInsSt)   == 4*sizeof(void*));
        NanoStaticAssert(sizeof(LInsSk)   == 2*sizeof(void*));
        NanoStaticAssert(sizeof(LInsC)    == 3*sizeof(void*));
        NanoStaticAssert(sizeof(LInsP)    == 2*sizeof(void*));
        NanoStaticAssert(sizeof(LInsI)    == 2*sizeof(void*));
    #if defined NANOJIT_64BIT
        NanoStaticAssert(sizeof(LInsQorD) == 2*sizeof(void*));
    #else
        NanoStaticAssert(sizeof(LInsQorD) == 3*sizeof(void*));
    #endif
        NanoStaticAssert(sizeof(LInsJtbl) == 4*sizeof(void*));

        // oprnd_1 must be in the same position in LIns{Op1,Op2,Op3,Ld,St,Jtbl}
        // because oprnd1() is used for all of them.
        #define OP1OFFSET (offsetof(LInsOp1,  ins) - offsetof(LInsOp1,  oprnd_1))
        NanoStaticAssert( OP1OFFSET == (offsetof(LInsOp2,  ins) - offsetof(LInsOp2,  oprnd_1)) );
        NanoStaticAssert( OP1OFFSET == (offsetof(LInsOp3,  ins) - offsetof(LInsOp3,  oprnd_1)) );
        NanoStaticAssert( OP1OFFSET == (offsetof(LInsLd,   ins) - offsetof(LInsLd,   oprnd_1)) );
        NanoStaticAssert( OP1OFFSET == (offsetof(LInsSt,   ins) - offsetof(LInsSt,   oprnd_1)) );
        NanoStaticAssert( OP1OFFSET == (offsetof(LInsJtbl, ins) - offsetof(LInsJtbl, oprnd_1)) );

        // oprnd_2 must be in the same position in LIns{Op2,Op3,St}
        // because oprnd2() is used for all of them.
        #define OP2OFFSET (offsetof(LInsOp2, ins) - offsetof(LInsOp2, oprnd_2))
        NanoStaticAssert( OP2OFFSET == (offsetof(LInsOp3, ins) - offsetof(LInsOp3, oprnd_2)) );
        NanoStaticAssert( OP2OFFSET == (offsetof(LInsSt,  ins) - offsetof(LInsSt,  oprnd_2)) );
    }

    bool insIsS16(LIns* i)
    {
        if (i->isImmI()) {
            int c = i->immI();
            return isS16(c);
        }
        if (i->isCmov()) {
            return insIsS16(i->oprnd2()) && insIsS16(i->oprnd3());
        }
        if (i->isCmp())
            return true;
        // many other possibilities too.
        return false;
    }

    LIns* ExprFilter::ins1(LOpcode v, LIns* oprnd)
    {
        switch (v) {
#ifdef NANOJIT_64BIT
        case LIR_q2i:
            if (oprnd->isImmQ())
                return insImmI(oprnd->immQlo());
            break;
        case LIR_i2q:
            if (oprnd->isImmI())
                return insImmQ(int64_t(int32_t(oprnd->immI())));
            break;
        case LIR_ui2uq:
            if (oprnd->isImmI())
                return insImmQ(uint64_t(uint32_t(oprnd->immI())));
            break;
#endif
#if NJ_SOFTFLOAT_SUPPORTED
        case LIR_dlo2i:
            if (oprnd->isImmD())
                return insImmI(oprnd->immDlo());
            if (oprnd->isop(LIR_ii2d))
                return oprnd->oprnd1();
            break;
        case LIR_dhi2i:
            if (oprnd->isImmD())
                return insImmI(oprnd->immDhi());
            if (oprnd->isop(LIR_ii2d))
                return oprnd->oprnd2();
            break;
#endif
        case LIR_noti:
            if (oprnd->isImmI())
                return insImmI(~oprnd->immI());
        involution:
            if (v == oprnd->opcode())
                return oprnd->oprnd1();
            break;
        case LIR_negi:
            if (oprnd->isImmI())
                return insImmI(-oprnd->immI());
            if (oprnd->isop(LIR_subi)) // -(a-b) = b-a
                return out->ins2(LIR_subi, oprnd->oprnd2(), oprnd->oprnd1());
            goto involution;
        case LIR_negd:
            if (oprnd->isImmD())
                return insImmD(-oprnd->immD());
            if (oprnd->isop(LIR_subd))
                return out->ins2(LIR_subd, oprnd->oprnd2(), oprnd->oprnd1());
            goto involution;
        case LIR_i2d:
            if (oprnd->isImmI())
                return insImmD(oprnd->immI());
            break;
        case LIR_d2i:
            if (oprnd->isImmD())
                return insImmI(int32_t(oprnd->immD()));
            break;
        case LIR_ui2d:
            if (oprnd->isImmI())
                return insImmD(uint32_t(oprnd->immI()));
            break;
        default:
            ;
        }

        return out->ins1(v, oprnd);
    }

    // This is an ugly workaround for an apparent compiler
    // bug; in VC2008, compiling with optimization on
    // will produce spurious errors if this code is inlined
    // into ExprFilter::ins2(). See https://bugzilla.mozilla.org/show_bug.cgi?id=538504
    inline double do_join(int32_t c1, int32_t c2)
    {
        union {
            double d;
            uint64_t u64;
        } u;
        u.u64 = uint32_t(c1) | uint64_t(c2)<<32;
        return u.d;
    }

    LIns* ExprFilter::ins2(LOpcode v, LIns* oprnd1, LIns* oprnd2)
    {
        NanoAssert(oprnd1 && oprnd2);

        if (oprnd1 == oprnd2) {
            // The operands are equal.
            switch (v) {
            case LIR_xori:
            case LIR_subi:
            case LIR_ltui:
            case LIR_gtui:
            case LIR_gti:
            case LIR_lti:
                return insImmI(0);

            case LIR_ori:
            case LIR_andi:
                return oprnd1;

            case LIR_lei:
            case LIR_leui:
            case LIR_gei:
            case LIR_geui:
                return insImmI(1);      // (x <= x) == 1; (x >= x) == 1

            default:
                break;
            }
        }

        if (oprnd1->isImmI() && oprnd2->isImmI()) {
            // The operands are both 32-bit integer immediates.
            int32_t c1 = oprnd1->immI();
            int32_t c2 = oprnd2->immI();
            double d;
            int32_t r;

            switch (v) {
#if NJ_SOFTFLOAT_SUPPORTED
            case LIR_ii2d:  return insImmD(do_join(c1, c2));
#endif
            case LIR_eqi:   return insImmI(c1 == c2);
            case LIR_lti:   return insImmI(c1 <  c2);
            case LIR_gti:   return insImmI(c1 >  c2);
            case LIR_lei:   return insImmI(c1 <= c2);
            case LIR_gei:   return insImmI(c1 >= c2);
            case LIR_ltui:  return insImmI(uint32_t(c1) <  uint32_t(c2));
            case LIR_gtui:  return insImmI(uint32_t(c1) >  uint32_t(c2));
            case LIR_leui:  return insImmI(uint32_t(c1) <= uint32_t(c2));
            case LIR_geui:  return insImmI(uint32_t(c1) >= uint32_t(c2));

            case LIR_rshi:  return insImmI(c1 >> c2);
            case LIR_lshi:  return insImmI(c1 << c2);
            case LIR_rshui: return insImmI(uint32_t(c1) >> c2);

            case LIR_ori:   return insImmI(c1 | c2);
            case LIR_andi:  return insImmI(c1 & c2);
            case LIR_xori:  return insImmI(c1 ^ c2);

            case LIR_addi:  d = double(c1) + double(c2);    goto fold;
            case LIR_subi:  d = double(c1) - double(c2);    goto fold;
            case LIR_muli:  d = double(c1) * double(c2);    goto fold;
            fold:
                // Make sure the constant expression doesn't overflow.  This
                // probably isn't necessary, because the C++ overflow
                // behaviour is very likely to be the same as the machine code
                // overflow behaviour, but we do it just to be safe.
                r = int32_t(d);
                if (r == d)
                    return insImmI(r);
                break;

#if defined NANOJIT_IA32 || defined NANOJIT_X64
            case LIR_divi:
            case LIR_modi:
                // We can't easily fold div and mod, since folding div makes it
                // impossible to calculate the mod that refers to it. The
                // frontend shouldn't emit div and mod with constant operands.
                NanoAssert(0);
#endif
            default:
                break;
            }

#ifdef NANOJIT_64BIT
        } else if (oprnd1->isImmQ() && oprnd2->isImmQ()) {
            // The operands are both 64-bit integer immediates.
            int64_t c1 = oprnd1->immQ();
            int64_t c2 = oprnd2->immQ();
            static const int64_t MIN_INT64 = int64_t(0x8000000000000000LL);
            static const int64_t MAX_INT64 = int64_t(0x7FFFFFFFFFFFFFFFLL);

            switch (v) {
            case LIR_eqq:   return insImmI(c1 == c2);
            case LIR_ltq:   return insImmI(c1 <  c2);
            case LIR_gtq:   return insImmI(c1 >  c2);
            case LIR_leq:   return insImmI(c1 <= c2);
            case LIR_geq:   return insImmI(c1 >= c2);
            case LIR_ltuq:  return insImmI(uint64_t(c1) <  uint64_t(c2));
            case LIR_gtuq:  return insImmI(uint64_t(c1) >  uint64_t(c2));
            case LIR_leuq:  return insImmI(uint64_t(c1) <= uint64_t(c2));
            case LIR_geuq:  return insImmI(uint64_t(c1) >= uint64_t(c2));

            case LIR_orq:   return insImmQ(c1 | c2);
            case LIR_andq:  return insImmQ(c1 & c2);
            case LIR_xorq:  return insImmQ(c1 ^ c2);

            // Nb: LIR_rshq, LIR_lshq and LIR_rshuq aren't here because their
            // second arg is a 32-bit int.

            case LIR_addq:
                // Overflow is only possible if both values are positive or
                // both negative.  Just like the 32-bit case, this check
                // probably isn't necessary, because the C++ overflow
                // behaviour is very likely to be the same as the machine code
                // overflow behaviour, but we do it just to be safe.
                if (c1 > 0 && c2 > 0) {
                    // Overflows if: c1 + c2 > MAX_INT64
                    // Re-express to avoid overflow in the check: c1 > MAX_INT64 - c2
                    if (c1 > MAX_INT64 - c2)
                        break;                  // overflow
                } else if (c1 < 0 && c2 < 0) {
                    // Overflows if: c1 + c2 < MIN_INT64
                    // Re-express to avoid overflow in the check: c1 < MIN_INT64 - c2
                    if (c1 < MIN_INT64 - c2)
                        break;                  // overflow
                }
                return insImmQ(c1 + c2);

            case LIR_subq:
                // Overflow is only possible if one value is positive and one
                // negative.
                if (c1 > 0 && c2 < 0) {
                    // Overflows if: c1 - c2 > MAX_INT64
                    // Re-express to avoid overflow in the check: c1 > MAX_INT64 + c2
                    if (c1 > MAX_INT64 + c2)
                        break;                  // overflow
                } else if (c1 < 0 && c2 > 0) {
                    // Overflows if: c1 - c2 < MIN_INT64
                    // Re-express to avoid overflow in the check: c1 < MIN_INT64 + c2
                    if (c1 < MIN_INT64 + c2)
                        break;                  // overflow
                }
                return insImmQ(c1 - c2);

            default:
                break;
            }
#endif
        } else if (oprnd1->isImmD() && oprnd2->isImmD()) {
            // The operands are both 64-bit double immediates.
            double c1 = oprnd1->immD();
            double c2 = oprnd2->immD();
            switch (v) {
            case LIR_eqd:   return insImmI(c1 == c2);
            case LIR_ltd:   return insImmI(c1 <  c2);
            case LIR_gtd:   return insImmI(c1 >  c2);
            case LIR_led:   return insImmI(c1 <= c2);
            case LIR_ged:   return insImmI(c1 >= c2);

            case LIR_addd:  return insImmD(c1 + c2);
            case LIR_subd:  return insImmD(c1 - c2);
            case LIR_muld:  return insImmD(c1 * c2);
            case LIR_divd:  return insImmD(c1 / c2);

            default:        break;
            }

        } else if (oprnd1->isImmI() && !oprnd2->isImmI()) {
            // The first operand is a 32-bit integer immediate;  move it to
            // the right if possible.
            switch (v) {
            case LIR_addi:
            case LIR_muli:
            case LIR_addd:
            case LIR_muld:
            case LIR_xori:
            case LIR_ori:
            case LIR_andi:
            case LIR_eqi: {
                // move const to rhs
                LIns* t = oprnd2;
                oprnd2 = oprnd1;
                oprnd1 = t;
                break;
            }
            default:
                if (isCmpIOpcode(v)) {
                    // move const to rhs, swap the operator
                    LIns *t = oprnd2;
                    oprnd2 = oprnd1;
                    oprnd1 = t;
                    v = invertCmpIOpcode(v);
                }
                break;
            }
        }

        if (oprnd2->isImmI()) {
            // The second operand is a 32-bit integer immediate.
            int c = oprnd2->immI();
            switch (v) {
            case LIR_addi:
                if (oprnd1->isop(LIR_addi) && oprnd1->oprnd2()->isImmI()) {
                    // add(add(x,c1),c2) => add(x,c1+c2)
                    c += oprnd1->oprnd2()->immI();
                    oprnd2 = insImmI(c);
                    oprnd1 = oprnd1->oprnd1();
                }
                break;

            case LIR_subi:
                if (oprnd1->isop(LIR_addi) && oprnd1->oprnd2()->isImmI()) {
                    // sub(add(x,c1),c2) => add(x,c1-c2)
                    c = oprnd1->oprnd2()->immI() - c;
                    oprnd2 = insImmI(c);
                    oprnd1 = oprnd1->oprnd1();
                    v = LIR_addi;
                }
                break;

            case LIR_rshi:
                if (c == 16 && oprnd1->isop(LIR_lshi) &&
                    oprnd1->oprnd2()->isImmI(16) &&
                    insIsS16(oprnd1->oprnd1()))
                {
                    // rsh(lhs(x,16),16) == x, if x is S16
                    return oprnd1->oprnd1();
                }
                break;

            default:
                break;
            }

            if (c == 0) {
                switch (v) {
                case LIR_addi:
                case LIR_ori:
                case LIR_xori:
                case LIR_subi:
                case LIR_lshi:
                case LIR_rshi:
                case LIR_rshui:
                    return oprnd1;

                case LIR_andi:
                case LIR_muli:
                case LIR_ltui: // unsigned < 0 -> always false
                    return oprnd2;

                case LIR_geui: // unsigned >= 0 -> always true
                    return insImmI(1);

                case LIR_eqi:
                    if (oprnd1->isop(LIR_ori) &&
                        oprnd1->oprnd2()->isImmI() &&
                        oprnd1->oprnd2()->immI() != 0)
                    {
                        // (x or c) != 0 if c != 0
                        return insImmI(0);
                    }

                default:
                    break;
                }

            } else if (c == -1) {
                switch (v) {
                case LIR_ori:  return oprnd2;       // x | -1 = -1
                case LIR_andi: return oprnd1;       // x & -1 = x
                case LIR_gtui: return insImmI(0);   // u32 > 0xffffffff -> always false
                case LIR_leui: return insImmI(1);   // u32 <= 0xffffffff -> always true
                default:       break;
                }

            } else if (c == 1) {
                if (oprnd1->isCmp()) {
                    switch (v) {
                    case LIR_ori:   return oprnd2;      // cmp | 1 = 1   (and oprnd2 == 1)
                    case LIR_andi:  return oprnd1;      // cmp & 1 = cmp
                    case LIR_gtui:  return insImmI(0);  // (0|1) > 1 -> always false
                    default:        break;
                    }
                } else if (v == LIR_muli) {
                    return oprnd1;          // x * 1 = x
                }
            }
        }

#if NJ_SOFTFLOAT_SUPPORTED
        LIns* ins;
        if (v == LIR_ii2d && oprnd1->isop(LIR_dlo2i) && oprnd2->isop(LIR_dhi2i) &&
            (ins = oprnd1->oprnd1()) == oprnd2->oprnd1()) {
            // qjoin(qlo(x),qhi(x)) == x
            return ins;
        }
#endif

        return out->ins2(v, oprnd1, oprnd2);
    }

    LIns* ExprFilter::ins3(LOpcode v, LIns* oprnd1, LIns* oprnd2, LIns* oprnd3)
    {
        NanoAssert(oprnd1 && oprnd2 && oprnd3);
        NanoAssert(isCmovOpcode(v));
        if (oprnd2 == oprnd3) {
            // c ? a : a => a
            return oprnd2;
        }
        if (oprnd1->isImmI()) {
            // const ? x : y => return x or y depending on const
            return oprnd1->immI() ? oprnd2 : oprnd3;
        }
        if (oprnd1->isop(LIR_eqi) &&
            ((oprnd1->oprnd2() == oprnd2 && oprnd1->oprnd1() == oprnd3) ||
             (oprnd1->oprnd1() == oprnd2 && oprnd1->oprnd2() == oprnd3))) {
            // (y == x) ? x : y  =>  y
            // (x == y) ? x : y  =>  y
            return oprnd3;
        }

        return out->ins3(v, oprnd1, oprnd2, oprnd3);
    }

    LIns* ExprFilter::insGuard(LOpcode v, LIns* c, GuardRecord *gr)
    {
        if (v == LIR_xt || v == LIR_xf) {
            if (c->isImmI()) {
                if ((v == LIR_xt && !c->immI()) || (v == LIR_xf && c->immI())) {
                    return 0; // no guard needed
                } else {
#ifdef JS_TRACER
                    // We're emitting a guard that will always fail. Any code
                    // emitted after this guard is dead code.  But it won't be
                    // optimized away, and it could indicate a performance
                    // problem or other bug, so assert in debug builds.
                    NanoAssertMsg(0, "Constantly false guard detected");
#endif
                    return out->insGuard(LIR_x, NULL, gr);
                }
            } else {
                while (c->isop(LIR_eqi) && c->oprnd1()->isCmp() && c->oprnd2()->isImmI(0)) {
                    // xt(eq(cmp,0)) => xf(cmp)   or   xf(eq(cmp,0)) => xt(cmp)
                    v = invertCondGuardOpcode(v);
                    c = c->oprnd1();
                }
            }
        }
        return out->insGuard(v, c, gr);
    }

    // Simplify operator if possible.  Always return NULL if overflow is possible.

    LIns* ExprFilter::simplifyOverflowArith(LOpcode op, LIns** opnd1, LIns** opnd2)
    {
        LIns* oprnd1 = *opnd1;
        LIns* oprnd2 = *opnd2;

        if (oprnd1->isImmI() && oprnd2->isImmI()) {
            int32_t c1 = oprnd1->immI();
            int32_t c2 = oprnd2->immI();
            double d = 0.0;

            // The code below attempts to perform the operation while
            // detecting overflow.  For multiplication, we may unnecessarily
            // infer a possible overflow due to the insufficient integer
            // range of the double type.

            switch (op) {
            case LIR_addjovi:
            case LIR_addxovi:    d = double(c1) + double(c2);    break;
            case LIR_subjovi:
            case LIR_subxovi:    d = double(c1) - double(c2);    break;
            case LIR_muljovi:
            case LIR_mulxovi:    d = double(c1) * double(c2);    break;
            default:             NanoAssert(0);                  break;
            }
            int32_t r = int32_t(d);
            if (r == d)
                return insImmI(r);

        } else if (oprnd1->isImmI() && !oprnd2->isImmI()) {
            switch (op) {
            case LIR_addjovi:
            case LIR_addxovi:
            case LIR_muljovi:
            case LIR_mulxovi: {
                // swap operands, moving const to rhs
                LIns* t = oprnd2;
                oprnd2 = oprnd1;
                oprnd1 = t;
                // swap actual arguments in caller as well
                *opnd1 = oprnd1;
                *opnd2 = oprnd2;
                break;
            }
            case LIR_subjovi:
            case LIR_subxovi:
                break;
            default:
                NanoAssert(0);
            }
        }

        if (oprnd2->isImmI()) {
            int c = oprnd2->immI();
            if (c == 0) {
                switch (op) {
                case LIR_addjovi:
                case LIR_addxovi:
                case LIR_subjovi:
                case LIR_subxovi:
                    return oprnd1;
                case LIR_muljovi:
                case LIR_mulxovi:
                    return oprnd2;
                default:
                    ;
                }
            } else if (c == 1 && (op == LIR_muljovi || op == LIR_mulxovi)) {
                return oprnd1;
            }
        }

        return NULL;
    }

    LIns* ExprFilter::insGuardXov(LOpcode op, LIns* oprnd1, LIns* oprnd2, GuardRecord *gr)
    {
        LIns* simplified = simplifyOverflowArith(op, &oprnd1, &oprnd2);
        if (simplified)
            return simplified;

        return out->insGuardXov(op, oprnd1, oprnd2, gr);
    }

    LIns* ExprFilter::insBranch(LOpcode v, LIns *c, LIns *t)
    {
        if (v == LIR_jt || v == LIR_jf) {
            if (c->isImmI()) {
                if ((v == LIR_jt && !c->immI()) || (v == LIR_jf && c->immI())) {
                    return 0; // no jump needed
                } else {
#ifdef JS_TRACER
                    // We're emitting a guard that will always fail. Any code
                    // between here and the target is dead (if it's a forward
                    // jump).  But it won't be optimized away, and it could
                    // indicate a performance problem or other bug, so assert
                    // in debug builds.
                    NanoAssertMsg(0, "Constantly false branch detected");
#endif
                    return out->insBranch(LIR_j, NULL, t);
                }
            } else {
                while (c->isop(LIR_eqi) && c->oprnd1()->isCmp() && c->oprnd2()->isImmI(0)) {
                    // jt(eq(cmp,0)) => jf(cmp)   or   jf(eq(cmp,0)) => jt(cmp)
                    v = invertCondJmpOpcode(v);
                    c = c->oprnd1();
                }
            }
        }
        return out->insBranch(v, c, t);
    }

    LIns* ExprFilter::insBranchJov(LOpcode op, LIns* oprnd1, LIns* oprnd2, LIns* target)
    {
        LIns* simplified = simplifyOverflowArith(op, &oprnd1, &oprnd2);
        if (simplified)
            return simplified;

        return out->insBranchJov(op, oprnd1, oprnd2, target);
    }

    LIns* ExprFilter::insLoad(LOpcode op, LIns* base, int32_t off, AccSet accSet) {
        if (base->isImmP() && !isS8(off)) {
            // if the effective address is constant, then transform:
            // ld const[bigconst] => ld (const+bigconst)[0]
            // note: we don't do this optimization for <8bit field offsets,
            // under the assumption that we're more likely to CSE-match the
            // constant base address if we dont const-fold small offsets.
            uintptr_t p = (uintptr_t)base->immP() + off;
            return out->insLoad(op, insImmP((void*)p), 0, accSet);
        }
        return out->insLoad(op, base, off, accSet);
    }

    LIns* LirWriter::insStore(LIns* value, LIns* base, int32_t d, AccSet accSet)
    {
        // Determine which kind of store should be used for 'value' based on
        // its type.
        LOpcode op = LOpcode(0);
        switch (value->retType()) {
        case LTy_I: op = LIR_sti;   break;
#ifdef NANOJIT_64BIT
        case LTy_Q: op = LIR_stq;   break;
#endif
        case LTy_D: op = LIR_std;   break;
        case LTy_V: NanoAssert(0);  break;
        default:    NanoAssert(0);  break;
        }
        return insStore(op, value, base, d, accSet);
    }

    LIns* LirWriter::insChoose(LIns* cond, LIns* iftrue, LIns* iffalse, bool use_cmov)
    {
        // 'cond' must be a conditional, unless it has been optimized to 0 or
        // 1.  In that case make it an ==0 test and flip the branches.  It'll
        // get constant-folded by ExprFilter subsequently.
        if (!cond->isCmp()) {
            NanoAssert(cond->isImmI());
            cond = insEqI_0(cond);
            LIns* tmp = iftrue;
            iftrue = iffalse;
            iffalse = tmp;
        }

        if (use_cmov) {
            LOpcode op = LIR_cmovi;
            if (iftrue->isI() && iffalse->isI()) {
                op = LIR_cmovi;
#ifdef NANOJIT_64BIT
            } else if (iftrue->isQ() && iffalse->isQ()) {
                op = LIR_cmovq;
#endif
            } else if (iftrue->isD() && iffalse->isD()) {
                NanoAssertMsg(0, "LIR_fcmov doesn't exist yet, sorry");
            } else {
                NanoAssert(0);  // type error
            }
            return ins3(op, cond, iftrue, iffalse);
        }

        LIns* ncond = ins1(LIR_negi, cond); // cond ? -1 : 0
        return ins2(LIR_ori,
                    ins2(LIR_andi, iftrue, ncond),
                    ins2(LIR_andi, iffalse, ins1(LIR_noti, ncond)));
    }

    LIns* LirBufWriter::insCall(const CallInfo *ci, LIns* args[])
    {
        LOpcode op = getCallOpcode(ci);
#if NJ_SOFTFLOAT_SUPPORTED
        // SoftFloat: convert LIR_calld to LIR_calli.
        if (_config.soft_float && op == LIR_calld)
            op = LIR_calli;
#endif

        int32_t argc = ci->count_args();
        NanoAssert(argc <= (int)MAXARGS);

        // Allocate space for and copy the arguments.  We use the same
        // allocator as the normal LIR buffers so it has the same lifetime.
        // Nb: this must be kept in sync with arg().
        LIns** args2 = (LIns**)_buf->_allocator.alloc(argc * sizeof(LIns*));
        memcpy(args2, args, argc * sizeof(LIns*));

        // Allocate and write the call instruction.
        LInsC* insC = (LInsC*)_buf->makeRoom(sizeof(LInsC));
        LIns*  ins  = insC->getLIns();
        ins->initLInsC(op, args2, ci);
        return ins;
    }

    using namespace avmplus;

    StackFilter::StackFilter(LirFilter *in, Allocator& alloc, LIns* sp)
        : LirFilter(in), sp(sp), stk(alloc), top(0)
    {}

    // If we see a sequence like this:
    //
    //   sti sp[0]
    //   ...
    //   sti sp[0]
    //
    // where '...' contains no guards, we can remove the first store.  Also,
    // because stack entries are eight bytes each (we check this), if we have
    // this:
    //
    //   stfi sp[0]
    //   ...
    //   sti sp[0]
    //
    // we can again remove the first store -- even though the second store
    // doesn't clobber the high four bytes -- because we know the entire value
    // stored by the first store is dead.
    //
    LIns* StackFilter::read()
    {
        for (;;) {
            LIns* ins = in->read();

            if (ins->isStore()) {
                LIns* base = ins->oprnd2();
                if (base == sp) {
                    // 'disp' must be eight-aligned because each stack entry is 8 bytes.
                    NanoAssert((ins->disp() & 0x7) == 0);

                    int d = ins->disp() >> 3;
                    if (d >= top) {
                        continue;
                    } else {
                        d = top - d;
                        if (stk.get(d)) {
                            continue;
                        } else {
                            stk.set(d);
                        }
                    }
                }
            }
            /*
             * NB: If there is a backward branch other than the loop-restart branch, this is
             * going to be wrong. Unfortunately there doesn't seem to be an easy way to detect
             * such branches. Just do not create any.
             *
             * The isLive() call is valid because liveness will have been
             * computed by Assembler::gen() for every instruction following
             * this guard.
             */
            else if (ins->isGuard() && ins->isLive()) {
                stk.reset();
                top = getTop(ins);
                top >>= 3;
            }

            return ins;
        }
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

        void add(LIns* ins, LIns* use) {
            if (!ins->isImmAny() && !live.containsKey(ins)) {
                NanoAssert(size_t(ins->opcode()) < sizeof(lirNames) / sizeof(lirNames[0]));
                live.put(ins,use);
            }
        }

        void retire(LIns* i) {
            RetiredEntry *e = new (alloc) RetiredEntry();
            e->i = i;
            SeqBuilder<LIns*> livelist(alloc);
            HashMap<LIns*, LIns*>::Iter iter(live);
            int live_count = 0;
            while (iter.next()) {
                LIns* ins = iter.key();
                if (!ins->isV()) {
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

        bool contains(LIns* i) {
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
    void live(LirFilter* in, Allocator& alloc, Fragment *frag, LogControl *logc)
    {
        // traverse backwards to find live exprs and a few other stats.

        LiveTable live(alloc);
        uint32_t exits = 0;
        int total = 0;
        if (frag->lirbuf->state)
            live.add(frag->lirbuf->state, 0);
        for (LIns* ins = in->read(); !ins->isop(LIR_start); ins = in->read())
        {
            total++;

            // First handle instructions that are always live (ie. those that
            // don't require being marked as live), eg. those with
            // side-effects.  We ignore LIR_paramp.
            if (ins->isLive() && !ins->isop(LIR_paramp))
            {
                live.add(ins, 0);
                if (ins->isGuard())
                    exits++;
            }

            // now propagate liveness
            if (live.contains(ins))
            {
                live.retire(ins);

                switch (ins->opcode()) {
                case LIR_skip:
                    NanoAssertMsg(0, "Shouldn't see LIR_skip");
                    break;

                case LIR_start:
                case LIR_regfence:
                case LIR_paramp:
                case LIR_x:
                case LIR_xbarrier:
                case LIR_j:
                case LIR_label:
                case LIR_immi:
                CASE64(LIR_immq:)
                case LIR_immd:
                case LIR_allocp:
                    // No operands, do nothing.
                    break;

                case LIR_ldi:
                CASE64(LIR_ldq:)
                case LIR_ldd:
                case LIR_lduc2ui:
                case LIR_ldus2ui:
                case LIR_ldc2i:
                case LIR_lds2i:
                case LIR_ldf2d:
                case LIR_reti:
                CASE64(LIR_retq:)
                case LIR_retd:
                case LIR_livei:
                CASE64(LIR_liveq:)
                case LIR_lived:
                case LIR_xt:
                case LIR_xf:
                case LIR_xtbl:
                case LIR_jt:
                case LIR_jf:
                case LIR_jtbl:
                case LIR_negi:
                case LIR_negd:
                case LIR_noti:
                CASESF(LIR_dlo2i:)
                CASESF(LIR_dhi2i:)
                CASESF(LIR_hcalli:)
                CASE64(LIR_i2q:)
                CASE64(LIR_ui2uq:)
                case LIR_i2d:
                case LIR_ui2d:
                CASE64(LIR_q2i:)
                case LIR_d2i:
                CASE86(LIR_modi:)
                    live.add(ins->oprnd1(), 0);
                    break;

                case LIR_sti:
                CASE64(LIR_stq:)
                case LIR_std:
                case LIR_sti2c:
                case LIR_sti2s:
                case LIR_std2f:
                case LIR_eqi:
                case LIR_lti:
                case LIR_gti:
                case LIR_lei:
                case LIR_gei:
                case LIR_ltui:
                case LIR_gtui:
                case LIR_leui:
                case LIR_geui:
                case LIR_eqd:
                case LIR_ltd:
                case LIR_gtd:
                case LIR_led:
                case LIR_ged:
                CASE64(LIR_eqq:)
                CASE64(LIR_ltq:)
                CASE64(LIR_gtq:)
                CASE64(LIR_leq:)
                CASE64(LIR_geq:)
                CASE64(LIR_ltuq:)
                CASE64(LIR_gtuq:)
                CASE64(LIR_leuq:)
                CASE64(LIR_geuq:)
                case LIR_lshi:
                case LIR_rshi:
                case LIR_rshui:
                CASE64(LIR_lshq:)
                CASE64(LIR_rshq:)
                CASE64(LIR_rshuq:)
                case LIR_addi:
                case LIR_subi:
                case LIR_muli:
                case LIR_addxovi:
                case LIR_subxovi:
                case LIR_mulxovi:
                case LIR_addjovi:
                case LIR_subjovi:
                case LIR_muljovi:
                CASE86(LIR_divi:)
                case LIR_addd:
                case LIR_subd:
                case LIR_muld:
                case LIR_divd:
                CASE64(LIR_addq:)
                CASE64(LIR_subq:)
                CASE64(LIR_addjovq:)
                CASE64(LIR_subjovq:)
                case LIR_andi:
                case LIR_ori:
                case LIR_xori:
                CASE64(LIR_andq:)
                CASE64(LIR_orq:)
                CASE64(LIR_xorq:)
                CASESF(LIR_ii2d:)
                case LIR_file:
                case LIR_line:
                    live.add(ins->oprnd1(), 0);
                    live.add(ins->oprnd2(), 0);
                    break;

                case LIR_cmovi:
                CASE64(LIR_cmovq:)
                    live.add(ins->oprnd1(), 0);
                    live.add(ins->oprnd2(), 0);
                    live.add(ins->oprnd3(), 0);
                    break;

                case LIR_calli:
                case LIR_calld:
                CASE64(LIR_callq:)
                    for (int i = 0, argc = ins->argc(); i < argc; i++)
                        live.add(ins->arg(i), 0);
                    break;

                default:
                    NanoAssertMsgf(0, "unhandled opcode: %d", ins->opcode());
                    break;
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
        LInsPrinter *printer = frag->lirbuf->printer;
        bool newblock = true;
        for (Seq<RetiredEntry*>* p = live.retired.get(); p != NULL; p = p->tail) {
            RetiredEntry* e = p->head;
            InsBuf ib;
            RefBuf rb;
            char livebuf[4000], *s=livebuf;
            *s = 0;
            if (!newblock && e->i->isop(LIR_label)) {
                logc->printf("\n");
            }
            newblock = false;
            for (Seq<LIns*>* p = e->live; p != NULL; p = p->tail) {
                VMPI_strcpy(s, printer->formatRef(&rb, p->head));
                s += VMPI_strlen(s);
                *s++ = ' '; *s = 0;
                NanoAssert(s < livebuf+sizeof(livebuf));
            }
            /* If the LIR insn is pretty short, print it and its
               live-after set on the same line.  If not, put
               live-after set on a new line, suitably indented. */
            const char* insn_text = printer->formatIns(&ib, e->i);
            if (VMPI_strlen(insn_text) >= 30-2) {
                logc->printf("  %-30s\n  %-30s %s\n", insn_text, "", livebuf);
            } else {
                logc->printf("  %-30s %s\n", insn_text, livebuf);
            }

            if (e->i->isGuard() || e->i->isBranch() || e->i->isRet()) {
                logc->printf("\n");
                newblock = true;
            }
        }
    }

    void LirNameMap::addNameWithSuffix(LIns* ins, const char *name, int suffix,
                                       bool ignoreOneSuffix) {
        // The lookup may succeed, ie. we may already have a name for this
        // instruction.  This can happen because of CSE.  Eg. if we have this:
        //
        //   ins = addName("foo", insImmI(0))
        //
        // that assigns the name "foo1" to 'ins'.  If we later do this:
        //
        //   ins2 = addName("foo", insImmI(0))
        //
        // then CSE will cause 'ins' and 'ins2' to be equal.  So 'ins2'
        // already has a name ("foo1") and there's no need to generate a new
        // name "foo2".
        //
        if (!names.containsKey(ins)) {
            const int N = 100;
            char name2[N];
            if (suffix == 1 && ignoreOneSuffix) {
                VMPI_snprintf(name2, N, "%s", name);                // don't add '1' suffix
            } else if (VMPI_isdigit(name[VMPI_strlen(name)-1])) {
                VMPI_snprintf(name2, N, "%s_%d", name, suffix);     // use '_' to avoid confusion
            } else {
                VMPI_snprintf(name2, N, "%s%d", name, suffix);      // normal case
            }

            char *copy = new (alloc) char[VMPI_strlen(name2)+1];
            VMPI_strcpy(copy, name2);
            Entry *e = new (alloc) Entry(copy);
            names.put(ins, e);
        }
    }

    void LirNameMap::addName(LIns* ins, const char* name) {
        addNameWithSuffix(ins, name, namecounts.add(name), /*ignoreOneSuffix*/true);
    }

    const char* LirNameMap::createName(LIns* ins) {
        if (ins->isCall()) {
#if NJ_SOFTFLOAT_SUPPORTED
            if (ins->isop(LIR_hcalli)) {
                ins = ins->oprnd1();    // we've presumably seen the other half already
            } else
#endif
            {
                addNameWithSuffix(ins, ins->callInfo()->_name, funccounts.add(ins->callInfo()),
                                  /*ignoreOneSuffix*/false);
            }
        } else {
            addNameWithSuffix(ins, lirNames[ins->opcode()], lircounts.add(ins->opcode()),
                              /*ignoreOneSuffix*/false);

        }
        return names.get(ins)->name;
    }

    const char* LirNameMap::lookupName(LIns* ins)
    {
        Entry* e = names.get(ins);
        return e ? e->name : NULL;
    }


    char* LInsPrinter::formatAccSet(RefBuf* buf, AccSet accSet) {
        int i = 0;
        // 'c' is short for "const", because 'r' is used for RSTACK.
        if (accSet & ACC_READONLY) { buf->buf[i++] = 'c'; accSet &= ~ACC_READONLY; }
        if (accSet & ACC_STACK)    { buf->buf[i++] = 's'; accSet &= ~ACC_STACK; }
        if (accSet & ACC_RSTACK)   { buf->buf[i++] = 'r'; accSet &= ~ACC_RSTACK; }
        if (accSet & ACC_OTHER)    { buf->buf[i++] = 'o'; accSet &= ~ACC_OTHER; }
        // This assertion will fail if we add a new accSet value but
        // forget to handle it here.
        NanoAssert(accSet == 0);
        buf->buf[i] = 0;
        NanoAssert(size_t(i) < buf->len);
        return buf->buf;
    }

    char* LInsPrinter::formatImmI(RefBuf* buf, int32_t c) {
        if (-10000 < c || c < 10000) {
            VMPI_snprintf(buf->buf, buf->len, "%d", c);
        } else {
#if !defined NANOJIT_64BIT
            formatAddr(buf, (void*)c);
#else
            VMPI_snprintf(buf->buf, buf->len, "0x%x", (unsigned int)c);
#endif
        }
        return buf->buf;
    }

    char* LInsPrinter::formatImmQ(RefBuf* buf, uint64_t c) {
        if (-10000 < (int64_t)c || c < 10000) {
            VMPI_snprintf(buf->buf, buf->len, "%dLL", (int)c);
        } else {
#if defined NANOJIT_64BIT
            formatAddr(buf, (void*)c);
#else
            VMPI_snprintf(buf->buf, buf->len, "0x%llxLL", c);
#endif
        }
        return buf->buf;
    }

    char* LInsPrinter::formatImmD(RefBuf* buf, double c) {
        VMPI_snprintf(buf->buf, buf->len, "%g", c);
        return buf->buf;
    }

    char* LInsPrinter::formatAddr(RefBuf* buf, void* p)
    {
        char*   name;
        int32_t offset;
        addrNameMap->lookupAddr(p, name, offset);

        if (name) {
            if (offset != 0) {
                VMPI_snprintf(buf->buf, buf->len, "%p %s+%d", p, name, offset);
            } else {
                VMPI_snprintf(buf->buf, buf->len, "%p %s", p, name);
            }
        } else {
            VMPI_snprintf(buf->buf, buf->len, "%p", p);
        }

        return buf->buf;
    }

    char* LInsPrinter::formatRef(RefBuf* buf, LIns *ref, bool showImmValue)
    {
        // Give 'ref' a name if it doesn't have one.
        const char* name = lirNameMap->lookupName(ref);
        if (!name) {
            name = lirNameMap->createName(ref);
        }

        // Put it in the buffer.  If it's an immediate, show the value if
        // showImmValue==true.  (This facility allows us to print immediate
        // values when they're used but not when they're def'd, ie. we don't
        // want "immi1/*1*/ = immi 1".)
        RefBuf buf2;
        if (ref->isImmI() && showImmValue) {
            VMPI_snprintf(buf->buf, buf->len, "%s/*%s*/", name, formatImmI(&buf2, ref->immI()));
        }
#ifdef NANOJIT_64BIT
        else if (ref->isImmQ() && showImmValue) {
            VMPI_snprintf(buf->buf, buf->len, "%s/*%s*/", name, formatImmQ(&buf2, ref->immQ()));
        }
#endif
        else if (ref->isImmD() && showImmValue) {
            VMPI_snprintf(buf->buf, buf->len, "%s/*%s*/", name, formatImmD(&buf2, ref->immD()));
        }
        else {
            VMPI_snprintf(buf->buf, buf->len, "%s", name);
        }

        return buf->buf;
    }

    char* LInsPrinter::formatIns(InsBuf* buf, LIns* i)
    {
        char *s = buf->buf;
        size_t n = buf->len;
        RefBuf b1, b2, b3, b4;
        LOpcode op = i->opcode();
        switch (op)
        {
            case LIR_immi:
                VMPI_snprintf(s, n, "%s = %s %s", formatRef(&b1, i, /*showImmValue*/false),
                              lirNames[op], formatImmI(&b2, i->immI()));
                break;

#ifdef NANOJIT_64BIT
            case LIR_immq:
                VMPI_snprintf(s, n, "%s = %s %s", formatRef(&b1, i, /*showImmValue*/false),
                              lirNames[op], formatImmQ(&b2, i->immQ()));
                break;
#endif

            case LIR_immd:
                VMPI_snprintf(s, n, "%s = %s %s", formatRef(&b1, i, /*showImmValue*/false),
                              lirNames[op], formatImmD(&b2, i->immD()));
                break;

            case LIR_allocp:
                VMPI_snprintf(s, n, "%s = %s %d", formatRef(&b1, i), lirNames[op], i->size());
                break;

            case LIR_start:
            case LIR_regfence:
                VMPI_snprintf(s, n, "%s", lirNames[op]);
                break;

            case LIR_calli:
            case LIR_calld:
            CASE64(LIR_callq:) {
                const CallInfo* call = i->callInfo();
                int32_t argc = i->argc();
                int32_t m = int32_t(n);     // Windows doesn't have 'ssize_t'
                if (call->isIndirect())
                    m -= VMPI_snprintf(s, m, "%s = %s.%s [%s] ( ", formatRef(&b1, i), lirNames[op],
                                       formatAccSet(&b2, call->_storeAccSet),
                                       formatRef(&b3, i->arg(--argc)));
                else
                    m -= VMPI_snprintf(s, m, "%s = %s.%s #%s ( ", formatRef(&b1, i), lirNames[op],
                                       formatAccSet(&b2, call->_storeAccSet), call->_name);
                if (m < 0) break;
                for (int32_t j = argc - 1; j >= 0; j--) {
                    s += VMPI_strlen(s);
                    m -= VMPI_snprintf(s, m, "%s ",formatRef(&b2, i->arg(j)));
                    if (m < 0) break;
                }
                s += VMPI_strlen(s);
                m -= VMPI_snprintf(s, m, ")");
                break;
            }

            case LIR_jtbl: {
                int32_t m = int32_t(n);     // Windows doesn't have 'ssize_t'
                m -= VMPI_snprintf(s, m, "%s %s [ ", lirNames[op], formatRef(&b1, i->oprnd1()));
                if (m < 0) break;
                for (uint32_t j = 0, sz = i->getTableSize(); j < sz; j++) {
                    LIns* target = i->getTarget(j);
                    s += VMPI_strlen(s);
                    m -= VMPI_snprintf(s, m, "%s ", target ? formatRef(&b2, target) : "unpatched");
                    if (m < 0) break;
                }
                s += VMPI_strlen(s);
                m -= VMPI_snprintf(s, m, "]");
                break;
            }

            case LIR_paramp: {
                uint32_t arg = i->paramArg();
                if (!i->paramKind()) {
                    if (arg < sizeof(Assembler::argRegs)/sizeof(Assembler::argRegs[0])) {
                        VMPI_snprintf(s, n, "%s = %s %d %s", formatRef(&b1, i), lirNames[op],
                            arg, gpn(Assembler::argRegs[arg]));
                    } else {
                        VMPI_snprintf(s, n, "%s = %s %d", formatRef(&b1, i), lirNames[op], arg);
                    }
                } else {
                    VMPI_snprintf(s, n, "%s = %s %d %s", formatRef(&b1, i), lirNames[op],
                        arg, gpn(Assembler::savedRegs[arg]));
                }
                break;
            }

            case LIR_label:
                VMPI_snprintf(s, n, "%s:", formatRef(&b1, i));
                break;

            case LIR_jt:
            case LIR_jf:
                VMPI_snprintf(s, n, "%s %s -> %s", lirNames[op], formatRef(&b1, i->oprnd1()),
                    i->oprnd2() ? formatRef(&b2, i->oprnd2()) : "unpatched");
                break;

            case LIR_j:
                VMPI_snprintf(s, n, "%s -> %s", lirNames[op],
                    i->oprnd2() ? formatRef(&b1, i->oprnd2()) : "unpatched");
                break;

            case LIR_livei:
            case LIR_lived:
            CASE64(LIR_liveq:)
            case LIR_reti:
            CASE64(LIR_retq:)
            case LIR_retd:
                VMPI_snprintf(s, n, "%s %s", lirNames[op], formatRef(&b1, i->oprnd1()));
                break;

            CASESF(LIR_hcalli:)
            case LIR_negi:
            case LIR_negd:
            case LIR_i2d:
            case LIR_ui2d:
            CASESF(LIR_dlo2i:)
            CASESF(LIR_dhi2i:)
            case LIR_noti:
            CASE86(LIR_modi:)
            CASE64(LIR_i2q:)
            CASE64(LIR_ui2uq:)
            CASE64(LIR_q2i:)
            case LIR_d2i:
                VMPI_snprintf(s, n, "%s = %s %s", formatRef(&b1, i), lirNames[op],
                             formatRef(&b2, i->oprnd1()));
                break;

            case LIR_x:
            case LIR_xt:
            case LIR_xf:
            case LIR_xbarrier:
            case LIR_xtbl:
                formatGuard(buf, i);
                break;

            case LIR_addxovi:
            case LIR_subxovi:
            case LIR_mulxovi:
                formatGuardXov(buf, i);
                break;

            case LIR_addjovi:
            case LIR_subjovi:
            case LIR_muljovi:
            CASE64(LIR_addjovq:)
            CASE64(LIR_subjovq:)
                VMPI_snprintf(s, n, "%s = %s %s, %s ; ovf -> %s", formatRef(&b1, i), lirNames[op],
                    formatRef(&b2, i->oprnd1()),
                    formatRef(&b3, i->oprnd2()),
                    i->oprnd3() ? formatRef(&b4, i->oprnd3()) : "unpatched");
                break;

            case LIR_addi:       CASE64(LIR_addq:)
            case LIR_subi:       CASE64(LIR_subq:)
            case LIR_muli:
            CASE86(LIR_divi:)
            case LIR_addd:
            case LIR_subd:
            case LIR_muld:
            case LIR_divd:
            case LIR_andi:       CASE64(LIR_andq:)
            case LIR_ori:        CASE64(LIR_orq:)
            case LIR_xori:       CASE64(LIR_xorq:)
            case LIR_lshi:       CASE64(LIR_lshq:)
            case LIR_rshi:       CASE64(LIR_rshq:)
            case LIR_rshui:      CASE64(LIR_rshuq:)
            case LIR_eqi:        CASE64(LIR_eqq:)
            case LIR_lti:        CASE64(LIR_ltq:)
            case LIR_lei:        CASE64(LIR_leq:)
            case LIR_gti:        CASE64(LIR_gtq:)
            case LIR_gei:        CASE64(LIR_geq:)
            case LIR_ltui:       CASE64(LIR_ltuq:)
            case LIR_leui:       CASE64(LIR_leuq:)
            case LIR_gtui:       CASE64(LIR_gtuq:)
            case LIR_geui:       CASE64(LIR_geuq:)
            case LIR_eqd:
            case LIR_ltd:
            case LIR_led:
            case LIR_gtd:
            case LIR_ged:
#if NJ_SOFTFLOAT_SUPPORTED
            case LIR_ii2d:
#endif
                VMPI_snprintf(s, n, "%s = %s %s, %s", formatRef(&b1, i), lirNames[op],
                    formatRef(&b2, i->oprnd1()),
                    formatRef(&b3, i->oprnd2()));
                break;

            CASE64(LIR_cmovq:)
            case LIR_cmovi:
                VMPI_snprintf(s, n, "%s = %s %s ? %s : %s", formatRef(&b1, i), lirNames[op],
                    formatRef(&b2, i->oprnd1()),
                    formatRef(&b3, i->oprnd2()),
                    formatRef(&b4, i->oprnd3()));
                break;

            case LIR_ldi:
            CASE64(LIR_ldq:)
            case LIR_ldd:
            case LIR_lduc2ui:
            case LIR_ldus2ui:
            case LIR_ldc2i:
            case LIR_lds2i:
            case LIR_ldf2d:
                VMPI_snprintf(s, n, "%s = %s.%s %s[%d]", formatRef(&b1, i), lirNames[op],
                    formatAccSet(&b2, i->accSet()),
                    formatRef(&b3, i->oprnd1()),
                    i->disp());
                break;

            case LIR_sti:
            CASE64(LIR_stq:)
            case LIR_std:
            case LIR_sti2c:
            case LIR_sti2s:
            case LIR_std2f:
                VMPI_snprintf(s, n, "%s.%s %s[%d] = %s", lirNames[op],
                    formatAccSet(&b1, i->accSet()),
                    formatRef(&b2, i->oprnd2()),
                    i->disp(),
                    formatRef(&b3, i->oprnd1()));
                break;

            default:
                NanoAssertMsgf(0, "Can't handle opcode %s\n", lirNames[op]);
                break;
        }
        return buf->buf;
    }
#endif


    CseFilter::CseFilter(LirWriter *out, Allocator& alloc)
        : LirWriter(out), storesSinceLastLoad(ACC_NONE), alloc(alloc)
    {
        m_find[LInsImmI]         = &CseFilter::findImmI;
        m_find[LInsImmQ]         = PTR_SIZE(NULL, &CseFilter::findImmQ);
        m_find[LInsImmD]         = &CseFilter::findImmD;
        m_find[LIns1]            = &CseFilter::find1;
        m_find[LIns2]            = &CseFilter::find2;
        m_find[LIns3]            = &CseFilter::find3;
        m_find[LInsCall]         = &CseFilter::findCall;
        m_find[LInsLoadReadOnly] = &CseFilter::findLoadReadOnly;
        m_find[LInsLoadStack]    = &CseFilter::findLoadStack;
        m_find[LInsLoadRStack]   = &CseFilter::findLoadRStack;
        m_find[LInsLoadOther]    = &CseFilter::findLoadOther;
        m_find[LInsLoadMultiple] = &CseFilter::findLoadMultiple;

        m_cap[LInsImmI]         = 128;
        m_cap[LInsImmQ]         = PTR_SIZE(0, 16);
        m_cap[LInsImmD]         = 16;
        m_cap[LIns1]            = 256;
        m_cap[LIns2]            = 512;
        m_cap[LIns3]            = 16;
        m_cap[LInsCall]         = 64;
        m_cap[LInsLoadReadOnly] = 16;
        m_cap[LInsLoadStack]    = 16;
        m_cap[LInsLoadRStack]   = 16;
        m_cap[LInsLoadOther]    = 16;
        m_cap[LInsLoadMultiple] = 16;

        for (LInsHashKind kind = LInsFirst; kind <= LInsLast; kind = nextKind(kind)) {
            m_list[kind] = new (alloc) LIns*[m_cap[kind]];
        }
        clear();
    }

    // Inlined/separated version of SuperFastHash.
    // This content is copyrighted by Paul Hsieh.
    // For reference see: http://www.azillionmonkeys.com/qed/hash.html
    //
    inline uint32_t CseFilter::hash8(uint32_t hash, const uint8_t data)
    {
        hash += data;
        hash ^= hash << 10;
        hash += hash >> 1;
        return hash;
    }

    inline uint32_t CseFilter::hash32(uint32_t hash, const uint32_t data)
    {
        const uint32_t dlo = data & 0xffff;
        const uint32_t dhi = data >> 16;
        hash += dlo;
        const uint32_t tmp = (dhi << 11) ^ hash;
        hash = (hash << 16) ^ tmp;
        hash += hash >> 11;
        return hash;
    }

    inline uint32_t CseFilter::hashptr(uint32_t hash, const void* data)
    {
#ifdef NANOJIT_64BIT
        hash = hash32(hash, uint32_t(uintptr_t(data) >> 32));
        hash = hash32(hash, uint32_t(uintptr_t(data)));
        return hash;
#else
        return hash32(hash, uint32_t(data));
#endif
    }

    inline uint32_t CseFilter::hashfinish(uint32_t hash)
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

    void CseFilter::clear(LInsHashKind kind) {
        VMPI_memset(m_list[kind], 0, sizeof(LIns*)*m_cap[kind]);
        m_used[kind] = 0;
    }

    void CseFilter::clear() {
        for (LInsHashKind kind = LInsFirst; kind <= LInsLast; kind = nextKind(kind)) {
            clear(kind);
        }
    }

    inline uint32_t CseFilter::hashImmI(int32_t a) {
        return hashfinish(hash32(0, a));
    }

    inline uint32_t CseFilter::hashImmQorD(uint64_t a) {
        uint32_t hash = hash32(0, uint32_t(a >> 32));
        return hashfinish(hash32(hash, uint32_t(a)));
    }

    inline uint32_t CseFilter::hash1(LOpcode op, LIns* a) {
        uint32_t hash = hash8(0, uint8_t(op));
        return hashfinish(hashptr(hash, a));
    }

    inline uint32_t CseFilter::hash2(LOpcode op, LIns* a, LIns* b) {
        uint32_t hash = hash8(0, uint8_t(op));
        hash = hashptr(hash, a);
        return hashfinish(hashptr(hash, b));
    }

    inline uint32_t CseFilter::hash3(LOpcode op, LIns* a, LIns* b, LIns* c) {
        uint32_t hash = hash8(0, uint8_t(op));
        hash = hashptr(hash, a);
        hash = hashptr(hash, b);
        return hashfinish(hashptr(hash, c));
    }

    NanoStaticAssert(sizeof(AccSet) == 1);  // required for hashLoad to work properly

    // Nb: no need to hash the load's AccSet because each region's loads go in
    // a different hash table.
    inline uint32_t CseFilter::hashLoad(LOpcode op, LIns* a, int32_t d, AccSet accSet) {
        uint32_t hash = hash8(0,uint8_t(op));
        hash = hashptr(hash, a);
        hash = hash32(hash, d);
        return hashfinish(hash8(hash, accSet));
    }

    inline uint32_t CseFilter::hashCall(const CallInfo *ci, uint32_t argc, LIns* args[]) {
        uint32_t hash = hashptr(0, ci);
        for (int32_t j=argc-1; j >= 0; j--)
            hash = hashptr(hash,args[j]);
        return hashfinish(hash);
    }

    void CseFilter::grow(LInsHashKind kind)
    {
        const uint32_t oldcap = m_cap[kind];
        m_cap[kind] <<= 1;
        LIns** oldlist = m_list[kind];
        m_list[kind] = new (alloc) LIns*[m_cap[kind]];
        VMPI_memset(m_list[kind], 0, m_cap[kind] * sizeof(LIns*));
        find_t find = m_find[kind];
        for (uint32_t i = 0; i < oldcap; i++) {
            LIns* ins = oldlist[i];
            if (!ins) continue;
            uint32_t j = (this->*find)(ins);
            NanoAssert(!m_list[kind][j]);
            m_list[kind][j] = ins;
        }
    }

    void CseFilter::add(LInsHashKind kind, LIns* ins, uint32_t k)
    {
        NanoAssert(!m_list[kind][k]);
        m_used[kind]++;
        m_list[kind][k] = ins;
        if ((m_used[kind] * 4) >= (m_cap[kind] * 3)) {  // load factor of 0.75
            grow(kind);
        }
    }

    inline LIns* CseFilter::findImmI(int32_t a, uint32_t &k)
    {
        LInsHashKind kind = LInsImmI;
        const uint32_t bitmask = m_cap[kind] - 1;
        k = hashImmI(a) & bitmask;
        uint32_t n = 1;
        while (true) {
            LIns* ins = m_list[kind][k];
            if (!ins)
                return NULL;
            NanoAssert(ins->isImmI());
            if (ins->immI() == a)
                return ins;
            // Quadratic probe:  h(k,i) = h(k) + 0.5i + 0.5i^2, which gives the
            // sequence h(k), h(k)+1, h(k)+3, h(k)+6, h+10, ...  This is a
            // good sequence for 2^n-sized tables as the values h(k,i) for i
            // in [0,m − 1] are all distinct so termination is guaranteed.
            // See http://portal.acm.org/citation.cfm?id=360737 and
            // http://en.wikipedia.org/wiki/Quadratic_probing (fetched
            // 06-Nov-2009) for more details.
            k = (k + n) & bitmask;
            n += 1;
        }
    }

    uint32_t CseFilter::findImmI(LIns* ins)
    {
        uint32_t k;
        findImmI(ins->immI(), k);
        return k;
    }

#ifdef NANOJIT_64BIT
    inline LIns* CseFilter::findImmQ(uint64_t a, uint32_t &k)
    {
        LInsHashKind kind = LInsImmQ;
        const uint32_t bitmask = m_cap[kind] - 1;
        k = hashImmQorD(a) & bitmask;
        uint32_t n = 1;
        while (true) {
            LIns* ins = m_list[kind][k];
            if (!ins)
                return NULL;
            NanoAssert(ins->isImmQ());
            if (ins->immQ() == a)
                return ins;
            k = (k + n) & bitmask;
            n += 1;
        }
    }

    uint32_t CseFilter::findImmQ(LIns* ins)
    {
        uint32_t k;
        findImmQ(ins->immQ(), k);
        return k;
    }
#endif

    inline LIns* CseFilter::findImmD(uint64_t a, uint32_t &k)
    {
        LInsHashKind kind = LInsImmD;
        const uint32_t bitmask = m_cap[kind] - 1;
        k = hashImmQorD(a) & bitmask;
        uint32_t n = 1;
        while (true) {
            LIns* ins = m_list[kind][k];
            if (!ins)
                return NULL;
            NanoAssert(ins->isImmD());
            if (ins->immDasQ() == a)
                return ins;
            k = (k + n) & bitmask;
            n += 1;
        }
    }

    uint32_t CseFilter::findImmD(LIns* ins)
    {
        uint32_t k;
        findImmD(ins->immDasQ(), k);
        return k;
    }

    inline LIns* CseFilter::find1(LOpcode op, LIns* a, uint32_t &k)
    {
        LInsHashKind kind = LIns1;
        const uint32_t bitmask = m_cap[kind] - 1;
        k = hash1(op, a) & bitmask;
        uint32_t n = 1;
        while (true) {
            LIns* ins = m_list[kind][k];
            if (!ins)
                return NULL;
            if (ins->isop(op) && ins->oprnd1() == a)
                return ins;
            k = (k + n) & bitmask;
            n += 1;
        }
    }

    uint32_t CseFilter::find1(LIns* ins)
    {
        uint32_t k;
        find1(ins->opcode(), ins->oprnd1(), k);
        return k;
    }

    inline LIns* CseFilter::find2(LOpcode op, LIns* a, LIns* b, uint32_t &k)
    {
        LInsHashKind kind = LIns2;
        const uint32_t bitmask = m_cap[kind] - 1;
        k = hash2(op, a, b) & bitmask;
        uint32_t n = 1;
        while (true) {
            LIns* ins = m_list[kind][k];
            if (!ins)
                return NULL;
            if (ins->isop(op) && ins->oprnd1() == a && ins->oprnd2() == b)
                return ins;
            k = (k + n) & bitmask;
            n += 1;
        }
    }

    uint32_t CseFilter::find2(LIns* ins)
    {
        uint32_t k;
        find2(ins->opcode(), ins->oprnd1(), ins->oprnd2(), k);
        return k;
    }

    inline LIns* CseFilter::find3(LOpcode op, LIns* a, LIns* b, LIns* c, uint32_t &k)
    {
        LInsHashKind kind = LIns3;
        const uint32_t bitmask = m_cap[kind] - 1;
        k = hash3(op, a, b, c) & bitmask;
        uint32_t n = 1;
        while (true) {
            LIns* ins = m_list[kind][k];
            if (!ins)
                return NULL;
            if (ins->isop(op) && ins->oprnd1() == a && ins->oprnd2() == b && ins->oprnd3() == c)
                return ins;
            k = (k + n) & bitmask;
            n += 1;
        }
    }

    uint32_t CseFilter::find3(LIns* ins)
    {
        uint32_t k;
        find3(ins->opcode(), ins->oprnd1(), ins->oprnd2(), ins->oprnd3(), k);
        return k;
    }

    inline LIns* CseFilter::findLoad(LOpcode op, LIns* a, int32_t d, AccSet accSet,
                                     LInsHashKind kind, uint32_t &k)
    {
        (void)accSet;
        const uint32_t bitmask = m_cap[kind] - 1;
        k = hashLoad(op, a, d, accSet) & bitmask;
        uint32_t n = 1;
        while (true) {
            LIns* ins = m_list[kind][k];
            if (!ins)
                return NULL;
            NanoAssert(ins->accSet() == accSet);
            if (ins->isop(op) && ins->oprnd1() == a && ins->disp() == d)
                return ins;
            k = (k + n) & bitmask;
            n += 1;
        }
    }

    uint32_t CseFilter::findLoadReadOnly(LIns* ins)
    {
        uint32_t k;
        findLoad(ins->opcode(), ins->oprnd1(), ins->disp(), ins->accSet(), LInsLoadReadOnly, k);
        return k;
    }

    uint32_t CseFilter::findLoadStack(LIns* ins)
    {
        uint32_t k;
        findLoad(ins->opcode(), ins->oprnd1(), ins->disp(), ins->accSet(), LInsLoadStack, k);
        return k;
    }

    uint32_t CseFilter::findLoadRStack(LIns* ins)
    {
        uint32_t k;
        findLoad(ins->opcode(), ins->oprnd1(), ins->disp(), ins->accSet(), LInsLoadRStack, k);
        return k;
    }

    uint32_t CseFilter::findLoadOther(LIns* ins)
    {
        uint32_t k;
        findLoad(ins->opcode(), ins->oprnd1(), ins->disp(), ins->accSet(), LInsLoadOther, k);
        return k;
    }

    uint32_t CseFilter::findLoadMultiple(LIns* ins)
    {
        uint32_t k;
        findLoad(ins->opcode(), ins->oprnd1(), ins->disp(), ins->accSet(), LInsLoadMultiple, k);
        return k;
    }

    bool argsmatch(LIns* ins, uint32_t argc, LIns* args[])
    {
        for (uint32_t j=0; j < argc; j++)
            if (ins->arg(j) != args[j])
                return false;
        return true;
    }

    inline LIns* CseFilter::findCall(const CallInfo *ci, uint32_t argc, LIns* args[], uint32_t &k)
    {
        LInsHashKind kind = LInsCall;
        const uint32_t bitmask = m_cap[kind] - 1;
        k = hashCall(ci, argc, args) & bitmask;
        uint32_t n = 1;
        while (true) {
            LIns* ins = m_list[kind][k];
            if (!ins)
                return NULL;
            if (ins->isCall() && ins->callInfo() == ci && argsmatch(ins, argc, args))
                return ins;
            k = (k + n) & bitmask;
            n += 1;
        }
    }

    uint32_t CseFilter::findCall(LIns* ins)
    {
        LIns* args[MAXARGS];
        uint32_t argc = ins->argc();
        NanoAssert(argc < MAXARGS);
        for (uint32_t j=0; j < argc; j++)
            args[j] = ins->arg(j);
        uint32_t k;
        findCall(ins->callInfo(), argc, args, k);
        return k;
    }

    LIns* CseFilter::insImmI(int32_t imm)
    {
        uint32_t k;
        LIns* ins = findImmI(imm, k);
        if (!ins) {
            ins = out->insImmI(imm);
            add(LInsImmI, ins, k);
        }
        // We assume that downstream stages do not modify the instruction, so
        // that we can insert 'ins' into slot 'k'.  Check this.
        NanoAssert(ins->isop(LIR_immi) && ins->immI() == imm);
        return ins;
    }

#ifdef NANOJIT_64BIT
    LIns* CseFilter::insImmQ(uint64_t q)
    {
        uint32_t k;
        LIns* ins = findImmQ(q, k);
        if (!ins) {
            ins = out->insImmQ(q);
            add(LInsImmQ, ins, k);
        }
        NanoAssert(ins->isop(LIR_immq) && ins->immQ() == q);
        return ins;
    }
#endif

    LIns* CseFilter::insImmD(double d)
    {
        uint32_t k;
        // We must pun 'd' as a uint64_t otherwise 0 and -0 will be treated as
        // equal, which breaks things (see bug 527288).
        union {
            double d;
            uint64_t u64;
        } u;
        u.d = d;
        LIns* ins = findImmD(u.u64, k);
        if (!ins) {
            ins = out->insImmD(d);
            add(LInsImmD, ins, k);
        }
        NanoAssert(ins->isop(LIR_immd) && ins->immDasQ() == u.u64);
        return ins;
    }

    LIns* CseFilter::ins0(LOpcode op)
    {
        if (op == LIR_label)
            clear();
        return out->ins0(op);
    }

    LIns* CseFilter::ins1(LOpcode op, LIns* a)
    {
        LIns* ins;
        if (isCseOpcode(op)) {
            uint32_t k;
            ins = find1(op, a, k);
            if (!ins) {
                ins = out->ins1(op, a);
                add(LIns1, ins, k);
            }
        } else {
            ins = out->ins1(op, a);
        }
        NanoAssert(ins->isop(op) && ins->oprnd1() == a);
        return ins;
    }

    LIns* CseFilter::ins2(LOpcode op, LIns* a, LIns* b)
    {
        LIns* ins;
        NanoAssert(isCseOpcode(op));
        uint32_t k;
        ins = find2(op, a, b, k);
        if (!ins) {
            ins = out->ins2(op, a, b);
            add(LIns2, ins, k);
        }
        NanoAssert(ins->isop(op) && ins->oprnd1() == a && ins->oprnd2() == b);
        return ins;
    }

    LIns* CseFilter::ins3(LOpcode op, LIns* a, LIns* b, LIns* c)
    {
        NanoAssert(isCseOpcode(op));
        uint32_t k;
        LIns* ins = find3(op, a, b, c, k);
        if (!ins) {
            ins = out->ins3(op, a, b, c);
            add(LIns3, ins, k);
        }
        NanoAssert(ins->isop(op) && ins->oprnd1() == a && ins->oprnd2() == b && ins->oprnd3() == c);
        return ins;
    }

    LIns* CseFilter::insLoad(LOpcode op, LIns* base, int32_t disp, AccSet loadAccSet)
    {
        LIns* ins;
        if (isS16(disp)) {
            // Clear all loads aliased by stores and calls since the last time
            // we were in this function.
            if (storesSinceLastLoad != ACC_NONE) {
                NanoAssert(!(storesSinceLastLoad & ACC_READONLY));  // can't store to READONLY
                if (storesSinceLastLoad & ACC_STACK)  { clear(LInsLoadStack); }
                if (storesSinceLastLoad & ACC_RSTACK) { clear(LInsLoadRStack); }
                if (storesSinceLastLoad & ACC_OTHER)  { clear(LInsLoadOther); }
                // Loads marked with multiple access regions must be treated
                // conservatively -- we always clear all of them.
                clear(LInsLoadMultiple);
                storesSinceLastLoad = ACC_NONE;
            }

            LInsHashKind kind;
            switch (loadAccSet) {
            case ACC_READONLY:  kind = LInsLoadReadOnly;    break;
            case ACC_STACK:     kind = LInsLoadStack;       break;
            case ACC_RSTACK:    kind = LInsLoadRStack;      break;
            case ACC_OTHER:     kind = LInsLoadOther;       break;
            default:            kind = LInsLoadMultiple;    break;
            }

            uint32_t k;
            ins = findLoad(op, base, disp, loadAccSet, kind, k);
            if (!ins) {
                ins = out->insLoad(op, base, disp, loadAccSet);
                add(kind, ins, k);
            }
            NanoAssert(ins->isop(op) && ins->oprnd1() == base && ins->disp() == disp);

        } else {
            // If the displacement is more than 16 bits, put it in a separate
            // instruction.  Nb: LirBufWriter also does this, we do it here
            // too because CseFilter relies on LirBufWriter not changing code.
            ins = insLoad(op, ins2(LIR_addp, base, insImmWord(disp)), 0, loadAccSet);
        }
        return ins;
    }

    LIns* CseFilter::insStore(LOpcode op, LIns* value, LIns* base, int32_t disp, AccSet accSet)
    {
        LIns* ins;
        if (isS16(disp)) {
            storesSinceLastLoad |= accSet;
            ins = out->insStore(op, value, base, disp, accSet);
            NanoAssert(ins->isop(op) && ins->oprnd1() == value && ins->oprnd2() == base &&
                       ins->disp() == disp && ins->accSet() == accSet);
        } else {
            // If the displacement is more than 16 bits, put it in a separate
            // instruction.  Nb: LirBufWriter also does this, we do it here
            // too because CseFilter relies on LirBufWriter not changing code.
            ins = insStore(op, value, ins2(LIR_addp, base, insImmWord(disp)), 0, accSet);
        }
        return ins;
    }

    LIns* CseFilter::insGuard(LOpcode op, LIns* c, GuardRecord *gr)
    {
        // LIR_xt and LIR_xf guards are CSEable.  Note that we compare the
        // opcode and condition when determining if two guards are equivalent
        // -- in find1() and hash1() -- but we do *not* compare the
        // GuardRecord.  This works because:
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
        LIns* ins;
        if (isCseOpcode(op)) {
            // conditional guard
            uint32_t k;
            ins = find1(op, c, k);
            if (!ins) {
                ins = out->insGuard(op, c, gr);
                add(LIns1, ins, k);
            }
        } else {
            ins = out->insGuard(op, c, gr);
        }
        NanoAssert(ins->isop(op) && ins->oprnd1() == c);
        return ins;
    }

    LIns* CseFilter::insGuardXov(LOpcode op, LIns* a, LIns* b, GuardRecord *gr)
    {
        // LIR_*xov are CSEable.  See CseFilter::insGuard() for details.
        NanoAssert(isCseOpcode(op));
        // conditional guard
        uint32_t k;
        LIns* ins = find2(op, a, b, k);
        if (!ins) {
            ins = out->insGuardXov(op, a, b, gr);
            add(LIns2, ins, k);
        }
        NanoAssert(ins->isop(op) && ins->oprnd1() == a && ins->oprnd2() == b);
        return ins;
    }

    // There is no CseFilter::insBranchJov(), as LIR_*jov* are not CSEable.

    LIns* CseFilter::insCall(const CallInfo *ci, LIns* args[])
    {
        LIns* ins;
        uint32_t argc = ci->count_args();
        if (ci->_isPure) {
            NanoAssert(ci->_storeAccSet == ACC_NONE);
            uint32_t k;
            ins = findCall(ci, argc, args, k);
            if (!ins) {
                ins = out->insCall(ci, args);
                add(LInsCall, ins, k);
            }
        } else {
            // We only need to worry about aliasing if !ci->_isPure.
            storesSinceLastLoad |= ci->_storeAccSet;
            ins = out->insCall(ci, args);
        }
        NanoAssert(ins->isCall() && ins->callInfo() == ci && argsmatch(ins, argc, args));
        return ins;
    }


#if NJ_SOFTFLOAT_SUPPORTED
    static double FASTCALL i2d(int32_t i)           { return i; }
    static double FASTCALL ui2d(uint32_t u)          { return u; }
    static double FASTCALL negd(double a)           { return -a; }
    static double FASTCALL addd(double a, double b) { return a + b; }
    static double FASTCALL subd(double a, double b) { return a - b; }
    static double FASTCALL muld(double a, double b) { return a * b; }
    static double FASTCALL divd(double a, double b) { return a / b; }
    static int32_t FASTCALL eqd(double a, double b) { return a == b; }
    static int32_t FASTCALL ltd(double a, double b) { return a <  b; }
    static int32_t FASTCALL gtd(double a, double b) { return a >  b; }
    static int32_t FASTCALL led(double a, double b) { return a <= b; }
    static int32_t FASTCALL ged(double a, double b) { return a >= b; }

    #define SIG_D_I     CallInfo::typeSig1(ARGTYPE_D, ARGTYPE_I)
    #define SIG_D_UI    CallInfo::typeSig1(ARGTYPE_D, ARGTYPE_UI)
    #define SIG_D_D     CallInfo::typeSig1(ARGTYPE_D, ARGTYPE_D)
    #define SIG_D_DD    CallInfo::typeSig2(ARGTYPE_D, ARGTYPE_D, ARGTYPE_D)
    #define SIG_B_DD    CallInfo::typeSig2(ARGTYPE_B, ARGTYPE_D, ARGTYPE_D)

    #define SF_CALLINFO(name, typesig) \
        static const CallInfo name##_ci = \
            { (intptr_t)&name, typesig, ABI_FASTCALL, /*isPure*/1, ACC_NONE verbose_only(, #name) }

    SF_CALLINFO(i2d,  SIG_D_I);
    SF_CALLINFO(ui2d, SIG_D_UI);
    SF_CALLINFO(negd, SIG_D_D);
    SF_CALLINFO(addd, SIG_D_DD);
    SF_CALLINFO(subd, SIG_D_DD);
    SF_CALLINFO(muld, SIG_D_DD);
    SF_CALLINFO(divd, SIG_D_DD);
    SF_CALLINFO(eqd,  SIG_B_DD);
    SF_CALLINFO(ltd,  SIG_B_DD);
    SF_CALLINFO(gtd,  SIG_B_DD);
    SF_CALLINFO(led,  SIG_B_DD);
    SF_CALLINFO(ged,  SIG_B_DD);

    SoftFloatOps::SoftFloatOps()
    {
        memset(opmap, 0, sizeof(opmap));
        opmap[LIR_i2d] = &i2d_ci;
        opmap[LIR_ui2d] = &ui2d_ci;
        opmap[LIR_negd] = &negd_ci;
        opmap[LIR_addd] = &addd_ci;
        opmap[LIR_subd] = &subd_ci;
        opmap[LIR_muld] = &muld_ci;
        opmap[LIR_divd] = &divd_ci;
        opmap[LIR_eqd] = &eqd_ci;
        opmap[LIR_ltd] = &ltd_ci;
        opmap[LIR_gtd] = &gtd_ci;
        opmap[LIR_led] = &led_ci;
        opmap[LIR_ged] = &ged_ci;
    }

    const SoftFloatOps softFloatOps;

    SoftFloatFilter::SoftFloatFilter(LirWriter *out) : LirWriter(out)
    {}

    LIns* SoftFloatFilter::split(LIns *a) {
        if (a->isD() && !a->isop(LIR_ii2d)) {
            // all F64 args must be qjoin's for soft-float
            a = ins2(LIR_ii2d, ins1(LIR_dlo2i, a), ins1(LIR_dhi2i, a));
        }
        return a;
    }

    LIns* SoftFloatFilter::split(const CallInfo *call, LIns* args[]) {
        LIns *lo = out->insCall(call, args);
        LIns *hi = out->ins1(LIR_hcalli, lo);
        return out->ins2(LIR_ii2d, lo, hi);
    }

    LIns* SoftFloatFilter::callD1(const CallInfo *call, LIns *a) {
        LIns *args[] = { split(a) };
        return split(call, args);
    }

    LIns* SoftFloatFilter::callD2(const CallInfo *call, LIns *a, LIns *b) {
        LIns *args[] = { split(b), split(a) };
        return split(call, args);
    }

    LIns* SoftFloatFilter::cmpD(const CallInfo *call, LIns *a, LIns *b) {
        LIns *args[] = { split(b), split(a) };
        return out->ins2(LIR_eqi, out->insCall(call, args), out->insImmI(1));
    }

    LIns* SoftFloatFilter::ins1(LOpcode op, LIns *a) {
        const CallInfo *ci = softFloatOps.opmap[op];
        if (ci)
            return callD1(ci, a);
        if (op == LIR_retd)
            return out->ins1(op, split(a));
        return out->ins1(op, a);
    }

    LIns* SoftFloatFilter::ins2(LOpcode op, LIns *a, LIns *b) {
        const CallInfo *ci = softFloatOps.opmap[op];
        if (ci) {
            if (isCmpDOpcode(op))
                return cmpD(ci, a, b);
            return callD2(ci, a, b);
        }
        return out->ins2(op, a, b);
    }

    LIns* SoftFloatFilter::insCall(const CallInfo *ci, LIns* args[]) {
        uint32_t nArgs = ci->count_args();
        for (uint32_t i = 0; i < nArgs; i++)
            args[i] = split(args[i]);

        if (ci->returnType() == ARGTYPE_D) {
            // This function returns a double as two 32bit values, so replace
            // call with qjoin(qhi(call), call).
            return split(ci, args);
        }
        return out->insCall(ci, args);
    }
#endif // NJ_SOFTFLOAT_SUPPORTED


    #endif /* FEATURE_NANOJIT */

#if defined(NJ_VERBOSE)
    AddrNameMap::AddrNameMap(Allocator& a)
        : allocator(a), names(a)
    {}

    void AddrNameMap::addAddrRange(const void *p, size_t size, size_t align, const char *name)
    {
        if (!this || names.containsKey(p))
            return;
        char* copy = new (allocator) char[VMPI_strlen(name)+1];
        VMPI_strcpy(copy, name);
        Entry *e = new (allocator) Entry(copy, size << align, align);
        names.put(p, e);
    }

    void AddrNameMap::lookupAddr(void *p, char*& name, int32_t& offset)
    {
        const void *start = names.findNear(p);
        if (start) {
            Entry *e = names.get(start);
            const void *end = (const char*)start + e->size;
            if (p == start) {
                name = e->name;
                offset = 0;
            }
            else if (p > start && p < end) {
                name = e->name;
                offset = int32_t(intptr_t(p)-intptr_t(start)) >> e->align;
            }
            else {
                name = NULL;
                offset = 0;
            }
        } else {
            name = NULL;
            offset = 0;
        }
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
        // Flush every line immediately so that if crashes occur in generated
        // code we won't lose any output.
        fflush(stdout);
    }

#endif // NJ_VERBOSE


#ifdef FEATURE_NANOJIT
#ifdef DEBUG
    const char* ValidateWriter::type2string(LTy type)
    {
        switch (type) {
        case LTy_V:                  return "void";
        case LTy_I:                   return "int32";
#ifdef NANOJIT_64BIT
        case LTy_Q:                   return "int64";
#endif
        case LTy_D:                   return "float64";
        default:       NanoAssert(0);   return "???";
        }
    }

    void ValidateWriter::typeCheckArgs(LOpcode op, int nArgs, LTy formals[], LIns* args[])
    {
        NanoAssert(nArgs >= 0);

        // Type-check the arguments.
        for (int i = 0; i < nArgs; i++) {
            LTy formal = formals[i];
            LTy actual = args[i]->retType();
            if (formal != actual) {
                // Assert on a type error.  The disadvantage of doing this (as
                // opposed to printing a message and continuing) is that at
                // most one type error will be detected per run.  But type
                // errors should be rare, and assertion failures are certain
                // to be caught by test suites whereas error messages may not
                // be.
                NanoAssertMsgf(0,
                    "LIR type error (%s): arg %d of '%s' is '%s' "
                    "which has type %s (expected %s)",
                    whereInPipeline, i+1, lirNames[op],
                    lirNames[args[i]->opcode()],
                    type2string(actual), type2string(formal));
            }
        }
    }

    void ValidateWriter::errorStructureShouldBe(LOpcode op, const char* argDesc, int argN,
                                                LIns* arg, const char* shouldBeDesc)
    {
        NanoAssertMsgf(0,
            "LIR structure error (%s): %s %d of '%s' is '%s' (expected %s)",
            whereInPipeline, argDesc, argN,
            lirNames[op], lirNames[arg->opcode()], shouldBeDesc);
    }

    void ValidateWriter::errorAccSet(const char* what, AccSet accSet, const char* shouldDesc)
    {
        RefBuf b;
        NanoAssertMsgf(0,
            "LIR AccSet error (%s): '%s' AccSet is '%s'; %s",
            whereInPipeline, what, printer->formatAccSet(&b, accSet), shouldDesc);
    }

    void ValidateWriter::checkLInsIsACondOrConst(LOpcode op, int argN, LIns* ins)
    {
        // We could introduce a LTy_B32 type in the type system but that's a
        // bit weird because its representation is identical to LTy_I.  It's
        // easier to just do this check structurally.  Also, optimization can
        // cause the condition to become a LIR_immi.
        if (!ins->isCmp() && !ins->isImmI())
            errorStructureShouldBe(op, "argument", argN, ins, "a condition or 32-bit constant");
    }

    void ValidateWriter::checkLInsIsNull(LOpcode op, int argN, LIns* ins)
    {
        if (ins)
            errorStructureShouldBe(op, "argument", argN, ins, NULL);
    }

    void ValidateWriter::checkLInsHasOpcode(LOpcode op, int argN, LIns* ins, LOpcode op2)
    {
        if (!ins->isop(op2))
            errorStructureShouldBe(op, "argument", argN, ins, lirNames[op2]);
    }

    void ValidateWriter::checkAccSet(LOpcode op, LIns* base, AccSet accSet, AccSet maxAccSet)
    {
        if (accSet == ACC_NONE)
            errorAccSet(lirNames[op], accSet, "it should not equal ACC_NONE");

        if (accSet & ~maxAccSet)
            errorAccSet(lirNames[op], accSet,
                "it should not contain bits that aren't in ACC_LOAD_ANY/ACC_STORE_ANY");

        // Some sanity checking, which is based on the following assumptions:
        // - STACK ones should use 'sp' or 'sp+k' as the base.  (We could look
        //   for more complex patterns, but that feels dangerous.  Better to
        //   keep it really simple.)
        // - RSTACK ones should use 'rp' as the base.
        // - READONLY/OTHER ones should not use 'sp'/'sp+k' or 'rp' as the base.
        //
        // Things that aren't checked:
        // - There's no easy way to check if READONLY ones really are read-only.

        bool isStack = base == sp ||
                      (base->isop(LIR_addp) && base->oprnd1() == sp && base->oprnd2()->isImmP());
        bool isRStack = base == rp;

        switch (accSet) {
        case ACC_STACK:
            if (!isStack)
                errorAccSet(lirNames[op], accSet, "but it's not a stack access");
            break;

        case ACC_RSTACK:
            if (!isRStack)
                errorAccSet(lirNames[op], accSet, "but it's not an rstack access");
            break;

        case ACC_READONLY:
        case ACC_OTHER:
            if (isStack)
                errorAccSet(lirNames[op], accSet, "but it's a stack access");
            if (isRStack)
                errorAccSet(lirNames[op], accSet, "but it's an rstack access");
            break;

        default:
            break;
        }
    }

    ValidateWriter::ValidateWriter(LirWriter *out, LInsPrinter* printer, const char* where)
        : LirWriter(out), printer(printer), whereInPipeline(where), sp(0), rp(0)
    {}

    LIns* ValidateWriter::insLoad(LOpcode op, LIns* base, int32_t d, AccSet accSet)
    {
        checkAccSet(op, base, accSet, ACC_LOAD_ANY);

        int nArgs = 1;
        LTy formals[1] = { LTy_P };
        LIns* args[1] = { base };

        switch (op) {
        case LIR_ldi:
        case LIR_ldd:
        case LIR_lduc2ui:
        case LIR_ldus2ui:
        case LIR_ldc2i:
        case LIR_lds2i:
        case LIR_ldf2d:
        CASE64(LIR_ldq:)
            break;
        default:
            NanoAssert(0);
        }

        typeCheckArgs(op, nArgs, formals, args);

        return out->insLoad(op, base, d, accSet);
    }

    LIns* ValidateWriter::insStore(LOpcode op, LIns* value, LIns* base, int32_t d, AccSet accSet)
    {
        checkAccSet(op, base, accSet, ACC_STORE_ANY);

        int nArgs = 2;
        LTy formals[2] = { LTy_V, LTy_P };     // LTy_V is overwritten shortly
        LIns* args[2] = { value, base };

        switch (op) {
        case LIR_sti2c:
        case LIR_sti2s:
        case LIR_sti:
            formals[0] = LTy_I;
            break;

#ifdef NANOJIT_64BIT
        case LIR_stq:
            formals[0] = LTy_Q;
            break;
#endif

        case LIR_std:
        case LIR_std2f:
            formals[0] = LTy_D;
            break;

        default:
            NanoAssert(0);
        }

        typeCheckArgs(op, nArgs, formals, args);

        return out->insStore(op, value, base, d, accSet);
    }

    LIns* ValidateWriter::ins0(LOpcode op)
    {
        switch (op) {
        case LIR_start:
        case LIR_regfence:
        case LIR_label:
            break;
        default:
            NanoAssert(0);
        }

        // No args to type-check.

        return out->ins0(op);
    }

    LIns* ValidateWriter::ins1(LOpcode op, LIns* a)
    {
        int nArgs = 1;
        LTy formals[1];
        LIns* args[1] = { a };

        switch (op) {
        case LIR_negi:
        case LIR_noti:
        case LIR_i2d:
        case LIR_ui2d:
        case LIR_livei:
        case LIR_reti:
            formals[0] = LTy_I;
            break;

#ifdef NANOJIT_64BIT
        case LIR_i2q:
        case LIR_ui2uq:
            formals[0] = LTy_I;
            break;

        case LIR_q2i:
        case LIR_retq:
        case LIR_liveq:
            formals[0] = LTy_Q;
            break;
#endif

#if defined NANOJIT_IA32 || defined NANOJIT_X64
        case LIR_modi:       // see LIRopcode.tbl for why 'mod' is unary
            checkLInsHasOpcode(op, 1, a, LIR_divi);
            formals[0] = LTy_I;
            break;
#endif

#if NJ_SOFTFLOAT_SUPPORTED
        case LIR_dlo2i:
        case LIR_dhi2i:
            formals[0] = LTy_D;
            break;

        case LIR_hcalli:
            // The operand of a LIR_hcalli is LIR_calli, even though the
            // function being called has a return type of LTy_D.
            checkLInsHasOpcode(op, 1, a, LIR_calli);
            formals[0] = LTy_I;
            break;
#endif

        case LIR_negd:
        case LIR_retd:
        case LIR_lived:
        case LIR_d2i:
            formals[0] = LTy_D;
            break;

        case LIR_file:
        case LIR_line:
            // XXX: not sure about these ones.  Ignore for the moment.
            nArgs = 0;
            break;

        default:
            NanoAssertMsgf(0, "%s\n", lirNames[op]);
        }

        typeCheckArgs(op, nArgs, formals, args);

        return out->ins1(op, a);
    }

    LIns* ValidateWriter::ins2(LOpcode op, LIns* a, LIns* b)
    {
        int nArgs = 2;
        LTy formals[2];
        LIns* args[2] = { a, b };

        switch (op) {
        case LIR_addi:
        case LIR_subi:
        case LIR_muli:
        CASE86(LIR_divi:)
        case LIR_andi:
        case LIR_ori:
        case LIR_xori:
        case LIR_lshi:
        case LIR_rshi:
        case LIR_rshui:
        case LIR_eqi:
        case LIR_lti:
        case LIR_gti:
        case LIR_lei:
        case LIR_gei:
        case LIR_ltui:
        case LIR_gtui:
        case LIR_leui:
        case LIR_geui:
            formals[0] = LTy_I;
            formals[1] = LTy_I;
            break;

#if NJ_SOFTFLOAT_SUPPORTED
        case LIR_ii2d:
            formals[0] = LTy_I;
            formals[1] = LTy_I;
            break;
#endif

#ifdef NANOJIT_64BIT
        case LIR_andq:
        case LIR_orq:
        case LIR_xorq:
        case LIR_addq:
        case LIR_subq:
        case LIR_eqq:
        case LIR_ltq:
        case LIR_gtq:
        case LIR_leq:
        case LIR_geq:
        case LIR_ltuq:
        case LIR_gtuq:
        case LIR_leuq:
        case LIR_geuq:
            formals[0] = LTy_Q;
            formals[1] = LTy_Q;
            break;

        case LIR_lshq:
        case LIR_rshq:
        case LIR_rshuq:
            formals[0] = LTy_Q;
            formals[1] = LTy_I;
            break;
#endif

        case LIR_addd:
        case LIR_subd:
        case LIR_muld:
        case LIR_divd:
        case LIR_eqd:
        case LIR_gtd:
        case LIR_ltd:
        case LIR_led:
        case LIR_ged:
            formals[0] = LTy_D;
            formals[1] = LTy_D;
            break;

        default:
            NanoAssert(0);
        }

        typeCheckArgs(op, nArgs, formals, args);

        return out->ins2(op, a, b);
    }

    LIns* ValidateWriter::ins3(LOpcode op, LIns* a, LIns* b, LIns* c)
    {
        int nArgs = 3;
        LTy formals[3] = { LTy_I, LTy_V, LTy_V };   // LTy_V gets overwritten
        LIns* args[3] = { a, b, c };

        switch (op) {
        case LIR_cmovi:
            checkLInsIsACondOrConst(op, 1, a);
            formals[1] = LTy_I;
            formals[2] = LTy_I;
            break;

#ifdef NANOJIT_64BIT
        case LIR_cmovq:
            checkLInsIsACondOrConst(op, 1, a);
            formals[1] = LTy_Q;
            formals[2] = LTy_Q;
            break;
#endif

        default:
            NanoAssert(0);
        }

        typeCheckArgs(op, nArgs, formals, args);

        return out->ins3(op, a, b, c);
    }

    LIns* ValidateWriter::insParam(int32_t arg, int32_t kind)
    {
        return out->insParam(arg, kind);
    }

    LIns* ValidateWriter::insImmI(int32_t imm)
    {
        return out->insImmI(imm);
    }

#ifdef NANOJIT_64BIT
    LIns* ValidateWriter::insImmQ(uint64_t imm)
    {
        return out->insImmQ(imm);
    }
#endif

    LIns* ValidateWriter::insImmD(double d)
    {
        return out->insImmD(d);
    }

    LIns* ValidateWriter::insCall(const CallInfo *ci, LIns* args0[])
    {
        ArgType argTypes[MAXARGS];
        uint32_t nArgs = ci->getArgTypes(argTypes);
        LTy formals[MAXARGS];
        LIns* args[MAXARGS];    // in left-to-right order, unlike args0[]

        LOpcode op = getCallOpcode(ci);

        if (ci->_isPure && ci->_storeAccSet != ACC_NONE)
            errorAccSet(ci->_name, ci->_storeAccSet, "it should be ACC_NONE for pure functions");

        if (ci->_storeAccSet & ~ACC_STORE_ANY)
            errorAccSet(lirNames[op], ci->_storeAccSet,
                "it should not contain bits that aren't in ACC_STORE_ANY");

        // This loop iterates over the args from right-to-left (because arg()
        // and getArgTypes() use right-to-left order), but puts the results
        // into formals[] and args[] in left-to-right order so that arg
        // numbers in error messages make sense to the user.
        for (uint32_t i = 0; i < nArgs; i++) {
            uint32_t i2 = nArgs - i - 1;    // converts right-to-left to left-to-right
            switch (argTypes[i]) {
            case ARGTYPE_I:
            case ARGTYPE_UI:         formals[i2] = LTy_I;   break;
#ifdef NANOJIT_64BIT
            case ARGTYPE_Q:         formals[i2] = LTy_Q;   break;
#endif
            case ARGTYPE_D:         formals[i2] = LTy_D;   break;
            default: NanoAssertMsgf(0, "%d %s\n", argTypes[i],ci->_name); formals[i2] = LTy_V;  break;
            }
            args[i2] = args0[i];
        }

        typeCheckArgs(op, nArgs, formals, args);

        return out->insCall(ci, args0);
    }

    LIns* ValidateWriter::insGuard(LOpcode op, LIns *cond, GuardRecord *gr)
    {
        int nArgs = -1;     // init to shut compilers up
        LTy formals[1];
        LIns* args[1];

        switch (op) {
        case LIR_x:
        case LIR_xbarrier:
            checkLInsIsNull(op, 1, cond);
            nArgs = 0;
            break;

        case LIR_xt:
        case LIR_xf:
            checkLInsIsACondOrConst(op, 1, cond);
            nArgs = 1;
            formals[0] = LTy_I;
            args[0] = cond;
            break;

        case LIR_xtbl:
            nArgs = 1;
            formals[0] = LTy_I;   // unlike xt/xf/jt/jf, this is an index, not a condition
            args[0] = cond;
            break;

        default:
            NanoAssert(0);
        }

        typeCheckArgs(op, nArgs, formals, args);

        return out->insGuard(op, cond, gr);
    }

    LIns* ValidateWriter::insGuardXov(LOpcode op, LIns* a, LIns* b, GuardRecord* gr)
    {
        int nArgs = 2;
        LTy formals[2] = { LTy_I, LTy_I };
        LIns* args[2] = { a, b };

        switch (op) {
        case LIR_addxovi:
        case LIR_subxovi:
        case LIR_mulxovi:
            break;

        default:
            NanoAssert(0);
        }

        typeCheckArgs(op, nArgs, formals, args);

        return out->insGuardXov(op, a, b, gr);
    }

    LIns* ValidateWriter::insBranch(LOpcode op, LIns* cond, LIns* to)
    {
        int nArgs = -1;     // init to shut compilers up
        LTy formals[1];
        LIns* args[1];

        switch (op) {
        case LIR_j:
            checkLInsIsNull(op, 1, cond);
            nArgs = 0;
            break;

        case LIR_jt:
        case LIR_jf:
            checkLInsIsACondOrConst(op, 1, cond);
            nArgs = 1;
            formals[0] = LTy_I;
            args[0] = cond;
            break;

        default:
            NanoAssert(0);
        }

        // We check that target is a label in ValidateReader because it may
        // not have been set here.

        typeCheckArgs(op, nArgs, formals, args);

        return out->insBranch(op, cond, to);
    }

    LIns* ValidateWriter::insBranchJov(LOpcode op, LIns* a, LIns* b, LIns* to)
    {
        int nArgs = 2;
        LTy formals[2];
        LIns* args[2] = { a, b };

        switch (op) {
        case LIR_addjovi:
        case LIR_subjovi:
        case LIR_muljovi:
            formals[0] = LTy_I;
            formals[1] = LTy_I;
            break;

#ifdef NANOJIT_64BIT
        case LIR_addjovq:
        case LIR_subjovq:
            formals[0] = LTy_Q;
            formals[1] = LTy_Q;
            break;
#endif
        default:
            NanoAssert(0);
        }

        // We check that target is a label in ValidateReader because it may
        // not have been set here.

        typeCheckArgs(op, nArgs, formals, args);

        return out->insBranchJov(op, a, b, to);
    }

    LIns* ValidateWriter::insAlloc(int32_t size)
    {
        return out->insAlloc(size);
    }

    LIns* ValidateWriter::insJtbl(LIns* index, uint32_t size)
    {
        int nArgs = 1;
        LTy formals[1] = { LTy_I };
        LIns* args[1] = { index };

        typeCheckArgs(LIR_jtbl, nArgs, formals, args);

        // We check that all jump table entries are labels in ValidateReader
        // because they won't have been set here.

        return out->insJtbl(index, size);
    }

    ValidateReader::ValidateReader(LirFilter* in) : LirFilter(in)
        {}

    LIns* ValidateReader::read()
    {
        LIns *ins = in->read();
        switch (ins->opcode()) {
        case LIR_jt:
        case LIR_jf:
        case LIR_j:
            NanoAssert(ins->getTarget() && ins->oprnd2()->isop(LIR_label));
            break;

        case LIR_addjovi:
        case LIR_subjovi:
        case LIR_muljovi:
        CASE64(LIR_addjovq:)
        CASE64(LIR_subjovq:)
            NanoAssert(ins->getTarget() && ins->oprnd3()->isop(LIR_label));
            break;

        case LIR_jtbl: {
            uint32_t tableSize = ins->getTableSize();
            NanoAssert(tableSize > 0);
            for (uint32_t i = 0; i < tableSize; i++) {
                LIns* target = ins->getTarget(i);
                NanoAssert(target);
                NanoAssert(target->isop(LIR_label));
            }
            break;
        }
        default:
            ;
        }
        return ins;
    }

#endif
#endif

}
