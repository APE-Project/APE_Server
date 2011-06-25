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
 * Portions created by the Initial Developer are Copyright (C) 2005
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s): Phil Schwartau <pschwartau@meer.net>
 *                 Bob Clary <bob@bclary.com>
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
var BUGNUMBER = 157334;
var summary = 'String concat should not be O(N**2)';
var actual = '';
var expect = '';

printBugNumber(BUGNUMBER);
printStatus (summary);

var data = {X:[], Y:[]};
for (var size = 1000; size < 10000; size += 1000)
{ 
  data.X.push(size);
  data.Y.push(concat(size));
  gc();
}

var order = BigO(data);

var msg = '';
for (var p = 0; p < data.X.length; p++)
{
  msg += '(' + data.X[p] + ', ' + data.Y[p] + '); ';
}
printStatus(msg);
printStatus('Order: ' + order);
reportCompare(true, order < 2, 'BigO ' + order + ' < 2');

function concat(size)
{
  var c='qwertyuiop';
  var x='';
  var y='';
  var loop;

  for (loop = 0; loop < size; loop++)
  {
    x += c;
  }

  y = x + 'y';
  x = x + 'x';

  var start = new Date();
  for (loop = 0; loop < 1000; loop++)
  {
    var z = x + y + loop.toString();
  }
  var stop = new Date();
  return stop - start;
}
