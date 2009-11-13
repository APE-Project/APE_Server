/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * ***** BEGIN LICENSE BLOCK *****
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
 * The Original Code is JSAPI tests.
 *
 * The Initial Developer of the Original Code is
 * Mozilla Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2009
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *     Jason Orendorff
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
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

#include "tests.h"
#include <iostream>

using namespace std;

JSAPITest *JSAPITest::list;

int main()
{
    int failures = 0;

    for (JSAPITest *test = JSAPITest::list; test; test = test->next) {
        string name = test->name();

        cout << name << endl;
        if (!test->init()) {
            cout << "TEST-UNEXPECTED-FAIL | " << name << " | Failed to initialize." << endl;
            failures++;
            continue;
        }

        if (test->run()) {
            cout << "TEST-PASS | " << name << " | ok" << endl;
        } else {
            cout << (test->knownFail ? "TEST-KNOWN-FAIL" : "TEST-UNEXPECTED-FAIL")
                 << " | " << name << " | " << test->messages() << endl;
            if (!test->knownFail)
                failures++;
        }
        test->uninit();
    }

    if (failures) {
        cout << "\n" << failures << " unexpected failure" << (failures == 1 ? "." : "s.") << endl;
        return 1;
    }
    cout << "\nPassed." << endl;
    return 0;
}
