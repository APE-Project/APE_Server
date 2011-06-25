/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
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
 * Contributor(s): nanto vi (TOYAMA Nao)
 *                 Blake Kaplan
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

//-----------------------------------------------------------------------------
var BUGNUMBER = 343765;
var summary = 'Function defined in a let statement/expression does not work correctly outside the let scope';
var actual = '';
var expect = '';


//-----------------------------------------------------------------------------
test();
//-----------------------------------------------------------------------------

function test()
{
  enterFunc ('test');
  printBugNumber(BUGNUMBER);
  printStatus (summary);
 
  print("SECTION 1");
  try {
    let (a = 2, b = 3) { function f() { return String([a,b]); } f(); throw 42; }
  } catch (e) {
    f();
  }

  reportCompare(expect, actual, summary);

  print("SECTION 2");
  try {
    with ({a:2,b:3}) { function f() { return String([a,b]); } f(); throw 42; }
  } catch (e) {
    f();
  }

  print("SECTION 3");
  function g3() {
    print("Here!");
    with ({a:2,b:3}) {
      function f() {
        return String([a,b]);
      }

      f();
      return f;
    }
  }

  k = g3();
  k();

  print("SECTION 4");
  function g4() {
    print("Here!");
    let (a=2,b=3) {
      function f() {
        return String([a,b]);
      }

      f();
      return f;
    }
  }

  k = g4();
  k();

  print("SECTION 5");
  function g5() {
    print("Here!");
    let (a=2,b=3) {
      function f() {
        return String([a,b]);
      }

      f();
      yield f;
    }
  }

  k = g5().next();
  k();

  reportCompare(expect, actual, summary);

  exitFunc ('test');
}

function returnResult(a)
{
  return a + '';
}
