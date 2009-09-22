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
/ Portions created by the Initial Developer are Copyright (C) 2000
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

/ PRInt32 _PR_x86_AtomicIncrement(PRInt32 *val)
/
/ Atomically increment the integer pointed to by 'val' and return
/ the result of the increment.
/
    .text
    .globl _PR_x86_AtomicIncrement
    .align 4
_PR_x86_AtomicIncrement:
    movl 4(%esp), %ecx
    movl $1, %eax
    lock
    xaddl %eax, (%ecx)
    incl %eax
    ret

/ PRInt32 _PR_x86_AtomicDecrement(PRInt32 *val)
/
/ Atomically decrement the integer pointed to by 'val' and return
/ the result of the decrement.
/
    .text
    .globl _PR_x86_AtomicDecrement
    .align 4
_PR_x86_AtomicDecrement:
    movl 4(%esp), %ecx
    movl $-1, %eax
    lock
    xaddl %eax, (%ecx)
    decl %eax
    ret

/ PRInt32 _PR_x86_AtomicSet(PRInt32 *val, PRInt32 newval)
/
/ Atomically set the integer pointed to by 'val' to the new
/ value 'newval' and return the old value.
/
/ An alternative implementation:
/   .text
/   .globl _PR_x86_AtomicSet
/   .align 4
/_PR_x86_AtomicSet:
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
    .globl _PR_x86_AtomicSet
    .align 4
_PR_x86_AtomicSet:
    movl 4(%esp), %ecx
    movl 8(%esp), %eax
    xchgl %eax, (%ecx)
    ret

/ PRInt32 _PR_x86_AtomicAdd(PRInt32 *ptr, PRInt32 val)
/
/ Atomically add 'val' to the integer pointed to by 'ptr'
/ and return the result of the addition.
/
    .text
    .globl _PR_x86_AtomicAdd
    .align 4
_PR_x86_AtomicAdd:
    movl 4(%esp), %ecx
    movl 8(%esp), %eax
    movl %eax, %edx
    lock
    xaddl %eax, (%ecx)
    addl %edx, %eax
    ret

/ Magic indicating no need for an executable stack
.section .note.GNU-stack, "", @progbits ; .previous
