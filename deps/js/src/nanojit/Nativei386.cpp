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
 *   Mozilla TraceMonkey Team
 *   Asko Tontti <atontti@cc.hut.fi>
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

#ifdef _MSC_VER
    // disable some specific warnings which are normally useful, but pervasive in the code-gen macros
    #pragma warning(disable:4310) // cast truncates constant value
#endif

namespace nanojit
{
    #if defined FEATURE_NANOJIT && defined NANOJIT_IA32

    #ifdef NJ_VERBOSE
        const char *regNames[] = {
            "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
            "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
            "f0"
        };
    #endif

    #define TODO(x) do{ verbose_only(outputf(#x);) NanoAssertMsgf(false, "%s", #x); } while(0)

    const Register Assembler::argRegs[] = { ECX, EDX };
    const Register Assembler::retRegs[] = { EAX, EDX };
    const Register Assembler::savedRegs[] = { EBX, ESI, EDI };

    const static uint8_t max_abi_regs[] = {
        2, /* ABI_FASTCALL */
        1, /* ABI_THISCALL */
        0, /* ABI_STDCALL */
        0  /* ABI_CDECL */
    };

    typedef Register R;
    typedef int32_t  I32;

    // XXX rearrange NanoAssert() expression to workaround apparent gcc 4.3 bug:
    // XXX "error: logical && with non-zero constant will always evaluate as true"
    // underrunProtect(6) is necessary for worst-case
    inline void Assembler::MODRMs(I32 r, I32 d, R b, I32 l, I32 i) {
        NanoAssert(unsigned(i)<8 && unsigned(b)<8 && unsigned(r)<8);
        if (d == 0 && b != EBP) {
            _nIns -= 2;
            _nIns[0] = (uint8_t) ( 0<<6 | r<<3 | 4);
            _nIns[1] = (uint8_t) ( l<<6 | i<<3 | b);
        } else if (isS8(d)) {
            _nIns -= 3;
            _nIns[0] = (uint8_t) ( 1<<6 | r<<3 | 4 );
            _nIns[1] = (uint8_t) ( l<<6 | i<<3 | b );
            _nIns[2] = (uint8_t) d;
        } else {
            IMM32(d);
            *(--_nIns) = (uint8_t) ( l<<6 | i<<3 | b );
            *(--_nIns) = (uint8_t) ( 2<<6 | r<<3 | 4 );
        }
    }

    // underrunProtect(6) is necessary for worst-case
    inline void Assembler::MODRMm(I32 r, I32 d, R b) {
        NanoAssert(unsigned(r)<8 && ((b)==UnspecifiedReg || unsigned(b)<8));
        if ((b) == UnspecifiedReg) {
            IMM32(d);
            *(--_nIns) = (uint8_t) (0<<6 | (r)<<3 | 5);
        } else if ((b) == ESP) {
            MODRMs(r, d, b, 0, (Register)4);
        } else if ( (d) == 0 && (b) != EBP) {
            *(--_nIns) = (uint8_t) ( 0<<6 | r<<3 | b );
        } else if (isS8(d)) {
            *(--_nIns) = (uint8_t) (d);
            *(--_nIns) = (uint8_t) ( 1<<6 | r<<3 | b );
        } else {
            IMM32(d);
            *(--_nIns) = (uint8_t) ( 2<<6 | r<<3 | b );
        }
    }

    inline void Assembler::MODRMSIB(R reg, R base, I32 index, I32 scale, I32 disp) {
        if (disp != 0 || base == EBP) {
            if (isS8(disp)) {
                *(--_nIns) = int8_t(disp);
            } else {
                IMM32(disp);
            }
        }
        *(--_nIns) = uint8_t( scale<<6 | index<<3 | base );
        if (disp == 0 && base != EBP) {
            *(--_nIns) = uint8_t( (reg<<3) | 4);
        } else if (isS8(disp)) {
            *(--_nIns) = uint8_t( (1<<6) | (reg<<3) | 4 );
        } else {
            *(--_nIns) = uint8_t( (2<<6) | (reg<<3) | 4 );
        }
    }

    inline void Assembler::MODRMdm(I32 r, I32 addr) {
        NanoAssert(unsigned(r)<8);
        IMM32(addr);
        *(--_nIns) = (uint8_t)( r<<3 | 5 );
    }

    inline void Assembler::ALU0(I32 o) {
        underrunProtect(1);
        *(--_nIns) = uint8_t(o);
    }

    inline void Assembler::ALUm(I32 c, I32 r, I32 d, R b) {
        underrunProtect(8);
        MODRMm(r, d, b);
        *(--_nIns) = uint8_t(c);
    }

    inline void Assembler::ALUdm(I32 c, I32 r, I32 addr) {
        underrunProtect(6);
        MODRMdm(r, addr);
        *(--_nIns) = uint8_t(c);
    }

    inline void Assembler::ALUsib(I32 c, R r, R base, I32 index, I32 scale, I32 disp) {
        underrunProtect(7);
        MODRMSIB(r, base, index, scale, disp);
        *(--_nIns) = uint8_t(c);
    }

    inline void Assembler::ALUm16(I32 c, I32 r, I32 d, R b) {
        underrunProtect(9);
        MODRMm(r, d, b);
        *(--_nIns) = uint8_t(c);
        *(--_nIns) = 0x66;
    }

    inline void Assembler::ALU2dm(I32 c, I32 r, I32 addr) {
        underrunProtect(7);
        MODRMdm(r, addr);
        *(--_nIns) = uint8_t(c);
        *(--_nIns) = uint8_t(c>>8);
    }

    inline void Assembler::ALU2m(I32 c, I32 r, I32 d, R b) {
        underrunProtect(9);
        MODRMm(r, d, b);
        *(--_nIns) = uint8_t(c);
        *(--_nIns) = uint8_t(c>>8);
    }

    inline void Assembler::ALU2sib(I32 c, Register r, R base, I32 index, I32 scale, I32 disp) {
        underrunProtect(8);
        MODRMSIB(r, base, index, scale, disp);
        *(--_nIns) = uint8_t(c);
        *(--_nIns) = uint8_t(c>>8);
    }

    inline void Assembler::ALUi(I32 c, I32 r, I32 i) {
        underrunProtect(6);
        NanoAssert(unsigned(r)<8);
        if (isS8(i)) {
            *(--_nIns) = uint8_t(i);
            MODRM(c>>3, r);
            *(--_nIns) = uint8_t(0x83);
        } else {
            IMM32(i);
            if ( r == EAX) {
                *(--_nIns) = uint8_t(c);
            } else {
                MODRM((c>>3),(r));
                *(--_nIns) = uint8_t(0x81);
            }
        }
    }

    inline void Assembler::ALUmi(I32 c, I32 d, Register b, I32 i) {
        underrunProtect(10);
        NanoAssert(((unsigned)b)<8);
        if (isS8(i)) {
            *(--_nIns) = uint8_t(i);
            MODRMm(c>>3, d, b);
            *(--_nIns) = uint8_t(0x83);
        } else {
            IMM32(i);
            MODRMm(c>>3, d, b);
            *(--_nIns) = uint8_t(0x81);
        }
    }

    inline void Assembler::ALU2(I32 c, I32 d, I32 s) {
        underrunProtect(3);
        MODRM((d),(s));
        _nIns -= 2;
        _nIns[0] = uint8_t(c>>8);
        _nIns[1] = uint8_t(c);
    }

    inline void Assembler::LAHF()        { count_alu(); ALU0(0x9F);                   asm_output("lahf"); }
    inline void Assembler::SAHF()        { count_alu(); ALU0(0x9E);                   asm_output("sahf"); }
    inline void Assembler::OR(R l, R r)  { count_alu(); ALU(0x0b, (l),(r));           asm_output("or %s,%s",gpn(l),gpn(r)); }
    inline void Assembler::AND(R l, R r) { count_alu(); ALU(0x23, (l),(r));           asm_output("and %s,%s",gpn(l),gpn(r)); }
    inline void Assembler::XOR(R l, R r) { count_alu(); ALU(0x33, (l),(r));           asm_output("xor %s,%s",gpn(l),gpn(r)); }
    inline void Assembler::ADD(R l, R r) { count_alu(); ALU(0x03, (l),(r));           asm_output("add %s,%s",gpn(l),gpn(r)); }
    inline void Assembler::SUB(R l, R r) { count_alu(); ALU(0x2b, (l),(r));           asm_output("sub %s,%s",gpn(l),gpn(r)); }
    inline void Assembler::MUL(R l, R r) { count_alu(); ALU2(0x0faf,(l),(r));         asm_output("mul %s,%s",gpn(l),gpn(r)); }
    inline void Assembler::DIV(R r)      { count_alu(); ALU(0xf7, (Register)7,(r));   asm_output("idiv  edx:eax, %s",gpn(r)); }
    inline void Assembler::NOT(R r)      { count_alu(); ALU(0xf7, (Register)2,(r));   asm_output("not %s",gpn(r)); }
    inline void Assembler::NEG(R r)      { count_alu(); ALU(0xf7, (Register)3,(r));   asm_output("neg %s",gpn(r)); }

    inline void Assembler::SHR(R r, R s) {
        count_alu();
        NanoAssert(s == ECX); (void)s;
        ALU(0xd3, (Register)5,(r));
        asm_output("shr %s,%s",gpn(r),gpn(s));
    }

    inline void Assembler::SAR(R r, R s) {
        count_alu();
        NanoAssert(s == ECX); (void)s;
        ALU(0xd3, (Register)7,(r));
        asm_output("sar %s,%s",gpn(r),gpn(s));
    }

    inline void Assembler::SHL(R r, R s) {
        count_alu();
        NanoAssert(s == ECX); (void)s;
        ALU(0xd3, (Register)4,(r));
        asm_output("shl %s,%s",gpn(r),gpn(s));
    }

    inline void Assembler::SHIFT(I32 c, R r, I32 i) {
        underrunProtect(3);
        *--_nIns = (uint8_t)(i);
        MODRM((Register)c,r);
        *--_nIns = 0xc1;
    }

    inline void Assembler::SHLi(R r, I32 i)   { count_alu(); SHIFT(4,r,i); asm_output("shl %s,%d", gpn(r),i); }
    inline void Assembler::SHRi(R r, I32 i)   { count_alu(); SHIFT(5,r,i); asm_output("shr %s,%d", gpn(r),i); }
    inline void Assembler::SARi(R r, I32 i)   { count_alu(); SHIFT(7,r,i); asm_output("sar %s,%d", gpn(r),i); }

    inline void Assembler::MOVZX8(R d, R s)   { count_alu(); ALU2(0x0fb6,d,s); asm_output("movzx %s,%s", gpn(d),gpn(s)); }

    inline void Assembler::SUBi(R r, I32 i)   { count_alu(); ALUi(0x2d,r,i);   asm_output("sub %s,%d",gpn(r),i); }
    inline void Assembler::ADDi(R r, I32 i)   { count_alu(); ALUi(0x05,r,i);   asm_output("add %s,%d",gpn(r),i); }
    inline void Assembler::ANDi(R r, I32 i)   { count_alu(); ALUi(0x25,r,i);   asm_output("and %s,%d",gpn(r),i); }
    inline void Assembler::ORi(R r, I32 i)    { count_alu(); ALUi(0x0d,r,i);   asm_output("or %s,%d",gpn(r),i); }
    inline void Assembler::XORi(R r, I32 i)   { count_alu(); ALUi(0x35,r,i);   asm_output("xor %s,%d",gpn(r),i); }

    inline void Assembler::ADDmi(I32 d, R b, I32 i) { count_alust(); ALUmi(0x05, d, b, i); asm_output("add %d(%s), %d", d, gpn(b), i); }

    inline void Assembler::TEST(R d, R s)      { count_alu(); ALU(0x85,d,s);   asm_output("test %s,%s",gpn(d),gpn(s)); }
    inline void Assembler::CMP(R l, R r)       { count_alu(); ALU(0x3b,l,r);   asm_output("cmp %s,%s",gpn(l),gpn(r)); }
    inline void Assembler::CMPi(R r, I32 i)    { count_alu(); ALUi(0x3d,r,i);  asm_output("cmp %s,%d",gpn(r),i); }

    inline void Assembler::LEA(R r, I32 d, R b)    { count_alu(); ALUm(0x8d, r,d,b);   asm_output("lea %s,%d(%s)",gpn(r),d,gpn(b)); }
    // lea %r, d(%i*4)
    // This addressing mode is not supported by the MODRMSIB macro.
    inline void Assembler::LEAmi4(R r, I32 d, I32 i) {
        count_alu();
        IMM32(int32_t(d));
        *(--_nIns) = (2<<6) | ((uint8_t)i<<3) | 5;
        *(--_nIns) = (0<<6) | ((uint8_t)r<<3) | 4;
        *(--_nIns) = 0x8d;
        asm_output("lea %s, %p(%s*4)", gpn(r), (void*)d, gpn(i));
    }

    inline void Assembler::CDQ()       { SARi(EDX, 31); MR(EDX, EAX); }

    inline void Assembler::INCLi(I32 p) {
        count_alu();
        underrunProtect(6);
        IMM32((uint32_t)(ptrdiff_t)p); *(--_nIns) = 0x05; *(--_nIns) = 0xFF;
        asm_output("incl  (%p)", (void*)p);
    }

    inline void Assembler::SETE( R r)  { count_alu(); ALU2(0x0f94,(r),(r));    asm_output("sete %s", gpn(r)); }
    inline void Assembler::SETNP(R r)  { count_alu(); ALU2(0x0f9B,(r),(r));    asm_output("setnp %s",gpn(r)); }
    inline void Assembler::SETL( R r)  { count_alu(); ALU2(0x0f9C,(r),(r));    asm_output("setl %s", gpn(r)); }
    inline void Assembler::SETLE(R r)  { count_alu(); ALU2(0x0f9E,(r),(r));    asm_output("setle %s",gpn(r)); }
    inline void Assembler::SETG( R r)  { count_alu(); ALU2(0x0f9F,(r),(r));    asm_output("setg %s", gpn(r)); }
    inline void Assembler::SETGE(R r)  { count_alu(); ALU2(0x0f9D,(r),(r));    asm_output("setge %s",gpn(r)); }
    inline void Assembler::SETB( R r)  { count_alu(); ALU2(0x0f92,(r),(r));    asm_output("setb %s", gpn(r)); }
    inline void Assembler::SETBE(R r)  { count_alu(); ALU2(0x0f96,(r),(r));    asm_output("setbe %s",gpn(r)); }
    inline void Assembler::SETA( R r)  { count_alu(); ALU2(0x0f97,(r),(r));    asm_output("seta %s", gpn(r)); }
    inline void Assembler::SETAE(R r)  { count_alu(); ALU2(0x0f93,(r),(r));    asm_output("setae %s",gpn(r)); }
    inline void Assembler::SETO( R r)  { count_alu(); ALU2(0x0f92,(r),(r));    asm_output("seto %s", gpn(r)); }

    inline void Assembler::MREQ(R d, R s) { count_alu(); ALU2(0x0f44,d,s); asm_output("cmove %s,%s",  gpn(d),gpn(s)); }
    inline void Assembler::MRNE(R d, R s) { count_alu(); ALU2(0x0f45,d,s); asm_output("cmovne %s,%s", gpn(d),gpn(s)); }
    inline void Assembler::MRL( R d, R s) { count_alu(); ALU2(0x0f4C,d,s); asm_output("cmovl %s,%s",  gpn(d),gpn(s)); }
    inline void Assembler::MRLE(R d, R s) { count_alu(); ALU2(0x0f4E,d,s); asm_output("cmovle %s,%s", gpn(d),gpn(s)); }
    inline void Assembler::MRG( R d, R s) { count_alu(); ALU2(0x0f4F,d,s); asm_output("cmovg %s,%s",  gpn(d),gpn(s)); }
    inline void Assembler::MRGE(R d, R s) { count_alu(); ALU2(0x0f4D,d,s); asm_output("cmovge %s,%s", gpn(d),gpn(s)); }
    inline void Assembler::MRB( R d, R s) { count_alu(); ALU2(0x0f42,d,s); asm_output("cmovb %s,%s",  gpn(d),gpn(s)); }
    inline void Assembler::MRBE(R d, R s) { count_alu(); ALU2(0x0f46,d,s); asm_output("cmovbe %s,%s", gpn(d),gpn(s)); }
    inline void Assembler::MRA( R d, R s) { count_alu(); ALU2(0x0f47,d,s); asm_output("cmova %s,%s",  gpn(d),gpn(s)); }
    inline void Assembler::MRAE(R d, R s) { count_alu(); ALU2(0x0f43,d,s); asm_output("cmovae %s,%s", gpn(d),gpn(s)); }
    inline void Assembler::MRNO(R d, R s) { count_alu(); ALU2(0x0f41,d,s); asm_output("cmovno %s,%s", gpn(d),gpn(s)); }

    // these aren't currently used but left in for reference
    //#define LDEQ(r,d,b) do { ALU2m(0x0f44,r,d,b); asm_output("cmove %s,%d(%s)", gpn(r),d,gpn(b)); } while(0)
    //#define LDNEQ(r,d,b) do { ALU2m(0x0f45,r,d,b); asm_output("cmovne %s,%d(%s)", gpn(r),d,gpn(b)); } while(0)

    inline void Assembler::LD(R reg, I32 disp, R base) {
        count_ld();
        ALUm(0x8b,reg,disp,base);
        asm_output("mov %s,%d(%s)",gpn(reg),disp,gpn(base));
    }

    inline void Assembler::LDdm(R reg, I32 addr) {
        count_ld();
        ALUdm(0x8b,reg,addr);
        asm_output("mov   %s,0(%lx)",gpn(reg),(unsigned long)addr);
    }

#define SIBIDX(n)    "1248"[n]

    inline void Assembler::LDsib(R reg, I32 disp, R base, I32 index, I32 scale) {
        count_ld();
        ALUsib(0x8b, reg, base, index, scale, disp);
        asm_output("mov   %s,%d(%s+%s*%c)",gpn(reg),disp,gpn(base),gpn(index),SIBIDX(scale));
    }

    // note: movzx/movsx are being output with an 8/16 suffix to indicate the
    // size being loaded. This doesn't really match standard intel format
    // (though is arguably terser and more obvious in this case) and would
    // probably be nice to fix.  (Likewise, the 8/16 bit stores being output
    // as "mov8" and "mov16" respectively.)

    // Load 16-bit, sign extend.
    inline void Assembler::LD16S(R r, I32 d, R b) {
        count_ld();
        ALU2m(0x0fbf, r, d, b);
        asm_output("movsx16 %s,%d(%s)", gpn(r),d,gpn(b));
    }

    inline void Assembler::LD16Sdm(R r, I32 addr) {
        count_ld();
        ALU2dm(0x0fbf, r, addr);
        asm_output("movsx16 %s,0(%lx)", gpn(r),(unsigned long)addr);
    }

    inline void Assembler::LD16Ssib(R r, I32 disp, R base, I32 index, I32 scale) {
        count_ld();
        ALU2sib(0x0fbf, r, base, index, scale, disp);
        asm_output("movsx16 %s,%d(%s+%s*%c)",gpn(r),disp,gpn(base),gpn(index),SIBIDX(scale));
    }

    // Load 16-bit, zero extend.
    inline void Assembler::LD16Z(R r, I32 d, R b) {
        count_ld();
        ALU2m(0x0fb7, r, d, b);
        asm_output("movzx16 %s,%d(%s)", gpn(r),d,gpn(b));
    }

    inline void Assembler::LD16Zdm(R r, I32 addr) {
        count_ld();
        ALU2dm(0x0fb7, r, addr);
        asm_output("movzx16 %s,0(%lx)", gpn(r),(unsigned long)addr);
    }

    inline void Assembler::LD16Zsib(R r, I32 disp, R base, I32 index, I32 scale) {
        count_ld();
        ALU2sib(0x0fb7, r, base, index, scale, disp);
        asm_output("movzx16 %s,%d(%s+%s*%c)",gpn(r),disp,gpn(base),gpn(index),SIBIDX(scale));
    }

    // Load 8-bit, zero extend.
    inline void Assembler::LD8Z(R r, I32 d, R b) {
        count_ld();
        ALU2m(0x0fb6, r, d, b);
        asm_output("movzx8 %s,%d(%s)", gpn(r),d,gpn(b));
    }

    inline void Assembler::LD8Zdm(R r, I32 addr) {
        count_ld();
        ALU2dm(0x0fb6, r, addr);
        asm_output("movzx8 %s,0(%lx)", gpn(r),(long unsigned)addr);
    }

    inline void Assembler::LD8Zsib(R r, I32 disp, R base, I32 index, I32 scale) {
        count_ld();
        ALU2sib(0x0fb6, r, base, index, scale, disp);
        asm_output("movzx8 %s,%d(%s+%s*%c)",gpn(r),disp,gpn(base),gpn(index),SIBIDX(scale));
    }

    // Load 8-bit, sign extend.
    inline void Assembler::LD8S(R r, I32 d, R b) {
        count_ld();
        ALU2m(0x0fbe, r, d, b);
        asm_output("movsx8 %s,%d(%s)", gpn(r),d,gpn(b));
    }

    inline void Assembler::LD8Sdm(R r, I32 addr) {
        count_ld();
        ALU2dm(0x0fbe, r, addr);
        asm_output("movsx8 %s,0(%lx)", gpn(r),(long unsigned)addr);
    }

    inline void Assembler::LD8Ssib(R r, I32 disp, R base, I32 index, I32 scale) {
        count_ld();
        ALU2sib(0x0fbe, r, base, index, scale, disp);
        asm_output("movsx8 %s,%d(%s+%s*%c)",gpn(r),disp,gpn(base),gpn(index),SIBIDX(scale));
    }

    inline void Assembler::LDi(R r, I32 i) {
        count_ld();
        underrunProtect(5);
        IMM32(i);
        NanoAssert(((unsigned)r)<8);
        *(--_nIns) = (uint8_t) ( 0xb8 | r );
        asm_output("mov %s,%d",gpn(r),i);
    }

    // Quirk of x86-32: reg must be a/b/c/d for byte stores here.
    inline void Assembler::ST8(R base, I32 disp, R reg) {
        count_st();
        NanoAssert(((unsigned)reg)<4);
        ALUm(0x88, reg, disp, base);
        asm_output("mov8 %d(%s),%s",disp,base==UnspecifiedReg?"0":gpn(base),gpn(reg));
    }

    inline void Assembler::ST16(R base, I32 disp, R reg) {
        count_st();
        ALUm16(0x89, reg, disp, base);
        asm_output("mov16 %d(%s),%s",disp,base==UnspecifiedReg?"0":gpn(base),gpn(reg));
    }

    inline void Assembler::ST(R base, I32 disp, R reg) {
        count_st();
        ALUm(0x89, reg, disp, base);
        asm_output("mov %d(%s),%s",disp,base==UnspecifiedReg?"0":gpn(base),gpn(reg));
    }

    inline void Assembler::ST8i(R base, I32 disp, I32 imm) {
        count_st();
        underrunProtect(8);
        IMM8(imm);
        MODRMm(0, disp, base);
        *(--_nIns) = 0xc6;
        asm_output("mov8 %d(%s),%d",disp,gpn(base),imm);
    }

    inline void Assembler::ST16i(R base, I32 disp, I32 imm) {
        count_st();
        underrunProtect(10);
        IMM16(imm);
        MODRMm(0, disp, base);
        *(--_nIns) = 0xc7;
        *(--_nIns) = 0x66;
        asm_output("mov16 %d(%s),%d",disp,gpn(base),imm);
    }

    inline void Assembler::STi(R base, I32 disp, I32 imm) {
        count_st();
        underrunProtect(11);
        IMM32(imm);
        MODRMm(0, disp, base);
        *(--_nIns) = 0xc7;
        asm_output("mov %d(%s),%d",disp,gpn(base),imm);
    }

    inline void Assembler::RET()   { count_ret(); ALU0(0xc3); asm_output("ret"); }
    inline void Assembler::NOP()   { count_alu(); ALU0(0x90); asm_output("nop"); }
    inline void Assembler::INT3()  {              ALU0(0xcc); asm_output("int3"); }

    inline void Assembler::PUSHi(I32 i) {
        count_push();
        if (isS8(i)) {
            underrunProtect(2);
            _nIns-=2; _nIns[0] = 0x6a; _nIns[1] = uint8_t(i);
            asm_output("push %d",i);
        } else {
            PUSHi32(i);
        }
    }

    inline void Assembler::PUSHi32(I32 i) {
        count_push();
        underrunProtect(5);
        IMM32(i);
        *(--_nIns) = 0x68;
        asm_output("push %d",i);
    }

    inline void Assembler::PUSHr(R r) {
        count_push();
        underrunProtect(1);
        NanoAssert(((unsigned)r)<8);
        *(--_nIns) = (uint8_t) ( 0x50 | r );
        asm_output("push %s",gpn(r));
    }

    inline void Assembler::PUSHm(I32 d, R b) {
        count_pushld();
        ALUm(0xff, 6, d, b);
        asm_output("push %d(%s)",d,gpn(b));
    }

    inline void Assembler::POPr(R r) {
        count_pop();
        underrunProtect(1);
        NanoAssert(((unsigned)r)<8);
        *(--_nIns) = (uint8_t) ( 0x58 | (r) );
        asm_output("pop %s",gpn(r));
    }

    inline void Assembler::JCC(I32 o, NIns* t, const char* n) {
        count_jcc();
        underrunProtect(6);
        intptr_t tt = (intptr_t)t - (intptr_t)_nIns;
        if (t && isS8(tt)) {
            _nIns -= 2;
            _nIns[0] = uint8_t( 0x70 | o );
            _nIns[1] = uint8_t(tt);
        } else {
            IMM32(tt);
            _nIns -= 2;
            _nIns[0] = JCC32;
            _nIns[1] = (uint8_t) ( 0x80 | o );
        }
        asm_output("%-5s %p", n, t); (void) n;
    }

    inline void Assembler::JMP_long(NIns* t) {
        count_jmp();
        underrunProtect(5);
        NanoAssert(t);
        intptr_t tt = (intptr_t)t - (intptr_t)_nIns;
        IMM32(tt); \
        *(--_nIns) = JMP32; \
        asm_output("jmp %p", t); \
        verbose_only( verbose_outputf("%010lx:", (unsigned long)_nIns); )
    }

    inline void Assembler::JMP_indirect(R r) {
        underrunProtect(2);
        MODRMm(4, 0, r);
        *(--_nIns) = 0xff;
        asm_output("jmp   *(%s)", gpn(r));
    }

    inline void Assembler::JMP_indexed(Register x, I32 ss, NIns** addr) {
        underrunProtect(7);
        IMM32(int32_t(addr));
        _nIns -= 3;
        _nIns[0]   = (NIns) 0xff; /* jmp */
        _nIns[1]   = (NIns) (0<<6 | 4<<3 | 4); /* modrm: base=sib + disp32 */
        _nIns[2]   = (NIns) (ss<<6 | (x)<<3 | 5); /* sib: x<<ss + table */
        asm_output("jmp   *(%s*%d+%p)", gpn(x), 1<<ss, (void*)(addr));
    }

    inline void Assembler::JE(NIns* t)   { JCC(0x04, t, "je"); }
    inline void Assembler::JNE(NIns* t)  { JCC(0x05, t, "jne"); }
    inline void Assembler::JP(NIns* t)   { JCC(0x0A, t, "jp"); }
    inline void Assembler::JNP(NIns* t)  { JCC(0x0B, t, "jnp"); }

    inline void Assembler::JB(NIns* t)   { JCC(0x02, t, "jb"); }
    inline void Assembler::JNB(NIns* t)  { JCC(0x03, t, "jnb"); }
    inline void Assembler::JBE(NIns* t)  { JCC(0x06, t, "jbe"); }
    inline void Assembler::JNBE(NIns* t) { JCC(0x07, t, "jnbe"); }

    inline void Assembler::JA(NIns* t)   { JCC(0x07, t, "ja"); }
    inline void Assembler::JNA(NIns* t)  { JCC(0x06, t, "jna"); }
    inline void Assembler::JAE(NIns* t)  { JCC(0x03, t, "jae"); }
    inline void Assembler::JNAE(NIns* t) { JCC(0x02, t, "jnae"); }

    inline void Assembler::JL(NIns* t)   { JCC(0x0C, t, "jl"); }
    inline void Assembler::JNL(NIns* t)  { JCC(0x0D, t, "jnl"); }
    inline void Assembler::JLE(NIns* t)  { JCC(0x0E, t, "jle"); }
    inline void Assembler::JNLE(NIns* t) { JCC(0x0F, t, "jnle"); }

    inline void Assembler::JG(NIns* t)   { JCC(0x0F, t, "jg"); }
    inline void Assembler::JNG(NIns* t)  { JCC(0x0E, t, "jng"); }
    inline void Assembler::JGE(NIns* t)  { JCC(0x0D, t, "jge"); }
    inline void Assembler::JNGE(NIns* t) { JCC(0x0C, t, "jnge"); }

    inline void Assembler::JO(NIns* t)   { JCC(0x00, t, "jo"); }
    inline void Assembler::JNO(NIns* t)  { JCC(0x01, t, "jno"); }

    // sse instructions
    inline void Assembler::SSE(I32 c, I32 d, I32 s) {
        underrunProtect(9);
        MODRM((d),(s));
        _nIns -= 3;
        _nIns[0] = uint8_t((c>>16) & 0xff);
        _nIns[1] = uint8_t((c>>8) & 0xff);
        _nIns[2] = uint8_t(c&0xff);
    }

    inline void Assembler::SSEm(I32 c, I32 r, I32 d, R b) {
        underrunProtect(9);
        MODRMm(r, d, b);
        _nIns -= 3;
        _nIns[0] = uint8_t((c>>16) & 0xff);
        _nIns[1] = uint8_t((c>>8) & 0xff);
        _nIns[2] = uint8_t(c & 0xff);
    }

    inline void Assembler::LDSDm(R r, const double* addr) {
        count_ldq();
        underrunProtect(8);
        IMM32(int32_t(addr));
        *(--_nIns) = uint8_t(((r)&7)<<3|5);
        *(--_nIns) = 0x10;
        *(--_nIns) = 0x0f;
        *(--_nIns) = 0xf2;
        asm_output("movsd %s,(%p) // =%f",gpn(r),(void*)addr,*addr);
    }

    inline void Assembler::SSE_LDSD(R r, I32 d, R b) { count_ldq(); SSEm(0xf20f10, r&7, d, b); asm_output("movsd %s,%d(%s)",gpn(r),(d),gpn(b)); }
    inline void Assembler::SSE_LDQ( R r, I32 d, R b) { count_ldq(); SSEm(0xf30f7e, r&7, d, b); asm_output("movq %s,%d(%s)",gpn(r),d,gpn(b)); }
    inline void Assembler::SSE_LDSS(R r, I32 d, R b) { count_ld();  SSEm(0xf30f10, r&7, d, b); asm_output("movss %s,%d(%s)",gpn(r),d,gpn(b)); }
    inline void Assembler::SSE_STSD(I32 d, R b, R r) { count_stq(); SSEm(0xf20f11, r&7, d, b); asm_output("movsd %d(%s),%s",(d),gpn(b),gpn(r)); }
    inline void Assembler::SSE_STQ( I32 d, R b, R r) { count_stq(); SSEm(0x660fd6, r&7, d, b); asm_output("movq %d(%s),%s",(d),gpn(b),gpn(r)); }
    inline void Assembler::SSE_STSS(I32 d, R b, R r) { count_st();  SSEm(0xf30f11, r&7, d, b); asm_output("movss %d(%s),%s",(d),gpn(b),gpn(r)); }

    inline void Assembler::SSE_CVTSI2SD(R xr, R gr)  { count_fpu(); SSE(0xf20f2a, xr&7, gr&7); asm_output("cvtsi2sd %s,%s",gpn(xr),gpn(gr)); }
    inline void Assembler::SSE_CVTSD2SI(R gr, R xr)  { count_fpu(); SSE(0xf20f2d, gr&7, xr&7); asm_output("cvtsd2si %s,%s",gpn(gr),gpn(xr)); }
    inline void Assembler::SSE_CVTSD2SS(R xr, R gr)  { count_fpu(); SSE(0xf20f5a, xr&7, gr&7); asm_output("cvtsd2ss %s,%s",gpn(xr),gpn(gr)); }
    inline void Assembler::SSE_CVTSS2SD(R xr, R gr)  { count_fpu(); SSE(0xf30f5a, xr&7, gr&7); asm_output("cvtss2sd %s,%s",gpn(xr),gpn(gr)); }
    inline void Assembler::SSE_CVTDQ2PD(R d,  R r)   { count_fpu(); SSE(0xf30fe6, d&7,  r&7);  asm_output("cvtdq2pd %s,%s",gpn(d),gpn(r)); }

    // Move and zero-extend GP reg to XMM reg.
    inline void Assembler::SSE_MOVD(R d, R s) {
        count_mov();
        if (_is_xmm_reg_(s)) {
            NanoAssert(_is_gp_reg_(d));
            SSE(0x660f7e, s&7, d&7);
        } else {
            NanoAssert(_is_gp_reg_(s));
            NanoAssert(_is_xmm_reg_(d));
            SSE(0x660f6e, d&7, s&7);
        }
        asm_output("movd %s,%s",gpn(d),gpn(s));
    }

    inline void Assembler::SSE_MOVSD(R rd, R rs) {
        count_mov();
        NanoAssert(_is_xmm_reg_(rd) && _is_xmm_reg_(rs));
        SSE(0xf20f10, rd&7, rs&7);
        asm_output("movsd %s,%s",gpn(rd),gpn(rs));
    }

    inline void Assembler::SSE_MOVDm(R d, R b, R xrs) {
        count_st();
        NanoAssert(_is_xmm_reg_(xrs) && (_is_gp_reg_(b) || b==FP));
        SSEm(0x660f7e, xrs&7, d, b);
        asm_output("movd %d(%s),%s", d, gpn(b), gpn(xrs));
    }

    inline void Assembler::SSE_ADDSD(R rd, R rs) {
        count_fpu();
        NanoAssert(_is_xmm_reg_(rd) && _is_xmm_reg_(rs));
        SSE(0xf20f58, rd&7, rs&7);
        asm_output("addsd %s,%s",gpn(rd),gpn(rs));
    }

    inline void Assembler::SSE_ADDSDm(R r, const double* addr) {
        count_fpuld();
        underrunProtect(8);
        NanoAssert(_is_xmm_reg_(r));
        const double* daddr = addr;
        IMM32(int32_t(daddr));
        *(--_nIns) = uint8_t((r&7)<<3 | 5);
        *(--_nIns) = 0x58;
        *(--_nIns) = 0x0f;
        *(--_nIns) = 0xf2;
        asm_output("addsd %s,%p // =%f",gpn(r),(void*)daddr,*daddr);
    }

    inline void Assembler::SSE_SUBSD(R rd, R rs) {
        count_fpu();
        NanoAssert(_is_xmm_reg_(rd) && _is_xmm_reg_(rs));
        SSE(0xf20f5c, rd&7, rs&7);
        asm_output("subsd %s,%s",gpn(rd),gpn(rs));
    }

    inline void Assembler::SSE_MULSD(R rd, R rs) {
        count_fpu();
        NanoAssert(_is_xmm_reg_(rd) && _is_xmm_reg_(rs));
        SSE(0xf20f59, rd&7, rs&7);
        asm_output("mulsd %s,%s",gpn(rd),gpn(rs));
    }

    inline void Assembler::SSE_DIVSD(R rd, R rs) {
        count_fpu();
        NanoAssert(_is_xmm_reg_(rd) && _is_xmm_reg_(rs));
        SSE(0xf20f5e, rd&7, rs&7);
        asm_output("divsd %s,%s",gpn(rd),gpn(rs));
    }

    inline void Assembler::SSE_UCOMISD(R rl, R rr) {
        count_fpu();
        NanoAssert(_is_xmm_reg_(rl) && _is_xmm_reg_(rr));
        SSE(0x660f2e, rl&7, rr&7);
        asm_output("ucomisd %s,%s",gpn(rl),gpn(rr));
    }

    inline void Assembler::SSE_CVTSI2SDm(R xr, R d, R b) {
        count_fpu();
        NanoAssert(_is_xmm_reg_(xr) && _is_gp_reg_(b));
        SSEm(0xf20f2a, xr&7, d, b);
        asm_output("cvtsi2sd %s,%d(%s)",gpn(xr),d,gpn(b));
    }

    inline void Assembler::SSE_XORPD(R r, const uint32_t* maskaddr) {
        count_fpuld();
        underrunProtect(8);
        IMM32(int32_t(maskaddr));
        *(--_nIns) = uint8_t((r&7)<<3 | 5);
        *(--_nIns) = 0x57;
        *(--_nIns) = 0x0f;
        *(--_nIns) = 0x66;
        asm_output("xorpd %s,[%p]",gpn(r),(void*)maskaddr);
    }

    inline void Assembler::SSE_XORPDr(R rd, R rs) {
        count_fpu();
        SSE(0x660f57, rd&7, rs&7);
        asm_output("xorpd %s,%s",gpn(rd),gpn(rs));
    }

    // floating point unit
    inline void Assembler::FPUc(I32 o) {
        underrunProtect(2);
        *(--_nIns) = (uint8_t)(o & 0xff);
        *(--_nIns) = (uint8_t)((o>>8) & 0xff);
    }

    inline void Assembler::FPUm(I32 o, I32 d, R b) {
        underrunProtect(7);
        MODRMm(uint8_t(o), d, b);
        *(--_nIns) = (uint8_t)(o>>8);
    }

    inline void Assembler::FPUdm(I32 o, const double* const m) {
        underrunProtect(6);
        MODRMdm(uint8_t(o), int32_t(m));
        *(--_nIns) = uint8_t(o>>8);
    }

    inline void Assembler::TEST_AH(I32 i) {
        count_alu();
        underrunProtect(3);
        *(--_nIns) = uint8_t(i);
        *(--_nIns) = 0xc4;
        *(--_nIns) = 0xf6;
        asm_output("test ah, %d",i);
    }

    inline void Assembler::TEST_AX(I32 i) {
        count_fpu();
        underrunProtect(5);
        *(--_nIns) = 0;
        *(--_nIns) = uint8_t(i);
        *(--_nIns) = uint8_t((i)>>8);
        *(--_nIns) = 0;
        *(--_nIns) = 0xa9;
        asm_output("test ax, %d",i);
    }

    inline void Assembler::FNSTSW_AX() { count_fpu(); FPUc(0xdfe0);    asm_output("fnstsw_ax"); }
    inline void Assembler::FCHS()      { count_fpu(); FPUc(0xd9e0);    asm_output("fchs"); }
    inline void Assembler::FLD1()      { count_fpu(); FPUc(0xd9e8);    asm_output("fld1"); fpu_push(); }
    inline void Assembler::FLDZ()      { count_fpu(); FPUc(0xd9ee);    asm_output("fldz"); fpu_push(); }

    inline void Assembler::FFREE(R r)  { count_fpu(); FPU(0xddc0, r);  asm_output("ffree %s",gpn(r)); }

    inline void Assembler::FST32(bool p, I32 d, R b){ count_stq(); FPUm(0xd902|(p?1:0), d, b);   asm_output("fst%s32 %d(%s)",(p?"p":""),d,gpn(b)); if (p) fpu_pop(); }
    inline void Assembler::FSTQ(bool p, I32 d, R b) { count_stq(); FPUm(0xdd02|(p?1:0), d, b);   asm_output("fst%sq %d(%s)",(p?"p":""),d,gpn(b)); if (p) fpu_pop(); }

    inline void Assembler::FSTPQ(I32 d, R b) { FSTQ(1, d, b); }

    inline void Assembler::FCOM(bool p, I32 d, R b) { count_fpuld(); FPUm(0xdc02|(p?1:0), d, b); asm_output("fcom%s %d(%s)",(p?"p":""),d,gpn(b)); if (p) fpu_pop(); }
    inline void Assembler::FCOMdm(bool p, const double* dm) {
        count_fpuld();
        FPUdm(0xdc02|(p?1:0), dm);
        asm_output("fcom%s (%p)",(p?"p":""),(void*)dm);
        if (p) fpu_pop();
    }

    inline void Assembler::FLD32(I32 d, R b)        { count_ldq();   FPUm(0xd900, d, b); asm_output("fld32 %d(%s)",d,gpn(b)); fpu_push();}
    inline void Assembler::FLDQ(I32 d, R b)         { count_ldq();   FPUm(0xdd00, d, b); asm_output("fldq %d(%s)",d,gpn(b)); fpu_push();}
    inline void Assembler::FLDQdm(const double* dm) { count_ldq();   FPUdm(0xdd00, dm);  asm_output("fldq (%p)",(void*)dm); fpu_push();}
    inline void Assembler::FILDQ(I32 d, R b)        { count_fpuld(); FPUm(0xdf05, d, b); asm_output("fildq %d(%s)",d,gpn(b)); fpu_push(); }
    inline void Assembler::FILD(I32 d, R b)         { count_fpuld(); FPUm(0xdb00, d, b); asm_output("fild %d(%s)",d,gpn(b)); fpu_push(); }

    inline void Assembler::FIST(bool p, I32 d, R b) {
        count_fpu();
        FPUm(0xdb02|(p?1:0), d, b);
        asm_output("fist%s %d(%s)",(p?"p":""),d,gpn(b));
        if (p) fpu_pop();
    }

    inline void Assembler::FADD( I32 d, R b) { count_fpu(); FPUm(0xdc00, d, b); asm_output("fadd %d(%s)", d,gpn(b)); }
    inline void Assembler::FSUB( I32 d, R b) { count_fpu(); FPUm(0xdc04, d, b); asm_output("fsub %d(%s)", d,gpn(b)); }
    inline void Assembler::FSUBR(I32 d, R b) { count_fpu(); FPUm(0xdc05, d, b); asm_output("fsubr %d(%s)",d,gpn(b)); }
    inline void Assembler::FMUL( I32 d, R b) { count_fpu(); FPUm(0xdc01, d, b); asm_output("fmul %d(%s)", d,gpn(b)); }
    inline void Assembler::FDIV( I32 d, R b) { count_fpu(); FPUm(0xdc06, d, b); asm_output("fdiv %d(%s)", d,gpn(b)); }
    inline void Assembler::FDIVR(I32 d, R b) { count_fpu(); FPUm(0xdc07, d, b); asm_output("fdivr %d(%s)",d,gpn(b)); }

    inline void Assembler::FADDdm( const double *dm) { count_ldq(); FPUdm(0xdc00, dm); asm_output("fadd (%p)", (void*)dm); }
    inline void Assembler::FSUBRdm(const double* dm) { count_ldq(); FPUdm(0xdc05, dm); asm_output("fsubr (%p)",(void*)dm); }
    inline void Assembler::FMULdm( const double* dm) { count_ldq(); FPUdm(0xdc01, dm); asm_output("fmul (%p)", (void*)dm); }
    inline void Assembler::FDIVRdm(const double* dm) { count_ldq(); FPUdm(0xdc07, dm); asm_output("fdivr (%p)",(void*)dm); }

    inline void Assembler::FINCSTP()   { count_fpu(); FPUc(0xd9f7);    asm_output("fincstp"); }

    inline void Assembler::FCOMP()     { count_fpu(); FPUc(0xD8D9);    asm_output("fcomp"); fpu_pop();}
    inline void Assembler::FCOMPP()    { count_fpu(); FPUc(0xDED9);    asm_output("fcompp"); fpu_pop();fpu_pop();}
    inline void Assembler::FLDr(R r)   { count_ldq(); FPU(0xd9c0,r);   asm_output("fld %s",gpn(r)); fpu_push(); }
    inline void Assembler::EMMS()      { count_fpu(); FPUc(0x0f77);    asm_output("emms"); }

    // standard direct call
    inline void Assembler::CALL(const CallInfo* ci) {
        count_call();
        underrunProtect(5);
        int offset = (ci->_address) - ((int)_nIns);
        IMM32( (uint32_t)offset );
        *(--_nIns) = 0xE8;
        verbose_only(asm_output("call %s",(ci->_name));)
        debug_only(if (ci->returnType()==ARGTYPE_D) fpu_push();)
    }

    // indirect call thru register
    inline void Assembler::CALLr(const CallInfo* ci, Register r) {
        count_calli();
        underrunProtect(2);
        ALU(0xff, 2, (r));
        verbose_only(asm_output("call %s",gpn(r));)
        debug_only(if (ci->returnType()==ARGTYPE_D) fpu_push();) (void)ci;
    }

    void Assembler::nInit(AvmCore*)
    {
        nHints[LIR_calli]  = rmask(retRegs[0]);
        nHints[LIR_calld]  = rmask(FST0);
        nHints[LIR_paramp] = PREFER_SPECIAL;
        nHints[LIR_immi]   = ScratchRegs;
        // Nb: Doing this with a loop future-proofs against the possibilty of
        // new comparison operations being added.
        for (LOpcode op = LOpcode(0); op < LIR_sentinel; op = LOpcode(op+1))
            if (isCmpOpcode(op))
                nHints[op] = AllowableFlagRegs;
    }

    void Assembler::nBeginAssembly() {
        max_stk_args = 0;
    }

    NIns* Assembler::genPrologue()
    {
        // Prologue
        uint32_t stackNeeded = max_stk_args + STACK_GRANULARITY * _activation.stackSlotsNeeded();

        uint32_t stackPushed =
            STACK_GRANULARITY + // returnaddr
            STACK_GRANULARITY; // ebp

        uint32_t aligned = alignUp(stackNeeded + stackPushed, NJ_ALIGN_STACK);
        uint32_t amt = aligned - stackPushed;

        // Reserve stackNeeded bytes, padded
        // to preserve NJ_ALIGN_STACK-byte alignment.
        if (amt)
        {
            SUBi(SP, amt);
        }

        verbose_only( asm_output("[frag entry]"); )
        NIns *fragEntry = _nIns;
        MR(FP, SP); // Establish our own FP.
        PUSHr(FP); // Save caller's FP.

        return fragEntry;
    }

    void Assembler::nFragExit(LIns* guard)
    {
        SideExit *exit = guard->record()->exit;
        Fragment *frag = exit->target;
        GuardRecord *lr = 0;
        bool destKnown = (frag && frag->fragEntry);

        // Generate jump to epilog and initialize lr.
        // If the guard is LIR_xtbl, use a jump table with epilog in every entry
        if (guard->isop(LIR_xtbl)) {
            lr = guard->record();
            Register r = EDX;
            SwitchInfo* si = guard->record()->exit->switchInfo;
            if (!_epilogue)
                _epilogue = genEpilogue();
            emitJumpTable(si, _epilogue);
            JMP_indirect(r);
            LEAmi4(r, int32_t(si->table), r);
        } else {
            // If the guard already exists, use a simple jump.
            if (destKnown) {
                JMP(frag->fragEntry);
                lr = 0;
            } else {  // Target doesn't exist. Jump to an epilogue for now. This can be patched later.
                if (!_epilogue)
                    _epilogue = genEpilogue();
                lr = guard->record();
                JMP_long(_epilogue);
                lr->jmp = _nIns;
            }
        }

        // profiling for the exit
        verbose_only(
           if (_logc->lcbits & LC_FragProfile) {
              INCLi( int32_t(&guard->record()->profCount) );
           }
        )

        // Restore ESP from EBP, undoing SUBi(SP,amt) in the prologue
        MR(SP,FP);

        // return value is GuardRecord*
        asm_immi(EAX, int(lr), /*canClobberCCs*/true);
    }

    NIns *Assembler::genEpilogue()
    {
        RET();
        POPr(FP); // Restore caller's FP.

        return  _nIns;
    }

    void Assembler::asm_call(LIns* ins)
    {
        Register rr = ( ins->isop(LIR_calld) ? FST0 : retRegs[0] );
        prepareResultReg(ins, rmask(rr));

        evictScratchRegsExcept(rmask(rr));

        const CallInfo* call = ins->callInfo();
        // must be signed, not unsigned
        uint32_t iargs = call->count_int32_args();
        int32_t fargs = call->count_args() - iargs;

        bool indirect = call->isIndirect();
        if (indirect) {
            // target arg isn't pushed, its consumed in the call
            iargs --;
        }

        AbiKind abi = call->_abi;
        uint32_t max_regs = max_abi_regs[abi];
        if (max_regs > iargs)
            max_regs = iargs;

        int32_t istack = iargs-max_regs;  // first 2 4B args are in registers
        int32_t extra = 0;
        const int32_t pushsize = 4*istack + 8*fargs; // actual stack space used

#if _MSC_VER
        // msc only provides 4-byte alignment, anything more than 4 on windows
        // x86-32 requires dynamic ESP alignment in prolog/epilog and static
        // esp-alignment here.
        uint32_t align = 4;//NJ_ALIGN_STACK;
#else
        uint32_t align = NJ_ALIGN_STACK;
#endif

        if (pushsize) {
            if (_config.i386_fixed_esp) {
                // In case of fastcall, stdcall and thiscall the callee cleans up the stack,
                // and since we reserve max_stk_args words in the prolog to call functions
                // and don't adjust the stack pointer individually for each call we have
                // to undo here any changes the callee just did to the stack.
                if (abi != ABI_CDECL)
                    SUBi(SP, pushsize);
            } else {
                // stack re-alignment
                // only pop our adjustment amount since callee pops args in FASTCALL mode
                extra = alignUp(pushsize, align) - pushsize;
                if (call->_abi == ABI_CDECL) {
                    // with CDECL only, caller pops args
                    ADDi(SP, extra+pushsize);
                } else if (extra > 0) {
                    ADDi(SP, extra);
                }
            }
        }

        NanoAssert(ins->isop(LIR_callp) || ins->isop(LIR_calld));
        if (!indirect) {
            CALL(call);
        }
        else {
            // Indirect call.  x86 Calling conventions don't use EAX as an
            // argument, and do use EAX as a return value.  We need a register
            // for the address to call, so we use EAX since it will always be
            // available.
            CALLr(call, EAX);
        }

        // Call this now so that the arg setup can involve 'rr'.
        freeResourcesOf(ins);

        // Make sure fpu stack is empty before call.
        NanoAssert(_allocator.isFree(FST0));

        // Pre-assign registers to the first N 4B args based on the calling convention.
        uint32_t n = 0;

        ArgType argTypes[MAXARGS];
        uint32_t argc = call->getArgTypes(argTypes);
        int32_t stkd = 0;

        if (indirect) {
            argc--;
            asm_arg(ARGTYPE_P, ins->arg(argc), EAX, stkd);
            if (!_config.i386_fixed_esp)
                stkd = 0;
        }

        for (uint32_t i = 0; i < argc; i++)
        {
            uint32_t j = argc-i-1;
            ArgType ty = argTypes[j];
            Register r = UnspecifiedReg;
            if (n < max_regs && ty != ARGTYPE_D) {
                r = argRegs[n++]; // tell asm_arg what reg to use
            }
            asm_arg(ty, ins->arg(j), r, stkd);
            if (!_config.i386_fixed_esp)
                stkd = 0;
        }

        if (_config.i386_fixed_esp) {
            if (pushsize > max_stk_args)
                max_stk_args = pushsize;
        } else if (extra > 0) {
            SUBi(SP, extra);
        }
    }

    Register Assembler::nRegisterAllocFromSet(RegisterMask set)
    {
        Register r;
        RegAlloc &regs = _allocator;
    #ifdef _MSC_VER
        _asm
        {
            mov ecx, regs
            bsf eax, set                    // i = first bit set
            btr RegAlloc::free[ecx], eax    // free &= ~rmask(i)
            mov r, eax
        }
    #elif defined __SUNPRO_CC
        // Workaround for Sun Studio bug on handler embeded asm code.
        // See bug 544447 for detail.
        // https://bugzilla.mozilla.org/show_bug.cgi?id=544447
         asm(
             "bsf    %1, %%edi\n\t"
             "btr    %%edi, (%2)\n\t"
             "movl   %%edi, %0\n\t"
             : "=a"(r) : "d"(set), "c"(&regs.free) : "%edi", "memory" );
    #else
        asm(
            "bsf    %1, %%eax\n\t"
            "btr    %%eax, %2\n\t"
            "movl   %%eax, %0\n\t"
            : "=m"(r) : "m"(set), "m"(regs.free) : "%eax", "memory" );
    #endif /* _MSC_VER */
        return r;
    }

    void Assembler::nRegisterResetAll(RegAlloc& a)
    {
        // add scratch registers to our free list for the allocator
        a.clear();
        a.free = SavedRegs | ScratchRegs;
        if (!_config.i386_sse2)
            a.free &= ~XmmRegs;
        debug_only( a.managed = a.free; )
    }

    void Assembler::nPatchBranch(NIns* branch, NIns* targ)
    {
        intptr_t offset = intptr_t(targ) - intptr_t(branch);
        if (branch[0] == JMP32) {
            *(int32_t*)&branch[1] = offset - 5;
        } else if (branch[0] == JCC32) {
            *(int32_t*)&branch[2] = offset - 6;
        } else
            NanoAssertMsg(0, "Unknown branch type in nPatchBranch");
    }

    RegisterMask Assembler::nHint(LIns* ins)
    {
        NanoAssert(ins->isop(LIR_paramp));
        RegisterMask prefer = 0;
        uint8_t arg = ins->paramArg();
        if (ins->paramKind() == 0) {
            uint32_t max_regs = max_abi_regs[_thisfrag->lirbuf->abi];
            if (arg < max_regs) 
                prefer = rmask(argRegs[arg]);
        } else {
            if (arg < NumSavedRegs)
                prefer = rmask(savedRegs[arg]);
        }
        return prefer;
    }

    // Return true if we can generate code for this instruction that neither
    // sets CCs nor clobbers any input register.
    // LEA is the only native instruction that fits those requirements.
    bool canRematLEA(LIns* ins)
    {
        if (ins->isop(LIR_addi))
            return ins->oprnd1()->isInReg() && ins->oprnd2()->isImmI();
        // Subtract and some left-shifts could be rematerialized using LEA,
        // but it hasn't shown to help in real code yet.  Noting them anyway:
        // maybe sub? R = subl rL, const  =>  leal R, [rL + -const]
        // maybe lsh? R = lshl rL, 1/2/3  =>  leal R, [rL * 2/4/8]
        return false;
    }

    bool Assembler::canRemat(LIns* ins)
    {
        return ins->isImmAny() || ins->isop(LIR_allocp) || canRematLEA(ins);
    }

    // WARNING: the code generated by this function must not affect the
    // condition codes.  See asm_cmp().
    void Assembler::asm_restore(LIns* ins, Register r)
    {
        NanoAssert(ins->getReg() == r);

        uint32_t arg;
        uint32_t abi_regcount;
        if (ins->isop(LIR_allocp)) {
            // The value of a LIR_allocp instruction is the address of the
            // stack allocation.  We can rematerialize that from the record we
            // have of where the allocation lies in the stack.
            NanoAssert(ins->isInAr());  // must have stack slots allocated
            LEA(r, arDisp(ins), FP);

        } else if (ins->isImmI()) {
            asm_immi(r, ins->immI(), /*canClobberCCs*/false);

        } else if (ins->isImmD()) {
            asm_immd(r, ins->immDasQ(), ins->immD(), /*canClobberCCs*/false);

        } else if (ins->isop(LIR_paramp) && ins->paramKind() == 0 &&
            (arg = ins->paramArg()) >= (abi_regcount = max_abi_regs[_thisfrag->lirbuf->abi])) {
            // Incoming arg is on stack, can restore it from there instead of spilling.

            // this case is intentionally not detected in canRemat(), because we still
            // emit a load instead of a fast ALU operation.  We don't want parameter
            // spills to have precedence over immediates & ALU ops, but if one does
            // spill, we want to load it directly from its stack area, saving a store
            // in the prolog.

            // Compute position of argument relative to ebp.  Higher argument
            // numbers are at higher positive offsets.  The first abi_regcount
            // arguments are in registers, rest on stack.  +8 accomodates the
            // return address and saved ebp value.  Assuming abi_regcount == 0:
            //
            //    low-addr  ebp
            //    [frame...][saved-ebp][return-addr][arg0][arg1]...
            //
            int d = (arg - abi_regcount) * sizeof(intptr_t) + 8;
            LD(r, d, FP);

        } else if (canRematLEA(ins)) {
            LEA(r, ins->oprnd2()->immI(), ins->oprnd1()->getReg());

        } else {
            int d = findMemFor(ins);
            if (ins->isI()) {
                NanoAssert(rmask(r) & GpRegs);
                LD(r, d, FP);
            } else {
                NanoAssert(ins->isD());
                if (rmask(r) & XmmRegs) {
                    SSE_LDQ(r, d, FP);
                } else {
                    NanoAssert(rmask(r) & x87Regs);
                    FLDQ(d, FP);
                }
            }
        }
    }

    void Assembler::asm_store32(LOpcode op, LIns* value, int dr, LIns* base)
    {
        if (value->isImmI()) {
            Register rb = getBaseReg(base, dr, GpRegs);
            int c = value->immI();
            switch (op) {
                case LIR_sti2c:
                    ST8i(rb, dr, c);
                    break;
                case LIR_sti2s:
                    ST16i(rb, dr, c);
                    break;
                case LIR_sti:
                    STi(rb, dr, c);
                    break;
                default:
                    NanoAssertMsg(0, "asm_store32 should never receive this LIR opcode");
                    break;
            }
        }
        else
        {
            // Quirk of x86-32: reg must be a/b/c/d for single-byte stores.
            const RegisterMask SrcRegs = (op == LIR_sti2c) ?
                            (1<<EAX | 1<<ECX | 1<<EDX | 1<<EBX) :
                            GpRegs;

            Register ra, rb;
            if (base->isImmI()) {
                // absolute address
                rb = UnspecifiedReg;
                dr += base->immI();
                ra = findRegFor(value, SrcRegs);
            } else {
                getBaseReg2(SrcRegs, value, ra, GpRegs, base, rb, dr);
            }
            switch (op) {
                case LIR_sti2c:
                    ST8(rb, dr, ra);
                    break;
                case LIR_sti2s:
                    ST16(rb, dr, ra);
                    break;
                case LIR_sti:
                    ST(rb, dr, ra);
                    break;
                default:
                    NanoAssertMsg(0, "asm_store32 should never receive this LIR opcode");
                    break;
            }
        }
    }

    void Assembler::asm_spill(Register rr, int d, bool pop, bool quad)
    {
        (void)quad;
        NanoAssert(d);
        if (rmask(rr) & GpRegs) {
            ST(FP, d, rr);
        } else if (rmask(rr) & XmmRegs) {
            SSE_STQ(d, FP, rr);
        } else {
            NanoAssert(rmask(rr) & x87Regs);
            FSTQ((pop?1:0), d, FP);
        }
    }

    void Assembler::asm_load64(LIns* ins)
    {
        LIns* base = ins->oprnd1();
        int db = ins->disp();

        Register rb = getBaseReg(base, db, GpRegs);

        // There are two cases:
        // - 'ins' is in FpRegs: load it.
        // - otherwise: there's no point loading the value into a register
        //   because its only use will be to immediately spill it.  Instead we
        //   do a memory-to-memory move from the load address directly to the
        //   spill slot.  (There must be a spill slot assigned.)  This is why
        //   we don't use prepareResultReg() here unlike most other places --
        //   because it mandates bringing the value into a register.
        //
        if (ins->isInReg()) {
            Register rr = ins->getReg();
            asm_maybe_spill(ins, false);    // if also in memory in post-state, spill it now
            switch (ins->opcode()) {
            case LIR_ldd:
                if (rmask(rr) & XmmRegs) {
                    SSE_LDQ(rr, db, rb);
                } else {
                    NanoAssert(rmask(rr) & x87Regs);
                    FLDQ(db, rb);
                }
                break;

            case LIR_ldf2d:
                if (rmask(rr) & XmmRegs) {
                    SSE_CVTSS2SD(rr, rr);
                    SSE_LDSS(rr, db, rb);
                    SSE_XORPDr(rr,rr);
                } else {
                    NanoAssert(rmask(rr) & x87Regs);
                    FLD32(db, rb);
                }
                break;

            default:
                NanoAssert(0);
                break;
            }

        } else {
            NanoAssert(ins->isInAr());
            int dr = arDisp(ins);

            switch (ins->opcode()) {
            case LIR_ldd:
                // Don't use an fpu reg to simply load & store the value.
                asm_mmq(FP, dr, rb, db);
                break;

            case LIR_ldf2d:
                // Need to use fpu to expand 32->64.
                FSTPQ(dr, FP);
                FLD32(db, rb);
                break;

            default:
                NanoAssert(0);
                break;
            }
        }

        freeResourcesOf(ins);
    }

    void Assembler::asm_store64(LOpcode op, LIns* value, int dr, LIns* base)
    {
        Register rb = getBaseReg(base, dr, GpRegs);

        if (op == LIR_std2f) {
            bool pop = !value->isInReg();
            Register rv = ( pop
                          ? findRegFor(value, _config.i386_sse2 ? XmmRegs : FpRegs)
                          : value->getReg() );

            if (rmask(rv) & XmmRegs) {
                // need a scratch reg
                Register rt = registerAllocTmp(XmmRegs);

                // cvt to single-precision and store
                SSE_STSS(dr, rb, rt);
                SSE_CVTSD2SS(rt, rv);
                SSE_XORPDr(rt, rt);     // zero dest to ensure no dependency stalls

            } else {
                FST32(pop?1:0, dr, rb);
            }

        } else if (value->isImmD()) {
            STi(rb, dr+4, value->immDhi());
            STi(rb, dr,   value->immDlo());

        } else if (value->isop(LIR_ldd)) {
            // value is 64bit struct or int64_t, or maybe a double.
            // It may be live in an FPU reg.  Either way, don't put it in an
            // FPU reg just to load & store it.

            // a) If we know it's not a double, this is right.
            // b) If we guarded that it's a double, this store could be on the
            //    side exit, copying a non-double.
            // c) Maybe it's a double just being stored.  Oh well.

            if (_config.i386_sse2) {
                Register rv = findRegFor(value, XmmRegs);
                SSE_STQ(dr, rb, rv);
            } else {
                int da = findMemFor(value);
                asm_mmq(rb, dr, FP, da);
            }

        } else {
            bool pop = !value->isInReg();
            Register rv = ( pop
                          ? findRegFor(value, _config.i386_sse2 ? XmmRegs : FpRegs)
                          : value->getReg() );

            if (rmask(rv) & XmmRegs) {
                SSE_STQ(dr, rb, rv);
            } else {
                FSTQ(pop?1:0, dr, rb);
            }
        }
    }

    // Copy 64 bits: (rd+dd) <- (rs+ds).
    //
    void Assembler::asm_mmq(Register rd, int dd, Register rs, int ds)
    {
        // Value is either a 64-bit struct or maybe a float that isn't live in
        // an FPU reg.  Either way, avoid allocating an FPU reg just to load
        // and store it.
        if (_config.i386_sse2) {
            Register t = registerAllocTmp(XmmRegs);
            SSE_STQ(dd, rd, t);
            SSE_LDQ(t, ds, rs);
        } else {
            // We avoid copying via the FP stack because it's slow and likely
            // to cause spills.
            Register t = registerAllocTmp(GpRegs & ~(rmask(rd)|rmask(rs)));
            ST(rd, dd+4, t);
            LD(t, ds+4, rs);
            ST(rd, dd, t);
            LD(t, ds, rs);
        }
    }

    NIns* Assembler::asm_branch(bool branchOnFalse, LIns* cond, NIns* targ)
    {
        LOpcode condop = cond->opcode();
        NanoAssert(cond->isCmp());

        // Handle float conditions separately.
        if (isCmpDOpcode(condop)) {
            return asm_branchd(branchOnFalse, cond, targ);
        }

        if (branchOnFalse) {
            // op == LIR_xf/LIR_jf
            switch (condop) {
            case LIR_eqi:   JNE(targ);      break;
            case LIR_lti:   JNL(targ);      break;
            case LIR_lei:   JNLE(targ);     break;
            case LIR_gti:   JNG(targ);      break;
            case LIR_gei:   JNGE(targ);     break;
            case LIR_ltui:  JNB(targ);      break;
            case LIR_leui:  JNBE(targ);     break;
            case LIR_gtui:  JNA(targ);      break;
            case LIR_geui:  JNAE(targ);     break;
            default:        NanoAssert(0);  break;
            }
        } else {
            // op == LIR_xt/LIR_jt
            switch (condop) {
            case LIR_eqi:   JE(targ);       break;
            case LIR_lti:   JL(targ);       break;
            case LIR_lei:   JLE(targ);      break;
            case LIR_gti:   JG(targ);       break;
            case LIR_gei:   JGE(targ);      break;
            case LIR_ltui:  JB(targ);       break;
            case LIR_leui:  JBE(targ);      break;
            case LIR_gtui:  JA(targ);       break;
            case LIR_geui:  JAE(targ);      break;
            default:        NanoAssert(0);  break;
            }
        }
        NIns* at = _nIns;
        asm_cmp(cond);
        return at;
    }

    NIns* Assembler::asm_branch_ov(LOpcode, NIns* target)
    {
        JO(target);
        return _nIns;
    }

    void Assembler::asm_switch(LIns* ins, NIns* exit)
    {
        LIns* diff = ins->oprnd1();
        findSpecificRegFor(diff, EDX);
        JMP(exit);
    }

    void Assembler::asm_jtbl(LIns* ins, NIns** table)
    {
        Register indexreg = findRegFor(ins->oprnd1(), GpRegs);
        JMP_indexed(indexreg, 2, table);
    }

    // This generates a 'test' or 'cmp' instruction for a condition, which
    // causes the condition codes to be set appropriately.  It's used with
    // conditional branches, conditional moves, and when generating
    // conditional values.  For example:
    //
    //   LIR:   eq1 = eq a, 0
    //   LIR:   xf1: xf eq1 -> ...
    //   asm:       test edx, edx       # generated by this function
    //   asm:       je ...
    //
    // If this is the only use of eq1, then on entry 'cond' is *not* marked as
    // used, and we do not allocate a register for it.  That's because its
    // result ends up in the condition codes rather than a normal register.
    // This doesn't get recorded in the regstate and so the asm code that
    // consumes the result (eg. a conditional branch like 'je') must follow
    // shortly after.
    //
    // If eq1 is instead used again later, we will also generate code
    // (eg. in asm_cond()) to compute it into a normal register, something
    // like this:
    //
    //   LIR:   eq1 = eq a, 0
    //   LIR:       test edx, edx
    //   asm:       sete ebx
    //   asm:       movzx ebx, ebx
    //
    // In this case we end up computing the condition twice, but that's ok, as
    // it's just as short as testing eq1's value in the code generated for the
    // guard.
    //
    // WARNING: Because the condition code update is not recorded in the
    // regstate, this function cannot generate any code that will affect the
    // condition codes prior to the generation of the test/cmp, because any
    // such code will be run after the test/cmp but before the instruction
    // that consumes the condition code.  And because this function calls
    // findRegFor() before the test/cmp is generated, and findRegFor() calls
    // asm_restore(), that means that asm_restore() cannot generate code which
    // affects the condition codes.
    //
    void Assembler::asm_cmp(LIns *cond)
    {
        LIns* lhs = cond->oprnd1();
        LIns* rhs = cond->oprnd2();

        NanoAssert(lhs->isI() && rhs->isI());

        // Ready to issue the compare.
        if (rhs->isImmI()) {
            int c = rhs->immI();
            // findRegFor() can call asm_restore() -- asm_restore() better not
            // disturb the CCs!
            Register r = findRegFor(lhs, GpRegs);
            if (c == 0 && cond->isop(LIR_eqi)) {
                bool canSkipTest = lhs->isop(LIR_andi) || lhs->isop(LIR_ori);
                if (canSkipTest) {
                    // Setup a short-lived reader to do lookahead;  does no
                    // optimisations but that should be good enough for this
                    // simple case, something like this:
                    //
                    //   a = andi x, y      # lhs
                    //   eq1 = eq a, 0      # cond
                    //   xt eq1             # currIns
                    //
                    // Note that we don't have to worry about lookahead
                    // hitting the start of the buffer, because read() will
                    // just return LIR_start repeatedly in that case.
                    //
                    LirReader lookahead(currIns);
                    canSkipTest = currIns == lookahead.read() &&
                                  cond == lookahead.read() &&
                                  lhs == lookahead.read();
                }
                if (canSkipTest) {
                    // Do nothing.  At run-time, 'lhs' will have just computed
                    // by an i386 instruction that sets ZF for us ('and' or
                    // 'or'), so we don't have to do it ourselves.
                } else {
                    TEST(r, r);     // sets ZF according to the value of 'lhs'
                }
            } else {
                CMPi(r, c);
            }
        } else {
            Register ra, rb;
            findRegFor2(GpRegs, lhs, ra, GpRegs, rhs, rb);
            CMP(ra, rb);
        }
    }

    void Assembler::asm_condd(LIns* ins)
    {
        LOpcode opcode = ins->opcode();
        Register r = prepareResultReg(ins, AllowableFlagRegs);

        // SETcc only sets low 8 bits, so extend
        MOVZX8(r,r);

        if (_config.i386_sse2) {
            // LIR_ltd and LIR_gtd are handled by the same case because
            // asm_cmpd() converts LIR_ltd(a,b) to LIR_gtd(b,a).  Likewise
            // for LIR_led/LIR_ged.
            switch (opcode) {
            case LIR_eqd:   SETNP(r);       break;
            case LIR_ltd:
            case LIR_gtd:   SETA(r);        break;
            case LIR_led:
            case LIR_ged:   SETAE(r);       break;
            default:        NanoAssert(0);  break;
            }
        } else {
            SETNP(r);
        }

        freeResourcesOf(ins);

        asm_cmpd(ins);
    }

    void Assembler::asm_cond(LIns* ins)
    {
        LOpcode op = ins->opcode();

        Register r = prepareResultReg(ins, AllowableFlagRegs);

        // SETcc only sets low 8 bits, so extend
        MOVZX8(r,r);
        switch (op) {
        case LIR_eqi:   SETE(r);        break;
        case LIR_lti:   SETL(r);        break;
        case LIR_lei:   SETLE(r);       break;
        case LIR_gti:   SETG(r);        break;
        case LIR_gei:   SETGE(r);       break;
        case LIR_ltui:  SETB(r);        break;
        case LIR_leui:  SETBE(r);       break;
        case LIR_gtui:  SETA(r);        break;
        case LIR_geui:  SETAE(r);       break;
        default:        NanoAssert(0);  break;
        }

        freeResourcesOf(ins);

        asm_cmp(ins);
    }

    // Two example cases for "ins = add lhs, rhs".  '*' lines are those
    // generated in this function.
    //
    //   asm:   define lhs into rr
    //   asm:   define rhs into rb
    //          ...
    // * asm:   add rr, rb
    // * asm:   spill rr if necessary
    //          ... no more uses of lhs in rr...
    //
    //   asm:   define lhs into ra
    //   asm:   define rhs into rb
    //          ...
    // * asm:   mov rr, ra
    // * asm:   add rr, rb
    // * asm:   spill rr if necessary
    //          ... some uses of lhs in ra...
    //
    void Assembler::asm_arith(LIns* ins)
    {
        LOpcode op = ins->opcode();

        // First special case.
        if (op == LIR_modi) {
            asm_div_mod(ins);
            return;
        }

        LIns* lhs = ins->oprnd1();
        LIns* rhs = ins->oprnd2();

        // Second special case.
        // XXX: bug 547125: don't need this once LEA is used for LIR_addi in all cases below
        if (op == LIR_addi && lhs->isop(LIR_allocp) && rhs->isImmI()) {
            // LIR_addi(LIR_allocp, LIR_immi) -- use lea.
            Register rr = prepareResultReg(ins, GpRegs);
            int d = findMemFor(lhs) + rhs->immI();

            LEA(rr, d, FP);

            freeResourcesOf(ins);

            return;
        }

        bool isConstRhs;
        RegisterMask allow = GpRegs;
        Register rb = UnspecifiedReg;

        switch (op) {
        case LIR_divi:
            // Nb: if the div feeds into a mod it will be handled by
            // asm_div_mod() rather than here.
            isConstRhs = false;
            rb = findRegFor(rhs, (GpRegs & ~(rmask(EAX)|rmask(EDX))));
            allow = rmask(EAX);
            evictIfActive(EDX);
            break;
        case LIR_muli:
        case LIR_muljovi:
        case LIR_mulxovi:
            isConstRhs = false;
            if (lhs != rhs) {
                rb = findRegFor(rhs, allow);
                allow &= ~rmask(rb);
            }
            break;
        case LIR_lshi:
        case LIR_rshi:
        case LIR_rshui:
            isConstRhs = rhs->isImmI();
            if (!isConstRhs) {
                rb = findSpecificRegFor(rhs, ECX);
                allow &= ~rmask(rb);
            }
            break;
        default:
            isConstRhs = rhs->isImmI();
            if (!isConstRhs && lhs != rhs) {
                rb = findRegFor(rhs, allow);
                allow &= ~rmask(rb);
            }
            break;
        }

        // Somewhere for the result of 'ins'.
        Register rr = prepareResultReg(ins, allow);

        // If 'lhs' isn't in a register, it can be clobbered by 'ins'.
        Register ra = lhs->isInReg() ? lhs->getReg() : rr;

        if (!isConstRhs) {
            if (lhs == rhs)
                rb = ra;

            switch (op) {
            case LIR_addi:
            case LIR_addjovi:
            case LIR_addxovi:    ADD(rr, rb); break;     // XXX: bug 547125: could use LEA for LIR_addi
            case LIR_subi:
            case LIR_subjovi:
            case LIR_subxovi:    SUB(rr, rb); break;
            case LIR_muli:
            case LIR_muljovi:
            case LIR_mulxovi:    MUL(rr, rb); break;
            case LIR_andi:       AND(rr, rb); break;
            case LIR_ori:        OR( rr, rb); break;
            case LIR_xori:       XOR(rr, rb); break;
            case LIR_lshi:       SHL(rr, rb); break;
            case LIR_rshi:       SAR(rr, rb); break;
            case LIR_rshui:      SHR(rr, rb); break;
            case LIR_divi:
                DIV(rb);
                CDQ(); // sign-extend EAX into EDX:EAX
                break;
            default:            NanoAssert(0);  break;
            }

        } else {
            int c = rhs->immI();
            switch (op) {
            case LIR_addi:
                // this doesn't set cc's, only use it when cc's not required.
                LEA(rr, c, ra);
                ra = rr; // suppress mov
                break;
            case LIR_addjovi:
            case LIR_addxovi:    ADDi(rr, c);    break;
            case LIR_subi:
            case LIR_subjovi:
            case LIR_subxovi:    SUBi(rr, c);    break;
            case LIR_andi:       ANDi(rr, c);    break;
            case LIR_ori:        ORi( rr, c);    break;
            case LIR_xori:       XORi(rr, c);    break;
            case LIR_lshi:       SHLi(rr, c);    break;
            case LIR_rshi:       SARi(rr, c);    break;
            case LIR_rshui:      SHRi(rr, c);    break;
            default:            NanoAssert(0);  break;
            }
        }

        if (rr != ra)
            MR(rr, ra);

        freeResourcesOf(ins);
        if (!lhs->isInReg()) {
            NanoAssert(ra == rr);
            findSpecificRegForUnallocated(lhs, ra);
        }
    }

    // Generates code for a LIR_modi(LIR_divi(divL, divR)) sequence.
    void Assembler::asm_div_mod(LIns* mod)
    {
        LIns* div = mod->oprnd1();

        // LIR_modi expects the LIR_divi to be near (no interference from the register allocator).
        NanoAssert(mod->isop(LIR_modi));
        NanoAssert(div->isop(LIR_divi));

        LIns* divL = div->oprnd1();
        LIns* divR = div->oprnd2();

        prepareResultReg(mod, rmask(EDX));
        prepareResultReg(div, rmask(EAX));

        Register rDivR = findRegFor(divR, (GpRegs & ~(rmask(EAX)|rmask(EDX))));
        Register rDivL = divL->isInReg() ? divL->getReg() : EAX;

        DIV(rDivR);
        CDQ();     // sign-extend EAX into EDX:EAX
        if (EAX != rDivL)
            MR(EAX, rDivL);

        freeResourcesOf(mod);
        freeResourcesOf(div);
        if (!divL->isInReg()) {
            NanoAssert(rDivL == EAX);
            findSpecificRegForUnallocated(divL, EAX);
        }
    }

    // Two example cases for "ins = neg lhs".  Lines marked with '*' are
    // generated in this function.
    //
    //   asm:   define lhs into rr
    //          ...
    // * asm:   neg rr
    // * asm:   spill rr if necessary
    //          ... no more uses of lhs in rr...
    //
    //
    //   asm:   define lhs into ra
    //          ...
    // * asm:   mov rr, ra
    // * asm:   neg rr
    // * asm:   spill rr if necessary
    //          ... more uses of lhs in ra...
    //
    void Assembler::asm_neg_not(LIns* ins)
    {
        LIns* lhs = ins->oprnd1();

        Register rr = prepareResultReg(ins, GpRegs);

        // If 'lhs' isn't in a register, it can be clobbered by 'ins'.
        Register ra = lhs->isInReg() ? lhs->getReg() : rr;

        if (ins->isop(LIR_noti)) {
            NOT(rr);
        } else {
            NanoAssert(ins->isop(LIR_negi));
            NEG(rr);
        }
        if (rr != ra)
            MR(rr, ra);

        freeResourcesOf(ins);
        if (!lhs->isInReg()) {
            NanoAssert(ra == rr);
            findSpecificRegForUnallocated(lhs, ra);
        }
    }

    void Assembler::asm_load32(LIns* ins)
    {
        LOpcode op = ins->opcode();
        LIns* base = ins->oprnd1();
        int32_t d = ins->disp();

        Register rr = prepareResultReg(ins, GpRegs);

        if (base->isImmI()) {
            intptr_t addr = base->immI();
            addr += d;
            switch (op) {
                case LIR_lduc2ui:
                    LD8Zdm(rr, addr);
                    break;
                case LIR_ldc2i:
                    LD8Sdm(rr, addr);
                    break;
                case LIR_ldus2ui:
                    LD16Zdm(rr, addr);
                    break;
                case LIR_lds2i:
                    LD16Sdm(rr, addr);
                    break;
                case LIR_ldi:
                    LDdm(rr, addr);
                    break;
                default:
                    NanoAssertMsg(0, "asm_load32 should never receive this LIR opcode");
                    break;
            }

            freeResourcesOf(ins);

        } else if (base->opcode() == LIR_addp) {
            // Search for add(X,Y).
            LIns *lhs = base->oprnd1();
            LIns *rhs = base->oprnd2();

            // If we have this:
            //
            //   W = ld (add(X, shl(Y, Z)))[d] , where int(1) <= Z <= int(3)
            //
            // we assign lhs=X, rhs=Y, scale=Z, and generate this:
            //
            //   mov rW, [rX+rY*(2^rZ)]
            //
            // Otherwise, we must have this:
            //
            //   W = ld (add(X, Y))[d]
            //
            // which we treat like this:
            //
            //   W = ld (add(X, shl(Y, 0)))[d]
            //
            int scale;
            if (rhs->opcode() == LIR_lshp && rhs->oprnd2()->isImmI()) {
                scale = rhs->oprnd2()->immI();
                if (scale >= 1 && scale <= 3)
                    rhs = rhs->oprnd1();
                else
                    scale = 0;
            } else {
                scale = 0;
            }

            // If 'lhs' isn't in a register, it can be clobbered by 'ins'.
            // Likewise for 'rhs', but we try it with 'lhs' first.
            Register ra, rb;
            // @todo -- If LHS and/or RHS is const, we could eliminate a register use.
            if (!lhs->isInReg()) {
                ra = rr;
                rb = findRegFor(rhs, GpRegs & ~(rmask(ra)));

            } else {
                ra = lhs->getReg();
                NanoAssert(ra != rr);
                rb = rhs->isInReg() ? findRegFor(rhs, GpRegs & ~(rmask(ra))) : rr;
            }

            switch (op) {
                case LIR_lduc2ui:
                    LD8Zsib(rr, d, ra, rb, scale);
                    break;
                case LIR_ldc2i:
                    LD8Ssib(rr, d, ra, rb, scale);
                    break;
                case LIR_ldus2ui:
                    LD16Zsib(rr, d, ra, rb, scale);
                    break;
                case LIR_lds2i:
                    LD16Ssib(rr, d, ra, rb, scale);
                    break;
                case LIR_ldi:
                    LDsib(rr, d, ra, rb, scale);
                    break;
                default:
                    NanoAssertMsg(0, "asm_load32 should never receive this LIR opcode");
                    break;
            }

            freeResourcesOf(ins);
            if (!lhs->isInReg()) {
                NanoAssert(ra == rr);
                findSpecificRegForUnallocated(lhs, ra);
            } else if (!rhs->isInReg()) {
                NanoAssert(rb == rr);
                findSpecificRegForUnallocated(rhs, rb);
            }

        } else {
            Register ra = getBaseReg(base, d, GpRegs);

            switch (op) {
                case LIR_lduc2ui:
                    LD8Z(rr, d, ra);
                    break;
                case LIR_ldc2i:
                    LD8S(rr, d, ra);
                    break;
                case LIR_ldus2ui:
                    LD16Z(rr, d, ra);
                    break;
                case LIR_lds2i:
                    LD16S(rr, d, ra);
                    break;
                case LIR_ldi:
                    LD(rr, d, ra);
                    break;
                default:
                    NanoAssertMsg(0, "asm_load32 should never receive this LIR opcode");
                    break;
            }

            freeResourcesOf(ins);
            if (!base->isop(LIR_allocp) && !base->isInReg()) {
                NanoAssert(ra == rr);
                findSpecificRegForUnallocated(base, ra);
            }
        }
    }

    void Assembler::asm_cmov(LIns* ins)
    {
        LIns* condval = ins->oprnd1();
        LIns* iftrue  = ins->oprnd2();
        LIns* iffalse = ins->oprnd3();

        NanoAssert(condval->isCmp());
        NanoAssert(ins->isop(LIR_cmovi) && iftrue->isI() && iffalse->isI());

        Register rr = prepareResultReg(ins, GpRegs);

        Register rf = findRegFor(iffalse, GpRegs & ~rmask(rr));

        // If 'iftrue' isn't in a register, it can be clobbered by 'ins'.
        Register rt = iftrue->isInReg() ? iftrue->getReg() : rr;

        // WARNING: We cannot generate any code that affects the condition
        // codes between the MRcc generation here and the asm_cmp() call
        // below.  See asm_cmp() for more details.
        switch (condval->opcode()) {
            // Note that these are all opposites...
            case LIR_eqi:    MRNE(rr, rf);   break;
            case LIR_lti:    MRGE(rr, rf);   break;
            case LIR_lei:    MRG( rr, rf);   break;
            case LIR_gti:    MRLE(rr, rf);   break;
            case LIR_gei:    MRL( rr, rf);   break;
            case LIR_ltui:   MRAE(rr, rf);   break;
            case LIR_leui:   MRA( rr, rf);   break;
            case LIR_gtui:   MRBE(rr, rf);   break;
            case LIR_geui:   MRB( rr, rf);   break;
            default: NanoAssert(0); break;
        }

        if (rr != rt)
            MR(rr, rt);

        freeResourcesOf(ins);
        if (!iftrue->isInReg()) {
            NanoAssert(rt == rr);
            findSpecificRegForUnallocated(iftrue, rr);
        }

        asm_cmp(condval);
    }

    void Assembler::asm_param(LIns* ins)
    {
        uint32_t arg = ins->paramArg();
        uint32_t kind = ins->paramKind();
        if (kind == 0) {
            // ordinary param
            AbiKind abi = _thisfrag->lirbuf->abi;
            uint32_t abi_regcount = max_abi_regs[abi];
            // argRegs must have as many elements as the largest argument register
            // requirement of an abi.  Currently, this is 2, for ABI_FASTCALL.  See
            // the definition of max_abi_regs earlier in this file.  The following
            // assertion reflects this invariant:
            NanoAssert(abi_regcount <= sizeof(argRegs)/sizeof(argRegs[0]));
            if (arg < abi_regcount) {
                // Incoming arg in register.
                prepareResultReg(ins, rmask(argRegs[arg]));
                // No code to generate.
            } else {
                // Incoming arg is on stack, and EBP points nearby (see genPrologue()).
                Register r = prepareResultReg(ins, GpRegs);
                int d = (arg - abi_regcount) * sizeof(intptr_t) + 8;
                LD(r, d, FP);
            }
        } else {
            // Saved param.
            prepareResultReg(ins, rmask(savedRegs[arg]));
            // No code to generate.
        }
        freeResourcesOf(ins);
    }

    void Assembler::asm_immi(LIns* ins)
    {
        Register rr = prepareResultReg(ins, GpRegs);

        asm_immi(rr, ins->immI(), /*canClobberCCs*/true);

        freeResourcesOf(ins);
    }

    void Assembler::asm_immi(Register r, int32_t val, bool canClobberCCs)
    {
        if (val == 0 && canClobberCCs)
            XOR(r, r);
        else
            LDi(r, val);
    }

    void Assembler::asm_immd(Register r, uint64_t q, double d, bool canClobberCCs)
    {
        // Floats require non-standard handling. There is no load-64-bit-immediate
        // instruction on i386, so in the general case, we must load it from memory.
        // This is unlike most other LIR operations which can be computed directly
        // in a register. We can special-case 0.0 and various other small ints
        // (1.0 on x87, any int32_t value on SSE2), but for all other values, we
        // allocate an 8-byte chunk via dataAlloc and load from there. Note that
        // this implies that floats never require spill area, since they will always
        // be rematerialized from const data (or inline instructions in the special cases).

        if (rmask(r) & XmmRegs) {
            if (q == 0) {
                // test (int64)0 since -0.0 == 0.0
                SSE_XORPDr(r, r);
            } else if (d && d == (int)d && canClobberCCs) {
                // can fit in 32bits? then use cvt which is faster
                Register tr = registerAllocTmp(GpRegs);
                SSE_CVTSI2SD(r, tr);
                SSE_XORPDr(r, r);   // zero r to ensure no dependency stalls
                asm_immi(tr, (int)d, canClobberCCs);
            } else {
                const uint64_t* p = findImmDFromPool(q);
                LDSDm(r, (const double*)p);
            }
        } else {
            NanoAssert(r == FST0);
            if (q == 0) {
                // test (int64)0 since -0.0 == 0.0
                FLDZ();
            } else if (d == 1.0) {
                FLD1();
            } else {
                const uint64_t* p = findImmDFromPool(q);
                FLDQdm((const double*)p);
            }
        }
    }

    void Assembler::asm_immd(LIns* ins)
    {
        NanoAssert(ins->isImmD());
        if (ins->isInReg()) {
            Register rr = ins->getReg();
            NanoAssert(rmask(rr) & FpRegs);
            asm_immd(rr, ins->immDasQ(), ins->immD(), /*canClobberCCs*/true);
        } else {
            // Do nothing, will be rematerialized when necessary.
        }

        freeResourcesOf(ins);
    }

    // negateMask is used by asm_fneg.
#if defined __SUNPRO_CC
    // From Sun Studio C++ Readme: #pragma align inside namespace requires mangled names.
    // Initialize here to avoid multithreading contention issues during initialization.
    static uint32_t negateMask_temp[] = {0, 0, 0, 0, 0, 0, 0};

    static uint32_t* negateMaskInit()
    {
        uint32_t* negateMask = (uint32_t*)alignUp(negateMask_temp, 16);
        negateMask[1] = 0x80000000;
        return negateMask;
    }

    static uint32_t *negateMask = negateMaskInit();
#else
    static const AVMPLUS_ALIGN16(uint32_t) negateMask[] = {0,0x80000000,0,0};
#endif

    void Assembler::asm_fneg(LIns* ins)
    {
        LIns *lhs = ins->oprnd1();

        if (_config.i386_sse2) {
            Register rr = prepareResultReg(ins, XmmRegs);

            // If 'lhs' isn't in a register, it can be clobbered by 'ins'.
            Register ra;
            if (!lhs->isInReg()) {
                ra = rr;
            } else if (!(rmask(lhs->getReg()) & XmmRegs)) {
                // We need to evict lhs from x87Regs, which then puts us in
                // the same situation as the !isInReg() case.
                evict(lhs);
                ra = rr;
            } else {
                ra = lhs->getReg();
            }

            SSE_XORPD(rr, negateMask);

            if (rr != ra)
                SSE_MOVSD(rr, ra);

            freeResourcesOf(ins);
            if (!lhs->isInReg()) {
                NanoAssert(ra == rr);
                findSpecificRegForUnallocated(lhs, ra);
            }

        } else {
            debug_only( Register rr = ) prepareResultReg(ins, x87Regs);
            NanoAssert(FST0 == rr);

            NanoAssert(!lhs->isInReg() || FST0 == lhs->getReg());

            FCHS();

            freeResourcesOf(ins);
            if (!lhs->isInReg())
                findSpecificRegForUnallocated(lhs, FST0);
        }
    }

    void Assembler::asm_arg(ArgType ty, LIns* ins, Register r, int32_t& stkd)
    {
        // If 'r' is known, then that's the register we have to put 'ins'
        // into.

        if (ty == ARGTYPE_I || ty == ARGTYPE_UI) {
            if (r != UnspecifiedReg) {
                if (ins->isImmI()) {
                    // Rematerialize the constant.
                    asm_immi(r, ins->immI(), /*canClobberCCs*/true);
                } else if (ins->isInReg()) {
                    if (r != ins->getReg())
                        MR(r, ins->getReg());
                } else if (ins->isInAr()) {
                    int d = arDisp(ins);
                    NanoAssert(d != 0);
                    if (ins->isop(LIR_allocp)) {
                        LEA(r, d, FP);
                    } else {
                        LD(r, d, FP);
                    }

                } else {
                    // This is the last use, so fine to assign it
                    // to the scratch reg, it's dead after this point.
                    findSpecificRegForUnallocated(ins, r);
                }
            }
            else {
                if (_config.i386_fixed_esp)
                    asm_stkarg(ins, stkd);
                else
                    asm_pusharg(ins);
            }

        } else {
            NanoAssert(ty == ARGTYPE_D);
            asm_farg(ins, stkd);
        }
    }

    void Assembler::asm_pusharg(LIns* ins)
    {
        // arg goes on stack
        if (!ins->isExtant() && ins->isImmI())
        {
            PUSHi(ins->immI());    // small const we push directly
        }
        else if (!ins->isExtant() || ins->isop(LIR_allocp))
        {
            Register ra = findRegFor(ins, GpRegs);
            PUSHr(ra);
        }
        else if (ins->isInReg())
        {
            PUSHr(ins->getReg());
        }
        else
        {
            NanoAssert(ins->isInAr());
            PUSHm(arDisp(ins), FP);
        }
    }

    void Assembler::asm_stkarg(LIns* ins, int32_t& stkd)
    {
        // arg goes on stack
        if (!ins->isExtant() && ins->isImmI())
        {
            // small const we push directly
            STi(SP, stkd, ins->immI());
        }
        else {
            Register ra;
            if (!ins->isInReg() || ins->isop(LIR_allocp))
                ra = findRegFor(ins, GpRegs & (~SavedRegs));
            else
                ra = ins->getReg();
            ST(SP, stkd, ra);
        }

        stkd += sizeof(int32_t);
    }

    void Assembler::asm_farg(LIns* ins, int32_t& stkd)
    {
        NanoAssert(ins->isD());
        Register r = findRegFor(ins, FpRegs);
        if (rmask(r) & XmmRegs) {
            SSE_STQ(stkd, SP, r);
        } else {
            FSTPQ(stkd, SP);

            // 22Jul09 rickr - Enabling the evict causes a 10% slowdown on primes
            //
            // evict() triggers a very expensive fstpq/fldq pair around the store.
            // We need to resolve the bug some other way.
            //
            // see https://bugzilla.mozilla.org/show_bug.cgi?id=491084

            // It's possible that the same LIns* with r=FST0 will appear in the argument list more
            // than once.  In this case FST0 will not have been evicted and the multiple pop
            // actions will unbalance the FPU stack.  A quick fix is to always evict FST0 manually.
            NanoAssert(r == FST0);
            NanoAssert(ins == _allocator.getActive(r));
            evict(ins);
        }
        if (!_config.i386_fixed_esp)
            SUBi(ESP, 8);

        stkd += sizeof(double);
    }

    void Assembler::asm_fop(LIns* ins)
    {
        LOpcode op = ins->opcode();
        if (_config.i386_sse2)
        {
            LIns *lhs = ins->oprnd1();
            LIns *rhs = ins->oprnd2();

            RegisterMask allow = XmmRegs;
            Register rb = UnspecifiedReg;
            if (lhs != rhs) {
                rb = findRegFor(rhs, allow);
                allow &= ~rmask(rb);
            }

            Register rr = prepareResultReg(ins, allow);

            // If 'lhs' isn't in a register, it can be clobbered by 'ins'.
            Register ra;
            if (!lhs->isInReg()) {
                ra = rr;

            } else if (!(rmask(lhs->getReg()) & XmmRegs)) {
                NanoAssert(lhs->getReg() == FST0);

                // We need to evict lhs from x87Regs, which then puts us in
                // the same situation as the !isInReg() case.
                evict(lhs);
                ra = rr;

            } else {
                ra = lhs->getReg();
                NanoAssert(rmask(ra) & XmmRegs);
            }

            if (lhs == rhs)
                rb = ra;

            switch (op) {
            case LIR_addd:  SSE_ADDSD(rr, rb);  break;
            case LIR_subd:  SSE_SUBSD(rr, rb);  break;
            case LIR_muld:  SSE_MULSD(rr, rb);  break;
            case LIR_divd:  SSE_DIVSD(rr, rb);  break;
            default:        NanoAssert(0);
            }

            if (rr != ra)
                SSE_MOVSD(rr, ra);

            freeResourcesOf(ins);
            if (!lhs->isInReg()) {
                NanoAssert(ra == rr);
                findSpecificRegForUnallocated(lhs, ra);
            }
        }
        else
        {
            // We swap lhs/rhs on purpose here, it works out better with
            // only one fpu reg -- we can use divr/subr.
            LIns* rhs = ins->oprnd1();
            LIns* lhs = ins->oprnd2();
            debug_only( Register rr = ) prepareResultReg(ins, rmask(FST0));
            NanoAssert(FST0 == rr);
            NanoAssert(!lhs->isInReg() || FST0 == lhs->getReg());

            if (rhs->isImmD()) {
                const uint64_t* p = findImmDFromPool(rhs->immDasQ());

                switch (op) {
                case LIR_addd:  FADDdm( (const double*)p);  break;
                case LIR_subd:  FSUBRdm((const double*)p);  break;
                case LIR_muld:  FMULdm( (const double*)p);  break;
                case LIR_divd:  FDIVRdm((const double*)p);  break;
                default:        NanoAssert(0);
                }

            } else {
                int db = findMemFor(rhs);

                switch (op) {
                case LIR_addd:  FADD( db, FP);  break;
                case LIR_subd:  FSUBR(db, FP);  break;
                case LIR_muld:  FMUL( db, FP);  break;
                case LIR_divd:  FDIVR(db, FP);  break;
                default:        NanoAssert(0);
                }
            }
            freeResourcesOf(ins);
            if (!lhs->isInReg()) {
                findSpecificRegForUnallocated(lhs, FST0);
            }
        }
    }

    void Assembler::asm_i2d(LIns* ins)
    {
        LIns* lhs = ins->oprnd1();

        Register rr = prepareResultReg(ins, FpRegs);
        if (rmask(rr) & XmmRegs) {
            // todo support int value in memory
            Register ra = findRegFor(lhs, GpRegs);
            SSE_CVTSI2SD(rr, ra);
            SSE_XORPDr(rr, rr);     // zero rr to ensure no dependency stalls
        } else {
            int d = findMemFor(lhs);
            FILD(d, FP);
        }

        freeResourcesOf(ins);
    }

    void Assembler::asm_ui2d(LIns* ins)
    {
        LIns* lhs = ins->oprnd1();

        Register rr = prepareResultReg(ins, FpRegs);
        if (rmask(rr) & XmmRegs) {
            Register rt = registerAllocTmp(GpRegs);

            // Technique inspired by gcc disassembly.  Edwin explains it:
            //
            // rt is 0..2^32-1
            //
            //     sub rt,0x80000000
            //
            // Now rt is -2^31..2^31-1, i.e. the range of int, but not the same value
            // as before.
            //
            //     cvtsi2sd rr,rt
            //
            // rr is now a double with the int value range.
            //
            //     addsd rr, 2147483648.0
            //
            // Adding back double(0x80000000) makes the range 0..2^32-1.

            static const double k_NEGONE = 2147483648.0;
            SSE_ADDSDm(rr, &k_NEGONE);

            SSE_CVTSI2SD(rr, rt);
            SSE_XORPDr(rr,rr);  // zero rr to ensure no dependency stalls

            if (lhs->isInRegMask(GpRegs)) {
                Register ra = lhs->getReg();
                LEA(rt, 0x80000000, ra);

            } else {
                const int d = findMemFor(lhs);
                SUBi(rt, 0x80000000);
                LD(rt, d, FP);
            }

        } else {
            const int disp = -8;
            const Register base = SP;
            Register ra = findRegFor(lhs, GpRegs);
            NanoAssert(rr == FST0);
            FILDQ(disp, base);
            STi(base, disp+4, 0);   // high 32 bits = 0
            ST(base, disp, ra);     // low 32 bits = unsigned value
        }

        freeResourcesOf(ins);
    }

    void Assembler::asm_d2i(LIns* ins)
    {
        LIns *lhs = ins->oprnd1();

        if (_config.i386_sse2) {
            Register rr = prepareResultReg(ins, GpRegs);
            Register ra = findRegFor(lhs, XmmRegs);
            SSE_CVTSD2SI(rr, ra);
        } else {
            int pop = !lhs->isInReg();
            findSpecificRegFor(lhs, FST0);
            if (ins->isInReg())
                evict(ins);
            int d = findMemFor(ins);
            FIST((pop?1:0), d, FP);
        }

        freeResourcesOf(ins);
    }

    void Assembler::asm_nongp_copy(Register rd, Register rs)
    {
        if ((rmask(rd) & XmmRegs) && (rmask(rs) & XmmRegs)) {
            // xmm -> xmm
            SSE_MOVSD(rd, rs);
        } else if ((rmask(rd) & GpRegs) && (rmask(rs) & XmmRegs)) {
            // xmm -> gp
            SSE_MOVD(rd, rs);
        } else {
            NanoAssertMsgf(false, "bad asm_nongp_copy(%s, %s)", gpn(rd), gpn(rs));
        }
    }

    NIns* Assembler::asm_branchd(bool branchOnFalse, LIns *cond, NIns *targ)
    {
        NIns* at;
        LOpcode opcode = cond->opcode();

        if (_config.i386_sse2) {
            // LIR_ltd and LIR_gtd are handled by the same case because
            // asm_cmpd() converts LIR_ltd(a,b) to LIR_gtd(b,a).  Likewise
            // for LIR_led/LIR_ged.
            if (branchOnFalse) {
                // op == LIR_xf
                switch (opcode) {
                case LIR_eqd:   JP(targ);       break;
                case LIR_ltd:
                case LIR_gtd:   JNA(targ);      break;
                case LIR_led:
                case LIR_ged:   JNAE(targ);     break;
                default:        NanoAssert(0);  break;
                }
            } else {
                // op == LIR_xt
                switch (opcode) {
                case LIR_eqd:   JNP(targ);      break;
                case LIR_ltd:
                case LIR_gtd:   JA(targ);       break;
                case LIR_led:
                case LIR_ged:   JAE(targ);      break;
                default:        NanoAssert(0);  break;
                }
            }
        } else {
            if (branchOnFalse)
                JP(targ);
            else
                JNP(targ);
        }

        at = _nIns;
        asm_cmpd(cond);

        return at;
    }

    // WARNING: This function cannot generate any code that will affect the
    // condition codes prior to the generation of the
    // ucomisd/fcompp/fcmop/fcom.  See asm_cmp() for more details.
    void Assembler::asm_cmpd(LIns *cond)
    {
        LOpcode condop = cond->opcode();
        NanoAssert(isCmpDOpcode(condop));
        LIns* lhs = cond->oprnd1();
        LIns* rhs = cond->oprnd2();
        NanoAssert(lhs->isD() && rhs->isD());

        if (_config.i386_sse2) {
            // First, we convert (a < b) into (b > a), and (a <= b) into (b >= a).
            if (condop == LIR_ltd) {
                condop = LIR_gtd;
                LIns* t = lhs; lhs = rhs; rhs = t;
            } else if (condop == LIR_led) {
                condop = LIR_ged;
                LIns* t = lhs; lhs = rhs; rhs = t;
            }

            if (condop == LIR_eqd) {
                if (lhs == rhs) {
                    // We can generate better code for LIR_eqd when lhs==rhs (NaN test).

                    // ucomisd    ZPC  outcome (SETNP/JNP succeeds if P==0)
                    // -------    ---  -------
                    // UNORDERED  111  SETNP/JNP fails
                    // EQUAL      100  SETNP/JNP succeeds

                    Register r = findRegFor(lhs, XmmRegs);
                    SSE_UCOMISD(r, r);
                } else {
                    // LAHF puts the flags into AH like so:  SF:ZF:0:AF:0:PF:1:CF (aka. SZ0A_0P1C).
                    // We then mask out the bits as follows.
                    // - LIR_eqd: mask == 0x44 == 0100_0100b, which extracts 0Z00_0P00 from AH.
                    int mask = 0x44;

                    // ucomisd       ZPC   lahf/test(0x44) SZP   outcome
                    // -------       ---   ---------       ---   -------
                    // UNORDERED     111   0100_0100       001   SETNP/JNP fails
                    // EQUAL         100   0100_0000       000   SETNP/JNP succeeds
                    // GREATER_THAN  000   0000_0000       011   SETNP/JNP fails
                    // LESS_THAN     001   0000_0000       011   SETNP/JNP fails

                    evictIfActive(EAX);
                    Register ra, rb;
                    findRegFor2(XmmRegs, lhs, ra, XmmRegs, rhs, rb);

                    TEST_AH(mask);
                    LAHF();
                    SSE_UCOMISD(ra, rb);
                }
            } else {
                // LIR_gtd:
                //   ucomisd       ZPC   outcome (SETA/JA succeeds if CZ==00)
                //   -------       ---   -------
                //   UNORDERED     111   SETA/JA fails
                //   EQUAL         100   SETA/JA fails
                //   GREATER_THAN  000   SETA/JA succeeds
                //   LESS_THAN     001   SETA/JA fails
                //
                // LIR_ged:
                //   ucomisd       ZPC   outcome (SETAE/JAE succeeds if C==0)
                //   -------       ---   -------
                //   UNORDERED     111   SETAE/JAE fails
                //   EQUAL         100   SETAE/JAE succeeds
                //   GREATER_THAN  000   SETAE/JAE succeeds
                //   LESS_THAN     001   SETAE/JAE fails

                Register ra, rb;
                findRegFor2(XmmRegs, lhs, ra, XmmRegs, rhs, rb);
                SSE_UCOMISD(ra, rb);
            }

        } else {
            // First, we convert (a > b) into (b < a), and (a >= b) into (b <= a).
            // Note that this is the opposite of the sse2 conversion above.
            if (condop == LIR_gtd) {
                condop = LIR_ltd;
                LIns* t = lhs; lhs = rhs; rhs = t;
            } else if (condop == LIR_ged) {
                condop = LIR_led;
                LIns* t = lhs; lhs = rhs; rhs = t;
            }

            // FNSTSW_AX puts the flags into AH like so:  B:C3:TOP3:TOP2:TOP1:C2:C1:C0.
            // Furthermore, fcom/fcomp/fcompp sets C3:C2:C0 the same values
            // that Z:P:C are set by ucomisd, and the relative positions in AH
            // line up.  (Someone at Intel has a sense of humour.)  Therefore
            // we can use the same lahf/test(mask) technique as used in the
            // sse2 case above.  We could use fcomi/fcomip/fcomipp which set
            // ZPC directly and then use LAHF instead of FNSTSW_AX and make
            // this code generally more like the sse2 code, but we don't
            // because fcomi/fcomip/fcomipp/lahf aren't available on earlier
            // x86 machines.
            //
            // The masks are as follows:
            // - LIR_eqd: mask == 0x44 == 0100_0100b, which extracts 0Z00_0P00 from AH.
            // - LIR_ltd: mask == 0x05 == 0000_0101b, which extracts 0000_0P0C from AH.
            // - LIR_led: mask == 0x41 == 0100_0001b, which extracts 0Z00_000C from AH.
            //
            // LIR_eqd (very similar to the sse2 case above):
            //   ucomisd  C3:C2:C0   lahf/test(0x44) SZP   outcome
            //   -------  --------   ---------       ---   -------
            //   UNORDERED     111   0100_0100       001   SETNP fails
            //   EQUAL         100   0100_0000       000   SETNP succeeds
            //   GREATER_THAN  000   0000_0000       011   SETNP fails
            //   LESS_THAN     001   0000_0000       011   SETNP fails
            //
            // LIR_ltd:
            //   fcom     C3:C2:C0   lahf/test(0x05) SZP   outcome
            //   -------  --------   ---------       ---   -------
            //   UNORDERED     111   0000_0101       001   SETNP fails
            //   EQUAL         100   0000_0000       011   SETNP fails
            //   GREATER_THAN  000   0000_0000       011   SETNP fails
            //   LESS_THAN     001   0000_0001       000   SETNP succeeds
            //
            // LIR_led:
            //   fcom     C3:C2:C0   lahf/test(0x41) SZP   outcome
            //   -------       ---   ---------       ---   -------
            //   UNORDERED     111   0100_0001       001   SETNP fails
            //   EQUAL         100   0100_0000       000   SETNP succeeds
            //   GREATER_THAN  000   0000_0000       011   SETNP fails
            //   LESS_THAN     001   0000_0001       010   SETNP succeeds

            int mask = 0;   // init to avoid MSVC compile warnings
            switch (condop) {
            case LIR_eqd:   mask = 0x44;    break;
            case LIR_ltd:   mask = 0x05;    break;
            case LIR_led:   mask = 0x41;    break;
            default:        NanoAssert(0);  break;
            }

            evictIfActive(EAX);
            int pop = !lhs->isInReg();
            findSpecificRegFor(lhs, FST0);

            if (lhs == rhs) {
                // NaN test.
                TEST_AH(mask);
                FNSTSW_AX();        // requires EAX to be free
                if (pop)
                    FCOMPP();
                else
                    FCOMP();
                FLDr(FST0); // DUP
            } else {
                TEST_AH(mask);
                FNSTSW_AX();        // requires EAX to be free
                if (rhs->isImmD())
                {
                    const uint64_t* p = findImmDFromPool(rhs->immDasQ());
                    FCOMdm((pop?1:0), (const double*)p);
                }
                else
                {
                    int d = findMemFor(rhs);
                    FCOM((pop?1:0), d, FP);
                }
            }
        }
    }

    // Increment the 32-bit profiling counter at pCtr, without
    // changing any registers.
    verbose_only(
    void Assembler::asm_inc_m32(uint32_t* pCtr)
    {
       INCLi(int32_t(pCtr));
    }
    )

    void Assembler::nativePageReset()
    {}

    void Assembler::nativePageSetup()
    {
        NanoAssert(!_inExit);
        if (!_nIns)
            codeAlloc(codeStart, codeEnd, _nIns verbose_only(, codeBytes));
    }

    // enough room for n bytes
    void Assembler::underrunProtect(int n)
    {
        NIns *eip = _nIns;
        NanoAssertMsg(n<=LARGEST_UNDERRUN_PROT, "constant LARGEST_UNDERRUN_PROT is too small");
        // This may be in a normal code chunk or an exit code chunk.
        if (eip - n < codeStart) {
            codeAlloc(codeStart, codeEnd, _nIns verbose_only(, codeBytes));
            JMP(eip);
        }
    }

    void Assembler::asm_ret(LIns* ins)
    {
        genEpilogue();

        // Restore ESP from EBP, undoing SUBi(SP,amt) in the prologue
        MR(SP,FP);

        releaseRegisters();
        assignSavedRegs();

        LIns *val = ins->oprnd1();
        if (ins->isop(LIR_reti)) {
            findSpecificRegFor(val, retRegs[0]);
        } else {
            NanoAssert(ins->isop(LIR_retd));
            findSpecificRegFor(val, FST0);
            fpu_pop();
        }
    }

    void Assembler::swapCodeChunks() {
        if (!_nExitIns)
            codeAlloc(exitStart, exitEnd, _nExitIns verbose_only(, exitBytes));
        SWAP(NIns*, _nIns, _nExitIns);
        SWAP(NIns*, codeStart, exitStart);
        SWAP(NIns*, codeEnd, exitEnd);
        verbose_only( SWAP(size_t, codeBytes, exitBytes); )
    }

    #endif /* FEATURE_NANOJIT */
}
