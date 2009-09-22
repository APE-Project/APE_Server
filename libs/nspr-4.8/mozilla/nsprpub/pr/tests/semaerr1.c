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
 * Portions created by the Initial Developer are Copyright (C) 1999-2000
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
#include "plgetopt.h"

#include <stdio.h>

#ifdef SYMBIAN
#define SEM_NAME1 "c:\\data\\foo.sem"
#define SEM_NAME2 "c:\\data\\bar.sem"
#else
#define SEM_NAME1 "/tmp/foo.sem"
#define SEM_NAME2 "/tmp/bar.sem"
#endif
#define SEM_MODE  0666

static PRBool debug_mode = PR_FALSE;

static void Help(void)
{
    fprintf(stderr, "semaerr1 test program usage:\n");
    fprintf(stderr, "\t-d           debug mode         (FALSE)\n");
    fprintf(stderr, "\t-h           this message\n");
}  /* Help */

int main(int argc, char **argv)
{
    PLOptStatus os;
    PLOptState *opt = PL_CreateOptState(argc, argv, "dh");
    PRSem *sem;

    while (PL_OPT_EOL != (os = PL_GetNextOpt(opt))) {
        if (PL_OPT_BAD == os) continue;
        switch (opt->option) {
            case 'd':  /* debug mode */
                debug_mode = PR_TRUE;
                break;
            case 'h':
            default:
                Help();
                return 2;
        }
    }
    PL_DestroyOptState(opt);

    /*
     * PR_SEM_CREATE|PR_SEM_EXCL should be able to
     * create a nonexistent semaphore.
     */
    (void) PR_DeleteSemaphore(SEM_NAME2);
    sem = PR_OpenSemaphore(SEM_NAME2, PR_SEM_CREATE|PR_SEM_EXCL, SEM_MODE, 0);
    if (sem == NULL) {
        fprintf(stderr, "PR_OpenSemaphore failed\n");
        exit(1);
    }
    if (PR_CloseSemaphore(sem) == PR_FAILURE) {
        fprintf(stderr, "PR_CloseSemaphore failed\n");
        exit(1);
    }
    if (PR_DeleteSemaphore(SEM_NAME2) == PR_FAILURE) {
        fprintf(stderr, "PR_DeleteSemaphore failed\n");
        exit(1);
    }
    
    /*
     * Opening an existing semaphore with PR_SEM_CREATE|PR_SEM_EXCL.
     * should fail with PR_FILE_EXISTS_ERROR.
     */
    sem = PR_OpenSemaphore(SEM_NAME1, PR_SEM_CREATE|PR_SEM_EXCL, SEM_MODE, 0);
    if (sem != NULL) {
        fprintf(stderr, "PR_OpenSemaphore should fail but succeeded\n");
        exit(1);
    }
    if (PR_GetError() != PR_FILE_EXISTS_ERROR) {
        fprintf(stderr, "Expect %d but got %d\n", PR_FILE_EXISTS_ERROR,
                PR_GetError());
        exit(1);
    }

    /*
     * Try again, with just PR_SEM_CREATE.  This should succeed.
     */
    sem = PR_OpenSemaphore(SEM_NAME1, PR_SEM_CREATE, SEM_MODE, 0);
    if (sem == NULL) {
        fprintf(stderr, "PR_OpenSemaphore failed (%d, %d)\n",
                PR_GetError(), PR_GetOSError());
        exit(1);
    }
    if (PR_CloseSemaphore(sem) == PR_FAILURE) {
        fprintf(stderr, "PR_CloseSemaphore failed\n");
        exit(1);
    }

    sem = PR_OpenSemaphore(SEM_NAME2, PR_SEM_CREATE|PR_SEM_EXCL, SEM_MODE, 0);
    if (sem == NULL) {
        fprintf(stderr, "PR_OpenSemaphore failed (%d, %d)\n",
                PR_GetError(), PR_GetOSError());
        exit(1);
    }
    if (PR_CloseSemaphore(sem) == PR_FAILURE) {
        fprintf(stderr, "PR_CloseSemaphore failed\n");
        exit(1);
    }

    printf("PASS\n");
    return 0;
}
