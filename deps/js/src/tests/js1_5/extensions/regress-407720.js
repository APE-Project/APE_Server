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
 * Portions created by the Initial Developer are Copyright (C) 2008
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s): moz_bug_r_a4
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
var BUGNUMBER = 407720;
var summary = 'js_FindClassObject causes crashes with getter/setter - Browser only';
var actual = 'No Crash';
var expect = 'No Crash';

printBugNumber(BUGNUMBER);
printStatus (summary);

// stop the test after 60 seconds
var start = new Date();

if (typeof document != 'undefined')
{ 
  // delay test driver end
  gDelayTestDriverEnd = true;
  document.write('<iframe onload="onLoad()"><\/iframe>');
}
else
{
  actual = 'No Crash';
  reportCompare(expect, actual, summary);
}

function onLoad() 
{

  if ( (new Date() - start) < 60*1000)
  {
    var x = frames[0].Window.prototype;
    x.a = x.b = x.c = 1;
    x.__defineGetter__("HTML document.all class", function() {});
    frames[0].document.all;

    // retry
    frames[0].location = "about:blank";
  }
  else
  {
    actual = 'No Crash';

    reportCompare(expect, actual, summary);
    gDelayTestDriverEnd = false;
    jsTestDriverEnd();
  }
}
