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
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   pschwartau@netscape.com
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

/*
 * Date: 14 April 2001
 *
 * SUMMARY: Testing obj.prop getter/setter
 * Note: this is a non-ECMA extension to the language.
 */
//-----------------------------------------------------------------------------
var gTestfile = 'getset-003.js';
var UBound = 0;
var BUGNUMBER = '(none)';
var summary = 'Testing obj.prop getter/setter';
var statprefix = 'Status: ';
var status = '';
var statusitems = [ ];
var actual = '';
var actualvalues = [ ];
var expect= '';
var expectedvalues = [ ];
var cnDEFAULT = 'default name';
var cnFRED = 'Fred';
var obj = {};
var obj2 = {};
var s = '';


// SECTION1: define getter/setter directly on an object (not its prototype)
obj = new Object();
obj.nameSETS = 0;
obj.nameGETS = 0;
Object.defineProperty(obj, "name",
{
  enumerable: true, configurable: true,
  set: function(newValue) {this._name=newValue; this.nameSETS++;},
  get: function() {this.nameGETS++; return this._name;}
});

    status = 'In SECTION1 of test after 0 sets, 0 gets';
actual = [obj.nameSETS,obj.nameGETS];
expect = [0,0];
addThis();

s = obj.name;
status = 'In SECTION1 of test after 0 sets, 1 get';
actual = [obj.nameSETS,obj.nameGETS];
expect = [0,1];
addThis();

obj.name = cnFRED;
status = 'In SECTION1 of test after 1 set, 1 get';
actual = [obj.nameSETS,obj.nameGETS];
expect = [1,1];
addThis();

obj.name = obj.name;
status = 'In SECTION1 of test after 2 sets, 2 gets';
actual = [obj.nameSETS,obj.nameGETS];
expect = [2,2];
addThis();


// SECTION2: define getter/setter in Object.prototype
Object.prototype.nameSETS = 0;
Object.prototype.nameGETS = 0;
Object.defineProperty(Object.prototype, "name",
{
  enumerable: true, configurable: true,
  set: function(newValue) {this._name=newValue; this.nameSETS++;},
  get: function() {this.nameGETS++; return this._name;}
});

    obj = new Object();
status = 'In SECTION2 of test after 0 sets, 0 gets';
actual = [obj.nameSETS,obj.nameGETS];
expect = [0,0];
addThis();

s = obj.name;
status = 'In SECTION2 of test after 0 sets, 1 get';
actual = [obj.nameSETS,obj.nameGETS];
expect = [0,1];
addThis();

obj.name = cnFRED;
status = 'In SECTION2 of test after 1 set, 1 get';
actual = [obj.nameSETS,obj.nameGETS];
expect = [1,1];
addThis();

obj.name = obj.name;
status = 'In SECTION2 of test after 2 sets, 2 gets';
actual = [obj.nameSETS,obj.nameGETS];
expect = [2,2];
addThis();


// SECTION 3: define getter/setter in prototype of user-defined constructor
function TestObject()
{
}
TestObject.prototype.nameSETS = 0;
TestObject.prototype.nameGETS = 0;
Object.defineProperty(TestObject.prototype, "name",
{
  enumerable: true, configurable: true,
  set: function(newValue) {this._name=newValue; this.nameSETS++;},
  get: function() {this.nameGETS++; return this._name;}
});
    TestObject.prototype.name = cnDEFAULT;

obj = new TestObject();
status = 'In SECTION3 of test after 1 set, 0 gets'; // (we set a default value in the prototype)
actual = [obj.nameSETS,obj.nameGETS];
expect = [1,0];
addThis();

s = obj.name;
status = 'In SECTION3 of test after 1 set, 1 get';
actual = [obj.nameSETS,obj.nameGETS];
expect = [1,1];
addThis();

obj.name = cnFRED;
status = 'In SECTION3 of test after 2 sets, 1 get';
actual = [obj.nameSETS,obj.nameGETS];
expect = [2,1];
addThis();

obj.name = obj.name;
status = 'In SECTION3 of test after 3 sets, 2 gets';
actual = [obj.nameSETS,obj.nameGETS];
expect = [3,2];
addThis();

obj2 = new TestObject();
status = 'obj2 = new TestObject() after 1 set, 0 gets';
actual = [obj2.nameSETS,obj2.nameGETS];
expect = [1,0]; // we set a default value in the prototype -
addThis();

// Use both obj and obj2  -
obj2.name = obj.name +  obj2.name;
status = 'obj2 = new TestObject() after 2 sets, 1 get';
actual = [obj2.nameSETS,obj2.nameGETS];
expect = [2,1];
addThis();

status = 'In SECTION3 of test after 3 sets, 3 gets';
actual = [obj.nameSETS,obj.nameGETS];
expect = [3,3];  // we left off at [3,2] above -
addThis();


//---------------------------------------------------------------------------------
test();
//---------------------------------------------------------------------------------


function addThis()
{
  statusitems[UBound] = status;
  actualvalues[UBound] = actual.toString();
  expectedvalues[UBound] = expect.toString();
  UBound++;
}


function test()
{
  enterFunc ('test');
  printBugNumber(BUGNUMBER);
  printStatus (summary);

  for (var i = 0; i < UBound; i++)
  {
    reportCompare(expectedvalues[i], actualvalues[i], getStatus(i));
  }

  exitFunc ('test');
}


function getStatus(i)
{
  return statprefix + statusitems[i];
}
