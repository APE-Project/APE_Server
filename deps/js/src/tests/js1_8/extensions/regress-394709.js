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
 * Portions created by the Initial Developer are Copyright (C) 2007
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

//-----------------------------------------------------------------------------
var BUGNUMBER = 394709;
var summary = 'Do not leak with object.watch and closure';
var actual = 'No Leak';
var expect = 'No Leak';

if (typeof countHeap == 'undefined')
{
  countHeap = function () { 
    print('This test requires countHeap which is not supported'); 
    return 0;
  };
}

//-----------------------------------------------------------------------------
test();
//-----------------------------------------------------------------------------

function test()
{
  enterFunc ('test');
  printBugNumber(BUGNUMBER);
  printStatus (summary);

  // Ensure that we flush all values so that gc() collects all objects that
  // the user cannot reach from JS.
  eval();

  runtest();
  gc();
  var count1 = countHeap();
  runtest();
  gc();
  var count2 = countHeap();
  runtest();
  gc();
  var count3 = countHeap();
  /* Try to be tolerant of conservative GC noise: we want a steady leak. */
  if (count1 < count2 && count2 < count3)
    throw "A leaky watch point is detected";

  function runtest () {
    var obj = { b: 0 };
    obj.watch('b', watcher);

    function watcher(id, old, value) {
      ++obj.n;
      return value;
    }
  }

  reportCompare(expect, actual, summary);

  exitFunc ('test');
}
