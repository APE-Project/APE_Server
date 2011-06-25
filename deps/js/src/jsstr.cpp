/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
 *
 * ***** BEGIN LICENSE BLOCK *****
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
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
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
 * JS string type implementation.
 *
 * In order to avoid unnecessary js_LockGCThing/js_UnlockGCThing calls, these
 * native methods store strings (possibly newborn) converted from their 'this'
 * parameter and arguments on the stack: 'this' conversions at argv[-1], arg
 * conversions at their index (argv[0], argv[1]).  This is a legitimate method
 * of rooting things that might lose their newborn root due to subsequent GC
 * allocations in the same native method.
 */
#include <stdlib.h>
#include <string.h>
#include "jstypes.h"
#include "jsstdint.h"
#include "jsutil.h"
#include "jshash.h"
#include "jsprf.h"
#include "jsapi.h"
#include "jsarray.h"
#include "jsatom.h"
#include "jsbool.h"
#include "jsbuiltins.h"
#include "jscntxt.h"
#include "jsfun.h"      /* for JS_ARGS_LENGTH_MAX */
#include "jsgc.h"
#include "jsinterp.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jsregexp.h"
#include "jsscope.h"
#include "jsstaticcheck.h"
#include "jsstr.h"
#include "jsbit.h"
#include "jsvector.h"
#include "jsversion.h"

#include "jscntxtinlines.h"
#include "jsinterpinlines.h"
#include "jsobjinlines.h"
#include "jsregexpinlines.h"
#include "jsstrinlines.h"
#include "jsautooplen.h"        // generated headers last

using namespace js;
using namespace js::gc;

JS_STATIC_ASSERT(size_t(JSString::MAX_LENGTH) <= size_t(JSVAL_INT_MAX));
JS_STATIC_ASSERT(JSString::MAX_LENGTH <= JSVAL_INT_MAX);

const jschar *
js_GetStringChars(JSContext *cx, JSString *str)
{
    if (!js_MakeStringImmutable(cx, str))
        return NULL;
    return str->flatChars();
}

static JS_ALWAYS_INLINE size_t
RopeCapacityFor(size_t length)
{
    static const size_t ROPE_DOUBLING_MAX = 1024 * 1024;

    /*
     * Grow by 12.5% if the buffer is very large. Otherwise, round up to the
     * next power of 2. This is similar to what we do with arrays; see
     * JSObject::ensureDenseArrayElements.
     */
    if (length > ROPE_DOUBLING_MAX)
        return length + (length / 8);
    return RoundUpPow2(length);
}

static JS_ALWAYS_INLINE jschar *
AllocChars(JSContext *maybecx, size_t wholeCapacity)
{
    /* +1 for the null char at the end. */
    JS_STATIC_ASSERT(JSString::MAX_LENGTH * sizeof(jschar) < UINT32_MAX);
    size_t bytes = (wholeCapacity + 1) * sizeof(jschar);
    if (maybecx)
        return (jschar *)maybecx->malloc(bytes);
    return (jschar *)js_malloc(bytes);
}

const jschar *
JSString::flatten(JSContext *maybecx)
{
    JS_ASSERT(isRope());

    /*
     * Perform a depth-first dag traversal, splatting each node's characters
     * into a contiguous buffer. Visit each rope node three times:
     *  1. record position in the buffer and recurse into left child;
     *  2. recurse into the right child;
     *  3. transform the node into a dependent string.
     * To avoid maintaining a stack, tree nodes are mutated to indicate how
     * many times they have been visited. Since ropes can be dags, a node may
     * be encountered multiple times during traversal. However, step 3 above
     * leaves a valid dependent string, so everythings works out.  This
     * algorithm is homomorphic to TypedMarker(JSTracer *, JSString *).
     *
     * While ropes avoid all sorts of quadratic cases with string
     * concatenation, they can't help when ropes are immediately flattened.
     * One idiomatic case that we'd like to keep linear (and has traditionally
     * been linear in SM and other JS engines) is:
     *
     *   while (...) {
     *     s += ...
     *     s.flatten
     *   }
     *
     * To do this, when the buffer for a to-be-flattened rope is allocated, the
     * allocation size is rounded up. Then, if the resulting flat string is the
     * left-hand side of a new rope that gets flattened and there is enough
     * capacity, the rope is flattened into the same buffer, thereby avoiding
     * copying the left-hand side. Clearing the 'extensible' bit turns off this
     * optimization. This is necessary, e.g., when the JSAPI hands out the raw
     * null-terminated char array of a flat string.
     *
     * N.B. This optimization can create chains of dependent strings.
     */
    const size_t wholeLength = length();
    size_t wholeCapacity;
    jschar *wholeChars;
    JSString *str = this;
    jschar *pos;

    if (u.left->isExtensible() && u.left->s.capacity >= wholeLength) {
        wholeCapacity = u.left->s.capacity;
        wholeChars = const_cast<jschar *>(u.left->u.chars);
        pos = wholeChars + u.left->length();
        u.left->finishTraversalConversion(this, wholeChars, pos);
        goto visit_right_child;
    }

    wholeCapacity = RopeCapacityFor(wholeLength);
    wholeChars = AllocChars(maybecx, wholeCapacity);
    if (!wholeChars)
        return NULL;

    if (maybecx)
        maybecx->runtime->stringMemoryUsed += wholeLength * 2;
    pos = wholeChars;
    first_visit_node: {
        JSString *left = str->u.left;           /* Read before clobbered. */
        str->u.chars = pos;
        if (left->isRope()) {
            left->s.parent = str;               /* Return to this when 'left' done, */
            left->lengthAndFlags = 0x200;       /* but goto visit_right_child. */
            str = left;
            goto first_visit_node;
        }
        size_t len = left->length();
        PodCopy(pos, left->u.chars, len);
        pos += len;
    }
    visit_right_child: {
        JSString *right = str->s.right;
        if (right->isRope()) {
            right->s.parent = str;              /* Return to this node when 'right' done, */
            right->lengthAndFlags = 0x300;      /* but goto finish_node. */
            str = right;
            goto first_visit_node;
        }
        size_t len = right->length();
        PodCopy(pos, right->u.chars, len);
        pos += len;
    }
    finish_node: {
        if (str == this) {
            JS_ASSERT(pos == wholeChars + wholeLength);
            *pos = '\0';
            initFlatExtensible(wholeChars, wholeLength, wholeCapacity);
            return wholeChars;
        }
        size_t progress = str->lengthAndFlags;  /* Read before clobbered. */
        JSString *parent = str->s.parent;
        str->finishTraversalConversion(this, wholeChars, pos);
        str = parent;
        if (progress == 0x200)
            goto visit_right_child;
        goto finish_node;
    }
}

JS_STATIC_ASSERT(JSExternalString::TYPE_LIMIT == 8);
JSStringFinalizeOp JSExternalString::str_finalizers[JSExternalString::TYPE_LIMIT] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

#ifdef JS_TRACER

JSBool JS_FASTCALL
js_Flatten(JSContext *cx, JSString* str)
{
    return !!str->flatten(cx);
}
JS_DEFINE_CALLINFO_2(extern, BOOL, js_Flatten, CONTEXT, STRING, 0, nanojit::ACCSET_STORE_ANY)

#endif /* !JS_TRACER */

JSString * JS_FASTCALL
js_ConcatStrings(JSContext *cx, JSString *left, JSString *right)
{
    JS_ASSERT_IF(!JSString::isStatic(left) && !left->isAtomized(),
                 left->asCell()->compartment() == cx->compartment);
    JS_ASSERT_IF(!JSString::isStatic(right) && !right->isAtomized(),
                 right->asCell()->compartment() == cx->compartment);

    size_t leftLen = left->length();
    if (leftLen == 0)
        return right;

    size_t rightLen = right->length();
    if (rightLen == 0)
        return left;

    size_t wholeLength = leftLen + rightLen;

    if (JSShortString::fitsIntoShortString(wholeLength)) {
        JSShortString *shortStr = js_NewGCShortString(cx);
        if (!shortStr)
            return NULL;
        const jschar *leftChars = left->getChars(cx);
        if (!leftChars)
            return NULL;
        const jschar *rightChars = right->getChars(cx);
        if (!rightChars)
            return NULL;

        jschar *buf = shortStr->init(wholeLength);
        js_short_strncpy(buf, leftChars, leftLen);
        js_short_strncpy(buf + leftLen, rightChars, rightLen);
        buf[wholeLength] = 0;
        return shortStr->header();
    }

    if (wholeLength > JSString::MAX_LENGTH) {
        if (JS_ON_TRACE(cx)) {
            if (!CanLeaveTrace(cx))
                return NULL;
            LeaveTrace(cx);
        }
        js_ReportAllocationOverflow(cx);
        return NULL;
    }

    JSString *newRoot = js_NewGCString(cx);
    if (!newRoot)
        return NULL;

    newRoot->initRopeNode(left, right, wholeLength);
    return newRoot;
}

const jschar *
JSString::undepend(JSContext *cx)
{
    size_t n, size;
    jschar *s;

    if (!ensureLinear(cx))
        return NULL;

    if (isDependent()) {
        n = dependentLength();
        size = (n + 1) * sizeof(jschar);
        s = (jschar *) cx->malloc(size);
        if (!s)
            return NULL;

        cx->runtime->stringMemoryUsed += size;
        js_strncpy(s, dependentChars(), n);
        s[n] = 0;
        initFlat(s, n);

#ifdef DEBUG
        {
            JSRuntime *rt = cx->runtime;
            JS_RUNTIME_UNMETER(rt, liveDependentStrings);
            JS_RUNTIME_UNMETER(rt, totalDependentStrings);
            JS_LOCK_RUNTIME_VOID(rt,
                (rt->strdepLengthSum -= (double)n,
                 rt->strdepLengthSquaredSum -= (double)n * (double)n));
        }
#endif
    }

    return flatChars();
}

JSBool
js_MakeStringImmutable(JSContext *cx, JSString *str)
{
    /*
     * Flattening a rope may result in a dependent string, so we need to flatten
     * before undepending the string.
     */
    if (!str->isFlat() && !str->undepend(cx)) {
        JS_RUNTIME_METER(cx->runtime, badUndependStrings);
        return JS_FALSE;
    }
    str->flatClearExtensible();
    return JS_TRUE;
}

static JSLinearString *
ArgToRootedString(JSContext *cx, uintN argc, Value *vp, uintN arg)
{
    if (arg >= argc)
        return ATOM_TO_STRING(cx->runtime->atomState.typeAtoms[JSTYPE_VOID]);
    vp += 2 + arg;

    if (vp->isObject() && !DefaultValue(cx, &vp->toObject(), JSTYPE_STRING, vp))
        return NULL;

    JSLinearString *str;
    if (vp->isString()) {
        str = vp->toString()->ensureLinear(cx);
    } else if (vp->isBoolean()) {
        str = ATOM_TO_STRING(cx->runtime->atomState.booleanAtoms[
                                  (int)vp->toBoolean()]);
    } else if (vp->isNull()) {
        str = ATOM_TO_STRING(cx->runtime->atomState.nullAtom);
    } else if (vp->isUndefined()) {
        str = ATOM_TO_STRING(cx->runtime->atomState.typeAtoms[JSTYPE_VOID]);
    }
    else {
        str = NumberToString(cx, vp->toNumber());
        if (!str)
            return NULL;
        vp->setString(str);
    }
    return str;
}

/*
 * Forward declarations for URI encode/decode and helper routines
 */
static JSBool
str_decodeURI(JSContext *cx, uintN argc, Value *vp);

static JSBool
str_decodeURI_Component(JSContext *cx, uintN argc, Value *vp);

static JSBool
str_encodeURI(JSContext *cx, uintN argc, Value *vp);

static JSBool
str_encodeURI_Component(JSContext *cx, uintN argc, Value *vp);

static const uint32 OVERLONG_UTF8 = UINT32_MAX;

static uint32
Utf8ToOneUcs4Char(const uint8 *utf8Buffer, int utf8Length);

/*
 * Contributions from the String class to the set of methods defined for the
 * global object.  escape and unescape used to be defined in the Mocha library,
 * but as ECMA decided to spec them, they've been moved to the core engine
 * and made ECMA-compliant.  (Incomplete escapes are interpreted as literal
 * characters by unescape.)
 */

/*
 * Stuff to emulate the old libmocha escape, which took a second argument
 * giving the type of escape to perform.  Retained for compatibility, and
 * copied here to avoid reliance on net.h, mkparse.c/NET_EscapeBytes.
 */

#define URL_XALPHAS     ((uint8) 1)
#define URL_XPALPHAS    ((uint8) 2)
#define URL_PATH        ((uint8) 4)

static const uint8 urlCharType[256] =
/*      Bit 0           xalpha          -- the alphas
 *      Bit 1           xpalpha         -- as xalpha but
 *                             converts spaces to plus and plus to %20
 *      Bit 2 ...       path            -- as xalphas but doesn't escape '/'
 */
    /*   0 1 2 3 4 5 6 7 8 9 A B C D E F */
    {    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,       /* 0x */
         0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,       /* 1x */
         0,0,0,0,0,0,0,0,0,0,7,4,0,7,7,4,       /* 2x   !"#$%&'()*+,-./  */
         7,7,7,7,7,7,7,7,7,7,0,0,0,0,0,0,       /* 3x  0123456789:;<=>?  */
         7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,       /* 4x  @ABCDEFGHIJKLMNO  */
         7,7,7,7,7,7,7,7,7,7,7,0,0,0,0,7,       /* 5X  PQRSTUVWXYZ[\]^_  */
         0,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,       /* 6x  `abcdefghijklmno  */
         7,7,7,7,7,7,7,7,7,7,7,0,0,0,0,0,       /* 7X  pqrstuvwxyz{\}~  DEL */
         0, };

/* This matches the ECMA escape set when mask is 7 (default.) */

#define IS_OK(C, mask) (urlCharType[((uint8) (C))] & (mask))

/* See ECMA-262 Edition 3 B.2.1 */
JSBool
js_str_escape(JSContext *cx, uintN argc, Value *vp, Value *rval)
{
    const char digits[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                           '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };

    jsint mask = URL_XALPHAS | URL_XPALPHAS | URL_PATH;
    if (argc > 1) {
        double d;
        if (!ValueToNumber(cx, vp[3], &d))
            return JS_FALSE;
        if (!JSDOUBLE_IS_FINITE(d) ||
            (mask = (jsint)d) != d ||
            mask & ~(URL_XALPHAS | URL_XPALPHAS | URL_PATH))
        {
            char numBuf[12];
            JS_snprintf(numBuf, sizeof numBuf, "%lx", (unsigned long) mask);
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_BAD_STRING_MASK, numBuf);
            return JS_FALSE;
        }
    }

    JSLinearString *str = ArgToRootedString(cx, argc, vp, 0);
    if (!str)
        return JS_FALSE;

    size_t length = str->length();
    const jschar *chars = str->chars();

    /* Take a first pass and see how big the result string will need to be. */
    size_t newlength = length;
    for (size_t i = 0; i < length; i++) {
        jschar ch;
        if ((ch = chars[i]) < 128 && IS_OK(ch, mask))
            continue;
        if (ch < 256) {
            if (mask == URL_XPALPHAS && ch == ' ')
                continue;   /* The character will be encoded as '+' */
            newlength += 2; /* The character will be encoded as %XX */
        } else {
            newlength += 5; /* The character will be encoded as %uXXXX */
        }

        /*
         * This overflow test works because newlength is incremented by at
         * most 5 on each iteration.
         */
        if (newlength < length) {
            js_ReportAllocationOverflow(cx);
            return JS_FALSE;
        }
    }

    if (newlength >= ~(size_t)0 / sizeof(jschar)) {
        js_ReportAllocationOverflow(cx);
        return JS_FALSE;
    }

    jschar *newchars = (jschar *) cx->malloc((newlength + 1) * sizeof(jschar));
    if (!newchars)
        return JS_FALSE;
    size_t i, ni;
    for (i = 0, ni = 0; i < length; i++) {
        jschar ch;
        if ((ch = chars[i]) < 128 && IS_OK(ch, mask)) {
            newchars[ni++] = ch;
        } else if (ch < 256) {
            if (mask == URL_XPALPHAS && ch == ' ') {
                newchars[ni++] = '+'; /* convert spaces to pluses */
            } else {
                newchars[ni++] = '%';
                newchars[ni++] = digits[ch >> 4];
                newchars[ni++] = digits[ch & 0xF];
            }
        } else {
            newchars[ni++] = '%';
            newchars[ni++] = 'u';
            newchars[ni++] = digits[ch >> 12];
            newchars[ni++] = digits[(ch & 0xF00) >> 8];
            newchars[ni++] = digits[(ch & 0xF0) >> 4];
            newchars[ni++] = digits[ch & 0xF];
        }
    }
    JS_ASSERT(ni == newlength);
    newchars[newlength] = 0;

    JSString *retstr = js_NewString(cx, newchars, newlength);
    if (!retstr) {
        cx->free(newchars);
        return JS_FALSE;
    }
    rval->setString(retstr);
    return JS_TRUE;
}
#undef IS_OK

static JSBool
str_escape(JSContext *cx, uintN argc, Value *vp)
{
    return js_str_escape(cx, argc, vp, vp);
}

/* See ECMA-262 Edition 3 B.2.2 */
static JSBool
str_unescape(JSContext *cx, uintN argc, Value *vp)
{
    JSLinearString *str = ArgToRootedString(cx, argc, vp, 0);
    if (!str)
        return false;

    size_t length = str->length();
    const jschar *chars = str->chars();

    /* Don't bother allocating less space for the new string. */
    jschar *newchars = (jschar *) cx->malloc((length + 1) * sizeof(jschar));
    if (!newchars)
        return false;
    size_t ni = 0, i = 0;
    while (i < length) {
        jschar ch = chars[i++];
        if (ch == '%') {
            if (i + 1 < length &&
                JS7_ISHEX(chars[i]) && JS7_ISHEX(chars[i + 1]))
            {
                ch = JS7_UNHEX(chars[i]) * 16 + JS7_UNHEX(chars[i + 1]);
                i += 2;
            } else if (i + 4 < length && chars[i] == 'u' &&
                       JS7_ISHEX(chars[i + 1]) && JS7_ISHEX(chars[i + 2]) &&
                       JS7_ISHEX(chars[i + 3]) && JS7_ISHEX(chars[i + 4]))
            {
                ch = (((((JS7_UNHEX(chars[i + 1]) << 4)
                        + JS7_UNHEX(chars[i + 2])) << 4)
                      + JS7_UNHEX(chars[i + 3])) << 4)
                    + JS7_UNHEX(chars[i + 4]);
                i += 5;
            }
        }
        newchars[ni++] = ch;
    }
    newchars[ni] = 0;

    JSString *retstr = js_NewString(cx, newchars, ni);
    if (!retstr) {
        cx->free(newchars);
        return JS_FALSE;
    }
    vp->setString(retstr);
    return JS_TRUE;
}

#if JS_HAS_UNEVAL
static JSBool
str_uneval(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str;

    str = js_ValueToSource(cx, argc != 0 ? vp[2] : UndefinedValue());
    if (!str)
        return JS_FALSE;
    vp->setString(str);
    return JS_TRUE;
}
#endif

const char js_escape_str[] = "escape";
const char js_unescape_str[] = "unescape";
#if JS_HAS_UNEVAL
const char js_uneval_str[] = "uneval";
#endif
const char js_decodeURI_str[] = "decodeURI";
const char js_encodeURI_str[] = "encodeURI";
const char js_decodeURIComponent_str[] = "decodeURIComponent";
const char js_encodeURIComponent_str[] = "encodeURIComponent";

static JSFunctionSpec string_functions[] = {
    JS_FN(js_escape_str,             str_escape,                1,0),
    JS_FN(js_unescape_str,           str_unescape,              1,0),
#if JS_HAS_UNEVAL
    JS_FN(js_uneval_str,             str_uneval,                1,0),
#endif
    JS_FN(js_decodeURI_str,          str_decodeURI,             1,0),
    JS_FN(js_encodeURI_str,          str_encodeURI,             1,0),
    JS_FN(js_decodeURIComponent_str, str_decodeURI_Component,   1,0),
    JS_FN(js_encodeURIComponent_str, str_encodeURI_Component,   1,0),

    JS_FS_END
};

jschar      js_empty_ucstr[]  = {0};
JSSubString js_EmptySubString = {0, js_empty_ucstr};

static JSBool
str_getProperty(JSContext *cx, JSObject *obj, jsid id, Value *vp)
{
    JSString *str;

    if (JSID_IS_ATOM(id, cx->runtime->atomState.lengthAtom)) {
        if (obj->getClass() == &js_StringClass) {
            /* Follow ECMA-262 by fetching intrinsic length of our string. */
            str = obj->getPrimitiveThis().toString();
        } else {
            /* Preserve compatibility: convert obj to a string primitive. */
            str = js_ValueToString(cx, ObjectValue(*obj));
            if (!str)
                return JS_FALSE;
        }

        vp->setInt32(str->length());
    }

    return JS_TRUE;
}

#define STRING_ELEMENT_ATTRS (JSPROP_ENUMERATE|JSPROP_READONLY|JSPROP_PERMANENT)

static JSBool
str_enumerate(JSContext *cx, JSObject *obj)
{
    JSString *str, *str1;
    size_t i, length;

    str = obj->getPrimitiveThis().toString();

    length = str->length();
    for (i = 0; i < length; i++) {
        str1 = js_NewDependentString(cx, str, i, 1);
        if (!str1)
            return JS_FALSE;
        if (!obj->defineProperty(cx, INT_TO_JSID(i), StringValue(str1),
                                 PropertyStub, StrictPropertyStub,
                                 STRING_ELEMENT_ATTRS)) {
            return JS_FALSE;
        }
    }

    return obj->defineProperty(cx, ATOM_TO_JSID(cx->runtime->atomState.lengthAtom),
                               UndefinedValue(), NULL, NULL,
                               JSPROP_PERMANENT | JSPROP_READONLY | JSPROP_SHARED);
}

static JSBool
str_resolve(JSContext *cx, JSObject *obj, jsid id, uintN flags,
            JSObject **objp)
{
    if (!JSID_IS_INT(id))
        return JS_TRUE;

    JSString *str = obj->getPrimitiveThis().toString();

    jsint slot = JSID_TO_INT(id);
    if ((size_t)slot < str->length()) {
        JSString *str1 = JSString::getUnitString(cx, str, size_t(slot));
        if (!str1)
            return JS_FALSE;
        if (!obj->defineProperty(cx, id, StringValue(str1), NULL, NULL,
                                 STRING_ELEMENT_ATTRS)) {
            return JS_FALSE;
        }
        *objp = obj;
    }
    return JS_TRUE;
}

Class js_StringClass = {
    js_String_str,
    JSCLASS_HAS_RESERVED_SLOTS(1) | JSCLASS_NEW_RESOLVE |
    JSCLASS_HAS_CACHED_PROTO(JSProto_String),
    PropertyStub,         /* addProperty */
    PropertyStub,         /* delProperty */
    str_getProperty,
    StrictPropertyStub,   /* setProperty */
    str_enumerate,
    (JSResolveOp)str_resolve,
    ConvertStub
};

/*
 * Returns a JSString * for the |this| value associated with vp, or throws a
 * TypeError if |this| is null or undefined.  This algorithm is the same as
 * calling CheckObjectCoercible(this), then returning ToString(this), as all
 * String.prototype.* methods do.
 */
static JS_ALWAYS_INLINE JSString *
ThisToStringForStringProto(JSContext *cx, Value *vp)
{
    if (vp[1].isString())
        return vp[1].toString();

    if (vp[1].isObject()) {
        JSObject *obj = &vp[1].toObject();
        if (obj->getClass() == &js_StringClass &&
            ClassMethodIsNative(cx, obj,
                                &js_StringClass,
                                ATOM_TO_JSID(cx->runtime->atomState.toStringAtom),
                                js_str_toString))
        {
            vp[1] = obj->getPrimitiveThis();
            return vp[1].toString();
        }
    } else if (vp[1].isNullOrUndefined()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_CONVERT_TO,
                             vp[1].isNull() ? "null" : "undefined", "object");
        return NULL;
    }

    JSString *str = js_ValueToString(cx, vp[1]);
    if (!str)
        return NULL;
    vp[1].setString(str);
    return str;
}

#if JS_HAS_TOSOURCE

/*
 * String.prototype.quote is generic (as are most string methods), unlike
 * toSource, toString, and valueOf.
 */
static JSBool
str_quote(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str = ThisToStringForStringProto(cx, vp);
    if (!str)
        return false;
    str = js_QuoteString(cx, str, '"');
    if (!str)
        return false;
    vp->setString(str);
    return true;
}

static JSBool
str_toSource(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str;
    if (!GetPrimitiveThis(cx, vp, &str))
        return false;

    str = js_QuoteString(cx, str, '"');
    if (!str)
        return false;

    char buf[16];
    size_t j = JS_snprintf(buf, sizeof buf, "(new String(");

    JS::Anchor<JSString *> anchor(str);
    size_t k = str->length();
    const jschar *s = str->getChars(cx);
    if (!s)
        return false;

    size_t n = j + k + 2;
    jschar *t = (jschar *) cx->malloc((n + 1) * sizeof(jschar));
    if (!t)
        return false;

    size_t i;
    for (i = 0; i < j; i++)
        t[i] = buf[i];
    for (j = 0; j < k; i++, j++)
        t[i] = s[j];
    t[i++] = ')';
    t[i++] = ')';
    t[i] = 0;

    str = js_NewString(cx, t, n);
    if (!str) {
        cx->free(t);
        return false;
    }
    vp->setString(str);
    return true;
}

#endif /* JS_HAS_TOSOURCE */

JSBool
js_str_toString(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str;
    if (!GetPrimitiveThis(cx, vp, &str))
        return false;
    vp->setString(str);
    return true;
}

/*
 * Java-like string native methods.
 */
 
JS_ALWAYS_INLINE bool
ValueToIntegerRange(JSContext *cx, const Value &v, int32 *out)
{
    if (v.isInt32()) {
        *out = v.toInt32();
    } else {
        double d;

        if (!ValueToNumber(cx, v, &d))
            return false;

        d = js_DoubleToInteger(d);
        if (d > INT32_MAX)
            *out = INT32_MAX;
        else if (d < INT32_MIN)
            *out = INT32_MIN;
        else 
            *out = int32(d);
    }

    return true;
}

static JSBool
str_substring(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str = ThisToStringForStringProto(cx, vp);
    if (!str)
        return false;

    int32 length, begin, end;
    if (argc > 0) {
        end = length = int32(str->length());

        if (!ValueToIntegerRange(cx, vp[2], &begin))
            return false;

        if (begin < 0)
            begin = 0;
        else if (begin > length)
            begin = length;

        if (argc > 1 && !vp[3].isUndefined()) {
            if (!ValueToIntegerRange(cx, vp[3], &end))
                return false;

            if (end > length) {
                end = length;
            } else {
                if (end < 0)
                    end = 0;
                if (end < begin) {
                    int32_t tmp = begin;
                    begin = end;
                    end = tmp;
                }
            }
        }

        str = js_NewDependentString(cx, str, size_t(begin), size_t(end - begin));
        if (!str)
            return false;
    }

    vp->setString(str);
    return true;
}

JSString* JS_FASTCALL
js_toLowerCase(JSContext *cx, JSString *str)
{
    size_t n = str->length();
    const jschar *s = str->getChars(cx);
    if (!s)
        return NULL;

    jschar *news = (jschar *) cx->malloc((n + 1) * sizeof(jschar));
    if (!news)
        return NULL;
    for (size_t i = 0; i < n; i++)
        news[i] = JS_TOLOWER(s[i]);
    news[n] = 0;
    str = js_NewString(cx, news, n);
    if (!str) {
        cx->free(news);
        return NULL;
    }
    return str;
}

static JSBool
str_toLowerCase(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str = ThisToStringForStringProto(cx, vp);
    if (!str)
        return false;
    str = js_toLowerCase(cx, str);
    if (!str)
        return false;
    vp->setString(str);
    return true;
}

static JSBool
str_toLocaleLowerCase(JSContext *cx, uintN argc, Value *vp)
{
    /*
     * Forcefully ignore the first (or any) argument and return toLowerCase(),
     * ECMA has reserved that argument, presumably for defining the locale.
     */
    if (cx->localeCallbacks && cx->localeCallbacks->localeToLowerCase) {
        JSString *str = ThisToStringForStringProto(cx, vp);
        if (!str)
            return false;
        return cx->localeCallbacks->localeToLowerCase(cx, str, Jsvalify(vp));
    }

    return str_toLowerCase(cx, 0, vp);
}

JSString* JS_FASTCALL
js_toUpperCase(JSContext *cx, JSString *str)
{
    size_t n = str->length();
    const jschar *s = str->getChars(cx);
    if (!s)
        return NULL;
    jschar *news = (jschar *) cx->malloc((n + 1) * sizeof(jschar));
    if (!news)
        return NULL;
    for (size_t i = 0; i < n; i++)
        news[i] = JS_TOUPPER(s[i]);
    news[n] = 0;
    str = js_NewString(cx, news, n);
    if (!str) {
        cx->free(news);
        return NULL;
    }
    return str;
}

static JSBool
str_toUpperCase(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str = ThisToStringForStringProto(cx, vp);
    if (!str)
        return false;
    str = js_toUpperCase(cx, str);
    if (!str)
        return false;
    vp->setString(str);
    return true;
}

static JSBool
str_toLocaleUpperCase(JSContext *cx, uintN argc, Value *vp)
{
    /*
     * Forcefully ignore the first (or any) argument and return toUpperCase(),
     * ECMA has reserved that argument, presumably for defining the locale.
     */
    if (cx->localeCallbacks && cx->localeCallbacks->localeToUpperCase) {
        JSString *str = ThisToStringForStringProto(cx, vp);
        if (!str)
            return false;
        return cx->localeCallbacks->localeToUpperCase(cx, str, Jsvalify(vp));
    }

    return str_toUpperCase(cx, 0, vp);
}

static JSBool
str_localeCompare(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str = ThisToStringForStringProto(cx, vp);
    if (!str)
        return false;

    if (argc == 0) {
        vp->setInt32(0);
    } else {
        JSString *thatStr = js_ValueToString(cx, vp[2]);
        if (!thatStr)
            return false;
        if (cx->localeCallbacks && cx->localeCallbacks->localeCompare) {
            vp[2].setString(thatStr);
            return cx->localeCallbacks->localeCompare(cx, str, thatStr, Jsvalify(vp));
        }
        int32 result;
        if (!CompareStrings(cx, str, thatStr, &result))
            return false;
        vp->setInt32(result);
    }
    return true;
}

JSBool
js_str_charAt(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str;
    jsint i;
    jsdouble d;

    if (vp[1].isString() && argc != 0 && vp[2].isInt32()) {
        str = vp[1].toString();
        i = vp[2].toInt32();
        if ((size_t)i >= str->length())
            goto out_of_range;
    } else {
        str = ThisToStringForStringProto(cx, vp);
        if (!str)
            return false;

        if (argc == 0) {
            d = 0.0;
        } else {
            if (!ValueToNumber(cx, vp[2], &d))
                return false;
            d = js_DoubleToInteger(d);
        }

        if (d < 0 || str->length() <= d)
            goto out_of_range;
        i = (jsint) d;
    }

    str = JSString::getUnitString(cx, str, size_t(i));
    if (!str)
        return false;
    vp->setString(str);
    return true;

  out_of_range:
    vp->setString(cx->runtime->emptyString);
    return true;
}

JSBool
js_str_charCodeAt(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str;
    jsint i;
    if (vp[1].isString() && argc != 0 && vp[2].isInt32()) {
        str = vp[1].toString();
        i = vp[2].toInt32();
        if ((size_t)i >= str->length())
            goto out_of_range;
    } else {
        str = ThisToStringForStringProto(cx, vp);
        if (!str)
            return false;

        double d;
        if (argc == 0) {
            d = 0.0;
        } else {
            if (!ValueToNumber(cx, vp[2], &d))
                return false;
            d = js_DoubleToInteger(d);
        }

        if (d < 0 || str->length() <= d)
            goto out_of_range;
        i = (jsint) d;
    }

    const jschar *chars;
    chars = str->getChars(cx);
    if (!chars)
        return false;

    vp->setInt32(chars[i]);
    return true;

out_of_range:
    vp->setDouble(js_NaN);
    return true;
}

jsint
js_BoyerMooreHorspool(const jschar *text, jsuint textlen,
                      const jschar *pat, jsuint patlen)
{
    uint8 skip[sBMHCharSetSize];

    JS_ASSERT(0 < patlen && patlen <= sBMHPatLenMax);
    for (jsuint i = 0; i < sBMHCharSetSize; i++)
        skip[i] = (uint8)patlen;
    jsuint m = patlen - 1;
    for (jsuint i = 0; i < m; i++) {
        jschar c = pat[i];
        if (c >= sBMHCharSetSize)
            return sBMHBadPattern;
        skip[c] = (uint8)(m - i);
    }
    jschar c;
    for (jsuint k = m;
         k < textlen;
         k += ((c = text[k]) >= sBMHCharSetSize) ? patlen : skip[c]) {
        for (jsuint i = k, j = m; ; i--, j--) {
            if (text[i] != pat[j])
                break;
            if (j == 0)
                return static_cast<jsint>(i);  /* safe: max string size */
        }
    }
    return -1;
}

struct MemCmp {
    typedef jsuint Extent;
    static JS_ALWAYS_INLINE Extent computeExtent(const jschar *, jsuint patlen) {
        return (patlen - 1) * sizeof(jschar);
    }
    static JS_ALWAYS_INLINE bool match(const jschar *p, const jschar *t, Extent extent) {
        return memcmp(p, t, extent) == 0;
    }
};

struct ManualCmp {
    typedef const jschar *Extent;
    static JS_ALWAYS_INLINE Extent computeExtent(const jschar *pat, jsuint patlen) {
        return pat + patlen;
    }
    static JS_ALWAYS_INLINE bool match(const jschar *p, const jschar *t, Extent extent) {
        for (; p != extent; ++p, ++t) {
            if (*p != *t)
                return false;
        }
        return true;
    }
};

template <class InnerMatch>
static jsint
UnrolledMatch(const jschar *text, jsuint textlen, const jschar *pat, jsuint patlen)
{
    JS_ASSERT(patlen > 0 && textlen > 0);
    const jschar *textend = text + textlen - (patlen - 1);
    const jschar p0 = *pat;
    const jschar *const patNext = pat + 1;
    const typename InnerMatch::Extent extent = InnerMatch::computeExtent(pat, patlen);
    uint8 fixup;

    const jschar *t = text;
    switch ((textend - t) & 7) {
      case 0: if (*t++ == p0) { fixup = 8; goto match; }
      case 7: if (*t++ == p0) { fixup = 7; goto match; }
      case 6: if (*t++ == p0) { fixup = 6; goto match; }
      case 5: if (*t++ == p0) { fixup = 5; goto match; }
      case 4: if (*t++ == p0) { fixup = 4; goto match; }
      case 3: if (*t++ == p0) { fixup = 3; goto match; }
      case 2: if (*t++ == p0) { fixup = 2; goto match; }
      case 1: if (*t++ == p0) { fixup = 1; goto match; }
    }
    while (t != textend) {
      if (t[0] == p0) { t += 1; fixup = 8; goto match; }
      if (t[1] == p0) { t += 2; fixup = 7; goto match; }
      if (t[2] == p0) { t += 3; fixup = 6; goto match; }
      if (t[3] == p0) { t += 4; fixup = 5; goto match; }
      if (t[4] == p0) { t += 5; fixup = 4; goto match; }
      if (t[5] == p0) { t += 6; fixup = 3; goto match; }
      if (t[6] == p0) { t += 7; fixup = 2; goto match; }
      if (t[7] == p0) { t += 8; fixup = 1; goto match; }
        t += 8;
        continue;
        do {
            if (*t++ == p0) {
              match:
                if (!InnerMatch::match(patNext, t, extent))
                    goto failed_match;
                return t - text - 1;
            }
          failed_match:;
        } while (--fixup > 0);
    }
    return -1;
}

static JS_ALWAYS_INLINE jsint
StringMatch(const jschar *text, jsuint textlen,
            const jschar *pat, jsuint patlen)
{
    if (patlen == 0)
        return 0;
    if (textlen < patlen)
        return -1;

#if defined(__i386__) || defined(_M_IX86) || defined(__i386)
    /*
     * Given enough registers, the unrolled loop below is faster than the
     * following loop. 32-bit x86 does not have enough registers.
     */
    if (patlen == 1) {
        const jschar p0 = *pat;
        for (const jschar *c = text, *end = text + textlen; c != end; ++c) {
            if (*c == p0)
                return c - text;
        }
        return -1;
    }
#endif

    /*
     * If the text or pattern string is short, BMH will be more expensive than
     * the basic linear scan due to initialization cost and a more complex loop
     * body. While the correct threshold is input-dependent, we can make a few
     * conservative observations:
     *  - When |textlen| is "big enough", the initialization time will be
     *    proportionally small, so the worst-case slowdown is minimized.
     *  - When |patlen| is "too small", even the best case for BMH will be
     *    slower than a simple scan for large |textlen| due to the more complex
     *    loop body of BMH.
     * From this, the values for "big enough" and "too small" are determined
     * empirically. See bug 526348.
     */
    if (textlen >= 512 && patlen >= 11 && patlen <= sBMHPatLenMax) {
        jsint index = js_BoyerMooreHorspool(text, textlen, pat, patlen);
        if (index != sBMHBadPattern)
            return index;
    }

    /*
     * For big patterns with large potential overlap we want the SIMD-optimized
     * speed of memcmp. For small patterns, a simple loop is faster.
     *
     * FIXME: Linux memcmp performance is sad and the manual loop is faster.
     */
    return
#if !defined(__linux__)
           patlen > 128 ? UnrolledMatch<MemCmp>(text, textlen, pat, patlen)
                        :
#endif
                          UnrolledMatch<ManualCmp>(text, textlen, pat, patlen);
}

static const size_t sRopeMatchThresholdRatioLog2 = 5;

/*
 * RopeMatch takes the text to search, the patern to search for in the text.
 * RopeMatch returns false on OOM and otherwise returns the match index through
 * the 'match' outparam (-1 for not found).
 */
static bool
RopeMatch(JSContext *cx, JSString *textstr, const jschar *pat, jsuint patlen, jsint *match)
{
    JS_ASSERT(textstr->isRope());

    if (patlen == 0) {
        *match = 0;
        return true;
    }
    if (textstr->length() < patlen) {
        *match = -1;
        return true;
    }

    /*
     * List of leaf nodes in the rope. If we run out of memory when trying to
     * append to this list, we can still fall back to StringMatch, so use the
     * system allocator so we don't report OOM in that case.
     */
    Vector<JSString *, 16, SystemAllocPolicy> strs;

    /*
     * We don't want to do rope matching if there is a poor node-to-char ratio,
     * since this means spending a lot of time in the match loop below. We also
     * need to build the list of leaf nodes. Do both here: iterate over the
     * nodes so long as there are not too many.
     */
    {
        size_t textstrlen = textstr->length();
        size_t threshold = textstrlen >> sRopeMatchThresholdRatioLog2;
        StringSegmentRange r(cx);
        if (!r.init(textstr))
            return false;
        while (!r.empty()) {
            if (threshold-- == 0 || !strs.append(r.front())) {
                const jschar *chars = textstr->getChars(cx);
                if (!chars)
                    return false;
                *match = StringMatch(chars, textstrlen, pat, patlen);
                return true;
            }
            if (!r.popFront())
                return false;
        }
    }

    /* Absolute offset from the beginning of the logical string textstr. */
    jsint pos = 0;

    // TODO: consider branching to a simple loop if patlen == 1

    for (JSString **outerp = strs.begin(); outerp != strs.end(); ++outerp) {
        /* First try to match without spanning two nodes. */
        JSString *outer = *outerp;
        const jschar *chars = outer->nonRopeChars();
        size_t len = outer->length();
        jsint matchResult = StringMatch(chars, len, pat, patlen);
        if (matchResult != -1) {
            *match = pos + matchResult;
            return true;
        }

        /* Test the overlap. */
        JSString **innerp = outerp;

        /*
         * Start searching at the first place where StringMatch wouldn't have
         * found the match.
         */
        const jschar *const text = chars + (patlen > len ? 0 : len - patlen + 1);
        const jschar *const textend = chars + len;
        const jschar p0 = *pat;
        const jschar *const p1 = pat + 1;
        const jschar *const patend = pat + patlen;
        for (const jschar *t = text; t != textend; ) {
            if (*t++ != p0)
                continue;
            const jschar *ttend = textend;
            for (const jschar *pp = p1, *tt = t; pp != patend; ++pp, ++tt) {
                while (tt == ttend) {
                    if (++innerp == strs.end()) {
                        *match = -1;
                        return true;
                    }
                    JSString *inner = *innerp;
                    tt = inner->nonRopeChars();
                    ttend = tt + inner->length();
                }
                if (*pp != *tt)
                    goto break_continue;
            }

            /* Matched! */
            *match = pos + (t - chars) - 1;  /* -1 because of *t++ above */
            return true;

          break_continue:;
        }

        pos += len;
    }

    *match = -1;
    return true;
}

static JSBool
str_indexOf(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str = ThisToStringForStringProto(cx, vp);
    if (!str)
        return false;

    JSLinearString *patstr = ArgToRootedString(cx, argc, vp, 0);
    if (!patstr)
        return false;

    jsuint textlen = str->length();
    const jschar *text = str->getChars(cx);
    if (!text)
        return false;

    jsuint patlen = patstr->length();
    const jschar *pat = patstr->chars();

    jsuint start;
    if (argc > 1) {
        if (vp[3].isInt32()) {
            jsint i = vp[3].toInt32();
            if (i <= 0) {
                start = 0;
            } else if (jsuint(i) > textlen) {
                start = textlen;
                textlen = 0;
            } else {
                start = i;
                text += start;
                textlen -= start;
            }
        } else {
            jsdouble d;
            if (!ValueToNumber(cx, vp[3], &d))
                return JS_FALSE;
            d = js_DoubleToInteger(d);
            if (d <= 0) {
                start = 0;
            } else if (d > textlen) {
                start = textlen;
                textlen = 0;
            } else {
                start = (jsint)d;
                text += start;
                textlen -= start;
            }
        }
    } else {
        start = 0;
    }

    jsint match = StringMatch(text, textlen, pat, patlen);
    vp->setInt32((match == -1) ? -1 : start + match);
    return true;
}

static JSBool
str_lastIndexOf(JSContext *cx, uintN argc, Value *vp)
{
    JSString *textstr = ThisToStringForStringProto(cx, vp);
    if (!textstr)
        return false;
    size_t textlen = textstr->length();
    const jschar *text = textstr->getChars(cx);
    if (!text)
        return false;

    JSLinearString *patstr = ArgToRootedString(cx, argc, vp, 0);
    if (!patstr)
        return false;

    size_t patlen = patstr->length();
    const jschar *pat = patstr->chars();

    jsint i = textlen - patlen; // Start searching here
    if (i < 0) {
        vp->setInt32(-1);
        return true;
    }

    if (argc > 1) {
        if (vp[3].isInt32()) {
            jsint j = vp[3].toInt32();
            if (j <= 0)
                i = 0;
            else if (j < i)
                i = j;
        } else {
            double d;
            if (!ValueToNumber(cx, vp[3], &d))
                return false;
            if (!JSDOUBLE_IS_NaN(d)) {
                d = js_DoubleToInteger(d);
                if (d <= 0)
                    i = 0;
                else if (d < i)
                    i = (jsint)d;
            }
        }
    }

    if (patlen == 0) {
        vp->setInt32(i);
        return true;
    }

    const jschar *t = text + i;
    const jschar *textend = text - 1;
    const jschar p0 = *pat;
    const jschar *patNext = pat + 1;
    const jschar *patEnd = pat + patlen;

    for (; t != textend; --t) {
        if (*t == p0) {
            const jschar *t1 = t + 1;
            for (const jschar *p1 = patNext; p1 != patEnd; ++p1, ++t1) {
                if (*t1 != *p1)
                    goto break_continue;
            }
            vp->setInt32(t - text);
            return true;
        }
      break_continue:;
    }

    vp->setInt32(-1);
    return true;
}

static JSBool
js_TrimString(JSContext *cx, Value *vp, JSBool trimLeft, JSBool trimRight)
{
    JSString *str = ThisToStringForStringProto(cx, vp);
    if (!str)
        return false;
    size_t length = str->length();
    const jschar *chars = str->getChars(cx);
    if (!chars)
        return false;

    size_t begin = 0;
    size_t end = length;

    if (trimLeft) {
        while (begin < length && JS_ISSPACE(chars[begin]))
            ++begin;
    }

    if (trimRight) {
        while (end > begin && JS_ISSPACE(chars[end-1]))
            --end;
    }

    str = js_NewDependentString(cx, str, begin, end - begin);
    if (!str)
        return false;

    vp->setString(str);
    return true;
}

static JSBool
str_trim(JSContext *cx, uintN argc, Value *vp)
{
    return js_TrimString(cx, vp, JS_TRUE, JS_TRUE);
}

static JSBool
str_trimLeft(JSContext *cx, uintN argc, Value *vp)
{
    return js_TrimString(cx, vp, JS_TRUE, JS_FALSE);
}

static JSBool
str_trimRight(JSContext *cx, uintN argc, Value *vp)
{
    return js_TrimString(cx, vp, JS_FALSE, JS_TRUE);
}

/*
 * Perl-inspired string functions.
 */

/* Result of a successfully performed flat match. */
class FlatMatch
{
    JSLinearString  *patstr;
    const jschar    *pat;
    size_t          patlen;
    int32           match_;

    friend class RegExpGuard;

  public:
    FlatMatch() : patstr(NULL) {} /* Old GCC wants this initialization. */
    JSString *pattern() const { return patstr; }
    size_t patternLength() const { return patlen; }

    /*
     * Note: The match is -1 when the match is performed successfully,
     * but no match is found.
     */
    int32 match() const { return match_; }
};

/* A regexp and optional associated object. */
class RegExpPair
{
    AutoRefCount<RegExp>    re_;
    JSObject                *reobj_;

    explicit RegExpPair(RegExpPair &);

  public:
    explicit RegExpPair(JSContext *cx) : re_(cx) {}

    void reset(JSObject &obj) {
        reobj_ = &obj;
        RegExp *re = RegExp::extractFrom(reobj_);
        JS_ASSERT(re);
        re_.reset(NeedsIncRef<RegExp>(re));
    }

    void reset(AlreadyIncRefed<RegExp> re) {
        reobj_ = NULL;
        re_.reset(re);
    }

    /* Note: May be null. */
    JSObject *reobj() const { return reobj_; }
    bool hasRegExp() const { return !re_.null(); }
    RegExp &re() const { JS_ASSERT(hasRegExp()); return *re_; }
};

/*
 * RegExpGuard factors logic out of String regexp operations.
 *
 * @param optarg    Indicates in which argument position RegExp
 *                  flags will be found, if present. This is a Mozilla
 *                  extension and not part of any ECMA spec.
 */
class RegExpGuard
{
    RegExpGuard(const RegExpGuard &);
    void operator=(const RegExpGuard &);

    JSContext   *cx;
    RegExpPair  rep;
    FlatMatch   fm;

    /*
     * Upper bound on the number of characters we are willing to potentially
     * waste on searching for RegExp meta-characters.
     */
    static const size_t MAX_FLAT_PAT_LEN = 256;

    static JSString *flattenPattern(JSContext *cx, JSLinearString *patstr) {
        StringBuffer sb(cx);
        if (!sb.reserve(patstr->length()))
            return NULL;

        static const jschar ESCAPE_CHAR = '\\';
        const jschar *chars = patstr->chars();
        size_t len = patstr->length();
        for (const jschar *it = chars; it != chars + len; ++it) {
            if (RegExp::isMetaChar(*it)) {
                if (!sb.append(ESCAPE_CHAR) || !sb.append(*it))
                    return NULL;
            } else {
                if (!sb.append(*it))
                    return NULL;
            }
        }
        return sb.finishString();
    }

  public:
    explicit RegExpGuard(JSContext *cx) : cx(cx), rep(cx) {}
    ~RegExpGuard() {}

    /* init must succeed in order to call tryFlatMatch or normalizeRegExp. */
    bool
    init(uintN argc, Value *vp)
    {
        if (argc != 0 && VALUE_IS_REGEXP(cx, vp[2])) {
            rep.reset(vp[2].toObject());
        } else {
            fm.patstr = ArgToRootedString(cx, argc, vp, 0);
            if (!fm.patstr)
                return false;
        }
        return true;
    }

    /*
     * Attempt to match |patstr| to |textstr|. A flags argument, metachars in the
     * pattern string, or a lengthy pattern string can thwart this process.
     *
     * @param checkMetaChars    Look for regexp metachars in the pattern string.
     * @return                  Whether flat matching could be used.
     *
     * N.B. tryFlatMatch returns NULL on OOM, so the caller must check cx->isExceptionPending().
     */
    const FlatMatch *
    tryFlatMatch(JSContext *cx, JSString *textstr, uintN optarg, uintN argc,
                 bool checkMetaChars = true)
    {
        if (rep.hasRegExp())
            return NULL;

        fm.pat = fm.patstr->chars();
        fm.patlen = fm.patstr->length();

        if (optarg < argc)
            return NULL;

        if (checkMetaChars &&
            (fm.patlen > MAX_FLAT_PAT_LEN || RegExp::hasMetaChars(fm.pat, fm.patlen))) {
            return NULL;
        }

        /*
         * textstr could be a rope, so we want to avoid flattening it for as
         * long as possible.
         */
        if (textstr->isRope()) {
            if (!RopeMatch(cx, textstr, fm.pat, fm.patlen, &fm.match_))
                return NULL;
        } else {
            const jschar *text = textstr->nonRopeChars();
            size_t textlen = textstr->length();
            fm.match_ = StringMatch(text, textlen, fm.pat, fm.patlen);
        }
        return &fm;
    }

    /* If the pattern is not already a regular expression, make it so. */
    const RegExpPair *
    normalizeRegExp(bool flat, uintN optarg, uintN argc, Value *vp)
    {
        if (rep.hasRegExp())
            return &rep;

        /* Build RegExp from pattern string. */
        JSString *opt;
        if (optarg < argc) {
            opt = js_ValueToString(cx, vp[2 + optarg]);
            if (!opt)
                return NULL;
        } else {
            opt = NULL;
        }

        JSString *patstr;
        if (flat) {
            patstr = flattenPattern(cx, fm.patstr);
            if (!patstr)
                return false;
        } else {
            patstr = fm.patstr;
        }
        JS_ASSERT(patstr);

        AlreadyIncRefed<RegExp> re = RegExp::createFlagged(cx, patstr, opt);
        if (!re)
            return NULL;
        rep.reset(re);
        return &rep;
    }

#if DEBUG
    bool hasRegExpPair() const { return rep.hasRegExp(); }
#endif
};

/* js_ExecuteRegExp indicates success in two ways, based on the 'test' flag. */
static JS_ALWAYS_INLINE bool
Matched(bool test, const Value &v)
{
    return test ? v.isTrue() : !v.isNull();
}

typedef bool (*DoMatchCallback)(JSContext *cx, RegExpStatics *res, size_t count, void *data);

/*
 * BitOR-ing these flags allows the DoMatch caller to control when how the
 * RegExp engine is called and when callbacks are fired.
 */
enum MatchControlFlags {
   TEST_GLOBAL_BIT         = 0x1, /* use RegExp.test for global regexps */
   TEST_SINGLE_BIT         = 0x2, /* use RegExp.test for non-global regexps */
   CALLBACK_ON_SINGLE_BIT  = 0x4, /* fire callback on non-global match */

   MATCH_ARGS    = TEST_GLOBAL_BIT,
   MATCHALL_ARGS = CALLBACK_ON_SINGLE_BIT,
   REPLACE_ARGS  = TEST_GLOBAL_BIT | TEST_SINGLE_BIT | CALLBACK_ON_SINGLE_BIT
};

/* Factor out looping and matching logic. */
static bool
DoMatch(JSContext *cx, RegExpStatics *res, Value *vp, JSString *str, const RegExpPair &rep,
        DoMatchCallback callback, void *data, MatchControlFlags flags)
{
    RegExp &re = rep.re();
    if (re.global()) {
        /* global matching ('g') */
        bool testGlobal = flags & TEST_GLOBAL_BIT;
        if (rep.reobj())
            rep.reobj()->zeroRegExpLastIndex();
        for (size_t count = 0, i = 0, length = str->length(); i <= length; ++count) {
            if (!re.execute(cx, res, str, &i, testGlobal, vp))
                return false;
            if (!Matched(testGlobal, *vp))
                break;
            if (!callback(cx, res, count, data))
                return false;
            if (!res->matched())
                ++i;
        }
    } else {
        /* single match */
        bool testSingle = !!(flags & TEST_SINGLE_BIT),
             callbackOnSingle = !!(flags & CALLBACK_ON_SINGLE_BIT);
        size_t i = 0;
        if (!re.execute(cx, res, str, &i, testSingle, vp))
            return false;
        if (callbackOnSingle && Matched(testSingle, *vp) && !callback(cx, res, 0, data))
            return false;
    }
    return true;
}

static bool
BuildFlatMatchArray(JSContext *cx, JSString *textstr, const FlatMatch &fm, Value *vp)
{
    if (fm.match() < 0) {
        vp->setNull();
        return true;
    }

    /* For this non-global match, produce a RegExp.exec-style array. */
    JSObject *obj = NewSlowEmptyArray(cx);
    if (!obj)
        return false;
    vp->setObject(*obj);

    return obj->defineProperty(cx, INT_TO_JSID(0), StringValue(fm.pattern())) &&
           obj->defineProperty(cx, ATOM_TO_JSID(cx->runtime->atomState.indexAtom),
                               Int32Value(fm.match())) &&
           obj->defineProperty(cx, ATOM_TO_JSID(cx->runtime->atomState.inputAtom),
                               StringValue(textstr));
}

typedef JSObject **MatchArgType;

/*
 * DoMatch will only callback on global matches, hence this function builds
 * only the "array of matches" returned by match on global regexps.
 */
static bool
MatchCallback(JSContext *cx, RegExpStatics *res, size_t count, void *p)
{
    JS_ASSERT(count <= JSID_INT_MAX);  /* by max string length */

    JSObject *&arrayobj = *static_cast<MatchArgType>(p);
    if (!arrayobj) {
        arrayobj = NewDenseEmptyArray(cx);
        if (!arrayobj)
            return false;
    }

    Value v;
    if (!res->createLastMatch(cx, &v))
        return false;

    JSAutoResolveFlags rf(cx, JSRESOLVE_QUALIFIED | JSRESOLVE_ASSIGNING);
    return !!arrayobj->setProperty(cx, INT_TO_JSID(count), &v, false);
}

static JSBool
str_match(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str = ThisToStringForStringProto(cx, vp);
    if (!str)
        return false;

    RegExpGuard g(cx);
    if (!g.init(argc, vp))
        return false;
    if (const FlatMatch *fm = g.tryFlatMatch(cx, str, 1, argc))
        return BuildFlatMatchArray(cx, str, *fm, vp);
    if (cx->isExceptionPending())  /* from tryFlatMatch */
        return false;

    const RegExpPair *rep = g.normalizeRegExp(false, 1, argc, vp);
    if (!rep)
        return false;

    AutoObjectRooter array(cx);
    MatchArgType arg = array.addr();
    RegExpStatics *res = cx->regExpStatics();
    if (!DoMatch(cx, res, vp, str, *rep, MatchCallback, arg, MATCH_ARGS))
        return false;

    /* When not global, DoMatch will leave |RegExp.exec()| in *vp. */
    if (rep->re().global())
        vp->setObjectOrNull(array.object());
    return true;
}

static JSBool
str_search(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str = ThisToStringForStringProto(cx, vp);
    if (!str)
        return false;

    RegExpGuard g(cx);
    if (!g.init(argc, vp))
        return false;
    if (const FlatMatch *fm = g.tryFlatMatch(cx, str, 1, argc)) {
        vp->setInt32(fm->match());
        return true;
    }
    if (cx->isExceptionPending())  /* from tryFlatMatch */
        return false;
    const RegExpPair *rep = g.normalizeRegExp(false, 1, argc, vp);
    if (!rep)
        return false;

    RegExpStatics *res = cx->regExpStatics();
    size_t i = 0;
    if (!rep->re().execute(cx, res, str, &i, true, vp))
        return false;

    if (vp->isTrue())
        vp->setInt32(res->matchStart());
    else
        vp->setInt32(-1);
    return true;
}

struct ReplaceData
{
    ReplaceData(JSContext *cx)
     : g(cx), sb(cx)
    {}

    JSString           *str;           /* 'this' parameter object as a string */
    RegExpGuard        g;              /* regexp parameter object and private data */
    JSObject           *lambda;        /* replacement function object or null */
    JSObject           *elembase;      /* object for function(a){return b[a]} replace */
    JSLinearString     *repstr;        /* replacement string */
    const jschar       *dollar;        /* null or pointer to first $ in repstr */
    const jschar       *dollarEnd;     /* limit pointer for js_strchr_limit */
    jsint              leftIndex;      /* left context index in str->chars */
    JSSubString        dollarStr;      /* for "$$" InterpretDollar result */
    bool               calledBack;     /* record whether callback has been called */
    InvokeSessionGuard session;        /* arguments for repeated lambda Invoke call */
    InvokeArgsGuard    singleShot;     /* arguments for single lambda Invoke call */
    StringBuffer       sb;             /* buffer built during DoMatch */
};

static bool
InterpretDollar(JSContext *cx, RegExpStatics *res, const jschar *dp, const jschar *ep,
                ReplaceData &rdata, JSSubString *out, size_t *skip)
{
    JS_ASSERT(*dp == '$');

    /* If there is only a dollar, bail now */
    if (dp + 1 >= ep)
        return false;

    /* Interpret all Perl match-induced dollar variables. */
    jschar dc = dp[1];
    if (JS7_ISDEC(dc)) {
        /* ECMA-262 Edition 3: 1-9 or 01-99 */
        uintN num = JS7_UNDEC(dc);
        if (num > res->parenCount())
            return false;

        const jschar *cp = dp + 2;
        if (cp < ep && (dc = *cp, JS7_ISDEC(dc))) {
            uintN tmp = 10 * num + JS7_UNDEC(dc);
            if (tmp <= res->parenCount()) {
                cp++;
                num = tmp;
            }
        }
        if (num == 0)
            return false;

        *skip = cp - dp;

        JS_ASSERT(num <= res->parenCount());

        /* 
         * Note: we index to get the paren with the (1-indexed) pair
         * number, as opposed to a (0-indexed) paren number.
         */
        res->getParen(num, out);
        return true;
    }

    *skip = 2;
    switch (dc) {
      case '$':
        rdata.dollarStr.chars = dp;
        rdata.dollarStr.length = 1;
        *out = rdata.dollarStr;
        return true;
      case '&':
        res->getLastMatch(out);
        return true;
      case '+':
        res->getLastParen(out);
        return true;
      case '`':
        res->getLeftContext(out);
        return true;
      case '\'':
        res->getRightContext(out);
        return true;
    }
    return false;
}

static bool
FindReplaceLength(JSContext *cx, RegExpStatics *res, ReplaceData &rdata, size_t *sizep)
{
    JSObject *base = rdata.elembase;
    if (base) {
        /*
         * The base object is used when replace was passed a lambda which looks like
         * 'function(a) { return b[a]; }' for the base object b.  b will not change
         * in the course of the replace unless we end up making a scripted call due
         * to accessing a scripted getter or a value with a scripted toString.
         */
        JS_ASSERT(rdata.lambda);
        JS_ASSERT(!base->getOps()->lookupProperty);
        JS_ASSERT(!base->getOps()->getProperty);

        Value match;
        if (!res->createLastMatch(cx, &match))
            return false;
        JSString *str = match.toString();

        JSAtom *atom;
        if (str->isAtomized()) {
            atom = STRING_TO_ATOM(str);
        } else {
            atom = js_AtomizeString(cx, str, 0);
            if (!atom)
                return false;
        }
        jsid id = ATOM_TO_JSID(atom);

        JSObject *holder;
        JSProperty *prop = NULL;
        if (js_LookupPropertyWithFlags(cx, base, id, JSRESOLVE_QUALIFIED, &holder, &prop) < 0)
            return false;

        /* Only handle the case where the property exists and is on this object. */
        if (prop && holder == base) {
            Shape *shape = (Shape *) prop;
            if (shape->slot != SHAPE_INVALID_SLOT && shape->hasDefaultGetter()) {
                Value value = base->getSlot(shape->slot);
                if (value.isString()) {
                    rdata.repstr = value.toString()->ensureLinear(cx);
                    if (!rdata.repstr)
                        return false;
                    *sizep = rdata.repstr->length();
                    return true;
                }
            }
        }

        /*
         * Couldn't handle this property, fall through and despecialize to the
         * general lambda case.
         */
        rdata.elembase = NULL;
    }

    JSObject *lambda = rdata.lambda;
    if (lambda) {
        /*
         * In the lambda case, not only do we find the replacement string's
         * length, we compute repstr and return it via rdata for use within
         * DoReplace.  The lambda is called with arguments ($&, $1, $2, ...,
         * index, input), i.e., all the properties of a regexp match array.
         * For $&, etc., we must create string jsvals from cx->regExpStatics.
         * We grab up stack space to keep the newborn strings GC-rooted.
         */
        uintN p = res->parenCount();
        uintN argc = 1 + p + 2;

        InvokeSessionGuard &session = rdata.session;
        if (!session.started()) {
            Value lambdav = ObjectValue(*lambda);
            if (!session.start(cx, lambdav, UndefinedValue(), argc))
                return false;
        }

        PreserveRegExpStatics staticsGuard(res);
        if (!staticsGuard.init(cx))
            return false;

        /* Push $&, $1, $2, ... */
        uintN argi = 0;
        if (!res->createLastMatch(cx, &session[argi++]))
            return false;

        for (size_t i = 0; i < res->parenCount(); ++i) {
            if (!res->createParen(cx, i + 1, &session[argi++]))
                return false;
        }

        /* Push match index and input string. */
        session[argi++].setInt32(res->matchStart());
        session[argi].setString(rdata.str);

        if (!session.invoke(cx))
            return false;

        /* root repstr: rdata is on the stack, so scanned by conservative gc. */
        JSString *repstr = ValueToString_TestForStringInline(cx, session.rval());
        if (!repstr)
            return false;
        rdata.repstr = repstr->ensureLinear(cx);
        if (!rdata.repstr)
            return false;
        *sizep = rdata.repstr->length();
        return true;
    }

    JSString *repstr = rdata.repstr;
    size_t replen = repstr->length();
    for (const jschar *dp = rdata.dollar, *ep = rdata.dollarEnd; dp;
         dp = js_strchr_limit(dp, '$', ep)) {
        JSSubString sub;
        size_t skip;
        if (InterpretDollar(cx, res, dp, ep, rdata, &sub, &skip)) {
            replen += sub.length - skip;
            dp += skip;
        } else {
            dp++;
        }
    }
    *sizep = replen;
    return true;
}

/* 
 * Precondition: |rdata.sb| already has necessary growth space reserved (as
 * derived from FindReplaceLength).
 */
static void
DoReplace(JSContext *cx, RegExpStatics *res, ReplaceData &rdata)
{
    JSLinearString *repstr = rdata.repstr;
    const jschar *cp;
    const jschar *bp = cp = repstr->chars();

    const jschar *dp = rdata.dollar;
    const jschar *ep = rdata.dollarEnd;
    for (; dp; dp = js_strchr_limit(dp, '$', ep)) {
        /* Move one of the constant portions of the replacement value. */
        size_t len = dp - cp;
        JS_ALWAYS_TRUE(rdata.sb.append(cp, len));
        cp = dp;

        JSSubString sub;
        size_t skip;
        if (InterpretDollar(cx, res, dp, ep, rdata, &sub, &skip)) {
            len = sub.length;
            JS_ALWAYS_TRUE(rdata.sb.append(sub.chars, len));
            cp += skip;
            dp += skip;
        } else {
            dp++;
        }
    }
    JS_ALWAYS_TRUE(rdata.sb.append(cp, repstr->length() - (cp - bp)));
}

static bool
ReplaceRegExpCallback(JSContext *cx, RegExpStatics *res, size_t count, void *p)
{
    ReplaceData &rdata = *static_cast<ReplaceData *>(p);

    rdata.calledBack = true;
    JSLinearString *str = rdata.str->assertIsLinear();  /* flattened for regexp */
    size_t leftoff = rdata.leftIndex;
    const jschar *left = str->chars() + leftoff;
    size_t leftlen = res->matchStart() - leftoff;
    rdata.leftIndex = res->matchLimit();

    size_t replen = 0;  /* silence 'unused' warning */
    if (!FindReplaceLength(cx, res, rdata, &replen))
        return false;

    size_t growth = leftlen + replen;
    if (!rdata.sb.reserve(rdata.sb.length() + growth))
        return false;
    JS_ALWAYS_TRUE(rdata.sb.append(left, leftlen)); /* skipped-over portion of the search value */
    DoReplace(cx, res, rdata);
    return true;
}

static bool
BuildFlatReplacement(JSContext *cx, JSString *textstr, JSString *repstr,
                     const FlatMatch &fm, Value *vp)
{
    RopeBuilder builder(cx);
    size_t match = fm.match();
    size_t matchEnd = match + fm.patternLength();

    if (textstr->isRope()) {
        /*
         * If we are replacing over a rope, avoid flattening it by iterating
         * through it, building a new rope.
         */
        StringSegmentRange r(cx);
        if (!r.init(textstr))
            return false;
        size_t pos = 0;
        while (!r.empty()) {
            JSString *str = r.front();
            size_t len = str->length();
            size_t strEnd = pos + len;
            if (pos < matchEnd && strEnd > match) {
                /*
                 * We need to special-case any part of the rope that overlaps
                 * with the replacement string.
                 */
                if (match >= pos) {
                    /*
                     * If this part of the rope overlaps with the left side of
                     * the pattern, then it must be the only one to overlap with
                     * the first character in the pattern, so we include the
                     * replacement string here.
                     */
                    JSString *leftSide = js_NewDependentString(cx, str, 0, match - pos);
                    if (!leftSide ||
                        !builder.append(leftSide) ||
                        !builder.append(repstr)) {
                        return false;
                    }
                }

                /*
                 * If str runs off the end of the matched string, append the
                 * last part of str.
                 */
                if (strEnd > matchEnd) {
                    JSString *rightSide = js_NewDependentString(cx, str, matchEnd - pos,
                                                                strEnd - matchEnd);
                    if (!rightSide || !builder.append(rightSide))
                        return false;
                }
            } else {
                if (!builder.append(str))
                    return false;
            }
            pos += str->length();
            if (!r.popFront())
                return false;
        }
    } else {
        JSString *leftSide = js_NewDependentString(cx, textstr, 0, match);
        if (!leftSide)
            return false;
        JSString *rightSide = js_NewDependentString(cx, textstr, match + fm.patternLength(),
                                                    textstr->length() - match - fm.patternLength());
        if (!rightSide ||
            !builder.append(leftSide) ||
            !builder.append(repstr) ||
            !builder.append(rightSide)) {
            return false;
        }
    }

    vp->setString(builder.result());
    return true;
}

/*
 * Perform a linear-scan dollar substitution on the replacement text,
 * constructing a result string that looks like:
 *
 *      newstring = string[:matchStart] + dollarSub(replaceValue) + string[matchLimit:]
 */
static inline bool
BuildDollarReplacement(JSContext *cx, JSString *textstrArg, JSLinearString *repstr,
                       const jschar *firstDollar, const FlatMatch &fm, Value *vp)
{
    JSLinearString *textstr = textstrArg->ensureLinear(cx);
    if (!textstr)
        return NULL;

    JS_ASSERT(repstr->chars() <= firstDollar && firstDollar < repstr->chars() + repstr->length());
    size_t matchStart = fm.match();
    size_t matchLimit = matchStart + fm.patternLength();

    /*
     * Most probably:
     *
     *      len(newstr) >= len(orig) - len(match) + len(replacement)
     *
     * Note that dollar vars _could_ make the resulting text smaller than this.
     */
    StringBuffer newReplaceChars(cx);
    if (!newReplaceChars.reserve(textstr->length() - fm.patternLength() + repstr->length()))
        return false;

    /* Move the pre-dollar chunk in bulk. */
    JS_ALWAYS_TRUE(newReplaceChars.append(repstr->chars(), firstDollar));

    /* Move the rest char-by-char, interpreting dollars as we encounter them. */
#define ENSURE(__cond) if (!(__cond)) return false;
    const jschar *repstrLimit = repstr->chars() + repstr->length();
    for (const jschar *it = firstDollar; it < repstrLimit; ++it) {
        if (*it != '$' || it == repstrLimit - 1) {
            ENSURE(newReplaceChars.append(*it));
            continue;
        }

        switch (*(it + 1)) {
          case '$': /* Eat one of the dollars. */
            ENSURE(newReplaceChars.append(*it));
            break;
          case '&':
            ENSURE(newReplaceChars.append(textstr->chars() + matchStart,
                                          textstr->chars() + matchLimit));
            break;
          case '`':
            ENSURE(newReplaceChars.append(textstr->chars(), textstr->chars() + matchStart));
            break;
          case '\'':
            ENSURE(newReplaceChars.append(textstr->chars() + matchLimit,
                                          textstr->chars() + textstr->length()));
            break;
          default: /* The dollar we saw was not special (no matter what its mother told it). */
            ENSURE(newReplaceChars.append(*it));
            continue;
        }
        ++it; /* We always eat an extra char in the above switch. */
    }

    JSString *leftSide = js_NewDependentString(cx, textstr, 0, matchStart);
    ENSURE(leftSide);

    JSString *newReplace = newReplaceChars.finishString();
    ENSURE(newReplace);

    JS_ASSERT(textstr->length() >= matchLimit);
    JSString *rightSide = js_NewDependentString(cx, textstr, matchLimit,
                                                textstr->length() - matchLimit);
    ENSURE(rightSide);

    RopeBuilder builder(cx);
    ENSURE(builder.append(leftSide) &&
           builder.append(newReplace) &&
           builder.append(rightSide));
#undef ENSURE

    vp->setString(builder.result());
    return true;
}

static inline bool
str_replace_regexp(JSContext *cx, uintN argc, Value *vp, ReplaceData &rdata)
{
    const RegExpPair *rep = rdata.g.normalizeRegExp(true, 2, argc, vp);
    if (!rep)
        return false;

    rdata.leftIndex = 0;
    rdata.calledBack = false;

    RegExpStatics *res = cx->regExpStatics();
    if (!DoMatch(cx, res, vp, rdata.str, *rep, ReplaceRegExpCallback, &rdata, REPLACE_ARGS))
        return false;

    if (!rdata.calledBack) {
        /* Didn't match, so the string is unmodified. */
        vp->setString(rdata.str);
        return true;
    }

    JSSubString sub;
    res->getRightContext(&sub);
    if (!rdata.sb.append(sub.chars, sub.length))
        return false;

    JSString *retstr = rdata.sb.finishString();
    if (!retstr)
        return false;

    vp->setString(retstr);
    return true;
}

static inline bool
str_replace_flat_lambda(JSContext *cx, uintN argc, Value *vp, ReplaceData &rdata,
                        const FlatMatch &fm)
{
    JS_ASSERT(fm.match() >= 0);
    LeaveTrace(cx);

    JSString *matchStr = js_NewDependentString(cx, rdata.str, fm.match(), fm.patternLength());
    if (!matchStr)
        return false;

    /* lambda(matchStr, matchStart, textstr) */
    static const uint32 lambdaArgc = 3;
    if (!cx->stack().pushInvokeArgs(cx, lambdaArgc, &rdata.singleShot))
        return false;

    CallArgs &args = rdata.singleShot;
    args.callee().setObject(*rdata.lambda);
    args.thisv().setUndefined();

    Value *sp = args.argv();
    sp[0].setString(matchStr);
    sp[1].setInt32(fm.match());
    sp[2].setString(rdata.str);

    if (!Invoke(cx, rdata.singleShot, 0))
        return false;

    JSString *repstr = js_ValueToString(cx, args.rval());
    if (!repstr)
        return false;

    JSString *leftSide = js_NewDependentString(cx, rdata.str, 0, fm.match());
    if (!leftSide)
        return false;

    size_t matchLimit = fm.match() + fm.patternLength();
    JSString *rightSide = js_NewDependentString(cx, rdata.str, matchLimit,
                                                rdata.str->length() - matchLimit);
    if (!rightSide)
        return false;

    RopeBuilder builder(cx);
    if (!(builder.append(leftSide) &&
          builder.append(repstr) &&
          builder.append(rightSide))) {
        return false;
    }

    vp->setString(builder.result());
    return true;
}

JSBool
js::str_replace(JSContext *cx, uintN argc, Value *vp)
{
    ReplaceData rdata(cx);
    rdata.str = ThisToStringForStringProto(cx, vp);
    if (!rdata.str)
        return false;
    static const uint32 optarg = 2;

    /* Extract replacement string/function. */
    if (argc >= optarg && js_IsCallable(vp[3])) {
        rdata.lambda = &vp[3].toObject();
        rdata.elembase = NULL;
        rdata.repstr = NULL;
        rdata.dollar = rdata.dollarEnd = NULL;

        if (rdata.lambda->isFunction()) {
            JSFunction *fun = rdata.lambda->getFunctionPrivate();
            if (fun->isInterpreted()) {
                /*
                 * Pattern match the script to check if it is is indexing into a
                 * particular object, e.g. 'function(a) { return b[a]; }'.  Avoid
                 * calling the script in such cases, which are used by javascript
                 * packers (particularly the popular Dean Edwards packer) to efficiently
                 * encode large scripts.  We only handle the code patterns generated
                 * by such packers here.
                 */
                JSScript *script = fun->u.i.script;
                jsbytecode *pc = script->code;

                Value table = UndefinedValue();
                if (JSOp(*pc) == JSOP_GETFCSLOT) {
                    table = rdata.lambda->getFlatClosureUpvar(GET_UINT16(pc));
                    pc += JSOP_GETFCSLOT_LENGTH;
                }

                if (table.isObject() &&
                    JSOp(*pc) == JSOP_GETARG && GET_SLOTNO(pc) == 0 &&
                    JSOp(*(pc + JSOP_GETARG_LENGTH)) == JSOP_GETELEM &&
                    JSOp(*(pc + JSOP_GETARG_LENGTH + JSOP_GETELEM_LENGTH)) == JSOP_RETURN) {
                    Class *clasp = table.toObject().getClass();
                    if (clasp->isNative() &&
                        !clasp->ops.lookupProperty &&
                        !clasp->ops.getProperty) {
                        rdata.elembase = &table.toObject();
                    }
                }
            }
        }
    } else {
        rdata.lambda = NULL;
        rdata.elembase = NULL;
        rdata.repstr = ArgToRootedString(cx, argc, vp, 1);
        if (!rdata.repstr)
            return false;

        /* We're about to store pointers into the middle of our string. */
        if (!js_MakeStringImmutable(cx, rdata.repstr))
            return false;
        rdata.dollarEnd = rdata.repstr->chars() + rdata.repstr->length();
        rdata.dollar = js_strchr_limit(rdata.repstr->chars(), '$',
                                       rdata.dollarEnd);
    }

    if (!rdata.g.init(argc, vp))
        return false;

    /*
     * Unlike its |String.prototype| brethren, |replace| doesn't convert
     * its input to a regular expression. (Even if it contains metachars.)
     *
     * However, if the user invokes our (non-standard) |flags| argument
     * extension then we revert to creating a regular expression. Note that
     * this is observable behavior through the side-effect mutation of the
     * |RegExp| statics.
     */

    const FlatMatch *fm = rdata.g.tryFlatMatch(cx, rdata.str, optarg, argc, false);
    if (!fm) {
        if (cx->isExceptionPending())  /* oom in RopeMatch in tryFlatMatch */
            return false;
        JS_ASSERT_IF(!rdata.g.hasRegExpPair(), argc > optarg);
        return str_replace_regexp(cx, argc, vp, rdata);
    }

    if (fm->match() < 0) {
        vp->setString(rdata.str);
        return true;
    }

    if (rdata.lambda)
        return str_replace_flat_lambda(cx, argc, vp, rdata, *fm);

    /* 
     * Note: we could optimize the text.length == pattern.length case if we wanted,
     * even in the presence of dollar metachars.
     */
    if (rdata.dollar)
        return BuildDollarReplacement(cx, rdata.str, rdata.repstr, rdata.dollar, *fm, vp);

    return BuildFlatReplacement(cx, rdata.str, rdata.repstr, *fm, vp);
}

/*
 * Subroutine used by str_split to find the next split point in str, starting
 * at offset *ip and looking either for the separator substring given by sep, or
 * for the next re match.  In the re case, return the matched separator in *sep,
 * and the possibly updated offset in *ip.
 *
 * Return -2 on error, -1 on end of string, >= 0 for a valid index of the next
 * separator occurrence if found, or str->length if no separator is found.
 */
static jsint
find_split(JSContext *cx, RegExpStatics *res, JSString *str, js::RegExp *re, jsint *ip,
           JSSubString *sep)
{
    /*
     * Stop if past end of string.  If at end of string, we will compare the
     * null char stored there (by js_NewString*) to sep->chars[j] in the while
     * loop at the end of this function, so that
     *
     *  "ab,".split(',') => ["ab", ""]
     *
     * and the resulting array converts back to the string "ab," for symmetry.
     * However, we ape Perl and do this only if there is a sufficiently large
     * limit argument (see str_split).
     */
    jsint i = *ip;
    size_t length = str->length();
    if ((size_t)i > length)
        return -1;

    const jschar *chars = str->getChars(cx);
    if (!chars)
        return -2;

    /*
     * Match a regular expression against the separator at or above index i.
     * Call js_ExecuteRegExp with true for the test argument.  On successful
     * match, get the separator from cx->regExpStatics.lastMatch.
     */
    if (re) {
        size_t index;
        Value rval;

      again:
        /* JS1.2 deviated from Perl by never matching at end of string. */
        index = (size_t)i;
        if (!re->execute(cx, res, str, &index, true, &rval))
            return -2;
        if (!rval.isTrue()) {
            /* Mismatch: ensure our caller advances i past end of string. */
            sep->length = 1;
            return length;
        }
        i = (jsint)index;
        JS_ASSERT(sep);
        res->getLastMatch(sep);
        if (sep->length == 0) {
            /*
             * Empty string match: never split on an empty match at the start
             * of a find_split cycle.  Same rule as for an empty global match
             * in DoMatch.
             */
            if (i == *ip) {
                /*
                 * "Bump-along" to avoid sticking at an empty match, but don't
                 * bump past end of string -- our caller must do that by adding
                 * sep->length to our return value.
                 */
                if ((size_t)i == length)
                    return -1;
                i++;
                goto again;
            }
            if ((size_t)i == length) {
                /*
                 * If there was a trivial zero-length match at the end of the
                 * split, then we shouldn't output the matched string at the end
                 * of the split array. See ECMA-262 Ed. 3, 15.5.4.14, Step 15.
                 */
                sep->chars = NULL;
            }
        }
        JS_ASSERT((size_t)i >= sep->length);
        return i - sep->length;
    }

    /*
     * Special case: if sep is the empty string, split str into one character
     * substrings.  Let our caller worry about whether to split once at end of
     * string into an empty substring.
     */
    if (sep->length == 0)
        return ((size_t)i == length) ? -1 : i + 1;

    /*
     * Now that we know sep is non-empty, search starting at i in str for an
     * occurrence of all of sep's chars.  If we find them, return the index of
     * the first separator char.  Otherwise, return length.
     */
    jsint match = StringMatch(chars + i, length - i, sep->chars, sep->length);
    return match == -1 ? length : match + i;
}

static JSBool
str_split(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str = ThisToStringForStringProto(cx, vp);
    if (!str)
        return false;

    if (argc == 0) {
        Value v = StringValue(str);
        JSObject *aobj = NewDenseCopiedArray(cx, 1, &v);
        if (!aobj)
            return false;
        vp->setObject(*aobj);
        return true;
    }

    RegExp *re;
    JSSubString *sep, tmp;
    if (VALUE_IS_REGEXP(cx, vp[2])) {
        re = static_cast<RegExp *>(vp[2].toObject().getPrivate());
        sep = &tmp;

        /* Set a magic value so we can detect a successful re match. */
        sep->chars = NULL;
        sep->length = 0;
    } else {
        JSString *sepstr = js_ValueToString(cx, vp[2]);
        if (!sepstr)
            return false;
        vp[2].setString(sepstr);

        /*
         * Point sep at a local copy of sepstr's header because find_split
         * will modify sep->length.
         */
        tmp.length = sepstr->length();
        tmp.chars = sepstr->getChars(cx);
        if (!tmp.chars)
            return false;
        re = NULL;
        sep = &tmp;
    }

    /* Use the second argument as the split limit, if given. */
    uint32 limit = 0; /* Avoid warning. */
    bool limited = (argc > 1) && !vp[3].isUndefined();
    if (limited) {
        jsdouble d;
        if (!ValueToNumber(cx, vp[3], &d))
            return false;

        /* Clamp limit between 0 and 1 + string length. */
        limit = js_DoubleToECMAUint32(d);
        if (limit > str->length())
            limit = 1 + str->length();
    }

    AutoValueVector splits(cx);

    RegExpStatics *res = cx->regExpStatics();
    jsint i, j;
    uint32 len = i = 0;
    while ((j = find_split(cx, res, str, re, &i, sep)) >= 0) {
        if (limited && len >= limit)
            break;

        JSString *sub = js_NewDependentString(cx, str, i, size_t(j - i));
        if (!sub || !splits.append(StringValue(sub)))
            return false;
        len++;

        /*
         * Imitate perl's feature of including parenthesized substrings that
         * matched part of the delimiter in the new array, after the split
         * substring that was delimited.
         */
        if (re && sep->chars) {
            for (uintN num = 0; num < res->parenCount(); num++) {
                if (limited && len >= limit)
                    break;
                JSSubString parsub;
                res->getParen(num + 1, &parsub);
                sub = js_NewStringCopyN(cx, parsub.chars, parsub.length);
                if (!sub || !splits.append(StringValue(sub)))
                    return false;
                len++;
            }
            sep->chars = NULL;
        }
        i = j + sep->length;
    }

    if (j == -2)
        return false;

    JSObject *aobj = NewDenseCopiedArray(cx, splits.length(), splits.begin());
    if (!aobj)
        return false;
    vp->setObject(*aobj);
    return true;
}

#if JS_HAS_PERL_SUBSTR
static JSBool
str_substr(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str = ThisToStringForStringProto(cx, vp);
    if (!str)
        return false;

    int32 length, len, begin;
    if (argc > 0) {
        length = int32(str->length());
        if (!ValueToIntegerRange(cx, vp[2], &begin))
            return false;

        if (begin >= length) {
            str = cx->runtime->emptyString;
            goto out;
        }
        if (begin < 0) {
            begin += length; /* length + INT_MIN will always be less then 0 */
            if (begin < 0)
                begin = 0;
        }

        if (argc == 1 || vp[3].isUndefined()) {
            len = length - begin;
        } else {
            if (!ValueToIntegerRange(cx, vp[3], &len))  
                return false;

            if (len <= 0) {
                str = cx->runtime->emptyString;
                goto out;
            }

            if (uint32(length) < uint32(begin + len))
                len = length - begin;
        }

        str = js_NewDependentString(cx, str, size_t(begin), size_t(len));
        if (!str)
            return false;
    }

out:
    vp->setString(str);
    return true;
}
#endif /* JS_HAS_PERL_SUBSTR */

/*
 * Python-esque sequence operations.
 */
static JSBool
str_concat(JSContext *cx, uintN argc, Value *vp)
{
    JSString *str = ThisToStringForStringProto(cx, vp);
    if (!str)
        return false;

    /* Set vp (aka rval) early to handle the argc == 0 case. */
    vp->setString(str);

    Value *argv;
    uintN i;
    for (i = 0, argv = vp + 2; i < argc; i++) {
        JSString *str2 = js_ValueToString(cx, argv[i]);
        if (!str2)
            return false;
        argv[i].setString(str2);

        str = js_ConcatStrings(cx, str, str2);
        if (!str)
            return false;
        vp->setString(str);
    }

    return true;
}

static JSBool
str_slice(JSContext *cx, uintN argc, Value *vp)
{
    if (argc == 1 && vp[1].isString() && vp[2].isInt32()) {
        size_t begin, end, length;

        JSString *str = vp[1].toString();
        begin = vp[2].toInt32();
        end = str->length();
        if (begin <= end) {
            length = end - begin;
            if (length == 0) {
                str = cx->runtime->emptyString;
            } else {
                str = (length == 1)
                      ? JSString::getUnitString(cx, str, begin)
                      : js_NewDependentString(cx, str, begin, length);
                if (!str)
                    return JS_FALSE;
            }
            vp->setString(str);
            return JS_TRUE;
        }
    }

    JSString *str = ThisToStringForStringProto(cx, vp);
    if (!str)
        return false;

    if (argc != 0) {
        double begin, end, length;

        if (!ValueToNumber(cx, vp[2], &begin))
            return JS_FALSE;
        begin = js_DoubleToInteger(begin);
        length = str->length();
        if (begin < 0) {
            begin += length;
            if (begin < 0)
                begin = 0;
        } else if (begin > length) {
            begin = length;
        }

        if (argc == 1 || vp[3].isUndefined()) {
            end = length;
        } else {
            if (!ValueToNumber(cx, vp[3], &end))
                return JS_FALSE;
            end = js_DoubleToInteger(end);
            if (end < 0) {
                end += length;
                if (end < 0)
                    end = 0;
            } else if (end > length) {
                end = length;
            }
            if (end < begin)
                end = begin;
        }

        str = js_NewDependentString(cx, str,
                                    (size_t)begin,
                                    (size_t)(end - begin));
        if (!str)
            return JS_FALSE;
    }
    vp->setString(str);
    return JS_TRUE;
}

#if JS_HAS_STR_HTML_HELPERS
/*
 * HTML composition aids.
 */
static bool
tagify(JSContext *cx, const char *begin, JSLinearString *param, const char *end,
       Value *vp)
{
    JSString *thisstr = ThisToStringForStringProto(cx, vp);
    if (!thisstr)
        return false;
    JSLinearString *str = thisstr->ensureLinear(cx);
    if (!str)
        return false;

    if (!end)
        end = begin;

    size_t beglen = strlen(begin);
    size_t taglen = 1 + beglen + 1;                     /* '<begin' + '>' */
    size_t parlen = 0; /* Avoid warning. */
    if (param) {
        parlen = param->length();
        taglen += 2 + parlen + 1;                       /* '="param"' */
    }
    size_t endlen = strlen(end);
    taglen += str->length() + 2 + endlen + 1;           /* 'str</end>' */

    if (taglen >= ~(size_t)0 / sizeof(jschar)) {
        js_ReportAllocationOverflow(cx);
        return false;
    }

    jschar *tagbuf = (jschar *) cx->malloc((taglen + 1) * sizeof(jschar));
    if (!tagbuf)
        return false;

    size_t j = 0;
    tagbuf[j++] = '<';
    for (size_t i = 0; i < beglen; i++)
        tagbuf[j++] = (jschar)begin[i];
    if (param) {
        tagbuf[j++] = '=';
        tagbuf[j++] = '"';
        js_strncpy(&tagbuf[j], param->chars(), parlen);
        j += parlen;
        tagbuf[j++] = '"';
    }
    tagbuf[j++] = '>';

    js_strncpy(&tagbuf[j], str->chars(), str->length());
    j += str->length();
    tagbuf[j++] = '<';
    tagbuf[j++] = '/';
    for (size_t i = 0; i < endlen; i++)
        tagbuf[j++] = (jschar)end[i];
    tagbuf[j++] = '>';
    JS_ASSERT(j == taglen);
    tagbuf[j] = 0;

    JSString *retstr = js_NewString(cx, tagbuf, taglen);
    if (!retstr) {
        js_free((char *)tagbuf);
        return false;
    }
    vp->setString(retstr);
    return true;
}

static JSBool
tagify_value(JSContext *cx, uintN argc, Value *vp,
             const char *begin, const char *end)
{
    JSLinearString *param = ArgToRootedString(cx, argc, vp, 0);
    if (!param)
        return JS_FALSE;
    return tagify(cx, begin, param, end, vp);
}

static JSBool
str_bold(JSContext *cx, uintN argc, Value *vp)
{
    return tagify(cx, "b", NULL, NULL, vp);
}

static JSBool
str_italics(JSContext *cx, uintN argc, Value *vp)
{
    return tagify(cx, "i", NULL, NULL, vp);
}

static JSBool
str_fixed(JSContext *cx, uintN argc, Value *vp)
{
    return tagify(cx, "tt", NULL, NULL, vp);
}

static JSBool
str_fontsize(JSContext *cx, uintN argc, Value *vp)
{
    return tagify_value(cx, argc, vp, "font size", "font");
}

static JSBool
str_fontcolor(JSContext *cx, uintN argc, Value *vp)
{
    return tagify_value(cx, argc, vp, "font color", "font");
}

static JSBool
str_link(JSContext *cx, uintN argc, Value *vp)
{
    return tagify_value(cx, argc, vp, "a href", "a");
}

static JSBool
str_anchor(JSContext *cx, uintN argc, Value *vp)
{
    return tagify_value(cx, argc, vp, "a name", "a");
}

static JSBool
str_strike(JSContext *cx, uintN argc, Value *vp)
{
    return tagify(cx, "strike", NULL, NULL, vp);
}

static JSBool
str_small(JSContext *cx, uintN argc, Value *vp)
{
    return tagify(cx, "small", NULL, NULL, vp);
}

static JSBool
str_big(JSContext *cx, uintN argc, Value *vp)
{
    return tagify(cx, "big", NULL, NULL, vp);
}

static JSBool
str_blink(JSContext *cx, uintN argc, Value *vp)
{
    return tagify(cx, "blink", NULL, NULL, vp);
}

static JSBool
str_sup(JSContext *cx, uintN argc, Value *vp)
{
    return tagify(cx, "sup", NULL, NULL, vp);
}

static JSBool
str_sub(JSContext *cx, uintN argc, Value *vp)
{
    return tagify(cx, "sub", NULL, NULL, vp);
}
#endif /* JS_HAS_STR_HTML_HELPERS */

#ifdef JS_TRACER
JSString* FASTCALL
js_String_getelem(JSContext* cx, JSString* str, int32 i)
{
    if ((size_t)i >= str->length())
        return NULL;
    return JSString::getUnitString(cx, str, size_t(i));
}
#endif

JS_DEFINE_TRCINFO_1(str_concat,
    (3, (extern, STRING_RETRY, js_ConcatStrings, CONTEXT, THIS_STRING, STRING,
         1, nanojit::ACCSET_NONE)))

static JSFunctionSpec string_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN("quote",             str_quote,             0,JSFUN_GENERIC_NATIVE),
    JS_FN(js_toSource_str,     str_toSource,          0,0),
#endif

    /* Java-like methods. */
    JS_FN(js_toString_str,     js_str_toString,       0,0),
    JS_FN(js_valueOf_str,      js_str_toString,       0,0),
    JS_FN(js_toJSON_str,       js_str_toString,       0,0),
    JS_FN("substring",         str_substring,         2,JSFUN_GENERIC_NATIVE),
    JS_FN("toLowerCase",       str_toLowerCase,       0,JSFUN_GENERIC_NATIVE),
    JS_FN("toUpperCase",       str_toUpperCase,       0,JSFUN_GENERIC_NATIVE),
    JS_FN("charAt",            js_str_charAt,         1,JSFUN_GENERIC_NATIVE),
    JS_FN("charCodeAt",        js_str_charCodeAt,     1,JSFUN_GENERIC_NATIVE),
    JS_FN("indexOf",           str_indexOf,           1,JSFUN_GENERIC_NATIVE),
    JS_FN("lastIndexOf",       str_lastIndexOf,       1,JSFUN_GENERIC_NATIVE),
    JS_FN("trim",              str_trim,              0,JSFUN_GENERIC_NATIVE),
    JS_FN("trimLeft",          str_trimLeft,          0,JSFUN_GENERIC_NATIVE),
    JS_FN("trimRight",         str_trimRight,         0,JSFUN_GENERIC_NATIVE),
    JS_FN("toLocaleLowerCase", str_toLocaleLowerCase, 0,JSFUN_GENERIC_NATIVE),
    JS_FN("toLocaleUpperCase", str_toLocaleUpperCase, 0,JSFUN_GENERIC_NATIVE),
    JS_FN("localeCompare",     str_localeCompare,     1,JSFUN_GENERIC_NATIVE),

    /* Perl-ish methods (search is actually Python-esque). */
    JS_FN("match",             str_match,             1,JSFUN_GENERIC_NATIVE),
    JS_FN("search",            str_search,            1,JSFUN_GENERIC_NATIVE),
    JS_FN("replace",           str_replace,           2,JSFUN_GENERIC_NATIVE),
    JS_FN("split",             str_split,             2,JSFUN_GENERIC_NATIVE),
#if JS_HAS_PERL_SUBSTR
    JS_FN("substr",            str_substr,            2,JSFUN_GENERIC_NATIVE),
#endif

    /* Python-esque sequence methods. */
    JS_TN("concat",            str_concat,            1,JSFUN_GENERIC_NATIVE, &str_concat_trcinfo),
    JS_FN("slice",             str_slice,             2,JSFUN_GENERIC_NATIVE),

    /* HTML string methods. */
#if JS_HAS_STR_HTML_HELPERS
    JS_FN("bold",              str_bold,              0,0),
    JS_FN("italics",           str_italics,           0,0),
    JS_FN("fixed",             str_fixed,             0,0),
    JS_FN("fontsize",          str_fontsize,          1,0),
    JS_FN("fontcolor",         str_fontcolor,         1,0),
    JS_FN("link",              str_link,              1,0),
    JS_FN("anchor",            str_anchor,            1,0),
    JS_FN("strike",            str_strike,            0,0),
    JS_FN("small",             str_small,             0,0),
    JS_FN("big",               str_big,               0,0),
    JS_FN("blink",             str_blink,             0,0),
    JS_FN("sup",               str_sup,               0,0),
    JS_FN("sub",               str_sub,               0,0),
#endif

    JS_FS_END
};

/*
 * Set up some tools to make it easier to generate large tables. After constant
 * folding, for each n, Rn(0) is the comma-separated list R(0), R(1), ..., R(2^n-1).
 * Similary, Rn(k) (for any k and n) generates the list R(k), R(k+1), ..., R(k+2^n-1).
 * To use this, define R appropriately, then use Rn(0) (for some value of n), then
 * undefine R.
 */
#define R2(n)  R(n),   R((n) + (1 << 0)),    R((n) + (2 << 0)),    R((n) + (3 << 0))
#define R4(n)  R2(n),  R2((n) + (1 << 2)),   R2((n) + (2 << 2)),   R2((n) + (3 << 2))
#define R6(n)  R4(n),  R4((n) + (1 << 4)),   R4((n) + (2 << 4)),   R4((n) + (3 << 4))
#define R8(n)  R6(n),  R6((n) + (1 << 6)),   R6((n) + (2 << 6)),   R6((n) + (3 << 6))
#define R10(n) R8(n),  R8((n) + (1 << 8)),   R8((n) + (2 << 8)),   R8((n) + (3 << 8))
#define R12(n) R10(n), R10((n) + (1 << 10)), R10((n) + (2 << 10)), R10((n) + (3 << 10))

#define R3(n) R2(n), R2((n) + (1 << 2))
#define R7(n) R6(n), R6((n) + (1 << 6))

#define BUILD_LENGTH_AND_FLAGS(length, flags)                                 \
    (((length) << JSString::LENGTH_SHIFT) | (flags))

/*
 * Declare unit strings. Pack the string data itself into the mInlineChars
 * place in the header.
 */
#define R(c) {                                                                \
    BUILD_LENGTH_AND_FLAGS(1, JSString::FLAT | JSString::ATOMIZED),           \
    { (jschar *)(((char *)(unitStringTable + (c))) +                          \
      offsetof(JSString, inlineStorage)) },                                   \
    { {(c), 0x00} } }

#ifdef __SUNPRO_CC
#pragma pack(8)
#else
#pragma pack(push, 8)
#endif

const JSString JSString::unitStringTable[]
#ifdef __GNUC__
__attribute__ ((aligned (8)))
#endif
= { R8(0) };

#ifdef __SUNPRO_CC
#pragma pack(0)
#else
#pragma pack(pop)
#endif

#undef R

/*
 * Declare length-2 strings. We only store strings where both characters are
 * alphanumeric. The lower 10 short chars are the numerals, the next 26 are
 * the lowercase letters, and the next 26 are the uppercase letters.
 */
#define TO_SMALL_CHAR(c) ((c) >= '0' && (c) <= '9' ? (c) - '0' :              \
                          (c) >= 'a' && (c) <= 'z' ? (c) - 'a' + 10 :         \
                          (c) >= 'A' && (c) <= 'Z' ? (c) - 'A' + 36 :         \
                          JSString::INVALID_SMALL_CHAR)

#define R TO_SMALL_CHAR

const JSString::SmallChar JSString::toSmallChar[] = { R7(0) };

#undef R

/*
 * This is used when we generate our table of short strings, so the compiler is
 * happier if we use |c| as few times as possible.
 */
#define FROM_SMALL_CHAR(c) ((c) + ((c) < 10 ? '0' :      \
                                   (c) < 36 ? 'a' - 10 : \
                                   'A' - 36))
#define R FROM_SMALL_CHAR

const jschar JSString::fromSmallChar[] = { R6(0) };

#undef R

/*
 * For code-generation ease, length-2 strings are encoded as 12-bit int values,
 * where the upper 6 bits is the first character and the lower 6 bits is the
 * second character.
 */
#define R(c) {                                                                \
    BUILD_LENGTH_AND_FLAGS(2, JSString::FLAT | JSString::ATOMIZED),           \
    { (jschar *)(((char *)(length2StringTable + (c))) +                       \
      offsetof(JSString, inlineStorage)) },                                   \
    { {FROM_SMALL_CHAR((c) >> 6), FROM_SMALL_CHAR((c) & 0x3F), 0x00} } }

#ifdef __SUNPRO_CC
#pragma pack(8)
#else
#pragma pack(push, 8)
#endif

const JSString JSString::length2StringTable[]
#ifdef __GNUC__
__attribute__ ((aligned (8)))
#endif
= { R12(0) };

#ifdef __SUNPRO_CC
#pragma pack(0)
#else
#pragma pack(pop)
#endif

#undef R

/*
 * Declare int strings. Only int strings from 100 to 255 actually have to be
 * generated, since the rest are either unit strings or length-2 strings. To
 * avoid the runtime cost of figuring out where to look for the string for a
 * particular integer, we precompute a table of JSString*s which refer to the
 * correct location of the int string.
 */
#define R(c) {                                                                \
    BUILD_LENGTH_AND_FLAGS(3, JSString::FLAT | JSString::ATOMIZED),           \
    { (jschar *)(((char *)(hundredStringTable + ((c) - 100))) +               \
      offsetof(JSString, inlineStorage)) },                                   \
    { {((c) / 100) + '0', ((c) / 10 % 10) + '0', ((c) % 10) + '0', 0x00} } }


JS_STATIC_ASSERT(100 + (1 << 7) + (1 << 4) + (1 << 3) + (1 << 2) == 256);

#ifdef __SUNPRO_CC
#pragma pack(8)
#else
#pragma pack(push, 8)
#endif

const JSString JSString::hundredStringTable[]
#ifdef __GNUC__
__attribute__ ((aligned (8)))
#endif
= { R7(100), /* 100 through 227 */
    R4(100 + (1 << 7)), /* 228 through 243 */
    R3(100 + (1 << 7) + (1 << 4)), /* 244 through 251 */
    R2(100 + (1 << 7) + (1 << 4) + (1 << 3)) /* 252 through 255 */
};

#undef R

#define R(c) ((c) < 10 ? JSString::unitStringTable + ((c) + '0') :            \
              (c) < 100 ? JSString::length2StringTable +                      \
              ((size_t)TO_SMALL_CHAR(((c) / 10) + '0') << 6) +                \
              TO_SMALL_CHAR(((c) % 10) + '0') :                               \
              JSString::hundredStringTable + ((c) - 100))

const JSString *const JSString::intStringTable[] = { R8(0) };

#undef R

#ifdef __SUNPRO_CC
#pragma pack(0)
#else
#pragma pack(pop)
#endif

#undef R2
#undef R4
#undef R6
#undef R8
#undef R10
#undef R12

#undef R3
#undef R7

JSBool
js_String(JSContext *cx, uintN argc, Value *vp)
{
    Value *argv = vp + 2;

    JSString *str;
    if (argc > 0) {
        str = js_ValueToString(cx, argv[0]);
        if (!str)
            return false;
    } else {
        str = cx->runtime->emptyString;
    }

    if (IsConstructing(vp)) {
        JSObject *obj = NewBuiltinClassInstance(cx, &js_StringClass);
        if (!obj)
            return false;
        obj->setPrimitiveThis(StringValue(str));
        vp->setObject(*obj);
    } else {
        vp->setString(str);
    }
    return true;
}

static JSBool
str_fromCharCode(JSContext *cx, uintN argc, Value *vp)
{
    Value *argv;
    uintN i;
    jschar *chars;
    JSString *str;

    argv = vp + 2;
    JS_ASSERT(argc <= JS_ARGS_LENGTH_MAX);
    if (argc == 1) {
        uint16_t code;
        if (!ValueToUint16(cx, argv[0], &code))
            return JS_FALSE;
        if (code < UNIT_STRING_LIMIT) {
            str = JSString::unitString(code);
            if (!str)
                return JS_FALSE;
            vp->setString(str);
            return JS_TRUE;
        }
        argv[0].setInt32(code);
    }
    chars = (jschar *) cx->malloc((argc + 1) * sizeof(jschar));
    if (!chars)
        return JS_FALSE;
    for (i = 0; i < argc; i++) {
        uint16_t code;
        if (!ValueToUint16(cx, argv[i], &code)) {
            cx->free(chars);
            return JS_FALSE;
        }
        chars[i] = (jschar)code;
    }
    chars[i] = 0;
    str = js_NewString(cx, chars, argc);
    if (!str) {
        cx->free(chars);
        return JS_FALSE;
    }
    vp->setString(str);
    return JS_TRUE;
}

#ifdef JS_TRACER
static JSString* FASTCALL
String_fromCharCode(JSContext* cx, int32 i)
{
    JS_ASSERT(JS_ON_TRACE(cx));
    jschar c = (jschar)i;
    if (c < UNIT_STRING_LIMIT)
        return JSString::unitString(c);
    return js_NewStringCopyN(cx, &c, 1);
}
#endif

JS_DEFINE_TRCINFO_1(str_fromCharCode,
    (2, (static, STRING_RETRY, String_fromCharCode, CONTEXT, INT32, 1, nanojit::ACCSET_NONE)))

static JSFunctionSpec string_static_methods[] = {
    JS_TN("fromCharCode", str_fromCharCode, 1, 0, &str_fromCharCode_trcinfo),
    JS_FS_END
};

JSObject *
js_InitStringClass(JSContext *cx, JSObject *obj)
{
    JSObject *proto;

    /* Define the escape, unescape functions in the global object. */
    if (!JS_DefineFunctions(cx, obj, string_functions))
        return NULL;

    proto = js_InitClass(cx, obj, NULL, &js_StringClass, js_String, 1,
                         NULL, string_methods,
                         NULL, string_static_methods);
    if (!proto)
        return NULL;
    proto->setPrimitiveThis(StringValue(cx->runtime->emptyString));
    if (!js_DefineNativeProperty(cx, proto, ATOM_TO_JSID(cx->runtime->atomState.lengthAtom),
                                 UndefinedValue(), NULL, NULL,
                                 JSPROP_READONLY | JSPROP_PERMANENT | JSPROP_SHARED, 0, 0,
                                 NULL)) {
        return JS_FALSE;
    }

    return proto;
}

JSFlatString *
js_NewString(JSContext *cx, jschar *chars, size_t length)
{
    JSString *str;

    if (length > JSString::MAX_LENGTH) {
        if (JS_ON_TRACE(cx)) {
            /*
             * If we can't leave the trace, signal OOM condition, otherwise
             * exit from trace before throwing.
             */
            if (!CanLeaveTrace(cx))
                return NULL;

            LeaveTrace(cx);
        }
        js_ReportAllocationOverflow(cx);
        return NULL;
    }

    str = js_NewGCString(cx);
    if (!str)
        return NULL;
    str->initFlat(chars, length);
    cx->runtime->stringMemoryUsed += length * 2;
#ifdef DEBUG
  {
    JSRuntime *rt = cx->runtime;
    JS_RUNTIME_METER(rt, liveStrings);
    JS_RUNTIME_METER(rt, totalStrings);
    JS_LOCK_RUNTIME_VOID(rt,
        (rt->lengthSum += (double)length,
         rt->lengthSquaredSum += (double)length * (double)length));
  }
#endif
    return str->assertIsFlat();
}

static JS_ALWAYS_INLINE JSFlatString *
NewShortString(JSContext *cx, const jschar *chars, size_t length)
{
    JS_ASSERT(JSShortString::fitsIntoShortString(length));
    JSShortString *str = js_NewGCShortString(cx);
    if (!str)
        return NULL;
    jschar *storage = str->init(length);
    js_short_strncpy(storage, chars, length);
    storage[length] = 0;
    return str->header()->assertIsFlat();
}

static JSFlatString *
NewShortString(JSContext *cx, const char *chars, size_t length)
{
    JS_ASSERT(JSShortString::fitsIntoShortString(length));
    JSShortString *str = js_NewGCShortString(cx);
    if (!str)
        return NULL;
    jschar *storage = str->init(length);

    if (js_CStringsAreUTF8) {
#ifdef DEBUG
        size_t oldLength = length;
#endif
        if (!js_InflateStringToBuffer(cx, chars, length, storage, &length))
            return NULL;
        JS_ASSERT(length <= oldLength);
        storage[length] = 0;
        str->resetLength(length);
    } else {
        size_t n = length;
        jschar *p = storage;
        while (n--)
            *p++ = (unsigned char)*chars++;
        *p = 0;
    }
    return str->header()->assertIsFlat();
}

static const size_t sMinWasteSize = 16;

JSFlatString *
StringBuffer::finishString()
{
    JSContext *cx = context();
    if (cb.empty())
        return ATOM_TO_STRING(cx->runtime->atomState.emptyAtom);

    size_t length = cb.length();
    if (!checkLength(length))
        return NULL;

    JS_STATIC_ASSERT(JSShortString::MAX_SHORT_STRING_LENGTH < CharBuffer::InlineLength);
    if (JSShortString::fitsIntoShortString(length))
        return NewShortString(cx, cb.begin(), length);

    if (!cb.append('\0'))
        return NULL;

    size_t capacity = cb.capacity();

    jschar *buf = cb.extractRawBuffer();
    if (!buf)
        return NULL;

    /* For medium/big buffers, avoid wasting more than 1/4 of the memory. */
    JS_ASSERT(capacity >= length);
    if (capacity > sMinWasteSize && capacity - length > (length >> 2)) {
        size_t bytes = sizeof(jschar) * (length + 1);
        jschar *tmp = (jschar *)cx->realloc(buf, bytes);
        if (!tmp) {
            cx->free(buf);
            return NULL;
        }
        buf = tmp;
    }

    JSFlatString *str = js_NewString(cx, buf, length);
    if (!str)
        cx->free(buf);
    return str;
}

JSLinearString *
js_NewDependentString(JSContext *cx, JSString *baseArg, size_t start,
                      size_t length)
{
    JSString *ds;

    if (length == 0)
        return cx->runtime->emptyString;

    JSLinearString *base = baseArg->ensureLinear(cx);
    if (!base)
        return NULL;

    if (start == 0 && length == base->length())
        return base;

    const jschar *chars = base->chars() + start;

    JSLinearString *staticStr = JSString::lookupStaticString(chars, length);
    if (staticStr)
        return staticStr;

    /* Try to avoid long chains of dependent strings. */
    while (base->isDependent())
        base = base->dependentBase();

    JS_ASSERT(base->isFlat());

    ds = js_NewGCString(cx);
    if (!ds)
        return NULL;
    ds->initDependent(base, chars, length);
#ifdef DEBUG
  {
    JSRuntime *rt = cx->runtime;
    JS_RUNTIME_METER(rt, liveDependentStrings);
    JS_RUNTIME_METER(rt, totalDependentStrings);
    JS_RUNTIME_METER(rt, liveStrings);
    JS_RUNTIME_METER(rt, totalStrings);
    JS_LOCK_RUNTIME_VOID(rt,
        (rt->strdepLengthSum += (double)length,
         rt->strdepLengthSquaredSum += (double)length * (double)length));
    JS_LOCK_RUNTIME_VOID(rt,
        (rt->lengthSum += (double)length,
         rt->lengthSquaredSum += (double)length * (double)length));
  }
#endif
    return ds->assertIsLinear();
}

#ifdef DEBUG
#include <math.h>

void printJSStringStats(JSRuntime *rt)
{
    double mean, sigma;

    mean = JS_MeanAndStdDev(rt->totalStrings, rt->lengthSum,
                            rt->lengthSquaredSum, &sigma);

    fprintf(stderr, "%lu total strings, mean length %g (sigma %g)\n",
            (unsigned long)rt->totalStrings, mean, sigma);

    mean = JS_MeanAndStdDev(rt->totalDependentStrings, rt->strdepLengthSum,
                            rt->strdepLengthSquaredSum, &sigma);

    fprintf(stderr, "%lu total dependent strings, mean length %g (sigma %g)\n",
            (unsigned long)rt->totalDependentStrings, mean, sigma);
}
#endif

JSFlatString *
js_NewStringCopyN(JSContext *cx, const jschar *s, size_t n)
{
    if (JSShortString::fitsIntoShortString(n))
        return NewShortString(cx, s, n);

    jschar *news = (jschar *) cx->malloc((n + 1) * sizeof(jschar));
    if (!news)
        return NULL;
    js_strncpy(news, s, n);
    news[n] = 0;
    JSFlatString *str = js_NewString(cx, news, n);
    if (!str)
        cx->free(news);
    return str;
}

JSFlatString *
js_NewStringCopyN(JSContext *cx, const char *s, size_t n)
{
    if (JSShortString::fitsIntoShortString(n))
        return NewShortString(cx, s, n);

    jschar *chars = js_InflateString(cx, s, &n);
    if (!chars)
        return NULL;
    JSFlatString *str = js_NewString(cx, chars, n);
    if (!str)
        cx->free(chars);
    return str;
}

JSFlatString *
js_NewStringCopyZ(JSContext *cx, const jschar *s)
{
    size_t n = js_strlen(s);
    if (JSShortString::fitsIntoShortString(n))
        return NewShortString(cx, s, n);

    size_t m = (n + 1) * sizeof(jschar);
    jschar *news = (jschar *) cx->malloc(m);
    if (!news)
        return NULL;
    memcpy(news, s, m);
    JSFlatString *str = js_NewString(cx, news, n);
    if (!str)
        cx->free(news);
    return str;
}

JSFlatString *
js_NewStringCopyZ(JSContext *cx, const char *s)
{
    return js_NewStringCopyN(cx, s, strlen(s));
}

const char *
js_ValueToPrintable(JSContext *cx, const Value &v, JSAutoByteString *bytes, bool asSource)
{
    JSString *str;

    str = (asSource ? js_ValueToSource : js_ValueToString)(cx, v);
    if (!str)
        return NULL;
    str = js_QuoteString(cx, str, 0);
    if (!str)
        return NULL;
    return bytes->encode(cx, str);
}

JSString *
js_ValueToString(JSContext *cx, const Value &arg)
{
    Value v = arg;
    if (v.isObject() && !DefaultValue(cx, &v.toObject(), JSTYPE_STRING, &v))
        return NULL;

    JSString *str;
    if (v.isString()) {
        str = v.toString();
    } else if (v.isInt32()) {
        str = js_IntToString(cx, v.toInt32());
    } else if (v.isDouble()) {
        str = js_NumberToString(cx, v.toDouble());
    } else if (v.isBoolean()) {
        str = js_BooleanToString(cx, v.toBoolean());
    } else if (v.isNull()) {
        str = ATOM_TO_STRING(cx->runtime->atomState.nullAtom);
    } else {
        str = ATOM_TO_STRING(cx->runtime->atomState.typeAtoms[JSTYPE_VOID]);
    }
    return str;
}

/* This function implements E-262-3 section 9.8, toString. */
bool
js::ValueToStringBuffer(JSContext *cx, const Value &arg, StringBuffer &sb)
{
    Value v = arg;
    if (v.isObject() && !DefaultValue(cx, &v.toObject(), JSTYPE_STRING, &v))
        return false;

    if (v.isString()) {
        JSString *str = v.toString();
        size_t length = str->length();
        const jschar *chars = str->getChars(cx);
        if (!chars)
            return false;
        return sb.append(chars, length);
    }
    if (v.isNumber())
        return NumberValueToStringBuffer(cx, v, sb);
    if (v.isBoolean())
        return BooleanToStringBuffer(cx, v.toBoolean(), sb);
    if (v.isNull())
        return sb.append(cx->runtime->atomState.nullAtom);
    JS_ASSERT(v.isUndefined());
    return sb.append(cx->runtime->atomState.typeAtoms[JSTYPE_VOID]);
}

JS_FRIEND_API(JSString *)
js_ValueToSource(JSContext *cx, const Value &v)
{
    if (v.isUndefined())
        return ATOM_TO_STRING(cx->runtime->atomState.void0Atom);
    if (v.isString())
        return js_QuoteString(cx, v.toString(), '"');
    if (v.isPrimitive()) {
        /* Special case to preserve negative zero, _contra_ toString. */
        if (v.isDouble() && JSDOUBLE_IS_NEGZERO(v.toDouble())) {
            /* NB: _ucNstr rather than _ucstr to indicate non-terminated. */
            static const jschar js_negzero_ucNstr[] = {'-', '0'};

            return js_NewStringCopyN(cx, js_negzero_ucNstr, 2);
        }
        return js_ValueToString(cx, v);
    }

    JSAtom *atom = cx->runtime->atomState.toSourceAtom;
    AutoValueRooter tvr(cx);
    if (!js_TryMethod(cx, &v.toObject(), atom, 0, NULL, tvr.addr()))
        return NULL;
    return js_ValueToString(cx, tvr.value());
}

namespace js {

/*
 * str is not necessarily a GC thing here.
 */
static JS_ALWAYS_INLINE bool
EqualStringsTail(JSLinearString *str1, size_t length1, JSLinearString *str2)
{
    const jschar *s1 = str1->chars();
    const jschar *s1end = s1 + length1;
    const jschar *s2 = str2->chars();
    do {
        if (*s1 != *s2) {
            return false;
        }
        ++s1, ++s2;
    } while (s1 != s1end);

    return true;
}

bool
EqualStrings(JSContext *cx, JSString *str1, JSString *str2, JSBool *result)
{
    if (str1 == str2) {
        *result = true;
        return true;
    }

    size_t length1 = str1->length();
    if (length1 != str2->length()) {
        *result = false;
        return true;
    }

    if (length1 == 0) {
        *result = true;
        return true;
    }

    JSLinearString *linear1 = str1->ensureLinear(cx);
    if (!linear1)
        return false;
    JSLinearString *linear2 = str2->ensureLinear(cx);
    if (!linear2)
        return false;

    *result = EqualStringsTail(linear1, length1, linear2);
    return true;
}

bool
EqualStrings(JSLinearString *str1, JSLinearString *str2)
{
    if (str1 == str2)
        return true;

    size_t length1 = str1->length();
    if (length1 != str2->length())
        return false;

    if (length1 == 0)
        return true;

    return EqualStringsTail(str1, length1, str2);
}

}  /* namespace js */

JSBool JS_FASTCALL
js_EqualStringsOnTrace(JSContext *cx, JSString *str1, JSString *str2)
{
    JSBool result;
    return EqualStrings(cx, str1, str2, &result) ? result : JS_NEITHER;
}
JS_DEFINE_CALLINFO_3(extern, BOOL, js_EqualStringsOnTrace,
                     CONTEXT, STRING, STRING, 1, nanojit::ACCSET_NONE)

namespace js {

static bool
CompareStringsImpl(JSContext *cx, JSString *str1, JSString *str2, int32 *result)
{
    JS_ASSERT(str1);
    JS_ASSERT(str2);

    if (str1 == str2) {
        *result = 0;
        return true;
    }

    size_t l1 = str1->length();
    const jschar *s1 = str1->getChars(cx);
    if (!s1)
        return false;

    size_t l2 = str2->length();
    const jschar *s2 = str2->getChars(cx);
    if (!s2)
        return false;

    size_t n = JS_MIN(l1, l2);
    for (size_t i = 0; i < n; i++) {
        if (int32 cmp = s1[i] - s2[i]) {
            *result = cmp;
            return true;
        }
    }
    *result = (int32)(l1 - l2);
    return true;
}

bool
CompareStrings(JSContext *cx, JSString *str1, JSString *str2, int32 *result)
{
    return CompareStringsImpl(cx, str1, str2, result);
}

}  /* namespace js */

int32 JS_FASTCALL
js_CompareStringsOnTrace(JSContext *cx, JSString *str1, JSString *str2)
{
    int32 result;
    if (!CompareStringsImpl(cx, str1, str2, &result))
        return INT32_MIN;
    JS_ASSERT(result != INT32_MIN);
    return result;
}
JS_DEFINE_CALLINFO_3(extern, INT32, js_CompareStringsOnTrace,
                     CONTEXT, STRING, STRING, 1, nanojit::ACCSET_NONE)

namespace js {

bool
StringEqualsAscii(JSLinearString *str, const char *asciiBytes)
{
    size_t length = strlen(asciiBytes);
#ifdef DEBUG
    for (size_t i = 0; i != length; ++i)
        JS_ASSERT(unsigned(asciiBytes[i]) <= 127);
#endif
    if (length != str->length())
        return false;
    const jschar *chars = str->chars();
    for (size_t i = 0; i != length; ++i) {
        if (unsigned(asciiBytes[i]) != unsigned(chars[i]))
            return false;
    }
    return true;
}

} /* namespacejs */

size_t
js_strlen(const jschar *s)
{
    const jschar *t;

    for (t = s; *t != 0; t++)
        continue;
    return (size_t)(t - s);
}

jschar *
js_strchr(const jschar *s, jschar c)
{
    while (*s != 0) {
        if (*s == c)
            return (jschar *)s;
        s++;
    }
    return NULL;
}

jschar *
js_strchr_limit(const jschar *s, jschar c, const jschar *limit)
{
    while (s < limit) {
        if (*s == c)
            return (jschar *)s;
        s++;
    }
    return NULL;
}

jschar *
js_InflateString(JSContext *cx, const char *bytes, size_t *lengthp)
{
    size_t nbytes, nchars, i;
    jschar *chars;
#ifdef DEBUG
    JSBool ok;
#endif

    nbytes = *lengthp;
    if (js_CStringsAreUTF8) {
        if (!js_InflateStringToBuffer(cx, bytes, nbytes, NULL, &nchars))
            goto bad;
        chars = (jschar *) cx->malloc((nchars + 1) * sizeof (jschar));
        if (!chars)
            goto bad;
#ifdef DEBUG
        ok =
#endif
            js_InflateStringToBuffer(cx, bytes, nbytes, chars, &nchars);
        JS_ASSERT(ok);
    } else {
        nchars = nbytes;
        chars = (jschar *) cx->malloc((nchars + 1) * sizeof(jschar));
        if (!chars)
            goto bad;
        for (i = 0; i < nchars; i++)
            chars[i] = (unsigned char) bytes[i];
    }
    *lengthp = nchars;
    chars[nchars] = 0;
    return chars;

  bad:
    /*
     * For compatibility with callers of JS_DecodeBytes we must zero lengthp
     * on errors.
     */
    *lengthp = 0;
    return NULL;
}

/*
 * May be called with null cx.
 */
char *
js_DeflateString(JSContext *cx, const jschar *chars, size_t nchars)
{
    size_t nbytes, i;
    char *bytes;
#ifdef DEBUG
    JSBool ok;
#endif

    if (js_CStringsAreUTF8) {
        nbytes = js_GetDeflatedStringLength(cx, chars, nchars);
        if (nbytes == (size_t) -1)
            return NULL;
        bytes = (char *) (cx ? cx->malloc(nbytes + 1) : js_malloc(nbytes + 1));
        if (!bytes)
            return NULL;
#ifdef DEBUG
        ok =
#endif
            js_DeflateStringToBuffer(cx, chars, nchars, bytes, &nbytes);
        JS_ASSERT(ok);
    } else {
        nbytes = nchars;
        bytes = (char *) (cx ? cx->malloc(nbytes + 1) : js_malloc(nbytes + 1));
        if (!bytes)
            return NULL;
        for (i = 0; i < nbytes; i++)
            bytes[i] = (char) chars[i];
    }
    bytes[nbytes] = 0;
    return bytes;
}

size_t
js_GetDeflatedStringLength(JSContext *cx, const jschar *chars, size_t nchars)
{
    if (!js_CStringsAreUTF8)
        return nchars;

    return js_GetDeflatedUTF8StringLength(cx, chars, nchars);
}

/*
 * May be called with null cx through public API, see below.
 */
size_t
js_GetDeflatedUTF8StringLength(JSContext *cx, const jschar *chars, size_t nchars)
{
    size_t nbytes;
    const jschar *end;
    uintN c, c2;
    char buffer[10];

    nbytes = nchars;
    for (end = chars + nchars; chars != end; chars++) {
        c = *chars;
        if (c < 0x80)
            continue;
        if (0xD800 <= c && c <= 0xDFFF) {
            /* Surrogate pair. */
            chars++;

            /* nbytes sets 1 length since this is surrogate pair. */
            nbytes--;
            if (c >= 0xDC00 || chars == end)
                goto bad_surrogate;
            c2 = *chars;
            if (c2 < 0xDC00 || c2 > 0xDFFF)
                goto bad_surrogate;
            c = ((c - 0xD800) << 10) + (c2 - 0xDC00) + 0x10000;
        }
        c >>= 11;
        nbytes++;
        while (c) {
            c >>= 5;
            nbytes++;
        }
    }
    return nbytes;

  bad_surrogate:
    if (cx) {
        JS_snprintf(buffer, 10, "0x%x", c);
        JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR, js_GetErrorMessage,
                                     NULL, JSMSG_BAD_SURROGATE_CHAR, buffer);
    }
    return (size_t) -1;
}

JSBool
js_DeflateStringToBuffer(JSContext *cx, const jschar *src, size_t srclen,
                         char *dst, size_t *dstlenp)
{
    size_t dstlen, i;

    dstlen = *dstlenp;
    if (!js_CStringsAreUTF8) {
        if (srclen > dstlen) {
            for (i = 0; i < dstlen; i++)
                dst[i] = (char) src[i];
            if (cx) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_BUFFER_TOO_SMALL);
            }
            return JS_FALSE;
        }
        for (i = 0; i < srclen; i++)
            dst[i] = (char) src[i];
        *dstlenp = srclen;
        return JS_TRUE;
    }

    return js_DeflateStringToUTF8Buffer(cx, src, srclen, dst, dstlenp);
}

JSBool
js_DeflateStringToUTF8Buffer(JSContext *cx, const jschar *src, size_t srclen,
                             char *dst, size_t *dstlenp)
{
    size_t dstlen, i, origDstlen, utf8Len;
    jschar c, c2;
    uint32 v;
    uint8 utf8buf[6];

    dstlen = *dstlenp;
    origDstlen = dstlen;
    while (srclen) {
        c = *src++;
        srclen--;
        if ((c >= 0xDC00) && (c <= 0xDFFF))
            goto badSurrogate;
        if (c < 0xD800 || c > 0xDBFF) {
            v = c;
        } else {
            if (srclen < 1)
                goto badSurrogate;
            c2 = *src;
            if ((c2 < 0xDC00) || (c2 > 0xDFFF))
                goto badSurrogate;
            src++;
            srclen--;
            v = ((c - 0xD800) << 10) + (c2 - 0xDC00) + 0x10000;
        }
        if (v < 0x0080) {
            /* no encoding necessary - performance hack */
            if (dstlen == 0)
                goto bufferTooSmall;
            *dst++ = (char) v;
            utf8Len = 1;
        } else {
            utf8Len = js_OneUcs4ToUtf8Char(utf8buf, v);
            if (utf8Len > dstlen)
                goto bufferTooSmall;
            for (i = 0; i < utf8Len; i++)
                *dst++ = (char) utf8buf[i];
        }
        dstlen -= utf8Len;
    }
    *dstlenp = (origDstlen - dstlen);
    return JS_TRUE;

badSurrogate:
    *dstlenp = (origDstlen - dstlen);
    /* Delegate error reporting to the measurement function. */
    if (cx)
        js_GetDeflatedStringLength(cx, src - 1, srclen + 1);
    return JS_FALSE;

bufferTooSmall:
    *dstlenp = (origDstlen - dstlen);
    if (cx) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_BUFFER_TOO_SMALL);
    }
    return JS_FALSE;
}

JSBool
js_InflateStringToBuffer(JSContext *cx, const char *src, size_t srclen,
                         jschar *dst, size_t *dstlenp)
{
    size_t dstlen, i;

    if (!js_CStringsAreUTF8) {
        if (dst) {
            dstlen = *dstlenp;
            if (srclen > dstlen) {
                for (i = 0; i < dstlen; i++)
                    dst[i] = (unsigned char) src[i];
                if (cx) {
                    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                         JSMSG_BUFFER_TOO_SMALL);
                }
                return JS_FALSE;
            }
            for (i = 0; i < srclen; i++)
                dst[i] = (unsigned char) src[i];
        }
        *dstlenp = srclen;
        return JS_TRUE;
    }

    return js_InflateUTF8StringToBuffer(cx, src, srclen, dst, dstlenp);
}

JSBool
js_InflateUTF8StringToBuffer(JSContext *cx, const char *src, size_t srclen,
                             jschar *dst, size_t *dstlenp)
{
    size_t dstlen, origDstlen, offset, j, n;
    uint32 v;

    dstlen = dst ? *dstlenp : (size_t) -1;
    origDstlen = dstlen;
    offset = 0;

    while (srclen) {
        v = (uint8) *src;
        n = 1;
        if (v & 0x80) {
            while (v & (0x80 >> n))
                n++;
            if (n > srclen)
                goto bufferTooSmall;
            if (n == 1 || n > 4)
                goto badCharacter;
            for (j = 1; j < n; j++) {
                if ((src[j] & 0xC0) != 0x80)
                    goto badCharacter;
            }
            v = Utf8ToOneUcs4Char((uint8 *)src, n);
            if (v >= 0x10000) {
                v -= 0x10000;
                if (v > 0xFFFFF || dstlen < 2) {
                    *dstlenp = (origDstlen - dstlen);
                    if (cx) {
                        char buffer[10];
                        JS_snprintf(buffer, 10, "0x%x", v + 0x10000);
                        JS_ReportErrorFlagsAndNumber(cx,
                                                     JSREPORT_ERROR,
                                                     js_GetErrorMessage, NULL,
                                                     JSMSG_UTF8_CHAR_TOO_LARGE,
                                                     buffer);
                    }
                    return JS_FALSE;
                }
                if (dst) {
                    *dst++ = (jschar)((v >> 10) + 0xD800);
                    v = (jschar)((v & 0x3FF) + 0xDC00);
                }
                dstlen--;
            }
        }
        if (!dstlen)
            goto bufferTooSmall;
        if (dst)
            *dst++ = (jschar) v;
        dstlen--;
        offset += n;
        src += n;
        srclen -= n;
    }
    *dstlenp = (origDstlen - dstlen);
    return JS_TRUE;

badCharacter:
    *dstlenp = (origDstlen - dstlen);
    if (cx) {
        char buffer[10];
        JS_snprintf(buffer, 10, "%d", offset);
        JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR,
                                     js_GetErrorMessage, NULL,
                                     JSMSG_MALFORMED_UTF8_CHAR,
                                     buffer);
    }
    return JS_FALSE;

bufferTooSmall:
    *dstlenp = (origDstlen - dstlen);
    if (cx) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_BUFFER_TOO_SMALL);
    }
    return JS_FALSE;
}

/*
 * From java.lang.Character.java:
 *
 * The character properties are currently encoded into 32 bits in the
 * following manner:
 *
 * 10 bits      signed offset used for converting case
 *  1 bit       if 1, adding the signed offset converts the character to
 *              lowercase
 *  1 bit       if 1, subtracting the signed offset converts the character to
 *              uppercase
 *  1 bit       if 1, character has a titlecase equivalent (possibly itself)
 *  3 bits      0  may not be part of an identifier
 *              1  ignorable control; may continue a Unicode identifier or JS
 *                 identifier
 *              2  may continue a JS identifier but not a Unicode identifier
 *                 (unused)
 *              3  may continue a Unicode identifier or JS identifier
 *              4  is a JS whitespace character
 *              5  may start or continue a JS identifier;
 *                 may continue but not start a Unicode identifier (_)
 *              6  may start or continue a JS identifier but not a Unicode
 *                 identifier ($)
 *              7  may start or continue a Unicode identifier or JS identifier
 *              Thus:
 *                 5, 6, 7 may start a JS identifier
 *                 1, 2, 3, 5, 6, 7 may continue a JS identifier
 *                 7 may start a Unicode identifier
 *                 1, 3, 5, 7 may continue a Unicode identifier
 *                 1 is ignorable within an identifier
 *                 4 is JS whitespace
 *  2 bits      0  this character has no numeric property
 *              1  adding the digit offset to the character code and then
 *                 masking with 0x1F will produce the desired numeric value
 *              2  this character has a "strange" numeric value
 *              3  a JS supradecimal digit: adding the digit offset to the
 *                 character code, then masking with 0x1F, then adding 10
 *                 will produce the desired numeric value
 *  5 bits      digit offset
 *  1 bit       XML 1.0 name start character
 *  1 bit       XML 1.0 name character
 *  2 bits      reserved for future use
 *  5 bits      character type
 */

/* The X table has 1024 entries for a total of 1024 bytes. */

const uint8 js_X[] = {
  0,   1,   2,   3,   4,   5,   6,   7,  /*  0x0000 */
  8,   9,  10,  11,  12,  13,  14,  15,  /*  0x0200 */
 16,  17,  18,  19,  20,  21,  22,  23,  /*  0x0400 */
 24,  25,  26,  27,  28,  28,  28,  28,  /*  0x0600 */
 28,  28,  28,  28,  29,  30,  31,  32,  /*  0x0800 */
 33,  34,  35,  36,  37,  38,  39,  40,  /*  0x0A00 */
 41,  42,  43,  44,  45,  46,  28,  28,  /*  0x0C00 */
 47,  48,  49,  50,  51,  52,  53,  28,  /*  0x0E00 */
 28,  28,  54,  55,  56,  57,  58,  59,  /*  0x1000 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x1200 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x1400 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x1600 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x1800 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x1A00 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x1C00 */
 60,  60,  61,  62,  63,  64,  65,  66,  /*  0x1E00 */
 67,  68,  69,  70,  71,  72,  73,  74,  /*  0x2000 */
 75,  75,  75,  76,  77,  78,  28,  28,  /*  0x2200 */
 79,  80,  81,  82,  83,  83,  84,  85,  /*  0x2400 */
 86,  85,  28,  28,  87,  88,  89,  28,  /*  0x2600 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x2800 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x2A00 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x2C00 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x2E00 */
 90,  91,  92,  93,  94,  56,  95,  28,  /*  0x3000 */
 96,  97,  98,  99,  83, 100,  83, 101,  /*  0x3200 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x3400 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x3600 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x3800 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x3A00 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x3C00 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x3E00 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x4000 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x4200 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x4400 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x4600 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x4800 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x4A00 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0x4C00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x4E00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x5000 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x5200 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x5400 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x5600 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x5800 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x5A00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x5C00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x5E00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x6000 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x6200 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x6400 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x6600 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x6800 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x6A00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x6C00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x6E00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x7000 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x7200 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x7400 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x7600 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x7800 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x7A00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x7C00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x7E00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x8000 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x8200 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x8400 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x8600 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x8800 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x8A00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x8C00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x8E00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x9000 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x9200 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x9400 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x9600 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x9800 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x9A00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0x9C00 */
 56,  56,  56,  56,  56,  56, 102,  28,  /*  0x9E00 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0xA000 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0xA200 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0xA400 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0xA600 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0xA800 */
 28,  28,  28,  28,  28,  28,  28,  28,  /*  0xAA00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xAC00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xAE00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xB000 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xB200 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xB400 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xB600 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xB800 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xBA00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xBC00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xBE00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xC000 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xC200 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xC400 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xC600 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xC800 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xCA00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xCC00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xCE00 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xD000 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xD200 */
 56,  56,  56,  56,  56,  56,  56,  56,  /*  0xD400 */
 56,  56,  56,  56,  56,  56, 103,  28,  /*  0xD600 */
104, 104, 104, 104, 104, 104, 104, 104,  /*  0xD800 */
104, 104, 104, 104, 104, 104, 104, 104,  /*  0xDA00 */
104, 104, 104, 104, 104, 104, 104, 104,  /*  0xDC00 */
104, 104, 104, 104, 104, 104, 104, 104,  /*  0xDE00 */
105, 105, 105, 105, 105, 105, 105, 105,  /*  0xE000 */
105, 105, 105, 105, 105, 105, 105, 105,  /*  0xE200 */
105, 105, 105, 105, 105, 105, 105, 105,  /*  0xE400 */
105, 105, 105, 105, 105, 105, 105, 105,  /*  0xE600 */
105, 105, 105, 105, 105, 105, 105, 105,  /*  0xE800 */
105, 105, 105, 105, 105, 105, 105, 105,  /*  0xEA00 */
105, 105, 105, 105, 105, 105, 105, 105,  /*  0xEC00 */
105, 105, 105, 105, 105, 105, 105, 105,  /*  0xEE00 */
105, 105, 105, 105, 105, 105, 105, 105,  /*  0xF000 */
105, 105, 105, 105, 105, 105, 105, 105,  /*  0xF200 */
105, 105, 105, 105, 105, 105, 105, 105,  /*  0xF400 */
105, 105, 105, 105, 105, 105, 105, 105,  /*  0xF600 */
105, 105, 105, 105,  56,  56,  56,  56,  /*  0xF800 */
106,  28,  28,  28, 107, 108, 109, 110,  /*  0xFA00 */
 56,  56,  56,  56, 111, 112, 113, 114,  /*  0xFC00 */
115, 116,  56, 117, 118, 119, 120, 121   /*  0xFE00 */
};

/* The Y table has 7808 entries for a total of 7808 bytes. */

const uint8 js_Y[] = {
  0,   0,   0,   0,   0,   0,   0,   0,  /*    0 */
  0,   1,   1,   1,   1,   1,   0,   0,  /*    0 */
  0,   0,   0,   0,   0,   0,   0,   0,  /*    0 */
  0,   0,   0,   0,   0,   0,   0,   0,  /*    0 */
  2,   3,   3,   3,   4,   3,   3,   3,  /*    0 */
  5,   6,   3,   7,   3,   8,   3,   3,  /*    0 */
  9,   9,   9,   9,   9,   9,   9,   9,  /*    0 */
  9,   9,   3,   3,   7,   7,   7,   3,  /*    0 */
  3,  10,  10,  10,  10,  10,  10,  10,  /*    1 */
 10,  10,  10,  10,  10,  10,  10,  10,  /*    1 */
 10,  10,  10,  10,  10,  10,  10,  10,  /*    1 */
 10,  10,  10,   5,   3,   6,  11,  12,  /*    1 */
 11,  13,  13,  13,  13,  13,  13,  13,  /*    1 */
 13,  13,  13,  13,  13,  13,  13,  13,  /*    1 */
 13,  13,  13,  13,  13,  13,  13,  13,  /*    1 */
 13,  13,  13,   5,   7,   6,   7,   0,  /*    1 */
  0,   0,   0,   0,   0,   0,   0,   0,  /*    2 */
  0,   0,   0,   0,   0,   0,   0,   0,  /*    2 */
  0,   0,   0,   0,   0,   0,   0,   0,  /*    2 */
  0,   0,   0,   0,   0,   0,   0,   0,  /*    2 */
  2,   3,   4,   4,   4,   4,  15,  15,  /*    2 */
 11,  15,  16,   5,   7,   8,  15,  11,  /*    2 */
 15,   7,  17,  17,  11,  16,  15,   3,  /*    2 */
 11,  18,  16,   6,  19,  19,  19,   3,  /*    2 */
 20,  20,  20,  20,  20,  20,  20,  20,  /*    3 */
 20,  20,  20,  20,  20,  20,  20,  20,  /*    3 */
 20,  20,  20,  20,  20,  20,  20,   7,  /*    3 */
 20,  20,  20,  20,  20,  20,  20,  16,  /*    3 */
 21,  21,  21,  21,  21,  21,  21,  21,  /*    3 */
 21,  21,  21,  21,  21,  21,  21,  21,  /*    3 */
 21,  21,  21,  21,  21,  21,  21,   7,  /*    3 */
 21,  21,  21,  21,  21,  21,  21,  22,  /*    3 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    4 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    4 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    4 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    4 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    4 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    4 */
 25,  26,  23,  24,  23,  24,  23,  24,  /*    4 */
 16,  23,  24,  23,  24,  23,  24,  23,  /*    4 */
 24,  23,  24,  23,  24,  23,  24,  23,  /*    5 */
 24,  16,  23,  24,  23,  24,  23,  24,  /*    5 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    5 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    5 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    5 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    5 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    5 */
 27,  23,  24,  23,  24,  23,  24,  28,  /*    5 */
 16,  29,  23,  24,  23,  24,  30,  23,  /*    6 */
 24,  31,  31,  23,  24,  16,  32,  32,  /*    6 */
 33,  23,  24,  31,  34,  16,  35,  36,  /*    6 */
 23,  24,  16,  16,  35,  37,  16,  38,  /*    6 */
 23,  24,  23,  24,  23,  24,  38,  23,  /*    6 */
 24,  39,  40,  16,  23,  24,  39,  23,  /*    6 */
 24,  41,  41,  23,  24,  23,  24,  42,  /*    6 */
 23,  24,  16,  40,  23,  24,  40,  40,  /*    6 */
 40,  40,  40,  40,  43,  44,  45,  43,  /*    7 */
 44,  45,  43,  44,  45,  23,  24,  23,  /*    7 */
 24,  23,  24,  23,  24,  23,  24,  23,  /*    7 */
 24,  23,  24,  23,  24,  16,  23,  24,  /*    7 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    7 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    7 */
 16,  43,  44,  45,  23,  24,  46,  46,  /*    7 */
 46,  46,  23,  24,  23,  24,  23,  24,  /*    7 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    8 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    8 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*    8 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*    8 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*    8 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*    8 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*    8 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*    8 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*    9 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*    9 */
 16,  16,  16,  47,  48,  16,  49,  49,  /*    9 */
 50,  50,  16,  51,  16,  16,  16,  16,  /*    9 */
 49,  16,  16,  52,  16,  16,  16,  16,  /*    9 */
 53,  54,  16,  16,  16,  16,  16,  54,  /*    9 */
 16,  16,  55,  16,  16,  16,  16,  16,  /*    9 */
 16,  16,  16,  16,  16,  16,  16,  16,  /*    9 */
 16,  16,  16,  56,  16,  16,  16,  16,  /*   10 */
 56,  16,  57,  57,  16,  16,  16,  16,  /*   10 */
 16,  16,  58,  16,  16,  16,  16,  16,  /*   10 */
 16,  16,  16,  16,  16,  16,  16,  16,  /*   10 */
 16,  16,  16,  16,  16,  16,  16,  16,  /*   10 */
 16,  46,  46,  46,  46,  46,  46,  46,  /*   10 */
 59,  59,  59,  59,  59,  59,  59,  59,  /*   10 */
 59,  11,  11,  59,  59,  59,  59,  59,  /*   10 */
 59,  59,  11,  11,  11,  11,  11,  11,  /*   11 */
 11,  11,  11,  11,  11,  11,  11,  11,  /*   11 */
 59,  59,  11,  11,  11,  11,  11,  11,  /*   11 */
 11,  11,  11,  11,  11,  11,  11,  46,  /*   11 */
 59,  59,  59,  59,  59,  11,  11,  11,  /*   11 */
 11,  11,  46,  46,  46,  46,  46,  46,  /*   11 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   11 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   11 */
 60,  60,  60,  60,  60,  60,  60,  60,  /*   12 */
 60,  60,  60,  60,  60,  60,  60,  60,  /*   12 */
 60,  60,  60,  60,  60,  60,  60,  60,  /*   12 */
 60,  60,  60,  60,  60,  60,  60,  60,  /*   12 */
 60,  60,  60,  60,  60,  60,  60,  60,  /*   12 */
 60,  60,  60,  60,  60,  60,  60,  60,  /*   12 */
 60,  60,  60,  60,  60,  60,  60,  60,  /*   12 */
 60,  60,  60,  60,  60,  60,  60,  60,  /*   12 */
 60,  60,  60,  60,  60,  60,  46,  46,  /*   13 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   13 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   13 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   13 */
 60,  60,  46,  46,  46,  46,  46,  46,  /*   13 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   13 */
 46,  46,  46,  46,   3,   3,  46,  46,  /*   13 */
 46,  46,  59,  46,  46,  46,   3,  46,  /*   13 */
 46,  46,  46,  46,  11,  11,  61,   3,  /*   14 */
 62,  62,  62,  46,  63,  46,  64,  64,  /*   14 */
 16,  20,  20,  20,  20,  20,  20,  20,  /*   14 */
 20,  20,  20,  20,  20,  20,  20,  20,  /*   14 */
 20,  20,  46,  20,  20,  20,  20,  20,  /*   14 */
 20,  20,  20,  20,  65,  66,  66,  66,  /*   14 */
 16,  21,  21,  21,  21,  21,  21,  21,  /*   14 */
 21,  21,  21,  21,  21,  21,  21,  21,  /*   14 */
 21,  21,  16,  21,  21,  21,  21,  21,  /*   15 */
 21,  21,  21,  21,  67,  68,  68,  46,  /*   15 */
 69,  70,  38,  38,  38,  71,  72,  46,  /*   15 */
 46,  46,  38,  46,  38,  46,  38,  46,  /*   15 */
 38,  46,  23,  24,  23,  24,  23,  24,  /*   15 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   15 */
 73,  74,  16,  40,  46,  46,  46,  46,  /*   15 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   15 */
 46,  75,  75,  75,  75,  75,  75,  75,  /*   16 */
 75,  75,  75,  75,  75,  46,  75,  75,  /*   16 */
 20,  20,  20,  20,  20,  20,  20,  20,  /*   16 */
 20,  20,  20,  20,  20,  20,  20,  20,  /*   16 */
 20,  20,  20,  20,  20,  20,  20,  20,  /*   16 */
 20,  20,  20,  20,  20,  20,  20,  20,  /*   16 */
 21,  21,  21,  21,  21,  21,  21,  21,  /*   16 */
 21,  21,  21,  21,  21,  21,  21,  21,  /*   16 */
 21,  21,  21,  21,  21,  21,  21,  21,  /*   17 */
 21,  21,  21,  21,  21,  21,  21,  21,  /*   17 */
 46,  74,  74,  74,  74,  74,  74,  74,  /*   17 */
 74,  74,  74,  74,  74,  46,  74,  74,  /*   17 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   17 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   17 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   17 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   17 */
 23,  24,  15,  60,  60,  60,  60,  46,  /*   18 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   18 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   18 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   18 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   18 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   18 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   18 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   18 */
 40,  23,  24,  23,  24,  46,  46,  23,  /*   19 */
 24,  46,  46,  23,  24,  46,  46,  46,  /*   19 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   19 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   19 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   19 */
 23,  24,  23,  24,  46,  46,  23,  24,  /*   19 */
 23,  24,  23,  24,  23,  24,  46,  46,  /*   19 */
 23,  24,  46,  46,  46,  46,  46,  46,  /*   19 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   20 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   20 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   20 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   20 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   20 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   20 */
 46,  76,  76,  76,  76,  76,  76,  76,  /*   20 */
 76,  76,  76,  76,  76,  76,  76,  76,  /*   20 */
 76,  76,  76,  76,  76,  76,  76,  76,  /*   21 */
 76,  76,  76,  76,  76,  76,  76,  76,  /*   21 */
 76,  76,  76,  76,  76,  76,  76,  46,  /*   21 */
 46,  59,   3,   3,   3,   3,   3,   3,  /*   21 */
 46,  77,  77,  77,  77,  77,  77,  77,  /*   21 */
 77,  77,  77,  77,  77,  77,  77,  77,  /*   21 */
 77,  77,  77,  77,  77,  77,  77,  77,  /*   21 */
 77,  77,  77,  77,  77,  77,  77,  77,  /*   21 */
 77,  77,  77,  77,  77,  77,  77,  16,  /*   22 */
 46,   3,  46,  46,  46,  46,  46,  46,  /*   22 */
 46,  60,  60,  60,  60,  60,  60,  60,  /*   22 */
 60,  60,  60,  60,  60,  60,  60,  60,  /*   22 */
 60,  60,  46,  60,  60,  60,  60,  60,  /*   22 */
 60,  60,  60,  60,  60,  60,  60,  60,  /*   22 */
 60,  60,  60,  60,  60,  60,  60,  60,  /*   22 */
 60,  60,  46,  60,  60,  60,   3,  60,  /*   22 */
  3,  60,  60,   3,  60,  46,  46,  46,  /*   23 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   23 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   23 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   23 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   23 */
 40,  40,  40,  46,  46,  46,  46,  46,  /*   23 */
 40,  40,  40,   3,   3,  46,  46,  46,  /*   23 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   23 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   24 */
 46,  46,  46,  46,   3,  46,  46,  46,  /*   24 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   24 */
 46,  46,  46,   3,  46,  46,  46,   3,  /*   24 */
 46,  40,  40,  40,  40,  40,  40,  40,  /*   24 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   24 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   24 */
 40,  40,  40,  46,  46,  46,  46,  46,  /*   24 */
 59,  40,  40,  40,  40,  40,  40,  40,  /*   25 */
 40,  40,  40,  60,  60,  60,  60,  60,  /*   25 */
 60,  60,  60,  46,  46,  46,  46,  46,  /*   25 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   25 */
 78,  78,  78,  78,  78,  78,  78,  78,  /*   25 */
 78,  78,   3,   3,   3,   3,  46,  46,  /*   25 */
 60,  40,  40,  40,  40,  40,  40,  40,  /*   25 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   25 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   26 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   26 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   26 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   26 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   26 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   26 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   26 */
 46,  46,  40,  40,  40,  40,  40,  46,  /*   26 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   27 */
 40,  40,  40,  40,  40,  40,  40,  46,  /*   27 */
 40,  40,  40,  40,   3,  40,  60,  60,  /*   27 */
 60,  60,  60,  60,  60,  79,  79,  60,  /*   27 */
 60,  60,  60,  60,  60,  59,  59,  60,  /*   27 */
 60,  15,  60,  60,  60,  60,  46,  46,  /*   27 */
  9,   9,   9,   9,   9,   9,   9,   9,  /*   27 */
  9,   9,  46,  46,  46,  46,  46,  46,  /*   27 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   28 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   28 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   28 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   28 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   28 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   28 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   28 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   28 */
 46,  60,  60,  80,  46,  40,  40,  40,  /*   29 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   29 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   29 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   29 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   29 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   29 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   29 */
 40,  40,  46,  46,  60,  40,  80,  80,  /*   29 */
 80,  60,  60,  60,  60,  60,  60,  60,  /*   30 */
 60,  80,  80,  80,  80,  60,  46,  46,  /*   30 */
 15,  60,  60,  60,  60,  46,  46,  46,  /*   30 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   30 */
 40,  40,  60,  60,   3,   3,  81,  81,  /*   30 */
 81,  81,  81,  81,  81,  81,  81,  81,  /*   30 */
  3,  46,  46,  46,  46,  46,  46,  46,  /*   30 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   30 */
 46,  60,  80,  80,  46,  40,  40,  40,  /*   31 */
 40,  40,  40,  40,  40,  46,  46,  40,  /*   31 */
 40,  46,  46,  40,  40,  40,  40,  40,  /*   31 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   31 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   31 */
 40,  46,  40,  40,  40,  40,  40,  40,  /*   31 */
 40,  46,  40,  46,  46,  46,  40,  40,  /*   31 */
 40,  40,  46,  46,  60,  46,  80,  80,  /*   31 */
 80,  60,  60,  60,  60,  46,  46,  80,  /*   32 */
 80,  46,  46,  80,  80,  60,  46,  46,  /*   32 */
 46,  46,  46,  46,  46,  46,  46,  80,  /*   32 */
 46,  46,  46,  46,  40,  40,  46,  40,  /*   32 */
 40,  40,  60,  60,  46,  46,  81,  81,  /*   32 */
 81,  81,  81,  81,  81,  81,  81,  81,  /*   32 */
 40,  40,   4,   4,  82,  82,  82,  82,  /*   32 */
 19,  83,  15,  46,  46,  46,  46,  46,  /*   32 */
 46,  46,  60,  46,  46,  40,  40,  40,  /*   33 */
 40,  40,  40,  46,  46,  46,  46,  40,  /*   33 */
 40,  46,  46,  40,  40,  40,  40,  40,  /*   33 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   33 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   33 */
 40,  46,  40,  40,  40,  40,  40,  40,  /*   33 */
 40,  46,  40,  40,  46,  40,  40,  46,  /*   33 */
 40,  40,  46,  46,  60,  46,  80,  80,  /*   33 */
 80,  60,  60,  46,  46,  46,  46,  60,  /*   34 */
 60,  46,  46,  60,  60,  60,  46,  46,  /*   34 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   34 */
 46,  40,  40,  40,  40,  46,  40,  46,  /*   34 */
 46,  46,  46,  46,  46,  46,  81,  81,  /*   34 */
 81,  81,  81,  81,  81,  81,  81,  81,  /*   34 */
 60,  60,  40,  40,  40,  46,  46,  46,  /*   34 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   34 */
 46,  60,  60,  80,  46,  40,  40,  40,  /*   35 */
 40,  40,  40,  40,  46,  40,  46,  40,  /*   35 */
 40,  40,  46,  40,  40,  40,  40,  40,  /*   35 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   35 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   35 */
 40,  46,  40,  40,  40,  40,  40,  40,  /*   35 */
 40,  46,  40,  40,  46,  40,  40,  40,  /*   35 */
 40,  40,  46,  46,  60,  40,  80,  80,  /*   35 */
 80,  60,  60,  60,  60,  60,  46,  60,  /*   36 */
 60,  80,  46,  80,  80,  60,  46,  46,  /*   36 */
 15,  46,  46,  46,  46,  46,  46,  46,  /*   36 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   36 */
 40,  46,  46,  46,  46,  46,  81,  81,  /*   36 */
 81,  81,  81,  81,  81,  81,  81,  81,  /*   36 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   36 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   36 */
 46,  60,  80,  80,  46,  40,  40,  40,  /*   37 */
 40,  40,  40,  40,  40,  46,  46,  40,  /*   37 */
 40,  46,  46,  40,  40,  40,  40,  40,  /*   37 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   37 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   37 */
 40,  46,  40,  40,  40,  40,  40,  40,  /*   37 */
 40,  46,  40,  40,  46,  46,  40,  40,  /*   37 */
 40,  40,  46,  46,  60,  40,  80,  60,  /*   37 */
 80,  60,  60,  60,  46,  46,  46,  80,  /*   38 */
 80,  46,  46,  80,  80,  60,  46,  46,  /*   38 */
 46,  46,  46,  46,  46,  46,  60,  80,  /*   38 */
 46,  46,  46,  46,  40,  40,  46,  40,  /*   38 */
 40,  40,  46,  46,  46,  46,  81,  81,  /*   38 */
 81,  81,  81,  81,  81,  81,  81,  81,  /*   38 */
 15,  46,  46,  46,  46,  46,  46,  46,  /*   38 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   38 */
 46,  46,  60,  80,  46,  40,  40,  40,  /*   39 */
 40,  40,  40,  46,  46,  46,  40,  40,  /*   39 */
 40,  46,  40,  40,  40,  40,  46,  46,  /*   39 */
 46,  40,  40,  46,  40,  46,  40,  40,  /*   39 */
 46,  46,  46,  40,  40,  46,  46,  46,  /*   39 */
 40,  40,  40,  46,  46,  46,  40,  40,  /*   39 */
 40,  40,  40,  40,  40,  40,  46,  40,  /*   39 */
 40,  40,  46,  46,  46,  46,  80,  80,  /*   39 */
 60,  80,  80,  46,  46,  46,  80,  80,  /*   40 */
 80,  46,  80,  80,  80,  60,  46,  46,  /*   40 */
 46,  46,  46,  46,  46,  46,  46,  80,  /*   40 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   40 */
 46,  46,  46,  46,  46,  46,  46,  81,  /*   40 */
 81,  81,  81,  81,  81,  81,  81,  81,  /*   40 */
 84,  19,  19,  46,  46,  46,  46,  46,  /*   40 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   40 */
 46,  80,  80,  80,  46,  40,  40,  40,  /*   41 */
 40,  40,  40,  40,  40,  46,  40,  40,  /*   41 */
 40,  46,  40,  40,  40,  40,  40,  40,  /*   41 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   41 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   41 */
 40,  46,  40,  40,  40,  40,  40,  40,  /*   41 */
 40,  40,  40,  40,  46,  40,  40,  40,  /*   41 */
 40,  40,  46,  46,  46,  46,  60,  60,  /*   41 */
 60,  80,  80,  80,  80,  46,  60,  60,  /*   42 */
 60,  46,  60,  60,  60,  60,  46,  46,  /*   42 */
 46,  46,  46,  46,  46,  60,  60,  46,  /*   42 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   42 */
 40,  40,  46,  46,  46,  46,  81,  81,  /*   42 */
 81,  81,  81,  81,  81,  81,  81,  81,  /*   42 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   42 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   42 */
 46,  46,  80,  80,  46,  40,  40,  40,  /*   43 */
 40,  40,  40,  40,  40,  46,  40,  40,  /*   43 */
 40,  46,  40,  40,  40,  40,  40,  40,  /*   43 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   43 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   43 */
 40,  46,  40,  40,  40,  40,  40,  40,  /*   43 */
 40,  40,  40,  40,  46,  40,  40,  40,  /*   43 */
 40,  40,  46,  46,  46,  46,  80,  60,  /*   43 */
 80,  80,  80,  80,  80,  46,  60,  80,  /*   44 */
 80,  46,  80,  80,  60,  60,  46,  46,  /*   44 */
 46,  46,  46,  46,  46,  80,  80,  46,  /*   44 */
 46,  46,  46,  46,  46,  46,  40,  46,  /*   44 */
 40,  40,  46,  46,  46,  46,  81,  81,  /*   44 */
 81,  81,  81,  81,  81,  81,  81,  81,  /*   44 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   44 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   44 */
 46,  46,  80,  80,  46,  40,  40,  40,  /*   45 */
 40,  40,  40,  40,  40,  46,  40,  40,  /*   45 */
 40,  46,  40,  40,  40,  40,  40,  40,  /*   45 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   45 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   45 */
 40,  46,  40,  40,  40,  40,  40,  40,  /*   45 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   45 */
 40,  40,  46,  46,  46,  46,  80,  80,  /*   45 */
 80,  60,  60,  60,  46,  46,  80,  80,  /*   46 */
 80,  46,  80,  80,  80,  60,  46,  46,  /*   46 */
 46,  46,  46,  46,  46,  46,  46,  80,  /*   46 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   46 */
 40,  40,  46,  46,  46,  46,  81,  81,  /*   46 */
 81,  81,  81,  81,  81,  81,  81,  81,  /*   46 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   46 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   46 */
 46,  40,  40,  40,  40,  40,  40,  40,  /*   47 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   47 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   47 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   47 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   47 */
 40,  40,  40,  40,  40,  40,  40,   3,  /*   47 */
 40,  60,  40,  40,  60,  60,  60,  60,  /*   47 */
 60,  60,  60,  46,  46,  46,  46,   4,  /*   47 */
 40,  40,  40,  40,  40,  40,  59,  60,  /*   48 */
 60,  60,  60,  60,  60,  60,  60,  15,  /*   48 */
  9,   9,   9,   9,   9,   9,   9,   9,  /*   48 */
  9,   9,   3,   3,  46,  46,  46,  46,  /*   48 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   48 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   48 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   48 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   48 */
 46,  40,  40,  46,  40,  46,  46,  40,  /*   49 */
 40,  46,  40,  46,  46,  40,  46,  46,  /*   49 */
 46,  46,  46,  46,  40,  40,  40,  40,  /*   49 */
 46,  40,  40,  40,  40,  40,  40,  40,  /*   49 */
 46,  40,  40,  40,  46,  40,  46,  40,  /*   49 */
 46,  46,  40,  40,  46,  40,  40,   3,  /*   49 */
 40,  60,  40,  40,  60,  60,  60,  60,  /*   49 */
 60,  60,  46,  60,  60,  40,  46,  46,  /*   49 */
 40,  40,  40,  40,  40,  46,  59,  46,  /*   50 */
 60,  60,  60,  60,  60,  60,  46,  46,  /*   50 */
  9,   9,   9,   9,   9,   9,   9,   9,  /*   50 */
  9,   9,  46,  46,  40,  40,  46,  46,  /*   50 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   50 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   50 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   50 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   50 */
 15,  15,  15,  15,   3,   3,   3,   3,  /*   51 */
  3,   3,   3,   3,   3,   3,   3,   3,  /*   51 */
  3,   3,   3,  15,  15,  15,  15,  15,  /*   51 */
 60,  60,  15,  15,  15,  15,  15,  15,  /*   51 */
 78,  78,  78,  78,  78,  78,  78,  78,  /*   51 */
 78,  78,  85,  85,  85,  85,  85,  85,  /*   51 */
 85,  85,  85,  85,  15,  60,  15,  60,  /*   51 */
 15,  60,   5,   6,   5,   6,  80,  80,  /*   51 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   52 */
 46,  40,  40,  40,  40,  40,  40,  40,  /*   52 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   52 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   52 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   52 */
 40,  40,  46,  46,  46,  46,  46,  46,  /*   52 */
 46,  60,  60,  60,  60,  60,  60,  60,  /*   52 */
 60,  60,  60,  60,  60,  60,  60,  80,  /*   52 */
 60,  60,  60,  60,  60,   3,  60,  60,  /*   53 */
 60,  60,  60,  60,  46,  46,  46,  46,  /*   53 */
 60,  60,  60,  60,  60,  60,  46,  60,  /*   53 */
 46,  60,  60,  60,  60,  60,  60,  60,  /*   53 */
 60,  60,  60,  60,  60,  60,  60,  60,  /*   53 */
 60,  60,  60,  60,  60,  60,  46,  46,  /*   53 */
 46,  60,  60,  60,  60,  60,  60,  60,  /*   53 */
 46,  60,  46,  46,  46,  46,  46,  46,  /*   53 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   54 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   54 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   54 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   54 */
 76,  76,  76,  76,  76,  76,  76,  76,  /*   54 */
 76,  76,  76,  76,  76,  76,  76,  76,  /*   54 */
 76,  76,  76,  76,  76,  76,  76,  76,  /*   54 */
 76,  76,  76,  76,  76,  76,  76,  76,  /*   54 */
 76,  76,  76,  76,  76,  76,  46,  46,  /*   55 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   55 */
 16,  16,  16,  16,  16,  16,  16,  16,  /*   55 */
 16,  16,  16,  16,  16,  16,  16,  16,  /*   55 */
 16,  16,  16,  16,  16,  16,  16,  16,  /*   55 */
 16,  16,  16,  16,  16,  16,  16,  16,  /*   55 */
 16,  16,  16,  16,  16,  16,  16,  46,  /*   55 */
 46,  46,  46,   3,  46,  46,  46,  46,  /*   55 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   56 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   56 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   56 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   56 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   56 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   56 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   56 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   56 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   57 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   57 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   57 */
 40,  40,  46,  46,  46,  46,  46,  40,  /*   57 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   57 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   57 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   57 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   57 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   58 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   58 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   58 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   58 */
 40,  40,  40,  46,  46,  46,  46,  46,  /*   58 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   58 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   58 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   58 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   59 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   59 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   59 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   59 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   59 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   59 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   59 */
 40,  40,  46,  46,  46,  46,  46,  46,  /*   59 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   60 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   60 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   60 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   60 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   60 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   60 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   60 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   60 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   61 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   61 */
 23,  24,  23,  24,  23,  24,  16,  16,  /*   61 */
 16,  16,  16,  16,  46,  46,  46,  46,  /*   61 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   61 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   61 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   61 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   61 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   62 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   62 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   62 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   62 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   62 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   62 */
 23,  24,  23,  24,  23,  24,  23,  24,  /*   62 */
 23,  24,  46,  46,  46,  46,  46,  46,  /*   62 */
 86,  86,  86,  86,  86,  86,  86,  86,  /*   63 */
 87,  87,  87,  87,  87,  87,  87,  87,  /*   63 */
 86,  86,  86,  86,  86,  86,  46,  46,  /*   63 */
 87,  87,  87,  87,  87,  87,  46,  46,  /*   63 */
 86,  86,  86,  86,  86,  86,  86,  86,  /*   63 */
 87,  87,  87,  87,  87,  87,  87,  87,  /*   63 */
 86,  86,  86,  86,  86,  86,  86,  86,  /*   63 */
 87,  87,  87,  87,  87,  87,  87,  87,  /*   63 */
 86,  86,  86,  86,  86,  86,  46,  46,  /*   64 */
 87,  87,  87,  87,  87,  87,  46,  46,  /*   64 */
 16,  86,  16,  86,  16,  86,  16,  86,  /*   64 */
 46,  87,  46,  87,  46,  87,  46,  87,  /*   64 */
 86,  86,  86,  86,  86,  86,  86,  86,  /*   64 */
 87,  87,  87,  87,  87,  87,  87,  87,  /*   64 */
 88,  88,  89,  89,  89,  89,  90,  90,  /*   64 */
 91,  91,  92,  92,  93,  93,  46,  46,  /*   64 */
 86,  86,  86,  86,  86,  86,  86,  86,  /*   65 */
 87,  87,  87,  87,  87,  87,  87,  87,  /*   65 */
 86,  86,  86,  86,  86,  86,  86,  86,  /*   65 */
 87,  87,  87,  87,  87,  87,  87,  87,  /*   65 */
 86,  86,  86,  86,  86,  86,  86,  86,  /*   65 */
 87,  87,  87,  87,  87,  87,  87,  87,  /*   65 */
 86,  86,  16,  94,  16,  46,  16,  16,  /*   65 */
 87,  87,  95,  95,  96,  11,  38,  11,  /*   65 */
 11,  11,  16,  94,  16,  46,  16,  16,  /*   66 */
 97,  97,  97,  97,  96,  11,  11,  11,  /*   66 */
 86,  86,  16,  16,  46,  46,  16,  16,  /*   66 */
 87,  87,  98,  98,  46,  11,  11,  11,  /*   66 */
 86,  86,  16,  16,  16,  99,  16,  16,  /*   66 */
 87,  87, 100, 100, 101,  11,  11,  11,  /*   66 */
 46,  46,  16,  94,  16,  46,  16,  16,  /*   66 */
102, 102, 103, 103,  96,  11,  11,  46,  /*   66 */
  2,   2,   2,   2,   2,   2,   2,   2,  /*   67 */
  2,   2,   2,   2, 104, 104, 104, 104,  /*   67 */
  8,   8,   8,   8,   8,   8,   3,   3,  /*   67 */
  5,   6,   5,   5,   5,   6,   5,   5,  /*   67 */
  3,   3,   3,   3,   3,   3,   3,   3,  /*   67 */
105, 106, 104, 104, 104, 104, 104,  46,  /*   67 */
  3,   3,   3,   3,   3,   3,   3,   3,  /*   67 */
  3,   5,   6,   3,   3,   3,   3,  12,  /*   67 */
 12,   3,   3,   3,   7,   5,   6,  46,  /*   68 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   68 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   68 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   68 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   68 */
 46,  46, 104, 104, 104, 104, 104, 104,  /*   68 */
 17,  46,  46,  46,  17,  17,  17,  17,  /*   68 */
 17,  17,   7,   7,   7,   5,   6,  16,  /*   68 */
107, 107, 107, 107, 107, 107, 107, 107,  /*   69 */
107, 107,   7,   7,   7,   5,   6,  46,  /*   69 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   69 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   69 */
  4,   4,   4,   4,   4,   4,   4,   4,  /*   69 */
  4,   4,   4,   4,  46,  46,  46,  46,  /*   69 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   69 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   69 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   70 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   70 */
 60,  60,  60,  60,  60,  60,  60,  60,  /*   70 */
 60,  60,  60,  60,  60,  79,  79,  79,  /*   70 */
 79,  60,  46,  46,  46,  46,  46,  46,  /*   70 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   70 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   70 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   70 */
 15,  15,  38,  15,  15,  15,  15,  38,  /*   71 */
 15,  15,  16,  38,  38,  38,  16,  16,  /*   71 */
 38,  38,  38,  16,  15,  38,  15,  15,  /*   71 */
 38,  38,  38,  38,  38,  38,  15,  15,  /*   71 */
 15,  15,  15,  15,  38,  15,  38,  15,  /*   71 */
 38,  15,  38,  38,  38,  38,  16,  16,  /*   71 */
 38,  38,  15,  38,  16,  40,  40,  40,  /*   71 */
 40,  46,  46,  46,  46,  46,  46,  46,  /*   71 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   72 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   72 */
 46,  46,  46,  19,  19,  19,  19,  19,  /*   72 */
 19,  19,  19,  19,  19,  19,  19, 108,  /*   72 */
109, 109, 109, 109, 109, 109, 109, 109,  /*   72 */
109, 109, 109, 109, 110, 110, 110, 110,  /*   72 */
111, 111, 111, 111, 111, 111, 111, 111,  /*   72 */
111, 111, 111, 111, 112, 112, 112, 112,  /*   72 */
113, 113, 113,  46,  46,  46,  46,  46,  /*   73 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   73 */
  7,   7,   7,   7,   7,  15,  15,  15,  /*   73 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   73 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   73 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   73 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   73 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   73 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   74 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   74 */
 15,  15,   7,  15,   7,  15,  15,  15,  /*   74 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   74 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   74 */
 15,  15,  15,  46,  46,  46,  46,  46,  /*   74 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   74 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   74 */
  7,   7,   7,   7,   7,   7,   7,   7,  /*   75 */
  7,   7,   7,   7,   7,   7,   7,   7,  /*   75 */
  7,   7,   7,   7,   7,   7,   7,   7,  /*   75 */
  7,   7,   7,   7,   7,   7,   7,   7,  /*   75 */
  7,   7,   7,   7,   7,   7,   7,   7,  /*   75 */
  7,   7,   7,   7,   7,   7,   7,   7,  /*   75 */
  7,   7,   7,   7,   7,   7,   7,   7,  /*   75 */
  7,   7,   7,   7,   7,   7,   7,   7,  /*   75 */
  7,   7,   7,   7,   7,   7,   7,   7,  /*   76 */
  7,   7,   7,   7,   7,   7,   7,   7,  /*   76 */
  7,   7,   7,   7,   7,   7,   7,   7,  /*   76 */
  7,   7,   7,   7,   7,   7,   7,   7,  /*   76 */
  7,   7,   7,   7,   7,   7,   7,   7,  /*   76 */
  7,   7,   7,   7,   7,   7,   7,   7,  /*   76 */
  7,   7,  46,  46,  46,  46,  46,  46,  /*   76 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   76 */
 15,  46,  15,  15,  15,  15,  15,  15,  /*   77 */
  7,   7,   7,   7,  15,  15,  15,  15,  /*   77 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   77 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   77 */
  7,   7,  15,  15,  15,  15,  15,  15,  /*   77 */
 15,   5,   6,  15,  15,  15,  15,  15,  /*   77 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   77 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   77 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   78 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   78 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   78 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   78 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   78 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   78 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   78 */
 15,  15,  15,  46,  46,  46,  46,  46,  /*   78 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   79 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   79 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   79 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   79 */
 15,  15,  15,  15,  15,  46,  46,  46,  /*   79 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   79 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   79 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   79 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   80 */
 15,  15,  15,  46,  46,  46,  46,  46,  /*   80 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   80 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   80 */
114, 114, 114, 114, 114, 114, 114, 114,  /*   80 */
114, 114, 114, 114, 114, 114, 114, 114,  /*   80 */
114, 114, 114, 114,  82,  82,  82,  82,  /*   80 */
 82,  82,  82,  82,  82,  82,  82,  82,  /*   80 */
 82,  82,  82,  82,  82,  82,  82,  82,  /*   81 */
115, 115, 115, 115, 115, 115, 115, 115,  /*   81 */
115, 115, 115, 115, 115, 115, 115, 115,  /*   81 */
115, 115, 115, 115,  15,  15,  15,  15,  /*   81 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   81 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   81 */
 15,  15,  15,  15,  15,  15, 116, 116,  /*   81 */
116, 116, 116, 116, 116, 116, 116, 116,  /*   81 */
116, 116, 116, 116, 116, 116, 116, 116,  /*   82 */
116, 116, 116, 116, 116, 116, 116, 116,  /*   82 */
117, 117, 117, 117, 117, 117, 117, 117,  /*   82 */
117, 117, 117, 117, 117, 117, 117, 117,  /*   82 */
117, 117, 117, 117, 117, 117, 117, 117,  /*   82 */
117, 117, 118,  46,  46,  46,  46,  46,  /*   82 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   82 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   82 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   83 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   83 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   83 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   83 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   83 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   83 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   83 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   83 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   84 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   84 */
 15,  15,  15,  15,  15,  15,  46,  46,  /*   84 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   84 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   84 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   84 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   84 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   84 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   85 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   85 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   85 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   85 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   85 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   85 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   85 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   85 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   86 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   86 */
 15,  15,  15,  15,  46,  46,  46,  46,  /*   86 */
 46,  46,  15,  15,  15,  15,  15,  15,  /*   86 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   86 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   86 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   86 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   86 */
 46,  15,  15,  15,  15,  46,  15,  15,  /*   87 */
 15,  15,  46,  46,  15,  15,  15,  15,  /*   87 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   87 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   87 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   87 */
 46,  15,  15,  15,  15,  15,  15,  15,  /*   87 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   87 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   87 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   88 */
 15,  15,  15,  15,  46,  15,  46,  15,  /*   88 */
 15,  15,  15,  46,  46,  46,  15,  46,  /*   88 */
 15,  15,  15,  15,  15,  15,  15,  46,  /*   88 */
 46,  15,  15,  15,  15,  15,  15,  15,  /*   88 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   88 */
 46,  46,  46,  46,  46,  46, 119, 119,  /*   88 */
119, 119, 119, 119, 119, 119, 119, 119,  /*   88 */
114, 114, 114, 114, 114, 114, 114, 114,  /*   89 */
114, 114,  83,  83,  83,  83,  83,  83,  /*   89 */
 83,  83,  83,  83,  15,  46,  46,  46,  /*   89 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   89 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   89 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   89 */
 46,  15,  15,  15,  15,  15,  15,  15,  /*   89 */
 15,  15,  15,  15,  15,  15,  15,  46,  /*   89 */
  2,   3,   3,   3,  15,  59,   3, 120,  /*   90 */
  5,   6,   5,   6,   5,   6,   5,   6,  /*   90 */
  5,   6,  15,  15,   5,   6,   5,   6,  /*   90 */
  5,   6,   5,   6,   8,   5,   6,   5,  /*   90 */
 15, 121, 121, 121, 121, 121, 121, 121,  /*   90 */
121, 121,  60,  60,  60,  60,  60,  60,  /*   90 */
  8,  59,  59,  59,  59,  59,  15,  15,  /*   90 */
 46,  46,  46,  46,  46,  46,  46,  15,  /*   90 */
 46,  40,  40,  40,  40,  40,  40,  40,  /*   91 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   91 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   91 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   91 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   91 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   91 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   91 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   91 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   92 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   92 */
 40,  40,  40,  40,  40,  46,  46,  46,  /*   92 */
 46,  60,  60,  59,  59,  59,  59,  46,  /*   92 */
 46,  40,  40,  40,  40,  40,  40,  40,  /*   92 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   92 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   92 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   92 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   93 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   93 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   93 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   93 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   93 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   93 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   93 */
 40,  40,  40,   3,  59,  59,  59,  46,  /*   93 */
 46,  46,  46,  46,  46,  40,  40,  40,  /*   94 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   94 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   94 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   94 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   94 */
 40,  40,  40,  40,  40,  46,  46,  46,  /*   94 */
 46,  40,  40,  40,  40,  40,  40,  40,  /*   94 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   94 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*   95 */
 40,  40,  40,  40,  40,  40,  40,  46,  /*   95 */
 15,  15,  85,  85,  85,  85,  15,  15,  /*   95 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   95 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   95 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   95 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   95 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   95 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   96 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   96 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   96 */
 15,  15,  15,  15,  15,  46,  46,  46,  /*   96 */
 85,  85,  85,  85,  85,  85,  85,  85,  /*   96 */
 85,  85,  15,  15,  15,  15,  15,  15,  /*   96 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   96 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   96 */
 15,  15,  15,  15,  46,  46,  46,  46,  /*   97 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   97 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   97 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   97 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   97 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   97 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   97 */
 15,  15,  15,  15,  46,  46,  46,  15,  /*   97 */
114, 114, 114, 114, 114, 114, 114, 114,  /*   98 */
114, 114,  15,  15,  15,  15,  15,  15,  /*   98 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   98 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   98 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   98 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   98 */
 15,  46,  46,  46,  46,  46,  46,  46,  /*   98 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*   98 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   99 */
 15,  15,  15,  15,  46,  46,  46,  46,  /*   99 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   99 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   99 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   99 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   99 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*   99 */
 15,  15,  15,  15,  15,  15,  15,  46,  /*   99 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*  100 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*  100 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*  100 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*  100 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*  100 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*  100 */
 15,  15,  15,  15,  15,  15,  15,  46,  /*  100 */
 46,  46,  46,  15,  15,  15,  15,  15,  /*  100 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*  101 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*  101 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*  101 */
 15,  15,  15,  15,  15,  15,  46,  46,  /*  101 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*  101 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*  101 */
 15,  15,  15,  15,  15,  15,  15,  15,  /*  101 */
 15,  15,  15,  15,  15,  15,  15,  46,  /*  101 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  102 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  102 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  102 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  102 */
 40,  40,  40,  40,  40,  40,  46,  46,  /*  102 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  102 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  102 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  102 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  103 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  103 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  103 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  103 */
 40,  40,  40,  40,  46,  46,  46,  46,  /*  103 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  103 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  103 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  103 */
122, 122, 122, 122, 122, 122, 122, 122,  /*  104 */
122, 122, 122, 122, 122, 122, 122, 122,  /*  104 */
122, 122, 122, 122, 122, 122, 122, 122,  /*  104 */
122, 122, 122, 122, 122, 122, 122, 122,  /*  104 */
122, 122, 122, 122, 122, 122, 122, 122,  /*  104 */
122, 122, 122, 122, 122, 122, 122, 122,  /*  104 */
122, 122, 122, 122, 122, 122, 122, 122,  /*  104 */
122, 122, 122, 122, 122, 122, 122, 122,  /*  104 */
123, 123, 123, 123, 123, 123, 123, 123,  /*  105 */
123, 123, 123, 123, 123, 123, 123, 123,  /*  105 */
123, 123, 123, 123, 123, 123, 123, 123,  /*  105 */
123, 123, 123, 123, 123, 123, 123, 123,  /*  105 */
123, 123, 123, 123, 123, 123, 123, 123,  /*  105 */
123, 123, 123, 123, 123, 123, 123, 123,  /*  105 */
123, 123, 123, 123, 123, 123, 123, 123,  /*  105 */
123, 123, 123, 123, 123, 123, 123, 123,  /*  105 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  106 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  106 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  106 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  106 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  106 */
 40,  40,  40,  40,  40,  40,  46,  46,  /*  106 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  106 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  106 */
 16,  16,  16,  16,  16,  16,  16,  46,  /*  107 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  107 */
 46,  46,  46,  16,  16,  16,  16,  16,  /*  107 */
 46,  46,  46,  46,  46,  46,  60,  40,  /*  107 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  107 */
 40,   7,  40,  40,  40,  40,  40,  40,  /*  107 */
 40,  40,  40,  40,  40,  40,  40,  46,  /*  107 */
 40,  40,  40,  40,  40,  46,  40,  46,  /*  107 */
 40,  40,  46,  40,  40,  46,  40,  40,  /*  108 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  108 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  108 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  108 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  108 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  108 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  108 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  108 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  109 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  109 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  109 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  109 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  109 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  109 */
 40,  40,  46,  46,  46,  46,  46,  46,  /*  109 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  109 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  110 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  110 */
 46,  46,  46,  40,  40,  40,  40,  40,  /*  110 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  110 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  110 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  110 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  110 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  110 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  111 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  111 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  111 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  111 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  111 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  111 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  111 */
 40,  40,  40,  40,  40,  40,   5,   6,  /*  111 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  112 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  112 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  112 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  112 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  112 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  112 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  112 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  112 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  113 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  113 */
 46,  46,  40,  40,  40,  40,  40,  40,  /*  113 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  113 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  113 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  113 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  113 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  113 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  114 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  114 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  114 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  114 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  114 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  114 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  114 */
 40,  40,  40,  40,  46,  46,  46,  46,  /*  114 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  115 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  115 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  115 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  115 */
 60,  60,  60,  60,  46,  46,  46,  46,  /*  115 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  115 */
  3,   8,   8,  12,  12,   5,   6,   5,  /*  115 */
  6,   5,   6,   5,   6,   5,   6,   5,  /*  115 */
  6,   5,   6,   5,   6,  46,  46,  46,  /*  116 */
 46,   3,   3,   3,   3,  12,  12,  12,  /*  116 */
  3,   3,   3,  46,   3,   3,   3,   3,  /*  116 */
  8,   5,   6,   5,   6,   5,   6,   3,  /*  116 */
  3,   3,   7,   8,   7,   7,   7,  46,  /*  116 */
  3,   4,   3,   3,  46,  46,  46,  46,  /*  116 */
 40,  40,  40,  46,  40,  46,  40,  40,  /*  116 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  116 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  117 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  117 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  117 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  117 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  117 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  117 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  117 */
 40,  40,  40,  40,  40,  46,  46, 104,  /*  117 */
 46,   3,   3,   3,   4,   3,   3,   3,  /*  118 */
  5,   6,   3,   7,   3,   8,   3,   3,  /*  118 */
  9,   9,   9,   9,   9,   9,   9,   9,  /*  118 */
  9,   9,   3,   3,   7,   7,   7,   3,  /*  118 */
  3,  10,  10,  10,  10,  10,  10,  10,  /*  118 */
 10,  10,  10,  10,  10,  10,  10,  10,  /*  118 */
 10,  10,  10,  10,  10,  10,  10,  10,  /*  118 */
 10,  10,  10,   5,   3,   6,  11,  12,  /*  118 */
 11,  13,  13,  13,  13,  13,  13,  13,  /*  119 */
 13,  13,  13,  13,  13,  13,  13,  13,  /*  119 */
 13,  13,  13,  13,  13,  13,  13,  13,  /*  119 */
 13,  13,  13,   5,   7,   6,   7,  46,  /*  119 */
 46,   3,   5,   6,   3,   3,  40,  40,  /*  119 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  119 */
 59,  40,  40,  40,  40,  40,  40,  40,  /*  119 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  119 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  120 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  120 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  120 */
 40,  40,  40,  40,  40,  40,  59,  59,  /*  120 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  120 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  120 */
 40,  40,  40,  40,  40,  40,  40,  40,  /*  120 */
 40,  40,  40,  40,  40,  40,  40,  46,  /*  120 */
 46,  46,  40,  40,  40,  40,  40,  40,  /*  121 */
 46,  46,  40,  40,  40,  40,  40,  40,  /*  121 */
 46,  46,  40,  40,  40,  40,  40,  40,  /*  121 */
 46,  46,  40,  40,  40,  46,  46,  46,  /*  121 */
  4,   4,   7,  11,  15,   4,   4,  46,  /*  121 */
  7,   7,   7,   7,   7,  15,  15,  46,  /*  121 */
 46,  46,  46,  46,  46,  46,  46,  46,  /*  121 */
 46,  46,  46,  46,  46,  15,  46,  46   /*  121 */
};

/* The A table has 124 entries for a total of 496 bytes. */

const uint32 js_A[] = {
0x0001000F,  /*    0   Cc, ignorable */
0x0004000F,  /*    1   Cc, whitespace */
0x0004000C,  /*    2   Zs, whitespace */
0x00000018,  /*    3   Po */
0x0006001A,  /*    4   Sc, currency */
0x00000015,  /*    5   Ps */
0x00000016,  /*    6   Pe */
0x00000019,  /*    7   Sm */
0x00000014,  /*    8   Pd */
0x00036089,  /*    9   Nd, identifier part, decimal 16 */
0x0827FF81,  /*   10   Lu, hasLower (add 32), identifier start, supradecimal 31 */
0x0000001B,  /*   11   Sk */
0x00050017,  /*   12   Pc, underscore */
0x0817FF82,  /*   13   Ll, hasUpper (subtract 32), identifier start, supradecimal 31 */
0x0000000C,  /*   14   Zs */
0x0000001C,  /*   15   So */
0x00070182,  /*   16   Ll, identifier start */
0x0000600B,  /*   17   No, decimal 16 */
0x0000500B,  /*   18   No, decimal 8 */
0x0000800B,  /*   19   No, strange */
0x08270181,  /*   20   Lu, hasLower (add 32), identifier start */
0x08170182,  /*   21   Ll, hasUpper (subtract 32), identifier start */
0xE1D70182,  /*   22   Ll, hasUpper (subtract -121), identifier start */
0x00670181,  /*   23   Lu, hasLower (add 1), identifier start */
0x00570182,  /*   24   Ll, hasUpper (subtract 1), identifier start */
0xCE670181,  /*   25   Lu, hasLower (add -199), identifier start */
0x3A170182,  /*   26   Ll, hasUpper (subtract 232), identifier start */
0xE1E70181,  /*   27   Lu, hasLower (add -121), identifier start */
0x4B170182,  /*   28   Ll, hasUpper (subtract 300), identifier start */
0x34A70181,  /*   29   Lu, hasLower (add 210), identifier start */
0x33A70181,  /*   30   Lu, hasLower (add 206), identifier start */
0x33670181,  /*   31   Lu, hasLower (add 205), identifier start */
0x32A70181,  /*   32   Lu, hasLower (add 202), identifier start */
0x32E70181,  /*   33   Lu, hasLower (add 203), identifier start */
0x33E70181,  /*   34   Lu, hasLower (add 207), identifier start */
0x34E70181,  /*   35   Lu, hasLower (add 211), identifier start */
0x34670181,  /*   36   Lu, hasLower (add 209), identifier start */
0x35670181,  /*   37   Lu, hasLower (add 213), identifier start */
0x00070181,  /*   38   Lu, identifier start */
0x36A70181,  /*   39   Lu, hasLower (add 218), identifier start */
0x00070185,  /*   40   Lo, identifier start */
0x36670181,  /*   41   Lu, hasLower (add 217), identifier start */
0x36E70181,  /*   42   Lu, hasLower (add 219), identifier start */
0x00AF0181,  /*   43   Lu, hasLower (add 2), hasTitle, identifier start */
0x007F0183,  /*   44   Lt, hasUpper (subtract 1), hasLower (add 1), hasTitle, identifier start */
0x009F0182,  /*   45   Ll, hasUpper (subtract 2), hasTitle, identifier start */
0x00000000,  /*   46   unassigned */
0x34970182,  /*   47   Ll, hasUpper (subtract 210), identifier start */
0x33970182,  /*   48   Ll, hasUpper (subtract 206), identifier start */
0x33570182,  /*   49   Ll, hasUpper (subtract 205), identifier start */
0x32970182,  /*   50   Ll, hasUpper (subtract 202), identifier start */
0x32D70182,  /*   51   Ll, hasUpper (subtract 203), identifier start */
0x33D70182,  /*   52   Ll, hasUpper (subtract 207), identifier start */
0x34570182,  /*   53   Ll, hasUpper (subtract 209), identifier start */
0x34D70182,  /*   54   Ll, hasUpper (subtract 211), identifier start */
0x35570182,  /*   55   Ll, hasUpper (subtract 213), identifier start */
0x36970182,  /*   56   Ll, hasUpper (subtract 218), identifier start */
0x36570182,  /*   57   Ll, hasUpper (subtract 217), identifier start */
0x36D70182,  /*   58   Ll, hasUpper (subtract 219), identifier start */
0x00070084,  /*   59   Lm, identifier start */
0x00030086,  /*   60   Mn, identifier part */
0x09A70181,  /*   61   Lu, hasLower (add 38), identifier start */
0x09670181,  /*   62   Lu, hasLower (add 37), identifier start */
0x10270181,  /*   63   Lu, hasLower (add 64), identifier start */
0x0FE70181,  /*   64   Lu, hasLower (add 63), identifier start */
0x09970182,  /*   65   Ll, hasUpper (subtract 38), identifier start */
0x09570182,  /*   66   Ll, hasUpper (subtract 37), identifier start */
0x10170182,  /*   67   Ll, hasUpper (subtract 64), identifier start */
0x0FD70182,  /*   68   Ll, hasUpper (subtract 63), identifier start */
0x0F970182,  /*   69   Ll, hasUpper (subtract 62), identifier start */
0x0E570182,  /*   70   Ll, hasUpper (subtract 57), identifier start */
0x0BD70182,  /*   71   Ll, hasUpper (subtract 47), identifier start */
0x0D970182,  /*   72   Ll, hasUpper (subtract 54), identifier start */
0x15970182,  /*   73   Ll, hasUpper (subtract 86), identifier start */
0x14170182,  /*   74   Ll, hasUpper (subtract 80), identifier start */
0x14270181,  /*   75   Lu, hasLower (add 80), identifier start */
0x0C270181,  /*   76   Lu, hasLower (add 48), identifier start */
0x0C170182,  /*   77   Ll, hasUpper (subtract 48), identifier start */
0x00034089,  /*   78   Nd, identifier part, decimal 0 */
0x00000087,  /*   79   Me */
0x00030088,  /*   80   Mc, identifier part */
0x00037489,  /*   81   Nd, identifier part, decimal 26 */
0x00005A0B,  /*   82   No, decimal 13 */
0x00006E0B,  /*   83   No, decimal 23 */
0x0000740B,  /*   84   No, decimal 26 */
0x0000000B,  /*   85   No */
0xFE170182,  /*   86   Ll, hasUpper (subtract -8), identifier start */
0xFE270181,  /*   87   Lu, hasLower (add -8), identifier start */
0xED970182,  /*   88   Ll, hasUpper (subtract -74), identifier start */
0xEA970182,  /*   89   Ll, hasUpper (subtract -86), identifier start */
0xE7170182,  /*   90   Ll, hasUpper (subtract -100), identifier start */
0xE0170182,  /*   91   Ll, hasUpper (subtract -128), identifier start */
0xE4170182,  /*   92   Ll, hasUpper (subtract -112), identifier start */
0xE0970182,  /*   93   Ll, hasUpper (subtract -126), identifier start */
0xFDD70182,  /*   94   Ll, hasUpper (subtract -9), identifier start */
0xEDA70181,  /*   95   Lu, hasLower (add -74), identifier start */
0xFDE70181,  /*   96   Lu, hasLower (add -9), identifier start */
0xEAA70181,  /*   97   Lu, hasLower (add -86), identifier start */
0xE7270181,  /*   98   Lu, hasLower (add -100), identifier start */
0xFE570182,  /*   99   Ll, hasUpper (subtract -7), identifier start */
0xE4270181,  /*  100   Lu, hasLower (add -112), identifier start */
0xFE670181,  /*  101   Lu, hasLower (add -7), identifier start */
0xE0270181,  /*  102   Lu, hasLower (add -128), identifier start */
0xE0A70181,  /*  103   Lu, hasLower (add -126), identifier start */
0x00010010,  /*  104   Cf, ignorable */
0x0004000D,  /*  105   Zl, whitespace */
0x0004000E,  /*  106   Zp, whitespace */
0x0000400B,  /*  107   No, decimal 0 */
0x0000440B,  /*  108   No, decimal 2 */
0x0427438A,  /*  109   Nl, hasLower (add 16), identifier start, decimal 1 */
0x0427818A,  /*  110   Nl, hasLower (add 16), identifier start, strange */
0x0417638A,  /*  111   Nl, hasUpper (subtract 16), identifier start, decimal 17 */
0x0417818A,  /*  112   Nl, hasUpper (subtract 16), identifier start, strange */
0x0007818A,  /*  113   Nl, identifier start, strange */
0x0000420B,  /*  114   No, decimal 1 */
0x0000720B,  /*  115   No, decimal 25 */
0x06A0001C,  /*  116   So, hasLower (add 26) */
0x0690001C,  /*  117   So, hasUpper (subtract 26) */
0x00006C0B,  /*  118   No, decimal 22 */
0x0000560B,  /*  119   No, decimal 11 */
0x0007738A,  /*  120   Nl, identifier start, decimal 25 */
0x0007418A,  /*  121   Nl, identifier start, decimal 0 */
0x00000013,  /*  122   Cs */
0x00000012   /*  123   Co */
};

const jschar js_uriReservedPlusPound_ucstr[] =
    {';', '/', '?', ':', '@', '&', '=', '+', '$', ',', '#', 0};
const jschar js_uriUnescaped_ucstr[] =
    {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
     'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
     'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
     'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
     'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
     '-', '_', '.', '!', '~', '*', '\'', '(', ')', 0};

/*
 * This table allows efficient testing for the regular expression \w which is
 * defined by ECMA-262 15.10.2.6 to be [0-9A-Z_a-z].
 */
const bool js_alnum[] = {
/*       0      1      2      3      4      5      5      7      8      9      */
/*  0 */ false, false, false, false, false, false, false, false, false, false,
/*  1 */ false, false, false, false, false, false, false, false, false, false,
/*  2 */ false, false, false, false, false, false, false, false, false, false,
/*  3 */ false, false, false, false, false, false, false, false, false, false,
/*  4 */ false, false, false, false, false, false, false, false, true,  true,
/*  5 */ true,  true,  true,  true,  true,  true,  true,  true,  false, false,
/*  6 */ false, false, false, false, false, true,  true,  true,  true,  true,
/*  7 */ true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
/*  8 */ true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
/*  9 */ true,  false, false, false, false, true,  false, true,  true,  true,
/* 10 */ true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
/* 11 */ true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
/* 12 */ true,  true,  true,  false, false, false, false, false
};

#define URI_CHUNK 64U

static inline bool
TransferBufferToString(JSContext *cx, StringBuffer &sb, Value *rval)
{
    JSString *str = sb.finishString();
    if (!str)
        return false;
    rval->setString(str);
    return true;;
}

/*
 * ECMA 3, 15.1.3 URI Handling Function Properties
 *
 * The following are implementations of the algorithms
 * given in the ECMA specification for the hidden functions
 * 'Encode' and 'Decode'.
 */
static JSBool
Encode(JSContext *cx, JSString *str, const jschar *unescapedSet,
       const jschar *unescapedSet2, Value *rval)
{
    static const char HexDigits[] = "0123456789ABCDEF"; /* NB: uppercase */

    size_t length = str->length();
    const jschar *chars = str->getChars(cx);
    if (!chars)
        return JS_FALSE;

    if (length == 0) {
        rval->setString(cx->runtime->emptyString);
        return JS_TRUE;
    }

    StringBuffer sb(cx);
    jschar hexBuf[4];
    hexBuf[0] = '%';
    hexBuf[3] = 0;
    for (size_t k = 0; k < length; k++) {
        jschar c = chars[k];
        if (js_strchr(unescapedSet, c) ||
            (unescapedSet2 && js_strchr(unescapedSet2, c))) {
            if (!sb.append(c))
                return JS_FALSE;
        } else {
            if ((c >= 0xDC00) && (c <= 0xDFFF)) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_BAD_URI, NULL);
                return JS_FALSE;
            }
            uint32 v;
            if (c < 0xD800 || c > 0xDBFF) {
                v = c;
            } else {
                k++;
                if (k == length) {
                    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_BAD_URI, NULL);
                    return JS_FALSE;
                }
                jschar c2 = chars[k];
                if ((c2 < 0xDC00) || (c2 > 0xDFFF)) {
                    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_BAD_URI, NULL);
                    return JS_FALSE;
                }
                v = ((c - 0xD800) << 10) + (c2 - 0xDC00) + 0x10000;
            }
            uint8 utf8buf[4];
            size_t L = js_OneUcs4ToUtf8Char(utf8buf, v);
            for (size_t j = 0; j < L; j++) {
                hexBuf[1] = HexDigits[utf8buf[j] >> 4];
                hexBuf[2] = HexDigits[utf8buf[j] & 0xf];
                if (!sb.append(hexBuf, 3))
                    return JS_FALSE;
            }
        }
    }

    return TransferBufferToString(cx, sb, rval);
}

static JSBool
Decode(JSContext *cx, JSString *str, const jschar *reservedSet, Value *rval)
{
    size_t length = str->length();
    const jschar *chars = str->getChars(cx);
    if (!chars)
        return JS_FALSE;

    if (length == 0) {
        rval->setString(cx->runtime->emptyString);
        return JS_TRUE;
    }

    StringBuffer sb(cx);
    for (size_t k = 0; k < length; k++) {
        jschar c = chars[k];
        if (c == '%') {
            size_t start = k;
            if ((k + 2) >= length)
                goto report_bad_uri;
            if (!JS7_ISHEX(chars[k+1]) || !JS7_ISHEX(chars[k+2]))
                goto report_bad_uri;
            jsuint B = JS7_UNHEX(chars[k+1]) * 16 + JS7_UNHEX(chars[k+2]);
            k += 2;
            if (!(B & 0x80)) {
                c = (jschar)B;
            } else {
                intN n = 1;
                while (B & (0x80 >> n))
                    n++;
                if (n == 1 || n > 4)
                    goto report_bad_uri;
                uint8 octets[4];
                octets[0] = (uint8)B;
                if (k + 3 * (n - 1) >= length)
                    goto report_bad_uri;
                for (intN j = 1; j < n; j++) {
                    k++;
                    if (chars[k] != '%')
                        goto report_bad_uri;
                    if (!JS7_ISHEX(chars[k+1]) || !JS7_ISHEX(chars[k+2]))
                        goto report_bad_uri;
                    B = JS7_UNHEX(chars[k+1]) * 16 + JS7_UNHEX(chars[k+2]);
                    if ((B & 0xC0) != 0x80)
                        goto report_bad_uri;
                    k += 2;
                    octets[j] = (char)B;
                }
                uint32 v = Utf8ToOneUcs4Char(octets, n);
                if (v >= 0x10000) {
                    v -= 0x10000;
                    if (v > 0xFFFFF)
                        goto report_bad_uri;
                    c = (jschar)((v & 0x3FF) + 0xDC00);
                    jschar H = (jschar)((v >> 10) + 0xD800);
                    if (!sb.append(H))
                        return JS_FALSE;
                } else {
                    c = (jschar)v;
                }
            }
            if (js_strchr(reservedSet, c)) {
                if (!sb.append(chars + start, k - start + 1))
                    return JS_FALSE;
            } else {
                if (!sb.append(c))
                    return JS_FALSE;
            }
        } else {
            if (!sb.append(c))
                return JS_FALSE;
        }
    }

    return TransferBufferToString(cx, sb, rval);

  report_bad_uri:
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_URI);
    /* FALL THROUGH */

    return JS_FALSE;
}

static JSBool
str_decodeURI(JSContext *cx, uintN argc, Value *vp)
{
    JSLinearString *str = ArgToRootedString(cx, argc, vp, 0);
    if (!str)
        return JS_FALSE;
    return Decode(cx, str, js_uriReservedPlusPound_ucstr, vp);
}

static JSBool
str_decodeURI_Component(JSContext *cx, uintN argc, Value *vp)
{
    JSLinearString *str = ArgToRootedString(cx, argc, vp, 0);
    if (!str)
        return JS_FALSE;
    return Decode(cx, str, js_empty_ucstr, vp);
}

static JSBool
str_encodeURI(JSContext *cx, uintN argc, Value *vp)
{
    JSLinearString *str = ArgToRootedString(cx, argc, vp, 0);
    if (!str)
        return JS_FALSE;
    return Encode(cx, str, js_uriReservedPlusPound_ucstr, js_uriUnescaped_ucstr,
                  vp);
}

static JSBool
str_encodeURI_Component(JSContext *cx, uintN argc, Value *vp)
{
    JSLinearString *str = ArgToRootedString(cx, argc, vp, 0);
    if (!str)
        return JS_FALSE;
    return Encode(cx, str, js_uriUnescaped_ucstr, NULL, vp);
}

/*
 * Convert one UCS-4 char and write it into a UTF-8 buffer, which must be at
 * least 4 bytes long.  Return the number of UTF-8 bytes of data written.
 */
int
js_OneUcs4ToUtf8Char(uint8 *utf8Buffer, uint32 ucs4Char)
{
    int utf8Length = 1;

    JS_ASSERT(ucs4Char <= 0x10FFFF);
    if (ucs4Char < 0x80) {
        *utf8Buffer = (uint8)ucs4Char;
    } else {
        int i;
        uint32 a = ucs4Char >> 11;
        utf8Length = 2;
        while (a) {
            a >>= 5;
            utf8Length++;
        }
        i = utf8Length;
        while (--i) {
            utf8Buffer[i] = (uint8)((ucs4Char & 0x3F) | 0x80);
            ucs4Char >>= 6;
        }
        *utf8Buffer = (uint8)(0x100 - (1 << (8-utf8Length)) + ucs4Char);
    }
    return utf8Length;
}

/*
 * Convert a utf8 character sequence into a UCS-4 character and return that
 * character.  It is assumed that the caller already checked that the sequence
 * is valid.
 */
static uint32
Utf8ToOneUcs4Char(const uint8 *utf8Buffer, int utf8Length)
{
    uint32 ucs4Char;
    uint32 minucs4Char;
    /* from Unicode 3.1, non-shortest form is illegal */
    static const uint32 minucs4Table[] = {
        0x00000080, 0x00000800, 0x00010000
    };

    JS_ASSERT(utf8Length >= 1 && utf8Length <= 4);
    if (utf8Length == 1) {
        ucs4Char = *utf8Buffer;
        JS_ASSERT(!(ucs4Char & 0x80));
    } else {
        JS_ASSERT((*utf8Buffer & (0x100 - (1 << (7-utf8Length)))) ==
                  (0x100 - (1 << (8-utf8Length))));
        ucs4Char = *utf8Buffer++ & ((1<<(7-utf8Length))-1);
        minucs4Char = minucs4Table[utf8Length-2];
        while (--utf8Length) {
            JS_ASSERT((*utf8Buffer & 0xC0) == 0x80);
            ucs4Char = ucs4Char<<6 | (*utf8Buffer++ & 0x3F);
        }
        if (JS_UNLIKELY(ucs4Char < minucs4Char)) {
            ucs4Char = OVERLONG_UTF8;
        } else if (ucs4Char == 0xFFFE || ucs4Char == 0xFFFF) {
            ucs4Char = 0xFFFD;
        }
    }
    return ucs4Char;
}

namespace js {

size_t
PutEscapedStringImpl(char *buffer, size_t bufferSize, FILE *fp, JSLinearString *str, uint32 quote)
{
    enum {
        STOP, FIRST_QUOTE, LAST_QUOTE, CHARS, ESCAPE_START, ESCAPE_MORE
    } state;

    JS_ASSERT(quote == 0 || quote == '\'' || quote == '"');
    JS_ASSERT_IF(!buffer, bufferSize == 0);
    JS_ASSERT_IF(fp, !buffer);

    if (bufferSize == 0)
        buffer = NULL;
    else
        bufferSize--;

    const jschar *chars = str->chars();
    const jschar *charsEnd = chars + str->length();
    size_t n = 0;
    state = FIRST_QUOTE;
    uintN shift = 0;
    uintN hex = 0;
    uintN u = 0;
    char c = 0;  /* to quell GCC warnings */

    for (;;) {
        switch (state) {
          case STOP:
            goto stop;
          case FIRST_QUOTE:
            state = CHARS;
            goto do_quote;
          case LAST_QUOTE:
            state = STOP;
          do_quote:
            if (quote == 0)
                continue;
            c = (char)quote;
            break;
          case CHARS:
            if (chars == charsEnd) {
                state = LAST_QUOTE;
                continue;
            }
            u = *chars++;
            if (u < ' ') {
                if (u != 0) {
                    const char *escape = strchr(js_EscapeMap, (int)u);
                    if (escape) {
                        u = escape[1];
                        goto do_escape;
                    }
                }
                goto do_hex_escape;
            }
            if (u < 127) {
                if (u == quote || u == '\\')
                    goto do_escape;
                c = (char)u;
            } else if (u < 0x100) {
                goto do_hex_escape;
            } else {
                shift = 16;
                hex = u;
                u = 'u';
                goto do_escape;
            }
            break;
          do_hex_escape:
            shift = 8;
            hex = u;
            u = 'x';
          do_escape:
            c = '\\';
            state = ESCAPE_START;
            break;
          case ESCAPE_START:
            JS_ASSERT(' ' <= u && u < 127);
            c = (char)u;
            state = ESCAPE_MORE;
            break;
          case ESCAPE_MORE:
            if (shift == 0) {
                state = CHARS;
                continue;
            }
            shift -= 4;
            u = 0xF & (hex >> shift);
            c = (char)(u + (u < 10 ? '0' : 'A' - 10));
            break;
        }
        if (buffer) {
            JS_ASSERT(n <= bufferSize);
            if (n != bufferSize) {
                buffer[n] = c;
            } else {
                buffer[n] = '\0';
                buffer = NULL;
            }
        } else if (fp) {
            if (fputc(c, fp) < 0)
                return size_t(-1);
        }
        n++;
    }
  stop:
    if (buffer)
        buffer[n] = '\0';
    return n;
}

} /* namespace js */
