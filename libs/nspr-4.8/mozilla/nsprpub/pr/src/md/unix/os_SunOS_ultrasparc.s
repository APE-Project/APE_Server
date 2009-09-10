! -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*-
! 
! ***** BEGIN LICENSE BLOCK *****
! Version: MPL 1.1/GPL 2.0/LGPL 2.1
!
! The contents of this file are subject to the Mozilla Public License Version
! 1.1 (the "License"); you may not use this file except in compliance with
! the License. You may obtain a copy of the License at
! http://www.mozilla.org/MPL/
!
! Software distributed under the License is distributed on an "AS IS" basis,
! WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
! for the specific language governing rights and limitations under the
! License.
!
! The Original Code is the Netscape Portable Runtime (NSPR).
!
! The Initial Developer of the Original Code is
! Netscape Communications Corporation.
! Portions created by the Initial Developer are Copyright (C) 1998-2000
! the Initial Developer. All Rights Reserved.
!
! Contributor(s):
!
! Alternatively, the contents of this file may be used under the terms of
! either the GNU General Public License Version 2 or later (the "GPL"), or
! the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
! in which case the provisions of the GPL or the LGPL are applicable instead
! of those above. If you wish to allow use of your version of this file only
! under the terms of either the GPL or the LGPL, and not to allow others to
! use your version of this file under the terms of the MPL, indicate your
! decision by deleting the provisions above and replace them with the notice
! and other provisions required by the GPL or the LGPL. If you do not delete
! the provisions above, a recipient may use your version of this file under
! the terms of any one of the MPL, the GPL or the LGPL.
!
! ***** END LICENSE BLOCK *****

!
!  atomic increment, decrement and swap routines for V8+ sparc (ultrasparc)
!  using CAS (compare-and-swap) atomic instructions
!
!  this MUST be compiled with an ultrasparc-aware assembler
!
!  standard asm linkage macros; this module must be compiled
!  with the -P option (use C preprocessor)

#include <sys/asm_linkage.h>

!  ======================================================================
!
!  Perform the sequence a = a + 1 atomically with respect to other
!  fetch-and-adds to location a in a wait-free fashion.
!
!  usage : val = PR_AtomicIncrement(address)
!  return: current value (you'd think this would be old val)
!
!  -----------------------
!  Note on REGISTER USAGE:
!  as this is a LEAF procedure, a new stack frame is not created;
!  we use the caller's stack frame so what would normally be %i (input)
!  registers are actually %o (output registers).  Also, we must not
!  overwrite the contents of %l (local) registers as they are not
!  assumed to be volatile during calls.
!
!  So, the registers used are:
!     %o0  [input]   - the address of the value to increment
!     %o1  [local]   - work register
!     %o2  [local]   - work register
!     %o3  [local]   - work register
!  -----------------------

        ENTRY(PR_AtomicIncrement)       ! standard assembler/ELF prologue

retryAI:
        ld      [%o0], %o2              ! set o2 to the current value
        add     %o2, 0x1, %o3           ! calc the new value
        mov     %o3, %o1                ! save the return value
        cas     [%o0], %o2, %o3         ! atomically set if o0 hasn't changed
        cmp     %o2, %o3                ! see if we set the value
        bne     retryAI                 ! if not, try again
        nop                             ! empty out the branch pipeline
        retl                            ! return back to the caller
        mov     %o1, %o0                ! set the return code to the new value

        SET_SIZE(PR_AtomicIncrement)    ! standard assembler/ELF epilogue

!
!  end
!
!  ======================================================================
!

!  ======================================================================
!
!  Perform the sequence a = a - 1 atomically with respect to other
!  fetch-and-decs to location a in a wait-free fashion.
!
!  usage : val = PR_AtomicDecrement(address)
!  return: current value (you'd think this would be old val)
!
!  -----------------------
!  Note on REGISTER USAGE:
!  as this is a LEAF procedure, a new stack frame is not created;
!  we use the caller's stack frame so what would normally be %i (input)
!  registers are actually %o (output registers).  Also, we must not
!  overwrite the contents of %l (local) registers as they are not
!  assumed to be volatile during calls.
!
!  So, the registers used are:
!     %o0  [input]   - the address of the value to increment
!     %o1  [local]   - work register
!     %o2  [local]   - work register
!     %o3  [local]   - work register
!  -----------------------

        ENTRY(PR_AtomicDecrement)       ! standard assembler/ELF prologue

retryAD:
        ld      [%o0], %o2              ! set o2 to the current value
        sub     %o2, 0x1, %o3           ! calc the new value
        mov     %o3, %o1                ! save the return value
        cas     [%o0], %o2, %o3         ! atomically set if o0 hasn't changed
        cmp     %o2, %o3                ! see if we set the value
        bne     retryAD                 ! if not, try again
        nop                             ! empty out the branch pipeline
        retl                            ! return back to the caller
        mov     %o1, %o0                ! set the return code to the new value

        SET_SIZE(PR_AtomicDecrement)    ! standard assembler/ELF epilogue

!
!  end
!
!  ======================================================================
!

!  ======================================================================
!
!  Perform the sequence a = b atomically with respect to other
!  fetch-and-stores to location a in a wait-free fashion.
!
!  usage : old_val = PR_AtomicSet(address, newval)
!
!  -----------------------
!  Note on REGISTER USAGE:
!  as this is a LEAF procedure, a new stack frame is not created;
!  we use the caller's stack frame so what would normally be %i (input)
!  registers are actually %o (output registers).  Also, we must not
!  overwrite the contents of %l (local) registers as they are not
!  assumed to be volatile during calls.
!
!  So, the registers used are:
!     %o0  [input]   - the address of the value to increment
!     %o1  [input]   - the new value to set for [%o0]
!     %o2  [local]   - work register
!     %o3  [local]   - work register
!  -----------------------

        ENTRY(PR_AtomicSet)             ! standard assembler/ELF prologue

retryAS:
        ld      [%o0], %o2              ! set o2 to the current value
        mov     %o1, %o3                ! set up the new value
        cas     [%o0], %o2, %o3         ! atomically set if o0 hasn't changed
        cmp     %o2, %o3                ! see if we set the value
        bne     retryAS                 ! if not, try again
        nop                             ! empty out the branch pipeline
        retl                            ! return back to the caller
        mov     %o3, %o0                ! set the return code to the prev value

        SET_SIZE(PR_AtomicSet)          ! standard assembler/ELF epilogue

!
!  end
!
!  ======================================================================
!

!  ======================================================================
!
!  Perform the sequence a = a + b atomically with respect to other
!  fetch-and-adds to location a in a wait-free fashion.
!
!  usage : newval = PR_AtomicAdd(address, val)
!  return: the value after addition
!
        ENTRY(PR_AtomicAdd)       ! standard assembler/ELF prologue

retryAA:
        ld      [%o0], %o2              ! set o2 to the current value
        add     %o2, %o1, %o3           ! calc the new value
        mov     %o3, %o4                ! save the return value
        cas     [%o0], %o2, %o3         ! atomically set if o0 hasn't changed
        cmp     %o2, %o3                ! see if we set the value
        bne     retryAA                 ! if not, try again
        nop                             ! empty out the branch pipeline
        retl                            ! return back to the caller
        mov     %o4, %o0                ! set the return code to the new value

        SET_SIZE(PR_AtomicAdd)    		! standard assembler/ELF epilogue

!
!  end
!
