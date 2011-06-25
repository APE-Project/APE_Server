/*
 * Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/licenses/publicdomain/
 */
var args;

function upToTen()
{
  "use strict";
  eval("args = arguments;");
  for (var i = 0; i < RUNLOOP; i++)
    yield i;
}

var gen = upToTen();

var i = 0;
for (var v in gen)
{
  assertEq(v, i);
  i++;
}

assertEq(i, RUNLOOP);

assertEq(Object.prototype.toString.call(args), "[object Arguments]");
assertEq(args.length, 0);
