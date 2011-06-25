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
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
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
   File Name:          11.6.1-2.js
   ECMA Section:       11.6.1 The addition operator ( + )
   Description:

   The addition operator either performs string concatenation or numeric
   addition.

   The production AdditiveExpression : AdditiveExpression + MultiplicativeExpression
   is evaluated as follows:

   1.  Evaluate AdditiveExpression.
   2.  Call GetValue(Result(1)).
   3.  Evaluate MultiplicativeExpression.
   4.  Call GetValue(Result(3)).
   5.  Call ToPrimitive(Result(2)).
   6.  Call ToPrimitive(Result(4)).
   7.  If Type(Result(5)) is String or Type(Result(6)) is String, go to step 12.
   (Note that this step differs from step 3 in the algorithm for comparison
   for the relational operators in using or instead of and.)
   8.  Call ToNumber(Result(5)).
   9.  Call ToNumber(Result(6)).
   10. Apply the addition operation to Result(8) and Result(9). See the discussion below (11.6.3).
   11. Return Result(10).
   12. Call ToString(Result(5)).
   13. Call ToString(Result(6)).
   14. Concatenate Result(12) followed by Result(13).
   15. Return Result(14).

   Note that no hint is provided in the calls to ToPrimitive in steps 5 and 6.
   All native ECMAScript objects except Date objects handle the absence of a
   hint as if the hint Number were given; Date objects handle the absence of a
   hint as if the hint String were given. Host objects may handle the absence
   of a hint in some other manner.

   This test does only covers cases where the Additive or Mulplicative expression
   ToPrimitive is a string.

   Author:             christine@netscape.com
   Date:               12 november 1997
*/
var SECTION = "11.6.1-2";
var VERSION = "ECMA_1";
startTest();

writeHeaderToLog( SECTION + " The Addition operator ( + )");

// tests for "MyValuelessObject", where the value is
// set in the object's prototype, not the object itself.

new TestCase(   SECTION,
                "var EXP_1 = new MyValuelessObject('string'); var EXP_2 = new MyValuelessObject(false); EXP_1 + EXP_2",
                "stringfalse",
                eval("var EXP_1 = new MyValuelessObject('string'); var EXP_2 = new MyValuelessObject(false); EXP_1 + EXP_2") );

new TestCase(   SECTION,
                "var EXP_1 = new MyValuelessObject(new String('string')); var EXP_2 = new MyValuelessObject(new Boolean(false)); EXP_1 + EXP_2",
                "stringfalse",
                eval("var EXP_1 = new MyValuelessObject(new String('string')); var EXP_2 = new MyValuelessObject(new Boolean(false)); EXP_1 + EXP_2") );

// tests for "MyValuelessObject", where the value is
// set in the object's prototype, not the object itself.

new TestCase(   SECTION,
                "var EXP_1 = new MyValuelessObject(100); var EXP_2 = new MyValuelessObject('string'); EXP_1 + EXP_2",
                "100string",
                eval("var EXP_1 = new MyValuelessObject(100); var EXP_2 = new MyValuelessObject('string'); EXP_1 + EXP_2") );

new TestCase(   SECTION,
                "var EXP_1 = new MyValuelessObject(new String('string')); var EXP_2 = new MyValuelessObject(new Number(-1)); EXP_1 + EXP_2",
                "string-1",
                eval("var EXP_1 = new MyValuelessObject(new String('string')); var EXP_2 = new MyValuelessObject(new Number(-1)); EXP_1 + EXP_2") );

test();

function MyProtoValuelessObject() {
  this.valueOf = new Function ( "" );
  this.__proto__ = null;
}
function MyProtolessObject( value ) {
  this.valueOf = new Function( "return this.value" );
  this.__proto__ = null;
  this.value = value;
}
function MyValuelessObject(value) {
  this.__proto__ = new MyPrototypeObject(value);
}
function MyPrototypeObject(value) {
  this.valueOf = new Function( "return this.value;" );
  this.toString = new Function( "return (this.value + '');" );
  this.value = value;
}
function MyObject( value ) {
  this.valueOf = new Function( "return this.value" );
  this.value = value;
}
