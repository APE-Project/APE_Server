/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
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
 * JS boolean implementation.
 */
#include "jstypes.h"
#include "jsstdint.h"
#include "jsutil.h"
#include "jsapi.h"
#include "jsatom.h"
#include "jsbool.h"
#include "jscntxt.h"
#include "jsversion.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsstr.h"
#include "jsvector.h"

#include "jsinterpinlines.h"
#include "jsobjinlines.h"

using namespace js;

Class js_BooleanClass = {
    "Boolean",
    JSCLASS_HAS_RESERVED_SLOTS(1) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Boolean),
    PropertyStub,         /* addProperty */
    PropertyStub,         /* delProperty */
    PropertyStub,         /* getProperty */
    StrictPropertyStub,   /* setProperty */
    EnumerateStub,
    ResolveStub,
    ConvertStub
};

#if JS_HAS_TOSOURCE
#include "jsprf.h"

static JSBool
bool_toSource(JSContext *cx, uintN argc, Value *vp)
{
    bool b;
    if (!GetPrimitiveThis(cx, vp, &b))
        return false;

    char buf[32];
    JS_snprintf(buf, sizeof buf, "(new Boolean(%s))", JS_BOOLEAN_STR(b));
    JSString *str = JS_NewStringCopyZ(cx, buf);
    if (!str)
        return false;
    vp->setString(str);
    return true;
}
#endif

static JSBool
bool_toString(JSContext *cx, uintN argc, Value *vp)
{
    bool b;
    if (!GetPrimitiveThis(cx, vp, &b))
        return false;

    JSAtom *atom = cx->runtime->atomState.booleanAtoms[b ? 1 : 0];
    JSString *str = ATOM_TO_STRING(atom);
    if (!str)
        return JS_FALSE;
    vp->setString(str);
    return JS_TRUE;
}

static JSBool
bool_valueOf(JSContext *cx, uintN argc, Value *vp)
{
    bool b;
    if (!GetPrimitiveThis(cx, vp, &b))
        return false;

    vp->setBoolean(b);
    return JS_TRUE;
}

static JSFunctionSpec boolean_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,  bool_toSource,  0, 0),
#endif
    JS_FN(js_toString_str,  bool_toString,  0, 0),
    JS_FN(js_valueOf_str,   bool_valueOf,   0, 0),
    JS_FN(js_toJSON_str,    bool_valueOf,   0, 0),
    JS_FS_END
};

static JSBool
Boolean(JSContext *cx, uintN argc, Value *vp)
{
    Value *argv = vp + 2;
    bool b = argc != 0 ? js_ValueToBoolean(argv[0]) : false;

    if (IsConstructing(vp)) {
        JSObject *obj = NewBuiltinClassInstance(cx, &js_BooleanClass);
        if (!obj)
            return false;
        obj->setPrimitiveThis(BooleanValue(b));
        vp->setObject(*obj);
    } else {
        vp->setBoolean(b);
    }
    return true;
}

JSObject *
js_InitBooleanClass(JSContext *cx, JSObject *obj)
{
    JSObject *proto;

    proto = js_InitClass(cx, obj, NULL, &js_BooleanClass, Boolean, 1,
                         NULL, boolean_methods, NULL, NULL);
    if (!proto)
        return NULL;
    proto->setPrimitiveThis(BooleanValue(false));
    return proto;
}

JSString *
js_BooleanToString(JSContext *cx, JSBool b)
{
    return ATOM_TO_STRING(cx->runtime->atomState.booleanAtoms[b ? 1 : 0]);
}

/* This function implements E-262-3 section 9.8, toString. */
bool
js::BooleanToStringBuffer(JSContext *cx, JSBool b, StringBuffer &sb)
{
    return b ? sb.append("true") : sb.append("false");
}

JSBool
js_ValueToBoolean(const Value &v)
{
    if (v.isInt32())
        return v.toInt32() != 0;
    if (v.isString())
        return v.toString()->length() != 0;
    if (v.isObject())
        return JS_TRUE;
    if (v.isNullOrUndefined())
        return JS_FALSE;
    if (v.isDouble()) {
        jsdouble d;

        d = v.toDouble();
        return !JSDOUBLE_IS_NaN(d) && d != 0;
    }
    JS_ASSERT(v.isBoolean());
    return v.toBoolean();
}
