/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
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
 * The Original Code is the Netscape Portable Runtime (NSPR).
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998-2000
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
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

/* We want position independent code */
#define PIC

#include <sys/asm.h>
#include <sys/regdef.h>
#include <sys/syscall.h>

	.file 1 "os_ReliantUNIX.s"
	.option pic2
	.text

	.align	2
	.globl	getcxt
	.ent	getcxt
getcxt:
	.frame	sp,0,$31		# vars= 0, regs= 0/0, args= 0, extra= 0
	# saved integer regs
	sw	ra,180(a0)	# gpregs[CXT_EPC]
	sw	gp,152(a0)	# gpregs[CXT_GP]
	sw	sp,156(a0)	# gpregs[CXT_SP]
	sw	s8,160(a0)	# gpregs[CXT_S8]
	sw	s0,104(a0)	# gpregs[CXT_S0]
	sw	s1,108(a0)	# gpregs[CXT_S1]
	sw	s2,112(a0)	# gpregs[CXT_S2]
	sw	s3,116(a0)	# gpregs[CXT_S3]
	sw	s4,120(a0)	# gpregs[CXT_S4]
	sw	s5,124(a0)	# gpregs[CXT_S5]
	sw	s6,128(a0)	# gpregs[CXT_S6]
	sw	s7,132(a0)	# gpregs[CXT_S7]
	# csr
	cfc1	v0,$31
	# saved float regs
	s.d	$f20,264(a0)	# fpregs.fp_r.fp_dregs[10]
	s.d	$f22,272(a0)	# fpregs.fp_r.fp_dregs[11]
	s.d	$f24,280(a0)	# fpregs.fp_r.fp_dregs[12]
	s.d	$f26,288(a0)	# fpregs.fp_r.fp_dregs[13]
	s.d	$f28,296(a0)	# fpregs.fp_r.fp_dregs[14]
	s.d	$f30,304(a0)	# fpregs.fp_r.fp_dregs[15]
	sw	v0,312(a0)	# fpregs.fp_csr

	# give no illusions about the contents
	li	v0,0x0c		# UC_CPU | UC_MAU
	sw	v0,0(a0)	# uc_flags

	move	v0,zero
	j	ra
	.end	getcxt

	.align	2
	.globl	setcxt
	.ent	setcxt
setcxt:
	.frame	sp,0,$31		# vars= 0, regs= 0/0, args= 0, extra= 0
	lw	v0,312(a0)	# fpregs.fp_csr
	li	v1,0xfffc0fff	# mask out exception cause bits
	and	v0,v0,v1
	# saved integer regs
	lw	t9,180(a0)	# gpregs[CXT_EPC]
	lw	ra,180(a0)	# gpregs[CXT_EPC]
	lw	gp,152(a0)	# gpregs[CXT_GP]
	lw	sp,156(a0)	# gpregs[CXT_SP]
	ctc1	v0,$31		# fp_csr
	lw	s8,160(a0)	# gpregs[CXT_S8]
	lw	s0,104(a0)	# gpregs[CXT_S0]
	lw	s1,108(a0)	# gpregs[CXT_S1]
	lw	s2,112(a0)	# gpregs[CXT_S2]
	lw	s3,116(a0)	# gpregs[CXT_S3]
	lw	s4,120(a0)	# gpregs[CXT_S4]
	lw	s5,124(a0)	# gpregs[CXT_S5]
	lw	s6,128(a0)	# gpregs[CXT_S6]
	lw	s7,132(a0)	# gpregs[CXT_S7]
	# saved float regs
	l.d	$f20,264(a0)	# fpregs.fp_r.fp_dregs[10]
	l.d	$f22,272(a0)	# fpregs.fp_r.fp_dregs[11]
	l.d	$f24,280(a0)	# fpregs.fp_r.fp_dregs[12]
	l.d	$f26,288(a0)	# fpregs.fp_r.fp_dregs[13]
	l.d	$f28,296(a0)	# fpregs.fp_r.fp_dregs[14]
	l.d	$f30,304(a0)	# fpregs.fp_r.fp_dregs[15]

	# load these, too
	# they were not saved, but maybe the user modified them...
	lw	v0,48(a0)
	lw	v1,52(a0)
	lw	a1,60(a0)
	lw	a2,64(a0)
	lw	a3,68(a0)
	lw	a0,56(a0)	# there is no way back

	j	ra

	.end	setcxt
