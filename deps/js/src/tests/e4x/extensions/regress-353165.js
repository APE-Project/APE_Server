/* -*- Mode: java; tab-width:8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

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
 * The Original Code is JavaScript Engine testing utilities.
 *
 * The Initial Developer of the Original Code is
 * Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2006
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s): Igor Bukanov
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


var BUGNUMBER = 353165;
var summary = 'Do not crash with xml_getMethod';
var actual = 'No Crash';
var expect = 'No Crash';

printBugNumber(BUGNUMBER);
START(summary);

crash1();
crash2();

END();

function crash1()
{
    try {
        var set = new XML('<a><b>text</b></a>').children('b');
        var counter = 0;
        Object.defineProperty(Object.prototype, "unrooter",
        { enumerable: true, configurable: true,
          get: function() {
            ++counter;
            if (counter == 5) {
                set[0] = new XML('<c/>');
                if (typeof gc == "function") {
                    gc();
                    var tmp = Math.sqrt(2), tmp2;
                    for (i = 0; i != 50000; ++i)
                        tmp2 = tmp / 2;
                }
            }
            return undefined;
          } });

        set.unrooter();
    }
    catch(ex) {
        print('1: ' + ex);
    }
    TEST(1, expect, actual);

}

function crash2() {
    try {
        var expected = "SOME RANDOM TEXT";

        var set = <a><b>{expected}</b></a>.children('b');
        var counter = 0;

        function unrooter_impl() {
                return String(this);
        }

        Object.defineProperty(Object.prototype, "unrooter",
        { enumerable: true, configurable: true,
          get: function() {
            ++counter;
            if (counter == 7)
            return unrooter_impl;
            if (counter == 5) {
                set[0] = new XML('<c/>');
                if (typeof gc == "function") {
                    gc();
                    var tmp = 1e500, tmp2;
                    for (i = 0; i != 50000; ++i)
                        tmp2 = tmp / 1.1;
                }
            }
            return undefined;
          } });

        set.unrooter();
    }
    catch(ex) {
        print('2: ' + ex);
    }
    TEST(2, expect, actual);
}
