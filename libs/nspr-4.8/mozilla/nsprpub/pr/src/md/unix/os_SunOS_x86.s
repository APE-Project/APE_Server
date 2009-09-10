/ -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*-
/ 
/ ***** BEGIN LICENSE BLOCK *****
/ Version: MPL 1.1/GPL 2.0/LGPL 2.1
/
/ The contents of this file are subject to the Mozilla Public License Version
/ 1.1 (the "License"); you may not use this file except in compliance with
/ the License. You may obtain a copy of the License at
/ http://www.mozilla.org/MPL/
/
/ Software distributed under the License is distributed on an "AS IS" basis,
/ WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
/ for the specific language governing rights and limitations under the
/ License.
/
/ The Original Code is the Netscape Portable Runtime (NSPR).
/
/ The Initial Developer of the Original Code is
/ Netscape Communications Corporation.
/ Portions created by the Initial Developer are Copyright (C) 1998-2000
/ the Initial Developer. All Rights Reserved.
/
/ Contributor(s):
/
/ Alternatively, the contents of this file may be used under the terms of
/ either the GNU General Public License Version 2 or later (the "GPL"), or
/ the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
/ in which case the provisions of the GPL or the LGPL are applicable instead
/ of those above. If you wish to allow use of your version of this file only
/ under the terms of either the GPL or the LGPL, and not to allow others to
/ use your version of this file under the terms of the MPL, indicate your
/ decision by deleting the provisions above and replace them with the notice
/ and other provisions required by the GPL or the LGPL. If you do not delete
/ the provisions above, a recipient may use your version of this file under
/ the terms of any one of the MPL, the GPL or the LGPL.
/
/ ***** END LICENSE BLOCK *****

	.text

	.globl	getedi
getedi:
	movl	%edi,%eax
	ret
	.type	getedi,@function
	.size	getedi,.-getedi
 
	.globl	setedi
setedi:
	movl	4(%esp),%edi
	ret
	.type	setedi,@function
	.size	setedi,.-setedi

	.globl	__MD_FlushRegisterWindows
	.globl _MD_FlushRegisterWindows

__MD_FlushRegisterWindows:
_MD_FlushRegisterWindows:

	ret

/
/ sol_getsp()
/
/ Return the current sp (for debugging)
/
	.globl sol_getsp
sol_getsp:
	movl	%esp, %eax
	ret

/
/ sol_curthread()
/
/ Return a unique identifier for the currently active thread.
/
	.globl sol_curthread
sol_curthread:
	movl	%ecx, %eax
	ret

/ PRInt32 _MD_AtomicIncrement(PRInt32 *val)
/
/ Atomically increment the integer pointed to by 'val' and return
/ the result of the increment.
/
    .text
    .globl _MD_AtomicIncrement
    .align 4
_MD_AtomicIncrement:
    movl 4(%esp), %ecx
    movl $1, %eax
    lock
    xaddl %eax, (%ecx)
    incl %eax
    ret

/ PRInt32 _MD_AtomicDecrement(PRInt32 *val)
/
/ Atomically decrement the integer pointed to by 'val' and return
/ the result of the decrement.
/
    .text
    .globl _MD_AtomicDecrement
    .align 4
_MD_AtomicDecrement:
    movl 4(%esp), %ecx
    movl $-1, %eax
    lock
    xaddl %eax, (%ecx)
    decl %eax
    ret

/ PRInt32 _MD_AtomicSet(PRInt32 *val, PRInt32 newval)
/
/ Atomically set the integer pointed to by 'val' to the new
/ value 'newval' and return the old value.
/
/ An alternative implementation:
/   .text
/   .globl _MD_AtomicSet
/   .align 4
/_MD_AtomicSet:
/   movl 4(%esp), %ecx
/   movl 8(%esp), %edx
/   movl (%ecx), %eax
/retry:
/   lock
/   cmpxchgl %edx, (%ecx)
/   jne retry
/   ret
/
    .text
    .globl _MD_AtomicSet
    .align 4
_MD_AtomicSet:
    movl 4(%esp), %ecx
    movl 8(%esp), %eax
    xchgl %eax, (%ecx)
    ret

/ PRInt32 _MD_AtomicAdd(PRInt32 *ptr, PRInt32 val)
/
/ Atomically add 'val' to the integer pointed to by 'ptr'
/ and return the result of the addition.
/
    .text
    .globl _MD_AtomicAdd
    .align 4
_MD_AtomicAdd:
    movl 4(%esp), %ecx
    movl 8(%esp), %eax
    movl %eax, %edx
    lock
    xaddl %eax, (%ecx)
    addl %edx, %eax
    ret
