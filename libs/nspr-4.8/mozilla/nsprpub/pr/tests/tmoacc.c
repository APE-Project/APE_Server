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

#include "nspr.h"

#include <stdlib.h>
#include <string.h>

#include "plerror.h"
#include "plgetopt.h"

#define BASE_PORT 9867
#define DEFAULT_THREADS 1
#define DEFAULT_BACKLOG 10
#define DEFAULT_TIMEOUT 10
#define RANDOM_RANGE 100  /* should be significantly smaller than RAND_MAX */

typedef enum {running, stopped} Status;

typedef struct Shared
{
    PRLock *ml;
    PRCondVar *cv;
    PRBool passed;
    PRBool random;
    PRFileDesc *debug;
    PRIntervalTime timeout;
    PRFileDesc *listenSock;
    Status status;
} Shared;

static PRIntervalTime Timeout(const Shared *shared)
{
    PRIntervalTime timeout = shared->timeout;
    if (shared->random)
    {
        PRIntervalTime half = timeout >> 1;  /* one half of the interval */
        PRIntervalTime quarter = half >> 1;  /* one quarter of the interval */
        /* something in [0..timeout / 2) */
        PRUint32 random = (rand() % RANDOM_RANGE) * half / RANDOM_RANGE;
        timeout = (3 * quarter) + random;  /* [75..125)% */
    }
    return timeout;
}  /* Timeout */

static void Accept(void *arg)
{
    PRStatus rv;
    char *buffer = NULL;
    PRNetAddr clientAddr;
    Shared *shared = (Shared*)arg;
    PRInt32 recv_length = 0, flags = 0;
    PRFileDesc *clientSock;
    PRIntn toread, byte, bytes, loop = 0;
    struct Descriptor { PRInt32 length; PRUint32 checksum; } descriptor;

    do
    {
        PRUint32 checksum = 0;
        if (NULL != shared->debug)
            PR_fprintf(shared->debug, "[%d]accepting ... ", loop++);
        clientSock = PR_Accept(
            shared->listenSock, &clientAddr, Timeout(shared));
        if (clientSock != NULL)
        {
            if (NULL != shared->debug)
                PR_fprintf(shared->debug, "reading length ... ");
            bytes = PR_Recv(
                clientSock, &descriptor, sizeof(descriptor),
                flags, Timeout(shared));
            if (sizeof(descriptor) == bytes)
            {
                /* and, before doing something stupid ... */
                descriptor.length = PR_ntohl(descriptor.length);
                descriptor.checksum = PR_ntohl(descriptor.checksum);
                if (NULL != shared->debug)
                    PR_fprintf(shared->debug, "%d bytes ... ", descriptor.length);
                toread = descriptor.length;
                if (recv_length < descriptor.length)
                {
                    if (NULL != buffer) PR_DELETE(buffer);
                    buffer = (char*)PR_MALLOC(descriptor.length);
                    recv_length = descriptor.length;
                }
                for (toread = descriptor.length; toread > 0; toread -= bytes)
                {
                    bytes = PR_Recv(
                        clientSock, &buffer[descriptor.length - toread],
                        toread, flags, Timeout(shared));
                    if (-1 == bytes)
                    {
                        if (NULL != shared->debug)
                            PR_fprintf(shared->debug, "read data failed...");
                        bytes = 0;
                    }
                }
            }
            else if (NULL != shared->debug)
            {
                PR_fprintf(shared->debug, "read desciptor failed...");
                descriptor.length = -1;
            }
            if (NULL != shared->debug)
                PR_fprintf(shared->debug, "closing");
            rv = PR_Shutdown(clientSock, PR_SHUTDOWN_BOTH);
            if ((PR_FAILURE == rv) && (NULL != shared->debug))
            {
                PR_fprintf(shared->debug, " failed");
                shared->passed = PR_FALSE;
            }
            rv = PR_Close(clientSock);
            if (PR_FAILURE == rv) if (NULL != shared->debug)
            {
                PR_fprintf(shared->debug, " failed");
                shared->passed = PR_FALSE;
            }
            if (descriptor.length > 0)
            {
                for (byte = 0; byte < descriptor.length; ++byte)
                {
                    PRUint32 overflow = checksum & 0x80000000;
                    checksum = (checksum << 1);
                    if (0x00000000 != overflow) checksum += 1;
                    checksum += buffer[byte];
                }
                if ((descriptor.checksum != checksum) && (NULL != shared->debug))
                {
                    PR_fprintf(shared->debug, " ... data mismatch");
                    shared->passed = PR_FALSE;
                }
            }
            else if (0 == descriptor.length)
            {
                PR_Lock(shared->ml);
                shared->status = stopped;
                PR_NotifyCondVar(shared->cv);
                PR_Unlock(shared->ml);
            }
            if (NULL != shared->debug)
                PR_fprintf(shared->debug, "\n");
        }
        else
        {
            if (PR_PENDING_INTERRUPT_ERROR != PR_GetError())
            {
                if (NULL != shared->debug) PL_PrintError("Accept");
                shared->passed = PR_FALSE;
            }
        }        
    } while (running == shared->status);
    if (NULL != buffer) PR_DELETE(buffer);
}  /* Accept */

PRIntn Tmoacc(PRIntn argc, char **argv)
{
    PRStatus rv;
    PRIntn exitStatus;
    PRIntn index;
	Shared *shared;
	PLOptStatus os;
	PRThread **thread;
    PRNetAddr listenAddr;
    PRSocketOptionData sockOpt;
    PRIntn timeout = DEFAULT_TIMEOUT;
    PRIntn threads = DEFAULT_THREADS;
    PRIntn backlog = DEFAULT_BACKLOG;
    PRThreadScope thread_scope = PR_LOCAL_THREAD;

	PLOptState *opt = PL_CreateOptState(argc, argv, "dGb:t:T:R");

    shared = PR_NEWZAP(Shared);

    shared->debug = NULL;
    shared->passed = PR_TRUE;
    shared->random = PR_TRUE;
    shared->status = running;
    shared->ml = PR_NewLock();
    shared->cv = PR_NewCondVar(shared->ml);

	while (PL_OPT_EOL != (os = PL_GetNextOpt(opt)))
    {
        if (PL_OPT_BAD == os) continue;
        switch (opt->option)
        {
        case 'd':  /* debug mode */
            shared->debug = PR_GetSpecialFD(PR_StandardError);
            break;
        case 'G':  /* use global threads */
            thread_scope = PR_GLOBAL_THREAD;
            break;
        case 'b':  /* size of listen backlog */
            backlog = atoi(opt->value);
            break;
        case 't':  /* number of threads doing accept */
            threads = atoi(opt->value);
            break;
        case 'T':  /* timeout used for network operations */
            timeout = atoi(opt->value);
            break;
        case 'R':  /* randomize the timeout values */
            shared->random = PR_TRUE;
            break;
        default:
            break;
        }
    }
	PL_DestroyOptState(opt);
    if (0 == threads) threads = DEFAULT_THREADS;
    if (0 == backlog) backlog = DEFAULT_BACKLOG;
    if (0 == timeout) timeout = DEFAULT_TIMEOUT;
    
    PR_STDIO_INIT();
    memset(&listenAddr, 0, sizeof(listenAddr));
    rv = PR_InitializeNetAddr(PR_IpAddrAny, BASE_PORT, &listenAddr);
    PR_ASSERT(PR_SUCCESS == rv);

    shared->timeout = PR_SecondsToInterval(timeout);

    /* First bind to the socket */
    shared->listenSock = PR_NewTCPSocket();
    if (shared->listenSock)
    {
        sockOpt.option = PR_SockOpt_Reuseaddr;
        sockOpt.value.reuse_addr = PR_TRUE;
        rv = PR_SetSocketOption(shared->listenSock, &sockOpt);
        PR_ASSERT(PR_SUCCESS == rv);
        rv = PR_Bind(shared->listenSock, &listenAddr);
        if (rv != PR_FAILURE)
        {
            rv = PR_Listen(shared->listenSock, threads + backlog);
            if (PR_SUCCESS == rv)
            {
                thread = (PRThread**)PR_CALLOC(threads * sizeof(PRThread*));
                for (index = 0; index < threads; ++index)
                {
                    thread[index] = PR_CreateThread(
                        PR_USER_THREAD, Accept, shared,
                        PR_PRIORITY_NORMAL, thread_scope,
                        PR_JOINABLE_THREAD, 0);
                    PR_ASSERT(NULL != thread[index]);
                }

                PR_Lock(shared->ml);
                while (shared->status == running)
                    PR_WaitCondVar(shared->cv, PR_INTERVAL_NO_TIMEOUT);
                PR_Unlock(shared->ml);
                for (index = 0; index < threads; ++index)
                {
                    rv = PR_Interrupt(thread[index]);
                    PR_ASSERT(PR_SUCCESS== rv);
                    rv = PR_JoinThread(thread[index]);
                    PR_ASSERT(PR_SUCCESS== rv);
                }
                PR_DELETE(thread);
            }
            else
            {
                if (shared->debug) PL_PrintError("Listen");
                shared->passed = PR_FALSE;
            }
        }
        else
        {
            if (shared->debug) PL_PrintError("Bind");
            shared->passed = PR_FALSE;
        }

        PR_Close(shared->listenSock);
    }
    else
    {
        if (shared->debug) PL_PrintError("Create");
        shared->passed = PR_FALSE;
    }

    PR_DestroyCondVar(shared->cv);
    PR_DestroyLock(shared->ml);

    PR_fprintf(
        PR_GetSpecialFD(PR_StandardError), "%s\n",
        ((shared->passed) ? "PASSED" : "FAILED"));

    exitStatus = (shared->passed) ? 0 : 1;
    PR_DELETE(shared);
    return exitStatus;
}

int main(int argc, char **argv)
{
    return (PR_VersionCheck(PR_VERSION)) ?
        PR_Initialize(Tmoacc, argc, argv, 4) : -1;
}  /* main */

/* tmoacc */
