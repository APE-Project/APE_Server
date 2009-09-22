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

#include <string.h>

/*****************************************************************************/
/*****************************************************************************/
/************************** File descriptor caching **************************/
/*****************************************************************************/
/*****************************************************************************/

/*
** This code is built into debuggable versions of NSPR to assist in
** finding misused file descriptors. Since file descritors (PRFileDesc)
** are identified by a pointer to their structure, they can be the
** target of dangling references. Furthermore, NSPR caches and tries
** to aggressively reuse file descriptors, leading to more ambiguity.
** The following code will allow a debugging client to set environment
** variables and control the number of file descriptors that will be
** preserved before they are recycled. The environment variables are
** NSPR_FD_CACHE_SIZE_LOW and NSPR_FD_CACHE_SIZE_HIGH. The former sets
** the number of descriptors NSPR will allocate before beginning to
** recycle. The latter is the maximum number permitted in the cache
** (exclusive of those in use) at a time.
*/
typedef struct _PR_Fd_Cache
{
    PRLock *ml;
    PRIntn count;
    PRStack *stack;
    PRFileDesc *head, *tail;
    PRIntn limit_low, limit_high;
} _PR_Fd_Cache;

static _PR_Fd_Cache _pr_fd_cache;
static PRFileDesc **stack2fd = &(((PRFileDesc*)NULL)->higher);


/*
** Get a FileDescriptor from the cache if one exists. If not allocate
** a new one from the heap.
*/
PRFileDesc *_PR_Getfd(void)
{
    PRFileDesc *fd;
    /*
    ** $$$
    ** This may look a little wasteful. We'll see. Right now I want to
    ** be able to toggle between caching and not at runtime to measure
    ** the differences. If it isn't too annoying, I'll leave it in.
    ** $$$$
    **
    ** The test is against _pr_fd_cache.limit_high. If that's zero,
    ** we're not doing the extended cache but going for performance.
    */
    if (0 == _pr_fd_cache.limit_high)
    {
        PRStackElem *pop;
        PR_ASSERT(NULL != _pr_fd_cache.stack);
        pop = PR_StackPop(_pr_fd_cache.stack);
        if (NULL == pop) goto allocate;
        fd = (PRFileDesc*)((PRPtrdiff)pop - (PRPtrdiff)stack2fd);
    }
    else
    {
        do
        {
            if (NULL == _pr_fd_cache.head) goto allocate;  /* nothing there */
            if (_pr_fd_cache.count < _pr_fd_cache.limit_low) goto allocate;

            /* we "should" be able to extract an fd from the cache */
            PR_Lock(_pr_fd_cache.ml);  /* need the lock to do this safely */
            fd = _pr_fd_cache.head;  /* protected extraction */
            if (NULL == fd)  /* unexpected, but not fatal */
            {
                PR_ASSERT(0 == _pr_fd_cache.count);
                PR_ASSERT(NULL == _pr_fd_cache.tail);
            }
            else
            {
                _pr_fd_cache.count -= 1;
                _pr_fd_cache.head = fd->higher;
                if (NULL == _pr_fd_cache.head)
                {
                    PR_ASSERT(0 == _pr_fd_cache.count);
                    _pr_fd_cache.tail = NULL;
                }
                PR_ASSERT(&_pr_faulty_methods == fd->methods);
                PR_ASSERT(PR_INVALID_IO_LAYER == fd->identity);
                PR_ASSERT(_PR_FILEDESC_FREED == fd->secret->state);
            }
            PR_Unlock(_pr_fd_cache.ml);

        } while (NULL == fd);  /* then go around and allocate a new one */
    }

finished:
    fd->dtor = NULL;
    fd->lower = fd->higher = NULL;
    fd->identity = PR_NSPR_IO_LAYER;
    memset(fd->secret, 0, sizeof(PRFilePrivate));
    return fd;

allocate:
    fd = PR_NEW(PRFileDesc);
    if (NULL != fd)
    {
        fd->secret = PR_NEW(PRFilePrivate);
        if (NULL == fd->secret) PR_DELETE(fd);
    }
    if (NULL != fd) goto finished;
    else return NULL;

}  /* _PR_Getfd */

/*
** Return a file descriptor to the cache unless there are too many in
** there already. If put in cache, clear the fields first.
*/
void _PR_Putfd(PRFileDesc *fd)
{
    PR_ASSERT(PR_NSPR_IO_LAYER == fd->identity);
    fd->methods = &_pr_faulty_methods;
    fd->identity = PR_INVALID_IO_LAYER;
    fd->secret->state = _PR_FILEDESC_FREED;

    if (0 == _pr_fd_cache.limit_high)
    {
        PR_StackPush(_pr_fd_cache.stack, (PRStackElem*)(&fd->higher));
    }
    else
    {
        if (_pr_fd_cache.count > _pr_fd_cache.limit_high)
        {
            PR_Free(fd->secret);
            PR_Free(fd);
        }
        else
        {
            PR_Lock(_pr_fd_cache.ml);
            if (NULL == _pr_fd_cache.tail)
            {
                PR_ASSERT(0 == _pr_fd_cache.count);
                PR_ASSERT(NULL == _pr_fd_cache.head);
                _pr_fd_cache.head = _pr_fd_cache.tail = fd;
            }
            else
            {
                PR_ASSERT(NULL == _pr_fd_cache.tail->higher);
                _pr_fd_cache.tail->higher = fd;
                _pr_fd_cache.tail = fd;  /* new value */
            }
            fd->higher = NULL;  /* always so */
            _pr_fd_cache.count += 1;  /* count the new entry */
            PR_Unlock(_pr_fd_cache.ml);
        }
    }
}  /* _PR_Putfd */

PR_IMPLEMENT(PRStatus) PR_SetFDCacheSize(PRIntn low, PRIntn high)
{
    /*
    ** This can be called at any time, may adjust the cache sizes,
    ** turn the caches off, or turn them on. It is not dependent
    ** on the compilation setting of DEBUG.
    */
    if (!_pr_initialized) _PR_ImplicitInitialization();

    if (low > high) low = high;  /* sanity check the params */
    
    PR_Lock(_pr_fd_cache.ml);
    if (0 == high)  /* shutting down or staying down */
    {
        if (0 != _pr_fd_cache.limit_high)  /* shutting down */
        {
            _pr_fd_cache.limit_high = 0;  /* stop use */
            /*
            ** Hold the lock throughout - nobody's going to want it
            ** other than another caller to this routine. Just don't
            ** let that happen.
            **
            ** Put all the cached fds onto the new cache.
            */
            while (NULL != _pr_fd_cache.head)
            {
                PRFileDesc *fd = _pr_fd_cache.head;
                _pr_fd_cache.head = fd->higher;
                PR_StackPush(_pr_fd_cache.stack, (PRStackElem*)(&fd->higher));
            }
            _pr_fd_cache.limit_low = 0;
            _pr_fd_cache.tail = NULL;
            _pr_fd_cache.count = 0;
        }
    }
    else  /* starting up or just adjusting parameters */
    {
        PRBool was_using_stack = (0 == _pr_fd_cache.limit_high);
        _pr_fd_cache.limit_low = low;
        _pr_fd_cache.limit_high = high;
        if (was_using_stack)  /* was using stack - feed into cache */
        {
            PRStackElem *pop;
            while (NULL != (pop = PR_StackPop(_pr_fd_cache.stack)))
            {
                PRFileDesc *fd = (PRFileDesc*)
                    ((PRPtrdiff)pop - (PRPtrdiff)stack2fd);
                if (NULL == _pr_fd_cache.tail) _pr_fd_cache.tail = fd;
                fd->higher = _pr_fd_cache.head;
                _pr_fd_cache.head = fd;
                _pr_fd_cache.count += 1;
            }
        }
    }
    PR_Unlock(_pr_fd_cache.ml);
    return PR_SUCCESS;
}  /* PR_SetFDCacheSize */

void _PR_InitFdCache(void)
{
    /*
    ** The fd caching is enabled by default for DEBUG builds,
    ** disabled by default for OPT builds. That default can
    ** be overridden at runtime using environment variables
    ** or a super-wiz-bang API.
    */
    const char *low = PR_GetEnv("NSPR_FD_CACHE_SIZE_LOW");
    const char *high = PR_GetEnv("NSPR_FD_CACHE_SIZE_HIGH");

    /* 
    ** _low is allowed to be zero, _high is not.
    ** If _high is zero, we're not doing the caching.
    */

    _pr_fd_cache.limit_low = 0;
#if defined(DEBUG)
    _pr_fd_cache.limit_high = FD_SETSIZE;
#else
    _pr_fd_cache.limit_high = 0;
#endif  /* defined(DEBUG) */

    if (NULL != low) _pr_fd_cache.limit_low = atoi(low);
    if (NULL != high) _pr_fd_cache.limit_high = atoi(high);

    if (_pr_fd_cache.limit_low < 0)
        _pr_fd_cache.limit_low = 0;
    if (_pr_fd_cache.limit_low > FD_SETSIZE)
        _pr_fd_cache.limit_low = FD_SETSIZE;

    if (_pr_fd_cache.limit_high > FD_SETSIZE)
        _pr_fd_cache.limit_high = FD_SETSIZE;

    if (_pr_fd_cache.limit_high < _pr_fd_cache.limit_low)
        _pr_fd_cache.limit_high = _pr_fd_cache.limit_low;

    _pr_fd_cache.ml = PR_NewLock();
    PR_ASSERT(NULL != _pr_fd_cache.ml);
    _pr_fd_cache.stack = PR_CreateStack("FD");
    PR_ASSERT(NULL != _pr_fd_cache.stack);

}  /* _PR_InitFdCache */

void _PR_CleanupFdCache(void)
{
    PRFileDesc *fd, *next;
    PRStackElem *pop;

    for (fd = _pr_fd_cache.head; fd != NULL; fd = next)
    {
        next = fd->higher;
        PR_DELETE(fd->secret);
        PR_DELETE(fd);
    }
    _pr_fd_cache.head = NULL;
    _pr_fd_cache.tail = NULL;
    _pr_fd_cache.count = 0;
    PR_DestroyLock(_pr_fd_cache.ml);
    _pr_fd_cache.ml = NULL;
    while ((pop = PR_StackPop(_pr_fd_cache.stack)) != NULL)
    {
        fd = (PRFileDesc*)((PRPtrdiff)pop - (PRPtrdiff)stack2fd);
        PR_DELETE(fd->secret);
        PR_DELETE(fd);
    }
    PR_DestroyStack(_pr_fd_cache.stack);
    _pr_fd_cache.stack = NULL;
}  /* _PR_CleanupFdCache */

/* prfdcach.c */
