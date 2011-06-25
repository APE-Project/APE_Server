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
var BUGNUMBER = 324278;
var summary = 'GC without recursion';
var actual = 'No Crash';
var expect = 'No Crash';

printBugNumber(BUGNUMBER);
printStatus (summary);

// Number to push native stack size beyond 10MB if GC recurses generating
// segfault on Fedora Core / Ubuntu Linuxes where the stack size by default
// is 10MB/8MB.
var N = 100*1000;

function build(N) {
  // Exploit the fact that (in ES3), regexp literals are shared between
  // function invocations. Thus we build the following chain:
  // chainTop: function->regexp->function->regexp....->null
  // to check how GC would deal with this chain.

  var chainTop = null;
  for (var i = 0; i != N; ++i) {
    var f = Function('some_arg'+i, ' return /test/;');
    var re = f();
    re.previous = chainTop;
    chainTop = f;
  }
  return chainTop;
}

function check(chainTop, N) {
  for (var i = 0; i != N; ++i) {
    var re = chainTop();
    chainTop = re.previous;
  }
  if (chainTop !== null)
    throw "Bad chainTop";

}

if (typeof gc != "function") {
  gc = function() {
    for (var i = 0; i != 50*1000; ++i) {
      var tmp = new Object();
    }
  }
}

var chainTop = build(N);
printStatus("BUILT");
gc();
check(chainTop, N);
printStatus("CHECKED");
chainTop = null;
gc();
 
reportCompare(expect, actual, summary);
