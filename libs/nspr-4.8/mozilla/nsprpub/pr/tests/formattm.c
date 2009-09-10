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
 * Portions created by the Initial Developer are Copyright (C) 2003
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

/* A test program for PR_FormatTime and PR_FormatTimeUSEnglish */

#include "prtime.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    char buffer[256];
    char small_buffer[8];
    PRTime now;
    PRExplodedTime tod;

    now = PR_Now();
    PR_ExplodeTime(now, PR_LocalTimeParameters, &tod);

    if (PR_FormatTime(buffer, sizeof(buffer),
            "%a %b %d %H:%M:%S %Z %Y", &tod) != 0) {
        printf("%s\n", buffer);
    } else {
        fprintf(stderr, "PR_FormatTime(buffer) failed\n");
        return 1;
    }

    small_buffer[0] = '?';
    if (PR_FormatTime(small_buffer, sizeof(small_buffer),
            "%a %b %d %H:%M:%S %Z %Y", &tod) == 0) {
        if (small_buffer[0] != '\0') {
            fprintf(stderr, "PR_FormatTime(small_buffer) did not output "
                            "an empty string on failure\n");
            return 1;
        }
        printf("%s\n", small_buffer);
    } else {
        fprintf(stderr, "PR_FormatTime(small_buffer) succeeded "
                        "unexpectedly\n");
        return 1;
    }

    (void)PR_FormatTimeUSEnglish(buffer, sizeof(buffer),
        "%a %b %d %H:%M:%S %Z %Y", &tod);
    printf("%s\n", buffer);

    return 0;
}
