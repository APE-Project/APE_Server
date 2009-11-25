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

#ifdef FEATURE_NANOJIT

#ifdef VTUNE
#include "../core/CodegenLIR.h"
#endif

#ifdef _MSC_VER
    // disable some specific warnings which are normally useful, but pervasive in the code-gen macros
    #pragma warning(disable:4310) // cast truncates constant value
#endif

namespace nanojit
{
    /**
     * Need the following:
     *
     *    - merging paths ( build a graph? ), possibly use external rep to drive codegen
     */
    Assembler::Assembler(CodeAlloc& codeAlloc, Allocator& dataAlloc, Allocator& alloc, AvmCore* core, LogControl* logc)
        : codeList(NULL)
        , alloc(alloc)
        , _codeAlloc(codeAlloc)
        , _dataAlloc(dataAlloc)
        , _thisfrag(NULL)
        , _branchStateMap(alloc)
        , _patches(alloc)
        , _labels(alloc)
        , _epilogue(NULL)
        , _err(None)
    #if PEDANTIC
        , pedanticTop(NULL)
    #endif
    #ifdef VTUNE
        , cgen(NULL)
    #endif
        , config(core->config)
    {
        VMPI_memset(&_stats, 0, sizeof(_stats));
        nInit(core);
        (void)logc;
        verbose_only( _logc = logc; )
        verbose_only( _outputCache = 0; )
        verbose_only( outline[0] = '\0'; )
        verbose_only( outlineEOL[0] = '\0'; )
        verbose_only( outputAddr = false; )

        reset();
    }

    void Assembler::arReset()
    {
        _activation.lowwatermark = 0;
        _activation.tos = 0;

        for(uint32_t i=0; i<NJ_MAX_STACK_ENTRY; i++)
            _activation.entry[i] = 0;

        _branchStateMap.clear();
        _patches.clear();
        _labels.clear();
    }

    void Assembler::registerResetAll()
    {
        nRegisterResetAll(_allocator);

        // At start, should have some registers free and none active.
        NanoAssert(0 != _allocator.free);
        NanoAssert(0 == _allocator.countActive());
#ifdef NANOJIT_IA32
        debug_only(_fpuStkDepth = 0; )
#endif
    }

    // Finds a register in 'allow' to store the result of 'ins', evicting one
    // if necessary.  Doesn't consider the prior state of 'ins' (except that
    // ins->isUsed() must be true).
    Register Assembler::registerAlloc(LIns* ins, RegisterMask allow)
    {
        RegisterMask allowedAndFree = allow & _allocator.free;
        Register r;
        NanoAssert(ins->isUsed());

        if (allowedAndFree) {
            // At least one usable register is free -- no need to steal.  
            // Pick a preferred one if possible.
            RegisterMask preferredAndFree = allowedAndFree & SavedRegs;
            RegisterMask set = ( preferredAndFree ? preferredAndFree : allowedAndFree );
            r = nRegisterAllocFromSet(set);
            _allocator.addActive(r, ins);
            ins->setReg(r);
        } else {
            counter_increment(steals);

            // Nothing free, steal one.
            // LSRA says pick the one with the furthest use.
            LIns* vicIns = findVictim(allow);
            NanoAssert(vicIns->isUsed());
            r = vicIns->getReg();

            _allocator.removeActive(r);
            vicIns->setReg(UnknownReg);

            // Restore vicIns.
            verbose_only( if (_logc->lcbits & LC_Assembly) {
                            setOutputForEOL("  <= restore %s",
                            _thisfrag->lirbuf->names->formatRef(vicIns)); } )
            asm_restore(vicIns, r);

            // r ends up staying active, but the LIns defining it changes.
            _allocator.addActive(r, ins);
            ins->setReg(r);
        }
        return r;
    }

    // Finds a register in 'allow' to store a temporary value (one not
    // associated with a particular LIns), evicting one if necessary.  The
    // returned register is marked as being free and so can only be safely
    // used for code generation purposes until the register state is next
    // inspected or updated.
    Register Assembler::registerAllocTmp(RegisterMask allow)
    {
        LIns dummyIns;
        dummyIns.markAsUsed();
        Register r = registerAlloc(&dummyIns, allow); 

        // Mark r as free, ready for use as a temporary value.
        _allocator.removeActive(r);
        _allocator.addFree(r);
        return r;
     }

    /**
     * these instructions don't have to be saved & reloaded to spill,
     * they can just be recalculated w/out any inputs.
     */
    bool Assembler::canRemat(LIns *i) {
        return i->isconst() || i->isconstq() || i->isop(LIR_alloc);
    }

    void Assembler::codeAlloc(NIns *&start, NIns *&end, NIns *&eip
                              verbose_only(, size_t &nBytes))
    {
        // save the block we just filled
        if (start)
            CodeAlloc::add(codeList, start, end);

        // CodeAlloc contract: allocations never fail
        _codeAlloc.alloc(start, end);
        verbose_only( nBytes += (end - start) * sizeof(NIns); )
        NanoAssert(uintptr_t(end) - uintptr_t(start) >= (size_t)LARGEST_UNDERRUN_PROT);
        eip = end;

        #ifdef VTUNE
        if (_nIns && _nExitIns) {
            //cgen->jitAddRecord((uintptr_t)list->code, 0, 0, true); // add placeholder record for top of page
            cgen->jitCodePosUpdate((uintptr_t)list->code);
            cgen->jitPushInfo(); // new page requires new entry
        }
        #endif
    }

    void Assembler::reset()
    {
        _nIns = 0;
        _nExitIns = 0;
        codeStart = codeEnd = 0;
        exitStart = exitEnd = 0;
        _stats.pages = 0;
        codeList = 0;

        nativePageReset();
        registerResetAll();
        arReset();
    }

    #ifdef _DEBUG
    void Assembler::pageValidate()
    {
        if (error()) return;
        // This may be a normal code chunk or an exit code chunk.
        NanoAssertMsg(containsPtr(codeStart, codeEnd, _nIns),
                     "Native instruction pointer overstep paging bounds; check overrideProtect for last instruction");
    }
    #endif

    #ifdef _DEBUG

    void Assembler::resourceConsistencyCheck()
    {
        NanoAssert(!error());

#ifdef NANOJIT_IA32
        NanoAssert((_allocator.active[FST0] && _fpuStkDepth == -1) ||
            (!_allocator.active[FST0] && _fpuStkDepth == 0));
#endif

        AR &ar = _activation;
        // check AR entries
        NanoAssert(ar.tos < NJ_MAX_STACK_ENTRY);
        LIns* ins = 0;
        RegAlloc* regs = &_allocator;
        for(uint32_t i = ar.lowwatermark; i < ar.tos; i++)
        {
            ins = ar.entry[i];
            if ( !ins )
                continue;
            Register r = ins->getReg();
            uint32_t arIndex = ins->getArIndex();
            if (arIndex != 0) {
                if (ins->isop(LIR_alloc)) {
                    int j=i+1;
                    for (int n = i + (ins->size()>>2); j < n; j++) {
                        NanoAssert(ar.entry[j]==ins);
                    }
                    NanoAssert(arIndex == (uint32_t)j-1);
                    i = j-1;
                }
                else if (ins->isQuad()) {
                    NanoAssert(ar.entry[i - stack_direction(1)]==ins);
                    i += 1; // skip high word
                }
                else {
                    NanoAssertMsg(arIndex == i, "Stack record index mismatch");
                }
            }
            NanoAssertMsg( !isKnownReg(r) || regs->isConsistent(r,ins), "Register record mismatch");
        }

        registerConsistencyCheck();
    }

    void Assembler::registerConsistencyCheck()
    {
        RegisterMask managed = _allocator.managed;
        for (Register r = FirstReg; r <= LastReg; r = nextreg(r)) {
            if (rmask(r) & managed) {
                // A register managed by register allocation must be either
                // free or active, but not both.
                if (_allocator.isFree(r)) {
                    NanoAssertMsgf(_allocator.getActive(r)==0,
                        "register %s is free but assigned to ins", gpn(r));
                } else {
                    // An LIns defining a register must have that register in
                    // its reservation.
                    LIns* ins = _allocator.getActive(r);
                    NanoAssert(ins);
                    NanoAssertMsg(r == ins->getReg(), "Register record mismatch");
                }
            } else {
                // A register not managed by register allocation must be
                // neither free nor active.
                NanoAssert(!_allocator.isFree(r));
                NanoAssert(!_allocator.getActive(r));
            }
        }
    }
    #endif /* _DEBUG */

    void Assembler::findRegFor2(RegisterMask allow, LIns* ia, Register& ra, LIns* ib, Register& rb)
    {
        if (ia == ib) {
            ra = rb = findRegFor(ia, allow);
        } else {
            // You might think we could just do this:
            //
            //   ra = findRegFor(ia, allow);
            //   rb = findRegFor(ib, allow & ~rmask(ra));
            //
            // But if 'ib' was already in an allowed register, the first
            // findRegFor() call could evict it, whereupon the second
            // findRegFor() call would immediately restore it, which is
            // sub-optimal.  What we effectively do instead is this:
            //
            //   ra = findRegFor(ia, allow & ~rmask(rb));
            //   rb = findRegFor(ib, allow & ~rmask(ra));
            //
            // but we have to determine what 'rb' initially is to avoid the
            // mutual dependency between the assignments.
            bool rbDone = !ib->isUnusedOrHasUnknownReg() && (rb = ib->getReg(), allow & rmask(rb));
            if (rbDone) {
                allow &= ~rmask(rb);    // ib already in an allowable reg, keep that one
            }
            ra = findRegFor(ia, allow);
            if (!rbDone) {
                allow &= ~rmask(ra);
                rb = findRegFor(ib, allow);
            }
        }
    }

    Register Assembler::findSpecificRegFor(LIns* i, Register w)
    {
        return findRegFor(i, rmask(w));
    }

    // The 'op' argument is the opcode of the instruction containing the
    // displaced i[d] operand we're finding a register for. It is only used
    // for differentiating classes of valid displacement in the native
    // backends; a bit of a hack.
    Register Assembler::getBaseReg(LOpcode op, LIns *i, int &d, RegisterMask allow)
    {
    #if !PEDANTIC
        if (i->isop(LIR_alloc)) {
            int d2 = d;
            d2 += findMemFor(i);
            if (isValidDisplacement(op, d2)) {
                d = d2;
                return FP;
            }
        }
    #else
        (void) d;
    #endif
        return findRegFor(i, allow);
    }

    // Finds a register in 'allow' to hold the result of 'ins'.  Used when we
    // encounter a use of 'ins'.  The actions depend on the prior state of
    // 'ins':
    // - If the result of 'ins' is not in any register, we find an allowed
    //   one, evicting one if necessary.
    // - If the result of 'ins' is already in an allowed register, we use that.
    // - If the result of 'ins' is already in a not-allowed register, we find an
    //   allowed one and move it.
    //
    Register Assembler::findRegFor(LIns* ins, RegisterMask allow)
    {
        if (ins->isop(LIR_alloc)) {
            // never allocate a reg for this w/out stack space too
            findMemFor(ins);
        }

        Register r;

        if (!ins->isUsed()) {
            // No reservation.  Create one, and do a fresh allocation.
            ins->markAsUsed();
            RegisterMask prefer = hint(ins, allow);
            r = registerAlloc(ins, prefer);

        } else if (!ins->hasKnownReg()) {
            // Existing reservation with an unknown register.  Do a fresh
            // allocation.
            RegisterMask prefer = hint(ins, allow);
            r = registerAlloc(ins, prefer);

        } else if (rmask(r = ins->getReg()) & allow) {
            // Existing reservation with a known register allocated, and
            // that register is allowed.  Use it.
            _allocator.useActive(r);

        } else {
            // Existing reservation with a known register allocated, but
            // the register is not allowed.
            RegisterMask prefer = hint(ins, allow);
#ifdef NANOJIT_IA32
            if (((rmask(r)&XmmRegs) && !(allow&XmmRegs)) ||
                ((rmask(r)&x87Regs) && !(allow&x87Regs)))
            {
                // x87 <-> xmm copy required
                //_nvprof("fpu-evict",1);
                evict(r, ins);
                r = registerAlloc(ins, prefer);
            } else
#elif defined(NANOJIT_PPC)
            if (((rmask(r)&GpRegs) && !(allow&GpRegs)) ||
                ((rmask(r)&FpRegs) && !(allow&FpRegs)))
            {
                evict(r, ins);
                r = registerAlloc(ins, prefer);
            } else
#endif
            {
                // The post-state register holding 'ins' is 's', the pre-state
                // register holding 'ins' is 'r'.  For example, if s=eax and
                // r=ecx:
                //
                // pre-state:   ecx(ins)
                // instruction: mov eax, ecx
                // post-state:  eax(ins)
                //
                _allocator.retire(r);
                Register s = r;
                r = registerAlloc(ins, prefer);
                if ((rmask(s) & GpRegs) && (rmask(r) & GpRegs)) {
#ifdef NANOJIT_ARM
                    MOV(s, r);  // ie. move 'ins' from its pre-state reg to its post-state reg
#else
                    MR(s, r);
#endif
                }
                else {
                    asm_nongp_copy(s, r);
                }
            }
        }
        return r;
    }

    // Like findSpecificRegFor(), but only for when 'r' is known to be free
    // and 'ins' is known to not already have a register allocated.  Updates
    // the register state (maintaining the invariants) but does not generate
    // any code.  The return value is redundant, always being 'r', but it's
    // sometimes useful to have it there for assignments.
    Register Assembler::findSpecificRegForUnallocated(LIns* ins, Register r)
    {
        if (ins->isop(LIR_alloc)) {
            // never allocate a reg for this w/out stack space too
            findMemFor(ins);
        }

        NanoAssert(ins->isUnusedOrHasUnknownReg());
        NanoAssert(_allocator.free & rmask(r));

        if (!ins->isUsed())
            ins->markAsUsed();
        ins->setReg(r);
        _allocator.removeFree(r);
        _allocator.addActive(r, ins);

        return r;
    }
 
    int Assembler::findMemFor(LIns *ins)
    {
        if (!ins->isUsed())
            ins->markAsUsed();
        if (!ins->getArIndex()) {
            ins->setArIndex(arReserve(ins));
            NanoAssert(ins->getArIndex() <= _activation.tos);
        }
        return disp(ins);
    }

    Register Assembler::prepResultReg(LIns *ins, RegisterMask allow)
    {
        // 'pop' is only relevant on i386 and if 'allow' includes FST0, in
        // which case we have to pop if 'ins' isn't in FST0 in the post-state.
        // This could be because 'ins' is unused, is in a spill slot, or is in
        // an XMM register.
#ifdef NANOJIT_IA32
        const bool pop = (allow & rmask(FST0)) &&
                         (ins->isUnusedOrHasUnknownReg() || ins->getReg() != FST0);
#else
        const bool pop = false;
#endif
        Register r = findRegFor(ins, allow);
        freeRsrcOf(ins, pop);
        return r;
    }

    void Assembler::asm_spilli(LInsp ins, bool pop)
    {
        int d = disp(ins);
        Register r = ins->getReg();
        verbose_only( if (d && (_logc->lcbits & LC_Assembly)) {
                         setOutputForEOL("  <= spill %s",
                         _thisfrag->lirbuf->names->formatRef(ins)); } )
        asm_spill(r, d, pop, ins->isQuad());
    }

    // NOTE: Because this function frees slots on the stack, it is not safe to
    // follow a call to this with a call to anything which might spill a
    // register, as the stack can be corrupted. Refer to bug 495239 for a more
    // detailed description.
    void Assembler::freeRsrcOf(LIns *ins, bool pop)
    {
        Register r = ins->getReg();
        if (isKnownReg(r)) {
            asm_spilli(ins, pop);
            _allocator.retire(r);   // free any register associated with entry
        }
        int arIndex = ins->getArIndex();
        if (arIndex) {
            NanoAssert(_activation.entry[arIndex] == ins);
            arFree(arIndex);        // free any stack stack space associated with entry
        }
        ins->markAsClear();
    }

    // Frees 'r' in the RegAlloc state, if it's not already free.
    void Assembler::evictIfActive(Register r)
    {
        if (LIns* vic = _allocator.getActive(r)) {
            evict(r, vic);
        }
    }

    // Frees 'r' (which currently holds the result of 'vic') in the RegAlloc
    // state.  An example:
    //
    //   pre-state:     eax(ld1)
    //   instruction:   mov ebx,-4(ebp) <= restore add1   # %ebx is dest
    //   post-state:    eax(ld1) ebx(add1)
    //
    // At run-time we are *restoring* 'add1' into %ebx, hence the call to
    // asm_restore().  But at regalloc-time we are moving backwards through
    // the code, so in that sense we are *evicting* 'add1' from %ebx.
    //
    void Assembler::evict(Register r, LIns* vic)
    {
        // Not free, need to steal.
        counter_increment(steals);

        // Get vic's resv, check r matches.
        NanoAssert(!_allocator.isFree(r));
        NanoAssert(vic == _allocator.getActive(r));
        NanoAssert(r == vic->getReg());

        // Free r.
        _allocator.retire(r);
        vic->setReg(UnknownReg);

        // Restore vic.
        verbose_only( if (_logc->lcbits & LC_Assembly) {
                        setOutputForEOL("  <= restore %s",
                        _thisfrag->lirbuf->names->formatRef(vic)); } )
        asm_restore(vic, r);
    }

    void Assembler::patch(GuardRecord *lr)
    {
        if (!lr->jmp) // the guard might have been eliminated as redundant
            return;
        Fragment *frag = lr->exit->target;
        NanoAssert(frag->fragEntry != 0);
        nPatchBranch((NIns*)lr->jmp, frag->fragEntry);
        CodeAlloc::flushICache(lr->jmp, LARGEST_BRANCH_PATCH);
        verbose_only(verbose_outputf("patching jump at %p to target %p\n",
            lr->jmp, frag->fragEntry);)
    }

    void Assembler::patch(SideExit *exit)
    {
        GuardRecord *rec = exit->guards;
        NanoAssert(rec);
        while (rec) {
            patch(rec);
            rec = rec->next;
        }
    }

#ifdef NANOJIT_IA32
    void Assembler::patch(SideExit* exit, SwitchInfo* si)
    {
        for (GuardRecord* lr = exit->guards; lr; lr = lr->next) {
            Fragment *frag = lr->exit->target;
            NanoAssert(frag->fragEntry != 0);
            si->table[si->index] = frag->fragEntry;
        }
    }
#endif

    NIns* Assembler::asm_exit(LInsp guard)
    {
        SideExit *exit = guard->record()->exit;
        NIns* at = 0;
        if (!_branchStateMap.get(exit))
        {
            at = asm_leave_trace(guard);
        }
        else
        {
            RegAlloc* captured = _branchStateMap.get(exit);
            intersectRegisterState(*captured);
            at = exit->target->fragEntry;
            NanoAssert(at != 0);
            _branchStateMap.remove(exit);
        }
        return at;
    }

    NIns* Assembler::asm_leave_trace(LInsp guard)
    {
        verbose_only( int32_t nativeSave = _stats.native );
        verbose_only( verbose_outputf("----------------------------------- ## END exit block %p", guard);)

        RegAlloc capture = _allocator;

        // this point is unreachable.  so free all the registers.
        // if an instruction has a stack entry we will leave it alone,
        // otherwise we free it entirely.  intersectRegisterState will restore.
        releaseRegisters();

        swapCodeChunks();
        _inExit = true;

#ifdef NANOJIT_IA32
        debug_only( _sv_fpuStkDepth = _fpuStkDepth; _fpuStkDepth = 0; )
#endif

        nFragExit(guard);

        // restore the callee-saved register and parameters
        assignSavedRegs();
        assignParamRegs();

        intersectRegisterState(capture);

        // this can be useful for breaking whenever an exit is taken
        //INT3();
        //NOP();

        // we are done producing the exit logic for the guard so demark where our exit block code begins
        NIns* jmpTarget = _nIns;     // target in exit path for our mainline conditional jump

        // swap back pointers, effectively storing the last location used in the exit path
        swapCodeChunks();
        _inExit = false;

        //verbose_only( verbose_outputf("         LIR_xt/xf swapCodeChunks, _nIns is now %08X(%08X), _nExitIns is now %08X(%08X)",_nIns, *_nIns,_nExitIns,*_nExitIns) );
        verbose_only( verbose_outputf("%010lx:", (unsigned long)jmpTarget);)
        verbose_only( verbose_outputf("----------------------------------- ## BEGIN exit block (LIR_xt|LIR_xf)") );

#ifdef NANOJIT_IA32
        NanoAssertMsgf(_fpuStkDepth == _sv_fpuStkDepth, "LIR_xtf, _fpuStkDepth=%d, expect %d",_fpuStkDepth, _sv_fpuStkDepth);
        debug_only( _fpuStkDepth = _sv_fpuStkDepth; _sv_fpuStkDepth = 9999; )
#endif

        verbose_only(_stats.exitnative += (_stats.native-nativeSave));

        return jmpTarget;
    }

    void Assembler::beginAssembly(Fragment *frag)
    {
        verbose_only( codeBytes = 0; )
        verbose_only( exitBytes = 0; )

        reset();

        NanoAssert(codeList == 0);
        NanoAssert(codeStart == 0);
        NanoAssert(codeEnd == 0);
        NanoAssert(exitStart == 0);
        NanoAssert(exitEnd == 0);
        NanoAssert(_nIns == 0);
        NanoAssert(_nExitIns == 0);

        _thisfrag = frag;
        _activation.lowwatermark = 1;
        _activation.tos = _activation.lowwatermark;
        _inExit = false;

        counter_reset(native);
        counter_reset(exitnative);
        counter_reset(steals);
        counter_reset(spills);
        counter_reset(remats);

        setError(None);

        // native code gen buffer setup
        nativePageSetup();

        // make sure we got memory at least one page
        if (error()) return;

#ifdef PERFM
        _stats.pages = 0;
        _stats.codeStart = _nIns-1;
        _stats.codeExitStart = _nExitIns-1;
#endif /* PERFM */

        _epilogue = NULL;

        nBeginAssembly();
    }

    void Assembler::assemble(Fragment* frag, LirFilter* reader)
    {
        if (error()) return;
        _thisfrag = frag;

        // check the fragment is starting out with a sane profiling state
        verbose_only( NanoAssert(frag->nStaticExits == 0); )
        verbose_only( NanoAssert(frag->nCodeBytes == 0); )
        verbose_only( NanoAssert(frag->nExitBytes == 0); )
        verbose_only( NanoAssert(frag->profCount == 0); )
        verbose_only( if (_logc->lcbits & LC_FragProfile)
                          NanoAssert(frag->profFragID > 0);
                      else
                          NanoAssert(frag->profFragID == 0); )

        _inExit = false;

        gen(reader);

        if (!error()) {
            // patch all branches
            NInsMap::Iter iter(_patches);
            while (iter.next()) {
                NIns* where = iter.key();
                LIns* target = iter.value();
                if (target->isop(LIR_jtbl)) {
                    // Need to patch up a whole jump table, 'where' is the table.
                    LIns *jtbl = target;
                    NIns** native_table = (NIns**) where;
                    for (uint32_t i = 0, n = jtbl->getTableSize(); i < n; i++) {
                        LabelState* lstate = _labels.get(jtbl->getTarget(i));
                        NIns* ntarget = lstate->addr;
                        if (ntarget) {
                            native_table[i] = ntarget;
                        } else {
                            setError(UnknownBranch);
                            break;
                        }
                    }
                } else {
                    // target is a label for a single-target branch
                    LabelState *lstate = _labels.get(target);
                    NIns* ntarget = lstate->addr;
                    if (ntarget) {
                        nPatchBranch(where, ntarget);
                    } else {
                        setError(UnknownBranch);
                        break;
                    }
                }
            }
        }
    }

    void Assembler::endAssembly(Fragment* frag)
    {
        // don't try to patch code if we are in an error state since we might have partially
        // overwritten the code cache already
        if (error()) {
            // something went wrong, release all allocated code memory
            _codeAlloc.freeAll(codeList);
            _codeAlloc.free(exitStart, exitEnd);
            _codeAlloc.free(codeStart, codeEnd);
            return;
        }

        NIns* fragEntry = genPrologue();
        verbose_only( outputAddr=true; )
        verbose_only( asm_output("[prologue]"); )

        // check for resource leaks
        debug_only(
            for (uint32_t i = _activation.lowwatermark; i < _activation.tos; i++) {
                NanoAssertMsgf(_activation.entry[i] == 0, "frame entry %d wasn't freed\n",-4*i);
            }
        )

        NanoAssert(!_inExit);
        // save used parts of current block on fragment's code list, free the rest
#ifdef NANOJIT_ARM
        // [codeStart, _nSlot) ... gap ... [_nIns, codeEnd)
        _codeAlloc.addRemainder(codeList, exitStart, exitEnd, _nExitSlot, _nExitIns);
        _codeAlloc.addRemainder(codeList, codeStart, codeEnd, _nSlot, _nIns);
        verbose_only( exitBytes -= (_nExitIns - _nExitSlot) * sizeof(NIns); )
        verbose_only( codeBytes -= (_nIns - _nSlot) * sizeof(NIns); )
#else
        // [codeStart ... gap ... [_nIns, codeEnd))
        _codeAlloc.addRemainder(codeList, exitStart, exitEnd, exitStart, _nExitIns);
        _codeAlloc.addRemainder(codeList, codeStart, codeEnd, codeStart, _nIns);
        verbose_only( exitBytes -= (_nExitIns - exitStart) * sizeof(NIns); )
        verbose_only( codeBytes -= (_nIns - codeStart) * sizeof(NIns); )
#endif

        // at this point all our new code is in the d-cache and not the i-cache,
        // so flush the i-cache on cpu's that need it.
        CodeAlloc::flushICache(codeList);

        // save entry point pointers
        frag->fragEntry = fragEntry;
        frag->setCode(_nIns);
        PERFM_NVPROF("code", CodeAlloc::size(codeList));

#ifdef NANOJIT_IA32
        NanoAssertMsgf(_fpuStkDepth == 0,"_fpuStkDepth %d\n",_fpuStkDepth);
#endif

        debug_only( pageValidate(); )
        NanoAssert(_branchStateMap.isEmpty());
    }

    void Assembler::releaseRegisters()
    {
        for (Register r = FirstReg; r <= LastReg; r = nextreg(r))
        {
            LIns *ins = _allocator.getActive(r);
            if (ins) {
                // Clear reg allocation, preserve stack allocation.
                _allocator.retire(r);
                NanoAssert(r == ins->getReg());
                ins->setReg(UnknownReg);

                if (!ins->getArIndex()) {
                    ins->markAsClear();
                }
            }
        }
    }

#ifdef PERFM
#define countlir_live() _nvprof("lir-live",1)
#define countlir_ret() _nvprof("lir-ret",1)
#define countlir_alloc() _nvprof("lir-alloc",1)
#define countlir_var() _nvprof("lir-var",1)
#define countlir_use() _nvprof("lir-use",1)
#define countlir_def() _nvprof("lir-def",1)
#define countlir_imm() _nvprof("lir-imm",1)
#define countlir_param() _nvprof("lir-param",1)
#define countlir_cmov() _nvprof("lir-cmov",1)
#define countlir_ld() _nvprof("lir-ld",1)
#define countlir_ldq() _nvprof("lir-ldq",1)
#define countlir_alu() _nvprof("lir-alu",1)
#define countlir_qjoin() _nvprof("lir-qjoin",1)
#define countlir_qlo() _nvprof("lir-qlo",1)
#define countlir_qhi() _nvprof("lir-qhi",1)
#define countlir_fpu() _nvprof("lir-fpu",1)
#define countlir_st() _nvprof("lir-st",1)
#define countlir_stq() _nvprof("lir-stq",1)
#define countlir_jmp() _nvprof("lir-jmp",1)
#define countlir_jcc() _nvprof("lir-jcc",1)
#define countlir_label() _nvprof("lir-label",1)
#define countlir_xcc() _nvprof("lir-xcc",1)
#define countlir_x() _nvprof("lir-x",1)
#define countlir_call() _nvprof("lir-call",1)
#define countlir_jtbl() _nvprof("lir-jtbl",1)
#else
#define countlir_live()
#define countlir_ret()
#define countlir_alloc()
#define countlir_var()
#define countlir_use()
#define countlir_def()
#define countlir_imm()
#define countlir_param()
#define countlir_cmov()
#define countlir_ld()
#define countlir_ldq()
#define countlir_alu()
#define countlir_qjoin()
#define countlir_qlo()
#define countlir_qhi()
#define countlir_fpu()
#define countlir_st()
#define countlir_stq()
#define countlir_jmp()
#define countlir_jcc()
#define countlir_label()
#define countlir_xcc()
#define countlir_x()
#define countlir_call()
#define countlir_jtbl()
#endif

    void Assembler::gen(LirFilter* reader)
    {
        NanoAssert(_thisfrag->nStaticExits == 0);

        // trace must end with LIR_x, LIR_[f]ret, LIR_xtbl, or LIR_[f]live
        NanoAssert(reader->pos()->isop(LIR_x) ||
                   reader->pos()->isop(LIR_ret) ||
                   reader->pos()->isop(LIR_fret) ||
                   reader->pos()->isop(LIR_xtbl) ||
                   reader->pos()->isop(LIR_flive) ||
                   reader->pos()->isop(LIR_live));

        InsList pending_lives(alloc);

        for (LInsp ins = reader->read(); !ins->isop(LIR_start) && !error();
                                         ins = reader->read())
        {
            /* What's going on here: we're visiting all the LIR instructions
               in the buffer, working strictly backwards in buffer-order, and
               generating machine instructions for them as we go.

               For each LIns, we first determine whether it's actually
               necessary, and if not skip it.  Otherwise we generate code for
               it.  There are two kinds of "necessary" instructions:

               - "Statement" instructions, which have side effects.  Anything
                 that could change control flow or the state of memory.

               - "Value" or "expression" instructions, which compute a value
                 based only on the operands to the instruction (and, in the
                 case of loads, the state of memory).  Because we visit
                 instructions in reverse order, if some previously visited
                 instruction uses the value computed by this instruction, then
                 this instruction will already have a register assigned to
                 hold that value.  Hence we can consult the instruction to
                 detect whether its value is in fact used (i.e. not dead).

              Note that the backwards code traversal can make register
              allocation confusing.  (For example, we restore a value before
              we spill it!)  In particular, words like "before" and "after"
              must be used very carefully -- their meaning at regalloc-time is
              opposite to their meaning at run-time.  We use the term
              "pre-state" to refer to the register allocation state that
              occurs prior to an instruction's execution, and "post-state" to
              refer to the state that occurs after an instruction's execution,
              e.g.:

                pre-state:     ebx(ins)
                instruction:   mov eax, ebx     // mov dst, src
                post-state:    eax(ins)

              At run-time, the instruction updates the pre-state into the
              post-state (and these states are the real machine's states).
              But when allocating registers, because we go backwards, the
              pre-state is constructed from the post-state (and these states
              are those stored in RegAlloc).
            */
            bool required = ins->isStmt() || ins->isUsed();
            if (!required)
                continue;

#ifdef NJ_VERBOSE
            // Output the register post-state and/or activation post-state.
            // Because asm output comes in reverse order, doing it now means
            // it is printed after the LIR and asm, exactly when the
            // post-state should be shown.
            if ((_logc->lcbits & LC_Assembly) && (_logc->lcbits & LC_Activation))
                printActivationState();
            if ((_logc->lcbits & LC_Assembly) && (_logc->lcbits & LC_RegAlloc))
                printRegState();
#endif

            LOpcode op = ins->opcode();
            switch(op)
            {
                default:
                    NanoAssertMsgf(false, "unsupported LIR instruction: %d (~0x40: %d)\n", op, op&~LIR64);
                    break;

                case LIR_regfence:
                    evictAllActiveRegs();
                    break;

                case LIR_flive:
                case LIR_live: {
                    countlir_live();
                    LInsp op1 = ins->oprnd1();
                    // alloca's are meant to live until the point of the LIR_live instruction, marking
                    // other expressions as live ensures that they remain so at loop bottoms.
                    // alloca areas require special treatment because they are accessed indirectly and
                    // the indirect accesses are invisible to the assembler, other than via LIR_live.
                    // other expression results are only accessed directly in ways that are visible to
                    // the assembler, so extending those expression's lifetimes past the last loop edge
                    // isn't necessary.
                    if (op1->isop(LIR_alloc)) {
                        findMemFor(op1);
                    } else {
                        pending_lives.add(ins);
                    }
                    break;
                }

                case LIR_fret:
                case LIR_ret:  {
                    countlir_ret();
                    asm_ret(ins);
                    break;
                }

                // allocate some stack space.  the value of this instruction
                // is the address of the stack space.
                case LIR_alloc: {
                    countlir_alloc();
                    NanoAssert(ins->getArIndex() != 0);
                    Register r = ins->getReg();
                    if (isKnownReg(r)) {
                        _allocator.retire(r);
                        ins->setReg(UnknownReg);
                        asm_restore(ins, r);
                    }
                    freeRsrcOf(ins, 0);
                    break;
                }
                case LIR_int:
                {
                    countlir_imm();
                    asm_int(ins);
                    break;
                }
                case LIR_float:
                case LIR_quad:
                {
                    countlir_imm();
                    asm_quad(ins);
                    break;
                }
#if !defined NANOJIT_64BIT
                case LIR_callh:
                {
                    // return result of quad-call in register
                    prepResultReg(ins, rmask(retRegs[1]));
                    // if hi half was used, we must use the call to ensure it happens
                    findSpecificRegFor(ins->oprnd1(), retRegs[0]);
                    break;
                }
#endif
                case LIR_param:
                {
                    countlir_param();
                    asm_param(ins);
                    break;
                }
                case LIR_qlo:
                {
                    countlir_qlo();
                    asm_qlo(ins);
                    break;
                }
                case LIR_qhi:
                {
                    countlir_qhi();
                    asm_qhi(ins);
                    break;
                }
                case LIR_qcmov:
                case LIR_cmov:
                {
                    countlir_cmov();
                    asm_cmov(ins);
                    break;
                }
                case LIR_ld:
                case LIR_ldc:
                case LIR_ldcb:
                case LIR_ldcs:
                {
                    countlir_ld();
                    asm_ld(ins);
                    break;
                }
                case LIR_ldq:
                case LIR_ldqc:
                {
                    countlir_ldq();
                    asm_load64(ins);
                    break;
                }
                case LIR_neg:
                case LIR_not:
                {
                    countlir_alu();
                    asm_neg_not(ins);
                    break;
                }
                case LIR_qjoin:
                {
                    countlir_qjoin();
                    asm_qjoin(ins);
                    break;
                }

#if defined NANOJIT_64BIT
                case LIR_qiadd:
                case LIR_qiand:
                case LIR_qilsh:
                case LIR_qursh:
                case LIR_qirsh:
                case LIR_qior:
                case LIR_qaddp:
                case LIR_qxor:
                {
                    asm_qbinop(ins);
                    break;
                }
#endif

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
                case LIR_div:
                case LIR_mod:
                {
                    countlir_alu();
                    asm_arith(ins);
                    break;
                }
                case LIR_fneg:
                {
                    countlir_fpu();
                    asm_fneg(ins);
                    break;
                }
                case LIR_fadd:
                case LIR_fsub:
                case LIR_fmul:
                case LIR_fdiv:
                {
                    countlir_fpu();
                    asm_fop(ins);
                    break;
                }
                case LIR_i2f:
                {
                    countlir_fpu();
                    asm_i2f(ins);
                    break;
                }
                case LIR_u2f:
                {
                    countlir_fpu();
                    asm_u2f(ins);
                    break;
                }
                case LIR_i2q:
                case LIR_u2q:
                {
                    countlir_alu();
                    asm_promote(ins);
                    break;
                }
                case LIR_sti:
                {
                    countlir_st();
                    asm_store32(ins->oprnd1(), ins->disp(), ins->oprnd2());
                    break;
                }
                case LIR_stqi:
                {
                    countlir_stq();
                    LIns* value = ins->oprnd1();
                    LIns* base = ins->oprnd2();
                    int dr = ins->disp();
                    if (value->isop(LIR_qjoin))
                    {
                        // this is correct for little-endian only
                        asm_store32(value->oprnd1(), dr, base);
                        asm_store32(value->oprnd2(), dr+4, base);
                    }
                    else
                    {
                        asm_store64(value, dr, base);
                    }
                    break;
                }

                case LIR_j:
                {
                    countlir_jmp();
                    LInsp to = ins->getTarget();
                    LabelState *label = _labels.get(to);
                    // the jump is always taken so whatever register state we
                    // have from downstream code, is irrelevant to code before
                    // this jump.  so clear it out.  we will pick up register
                    // state from the jump target, if we have seen that label.
                    releaseRegisters();
                    if (label && label->addr) {
                        // forward jump - pick up register state from target.
                        unionRegisterState(label->regs);
                        JMP(label->addr);
                    }
                    else {
                        // backwards jump
                        handleLoopCarriedExprs(pending_lives);
                        if (!label) {
                            // save empty register state at loop header
                            _labels.add(to, 0, _allocator);
                        }
                        else {
                            intersectRegisterState(label->regs);
                        }
                        JMP(0);
                        _patches.put(_nIns, to);
                    }
                    break;
                }

                case LIR_jt:
                case LIR_jf:
                {
                    countlir_jcc();
                    LInsp to = ins->getTarget();
                    LIns* cond = ins->oprnd1();
                    LabelState *label = _labels.get(to);
                    if (label && label->addr) {
                        // forward jump to known label.  need to merge with label's register state.
                        unionRegisterState(label->regs);
                        asm_branch(op == LIR_jf, cond, label->addr);
                    }
                    else {
                        // back edge.
                        handleLoopCarriedExprs(pending_lives);
                        if (!label) {
                            // evict all registers, most conservative approach.
                            evictAllActiveRegs();
                            _labels.add(to, 0, _allocator);
                        }
                        else {
                            // evict all registers, most conservative approach.
                            intersectRegisterState(label->regs);
                        }
                        NIns *branch = asm_branch(op == LIR_jf, cond, 0);
                        _patches.put(branch,to);
                    }
                    break;
                }

                #if NJ_JTBL_SUPPORTED
                case LIR_jtbl:
                {
                    countlir_jtbl();
                    // Multiway jump can contain both forward and backward jumps.
                    // Out of range indices aren't allowed or checked.
                    // Code after this jtbl instruction is unreachable.
                    releaseRegisters();
                    AvmAssert(_allocator.countActive() == 0);

                    uint32_t count = ins->getTableSize();
                    bool has_back_edges = false;

                    // Merge the register states of labels we have already seen.
                    for (uint32_t i = count; i-- > 0;) {
                        LIns* to = ins->getTarget(i);
                        LabelState *lstate = _labels.get(to);
                        if (lstate) {
                            unionRegisterState(lstate->regs);
                            asm_output("   %u: [&%s]", i, _thisfrag->lirbuf->names->formatRef(to));
                        } else {
                            has_back_edges = true;
                        }
                    }
                    asm_output("forward edges");

                    // In a multi-way jump, the register allocator has no ability to deal
                    // with two existing edges that have conflicting register assignments, unlike
                    // a conditional branch where code can be inserted on the fall-through path
                    // to reconcile registers.  So, frontends *must* insert LIR_regfence at labels of
                    // forward jtbl jumps.  Check here to make sure no registers were picked up from
                    // any forward edges.
                    AvmAssert(_allocator.countActive() == 0);

                    if (has_back_edges) {
                        handleLoopCarriedExprs(pending_lives);
                        // save merged (empty) register state at target labels we haven't seen yet
                        for (uint32_t i = count; i-- > 0;) {
                            LIns* to = ins->getTarget(i);
                            LabelState *lstate = _labels.get(to);
                            if (!lstate) {
                                _labels.add(to, 0, _allocator);
                                asm_output("   %u: [&%s]", i, _thisfrag->lirbuf->names->formatRef(to));
                            }
                        }
                        asm_output("backward edges");
                    }

                    // Emit the jump instruction, which allocates 1 register for the jump index.
                    NIns** native_table = new (_dataAlloc) NIns*[count];
                    asm_output("[%p]:", (void*)native_table);
                    _patches.put((NIns*)native_table, ins);
                    asm_jtbl(ins, native_table);
                    break;
                }
                #endif

                case LIR_label:
                {
                    countlir_label();
                    LabelState *label = _labels.get(ins);
                    // add profiling inc, if necessary.
                    verbose_only( if (_logc->lcbits & LC_FragProfile) {
                        if (ins == _thisfrag->loopLabel)
                            asm_inc_m32(& _thisfrag->profCount);
                    })
                    if (!label) {
                        // label seen first, normal target of forward jump, save addr & allocator
                        _labels.add(ins, _nIns, _allocator);
                    }
                    else {
                        // we're at the top of a loop
                        NanoAssert(label->addr == 0);
                        //evictAllActiveRegs();
                        intersectRegisterState(label->regs);
                        label->addr = _nIns;
                    }
                    verbose_only( if (_logc->lcbits & LC_Assembly) { 
                        outputAddr=true; asm_output("[%s]", 
                        _thisfrag->lirbuf->names->formatRef(ins)); 
                    })
                    break;
                }
                case LIR_xbarrier: {
                    break;
                }
#ifdef NANOJIT_IA32
                case LIR_xtbl: {
                    NIns* exit = asm_exit(ins); // does intersectRegisterState()
                    asm_switch(ins, exit);
                    break;
                }
#else
                 case LIR_xtbl:
                    NanoAssertMsg(0, "Not supported for this architecture");
                    break;
#endif
                case LIR_xt:
                case LIR_xf:
                {
                    verbose_only( _thisfrag->nStaticExits++; )
                    countlir_xcc();
                    // we only support cmp with guard right now, also assume it is 'close' and only emit the branch
                    NIns* exit = asm_exit(ins); // does intersectRegisterState()
                    LIns* cond = ins->oprnd1();
                    asm_branch(op == LIR_xf, cond, exit);
                    break;
                }
                case LIR_x:
                {
                    verbose_only( _thisfrag->nStaticExits++; )
                    countlir_x();
                    // generate the side exit branch on the main trace.
                    NIns *exit = asm_exit(ins);
                    JMP( exit );
                    break;
                }

                case LIR_feq:
                case LIR_fle:
                case LIR_flt:
                case LIR_fgt:
                case LIR_fge:
                {
                    countlir_fpu();
                    asm_fcond(ins);
                    break;
                }
                case LIR_eq:
                case LIR_ov:
                case LIR_le:
                case LIR_lt:
                case LIR_gt:
                case LIR_ge:
                case LIR_ult:
                case LIR_ule:
                case LIR_ugt:
                case LIR_uge:
#ifdef NANOJIT_64BIT
                case LIR_qeq:
                case LIR_qle:
                case LIR_qlt:
                case LIR_qgt:
                case LIR_qge:
                case LIR_qult:
                case LIR_qule:
                case LIR_qugt:
                case LIR_quge:
#endif
                {
                    countlir_alu();
                    asm_cond(ins);
                    break;
                }

                case LIR_fcall:
            #ifdef NANOJIT_64BIT
                case LIR_qcall:
            #endif
                case LIR_icall:
                {
                    countlir_call();
                    Register rr = UnknownReg;
                    if (ARM_VFP && op == LIR_fcall)
                    {
                        // fcall
                        rr = asm_prep_fcall(ins);
                    }
                    else
                    {
                        rr = retRegs[0];
                        prepResultReg(ins, rmask(rr));
                    }

                    // do this after we've handled the call result, so we dont
                    // force the call result to be spilled unnecessarily.

                    evictScratchRegs();

                    asm_call(ins);
                    break;
                }

                #ifdef VTUNE
                case LIR_file:
                {
                    // we traverse backwards so we are now hitting the file
                    // that is associated with a bunch of LIR_lines we already have seen
                    uintptr_t currentFile = ins->oprnd1()->imm32();
                    cgen->jitFilenameUpdate(currentFile);
                    break;
                }
                case LIR_line:
                {
                    // add a new table entry, we don't yet knwo which file it belongs
                    // to so we need to add it to the update table too
                    // note the alloc, actual act is delayed; see above
                    uint32_t currentLine = (uint32_t) ins->oprnd1()->imm32();
                    cgen->jitLineNumUpdate(currentLine);
                    cgen->jitAddRecord((uintptr_t)_nIns, 0, currentLine, true);
                    break;
                }
                #endif // VTUNE
            }

#ifdef NJ_VERBOSE
            // We have to do final LIR printing inside this loop.  If we do it
            // before this loop, we we end up printing a lot of dead LIR
            // instructions.
            //
            // We print the LIns after generating the code.  This ensures that
            // the LIns will appear in debug output *before* the generated
            // code, because Assembler::outputf() prints everything in reverse.
            //
            // Note that some live LIR instructions won't be printed.  Eg. an
            // immediate won't be printed unless it is explicitly loaded into
            // a register (as opposed to being incorporated into an immediate
            // field in another machine instruction).
            //
            if (_logc->lcbits & LC_Assembly) {
                LirNameMap* names = _thisfrag->lirbuf->names;
                outputf("    %s", names->formatIns(ins));
                if (ins->isGuard() && ins->oprnd1()) {
                    // Special case: code is generated for guard conditions at
                    // the same time that code is generated for the guard
                    // itself.  If the condition is only used by the guard, we
                    // must print it now otherwise it won't get printed.  So
                    // we do print it now, with an explanatory comment.  If
                    // the condition *is* used again we'll end up printing it
                    // twice, but that's ok.
                    outputf("    %s       # codegen'd with the %s",
                            names->formatIns(ins->oprnd1()), lirNames[op]);

                } else if (ins->isop(LIR_cmov) || ins->isop(LIR_qcmov)) {
                    // Likewise for cmov conditions.
                    outputf("    %s       # codegen'd with the %s",
                            names->formatIns(ins->oprnd1()), lirNames[op]);

                } else if (ins->isop(LIR_mod)) {
                    // There's a similar case when a div feeds into a mod.
                    outputf("    %s       # codegen'd with the mod",
                            names->formatIns(ins->oprnd1()));
                }
            }
#endif

            if (error())
                return;

        #ifdef VTUNE
            cgen->jitCodePosUpdate((uintptr_t)_nIns);
        #endif

            // check that all is well (don't check in exit paths since its more complicated)
            debug_only( pageValidate(); )
            debug_only( resourceConsistencyCheck();  )
        }
    }

    /*
     * Write a jump table for the given SwitchInfo and store the table
     * address in the SwitchInfo. Every entry will initially point to
     * target.
     */
    void Assembler::emitJumpTable(SwitchInfo* si, NIns* target)
    {
        si->table = (NIns **) alloc.alloc(si->count * sizeof(NIns*));
        for (uint32_t i = 0; i < si->count; ++i)
            si->table[i] = target;
    }

    void Assembler::assignSavedRegs()
    {
        // restore saved regs
        releaseRegisters();
        LirBuffer *b = _thisfrag->lirbuf;
        for (int i=0, n = NumSavedRegs; i < n; i++) {
            LIns *p = b->savedRegs[i];
            if (p)
                findSpecificRegForUnallocated(p, savedRegs[p->paramArg()]);
        }
    }

    void Assembler::reserveSavedRegs()
    {
        LirBuffer *b = _thisfrag->lirbuf;
        for (int i=0, n = NumSavedRegs; i < n; i++) {
            LIns *p = b->savedRegs[i];
            if (p)
                findMemFor(p);
        }
    }

    // restore parameter registers
    void Assembler::assignParamRegs()
    {
        LInsp state = _thisfrag->lirbuf->state;
        if (state)
            findSpecificRegForUnallocated(state, argRegs[state->paramArg()]);
        LInsp param1 = _thisfrag->lirbuf->param1;
        if (param1)
            findSpecificRegForUnallocated(param1, argRegs[param1->paramArg()]);
    }

    void Assembler::handleLoopCarriedExprs(InsList& pending_lives)
    {
        // ensure that exprs spanning the loop are marked live at the end of the loop
        reserveSavedRegs();
        for (Seq<LIns*> *p = pending_lives.get(); p != NULL; p = p->tail) {
            LIns *i = p->head;
            NanoAssert(i->isop(LIR_live) || i->isop(LIR_flive));
            LIns *op1 = i->oprnd1();
            // must findMemFor even if we're going to findRegFor; loop-carried
            // operands may spill on another edge, and we need them to always
            // spill to the same place.
            findMemFor(op1);
            if (! (op1->isconst() || op1->isconstf() || op1->isconstq()))
                findRegFor(op1, i->isop(LIR_flive) ? FpRegs : GpRegs);
        }

        // clear this list since we have now dealt with those lifetimes.  extending
        // their lifetimes again later (earlier in the code) serves no purpose.
        pending_lives.clear();
    }

    void Assembler::arFree(uint32_t idx)
    {
        AR &ar = _activation;
        LIns *i = ar.entry[idx];
        NanoAssert(i != 0);
        do {
            ar.entry[idx] = 0;
            idx--;
        } while (ar.entry[idx] == i);
    }

#ifdef NJ_VERBOSE
    void Assembler::printRegState()
    {
        char* s = &outline[0];
        VMPI_memset(s, ' ', 26);  s[26] = '\0';
        s += VMPI_strlen(s);
        VMPI_sprintf(s, "RR");
        s += VMPI_strlen(s);

        for (Register r = FirstReg; r <= LastReg; r = nextreg(r)) {
            LIns *ins = _allocator.getActive(r);
            if (ins) {
                NanoAssertMsg(!_allocator.isFree(r),
                              "Coding error; register is both free and active! " );
                const char* n = _thisfrag->lirbuf->names->formatRef(ins);

                if (ins->isop(LIR_param) && ins->paramKind()==1 &&
                    r == Assembler::savedRegs[ins->paramArg()])
                {
                    // dont print callee-saved regs that arent used
                    continue;
                }

                const char* rname = ins->isQuad() ? fpn(r) : gpn(r);
                VMPI_sprintf(s, " %s(%s)", rname, n);
                s += VMPI_strlen(s);
            }
        }
        output();
    }

    void Assembler::printActivationState()
    {
        char* s = &outline[0];
        VMPI_memset(s, ' ', 26);  s[26] = '\0';
        s += VMPI_strlen(s);
        VMPI_sprintf(s, "AR");
        s += VMPI_strlen(s);

        int32_t max = _activation.tos < NJ_MAX_STACK_ENTRY ? _activation.tos : NJ_MAX_STACK_ENTRY;
        for (int32_t i = _activation.lowwatermark; i < max; i++) {
            LIns *ins = _activation.entry[i];
            if (ins) {
                const char* n = _thisfrag->lirbuf->names->formatRef(ins);
                if (ins->isop(LIR_alloc)) {
                    int32_t count = ins->size()>>2;
                    VMPI_sprintf(s," %d-%d(%s)", 4*i, 4*(i+count-1), n);
                    count += i-1;
                    while (i < count) {
                        NanoAssert(_activation.entry[i] == ins);
                        i++;
                    }
                }
                else if (ins->isQuad()) {
                    VMPI_sprintf(s," %d+(%s)", 4*i, n);
                    NanoAssert(_activation.entry[i+1] == ins);
                    i++;
                }
                else {
                    VMPI_sprintf(s," %d(%s)", 4*i, n);
                }
            }
            s += VMPI_strlen(s);
        }
        output();
    }
#endif

    bool canfit(int32_t size, int32_t loc, AR &ar) {
        for (int i=0; i < size; i++) {
            if (ar.entry[loc+stack_direction(i)])
                return false;
        }
        return true;
    }

    uint32_t Assembler::arReserve(LIns* l)
    {
        int32_t size = l->isop(LIR_alloc) ? (l->size()>>2) : l->isQuad() ? 2 : 1;
        AR &ar = _activation;
        const int32_t tos = ar.tos;
        int32_t start = ar.lowwatermark;
        int32_t i = 0;
        NanoAssert(start>0);

        if (size == 1) {
            // easy most common case -- find a hole, or make the frame bigger
            for (i=start; i < NJ_MAX_STACK_ENTRY; i++) {
                if (ar.entry[i] == 0) {
                    // found a hole
                    ar.entry[i] = l;
                    break;
                }
            }
        }
        else if (size == 2) {
            if ( (start&1)==1 ) start++;  // even 8 boundary
            for (i=start; i < NJ_MAX_STACK_ENTRY; i+=2) {
                if ( (ar.entry[i+stack_direction(1)] == 0) && (i==tos || (ar.entry[i] == 0)) ) {
                    // found 2 adjacent aligned slots
                    NanoAssert(ar.entry[i] == 0);
                    NanoAssert(ar.entry[i+stack_direction(1)] == 0);
                    ar.entry[i] = l;
                    ar.entry[i+stack_direction(1)] = l;
                    break;
                }
            }
        }
        else {
            // alloc larger block on 8byte boundary.
            if (start < size) start = size;
            if ((start&1)==1) start++;
            for (i=start; i < NJ_MAX_STACK_ENTRY; i+=2) {
                if (canfit(size, i, ar)) {
                    // place the entry in the table and mark the instruction with it
                    for (int32_t j=0; j < size; j++) {
                        NanoAssert(ar.entry[i+stack_direction(j)] == 0);
                        ar.entry[i+stack_direction(j)] = l;
                    }
                    break;
                }
            }
        }
        if (i >= (int32_t)ar.tos) {
            ar.tos = i+1;
        }
        if (tos+size >= NJ_MAX_STACK_ENTRY) {
            setError(StackFull);
        }
        return i;
    }

    /**
     * move regs around so the SavedRegs contains the highest priority regs.
     */
    void Assembler::evictScratchRegs()
    {
        // find the top GpRegs that are candidates to put in SavedRegs

        // tosave is a binary heap stored in an array.  the root is tosave[0],
        // left child is at i+1, right child is at i+2.

        Register tosave[LastReg-FirstReg+1];
        int len=0;
        RegAlloc *regs = &_allocator;
        for (Register r = FirstReg; r <= LastReg; r = nextreg(r)) {
            if (rmask(r) & GpRegs) {
                LIns *i = regs->getActive(r);
                if (i) {
                    if (canRemat(i)) {
                        evict(r, i);
                    }
                    else {
                        int32_t pri = regs->getPriority(r);
                        // add to heap by adding to end and bubbling up
                        int j = len++;
                        while (j > 0 && pri > regs->getPriority(tosave[j/2])) {
                            tosave[j] = tosave[j/2];
                            j /= 2;
                        }
                        NanoAssert(size_t(j) < sizeof(tosave)/sizeof(tosave[0]));
                        tosave[j] = r;
                    }
                }
            }
        }

        // now primap has the live exprs in priority order.
        // allocate each of the top priority exprs to a SavedReg

        RegisterMask allow = SavedRegs;
        while (allow && len > 0) {
            // get the highest priority var
            Register hi = tosave[0];
            if (!(rmask(hi) & SavedRegs)) {
                LIns *i = regs->getActive(hi);
                Register r = findRegFor(i, allow);
                allow &= ~rmask(r);
            }
            else {
                // hi is already in a saved reg, leave it alone.
                allow &= ~rmask(hi);
            }

            // remove from heap by replacing root with end element and bubbling down.
            if (allow && --len > 0) {
                Register last = tosave[len];
                int j = 0;
                while (j+1 < len) {
                    int child = j+1;
                    if (j+2 < len && regs->getPriority(tosave[j+2]) > regs->getPriority(tosave[j+1]))
                        child++;
                    if (regs->getPriority(last) > regs->getPriority(tosave[child]))
                        break;
                    tosave[j] = tosave[child];
                    j = child;
                }
                tosave[j] = last;
            }
        }

        // now evict everything else.
        evictSomeActiveRegs(~SavedRegs);
    }

    void Assembler::evictAllActiveRegs()
    {
        // generate code to restore callee saved registers
        // @todo speed this up
        for (Register r = FirstReg; r <= LastReg; r = nextreg(r)) {
            evictIfActive(r);
        }
    }

    void Assembler::evictSomeActiveRegs(RegisterMask regs)
    {
        // generate code to restore callee saved registers
        // @todo speed this up
        for (Register r = FirstReg; r <= LastReg; r = nextreg(r)) {
            if ((rmask(r) & regs)) {
                evictIfActive(r);
            }
        }
    }

    /**
     * Merge the current state of the registers with a previously stored version
     * current == saved    skip
     * current & saved     evict current, keep saved
     * current & !saved    evict current  (unionRegisterState would keep)
     * !current & saved    keep saved
     */
    void Assembler::intersectRegisterState(RegAlloc& saved)
    {
        // evictions and pops first
        RegisterMask skip = 0;
        verbose_only(bool shouldMention=false; )
        // The obvious thing to do here is to iterate from FirstReg to LastReg.
        // viz: for (Register r=FirstReg; r <= LastReg; r = nextreg(r)) ...
        // However, on ARM that causes lower-numbered integer registers
        // to be be saved at higher addresses, which inhibits the formation
        // of load/store multiple instructions.  Hence iterate the loop the
        // other way.  The "r <= LastReg" guards against wraparound in
        // the case where Register is treated as unsigned and FirstReg is zero.
        for (Register r=LastReg; r >= FirstReg && r <= LastReg;
                                 r = prevreg(r))
        {
            LIns * curins = _allocator.getActive(r);
            LIns * savedins = saved.getActive(r);
            if (curins == savedins)
            {
                //verbose_only( if (curins) verbose_outputf("                                              skip %s", regNames[r]); )
                skip |= rmask(r);
            }
            else
            {
                if (curins) {
                    //_nvprof("intersect-evict",1);
                    verbose_only( shouldMention=true; )
                    evict(r, curins);
                }

                #ifdef NANOJIT_IA32
                if (savedins && (rmask(r) & x87Regs)) {
                    verbose_only( shouldMention=true; )
                    FSTP(r);
                }
                #endif
            }
        }
        assignSaved(saved, skip);
        verbose_only(
            if (shouldMention)
                verbose_outputf("## merging registers (intersect) "
                                "with existing edge");
        )
    }

    /**
     * Merge the current state of the registers with a previously stored version.
     *
     * current == saved    skip
     * current & saved     evict current, keep saved
     * current & !saved    keep current (intersectRegisterState would evict)
     * !current & saved    keep saved
     */
    void Assembler::unionRegisterState(RegAlloc& saved)
    {
        // evictions and pops first
        verbose_only(bool shouldMention=false; )
        RegisterMask skip = 0;
        for (Register r=FirstReg; r <= LastReg; r = nextreg(r))
        {
            LIns * curins = _allocator.getActive(r);
            LIns * savedins = saved.getActive(r);
            if (curins == savedins)
            {
                //verbose_only( if (curins) verbose_outputf("                                              skip %s", regNames[r]); )
                skip |= rmask(r);
            }
            else
            {
                if (curins && savedins) {
                    //_nvprof("union-evict",1);
                    verbose_only( shouldMention=true; )
                    evict(r, curins);
                }

                #ifdef NANOJIT_IA32
                if (rmask(r) & x87Regs) {
                    if (savedins) {
                        FSTP(r);
                    }
                    else {
                        // saved state did not have fpu reg allocated,
                        // so we must evict here to keep x87 stack balanced.
                        evictIfActive(r);
                    }
                    verbose_only( shouldMention=true; )
                }
                #endif
            }
        }
        assignSaved(saved, skip);
        verbose_only( if (shouldMention) verbose_outputf("                                              merging registers (union) with existing edge");  )
    }

    void Assembler::assignSaved(RegAlloc &saved, RegisterMask skip)
    {
        // now reassign mainline registers
        for (Register r=FirstReg; r <= LastReg; r = nextreg(r))
        {
            LIns *i = saved.getActive(r);
            if (i && !(skip&rmask(r)))
                findSpecificRegFor(i, r);
        }
    }

    // Scan table for instruction with the lowest priority, meaning it is used
    // furthest in the future.
    LIns* Assembler::findVictim(RegisterMask allow)
    {
        NanoAssert(allow != 0);
        LIns *i, *a=0;
        int allow_pri = 0x7fffffff;
        for (Register r=FirstReg; r <= LastReg; r = nextreg(r))
        {
            if ((allow & rmask(r)) && (i = _allocator.getActive(r)) != 0)
            {
                int pri = canRemat(i) ? 0 : _allocator.getPriority(r);
                if (!a || pri < allow_pri) {
                    a = i;
                    allow_pri = pri;
                }
            }
        }
        NanoAssert(a != 0);
        return a;
    }

#ifdef NJ_VERBOSE
    char Assembler::outline[8192];
    char Assembler::outlineEOL[512];

    void Assembler::output()
    {
        // The +1 is for the terminating NUL char.
        VMPI_strncat(outline, outlineEOL, sizeof(outline)-(strlen(outline)+1));

        if (_outputCache) {
            char* str = new (alloc) char[VMPI_strlen(outline)+1];
            VMPI_strcpy(str, outline);
            _outputCache->insert(str);
        } else {
            _logc->printf("%s\n", outline);
        }

        outline[0] = '\0';
        outlineEOL[0] = '\0';
    }

    void Assembler::outputf(const char* format, ...)
    {
        va_list args;
        va_start(args, format);

        outline[0] = '\0';
        vsprintf(outline, format, args);
        output();
    }

    void Assembler::setOutputForEOL(const char* format, ...)
    {
        va_list args;
        va_start(args, format);

        outlineEOL[0] = '\0';
        vsprintf(outlineEOL, format, args);
    }
#endif // NJ_VERBOSE

    uint32_t CallInfo::_count_args(uint32_t mask) const
    {
        uint32_t argc = 0;
        uint32_t argt = _argtypes;
        for (uint32_t i = 0; i < MAXARGS; ++i) {
            argt >>= ARGSIZE_SHIFT;
            if (!argt)
                break;
            argc += (argt & mask) != 0;
        }
        return argc;
    }

    uint32_t CallInfo::get_sizes(ArgSize* sizes) const
    {
        uint32_t argt = _argtypes;
        uint32_t argc = 0;
        for (uint32_t i = 0; i < MAXARGS; i++) {
            argt >>= ARGSIZE_SHIFT;
            ArgSize a = ArgSize(argt & ARGSIZE_MASK_ANY);
            if (a != ARGSIZE_NONE) {
                sizes[argc++] = a;
            } else {
                break;
            }
        }
        return argc;
    }

    void LabelStateMap::add(LIns *label, NIns *addr, RegAlloc &regs) {
        LabelState *st = new (alloc) LabelState(addr, regs);
        labels.put(label, st);
    }

    LabelState* LabelStateMap::get(LIns *label) {
        return labels.get(label);
    }
}
#endif /* FEATURE_NANOJIT */
