/*
 * Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/licenses/publicdomain/
 */

try {
    evalcx("Object.freeze(this); eval('const q;')");
} catch (e) {
    assertEq(e.message, "({lazy:false}) is not extensible");
}

reportCompare(0, 0, "don't crash");
