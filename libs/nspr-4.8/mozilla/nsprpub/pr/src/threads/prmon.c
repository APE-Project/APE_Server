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

#include "primpl.h"

/************************************************************************/

/*
** Create a new monitor.
*/
PR_IMPLEMENT(PRMonitor*) PR_NewMonitor()
{
    PRMonitor *mon;
	PRCondVar *cvar;
	PRLock *lock;

    mon = PR_NEWZAP(PRMonitor);
    if (mon) {
		lock = PR_NewLock();
	    if (!lock) {
			PR_DELETE(mon);
			return 0;
    	}

	    cvar = PR_NewCondVar(lock);
	    if (!cvar) {
	    	PR_DestroyLock(lock);
			PR_DELETE(mon);
			return 0;
    	}
    	mon->cvar = cvar;
	mon->name = NULL;
    }
    return mon;
}

PR_IMPLEMENT(PRMonitor*) PR_NewNamedMonitor(const char* name)
{
    PRMonitor* mon = PR_NewMonitor();
    if (mon)
        mon->name = name;
    return mon;
}

/*
** Destroy a monitor. There must be no thread waiting on the monitor's
** condition variable. The caller is responsible for guaranteeing that the
** monitor is no longer in use.
*/
PR_IMPLEMENT(void) PR_DestroyMonitor(PRMonitor *mon)
{
	PR_DestroyLock(mon->cvar->lock);
    PR_DestroyCondVar(mon->cvar);
    PR_DELETE(mon);
}

/*
** Enter the lock associated with the monitor.
*/
PR_IMPLEMENT(void) PR_EnterMonitor(PRMonitor *mon)
{
    if (mon->cvar->lock->owner == _PR_MD_CURRENT_THREAD()) {
		mon->entryCount++;
    } else {
		PR_Lock(mon->cvar->lock);
		mon->entryCount = 1;
    }
}

/*
** Test and then enter the lock associated with the monitor if it's not
** already entered by some other thread. Return PR_FALSE if some other
** thread owned the lock at the time of the call.
*/
PR_IMPLEMENT(PRBool) PR_TestAndEnterMonitor(PRMonitor *mon)
{
    if (mon->cvar->lock->owner == _PR_MD_CURRENT_THREAD()) {
		mon->entryCount++;
		return PR_TRUE;
    } else {
		if (PR_TestAndLock(mon->cvar->lock)) {
	    	mon->entryCount = 1;
	   	 	return PR_TRUE;
		}
    }
    return PR_FALSE;
}

/*
** Exit the lock associated with the monitor once.
*/
PR_IMPLEMENT(PRStatus) PR_ExitMonitor(PRMonitor *mon)
{
    if (mon->cvar->lock->owner != _PR_MD_CURRENT_THREAD()) {
        return PR_FAILURE;
    }
    if (--mon->entryCount == 0) {
		return PR_Unlock(mon->cvar->lock);
    }
    return PR_SUCCESS;
}

/*
** Return the number of times that the current thread has entered the
** lock. Returns zero if the current thread has not entered the lock.
*/
PR_IMPLEMENT(PRIntn) PR_GetMonitorEntryCount(PRMonitor *mon)
{
    return (mon->cvar->lock->owner == _PR_MD_CURRENT_THREAD()) ?
        mon->entryCount : 0;
}

/*
** If the current thread is in |mon|, this assertion is guaranteed to
** succeed.  Otherwise, the behavior of this function is undefined.
*/
PR_IMPLEMENT(void) PR_AssertCurrentThreadInMonitor(PRMonitor *mon)
{
    PR_ASSERT_CURRENT_THREAD_OWNS_LOCK(mon->cvar->lock);
}

/*
** Wait for a notify on the condition variable. Sleep for "ticks" amount
** of time (if "tick" is 0 then the sleep is indefinite). While
** the thread is waiting it exits the monitors lock (as if it called
** PR_ExitMonitor as many times as it had called PR_EnterMonitor).  When
** the wait has finished the thread regains control of the monitors lock
** with the same entry count as before the wait began.
**
** The thread waiting on the monitor will be resumed when the monitor is
** notified (assuming the thread is the next in line to receive the
** notify) or when the "ticks" elapses.
**
** Returns PR_FAILURE if the caller has not locked the lock associated
** with the condition variable.
** This routine can return PR_PENDING_INTERRUPT if the waiting thread 
** has been interrupted.
*/
PR_IMPLEMENT(PRStatus) PR_Wait(PRMonitor *mon, PRIntervalTime ticks)
{
    PRUintn entryCount;
	PRStatus status;
    PRThread *me = _PR_MD_CURRENT_THREAD();

    if (mon->cvar->lock->owner != me) return PR_FAILURE;

    entryCount = mon->entryCount;
    mon->entryCount = 0;

	status = _PR_WaitCondVar(me, mon->cvar, mon->cvar->lock, ticks);

    mon->entryCount = entryCount;

    return status;
}

/*
** Notify the highest priority thread waiting on the condition
** variable. If a thread is waiting on the condition variable (using
** PR_Wait) then it is awakened and begins waiting on the monitor's lock.
*/
PR_IMPLEMENT(PRStatus) PR_Notify(PRMonitor *mon)
{
    PRThread *me = _PR_MD_CURRENT_THREAD();
    if (mon->cvar->lock->owner != me) return PR_FAILURE;
    PR_NotifyCondVar(mon->cvar);
    return PR_SUCCESS;
}

/*
** Notify all of the threads waiting on the condition variable. All of
** threads are notified in turn. The highest priority thread will
** probably acquire the monitor first when the monitor is exited.
*/
PR_IMPLEMENT(PRStatus) PR_NotifyAll(PRMonitor *mon)
{
    PRThread *me = _PR_MD_CURRENT_THREAD();
    if (mon->cvar->lock->owner != me) return PR_FAILURE;
    PR_NotifyAllCondVar(mon->cvar);
    return PR_SUCCESS;
}

/************************************************************************/

PRUint32 _PR_MonitorToString(PRMonitor *mon, char *buf, PRUint32 buflen)
{
    PRUint32 nb;

    if (mon->cvar->lock->owner) {
	nb = PR_snprintf(buf, buflen, "[%p] owner=%d[%p] count=%ld",
			 mon, mon->cvar->lock->owner->id,
			 mon->cvar->lock->owner, mon->entryCount);
    } else {
	nb = PR_snprintf(buf, buflen, "[%p]", mon);
    }
    return nb;
}
