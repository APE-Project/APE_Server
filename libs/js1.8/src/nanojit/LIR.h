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

#ifndef __nanojit_LIR__
#define __nanojit_LIR__

/**
 * Fundamentally, the arguments to the various operands can be grouped along
 * two dimensions.  One dimension is size: can the arguments fit into a 32-bit
 * register, or not?  The other dimension is whether the argument is an integer
 * (including pointers) or a floating-point value.  In all comments below,
 * "integer" means integer of any size, including 64-bit, unless otherwise
 * specified.  All floating-point values are always 64-bit.  Below, "quad" is
 * used for a 64-bit value that might be either integer or floating-point.
 */
namespace nanojit
{
    enum LOpcode
#if defined(_MSC_VER) && _MSC_VER >= 1400
#pragma warning(disable:4480) // nonstandard extension used: specifying underlying type for enum
          : unsigned
#endif
    {
        // flags; upper bits reserved
        LIR64    = 0x40,            // result is double or quad

#define OPDEF(op, number, args, repkind) \
        LIR_##op = (number),
#define OPDEF64(op, number, args, repkind) \
        LIR_##op = ((number) | LIR64),
#include "LIRopcode.tbl"
        LIR_sentinel,
#undef OPDEF
#undef OPDEF64

#ifdef NANOJIT_64BIT
#  define PTR_SIZE(a,b)  b
#else
#  define PTR_SIZE(a,b)  a
#endif

        // pointer op aliases
        LIR_ldp     = PTR_SIZE(LIR_ld,     LIR_ldq),
        LIR_ldcp    = PTR_SIZE(LIR_ldc,    LIR_ldqc),
        LIR_stpi    = PTR_SIZE(LIR_sti,    LIR_stqi),
        LIR_piadd   = PTR_SIZE(LIR_add,    LIR_qiadd),
        LIR_piand   = PTR_SIZE(LIR_and,    LIR_qiand),
        LIR_pilsh   = PTR_SIZE(LIR_lsh,    LIR_qilsh),
        LIR_pirsh   = PTR_SIZE(LIR_rsh,    LIR_qirsh),
        LIR_pursh   = PTR_SIZE(LIR_ush,    LIR_qursh),
        LIR_pcmov   = PTR_SIZE(LIR_cmov,   LIR_qcmov),
        LIR_pior    = PTR_SIZE(LIR_or,     LIR_qior),
        LIR_pxor    = PTR_SIZE(LIR_xor,    LIR_qxor),
        LIR_addp    = PTR_SIZE(LIR_iaddp,  LIR_qaddp),
        LIR_peq     = PTR_SIZE(LIR_eq,     LIR_qeq),
        LIR_plt     = PTR_SIZE(LIR_lt,     LIR_qlt),
        LIR_pgt     = PTR_SIZE(LIR_gt,     LIR_qgt),
        LIR_ple     = PTR_SIZE(LIR_le,     LIR_qle),
        LIR_pge     = PTR_SIZE(LIR_ge,     LIR_qge),
        LIR_pult    = PTR_SIZE(LIR_ult,    LIR_qult),
        LIR_pugt    = PTR_SIZE(LIR_ugt,    LIR_qugt),
        LIR_pule    = PTR_SIZE(LIR_ule,    LIR_qule),
        LIR_puge    = PTR_SIZE(LIR_uge,    LIR_quge),
        LIR_alloc   = PTR_SIZE(LIR_ialloc, LIR_qalloc),
        LIR_pcall   = PTR_SIZE(LIR_icall,  LIR_qcall),
        LIR_param   = PTR_SIZE(LIR_iparam, LIR_qparam)
    };

    struct GuardRecord;
    struct SideExit;

    enum AbiKind {
        ABI_FASTCALL,
        ABI_THISCALL,
        ABI_STDCALL,
        ABI_CDECL
    };

    enum ArgSize {
        ARGSIZE_NONE = 0,
        ARGSIZE_F = 1,      // double (64bit)
        ARGSIZE_I = 2,      // int32_t
        ARGSIZE_Q = 3,      // uint64_t
        ARGSIZE_U = 6,      // uint32_t
        ARGSIZE_MASK_ANY = 7,
        ARGSIZE_MASK_INT = 2,
        ARGSIZE_SHIFT = 3,

        // aliases
        ARGSIZE_P = PTR_SIZE(ARGSIZE_I, ARGSIZE_Q), // pointer
        ARGSIZE_LO = ARGSIZE_I, // int32_t
        ARGSIZE_B = ARGSIZE_I, // bool
        ARGSIZE_V = ARGSIZE_NONE  // void
    };

    enum IndirectCall {
        CALL_INDIRECT = 0
    };

    struct CallInfo
    {
        uintptr_t   _address;
        uint32_t    _argtypes:27;    // 9 3-bit fields indicating arg type, by ARGSIZE above (including ret type): a1 a2 a3 a4 a5 ret
        uint8_t     _cse:1;          // true if no side effects
        uint8_t     _fold:1;         // true if no side effects
        AbiKind     _abi:3;
        verbose_only ( const char* _name; )

        uint32_t _count_args(uint32_t mask) const;
        uint32_t get_sizes(ArgSize*) const;

        inline bool isIndirect() const {
            return _address < 256;
        }
        inline uint32_t count_args() const {
            return _count_args(ARGSIZE_MASK_ANY);
        }
        inline uint32_t count_iargs() const {
            return _count_args(ARGSIZE_MASK_INT);
        }
        // fargs = args - iargs
    };

    /*
     * Record for extra data used to compile switches as jump tables.
     */
    struct SwitchInfo
    {
        NIns**      table;       // Jump table; a jump address is NIns*
        uint32_t    count;       // Number of table entries
        // Index value at last execution of the switch. The index value
        // is the offset into the jump table. Thus it is computed as
        // (switch expression) - (lowest case value).
        uint32_t    index;
    };

    inline bool isCseOpcode(LOpcode op) {
        op = LOpcode(op & ~LIR64);
        return op >= LIR_int && op <= LIR_uge;
    }
    inline bool isRetOpcode(LOpcode op) {
        return (op & ~LIR64) == LIR_ret;
    }

    // The opcode is not logically part of the Reservation, but we include it
    // in this struct to ensure that opcode plus the Reservation fits in a
    // single word.  Yuk.
    struct Reservation
    {
        uint32_t arIndex:16;    // index into stack frame.  displ is -4*arIndex
        Register reg:7;         // register UnknownReg implies not in register
        uint32_t used:1;        // when set, the reservation is active
        LOpcode  opcode:8;

        inline void init() {
            reg = UnknownReg;
            arIndex = 0;
            used = 1;
        }

        inline void clear() {
            used = 0;
        }
    };

    // Array holding the 'operands' field from LIRopcode.tbl.
    extern const int8_t operandCount[];

    // Array holding the 'repkind' field from LIRopcode.tbl.
    extern const uint8_t repKinds[];

    //-----------------------------------------------------------------------
    // Low-level instructions.  This is a bit complicated, because we have a
    // variable-width representation to minimise space usage.
    //
    // - Instruction size is always an integral multiple of word size.
    //
    // - Every instruction has at least one word, holding the opcode and the
    //   reservation info.  That word is in class LIns.
    //
    // - Beyond that, most instructions have 1, 2 or 3 extra words.  These
    //   extra words are in classes LInsOp1, LInsOp2, etc (collectively called
    //   "LInsXYZ" in what follows).  Each LInsXYZ class also contains an LIns,
    //   accessible by the 'ins' member, which holds the LIns data.
    //
    // - LIR is written forward, but read backwards.  When reading backwards,
    //   in order to find the opcode, it must be in a predictable place in the
    //   LInsXYZ isn't affected by instruction width.  Therefore, the LIns
    //   word (which contains the opcode) is always the *last* word in an
    //   instruction.
    //
    // - Each instruction is created by casting pre-allocated bytes from a
    //   LirBuffer to the LInsXYZ type.  Therefore there are no constructors
    //   for LIns or LInsXYZ.
    //
    // - The standard handle for an instruction is a LIns*.  This actually
    //   points to the LIns word, ie. to the final word in the instruction.
    //   This is a bit odd, but it allows the instruction's opcode to be
    //   easily accessed.  Once you've looked at the opcode and know what kind
    //   of instruction it is, if you want to access any of the other words,
    //   you need to use toLInsXYZ(), which takes the LIns* and gives you an
    //   LInsXYZ*, ie. the pointer to the actual start of the instruction's
    //   bytes.  From there you can access the instruction-specific extra
    //   words.
    //
    // - However, from outside class LIns, LInsXYZ isn't visible, nor is
    //   toLInsXYZ() -- from outside LIns, all LIR instructions are handled
    //   via LIns pointers and get/set methods are used for all LIns/LInsXYZ
    //   accesses.  In fact, all data members in LInsXYZ are private and can
    //   only be accessed by LIns, which is a friend class.  The only thing
    //   anyone outside LIns can do with a LInsXYZ is call getLIns().
    //
    // - An example Op2 instruction and the likely pointers to it (each line
    //   represents a word, and pointers to a line point to the start of the
    //   word on that line):
    //
    //      [ oprnd_2         <-- LInsOp2* insOp2 == toLInsOp2(ins)
    //        oprnd_1
    //        opcode + resv ] <-- LIns* ins
    //
    // - LIR_skip instructions are more complicated.  They allow an arbitrary
    //   blob of data (the "payload") to be placed in the LIR stream.  The
    //   size of the payload is always a multiple of the word size.  A skip
    //   instruction's operand points to the previous instruction, which lets
    //   the payload be skipped over when reading backwards.  Here's an
    //   example of a skip instruction with a 3-word payload preceded by an
    //   LInsOp1:
    //
    //      [ oprnd_1
    //  +->   opcode + resv           ]
    //  |   [ data
    //  |     data
    //  |     data
    //  +---- prevLIns                  <-- LInsSk* insSk == toLInsSk(ins)
    //        opcode==LIR_skip + resv ] <-- LIns* ins
    //
    //   Skips are also used to link code pages.  If the first instruction on
    //   a page isn't a LIR_start, it will be a skip, and the skip's operand
    //   will point to the last LIns on the previous page.  In this case there
    //   isn't a payload as such;  in fact, the previous page might be at a
    //   higher address, ie. the operand might point forward rather than
    //   backward.
    //
    //   LInsSk has the same layout as LInsOp1, but we represent it as a
    //   different class because there are some places where we treat
    //   skips specially and so having it separate seems like a good idea.
    //
    // - Call instructions (LIR_call, LIR_fcall, LIR_calli, LIR_fcalli) are
    //   also more complicated.  They are preceded by the arguments to the
    //   call, which are laid out in reverse order.  For example, a call with
    //   3 args will look like this:
    //
    //      [ arg #2
    //        arg #1
    //        arg #0
    //        argc            <-- LInsC insC == toLInsC(ins)
    //        ci
    //        opcode + resv ] <-- LIns* ins
    //
    // - Various things about the size and layout of LIns and LInsXYZ are
    //   statically checked in staticSanityCheck().  In particular, this is
    //   worthwhile because there's nothing that guarantees that all the
    //   LInsXYZ classes have a size that is a multiple of word size (but in
    //   practice all sane compilers use a layout that results in this).  We
    //   also check that every LInsXYZ is word-aligned in
    //   LirBuffer::makeRoom();  this seems sensible to avoid potential
    //   slowdowns due to misalignment.  It relies on pages themselves being
    //   word-aligned, which is extremely likely.
    //
    // - There is an enum, LInsRepKind, with one member for each of the
    //   LInsXYZ kinds.  Each opcode is categorised with its LInsRepKind value
    //   in LIRopcode.tbl, and this is used in various places.
    //-----------------------------------------------------------------------

    enum LInsRepKind {
        // LRK_XYZ corresponds to class LInsXYZ.
        LRK_Op0,
        LRK_Op1,
        LRK_Op2,
        LRK_Op3,
        LRK_Ld,
        LRK_Sti,
        LRK_Sk,
        LRK_C,
        LRK_P,
        LRK_I,
        LRK_I64,
        LRK_None    // this one is used for unused opcode numbers
    };

    class LInsOp0;
    class LInsOp1;
    class LInsOp2;
    class LInsOp3;
    class LInsLd;
    class LInsSti;
    class LInsSk;
    class LInsC;
    class LInsP;
    class LInsI;
    class LInsI64;

    class LIns
    {
    private:
        // Last word: fields shared by all LIns kinds.  The reservation fields
        // are read/written during assembly.
        union {
            Reservation lastWord;
            // force sizeof(LIns)==8 and 8-byte alignment on 64-bit machines.
            // this is necessary because sizeof(Reservation)==4 and we want all
            // instances of LIns to be pointer-aligned.
            void* dummy;
        };

        // LIns-to-LInsXYZ converters.
        inline LInsOp0* toLInsOp0() const;
        inline LInsOp1* toLInsOp1() const;
        inline LInsOp2* toLInsOp2() const;
        inline LInsOp3* toLInsOp3() const;
        inline LInsLd*  toLInsLd()  const;
        inline LInsSti* toLInsSti() const;
        inline LInsSk*  toLInsSk()  const;
        inline LInsC*   toLInsC()   const;
        inline LInsP*   toLInsP()   const;
        inline LInsI*   toLInsI()   const;
        inline LInsI64* toLInsI64() const;

        void staticSanityCheck();

    public:
        // LIns initializers.
        inline void initLInsOp0(LOpcode opcode);
        inline void initLInsOp1(LOpcode opcode, LIns* oprnd1);
        inline void initLInsOp2(LOpcode opcode, LIns* oprnd1, LIns* oprnd2);
        inline void initLInsOp3(LOpcode opcode, LIns* oprnd1, LIns* oprnd2, LIns* oprnd3);
        inline void initLInsLd(LOpcode opcode, LIns* val, int32_t d);
        inline void initLInsSti(LOpcode opcode, LIns* val, LIns* base, int32_t d);
        inline void initLInsSk(LIns* prevLIns);
        // Nb: initLInsC() does NOT initialise the arguments.  That must be
        // done separately.
        inline void initLInsC(LOpcode opcode, int32_t argc, const CallInfo* ci);
        inline void initLInsP(int32_t arg, int32_t kind);
        inline void initLInsI(LOpcode opcode, int32_t imm32);
        inline void initLInsI64(LOpcode opcode, int64_t imm64);

        LOpcode opcode() const { return lastWord.opcode; }

        // Reservation functions.
        Reservation* resv() {
            return &lastWord;
        }
        // Like resv(), but asserts that the Reservation is used.
        Reservation* resvUsed() {
            Reservation* r = resv();
            NanoAssert(r->used);
            return r;
        }
        void markAsUsed() {
            lastWord.init();
        }
        void markAsClear() {
            lastWord.clear();
        }
        bool isUsed() {
            return lastWord.used;
        }
        bool hasKnownReg() {
            NanoAssert(isUsed());
            return getReg() != UnknownReg;
        }
        Register getReg() {
            NanoAssert(isUsed());
            return lastWord.reg;
        }
        void setReg(Register r) {
            NanoAssert(isUsed());
            lastWord.reg = r;
        }
        uint32_t getArIndex() {
            NanoAssert(isUsed());
            return lastWord.arIndex;
        }
        void setArIndex(uint32_t arIndex) {
            NanoAssert(isUsed());
            lastWord.arIndex = arIndex;
        }
        bool isUnusedOrHasUnknownReg() {
            return !isUsed() || !hasKnownReg();
        }

        // For various instruction kinds.
        inline LIns*    oprnd1() const;
        inline LIns*    oprnd2() const;
        inline LIns*    oprnd3() const;

        // For branches.
        inline LIns*    getTarget() const;
        inline void     setTarget(LIns* label);

        // For guards.
        inline GuardRecord* record() const;

        // Displacement for LInsLd/LInsSti
        inline int32_t  disp() const;

        // For LInsSk.
        inline void*    payload()  const;
        inline LIns*    prevLIns() const;

        // For LInsP.
        inline uint8_t  paramArg()  const;
        inline uint8_t  paramKind() const;

        // For LInsI.
        inline int32_t  imm32() const;

        // For LInsI64.
        inline int32_t  imm64_0() const;
        inline int32_t  imm64_1() const;
        inline uint64_t imm64()   const;
        inline double   imm64f()  const;

        // For LIR_alloc.
        inline int32_t  size()    const;
        inline void     setSize(int32_t nbytes);

        // For LInsC.
        inline LIns*    arg(uint32_t i)         const;
        inline uint32_t argc()                  const;
        inline LIns*    callArgN(uint32_t n)    const;
        inline const CallInfo* callInfo()       const;

        // isLInsXYZ() returns true if the instruction has the LInsXYZ form.
        // Note that there is some overlap with other predicates, eg.
        // isStore()==isLInsSti(), isCall()==isLInsC(), but that's ok;  these
        // ones are used mostly to check that opcodes are appropriate for
        // instruction layouts, the others are used for non-debugging
        // purposes.
        bool isLInsOp0() const {
            NanoAssert(LRK_None != repKinds[opcode()]);
            return LRK_Op0 == repKinds[opcode()];
        }
        bool isLInsOp1() const {
            NanoAssert(LRK_None != repKinds[opcode()]);
            return LRK_Op1 == repKinds[opcode()];
        }
        bool isLInsOp2() const {
            NanoAssert(LRK_None != repKinds[opcode()]);
            return LRK_Op2 == repKinds[opcode()];
        }
        bool isLInsOp3() const {
            NanoAssert(LRK_None != repKinds[opcode()]);
            return LRK_Op3 == repKinds[opcode()];
        }
        bool isLInsLd() const {
            NanoAssert(LRK_None != repKinds[opcode()]);
            return LRK_Ld == repKinds[opcode()];
        }
        bool isLInsSti() const {
            NanoAssert(LRK_None != repKinds[opcode()]);
            return LRK_Sti == repKinds[opcode()];
        }
        bool isLInsSk() const {
            NanoAssert(LRK_None != repKinds[opcode()]);
            return LRK_Sk == repKinds[opcode()];
        }
        bool isLInsC() const {
            NanoAssert(LRK_None != repKinds[opcode()]);
            return LRK_C == repKinds[opcode()];
        }
        bool isLInsP() const {
            NanoAssert(LRK_None != repKinds[opcode()]);
            return LRK_P == repKinds[opcode()];
        }
        bool isLInsI() const {
            NanoAssert(LRK_None != repKinds[opcode()]);
            return LRK_I == repKinds[opcode()];
        }
        bool isLInsI64() const {
            NanoAssert(LRK_None != repKinds[opcode()]);
            return LRK_I64 == repKinds[opcode()];
        }

        // LIns predicates.
        bool isCse() const {
            return isCseOpcode(opcode()) || (isCall() && callInfo()->_cse);
        }
        bool isRet() const {
            return isRetOpcode(opcode());
        }
        bool isop(LOpcode o) const {
            return opcode() == o;
        }
        bool isQuad() const {
            LOpcode op = opcode();
#ifdef NANOJIT_64BIT
            // callh in 64bit cpu's means a call that returns an int64 in a single register
            return (!(op >= LIR_qeq && op <= LIR_quge) && (op & LIR64) != 0) ||
                   op == LIR_callh;
#else
            // callh in 32bit cpu's means the 32bit MSW of an int64 result in 2 registers
            return (op & LIR64) != 0;
#endif
        }
        bool isCond() const {
            LOpcode op = opcode();
            return (op == LIR_ov) || isCmp();
        }
        bool isFloat() const;   // not inlined because it contains a switch
        bool isCmp() const {
            LOpcode op = opcode();
            return (op >= LIR_eq && op <= LIR_uge) ||
                   (op >= LIR_qeq && op <= LIR_quge) ||
                   (op >= LIR_feq && op <= LIR_fge);
        }
        bool isCall() const {
            LOpcode op = opcode();
            return (op & ~LIR64) == LIR_icall || op == LIR_qcall;
        }
        bool isStore() const {
            LOpcode op = LOpcode(opcode() & ~LIR64);
            return op == LIR_sti;
        }
        bool isLoad() const {
            LOpcode op = opcode();
            return op == LIR_ldq  || op == LIR_ld || op == LIR_ldc ||
                   op == LIR_ldqc || op == LIR_ldcs || op == LIR_ldcb;
        }
        bool isGuard() const {
            LOpcode op = opcode();
            return op == LIR_x || op == LIR_xf || op == LIR_xt ||
                   op == LIR_xbarrier || op == LIR_xtbl;
        }
        // True if the instruction is a 32-bit or smaller constant integer.
        bool isconst() const {
            return opcode() == LIR_int;
        }
        // True if the instruction is a 32-bit or smaller constant integer and
        // has the value val when treated as a 32-bit signed integer.
        bool isconstval(int32_t val) const {
            return isconst() && imm32()==val;
        }
        // True if the instruction is a constant quad value.
        bool isconstq() const {
            return opcode() == LIR_quad || opcode() == LIR_float;
        }
        // True if the instruction is a constant pointer value.
        bool isconstp() const
        {
#ifdef NANOJIT_64BIT
            return isconstq();
#else
            return isconst();
#endif
        }
        // True if the instruction is a constant float value.
        bool isconstf() const {
            return opcode() == LIR_float;
        }

        bool isBranch() const {
            return isop(LIR_jt) || isop(LIR_jf) || isop(LIR_j);
        }

        bool isPtr() {
#ifdef NANOJIT_64BIT
            return isQuad();
#else
            return !isQuad();
#endif
        }

        // Return true if removal of 'ins' from a LIR fragment could
        // possibly change the behaviour of that fragment, even if any
        // value computed by 'ins' is not used later in the fragment.
        // In other words, can 'ins' possible alter control flow or memory?
        // Note, this assumes that loads will never fault and hence cannot
        // affect the control flow.
        bool isStmt() {
            return isGuard() || isBranch() ||
                   (isCall() && !isCse()) ||
                   isStore() ||
                   isop(LIR_label) || isop(LIR_live) || isop(LIR_flive) ||
                   isop(LIR_regfence) ||
                   isRet();
        }

        inline void* constvalp() const
        {
        #ifdef NANOJIT_64BIT
            return (void*)imm64();
        #else
            return (void*)imm32();
        #endif
        }
    };

    typedef LIns* LInsp;
    typedef SeqBuilder<LIns*> InsList;


    // 0-operand form.  Used for LIR_start and LIR_label.
    class LInsOp0
    {
    private:
        friend class LIns;

        LIns        ins;

    public:
        LIns* getLIns() { return &ins; };
    };

    // 1-operand form.  Used for LIR_ret, LIR_ov, unary arithmetic/logic ops,
    // etc.
    class LInsOp1
    {
    private:
        friend class LIns;

        LIns*       oprnd_1;

        LIns        ins;

    public:
        LIns* getLIns() { return &ins; };
    };

    // 2-operand form.  Used for loads, guards, branches, comparisons, binary
    // arithmetic/logic ops, etc.
    class LInsOp2
    {
    private:
        friend class LIns;

        LIns*       oprnd_2;

        LIns*       oprnd_1;

        LIns        ins;

    public:
        LIns* getLIns() { return &ins; };
    };

    // 3-operand form.  Used for conditional moves.
    class LInsOp3
    {
    private:
        friend class LIns;

        LIns*       oprnd_3;

        LIns*       oprnd_2;

        LIns*       oprnd_1;

        LIns        ins;

    public:
        LIns* getLIns() { return &ins; };
    };

    // Used for all loads.
    class LInsLd
    {
    private:
        friend class LIns;

        int32_t     disp;

        LIns*       oprnd_1;

        LIns        ins;

    public:
        LIns* getLIns() { return &ins; };
    };

    // Used for LIR_sti and LIR_stqi.
    class LInsSti
    {
    private:
        friend class LIns;

        int32_t     disp;

        LIns*       oprnd_2;

        LIns*       oprnd_1;

        LIns        ins;

    public:
        LIns* getLIns() { return &ins; };
    };

    // Used for LIR_skip.
    class LInsSk
    {
    private:
        friend class LIns;

        LIns*       prevLIns;

        LIns        ins;

    public:
        LIns* getLIns() { return &ins; };
    };

    // Used for all variants of LIR_call.
    class LInsC
    {
    private:
        friend class LIns;

        uintptr_t   argc:8;

        const CallInfo* ci;

        LIns        ins;

    public:
        LIns* getLIns() { return &ins; };
    };

    // Used for LIR_iparam.
    class LInsP
    {
    private:
        friend class LIns;

        uintptr_t   arg:8;
        uintptr_t   kind:8;

        LIns        ins;

    public:
        LIns* getLIns() { return &ins; };
    };

    // Used for LIR_int and LIR_ialloc.
    class LInsI
    {
    private:
        friend class LIns;

        int32_t     imm32;

        LIns        ins;

    public:
        LIns* getLIns() { return &ins; };
    };

    // Used for LIR_quad.
    class LInsI64
    {
    private:
        friend class LIns;

        int32_t     imm64_0;

        int32_t     imm64_1;

        LIns        ins;

    public:
        LIns* getLIns() { return &ins; };
    };

    // Used only as a placeholder for OPDEF macros for unused opcodes in
    // LIRopcode.tbl.
    class LInsNone
    {
    };

    LInsOp0* LIns::toLInsOp0() const { return (LInsOp0*)( uintptr_t(this+1) - sizeof(LInsOp0) ); }
    LInsOp1* LIns::toLInsOp1() const { return (LInsOp1*)( uintptr_t(this+1) - sizeof(LInsOp1) ); }
    LInsOp2* LIns::toLInsOp2() const { return (LInsOp2*)( uintptr_t(this+1) - sizeof(LInsOp2) ); }
    LInsOp3* LIns::toLInsOp3() const { return (LInsOp3*)( uintptr_t(this+1) - sizeof(LInsOp3) ); }
    LInsLd*  LIns::toLInsLd()  const { return (LInsLd* )( uintptr_t(this+1) - sizeof(LInsLd ) ); }
    LInsSti* LIns::toLInsSti() const { return (LInsSti*)( uintptr_t(this+1) - sizeof(LInsSti) ); }
    LInsSk*  LIns::toLInsSk()  const { return (LInsSk* )( uintptr_t(this+1) - sizeof(LInsSk ) ); }
    LInsC*   LIns::toLInsC()   const { return (LInsC*  )( uintptr_t(this+1) - sizeof(LInsC  ) ); }
    LInsP*   LIns::toLInsP()   const { return (LInsP*  )( uintptr_t(this+1) - sizeof(LInsP  ) ); }
    LInsI*   LIns::toLInsI()   const { return (LInsI*  )( uintptr_t(this+1) - sizeof(LInsI  ) ); }
    LInsI64* LIns::toLInsI64() const { return (LInsI64*)( uintptr_t(this+1) - sizeof(LInsI64) ); }

    void LIns::initLInsOp0(LOpcode opcode) {
        lastWord.clear();
        lastWord.opcode = opcode;
        NanoAssert(isLInsOp0());
    }
    void LIns::initLInsOp1(LOpcode opcode, LIns* oprnd1) {
        lastWord.clear();
        lastWord.opcode = opcode;
        toLInsOp1()->oprnd_1 = oprnd1;
        NanoAssert(isLInsOp1());
    }
    void LIns::initLInsOp2(LOpcode opcode, LIns* oprnd1, LIns* oprnd2) {
        lastWord.clear();
        lastWord.opcode = opcode;
        toLInsOp2()->oprnd_1 = oprnd1;
        toLInsOp2()->oprnd_2 = oprnd2;
        NanoAssert(isLInsOp2());
    }
    void LIns::initLInsOp3(LOpcode opcode, LIns* oprnd1, LIns* oprnd2, LIns* oprnd3) {
        lastWord.clear();
        lastWord.opcode = opcode;
        toLInsOp3()->oprnd_1 = oprnd1;
        toLInsOp3()->oprnd_2 = oprnd2;
        toLInsOp3()->oprnd_3 = oprnd3;
        NanoAssert(isLInsOp3());
    }
    void LIns::initLInsLd(LOpcode opcode, LIns* val, int32_t d) {
        lastWord.clear();
        lastWord.opcode = opcode;
        toLInsLd()->oprnd_1 = val;
        toLInsLd()->disp = d;
        NanoAssert(isLInsLd());
    }
    void LIns::initLInsSti(LOpcode opcode, LIns* val, LIns* base, int32_t d) {
        lastWord.clear();
        lastWord.opcode = opcode;
        toLInsSti()->oprnd_1 = val;
        toLInsSti()->oprnd_2 = base;
        toLInsSti()->disp = d;
        NanoAssert(isLInsSti());
    }
    void LIns::initLInsSk(LIns* prevLIns) {
        lastWord.clear();
        lastWord.opcode = LIR_skip;
        toLInsSk()->prevLIns = prevLIns;
        NanoAssert(isLInsSk());
    }
    void LIns::initLInsC(LOpcode opcode, int32_t argc, const CallInfo* ci) {
        NanoAssert(isU8(argc));
        lastWord.clear();
        lastWord.opcode = opcode;
        toLInsC()->argc = argc;
        toLInsC()->ci = ci;
        NanoAssert(isLInsC());
    }
    void LIns::initLInsP(int32_t arg, int32_t kind) {
        lastWord.clear();
        lastWord.opcode = LIR_param;
        NanoAssert(isU8(arg) && isU8(kind));
        toLInsP()->arg = arg;
        toLInsP()->kind = kind;
        NanoAssert(isLInsP());
    }
    void LIns::initLInsI(LOpcode opcode, int32_t imm32) {
        lastWord.clear();
        lastWord.opcode = opcode;
        toLInsI()->imm32 = imm32;
        NanoAssert(isLInsI());
    }
    void LIns::initLInsI64(LOpcode opcode, int64_t imm64) {
        lastWord.clear();
        lastWord.opcode = opcode;
        toLInsI64()->imm64_0 = int32_t(imm64);
        toLInsI64()->imm64_1 = int32_t(imm64 >> 32);
        NanoAssert(isLInsI64());
    }

    LIns* LIns::oprnd1() const {
        NanoAssert(isLInsOp1() || isLInsOp2() || isLInsOp3() || isLInsLd() || isLInsSti());
        return toLInsOp2()->oprnd_1;
    }
    LIns* LIns::oprnd2() const {
        NanoAssert(isLInsOp2() || isLInsOp3() || isLInsSti());
        return toLInsOp2()->oprnd_2;
    }
    LIns* LIns::oprnd3() const {
        NanoAssert(isLInsOp3());
        return toLInsOp3()->oprnd_3;
    }

    LIns* LIns::getTarget() const {
        NanoAssert(isBranch());
        return oprnd2();
    }

    void LIns::setTarget(LIns* label) {
        NanoAssert(label && label->isop(LIR_label));
        NanoAssert(isBranch());
        toLInsOp2()->oprnd_2 = label;
    }

    GuardRecord *LIns::record() const {
        NanoAssert(isGuard());
        return (GuardRecord*)oprnd2();
    }

    int32_t LIns::disp() const {
        if (isLInsSti()) {
            return toLInsSti()->disp;
        } else {
            NanoAssert(isLInsLd());
            return toLInsLd()->disp;
        }
    }

    void* LIns::payload() const {
        NanoAssert(isop(LIR_skip));
        // Operand 1 points to the previous LIns;  we move past it to get to
        // the payload.
        return (void*) (uintptr_t(prevLIns()) + sizeof(LIns));
    }

    LIns* LIns::prevLIns() const {
        NanoAssert(isLInsSk());
        return toLInsSk()->prevLIns;
    }

    inline uint8_t LIns::paramArg()  const { NanoAssert(isop(LIR_param)); return toLInsP()->arg; }
    inline uint8_t LIns::paramKind() const { NanoAssert(isop(LIR_param)); return toLInsP()->kind; }

    inline int32_t LIns::imm32()     const { NanoAssert(isconst());  return toLInsI()->imm32; }

    inline int32_t LIns::imm64_0()   const { NanoAssert(isconstq()); return toLInsI64()->imm64_0; }
    inline int32_t LIns::imm64_1()   const { NanoAssert(isconstq()); return toLInsI64()->imm64_1; }
    uint64_t       LIns::imm64()     const {
        NanoAssert(isconstq());
        return (uint64_t(toLInsI64()->imm64_1) << 32) | uint32_t(toLInsI64()->imm64_0);
    }
    double         LIns::imm64f()    const {
        union {
            double f;
            uint64_t q;
        } u;
        u.q = imm64();
        return u.f;
    }

    int32_t LIns::size() const {
        NanoAssert(isop(LIR_alloc));
        return toLInsI()->imm32 << 2;
    }

    void LIns::setSize(int32_t nbytes) {
        NanoAssert(isop(LIR_alloc));
        NanoAssert(nbytes > 0);
        toLInsI()->imm32 = (nbytes+3)>>2; // # of required 32bit words
    }

    // Index args in r-l order.  arg(0) is rightmost arg.
    // Nb: this must be kept in sync with insCall().
    LIns* LIns::arg(uint32_t i) const
    {
        NanoAssert(isCall());
        NanoAssert(i < argc());
        // Move to the start of the LInsC, then move back one word per argument.
        LInsp* argSlot = (LInsp*)(uintptr_t(toLInsC()) - (i+1)*sizeof(void*));
        return *argSlot;
    }

    uint32_t LIns::argc() const {
        NanoAssert(isCall());
        return toLInsC()->argc;
    }

    LIns* LIns::callArgN(uint32_t n) const
    {
        return arg(argc()-n-1);
    }

    const CallInfo* LIns::callInfo() const
    {
        NanoAssert(isCall());
        return toLInsC()->ci;
    }

    class LirWriter
    {
        LInsp insDisp(LInsp base, int32_t& d) {
            if (!isValidDisplacement(d)) {
                base = ins2i(LIR_piadd, base, d);
                d = 0;
            }
            return base;
        }
    public:
        LirWriter *out;

        LirWriter(LirWriter* out)
            : out(out) {}
        virtual ~LirWriter() {}

        virtual LInsp ins0(LOpcode v) {
            return out->ins0(v);
        }
        virtual LInsp ins1(LOpcode v, LIns* a) {
            return out->ins1(v, a);
        }
        virtual LInsp ins2(LOpcode v, LIns* a, LIns* b) {
            return out->ins2(v, a, b);
        }
        virtual LInsp ins3(LOpcode v, LIns* a, LIns* b, LIns* c) {
            return out->ins3(v, a, b, c);
        }
        virtual LInsp insGuard(LOpcode v, LIns *c, GuardRecord *gr) {
            return out->insGuard(v, c, gr);
        }
        virtual LInsp insBranch(LOpcode v, LInsp condition, LInsp to) {
            return out->insBranch(v, condition, to);
        }
        // arg: 0=first, 1=second, ...
        // kind: 0=arg 1=saved-reg
        virtual LInsp insParam(int32_t arg, int32_t kind) {
            return out->insParam(arg, kind);
        }
        virtual LInsp insImm(int32_t imm) {
            return out->insImm(imm);
        }
        virtual LInsp insImmq(uint64_t imm) {
            return out->insImmq(imm);
        }
        virtual LInsp insImmf(double d) {
            return out->insImmf(d);
        }
        virtual LInsp insLoad(LOpcode op, LIns* base, int32_t d) {
            base = insDisp(base, d);
            return out->insLoad(op, base, d);
        }
        virtual LInsp insStorei(LIns* value, LIns* base, int32_t d) {
            base = insDisp(base, d);
            return out->insStorei(value, base, d);
        }
        virtual LInsp insCall(const CallInfo *call, LInsp args[]) {
            return out->insCall(call, args);
        }
        virtual LInsp insAlloc(int32_t size) {
            NanoAssert(size != 0);
            return out->insAlloc(size);
        }
        virtual LInsp insSkip(size_t size) {
            return out->insSkip(size);
        }
        void insAssert(LIns* expr) {
            #if defined DEBUG
            LIns* branch = insBranch(LIR_jt, expr, NULL);
            ins0(LIR_dbreak);
            branch->setTarget(ins0(LIR_label));
            #endif
        }

        // convenience functions

        // Inserts a conditional to execute and branches to execute if
        // the condition is true and false respectively.
        LIns*        ins_choose(LIns* cond, LIns* iftrue, LIns* iffalse);
        // Inserts an integer comparison to 0
        LIns*        ins_eq0(LIns* oprnd1);
        // Inserts a pointer comparison to 0
        LIns*        ins_peq0(LIns* oprnd1);
        // Inserts a binary operation where the second operand is an
        // integer immediate.
        LIns*        ins2i(LOpcode op, LIns *oprnd1, int32_t);
        LIns*        qjoin(LInsp lo, LInsp hi);
        LIns*        insImmPtr(const void *ptr);
        LIns*        insImmWord(intptr_t ptr);
        // Sign or zero extend integers to native integers. On 32-bit this is a no-op.
        LIns*        ins_i2p(LIns* intIns);
        LIns*        ins_u2p(LIns* uintIns);
    };


#ifdef NJ_VERBOSE
    extern const char* lirNames[];

    /**
     * map address ranges to meaningful names.
     */
    class LabelMap
    {
        Allocator& allocator;
        class Entry
        {
        public:
            Entry(int) : name(0), size(0), align(0) {}
            Entry(char *n, size_t s, size_t a) : name(n),size(s),align(a) {}
            char* name;
            size_t size:29, align:3;
        };
        TreeMap<const void*, Entry*> names;
        LogControl *logc;
        char buf[1000], *end;
        void formatAddr(const void *p, char *buf);
    public:
        LabelMap(Allocator& allocator, LogControl* logc);
        void add(const void *p, size_t size, size_t align, const char *name);
        const char *dup(const char *);
        const char *format(const void *p);
    };

    class LirNameMap
    {
        Allocator& alloc;

        template <class Key>
        class CountMap: public HashMap<Key, int> {
        public:
            CountMap(Allocator& alloc) : HashMap<Key, int>(alloc) {}
            int add(Key k) {
                int c = 1;
                if (containsKey(k)) {
                    c = 1+get(k);
                }
                put(k,c);
                return c;
            }
        };
        CountMap<int> lircounts;
        CountMap<const CallInfo *> funccounts;

        class Entry
        {
        public:
            Entry(int) : name(0) {}
            Entry(char* n) : name(n) {}
            char* name;
        };
        HashMap<LInsp, Entry*> names;
        LabelMap *labels;
        void formatImm(int32_t c, char *buf);
    public:

        LirNameMap(Allocator& alloc, LabelMap *lm)
            : alloc(alloc),
            lircounts(alloc),
            funccounts(alloc),
            names(alloc),
            labels(lm)
        {}

        void addName(LInsp i, const char *s);
        void copyName(LInsp i, const char *s, int suffix);
        const char *formatRef(LIns *ref);
        const char *formatIns(LInsp i);
        void formatGuard(LInsp i, char *buf);
    };


    class VerboseWriter : public LirWriter
    {
        InsList code;
        LirNameMap* names;
        LogControl* logc;
    public:
        VerboseWriter(Allocator& alloc, LirWriter *out,
                      LirNameMap* names, LogControl* logc)
            : LirWriter(out), code(alloc), names(names), logc(logc)
        {}

        LInsp add(LInsp i) {
            if (i)
                code.add(i);
            return i;
        }

        LInsp add_flush(LInsp i) {
            if ((i = add(i)) != 0)
                flush();
            return i;
        }

        void flush()
        {
            if (!code.isEmpty()) {
                int32_t count = 0;
                for (Seq<LIns*>* p = code.get(); p != NULL; p = p->tail) {
                    logc->printf("    %s\n",names->formatIns(p->head));
                    count++;
                }
                code.clear();
                if (count > 1)
                    logc->printf("\n");
            }
        }

        LIns* insGuard(LOpcode op, LInsp cond, GuardRecord *gr) {
            return add_flush(out->insGuard(op,cond,gr));
        }

        LIns* insBranch(LOpcode v, LInsp condition, LInsp to) {
            return add_flush(out->insBranch(v, condition, to));
        }

        LIns* ins0(LOpcode v) {
            if (v == LIR_label || v == LIR_start) {
                flush();
            }
            return add(out->ins0(v));
        }

        LIns* ins1(LOpcode v, LInsp a) {
            return isRetOpcode(v) ? add_flush(out->ins1(v, a)) : add(out->ins1(v, a));
        }
        LIns* ins2(LOpcode v, LInsp a, LInsp b) {
            return add(out->ins2(v, a, b));
        }
        LIns* ins3(LOpcode v, LInsp a, LInsp b, LInsp c) {
            return add(out->ins3(v, a, b, c));
        }
        LIns* insCall(const CallInfo *call, LInsp args[]) {
            return add_flush(out->insCall(call, args));
        }
        LIns* insParam(int32_t i, int32_t kind) {
            return add(out->insParam(i, kind));
        }
        LIns* insLoad(LOpcode v, LInsp base, int32_t disp) {
            return add(out->insLoad(v, base, disp));
        }
        LIns* insStorei(LInsp v, LInsp b, int32_t d) {
            return add(out->insStorei(v, b, d));
        }
        LIns* insAlloc(int32_t size) {
            return add(out->insAlloc(size));
        }
        LIns* insImm(int32_t imm) {
            return add(out->insImm(imm));
        }
        LIns* insImmq(uint64_t imm) {
            return add(out->insImmq(imm));
        }
        LIns* insImmf(double d) {
            return add(out->insImmf(d));
        }
    };

#endif

    class ExprFilter: public LirWriter
    {
    public:
        ExprFilter(LirWriter *out) : LirWriter(out) {}
        LIns* ins1(LOpcode v, LIns* a);
        LIns* ins2(LOpcode v, LIns* a, LIns* b);
        LIns* ins3(LOpcode v, LIns* a, LIns* b, LIns* c);
        LIns* insGuard(LOpcode, LIns *cond, GuardRecord *);
        LIns* insBranch(LOpcode, LIns *cond, LIns *target);
        LIns* insLoad(LOpcode op, LInsp base, int32_t off);
    };

    // @todo, this could be replaced by a generic HashMap or HashSet, if we had one
    class LInsHashSet
    {
        // must be a power of 2.
        // don't start too small, or we'll waste time growing and rehashing.
        // don't start too large, will waste memory.
        static const uint32_t kInitialCap = 64;

        LInsp *m_list;
        uint32_t m_used, m_cap;
        Allocator& alloc;

        static uint32_t hashcode(LInsp i);
        uint32_t find(LInsp name, uint32_t hash, const LInsp *list, uint32_t cap);
        static bool equals(LInsp a, LInsp b);
        void grow();

    public:

        LInsHashSet(Allocator&);
        LInsp find32(int32_t a, uint32_t &i);
        LInsp find64(LOpcode v, uint64_t a, uint32_t &i);
        LInsp find1(LOpcode v, LInsp a, uint32_t &i);
        LInsp find2(LOpcode v, LInsp a, LInsp b, uint32_t &i);
        LInsp find3(LOpcode v, LInsp a, LInsp b, LInsp c, uint32_t &i);
        LInsp findLoad(LOpcode v, LInsp a, int32_t b, uint32_t &i);
        LInsp findcall(const CallInfo *call, uint32_t argc, LInsp args[], uint32_t &i);
        LInsp add(LInsp i, uint32_t k);
        void clear();

        static uint32_t hashimm(int32_t);
        static uint32_t hashimmq(uint64_t);
        static uint32_t hash1(LOpcode v, LInsp);
        static uint32_t hash2(LOpcode v, LInsp, LInsp);
        static uint32_t hash3(LOpcode v, LInsp, LInsp, LInsp);
        static uint32_t hashLoad(LOpcode v, LInsp, int32_t);
        static uint32_t hashcall(const CallInfo *call, uint32_t argc, LInsp args[]);
    };

    class CseFilter: public LirWriter
    {
    public:
        LInsHashSet exprs;
        CseFilter(LirWriter *out, Allocator&);
        LIns* insImm(int32_t imm);
        LIns* insImmq(uint64_t q);
        LIns* insImmf(double d);
        LIns* ins0(LOpcode v);
        LIns* ins1(LOpcode v, LInsp);
        LIns* ins2(LOpcode v, LInsp, LInsp);
        LIns* ins3(LOpcode v, LInsp, LInsp, LInsp);
        LIns* insLoad(LOpcode op, LInsp cond, int32_t d);
        LIns* insCall(const CallInfo *call, LInsp args[]);
        LIns* insGuard(LOpcode op, LInsp cond, GuardRecord *gr);
    };

    class LirBuffer
    {
        public:
            LirBuffer(Allocator& alloc);
            void        clear();
            uintptr_t   makeRoom(size_t szB);   // make room for an instruction

            debug_only (void validate() const;)
            verbose_only(LirNameMap* names;)

            int32_t insCount();
            size_t  byteCount();

            // stats
            struct
            {
                uint32_t lir;    // # instructions
            }
            _stats;

            AbiKind abi;
            LInsp state,param1,sp,rp;
            LInsp savedRegs[NumSavedRegs];

        protected:
            friend class LirBufWriter;

            /** each chunk is just a raw area of LIns instances, with no header
                and no more than 8-byte alignment.  The chunk size is somewhat arbitrary
                as long as it's well larger than 2*sizeof(LInsSk) */
            static const size_t CHUNK_SZB = 8000;

            /** the first instruction on a chunk is always a start instruction, or a
             *  payload-less skip instruction linking to the previous chunk.  The biggest
             *  possible instruction would take up the entire rest of the chunk. */
            static const size_t MAX_LINS_SZB = CHUNK_SZB - sizeof(LInsSk);

            /** the maximum skip payload size is determined by the maximum instruction
             *  size.  We require that a skip's payload be adjacent to the skip LIns
             *  itself. */
            static const size_t MAX_SKIP_PAYLOAD_SZB = MAX_LINS_SZB - sizeof(LInsSk);

            /** get CHUNK_SZB more memory for LIR instructions */
            void        chunkAlloc();
            void        moveToNewChunk(uintptr_t addrOfLastLInsOnCurrentChunk);

            Allocator&  _allocator;
            uintptr_t   _unused;   // next unused instruction slot in the current LIR chunk
            uintptr_t   _limit;    // one past the last usable byte of the current LIR chunk
            size_t      _bytesAllocated;
    };

    class LirBufWriter : public LirWriter
    {
        LirBuffer*    _buf;        // underlying buffer housing the instructions

        public:
            LirBufWriter(LirBuffer* buf)
                : LirWriter(0), _buf(buf) {
            }

            // LirWriter interface
            LInsp    insLoad(LOpcode op, LInsp base, int32_t disp);
            LInsp    insStorei(LInsp o1, LInsp o2, int32_t disp);
            LInsp    ins0(LOpcode op);
            LInsp    ins1(LOpcode op, LInsp o1);
            LInsp    ins2(LOpcode op, LInsp o1, LInsp o2);
            LInsp    ins3(LOpcode op, LInsp o1, LInsp o2, LInsp o3);
            LInsp    insParam(int32_t i, int32_t kind);
            LInsp    insImm(int32_t imm);
            LInsp    insImmq(uint64_t imm);
            LInsp    insImmf(double d);
            LInsp    insCall(const CallInfo *call, LInsp args[]);
            LInsp    insGuard(LOpcode op, LInsp cond, GuardRecord *gr);
            LInsp    insBranch(LOpcode v, LInsp condition, LInsp to);
            LInsp   insAlloc(int32_t size);
            LInsp   insSkip(size_t);
    };

    class LirFilter
    {
    public:
        LirFilter *in;
        LirFilter(LirFilter *in) : in(in) {}
        virtual ~LirFilter(){}

        virtual LInsp read() {
            return in->read();
        }
        virtual LInsp pos() {
            return in->pos();
        }
    };

    // concrete
    class LirReader : public LirFilter
    {
        LInsp _i; // current instruction that this decoder is operating on.

    public:
        LirReader(LInsp i) : LirFilter(0), _i(i) {
            NanoAssert(_i);
        }
        virtual ~LirReader() {}

        // LirReader i/f
        LInsp read(); // advance to the prior instruction
        LInsp pos() {
            return _i;
        }
    };

    class Assembler;

    void compile(Assembler *assm, Fragment *frag verbose_only(, Allocator& alloc, LabelMap*));
    verbose_only(void live(Allocator& alloc, Fragment *frag, LirBuffer *lirbuf);)

    class StackFilter: public LirFilter
    {
        LirBuffer *lirbuf;
        LInsp sp;
        LInsp rp;
        BitSet spStk;
        BitSet rpStk;
        int spTop;
        int rpTop;
        void getTops(LInsp br, int& spTop, int& rpTop);

    public:
        StackFilter(LirFilter *in, Allocator& alloc, LirBuffer *lirbuf, LInsp sp, LInsp rp);
        bool ignoreStore(LInsp ins, int top, BitSet* stk);
        LInsp read();
    };

    // eliminate redundant loads by watching for stores & mutator calls
    class LoadFilter: public LirWriter
    {
    public:
        LInsp sp, rp;
        LInsHashSet exprs;

        void clear(LInsp p);
    public:
        LoadFilter(LirWriter *out, Allocator& alloc)
            : LirWriter(out), sp(NULL), rp(NULL), exprs(alloc)
        { }

        LInsp ins0(LOpcode);
        LInsp insLoad(LOpcode, LInsp base, int32_t disp);
        LInsp insStorei(LInsp v, LInsp b, int32_t d);
        LInsp insCall(const CallInfo *call, LInsp args[]);
    };

#ifdef DEBUG
    class SanityFilter : public LirWriter
    {
    public:
        SanityFilter(LirWriter* out) : LirWriter(out)
        { }
    public:
        LIns* ins1(LOpcode v, LIns* s0);
        LIns* ins2(LOpcode v, LIns* s0, LIns* s1);
        LIns* ins3(LOpcode v, LIns* s0, LIns* s1, LIns* s2);
    };
#endif
}
#endif // __nanojit_LIR__
