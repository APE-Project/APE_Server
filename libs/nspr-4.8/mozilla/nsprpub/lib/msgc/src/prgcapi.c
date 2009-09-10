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

#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include "prenv.h"
#include "prmem.h"
#include "prmon.h"
#include "prlog.h"
#include "prthread.h"
#include "private/pprthred.h"
#include "gcint.h"

/*
** Generic GC implementation independent code for the NSPR GC
*/

RootFinder *_pr_rootFinders;

CollectorType *_pr_collectorTypes;

/* GC State information */
GCInfo _pr_gcData;

GCBeginGCHook *_pr_beginGCHook;
void *_pr_beginGCHookArg;
GCBeginGCHook *_pr_endGCHook;
void *_pr_endGCHookArg;

GCBeginFinalizeHook *_pr_beginFinalizeHook;
void *_pr_beginFinalizeHookArg;
GCBeginFinalizeHook *_pr_endFinalizeHook;
void *_pr_endFinalizeHookArg;

FILE *_pr_dump_file;
int _pr_do_a_dump;
GCLockHook *_pr_GCLockHook;

extern PRLogModuleInfo *_pr_msgc_lm;

/************************************************************************/

static PRStatus PR_CALLBACK
pr_ScanOneThread(PRThread* t, void** addr, PRUword count, void* closure)
{
    _pr_gcData.processRootBlock(addr, count);
    return PR_SUCCESS;
}

/*
** Scan all of the threads C stack's and registers, looking for "root"
** pointers into the GC heap. These are the objects that the GC cannot
** move and are considered "live" by the GC. Caller has stopped all of
** the threads from running.
*/
static void PR_CALLBACK ScanThreads(void *arg)
{
    PR_ScanStackPointers(pr_ScanOneThread, arg);
}

/************************************************************************/

PR_IMPLEMENT(GCInfo *) PR_GetGCInfo(void)
{
    return &_pr_gcData;
}


PR_IMPLEMENT(PRInt32) PR_RegisterType(GCType *t)
{
    CollectorType *ct, *ect;
    int rv = -1;

    LOCK_GC();
    ct = &_pr_collectorTypes[0];
    ect = &_pr_collectorTypes[FREE_MEMORY_TYPEIX];
    for (; ct < ect; ct++) {
	if (ct->flags == 0) {
	    ct->gctype = *t;
	    ct->flags = _GC_TYPE_BUSY;
	    if (0 != ct->gctype.finalize) {
		ct->flags |= _GC_TYPE_FINAL;
	    }
	    if (0 != ct->gctype.getWeakLinkOffset) {
		ct->flags |= _GC_TYPE_WEAK;
	    }
	    rv = ct - &_pr_collectorTypes[0];
	    break;
	}
    }
    UNLOCK_GC();
    return rv;
}

PR_IMPLEMENT(PRStatus) PR_RegisterRootFinder(
    GCRootFinder f, char *name, void *arg)
{
    RootFinder *rf = PR_NEWZAP(RootFinder);
    if (rf) {
	    rf->func = f;
	    rf->name = name;
	    rf->arg = arg;

	    LOCK_GC();
	    rf->next = _pr_rootFinders;
	    _pr_rootFinders = rf;
	    UNLOCK_GC();
	    return PR_SUCCESS;
    }
    return PR_FAILURE;
}


PR_IMPLEMENT(int) PR_RegisterGCLockHook(GCLockHookFunc* f, void *arg)
{

    GCLockHook *rf = 0;

    rf = (GCLockHook*) calloc(1, sizeof(GCLockHook));
    if (rf) {
	rf->func = f;
	rf->arg = arg;

	LOCK_GC();
        /* first dummy node */
        if (! _pr_GCLockHook) {
          _pr_GCLockHook = (GCLockHook*) calloc(1, sizeof(GCLockHook));
          _pr_GCLockHook->next = _pr_GCLockHook;
          _pr_GCLockHook->prev = _pr_GCLockHook;
        }

        rf->next = _pr_GCLockHook;
        rf->prev = _pr_GCLockHook->prev;
        _pr_GCLockHook->prev->next = rf;
        _pr_GCLockHook->prev = rf;
	UNLOCK_GC();
	return 0;
    }
    return -1;
}

/*
PR_IMPLEMENT(void) PR_SetGCLockHook(GCLockHook *hook, void *arg)
{
    LOCK_GC();
    _pr_GCLockHook = hook;
    _pr_GCLockHookArg2 = arg;
    UNLOCK_GC();
}

PR_IMPLEMENT(void) PR_GetGCLockHook(GCLockHook **hook, void **arg)
{
    LOCK_GC();
    *hook = _pr_GCLockHook;
    *arg = _pr_GCLockHookArg2;
    UNLOCK_GC();
}
*/

  
PR_IMPLEMENT(void) PR_SetBeginGCHook(GCBeginGCHook *hook, void *arg)
{
    LOCK_GC();
    _pr_beginGCHook = hook;
    _pr_beginGCHookArg = arg;
    UNLOCK_GC();
}

PR_IMPLEMENT(void) PR_GetBeginGCHook(GCBeginGCHook **hook, void **arg)
{
    LOCK_GC();
    *hook = _pr_beginGCHook;
    *arg = _pr_beginGCHookArg;
    UNLOCK_GC();
}

PR_IMPLEMENT(void) PR_SetEndGCHook(GCEndGCHook *hook, void *arg)
{
    LOCK_GC();
    _pr_endGCHook = hook;
    _pr_endGCHookArg = arg;
    UNLOCK_GC();
}

PR_IMPLEMENT(void) PR_GetEndGCHook(GCEndGCHook **hook, void **arg)
{
    LOCK_GC();
    *hook = _pr_endGCHook;
    *arg = _pr_endGCHookArg;
    UNLOCK_GC();
}

PR_IMPLEMENT(void) PR_SetBeginFinalizeHook(GCBeginFinalizeHook *hook, void *arg)
{
    LOCK_GC();
    _pr_beginFinalizeHook = hook;
    _pr_beginFinalizeHookArg = arg;
    UNLOCK_GC();
}

PR_IMPLEMENT(void) PR_GetBeginFinalizeHook(GCBeginFinalizeHook **hook, 
                                           void **arg)
{
    LOCK_GC();
    *hook = _pr_beginFinalizeHook;
    *arg = _pr_beginFinalizeHookArg;
    UNLOCK_GC();
}

PR_IMPLEMENT(void) PR_SetEndFinalizeHook(GCEndFinalizeHook *hook, void *arg)
{
    LOCK_GC();
    _pr_endFinalizeHook = hook;
    _pr_endFinalizeHookArg = arg;
    UNLOCK_GC();
}

PR_IMPLEMENT(void) PR_GetEndFinalizeHook(GCEndFinalizeHook **hook, void **arg)
{
    LOCK_GC();
    *hook = _pr_endFinalizeHook;
    *arg = _pr_endFinalizeHookArg;
    UNLOCK_GC();
}

#ifdef DEBUG
#include "prprf.h"

PR_IMPLEMENT(void) GCTrace(char *fmt, ...)
{	
    va_list ap;
    char buf[400];

    va_start(ap, fmt);
    PR_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    PR_LOG(_pr_msgc_lm, PR_LOG_ALWAYS, ("%s", buf));
}
#endif

void _PR_InitGC(PRWord flags)
{
    static char firstTime = 1;

    if (!firstTime) return;
    firstTime = 0;

    _MD_InitGC();

    if (flags == 0) {
	char *ev = PR_GetEnv("GCLOG");
	if (ev && ev[0]) {
	    flags = atoi(ev);
	}
    }
    _pr_gcData.flags = flags;

    _pr_gcData.lock = PR_NewMonitor();

    _pr_collectorTypes = (CollectorType*) PR_CALLOC(256 * sizeof(CollectorType));

    PR_RegisterRootFinder(ScanThreads, "scan threads", 0);
    PR_RegisterRootFinder(_PR_ScanFinalQueue, "scan final queue", 0);
}

extern void pr_FinalizeOnExit(void);

#ifdef DEBUG
#ifdef GC_STATS
PR_PUBLIC_API(void) PR_PrintGCAllocStats(void);
#endif 
#endif 
  
PR_IMPLEMENT(void) 
PR_ShutdownGC(PRBool finalizeOnExit)
{
    /* first finalize all the objects in the heap */
    if (finalizeOnExit) {
        pr_FinalizeOnExit();
    }

#ifdef DEBUG
#ifdef GC_STATS
    PR_PrintGCAllocStats();
#endif /* GC_STATS */
#endif /* DEBUG */

    /* then the chance for any future allocations */

    /* finally delete the gc heap */

    /* write me */
}

/******************************************************************************/
