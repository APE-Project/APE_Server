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
 * Netscape Communication Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
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


/**
   File Name:          lexical-001.js
   CorrespondsTo:      ecma/LexicalConventions/7.2.js
   ECMA Section:       7.2 Line Terminators
   Description:        - readability
   - separate tokens
   - may occur between any two tokens
   - cannot occur within any token, not even a string
   - affect the process of automatic semicolon insertion.

   white space characters are:
   unicode     name            formal name     string representation
   \u000A      line feed       <LF>            \n
   \u000D      carriage return <CR>            \r

   this test uses onerror to capture line numbers.  because
   we use on error, we can only have one test case per file.

   Author:             christine@netscape.com
   Date:               11 september 1997
*/
var SECTION = "lexical-001";
var VERSION = "JS1_4";
var TITLE   = "Line Terminators";

startTest();
writeHeaderToLog( SECTION + " "+ TITLE);

var result = "Failed";
var exception = "No exception thrown";
var expect = "Passed";

try {
  result = eval("\r\n\expect");
} catch ( e ) {
  exception = e.toString();
}

new TestCase(
  SECTION,
  "OBJECT = new Object; result = new OBJECT()" +
  " (threw " + exception +")",
  expect,
  result );

test();
