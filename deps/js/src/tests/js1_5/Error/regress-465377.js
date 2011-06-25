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
var BUGNUMBER = 465377;
var summary = 'instanceof relations between Error objects';
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

  expect = actual = 'No Exception';

  try
  {
    var list = [
      "Error",
      "InternalError",
      "EvalError",
      "RangeError",
      "ReferenceError",
      "SyntaxError",
      "TypeError",
      "URIError"
      ];
    var instances = [];

    for (var i = 0; i != list.length; ++i) {
      var name = list[i];
      var constructor = this[name];
      var tmp = constructor.name;
      if (tmp !== name)
        throw "Bad value for "+name+".name: "+uneval(tmp);
      instances[i] = new constructor();
    }

    for (var i = 0; i != instances.length; ++i) {
      var instance = instances[i];
      var name = instance.name;
      var constructor = instance.constructor;
      if (constructor !== this[name])
        throw "Bad value of (new "+name+").constructor: "+uneval(tmp);
      var tmp = constructor.name;
      if (tmp !== name)
        throw "Bad value for constructor.name: "+uneval(tmp);
      if (!(instance instanceof Object))
        throw "Bad instanceof Object for "+name;
      if (!(instance instanceof Error))
        throw "Bad instanceof Error for "+name;
      if (!(instance instanceof constructor))
        throw "Bad instanceof constructor for "+name;
      if (instance instanceof Function)
        throw "Bad instanceof Function for "+name;
      for (var j = 1; j != instances.length; ++j) {
        if (i != j && instance instanceof instances[j].constructor) {
          throw "Unexpected (new "+name+") instanceof "+ instances[j].name;
        }
      }
    }

    print("OK");
  }
  catch(ex)
  {
    actual = ex + '';
  }
  reportCompare(expect, actual, summary);

  exitFunc ('test');
}
