/* -*- Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil; tab-width: 4 -*- */
/* vi: set ts=4 sw=4 expandtab: (add to ~/.vimrc: set modeline modelines=5) */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version 1.1 (the
 * "License"); you may not use this file except in compliance with the License. You may obtain
 * a copy of the License at http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis, WITHOUT
 * WARRANTY OF ANY KIND, either express or implied. See the License for the specific
 * language governing rights and limitations under the License.
 *
 * The Original Code is [Open Source Virtual Machine.]
 *
 * The Initial Developer of the Original Code is Adobe System Incorporated.  Portions created
 * by the Initial Developer are Copyright (C)[ 2004-2006 ] Adobe Systems Incorporated. All Rights
 * Reserved.
 *
 * Contributor(s): Adobe AS3 Team
 *                 Andreas Gal <gal@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of either the GNU
 * General Public License Version 2 or later (the "GPL"), or the GNU Lesser General Public
 * License Version 2.1 or later (the "LGPL"), in which case the provisions of the GPL or the
 * LGPL are applicable instead of those above. If you wish to allow use of your version of this
 * file only under the terms of either the GPL or the LGPL, and not to allow others to use your
 * version of this file under the terms of the MPL, indicate your decision by deleting provisions
 * above and replace them with the notice and other provisions required by the GPL or the
 * LGPL. If you do not delete the provisions above, a recipient may use your version of this file
 * under the terms of any one of the MPL, the GPL or the LGPL.
 *
 ***** END LICENSE BLOCK ***** */

#include "nanojit.h"

#ifdef SOLARIS
	#include <ucontext.h>
	#include <dlfcn.h>
	#include <procfs.h>
	#include <sys/stat.h>
    extern "C" caddr_t _getfp(void);
    typedef caddr_t maddr_ptr;
#else
    typedef void *maddr_ptr;
#endif

using namespace avmplus;

Config AvmCore::config;

void
avmplus::AvmLog(char const *msg, ...) {
}

#ifdef _DEBUG
void NanoAssertFail()
{
    #if defined(WIN32)
        DebugBreak();
        exit(3);
    #elif defined(XP_OS2) || (defined(__GNUC__) && defined(__i386))
        asm("int $3");
    #endif

    abort();
}
#endif

#ifdef WINCE

// Due to the per-process heap slots on Windows Mobile, we can often run in to OOM
// situations.  jemalloc has worked around this problem, and so we use it here.
// Using posix_memalign (or other malloc)functions) here only works because the OS
// and hardware doesn't check for the execute bit being set.

#ifndef MOZ_MEMORY
#error MOZ_MEMORY required for building on WINCE
#endif

void*
nanojit::CodeAlloc::allocCodeChunk(size_t nbytes) {
    void * buffer;
    posix_memalign(&buffer, 4096, nbytes);
    return buffer;
}

void
nanojit::CodeAlloc::freeCodeChunk(void *p, size_t nbytes) {
    ::free(p);
}

#elif defined(WIN32)

void*
nanojit::CodeAlloc::allocCodeChunk(size_t nbytes) {
    return VirtualAlloc(NULL,
                        nbytes,
                        MEM_COMMIT | MEM_RESERVE,
                        PAGE_EXECUTE_READWRITE);
}

void
nanojit::CodeAlloc::freeCodeChunk(void *p, size_t nbytes) {
    VirtualFree(p, 0, MEM_RELEASE);
}

#elif defined(AVMPLUS_OS2)

void*
nanojit::CodeAlloc::allocCodeChunk(size_t nbytes) {

    // alloc from high memory, fallback to low memory if that fails
    void * addr;
    if (DosAllocMem(&addr, nbytes, OBJ_ANY |
                    PAG_COMMIT | PAG_READ | PAG_WRITE | PAG_EXECUTE)) {
        if (DosAllocMem(&addr, nbytes,
                        PAG_COMMIT | PAG_READ | PAG_WRITE | PAG_EXECUTE)) {
            return 0;
        }
    }
    return addr;
}

void
nanojit::CodeAlloc::freeCodeChunk(void *p, size_t nbytes) {
    DosFreeMem(p);
}

#elif defined(AVMPLUS_UNIX)

void*
nanojit::CodeAlloc::allocCodeChunk(size_t nbytes) {
    return mmap(NULL,
                nbytes,
                PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANON,
                -1,
                0);
}

void
nanojit::CodeAlloc::freeCodeChunk(void *p, size_t nbytes) {
    munmap((maddr_ptr)p, nbytes);
}

#else // !WIN32 && !AVMPLUS_OS2 && !AVMPLUS_UNIX

void*
nanojit::CodeAlloc::allocCodeChunk(size_t nbytes) {
    return valloc(nbytes);
}

void
nanojit::CodeAlloc::freeCodeChunk(void *p, size_t nbytes) {
    ::free(p);
}

#endif // WIN32

