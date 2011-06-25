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
 *  File Name:          try-007.js
 *  ECMA Section:
 *  Description:        The try statement
 *
 *  This test has a for-in statement within a try block.
 *
 *
 *  Author:             christine@netscape.com
 *  Date:               11 August 1998
 */
var SECTION = "try-007";
var VERSION = "ECMA_2";
var TITLE   = "The try statement:  for-in";

startTest();
writeHeaderToLog( SECTION + " "+ TITLE);

/**
 *  This is the "check" function for test objects that will
 *  throw an exception.
 */
function throwException() {
  throw EXCEPTION_STRING +": " + this.valueOf();
}
var EXCEPTION_STRING = "Exception thrown:";

/**
 *  This is the "check" function for test objects that do not
 *  throw an exception
 */
function noException() {
  return this.valueOf();
}

/**
 *  Add test cases here
 */
TryForIn( new TryObject( "hello", throwException, true ));
TryForIn( new TryObject( "hola",  noException, false ));

/**
 *  Run the test.
 */

test();

/**
 *  This is the object that will be the "this" in a with block.
 *  The check function is either throwException() or noException().
 *  See above.
 *
 */
function TryObject( value, fun, exception ) {
  this.value = value;
  this.exception = exception;

  this.check = fun;
  this.valueOf = function () { return this.value; }
}

/**
 *  This function has a for-in statement within a try block.  Test cases
 *  are added after the try-catch-finally statement.  Within the for-in
 *  block, call a function that can throw an exception.  Verify that any
 *  exceptions are properly caught.
 */

function TryForIn( object ) {
  try {
    for ( p in object ) {
      if ( typeof object[p] == "function" ) {
	result = object[p]();
      }
    }
  } catch ( e ) {
    result = e;
  }

  new TestCase(
    SECTION,
    "TryForIn( " + object+ " )",
    (object.exception ? EXCEPTION_STRING +": " + object.value : object.value),
    result );

}
