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
** File:	prftest1.c
** Description:
**     This is a simple test of the PR_snprintf() function defined
**     in prprf.c.
**
** Modification History:
** 14-May-97 AGarcia- Converted the test to accomodate the debug_mode flag.
**	         The debug mode will print all of the printfs associated with this test.
**			 The regress mode will be the default mode. Since the regress tool limits
**           the output to a one line status:PASS or FAIL,all of the printf statements
**			 have been handled with an if (debug_mode) statement. 
***********************************************************************/
/***********************************************************************
** Includes
***********************************************************************/
/* Used to get the command line option */
#include "plgetopt.h"
#include "prttools.h"

#include "prinit.h"
#include "prlong.h"
#include "prprf.h"

#include <string.h>

#define BUF_SIZE 128

/***********************************************************************
** PRIVATE FUNCTION:    Test_Result
** DESCRIPTION: Used in conjunction with the regress tool, prints out the
**				status of the test case.
** INPUTS:      PASS/FAIL
** OUTPUTS:     None
** RETURN:      None
** SIDE EFFECTS:
**      
** RESTRICTIONS:
**      None
** MEMORY:      NA
** ALGORITHM:   Determine what the status is and print accordingly.
**      
***********************************************************************/


static void Test_Result (int result)
{
	if (result == PASS)
		printf ("PASS\n");
	else
		printf ("FAIL\n");
}

int main(int argc, char **argv)
{
    PRInt16 i16;
    PRIntn n;
    PRInt32 i32;
    PRInt64 i64;
    char buf[BUF_SIZE];
    char answer[BUF_SIZE];
    int i;

	/* The command line argument: -d is used to determine if the test is being run
	in debug mode. The regress tool requires only one line output:PASS or FAIL.
	All of the printfs associated with this test has been handled with a if (debug_mode)
	test.
	Usage: test_name -d
	*/
	PLOptStatus os;
	PLOptState *opt = PL_CreateOptState(argc, argv, "d:");
	while (PL_OPT_EOL != (os = PL_GetNextOpt(opt)))
    {
		if (PL_OPT_BAD == os) continue;
        switch (opt->option)
        {
        case 'd':  /* debug mode */
			debug_mode = 1;
            break;
         default:
            break;
        }
    }
	PL_DestroyOptState(opt);

	/* main test */
    PR_STDIO_INIT();
	
    i16 = -1;
    n = -1;
    i32 = -1;
    LL_I2L(i64, i32);

    PR_snprintf(buf, BUF_SIZE, "%hx %x %lx %llx", i16, n, i32, i64);
    strcpy(answer, "ffff ");
    for (i = PR_BYTES_PER_INT * 2; i; i--) {
	strcat(answer, "f");
    }
    strcat(answer, " ffffffff ffffffffffffffff");

    if (!strcmp(buf, answer)) {
	if (debug_mode) printf("PR_snprintf test 1 passed\n");
	else Test_Result (PASS);
    } else {
		if (debug_mode) {
			printf("PR_snprintf test 1 failed\n");
			printf("Converted string is %s\n", buf);
			printf("Should be %s\n", answer);
		}
		else
			Test_Result (FAIL);
    }

    return 0;
}
