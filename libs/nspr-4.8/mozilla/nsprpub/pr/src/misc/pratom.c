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

/*
**     PR Atomic operations
*/


#include "pratom.h"
#include "primpl.h"

#include <string.h>

/*
 * The following is a fallback implementation that emulates
 * atomic operations for platforms without atomic operations.
 * If a platform has atomic operations, it should define the
 * macro _PR_HAVE_ATOMIC_OPS, and the following will not be
 * compiled in.
 */

#if !defined(_PR_HAVE_ATOMIC_OPS)

#if defined(_PR_PTHREADS) && !defined(_PR_DCETHREADS)
/*
 * PR_AtomicDecrement() is used in NSPR's thread-specific data
 * destructor.  Because thread-specific data destructors may be
 * invoked after a PR_Cleanup() call, we need an implementation
 * of the atomic routines that doesn't need NSPR to be initialized.
 */

/*
 * We use a set of locks for all the emulated atomic operations.
 * By hashing on the address of the integer to be locked the
 * contention between multiple threads should be lessened.
 *
 * The number of atomic locks can be set by the environment variable
 * NSPR_ATOMIC_HASH_LOCKS
 */

/*
 * lock counts should be a power of 2
 */
#define DEFAULT_ATOMIC_LOCKS	16	/* should be in sync with the number of initializers
										below */
#define MAX_ATOMIC_LOCKS		(4 * 1024)

static pthread_mutex_t static_atomic_locks[DEFAULT_ATOMIC_LOCKS] = {
        PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };

#ifdef DEBUG
static PRInt32 static_hash_lock_counts[DEFAULT_ATOMIC_LOCKS];
static PRInt32 *hash_lock_counts = static_hash_lock_counts;
#endif

static PRUint32	num_atomic_locks = DEFAULT_ATOMIC_LOCKS;
static pthread_mutex_t *atomic_locks = static_atomic_locks;
static PRUint32 atomic_hash_mask = DEFAULT_ATOMIC_LOCKS - 1;

#define _PR_HASH_FOR_LOCK(ptr) 							\
			((PRUint32) (((PRUptrdiff) (ptr) >> 2)	^	\
						((PRUptrdiff) (ptr) >> 8)) &	\
						atomic_hash_mask)

void _PR_MD_INIT_ATOMIC()
{
char *eval;
int index;


	PR_ASSERT(PR_FloorLog2(MAX_ATOMIC_LOCKS) ==
						PR_CeilingLog2(MAX_ATOMIC_LOCKS));

	PR_ASSERT(PR_FloorLog2(DEFAULT_ATOMIC_LOCKS) ==
							PR_CeilingLog2(DEFAULT_ATOMIC_LOCKS));

	if (((eval = getenv("NSPR_ATOMIC_HASH_LOCKS")) != NULL)  &&
		((num_atomic_locks = atoi(eval)) != DEFAULT_ATOMIC_LOCKS)) {

		if (num_atomic_locks > MAX_ATOMIC_LOCKS)
			num_atomic_locks = MAX_ATOMIC_LOCKS;
		else if (num_atomic_locks < 1) 
			num_atomic_locks = 1;
		else {
			num_atomic_locks = PR_FloorLog2(num_atomic_locks);
			num_atomic_locks = 1L << num_atomic_locks;
		}
		atomic_locks = (pthread_mutex_t *) PR_Malloc(sizeof(pthread_mutex_t) *
						num_atomic_locks);
		if (atomic_locks) {
			for (index = 0; index < num_atomic_locks; index++) {
				if (pthread_mutex_init(&atomic_locks[index], NULL)) {
						PR_DELETE(atomic_locks);
						atomic_locks = NULL;
						break; 
				}
			}
		}
#ifdef DEBUG
		if (atomic_locks) {
			hash_lock_counts = PR_CALLOC(num_atomic_locks * sizeof(PRInt32));
			if (hash_lock_counts == NULL) {
				PR_DELETE(atomic_locks);
				atomic_locks = NULL;
			}
		}
#endif
		if (atomic_locks == NULL) {
			/*
			 *	Use statically allocated locks
			 */
			atomic_locks = static_atomic_locks;
			num_atomic_locks = DEFAULT_ATOMIC_LOCKS;
	#ifdef DEBUG
			hash_lock_counts = static_hash_lock_counts;
	#endif
		}
		atomic_hash_mask = num_atomic_locks - 1;
	}
	PR_ASSERT(PR_FloorLog2(num_atomic_locks) ==
								PR_CeilingLog2(num_atomic_locks));
}

PRInt32
_PR_MD_ATOMIC_INCREMENT(PRInt32 *val)
{
    PRInt32 rv;
    PRInt32 idx = _PR_HASH_FOR_LOCK(val);

    pthread_mutex_lock(&atomic_locks[idx]);
    rv = ++(*val);
#ifdef DEBUG
    hash_lock_counts[idx]++;
#endif
    pthread_mutex_unlock(&atomic_locks[idx]);
    return rv;
}

PRInt32
_PR_MD_ATOMIC_ADD(PRInt32 *ptr, PRInt32 val)
{
    PRInt32 rv;
    PRInt32 idx = _PR_HASH_FOR_LOCK(ptr);

    pthread_mutex_lock(&atomic_locks[idx]);
    rv = ((*ptr) += val);
#ifdef DEBUG
    hash_lock_counts[idx]++;
#endif
    pthread_mutex_unlock(&atomic_locks[idx]);
    return rv;
}

PRInt32
_PR_MD_ATOMIC_DECREMENT(PRInt32 *val)
{
    PRInt32 rv;
    PRInt32 idx = _PR_HASH_FOR_LOCK(val);

    pthread_mutex_lock(&atomic_locks[idx]);
    rv = --(*val);
#ifdef DEBUG
    hash_lock_counts[idx]++;
#endif
    pthread_mutex_unlock(&atomic_locks[idx]);
    return rv;
}

PRInt32
_PR_MD_ATOMIC_SET(PRInt32 *val, PRInt32 newval)
{
    PRInt32 rv;
    PRInt32 idx = _PR_HASH_FOR_LOCK(val);

    pthread_mutex_lock(&atomic_locks[idx]);
    rv = *val;
    *val = newval;
#ifdef DEBUG
    hash_lock_counts[idx]++;
#endif
    pthread_mutex_unlock(&atomic_locks[idx]);
    return rv;
}
#else  /* _PR_PTHREADS && !_PR_DCETHREADS */
/*
 * We use a single lock for all the emulated atomic operations.
 * The lock contention should be acceptable.
 */
static PRLock *atomic_lock = NULL;
void _PR_MD_INIT_ATOMIC(void)
{
    if (atomic_lock == NULL) {
        atomic_lock = PR_NewLock();
    }
}

PRInt32
_PR_MD_ATOMIC_INCREMENT(PRInt32 *val)
{
    PRInt32 rv;

    if (!_pr_initialized) {
        _PR_ImplicitInitialization();
    }
    PR_Lock(atomic_lock);
    rv = ++(*val);
    PR_Unlock(atomic_lock);
    return rv;
}

PRInt32
_PR_MD_ATOMIC_ADD(PRInt32 *ptr, PRInt32 val)
{
    PRInt32 rv;

    if (!_pr_initialized) {
        _PR_ImplicitInitialization();
    }
    PR_Lock(atomic_lock);
    rv = ((*ptr) += val);
    PR_Unlock(atomic_lock);
    return rv;
}

PRInt32
_PR_MD_ATOMIC_DECREMENT(PRInt32 *val)
{
    PRInt32 rv;

    if (!_pr_initialized) {
        _PR_ImplicitInitialization();
    }
    PR_Lock(atomic_lock);
    rv = --(*val);
    PR_Unlock(atomic_lock);
    return rv;
}

PRInt32
_PR_MD_ATOMIC_SET(PRInt32 *val, PRInt32 newval)
{
    PRInt32 rv;

    if (!_pr_initialized) {
        _PR_ImplicitInitialization();
    }
    PR_Lock(atomic_lock);
    rv = *val;
    *val = newval;
    PR_Unlock(atomic_lock);
    return rv;
}
#endif  /* _PR_PTHREADS && !_PR_DCETHREADS */

#endif  /* !_PR_HAVE_ATOMIC_OPS */

void _PR_InitAtomic(void)
{
    _PR_MD_INIT_ATOMIC();
}

PR_IMPLEMENT(PRInt32)
PR_AtomicIncrement(PRInt32 *val)
{
    return _PR_MD_ATOMIC_INCREMENT(val);
}

PR_IMPLEMENT(PRInt32)
PR_AtomicDecrement(PRInt32 *val)
{
    return _PR_MD_ATOMIC_DECREMENT(val);
}

PR_IMPLEMENT(PRInt32)
PR_AtomicSet(PRInt32 *val, PRInt32 newval)
{
    return _PR_MD_ATOMIC_SET(val, newval);
}

PR_IMPLEMENT(PRInt32)
PR_AtomicAdd(PRInt32 *ptr, PRInt32 val)
{
    return _PR_MD_ATOMIC_ADD(ptr, val);
}
/*
 * For platforms, which don't support the CAS (compare-and-swap) instruction
 * (or an equivalent), the stack operations are implemented by use of PRLock
 */

PR_IMPLEMENT(PRStack *)
PR_CreateStack(const char *stack_name)
{
PRStack *stack;

    if (!_pr_initialized) {
        _PR_ImplicitInitialization();
    }

    if ((stack = PR_NEW(PRStack)) == NULL) {
		return NULL;
	}
	if (stack_name) {
		stack->prstk_name = (char *) PR_Malloc(strlen(stack_name) + 1);
		if (stack->prstk_name == NULL) {
			PR_DELETE(stack);
			return NULL;
		}
		strcpy(stack->prstk_name, stack_name);
	} else
		stack->prstk_name = NULL;

#ifndef _PR_HAVE_ATOMIC_CAS
    stack->prstk_lock = PR_NewLock();
	if (stack->prstk_lock == NULL) {
		PR_Free(stack->prstk_name);
		PR_DELETE(stack);
		return NULL;
	}
#endif /* !_PR_HAVE_ATOMIC_CAS */

	stack->prstk_head.prstk_elem_next = NULL;
	
    return stack;
}

PR_IMPLEMENT(PRStatus)
PR_DestroyStack(PRStack *stack)
{
	if (stack->prstk_head.prstk_elem_next != NULL) {
		PR_SetError(PR_INVALID_STATE_ERROR, 0);
		return PR_FAILURE;
	}

	if (stack->prstk_name)
		PR_Free(stack->prstk_name);
#ifndef _PR_HAVE_ATOMIC_CAS
	PR_DestroyLock(stack->prstk_lock);
#endif /* !_PR_HAVE_ATOMIC_CAS */
	PR_DELETE(stack);

	return PR_SUCCESS;
}

#ifndef _PR_HAVE_ATOMIC_CAS

PR_IMPLEMENT(void)
PR_StackPush(PRStack *stack, PRStackElem *stack_elem)
{
    PR_Lock(stack->prstk_lock);
	stack_elem->prstk_elem_next = stack->prstk_head.prstk_elem_next;
	stack->prstk_head.prstk_elem_next = stack_elem;
    PR_Unlock(stack->prstk_lock);
    return;
}

PR_IMPLEMENT(PRStackElem *)
PR_StackPop(PRStack *stack)
{
PRStackElem *element;

    PR_Lock(stack->prstk_lock);
	element = stack->prstk_head.prstk_elem_next;
	if (element != NULL) {
		stack->prstk_head.prstk_elem_next = element->prstk_elem_next;
		element->prstk_elem_next = NULL;	/* debugging aid */
	}
    PR_Unlock(stack->prstk_lock);
    return element;
}
#endif /* !_PR_HAVE_ATOMIC_CAS */
