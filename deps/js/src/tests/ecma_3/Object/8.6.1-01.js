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
 * Contributor(s): Michael Daumling <daumling@adobe.com>
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

var BUGNUMBER = 315436;
var summary = 'In strict mode, setting a read-only property should generate a warning';

printBugNumber(BUGNUMBER);
printStatus (summary);

enterFunc (String (BUGNUMBER));

// should throw an error in strict mode
var actual = '';
var expect = 's.length is read-only';
var status = summary + ': Throw if STRICT and WERROR is enabled';

if (!options().match(/strict/))
{
  options('strict');
}
if (!options().match(/werror/))
{
  options('werror');
}

try
{
  var s = new String ('abc');
  s.length = 0;
}
catch (e)
{
  actual = e.message;
}

reportCompare(expect, actual, status);

// should not throw an error if in strict mode and WERROR is false

actual = 'did not throw';
expect = 'did not throw';
var status = summary + ': Do not throw if STRICT is enabled and WERROR is disabled';

// toggle werror off
options('werror');

try
{
  s.length = 0;
}
catch (e)
{
  actual = e.message;
}

reportCompare(expect, actual, status);

// should not throw an error if not in strict mode

actual = 'did not throw';
expect = 'did not throw';
var status = summary + ': Do not throw if not in strict mode';

// toggle strict off
options('strict');

try
{
  s.length = 0;
}
catch (e)
{
  actual = e.message;
}

reportCompare(expect, actual, status);
