/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=79:
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
 * JS object implementation.
 */
#include <stdlib.h>
#include <string.h>
#include "jstypes.h"
#include "jsstdint.h"
#include "jsarena.h"
#include "jsbit.h"
#include "jsutil.h"
#include "jshash.h"
#include "jsdhash.h"
#include "jsprf.h"
#include "jsapi.h"
#include "jsarray.h"
#include "jsatom.h"
#include "jsbool.h"
#include "jsbuiltins.h"
#include "jscntxt.h"
#include "jsversion.h"
#include "jsemit.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsinterp.h"
#include "jsiter.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jsparse.h"
#include "jsproxy.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsstaticcheck.h"
#include "jsstdint.h"
#include "jsstr.h"
#include "jstracer.h"
#include "jsdbgapi.h"
#include "json.h"
#include "jswrapper.h"

#include "jsinterpinlines.h"
#include "jsscopeinlines.h"
#include "jsscriptinlines.h"
#include "jsobjinlines.h"

#if JS_HAS_GENERATORS
#include "jsiter.h"
#endif

#if JS_HAS_XML_SUPPORT
#include "jsxml.h"
#endif

#if JS_HAS_XDR
#include "jsxdrapi.h"
#endif

#include "jsprobes.h"
#include "jsatominlines.h"
#include "jsobjinlines.h"
#include "jsscriptinlines.h"

#include "jsautooplen.h"

using namespace js;
using namespace js::gc;

JS_FRIEND_DATA(const JSObjectMap) JSObjectMap::sharedNonNative(JSObjectMap::SHAPELESS);

Class js_ObjectClass = {
    js_Object_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_Object),
    PropertyStub,         /* addProperty */
    PropertyStub,         /* delProperty */
    PropertyStub,         /* getProperty */
    StrictPropertyStub,   /* setProperty */
    EnumerateStub,
    ResolveStub,
    ConvertStub
};

JS_FRIEND_API(JSObject *)
js_ObjectToOuterObject(JSContext *cx, JSObject *obj)
{
    OBJ_TO_OUTER_OBJECT(cx, obj);
    return obj;
}

#if JS_HAS_OBJ_PROTO_PROP

static JSBool
obj_getProto(JSContext *cx, JSObject *obj, jsid id, Value *vp);

static JSBool
obj_setProto(JSContext *cx, JSObject *obj, jsid id, JSBool strict, Value *vp);

static JSPropertySpec object_props[] = {
    {js_proto_str, 0, JSPROP_PERMANENT|JSPROP_SHARED, Jsvalify(obj_getProto), Jsvalify(obj_setProto)},
    {0,0,0,0,0}
};

static JSBool
obj_getProto(JSContext *cx, JSObject *obj, jsid id, Value *vp)
{
    /* Let CheckAccess get the slot's value, based on the access mode. */
    uintN attrs;
    id = ATOM_TO_JSID(cx->runtime->atomState.protoAtom);
    return CheckAccess(cx, obj, id, JSACC_PROTO, vp, &attrs);
}

static JSBool
obj_setProto(JSContext *cx, JSObject *obj, jsid id, JSBool strict, Value *vp)
{
    /* ECMAScript 5 8.6.2 forbids changing [[Prototype]] if not [[Extensible]]. */
    if (!obj->isExtensible()) {
        obj->reportNotExtensible(cx);
        return false;
    }

    if (!vp->isObjectOrNull())
        return JS_TRUE;

    JSObject *pobj = vp->toObjectOrNull();
    if (pobj) {
        /*
         * Innerize pobj here to avoid sticking unwanted properties on the
         * outer object. This ensures that any with statements only grant
         * access to the inner object.
         */
        OBJ_TO_INNER_OBJECT(cx, pobj);
        if (!pobj)
            return JS_FALSE;
    }

    uintN attrs;
    id = ATOM_TO_JSID(cx->runtime->atomState.protoAtom);
    if (!CheckAccess(cx, obj, id, JSAccessMode(JSACC_PROTO|JSACC_WRITE), vp, &attrs))
        return JS_FALSE;

    return SetProto(cx, obj, pobj, JS_TRUE);
}

#else  /* !JS_HAS_OBJ_PROTO_PROP */

#define object_props NULL

#endif /* !JS_HAS_OBJ_PROTO_PROP */

static JSHashNumber
js_hash_object(const void *key)
{
    return JSHashNumber(uintptr_t(key) >> JS_GCTHING_ALIGN);
}

static JSHashEntry *
MarkSharpObjects(JSContext *cx, JSObject *obj, JSIdArray **idap)
{
    JSSharpObjectMap *map;
    JSHashTable *table;
    JSHashNumber hash;
    JSHashEntry **hep, *he;
    jsatomid sharpid;
    JSIdArray *ida;
    JSBool ok;
    jsint i, length;
    jsid id;
    JSObject *obj2;
    JSProperty *prop;

    JS_CHECK_RECURSION(cx, return NULL);

    map = &cx->sharpObjectMap;
    JS_ASSERT(map->depth >= 1);
    table = map->table;
    hash = js_hash_object(obj);
    hep = JS_HashTableRawLookup(table, hash, obj);
    he = *hep;
    if (!he) {
        sharpid = 0;
        he = JS_HashTableRawAdd(table, hep, hash, obj, (void *) sharpid);
        if (!he) {
            JS_ReportOutOfMemory(cx);
            return NULL;
        }

        ida = JS_Enumerate(cx, obj);
        if (!ida)
            return NULL;

        ok = JS_TRUE;
        for (i = 0, length = ida->length; i < length; i++) {
            id = ida->vector[i];
            ok = obj->lookupProperty(cx, id, &obj2, &prop);
            if (!ok)
                break;
            if (!prop)
                continue;
            bool hasGetter, hasSetter;
            AutoValueRooter v(cx);
            AutoValueRooter setter(cx);
            if (obj2->isNative()) {
                const Shape *shape = (Shape *) prop;
                hasGetter = shape->hasGetterValue();
                hasSetter = shape->hasSetterValue();
                if (hasGetter)
                    v.set(shape->getterValue());
                if (hasSetter)
                    setter.set(shape->setterValue());
            } else {
                hasGetter = hasSetter = false;
            }
            if (hasSetter) {
                /* Mark the getter, then set val to setter. */
                if (hasGetter && v.value().isObject()) {
                    ok = !!MarkSharpObjects(cx, &v.value().toObject(), NULL);
                    if (!ok)
                        break;
                }
                v.set(setter.value());
            } else if (!hasGetter) {
                ok = obj->getProperty(cx, id, v.addr());
                if (!ok)
                    break;
            }
            if (v.value().isObject() &&
                !MarkSharpObjects(cx, &v.value().toObject(), NULL)) {
                ok = JS_FALSE;
                break;
            }
        }
        if (!ok || !idap)
            JS_DestroyIdArray(cx, ida);
        if (!ok)
            return NULL;
    } else {
        sharpid = uintptr_t(he->value);
        if (sharpid == 0) {
            sharpid = ++map->sharpgen << SHARP_ID_SHIFT;
            he->value = (void *) sharpid;
        }
        ida = NULL;
    }
    if (idap)
        *idap = ida;
    return he;
}

JSHashEntry *
js_EnterSharpObject(JSContext *cx, JSObject *obj, JSIdArray **idap,
                    jschar **sp)
{
    JSSharpObjectMap *map;
    JSHashTable *table;
    JSIdArray *ida;
    JSHashNumber hash;
    JSHashEntry *he, **hep;
    jsatomid sharpid;
    char buf[20];
    size_t len;

    if (!JS_CHECK_OPERATION_LIMIT(cx))
        return NULL;

    /* Set to null in case we return an early error. */
    *sp = NULL;
    map = &cx->sharpObjectMap;
    table = map->table;
    if (!table) {
        table = JS_NewHashTable(8, js_hash_object, JS_CompareValues,
                                JS_CompareValues, NULL, NULL);
        if (!table) {
            JS_ReportOutOfMemory(cx);
            return NULL;
        }
        map->table = table;
        JS_KEEP_ATOMS(cx->runtime);
    }

    /* From this point the control must flow either through out: or bad:. */
    ida = NULL;
    if (map->depth == 0) {
        /*
         * Although MarkSharpObjects tries to avoid invoking getters,
         * it ends up doing so anyway under some circumstances; for
         * example, if a wrapped object has getters, the wrapper will
         * prevent MarkSharpObjects from recognizing them as such.
         * This could lead to js_LeaveSharpObject being called while
         * MarkSharpObjects is still working.
         *
         * Increment map->depth while we call MarkSharpObjects, to
         * ensure that such a call doesn't free the hash table we're
         * still using.
         */
        ++map->depth;
        he = MarkSharpObjects(cx, obj, &ida);
        --map->depth;
        if (!he)
            goto bad;
        JS_ASSERT((uintptr_t(he->value) & SHARP_BIT) == 0);
        if (!idap) {
            JS_DestroyIdArray(cx, ida);
            ida = NULL;
        }
    } else {
        hash = js_hash_object(obj);
        hep = JS_HashTableRawLookup(table, hash, obj);
        he = *hep;

        /*
         * It's possible that the value of a property has changed from the
         * first time the object's properties are traversed (when the property
         * ids are entered into the hash table) to the second (when they are
         * converted to strings), i.e., the JSObject::getProperty() call is not
         * idempotent.
         */
        if (!he) {
            he = JS_HashTableRawAdd(table, hep, hash, obj, NULL);
            if (!he) {
                JS_ReportOutOfMemory(cx);
                goto bad;
            }
            sharpid = 0;
            goto out;
        }
    }

    sharpid = uintptr_t(he->value);
    if (sharpid != 0) {
        len = JS_snprintf(buf, sizeof buf, "#%u%c",
                          sharpid >> SHARP_ID_SHIFT,
                          (sharpid & SHARP_BIT) ? '#' : '=');
        *sp = js_InflateString(cx, buf, &len);
        if (!*sp) {
            if (ida)
                JS_DestroyIdArray(cx, ida);
            goto bad;
        }
    }

out:
    JS_ASSERT(he);
    if ((sharpid & SHARP_BIT) == 0) {
        if (idap && !ida) {
            ida = JS_Enumerate(cx, obj);
            if (!ida) {
                if (*sp) {
                    cx->free(*sp);
                    *sp = NULL;
                }
                goto bad;
            }
        }
        map->depth++;
    }

    if (idap)
        *idap = ida;
    return he;

bad:
    /* Clean up the sharpObjectMap table on outermost error. */
    if (map->depth == 0) {
        JS_UNKEEP_ATOMS(cx->runtime);
        map->sharpgen = 0;
        JS_HashTableDestroy(map->table);
        map->table = NULL;
    }
    return NULL;
}

void
js_LeaveSharpObject(JSContext *cx, JSIdArray **idap)
{
    JSSharpObjectMap *map;
    JSIdArray *ida;

    map = &cx->sharpObjectMap;
    JS_ASSERT(map->depth > 0);
    if (--map->depth == 0) {
        JS_UNKEEP_ATOMS(cx->runtime);
        map->sharpgen = 0;
        JS_HashTableDestroy(map->table);
        map->table = NULL;
    }
    if (idap) {
        ida = *idap;
        if (ida) {
            JS_DestroyIdArray(cx, ida);
            *idap = NULL;
        }
    }
}

static intN
gc_sharp_table_entry_marker(JSHashEntry *he, intN i, void *arg)
{
    MarkObject((JSTracer *)arg, *(JSObject *)he->key, "sharp table entry");
    return JS_DHASH_NEXT;
}

void
js_TraceSharpMap(JSTracer *trc, JSSharpObjectMap *map)
{
    JS_ASSERT(map->depth > 0);
    JS_ASSERT(map->table);

    /*
     * During recursive calls to MarkSharpObjects a non-native object or
     * object with a custom getProperty method can potentially return an
     * unrooted value or even cut from the object graph an argument of one of
     * MarkSharpObjects recursive invocations. So we must protect map->table
     * entries against GC.
     *
     * We can not simply use JSTempValueRooter to mark the obj argument of
     * MarkSharpObjects during recursion as we have to protect *all* entries
     * in JSSharpObjectMap including those that contains otherwise unreachable
     * objects just allocated through custom getProperty. Otherwise newer
     * allocations can re-use the address of an object stored in the hashtable
     * confusing js_EnterSharpObject. So to address the problem we simply
     * mark all objects from map->table.
     *
     * An alternative "proper" solution is to use JSTempValueRooter in
     * MarkSharpObjects with code to remove during finalization entries
     * with otherwise unreachable objects. But this is way too complex
     * to justify spending efforts.
     */
    JS_HashTableEnumerateEntries(map->table, gc_sharp_table_entry_marker, trc);
}

#if JS_HAS_TOSOURCE
static JSBool
obj_toSource(JSContext *cx, uintN argc, Value *vp)
{
    JSBool ok;
    JSHashEntry *he;
    JSIdArray *ida;
    jschar *chars, *ochars, *vsharp;
    const jschar *idstrchars, *vchars;
    size_t nchars, idstrlength, gsoplength, vlength, vsharplength, curlen;
    const char *comma;
    JSObject *obj2;
    JSProperty *prop;
    Value *val;
    JSString *gsop[2];
    JSString *valstr, *str;
    JSLinearString *idstr;

    JS_CHECK_RECURSION(cx, return JS_FALSE);

    Value localroot[4];
    PodArrayZero(localroot);
    AutoArrayRooter tvr(cx, JS_ARRAY_LENGTH(localroot), localroot);

    /* If outermost, we need parentheses to be an expression, not a block. */
    JSBool outermost = (cx->sharpObjectMap.depth == 0);

    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj)
        return false;

    if (!(he = js_EnterSharpObject(cx, obj, &ida, &chars))) {
        ok = JS_FALSE;
        goto out;
    }
    if (IS_SHARP(he)) {
        /*
         * We didn't enter -- obj is already "sharp", meaning we've visited it
         * already in our depth first search, and therefore chars contains a
         * string of the form "#n#".
         */
        JS_ASSERT(!ida);
#if JS_HAS_SHARP_VARS
        nchars = js_strlen(chars);
#else
        chars[0] = '{';
        chars[1] = '}';
        chars[2] = 0;
        nchars = 2;
#endif
        goto make_string;
    }
    JS_ASSERT(ida);
    ok = JS_TRUE;

    if (!chars) {
        /* If outermost, allocate 4 + 1 for "({})" and the terminator. */
        chars = (jschar *) cx->runtime->malloc(((outermost ? 4 : 2) + 1) * sizeof(jschar));
        nchars = 0;
        if (!chars)
            goto error;
        if (outermost)
            chars[nchars++] = '(';
    } else {
        /* js_EnterSharpObject returned a string of the form "#n=" in chars. */
        MAKE_SHARP(he);
        nchars = js_strlen(chars);
        chars = (jschar *)
            js_realloc((ochars = chars), (nchars + 2 + 1) * sizeof(jschar));
        if (!chars) {
            js_free(ochars);
            goto error;
        }
        if (outermost) {
            /*
             * No need for parentheses around the whole shebang, because #n=
             * unambiguously begins an object initializer, and never a block
             * statement.
             */
            outermost = JS_FALSE;
        }
    }

    chars[nchars++] = '{';

    comma = NULL;

    /*
     * We have four local roots for cooked and raw value GC safety.  Hoist the
     * "localroot + 2" out of the loop using the val local, which refers to
     * the raw (unconverted, "uncooked") values.
     */
    val = localroot + 2;

    for (jsint i = 0, length = ida->length; i < length; i++) {
        /* Get strings for id and value and GC-root them via vp. */
        jsid id = ida->vector[i];

        ok = obj->lookupProperty(cx, id, &obj2, &prop);
        if (!ok)
            goto error;

        /*
         * Convert id to a value and then to a string.  Decide early whether we
         * prefer get/set or old getter/setter syntax.
         */
        JSString *s = js_ValueToString(cx, IdToValue(id));
        if (!s || !(idstr = s->ensureLinear(cx))) {
            ok = JS_FALSE;
            goto error;
        }
        vp->setString(idstr);                           /* local root */

        jsint valcnt = 0;
        if (prop) {
            bool doGet = true;
            if (obj2->isNative()) {
                const Shape *shape = (Shape *) prop;
                unsigned attrs = shape->attributes();
                if (attrs & JSPROP_GETTER) {
                    doGet = false;
                    val[valcnt] = shape->getterValue();
                    gsop[valcnt] = ATOM_TO_STRING(cx->runtime->atomState.getAtom);
                    valcnt++;
                }
                if (attrs & JSPROP_SETTER) {
                    doGet = false;
                    val[valcnt] = shape->setterValue();
                    gsop[valcnt] = ATOM_TO_STRING(cx->runtime->atomState.setAtom);
                    valcnt++;
                }
            }
            if (doGet) {
                valcnt = 1;
                gsop[0] = NULL;
                ok = obj->getProperty(cx, id, &val[0]);
                if (!ok)
                    goto error;
            }
        }

        /*
         * If id is a string that's not an identifier, or if it's a negative
         * integer, then it must be quoted.
         */
        bool idIsLexicalIdentifier = js_IsIdentifier(idstr);
        if (JSID_IS_ATOM(id)
            ? !idIsLexicalIdentifier
            : (!JSID_IS_INT(id) || JSID_TO_INT(id) < 0)) {
            s = js_QuoteString(cx, idstr, jschar('\''));
            if (!s || !(idstr = s->ensureLinear(cx))) {
                ok = JS_FALSE;
                goto error;
            }
            vp->setString(idstr);                       /* local root */
        }
        idstrlength = idstr->length();
        idstrchars = idstr->getChars(cx);
        if (!idstrchars) {
            ok = JS_FALSE;
            goto error;
        }

        for (jsint j = 0; j < valcnt; j++) {
            /*
             * Censor an accessor descriptor getter or setter part if it's
             * undefined.
             */
            if (gsop[j] && val[j].isUndefined())
                continue;

            /* Convert val[j] to its canonical source form. */
            valstr = js_ValueToSource(cx, val[j]);
            if (!valstr) {
                ok = JS_FALSE;
                goto error;
            }
            localroot[j].setString(valstr);             /* local root */
            vchars = valstr->getChars(cx);
            if (!vchars) {
                ok = JS_FALSE;
                goto error;
            }
            vlength = valstr->length();

            /*
             * If val[j] is a non-sharp object, and we're not serializing an
             * accessor (ECMA syntax can't accommodate sharpened accessors),
             * consider sharpening it.
             */
            vsharp = NULL;
            vsharplength = 0;
#if JS_HAS_SHARP_VARS
            if (!gsop[j] && val[j].isObject() && vchars[0] != '#') {
                he = js_EnterSharpObject(cx, &val[j].toObject(), NULL, &vsharp);
                if (!he) {
                    ok = JS_FALSE;
                    goto error;
                }
                if (IS_SHARP(he)) {
                    vchars = vsharp;
                    vlength = js_strlen(vchars);
                } else {
                    if (vsharp) {
                        vsharplength = js_strlen(vsharp);
                        MAKE_SHARP(he);
                    }
                    js_LeaveSharpObject(cx, NULL);
                }
            }
#endif

            /*
             * Remove '(function ' from the beginning of valstr and ')' from the
             * end so that we can put "get" in front of the function definition.
             */
            if (gsop[j] && IsFunctionObject(val[j])) {
                const jschar *start = vchars;
                const jschar *end = vchars + vlength;

                uint8 parenChomp = 0;
                if (vchars[0] == '(') {
                    vchars++;
                    parenChomp = 1;
                }

                /* Try to jump "function" keyword. */
                if (vchars)
                    vchars = js_strchr_limit(vchars, ' ', end);

                /*
                 * Jump over the function's name: it can't be encoded as part
                 * of an ECMA getter or setter.
                 */
                if (vchars)
                    vchars = js_strchr_limit(vchars, '(', end);

                if (vchars) {
                    if (*vchars == ' ')
                        vchars++;
                    vlength = end - vchars - parenChomp;
                } else {
                    gsop[j] = NULL;
                    vchars = start;
                }
            }

#define SAFE_ADD(n)                                                          \
    JS_BEGIN_MACRO                                                           \
        size_t n_ = (n);                                                     \
        curlen += n_;                                                        \
        if (curlen < n_)                                                     \
            goto overflow;                                                   \
    JS_END_MACRO

            curlen = nchars;
            if (comma)
                SAFE_ADD(2);
            SAFE_ADD(idstrlength + 1);
            if (gsop[j])
                SAFE_ADD(gsop[j]->length() + 1);
            SAFE_ADD(vsharplength);
            SAFE_ADD(vlength);
            /* Account for the trailing null. */
            SAFE_ADD((outermost ? 2 : 1) + 1);
#undef SAFE_ADD

            if (curlen > size_t(-1) / sizeof(jschar))
                goto overflow;

            /* Allocate 1 + 1 at end for closing brace and terminating 0. */
            chars = (jschar *) js_realloc((ochars = chars), curlen * sizeof(jschar));
            if (!chars) {
                chars = ochars;
                goto overflow;
            }

            if (comma) {
                chars[nchars++] = comma[0];
                chars[nchars++] = comma[1];
            }
            comma = ", ";

            if (gsop[j]) {
                gsoplength = gsop[j]->length();
                const jschar *gsopchars = gsop[j]->getChars(cx);
                if (!gsopchars)
                    goto overflow;
                js_strncpy(&chars[nchars], gsopchars, gsoplength);
                nchars += gsoplength;
                chars[nchars++] = ' ';
            }
            js_strncpy(&chars[nchars], idstrchars, idstrlength);
            nchars += idstrlength;
            /* Extraneous space after id here will be extracted later */
            chars[nchars++] = gsop[j] ? ' ' : ':';

            if (vsharplength) {
                js_strncpy(&chars[nchars], vsharp, vsharplength);
                nchars += vsharplength;
            }
            js_strncpy(&chars[nchars], vchars, vlength);
            nchars += vlength;

            if (vsharp)
                cx->free(vsharp);
        }
    }

    chars[nchars++] = '}';
    if (outermost)
        chars[nchars++] = ')';
    chars[nchars] = 0;

  error:
    js_LeaveSharpObject(cx, &ida);

    if (!ok) {
        if (chars)
            js_free(chars);
        goto out;
    }

    if (!chars) {
        JS_ReportOutOfMemory(cx);
        ok = JS_FALSE;
        goto out;
    }
  make_string:
    str = js_NewString(cx, chars, nchars);
    if (!str) {
        js_free(chars);
        ok = JS_FALSE;
        goto out;
    }
    vp->setString(str);
    ok = JS_TRUE;
  out:
    return ok;

  overflow:
    cx->free(vsharp);
    js_free(chars);
    chars = NULL;
    goto error;
}
#endif /* JS_HAS_TOSOURCE */

namespace js {

JSString *
obj_toStringHelper(JSContext *cx, JSObject *obj)
{
    if (obj->isProxy())
        return JSProxy::obj_toString(cx, obj);

    const char *clazz = obj->getClass()->name;
    size_t nchars = 9 + strlen(clazz); /* 9 for "[object ]" */
    jschar *chars = (jschar *) cx->malloc((nchars + 1) * sizeof(jschar));
    if (!chars)
        return NULL;

    const char *prefix = "[object ";
    nchars = 0;
    while ((chars[nchars] = (jschar)*prefix) != 0)
        nchars++, prefix++;
    while ((chars[nchars] = (jschar)*clazz) != 0)
        nchars++, clazz++;
    chars[nchars++] = ']';
    chars[nchars] = 0;

    JSString *str = js_NewString(cx, chars, nchars);
    if (!str)
        cx->free(chars);
    return str;
}

}

/* ES5 15.2.4.2.  Note steps 1 and 2 are errata. */
static JSBool
obj_toString(JSContext *cx, uintN argc, Value *vp)
{
    Value &thisv = vp[1];

    /* Step 1. */
    if (thisv.isUndefined()) {
        vp->setString(ATOM_TO_STRING(cx->runtime->atomState.objectUndefinedAtom));
        return true;
    }

    /* Step 2. */
    if (thisv.isNull()) {
        vp->setString(ATOM_TO_STRING(cx->runtime->atomState.objectNullAtom));
        return true;
    }

    /* Step 3. */
    JSObject *obj = ToObject(cx, &thisv);
    if (!obj)
        return false;

    /* Steps 4-5. */
    JSString *str = js::obj_toStringHelper(cx, obj);
    if (!str)
        return false;
    vp->setString(str);
    return true;
}

static JSBool
obj_toLocaleString(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj)
        return false;

    JSString *str = js_ValueToString(cx, ObjectValue(*obj));
    if (!str)
        return JS_FALSE;

    vp->setString(str);
    return JS_TRUE;
}

static JSBool
obj_valueOf(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj)
        return false;
    vp->setObject(*obj);
    return true;
}

/*
 * Check if CSP allows new Function() or eval() to run in the current
 * principals.
 */
JSBool
js_CheckContentSecurityPolicy(JSContext *cx, JSObject *scopeobj)
{
    // CSP is static per document, so if our check said yes before, that
    // answer is still valid.
    JSObject *global = scopeobj->getGlobal();
    Value v = global->getReservedSlot(JSRESERVED_GLOBAL_EVAL_ALLOWED);
    if (v.isUndefined()) {
        JSSecurityCallbacks *callbacks = JS_GetSecurityCallbacks(cx);

        // if there are callbacks, make sure that the CSP callback is installed and
        // that it permits eval().
        v.setBoolean((!callbacks || !callbacks->contentSecurityPolicyAllows) ||
                     callbacks->contentSecurityPolicyAllows(cx));

        // update the cache in the global object for the result of the security check
        js_SetReservedSlot(cx, global, JSRESERVED_GLOBAL_EVAL_ALLOWED, v);
    }
    return !v.isFalse();
}

/*
 * Check whether principals subsumes scopeobj's principals, and return true
 * if so (or if scopeobj has no principals, for backward compatibility with
 * the JS API, which does not require principals), and false otherwise.
 */
JSBool
js_CheckPrincipalsAccess(JSContext *cx, JSObject *scopeobj,
                         JSPrincipals *principals, JSAtom *caller)
{
    JSSecurityCallbacks *callbacks;
    JSPrincipals *scopePrincipals;

    callbacks = JS_GetSecurityCallbacks(cx);
    if (callbacks && callbacks->findObjectPrincipals) {
        scopePrincipals = callbacks->findObjectPrincipals(cx, scopeobj);
        if (!principals || !scopePrincipals ||
            !principals->subsume(principals, scopePrincipals)) {
            JSAutoByteString callerstr;
            if (!js_AtomToPrintableString(cx, caller, &callerstr))
                return JS_FALSE;
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_BAD_INDIRECT_CALL, callerstr.ptr());
            return JS_FALSE;
        }
    }
    return JS_TRUE;
}

static bool
CheckScopeChainValidity(JSContext *cx, JSObject *scopeobj)
{
    JSObject *inner = scopeobj;
    OBJ_TO_INNER_OBJECT(cx, inner);
    if (!inner)
        return false;
    JS_ASSERT(inner == scopeobj);

    /* XXX This is an awful gross hack. */
    while (scopeobj) {
        JSObjectOp op = scopeobj->getClass()->ext.innerObject;
        if (op && op(cx, scopeobj) != scopeobj) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_INDIRECT_CALL,
                                 js_eval_str);
            return false;
        }
        scopeobj = scopeobj->getParent();
    }

    return true;
}

const char *
js_ComputeFilename(JSContext *cx, JSStackFrame *caller,
                   JSPrincipals *principals, uintN *linenop)
{
    uint32 flags;
#ifdef DEBUG
    JSSecurityCallbacks *callbacks = JS_GetSecurityCallbacks(cx);
#endif

    JS_ASSERT(principals || !(callbacks  && callbacks->findObjectPrincipals));
    flags = JS_GetScriptFilenameFlags(caller->script());
    if ((flags & JSFILENAME_PROTECTED) &&
        principals &&
        strcmp(principals->codebase, "[System Principal]")) {
        *linenop = 0;
        return principals->codebase;
    }

    jsbytecode *pc = caller->pc(cx);
    if (pc && js_GetOpcode(cx, caller->script(), pc) == JSOP_EVAL) {
        JS_ASSERT(js_GetOpcode(cx, caller->script(), pc + JSOP_EVAL_LENGTH) == JSOP_LINENO);
        *linenop = GET_UINT16(pc + JSOP_EVAL_LENGTH);
    } else {
        *linenop = js_FramePCToLineNumber(cx, caller);
    }
    return caller->script()->filename;
}

#ifndef EVAL_CACHE_CHAIN_LIMIT
# define EVAL_CACHE_CHAIN_LIMIT 4
#endif

static inline JSScript **
EvalCacheHash(JSContext *cx, JSLinearString *str)
{
    const jschar *s = str->chars();
    size_t n = str->length();

    if (n > 100)
        n = 100;
    uint32 h;
    for (h = 0; n; s++, n--)
        h = JS_ROTATE_LEFT32(h, 4) ^ *s;

    h *= JS_GOLDEN_RATIO;
    h >>= 32 - JS_EVAL_CACHE_SHIFT;
    return &JS_SCRIPTS_TO_GC(cx)[h];
}

static JS_ALWAYS_INLINE JSScript *
EvalCacheLookup(JSContext *cx, JSLinearString *str, JSStackFrame *caller, uintN staticLevel,
                JSPrincipals *principals, JSObject *scopeobj, JSScript **bucket)
{
    /*
     * Cache local eval scripts indexed by source qualified by scope.
     *
     * An eval cache entry should never be considered a hit unless its
     * strictness matches that of the new eval code. The existing code takes
     * care of this, because hits are qualified by the function from which
     * eval was called, whose strictness doesn't change. (We don't cache evals
     * in eval code, so the calling function corresponds to the calling script,
     * and its strictness never varies.) Scripts produced by calls to eval from
     * global code aren't cached.
     *
     * FIXME bug 620141: Qualify hits by calling script rather than function.
     * Then we wouldn't need the unintuitive !isEvalFrame() hack in EvalKernel
     * to avoid caching nested evals in functions (thus potentially mismatching
     * on strict mode), and we could cache evals in global code if desired.
     */
    uintN count = 0;
    JSScript **scriptp = bucket;

    EVAL_CACHE_METER(probe);
    JSVersion version = cx->findVersion();
    JSScript *script;
    while ((script = *scriptp) != NULL) {
        if (script->savedCallerFun &&
            script->staticLevel == staticLevel &&
            script->getVersion() == version &&
            !script->hasSingletons &&
            (script->principals == principals ||
             (principals->subsume(principals, script->principals) &&
              script->principals->subsume(script->principals, principals)))) {
            /*
             * Get the prior (cache-filling) eval's saved caller function.
             * See Compiler::compileScript in jsparse.cpp.
             */
            JSFunction *fun = script->getFunction(0);

            if (fun == caller->fun()) {
                /*
                 * Get the source string passed for safekeeping in the
                 * atom map by the prior eval to Compiler::compileScript.
                 */
                JSAtom *src = script->atomMap.vector[0];

                if (src == str || EqualStrings(src, str)) {
                    /*
                     * Source matches, qualify by comparing scopeobj to the
                     * COMPILE_N_GO-memoized parent of the first literal
                     * function or regexp object if any. If none, then this
                     * script has no compiled-in dependencies on the prior
                     * eval's scopeobj.
                     */
                    JSObjectArray *objarray = script->objects();
                    int i = 1;

                    if (objarray->length == 1) {
                        if (JSScript::isValidOffset(script->regexpsOffset)) {
                            objarray = script->regexps();
                            i = 0;
                        } else {
                            EVAL_CACHE_METER(noscope);
                            i = -1;
                        }
                    }
                    if (i < 0 ||
                        objarray->vector[i]->getParent() == scopeobj) {
                        JS_ASSERT(staticLevel == script->staticLevel);
                        EVAL_CACHE_METER(hit);
                        *scriptp = script->u.nextToGC;
                        script->u.nextToGC = NULL;
                        return script;
                    }
                }
            }
        }

        if (++count == EVAL_CACHE_CHAIN_LIMIT)
            return NULL;
        EVAL_CACHE_METER(step);
        scriptp = &script->u.nextToGC;
    }
    return NULL;
}

/* ES5 15.1.2.1. */
static JSBool
eval(JSContext *cx, uintN argc, Value *vp)
{
    /*
     * NB: This method handles only indirect eval: direct eval is handled by
     *     JSOP_EVAL.
     */

    JSStackFrame *caller = js_GetScriptedCaller(cx, NULL);

    /* FIXME Bug 602994: This really should be perfectly cromulent. */
    if (!caller) {
        /* Eval code needs to inherit principals from the caller. */
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_BAD_INDIRECT_CALL, js_eval_str);
        return false;
    }

    return EvalKernel(cx, argc, vp, INDIRECT_EVAL, caller, vp[0].toObject().getGlobal());
}

namespace js {

bool
EvalKernel(JSContext *cx, uintN argc, Value *vp, EvalType evalType, JSStackFrame *caller,
           JSObject *scopeobj)
{
    /*
     * FIXME Bug 602994: Calls with no scripted caller should be permitted and
     *       should be implemented as indirect calls.
     */
    JS_ASSERT(caller);
    JS_ASSERT(scopeobj);

    /*
     * We once supported a second argument to eval to use as the scope chain
     * when evaluating the code string.  Warn when such uses are seen so that
     * authors will know that support for eval(s, o) has been removed.
     */
    JSScript *callerScript = caller->script();
    if (argc > 1 && !callerScript->warnedAboutTwoArgumentEval) {
        static const char TWO_ARGUMENT_WARNING[] =
            "Support for eval(code, scopeObject) has been removed. "
            "Use |with (scopeObject) eval(code);| instead.";
        if (!JS_ReportWarning(cx, TWO_ARGUMENT_WARNING))
            return false;
        callerScript->warnedAboutTwoArgumentEval = true;
    }

    /*
     * CSP check: Is eval() allowed at all?
     * Report errors via CSP is done in the script security mgr.
     */
    if (!js_CheckContentSecurityPolicy(cx, scopeobj)) {
        JS_ReportError(cx, "call to eval() blocked by CSP");
        return false;
    }

    /* ES5 15.1.2.1 step 1. */
    if (argc < 1) {
        vp->setUndefined();
        return true;
    }
    if (!vp[2].isString()) {
        *vp = vp[2];
        return true;
    }
    JSString *str = vp[2].toString();

    /* ES5 15.1.2.1 steps 2-8. */
    JSObject *callee = JSVAL_TO_OBJECT(JS_CALLEE(cx, Jsvalify(vp)));
    JS_ASSERT(IsBuiltinEvalFunction(callee->getFunctionPrivate()));
    JSPrincipals *principals = js_EvalFramePrincipals(cx, callee, caller);

    /*
     * Per ES5, indirect eval runs in the global scope. (eval is specified this
     * way so that the compiler can make assumptions about what bindings may or
     * may not exist in the current frame if it doesn't see 'eval'.)
     */
    uintN staticLevel;
    if (evalType == DIRECT_EVAL) {
        staticLevel = caller->script()->staticLevel + 1;

#ifdef DEBUG
        jsbytecode *callerPC = caller->pc(cx);
        JS_ASSERT_IF(caller->isFunctionFrame(), caller->hasCallObj());
        JS_ASSERT(callerPC && js_GetOpcode(cx, caller->script(), callerPC) == JSOP_EVAL);
#endif
    } else {
        /* Pretend that we're top level. */
        staticLevel = 0;

        JS_ASSERT(scopeobj == scopeobj->getGlobal());
        JS_ASSERT(scopeobj->isGlobal());
    }

    /* Ensure we compile this eval with the right object in the scope chain. */
    if (!CheckScopeChainValidity(cx, scopeobj))
        return false;

    JSLinearString *linearStr = str->ensureLinear(cx);
    if (!linearStr)
        return false;
    const jschar *chars = linearStr->chars();
    size_t length = linearStr->length();

    /*
     * If the eval string starts with '(' and ends with ')', it may be JSON.
     * Try the JSON parser first because it's much faster.  If the eval string
     * isn't JSON, JSON parsing will probably fail quickly, so little time
     * will be lost.
     */
    if (length > 2 && chars[0] == '(' && chars[length - 1] == ')') {
        JSONParser *jp = js_BeginJSONParse(cx, vp, /* suppressErrors = */true);
        if (jp != NULL) {
            /* Run JSON-parser on string inside ( and ). */
            bool ok = js_ConsumeJSONText(cx, jp, chars + 1, length - 2);
            ok &= js_FinishJSONParse(cx, jp, NullValue());
            if (ok)
                return true;
        }
    }

    /*
     * Direct calls to eval are supposed to see the caller's |this|. If we
     * haven't wrapped that yet, do so now, before we make a copy of it for
     * the eval code to use.
     */
    if (evalType == DIRECT_EVAL && !caller->computeThis(cx))
        return false;

    JSScript *script = NULL;
    JSScript **bucket = EvalCacheHash(cx, linearStr);
    if (evalType == DIRECT_EVAL && caller->isFunctionFrame() && !caller->isEvalFrame()) {
        script = EvalCacheLookup(cx, linearStr, caller, staticLevel, principals, scopeobj, bucket);

        /*
         * Although the eval cache keeps a script alive from the perspective of
         * the JS engine, from a jsdbgapi user's perspective each eval()
         * creates and destroys a script. This hides implementation details and
         * allows jsdbgapi clients to avoid calling JS_GetScriptObject after a
         * script has been returned to the eval cache, which is invalid since
         * script->u.object aliases script->u.nextToGC.
         */
        if (script) {
            js_CallNewScriptHook(cx, script, NULL);
            MUST_FLOW_THROUGH("destroy");
        }
    }

    /*
     * We can't have a callerFrame (down in js::Execute's terms) if we're in
     * global code (or if we're an indirect eval).
     */
    JSStackFrame *callerFrame = (staticLevel != 0) ? caller : NULL;
    if (!script) {
        uintN lineno;
        const char *filename = js_ComputeFilename(cx, caller, principals, &lineno);

        uint32 tcflags = TCF_COMPILE_N_GO | TCF_NEED_MUTABLE_SCRIPT | TCF_COMPILE_FOR_EVAL;
        script = Compiler::compileScript(cx, scopeobj, callerFrame,
                                         principals, tcflags, chars, length,
                                         filename, lineno, cx->findVersion(),
                                         linearStr, staticLevel);
        if (!script)
            return false;
    }

    assertSameCompartment(cx, scopeobj, script);

    /*
     * Belt-and-braces: check that the lesser of eval's principals and the
     * caller's principals has access to scopeobj.
     */
    JSBool ok = js_CheckPrincipalsAccess(cx, scopeobj, principals,
                                         cx->runtime->atomState.evalAtom) &&
                Execute(cx, scopeobj, script, callerFrame, JSFRAME_EVAL, vp);

    MUST_FLOW_LABEL(destroy);
    js_CallDestroyScriptHook(cx, script);

    script->u.nextToGC = *bucket;
    *bucket = script;
#ifdef CHECK_SCRIPT_OWNER
    script->owner = NULL;
#endif

    return ok;
}

JS_FRIEND_API(bool)
IsBuiltinEvalFunction(JSFunction *fun)
{
    return fun->maybeNative() == eval;
}

}

#if JS_HAS_OBJ_WATCHPOINT

static JSBool
obj_watch_handler(JSContext *cx, JSObject *obj, jsid id, jsval old,
                  jsval *nvp, void *closure)
{
    JSObject *callable;
    JSSecurityCallbacks *callbacks;
    JSStackFrame *caller;
    JSPrincipals *subject, *watcher;
    JSResolvingKey key;
    JSResolvingEntry *entry;
    uint32 generation;
    Value argv[3];
    JSBool ok;

    callable = (JSObject *) closure;

    callbacks = JS_GetSecurityCallbacks(cx);
    if (callbacks && callbacks->findObjectPrincipals) {
        /* Skip over any obj_watch_* frames between us and the real subject. */
        caller = js_GetScriptedCaller(cx, NULL);
        if (caller) {
            /*
             * Only call the watch handler if the watcher is allowed to watch
             * the currently executing script.
             */
            watcher = callbacks->findObjectPrincipals(cx, callable);
            subject = js_StackFramePrincipals(cx, caller);

            if (watcher && subject && !watcher->subsume(watcher, subject)) {
                /* Silently don't call the watch handler. */
                return JS_TRUE;
            }
        }
    }

    /* Avoid recursion on (obj, id) already being watched on cx. */
    key.obj = obj;
    key.id = id;
    if (!js_StartResolving(cx, &key, JSRESFLAG_WATCH, &entry))
        return JS_FALSE;
    if (!entry)
        return JS_TRUE;
    generation = cx->resolvingTable->generation;

    argv[0] = IdToValue(id);
    argv[1] = Valueify(old);
    argv[2] = Valueify(*nvp);
    ok = ExternalInvoke(cx, ObjectValue(*obj), ObjectOrNullValue(callable), 3, argv,
                        Valueify(nvp));
    js_StopResolving(cx, &key, JSRESFLAG_WATCH, entry, generation);
    return ok;
}

static JSBool
obj_watch(JSContext *cx, uintN argc, Value *vp)
{
    if (argc <= 1) {
        js_ReportMissingArg(cx, *vp, 1);
        return JS_FALSE;
    }

    JSObject *callable = js_ValueToCallableObject(cx, &vp[3], 0);
    if (!callable)
        return JS_FALSE;

    /* Compute the unique int/atom symbol id needed by js_LookupProperty. */
    jsid propid;
    if (!ValueToId(cx, vp[2], &propid))
        return JS_FALSE;

    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj)
        return false;

    Value tmp;
    uintN attrs;
    if (!CheckAccess(cx, obj, propid, JSACC_WATCH, &tmp, &attrs))
        return JS_FALSE;

    vp->setUndefined();

    if (attrs & JSPROP_READONLY)
        return JS_TRUE;
    if (obj->isDenseArray() && !obj->makeDenseArraySlow(cx))
        return JS_FALSE;
    return JS_SetWatchPoint(cx, obj, propid, obj_watch_handler, callable);
}

static JSBool
obj_unwatch(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj)
        return false;
    vp->setUndefined();
    jsid id;
    if (argc != 0) {
        if (!ValueToId(cx, vp[2], &id))
            return JS_FALSE;
    } else {
        id = JSID_VOID;
    }
    return JS_ClearWatchPoint(cx, obj, id, NULL, NULL);
}

#endif /* JS_HAS_OBJ_WATCHPOINT */

/*
 * Prototype and property query methods, to complement the 'in' and
 * 'instanceof' operators.
 */

/* Proposed ECMA 15.2.4.5. */
static JSBool
obj_hasOwnProperty(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj)
        return false;
    return js_HasOwnPropertyHelper(cx, obj->getOps()->lookupProperty, argc, vp);
}

JSBool
js_HasOwnPropertyHelper(JSContext *cx, LookupPropOp lookup, uintN argc,
                        Value *vp)
{
    jsid id;
    if (!ValueToId(cx, argc != 0 ? vp[2] : UndefinedValue(), &id))
        return JS_FALSE;

    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj)
        return false;
    JSObject *obj2;
    JSProperty *prop;
    if (obj->isProxy()) {
        bool has;
        if (!JSProxy::hasOwn(cx, obj, id, &has))
            return false;
        vp->setBoolean(has);
        return true;
    }
    if (!js_HasOwnProperty(cx, lookup, obj, id, &obj2, &prop))
        return JS_FALSE;
    vp->setBoolean(!!prop);
    return JS_TRUE;
}

JSBool
js_HasOwnProperty(JSContext *cx, LookupPropOp lookup, JSObject *obj, jsid id,
                  JSObject **objp, JSProperty **propp)
{
    JSAutoResolveFlags rf(cx, JSRESOLVE_QUALIFIED | JSRESOLVE_DETECTING);
    if (!(lookup ? lookup : js_LookupProperty)(cx, obj, id, objp, propp))
        return false;
    if (!*propp)
        return true;

    if (*objp == obj)
        return true;

    Class *clasp = (*objp)->getClass();
    JSObject *outer = NULL;
    if (JSObjectOp op = (*objp)->getClass()->ext.outerObject) {
        outer = op(cx, *objp);
        if (!outer)
            return false;
    }

    if (outer != *objp) {
        if ((*objp)->isNative() && obj->getClass() == clasp) {
            /*
             * The combination of JSPROP_SHARED and JSPROP_PERMANENT in a
             * delegated property makes that property appear to be direct in
             * all delegating instances of the same native class.  This hack
             * avoids bloating every function instance with its own 'length'
             * (AKA 'arity') property.  But it must not extend across class
             * boundaries, to avoid making hasOwnProperty lie (bug 320854).
             *
             * It's not really a hack, of course: a permanent property can't
             * be deleted, and JSPROP_SHARED means "don't allocate a slot in
             * any instance, prototype or delegating".  Without a slot, and
             * without the ability to remove and recreate (with differences)
             * the property, there is no way to tell whether it is directly
             * owned, or indirectly delegated.
             */
            Shape *shape = reinterpret_cast<Shape *>(*propp);
            if (shape->isSharedPermanent())
                return true;
        }

        *propp = NULL;
    }
    return true;
}

/* ES5 15.2.4.6. */
static JSBool
obj_isPrototypeOf(JSContext *cx, uintN argc, Value *vp)
{
    /* Step 1. */
    if (argc < 1 || !vp[2].isObject()) {
        vp->setBoolean(false);
        return true;
    }

    /* Step 2. */
    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj)
        return false;

    /* Step 3. */
    vp->setBoolean(js_IsDelegate(cx, obj, vp[2]));
    return true;
}

/* ES5 15.2.4.7. */
static JSBool
obj_propertyIsEnumerable(JSContext *cx, uintN argc, Value *vp)
{
    /* Step 1. */
    jsid id;
    if (!ValueToId(cx, argc != 0 ? vp[2] : UndefinedValue(), &id))
        return false;

    /* Step 2. */
    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj)
        return false;

    /* Steps 3-5. */
    return js_PropertyIsEnumerable(cx, obj, id, vp);
}

JSBool
js_PropertyIsEnumerable(JSContext *cx, JSObject *obj, jsid id, Value *vp)
{
    JSObject *pobj;
    JSProperty *prop;
    if (!obj->lookupProperty(cx, id, &pobj, &prop))
        return JS_FALSE;

    if (!prop) {
        vp->setBoolean(false);
        return JS_TRUE;
    }

    /*
     * XXX ECMA spec error compatible: return false unless hasOwnProperty.
     * The ECMA spec really should be fixed so propertyIsEnumerable and the
     * for..in loop agree on whether prototype properties are enumerable,
     * obviously by fixing this method (not by breaking the for..in loop!).
     *
     * We check here for shared permanent prototype properties, which should
     * be treated as if they are local to obj.  They are an implementation
     * technique used to satisfy ECMA requirements; users should not be able
     * to distinguish a shared permanent proto-property from a local one.
     */
    bool shared;
    uintN attrs;
    if (pobj->isNative()) {
        Shape *shape = (Shape *) prop;
        shared = shape->isSharedPermanent();
        attrs = shape->attributes();
    } else {
        shared = false;
        if (!pobj->getAttributes(cx, id, &attrs))
            return false;
    }
    if (pobj != obj && !shared) {
        vp->setBoolean(false);
        return true;
    }
    vp->setBoolean((attrs & JSPROP_ENUMERATE) != 0);
    return true;
}

#if OLD_GETTER_SETTER_METHODS

const char js_defineGetter_str[] = "__defineGetter__";
const char js_defineSetter_str[] = "__defineSetter__";
const char js_lookupGetter_str[] = "__lookupGetter__";
const char js_lookupSetter_str[] = "__lookupSetter__";

JS_FRIEND_API(JSBool)
js_obj_defineGetter(JSContext *cx, uintN argc, Value *vp)
{
    if (!BoxThisForVp(cx, vp))
        return false;
    JSObject *obj = &vp[1].toObject();

    if (argc <= 1 || !js_IsCallable(vp[3])) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_BAD_GETTER_OR_SETTER,
                             js_getter_str);
        return JS_FALSE;
    }
    PropertyOp getter = CastAsPropertyOp(&vp[3].toObject());

    jsid id;
    if (!ValueToId(cx, vp[2], &id))
        return JS_FALSE;
    if (!CheckRedeclaration(cx, obj, id, JSPROP_GETTER))
        return JS_FALSE;
    /*
     * Getters and setters are just like watchpoints from an access
     * control point of view.
     */
    Value junk;
    uintN attrs;
    if (!CheckAccess(cx, obj, id, JSACC_WATCH, &junk, &attrs))
        return JS_FALSE;
    vp->setUndefined();
    return obj->defineProperty(cx, id, UndefinedValue(), getter, StrictPropertyStub,
                               JSPROP_ENUMERATE | JSPROP_GETTER | JSPROP_SHARED);
}

JS_FRIEND_API(JSBool)
js_obj_defineSetter(JSContext *cx, uintN argc, Value *vp)
{
    if (!BoxThisForVp(cx, vp))
        return false;
    JSObject *obj = &vp[1].toObject();

    if (argc <= 1 || !js_IsCallable(vp[3])) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_BAD_GETTER_OR_SETTER,
                             js_setter_str);
        return JS_FALSE;
    }
    StrictPropertyOp setter = CastAsStrictPropertyOp(&vp[3].toObject());

    jsid id;
    if (!ValueToId(cx, vp[2], &id))
        return JS_FALSE;
    if (!CheckRedeclaration(cx, obj, id, JSPROP_SETTER))
        return JS_FALSE;
    /*
     * Getters and setters are just like watchpoints from an access
     * control point of view.
     */
    Value junk;
    uintN attrs;
    if (!CheckAccess(cx, obj, id, JSACC_WATCH, &junk, &attrs))
        return JS_FALSE;
    vp->setUndefined();
    return obj->defineProperty(cx, id, UndefinedValue(), PropertyStub, setter,
                               JSPROP_ENUMERATE | JSPROP_SETTER | JSPROP_SHARED);
}

static JSBool
obj_lookupGetter(JSContext *cx, uintN argc, Value *vp)
{
    jsid id;
    if (!ValueToId(cx, argc != 0 ? vp[2] : UndefinedValue(), &id))
        return JS_FALSE;
    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj)
        return JS_FALSE;
    JSObject *pobj;
    JSProperty *prop;
    if (!obj->lookupProperty(cx, id, &pobj, &prop))
        return JS_FALSE;
    vp->setUndefined();
    if (prop) {
        if (pobj->isNative()) {
            Shape *shape = (Shape *) prop;
            if (shape->hasGetterValue())
                *vp = shape->getterValue();
        }
    }
    return JS_TRUE;
}

static JSBool
obj_lookupSetter(JSContext *cx, uintN argc, Value *vp)
{
    jsid id;
    if (!ValueToId(cx, argc != 0 ? vp[2] : UndefinedValue(), &id))
        return JS_FALSE;
    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj)
        return JS_FALSE;
    JSObject *pobj;
    JSProperty *prop;
    if (!obj->lookupProperty(cx, id, &pobj, &prop))
        return JS_FALSE;
    vp->setUndefined();
    if (prop) {
        if (pobj->isNative()) {
            Shape *shape = (Shape *) prop;
            if (shape->hasSetterValue())
                *vp = shape->setterValue();
        }
    }
    return JS_TRUE;
}
#endif /* OLD_GETTER_SETTER_METHODS */

JSBool
obj_getPrototypeOf(JSContext *cx, uintN argc, Value *vp)
{
    if (argc == 0) {
        js_ReportMissingArg(cx, *vp, 0);
        return JS_FALSE;
    }

    if (vp[2].isPrimitive()) {
        char *bytes = DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, vp[2], NULL);
        if (!bytes)
            return JS_FALSE;
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_UNEXPECTED_TYPE, bytes, "not an object");
        JS_free(cx, bytes);
        return JS_FALSE;
    }

    JSObject *obj = &vp[2].toObject();
    uintN attrs;
    return CheckAccess(cx, obj, ATOM_TO_JSID(cx->runtime->atomState.protoAtom),
                       JSACC_PROTO, vp, &attrs);
}

extern JSBool
js_NewPropertyDescriptorObject(JSContext *cx, jsid id, uintN attrs,
                               const Value &getter, const Value &setter,
                               const Value &value, Value *vp)
{
    /* We have our own property, so start creating the descriptor. */
    JSObject *desc = NewBuiltinClassInstance(cx, &js_ObjectClass);
    if (!desc)
        return false;
    vp->setObject(*desc);    /* Root and return. */

    const JSAtomState &atomState = cx->runtime->atomState;
    if (attrs & (JSPROP_GETTER | JSPROP_SETTER)) {
        if (!desc->defineProperty(cx, ATOM_TO_JSID(atomState.getAtom), getter,
                                  PropertyStub, StrictPropertyStub, JSPROP_ENUMERATE) ||
            !desc->defineProperty(cx, ATOM_TO_JSID(atomState.setAtom), setter,
                                  PropertyStub, StrictPropertyStub, JSPROP_ENUMERATE)) {
            return false;
        }
    } else {
        if (!desc->defineProperty(cx, ATOM_TO_JSID(atomState.valueAtom), value,
                                  PropertyStub, StrictPropertyStub, JSPROP_ENUMERATE) ||
            !desc->defineProperty(cx, ATOM_TO_JSID(atomState.writableAtom),
                                  BooleanValue((attrs & JSPROP_READONLY) == 0),
                                  PropertyStub, StrictPropertyStub, JSPROP_ENUMERATE)) {
            return false;
        }
    }

    return desc->defineProperty(cx, ATOM_TO_JSID(atomState.enumerableAtom),
                                BooleanValue((attrs & JSPROP_ENUMERATE) != 0),
                                PropertyStub, StrictPropertyStub, JSPROP_ENUMERATE) &&
           desc->defineProperty(cx, ATOM_TO_JSID(atomState.configurableAtom),
                                BooleanValue((attrs & JSPROP_PERMANENT) == 0),
                                PropertyStub, StrictPropertyStub, JSPROP_ENUMERATE);
}

JSBool
js_GetOwnPropertyDescriptor(JSContext *cx, JSObject *obj, jsid id, Value *vp)
{
    if (obj->isProxy())
        return JSProxy::getOwnPropertyDescriptor(cx, obj, id, false, vp);

    JSObject *pobj;
    JSProperty *prop;
    if (!js_HasOwnProperty(cx, obj->getOps()->lookupProperty, obj, id, &pobj, &prop))
        return false;
    if (!prop) {
        vp->setUndefined();
        return true;
    }

    Value roots[] = { UndefinedValue(), UndefinedValue(), UndefinedValue() };
    AutoArrayRooter tvr(cx, JS_ARRAY_LENGTH(roots), roots);
    unsigned attrs;
    bool doGet = true;
    if (pobj->isNative()) {
        Shape *shape = (Shape *) prop;
        attrs = shape->attributes();
        if (attrs & (JSPROP_GETTER | JSPROP_SETTER)) {
            doGet = false;
            if (attrs & JSPROP_GETTER)
                roots[0] = shape->getterValue();
            if (attrs & JSPROP_SETTER)
                roots[1] = shape->setterValue();
        }
    } else {
        if (!pobj->getAttributes(cx, id, &attrs))
            return false;
    }

    if (doGet && !obj->getProperty(cx, id, &roots[2]))
        return false;

    return js_NewPropertyDescriptorObject(cx, id,
                                          attrs,
                                          roots[0], /* getter */
                                          roots[1], /* setter */
                                          roots[2], /* value */
                                          vp);
}

static bool
GetFirstArgumentAsObject(JSContext *cx, uintN argc, Value *vp, const char *method, JSObject **objp)
{
    if (argc == 0) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_MORE_ARGS_NEEDED,
                             method, "0", "s");
        return false;
    }

    const Value &v = vp[2];
    if (!v.isObject()) {
        char *bytes = DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, v, NULL);
        if (!bytes)
            return false;
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_UNEXPECTED_TYPE,
                             bytes, "not an object");
        JS_free(cx, bytes);
        return false;
    }

    *objp = &v.toObject();
    return true;
}

static JSBool
obj_getOwnPropertyDescriptor(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj;
    if (!GetFirstArgumentAsObject(cx, argc, vp, "Object.getOwnPropertyDescriptor", &obj))
        return JS_FALSE;
    AutoIdRooter nameidr(cx);
    if (!ValueToId(cx, argc >= 2 ? vp[3] : UndefinedValue(), nameidr.addr()))
        return JS_FALSE;
    return js_GetOwnPropertyDescriptor(cx, obj, nameidr.id(), vp);
}

static JSBool
obj_keys(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj;
    if (!GetFirstArgumentAsObject(cx, argc, vp, "Object.keys", &obj))
        return false;

    AutoIdVector props(cx);
    if (!GetPropertyNames(cx, obj, JSITER_OWNONLY, &props))
        return false;

    AutoValueVector vals(cx);
    if (!vals.reserve(props.length()))
        return false;
    for (size_t i = 0, len = props.length(); i < len; i++) {
        jsid id = props[i];
        if (JSID_IS_STRING(id)) {
            JS_ALWAYS_TRUE(vals.append(StringValue(JSID_TO_STRING(id))));
        } else if (JSID_IS_INT(id)) {
            JSString *str = js_IntToString(cx, JSID_TO_INT(id));
            if (!str)
                return false;
            JS_ALWAYS_TRUE(vals.append(StringValue(str)));
        } else {
            JS_ASSERT(JSID_IS_OBJECT(id));
        }
    }

    JS_ASSERT(props.length() <= UINT32_MAX);
    JSObject *aobj = NewDenseCopiedArray(cx, jsuint(vals.length()), vals.begin());
    if (!aobj)
        return false;
    vp->setObject(*aobj);

    return true;
}

static bool
HasProperty(JSContext* cx, JSObject* obj, jsid id, Value* vp, bool *foundp)
{
    if (!obj->hasProperty(cx, id, foundp, JSRESOLVE_QUALIFIED | JSRESOLVE_DETECTING))
        return false;
    if (!*foundp) {
        vp->setUndefined();
        return true;
    }

    /*
     * We must go through the method read barrier in case id is 'get' or 'set'.
     * There is no obvious way to defer cloning a joined function object whose
     * identity will be used by DefinePropertyOnObject, e.g., or reflected via
     * js_GetOwnPropertyDescriptor, as the getter or setter callable object.
     */
    return !!obj->getProperty(cx, id, vp);
}

PropDesc::PropDesc()
  : pd(UndefinedValue()),
    id(INT_TO_JSID(0)),
    value(UndefinedValue()),
    get(UndefinedValue()),
    set(UndefinedValue()),
    attrs(0),
    hasGet(false),
    hasSet(false),
    hasValue(false),
    hasWritable(false),
    hasEnumerable(false),
    hasConfigurable(false)
{
}

bool
PropDesc::initialize(JSContext* cx, jsid id, const Value &origval)
{
    Value v = origval;
    this->id = id;

    /* 8.10.5 step 1 */
    if (v.isPrimitive()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NOT_NONNULL_OBJECT);
        return false;
    }
    JSObject* desc = &v.toObject();

    /* Make a copy of the descriptor. We might need it later. */
    pd = v;

    /* Start with the proper defaults. */
    attrs = JSPROP_PERMANENT | JSPROP_READONLY;

    bool found;

    /* 8.10.5 step 3 */
#ifdef __GNUC__ /* quell GCC overwarning */
    found = false;
#endif
    if (!HasProperty(cx, desc, ATOM_TO_JSID(cx->runtime->atomState.enumerableAtom), &v, &found))
        return false;
    if (found) {
        hasEnumerable = JS_TRUE;
        if (js_ValueToBoolean(v))
            attrs |= JSPROP_ENUMERATE;
    }

    /* 8.10.5 step 4 */
    if (!HasProperty(cx, desc, ATOM_TO_JSID(cx->runtime->atomState.configurableAtom), &v, &found))
        return false;
    if (found) {
        hasConfigurable = JS_TRUE;
        if (js_ValueToBoolean(v))
            attrs &= ~JSPROP_PERMANENT;
    }

    /* 8.10.5 step 5 */
    if (!HasProperty(cx, desc, ATOM_TO_JSID(cx->runtime->atomState.valueAtom), &v, &found))
        return false;
    if (found) {
        hasValue = true;
        value = v;
    }

    /* 8.10.6 step 6 */
    if (!HasProperty(cx, desc, ATOM_TO_JSID(cx->runtime->atomState.writableAtom), &v, &found))
        return false;
    if (found) {
        hasWritable = JS_TRUE;
        if (js_ValueToBoolean(v))
            attrs &= ~JSPROP_READONLY;
    }

    /* 8.10.7 step 7 */
    if (!HasProperty(cx, desc, ATOM_TO_JSID(cx->runtime->atomState.getAtom), &v, &found))
        return false;
    if (found) {
        if ((v.isPrimitive() || !js_IsCallable(v)) && !v.isUndefined()) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_GET_SET_FIELD,
                                 js_getter_str);
            return false;
        }
        hasGet = true;
        get = v;
        attrs |= JSPROP_GETTER | JSPROP_SHARED;
    }

    /* 8.10.7 step 8 */
    if (!HasProperty(cx, desc, ATOM_TO_JSID(cx->runtime->atomState.setAtom), &v, &found))
        return false;
    if (found) {
        if ((v.isPrimitive() || !js_IsCallable(v)) && !v.isUndefined()) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_GET_SET_FIELD,
                                 js_setter_str);
            return false;
        }
        hasSet = true;
        set = v;
        attrs |= JSPROP_SETTER | JSPROP_SHARED;
    }

    /* 8.10.7 step 9 */
    if ((hasGet || hasSet) && (hasValue || hasWritable)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_INVALID_DESCRIPTOR);
        return false;
    }

    return true;
}

static JSBool
Reject(JSContext *cx, uintN errorNumber, bool throwError, jsid id, bool *rval)
{
    if (throwError) {
        jsid idstr;
        if (!js_ValueToStringId(cx, IdToValue(id), &idstr))
           return JS_FALSE;
        JSAutoByteString bytes(cx, JSID_TO_STRING(idstr));
        if (!bytes)
            return JS_FALSE;
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, errorNumber, bytes.ptr());
        return JS_FALSE;
    }

    *rval = false;
    return JS_TRUE;
}

static JSBool
Reject(JSContext *cx, JSObject *obj, uintN errorNumber, bool throwError, bool *rval)
{
    if (throwError) {
        if (js_ErrorFormatString[errorNumber].argCount == 1) {
            js_ReportValueErrorFlags(cx, JSREPORT_ERROR, errorNumber,
                                     JSDVG_IGNORE_STACK, ObjectValue(*obj),
                                     NULL, NULL, NULL);
        } else {
            JS_ASSERT(js_ErrorFormatString[errorNumber].argCount == 0);
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, errorNumber);
        }
        return JS_FALSE;
    }

    *rval = false;
    return JS_TRUE;
}

static JSBool
DefinePropertyOnObject(JSContext *cx, JSObject *obj, const PropDesc &desc,
                       bool throwError, bool *rval)
{
    /* 8.12.9 step 1. */
    JSProperty *current;
    JSObject *obj2;
    JS_ASSERT(!obj->getOps()->lookupProperty);
    if (!js_HasOwnProperty(cx, NULL, obj, desc.id, &obj2, &current))
        return JS_FALSE;

    JS_ASSERT(!obj->getOps()->defineProperty);

    /*
     * If we find a shared permanent property in a different object obj2 from
     * obj, then if the property is shared permanent (an old hack to optimize
     * per-object properties into one prototype property), ignore that lookup
     * result (null current).
     *
     * FIXME: bug 575997 (see also bug 607863).
     */
    if (current && obj2 != obj && obj2->isNative()) {
        /* See same assertion with comment further below. */
        JS_ASSERT(obj2->getClass() == obj->getClass());

        Shape *shape = (Shape *) current;
        if (shape->isSharedPermanent())
            current = NULL;
    }

    /* 8.12.9 steps 2-4. */
    if (!current) {
        if (!obj->isExtensible())
            return Reject(cx, obj, JSMSG_OBJECT_NOT_EXTENSIBLE, throwError, rval);

        *rval = true;

        if (desc.isGenericDescriptor() || desc.isDataDescriptor()) {
            JS_ASSERT(!obj->getOps()->defineProperty);
            return js_DefineProperty(cx, obj, desc.id, &desc.value,
                                     PropertyStub, StrictPropertyStub, desc.attrs);
        }

        JS_ASSERT(desc.isAccessorDescriptor());

        /*
         * Getters and setters are just like watchpoints from an access
         * control point of view.
         */
        Value dummy;
        uintN dummyAttrs;
        if (!CheckAccess(cx, obj, desc.id, JSACC_WATCH, &dummy, &dummyAttrs))
            return JS_FALSE;

        Value tmp = UndefinedValue();
        return js_DefineProperty(cx, obj, desc.id, &tmp,
                                 desc.getter(), desc.setter(), desc.attrs);
    }

    /* 8.12.9 steps 5-6 (note 5 is merely a special case of 6). */
    Value v = UndefinedValue();

    /*
     * In the special case of shared permanent properties, the "own" property
     * can be found on a different object.  In that case the returned property
     * might not be native, except: the shared permanent property optimization
     * is not applied if the objects have different classes (bug 320854), as
     * must be enforced by js_HasOwnProperty for the Shape cast below to be
     * safe.
     */
    JS_ASSERT(obj->getClass() == obj2->getClass());

    const Shape *shape = reinterpret_cast<Shape *>(current);
    do {
        if (desc.isAccessorDescriptor()) {
            if (!shape->isAccessorDescriptor())
                break;

            if (desc.hasGet) {
                JSBool same;
                if (!SameValue(cx, desc.getterValue(), shape->getterOrUndefined(), &same))
                    return JS_FALSE;
                if (!same)
                    break;
            }

            if (desc.hasSet) {
                JSBool same;
                if (!SameValue(cx, desc.setterValue(), shape->setterOrUndefined(), &same))
                    return JS_FALSE;
                if (!same)
                    break;
            }
        } else {
            /*
             * Determine the current value of the property once, if the current
             * value might actually need to be used or preserved later.  NB: we
             * guard on whether the current property is a data descriptor to
             * avoid calling a getter; we won't need the value if it's not a
             * data descriptor.
             */
            if (shape->isDataDescriptor()) {
                /*
                 * We must rule out a non-configurable js::PropertyOp-guarded
                 * property becoming a writable unguarded data property, since
                 * such a property can have its value changed to one the getter
                 * and setter preclude.
                 *
                 * A desc lacking writable but with value is a data descriptor
                 * and we must reject it as if it had writable: true if current
                 * is writable.
                 */
                if (!shape->configurable() &&
                    (!shape->hasDefaultGetter() || !shape->hasDefaultSetter()) &&
                    desc.isDataDescriptor() &&
                    (desc.hasWritable ? desc.writable() : shape->writable()))
                {
                    return Reject(cx, JSMSG_CANT_REDEFINE_PROP, throwError, desc.id, rval);
                }

                if (!js_NativeGet(cx, obj, obj2, shape, JSGET_NO_METHOD_BARRIER, &v))
                    return JS_FALSE;
            }

            if (desc.isDataDescriptor()) {
                if (!shape->isDataDescriptor())
                    break;

                JSBool same;
                if (desc.hasValue) {
                    if (!SameValue(cx, desc.value, v, &same))
                        return JS_FALSE;
                    if (!same) {
                        /*
                         * Insist that a non-configurable js::PropertyOp data
                         * property is frozen at exactly the last-got value.
                         *
                         * Duplicate the first part of the big conjunction that
                         * we tested above, rather than add a local bool flag.
                         * Likewise, don't try to keep shape->writable() in a
                         * flag we veto from true to false for non-configurable
                         * PropertyOp-based data properties and test before the
                         * SameValue check later on in order to re-use that "if
                         * (!SameValue) Reject" logic.
                         *
                         * This function is large and complex enough that it
                         * seems best to repeat a small bit of code and return
                         * Reject(...) ASAP, instead of being clever.
                         */
                        if (!shape->configurable() &&
                            (!shape->hasDefaultGetter() || !shape->hasDefaultSetter()))
                        {
                            return Reject(cx, JSMSG_CANT_REDEFINE_PROP, throwError, desc.id, rval);
                        }
                        break;
                    }
                }
                if (desc.hasWritable && desc.writable() != shape->writable())
                    break;
            } else {
                /* The only fields in desc will be handled below. */
                JS_ASSERT(desc.isGenericDescriptor());
            }
        }

        if (desc.hasConfigurable && desc.configurable() != shape->configurable())
            break;
        if (desc.hasEnumerable && desc.enumerable() != shape->enumerable())
            break;

        /* The conditions imposed by step 5 or step 6 apply. */
        *rval = true;
        return JS_TRUE;
    } while (0);

    /* 8.12.9 step 7. */
    if (!shape->configurable()) {
        /*
         * Since [[Configurable]] defaults to false, we don't need to check
         * whether it was specified.  We can't do likewise for [[Enumerable]]
         * because its putative value is used in a comparison -- a comparison
         * whose result must always be false per spec if the [[Enumerable]]
         * field is not present.  Perfectly pellucid logic, eh?
         */
        JS_ASSERT_IF(!desc.hasConfigurable, !desc.configurable());
        if (desc.configurable() ||
            (desc.hasEnumerable && desc.enumerable() != shape->enumerable())) {
            return Reject(cx, JSMSG_CANT_REDEFINE_PROP, throwError, desc.id, rval);
        }
    }

    bool callDelProperty = false;

    if (desc.isGenericDescriptor()) {
        /* 8.12.9 step 8, no validation required */
    } else if (desc.isDataDescriptor() != shape->isDataDescriptor()) {
        /* 8.12.9 step 9. */
        if (!shape->configurable())
            return Reject(cx, JSMSG_CANT_REDEFINE_PROP, throwError, desc.id, rval);
    } else if (desc.isDataDescriptor()) {
        /* 8.12.9 step 10. */
        JS_ASSERT(shape->isDataDescriptor());
        if (!shape->configurable() && !shape->writable()) {
            if (desc.hasWritable && desc.writable())
                return Reject(cx, JSMSG_CANT_REDEFINE_PROP, throwError, desc.id, rval);
            if (desc.hasValue) {
                JSBool same;
                if (!SameValue(cx, desc.value, v, &same))
                    return JS_FALSE;
                if (!same)
                    return Reject(cx, JSMSG_CANT_REDEFINE_PROP, throwError, desc.id, rval);
            }
        }

        callDelProperty = !shape->hasDefaultGetter() || !shape->hasDefaultSetter();
    } else {
        /* 8.12.9 step 11. */
        JS_ASSERT(desc.isAccessorDescriptor() && shape->isAccessorDescriptor());
        if (!shape->configurable()) {
            if (desc.hasSet) {
                JSBool same;
                if (!SameValue(cx, desc.setterValue(), shape->setterOrUndefined(), &same))
                    return JS_FALSE;
                if (!same)
                    return Reject(cx, JSMSG_CANT_REDEFINE_PROP, throwError, desc.id, rval);
            }

            if (desc.hasGet) {
                JSBool same;
                if (!SameValue(cx, desc.getterValue(), shape->getterOrUndefined(), &same))
                    return JS_FALSE;
                if (!same)
                    return Reject(cx, JSMSG_CANT_REDEFINE_PROP, throwError, desc.id, rval);
            }
        }
    }

    /* 8.12.9 step 12. */
    uintN attrs;
    PropertyOp getter;
    StrictPropertyOp setter;
    if (desc.isGenericDescriptor()) {
        uintN changed = 0;
        if (desc.hasConfigurable)
            changed |= JSPROP_PERMANENT;
        if (desc.hasEnumerable)
            changed |= JSPROP_ENUMERATE;

        attrs = (shape->attributes() & ~changed) | (desc.attrs & changed);
        if (shape->isMethod()) {
            JS_ASSERT(!(attrs & (JSPROP_GETTER | JSPROP_SETTER)));
            getter = PropertyStub;
            setter = StrictPropertyStub;
        } else {
            getter = shape->getter();
            setter = shape->setter();
        }
    } else if (desc.isDataDescriptor()) {
        uintN unchanged = 0;
        if (!desc.hasConfigurable)
            unchanged |= JSPROP_PERMANENT;
        if (!desc.hasEnumerable)
            unchanged |= JSPROP_ENUMERATE;
        if (!desc.hasWritable)
            unchanged |= JSPROP_READONLY;

        if (desc.hasValue)
            v = desc.value;
        attrs = (desc.attrs & ~unchanged) | (shape->attributes() & unchanged);
        getter = PropertyStub;
        setter = StrictPropertyStub;
    } else {
        JS_ASSERT(desc.isAccessorDescriptor());

        /*
         * Getters and setters are just like watchpoints from an access
         * control point of view.
         */
        Value dummy;
        if (!CheckAccess(cx, obj2, desc.id, JSACC_WATCH, &dummy, &attrs))
             return JS_FALSE;

        JS_ASSERT_IF(shape->isMethod(), !(attrs & (JSPROP_GETTER | JSPROP_SETTER)));

        /* 8.12.9 step 12. */
        uintN changed = 0;
        if (desc.hasConfigurable)
            changed |= JSPROP_PERMANENT;
        if (desc.hasEnumerable)
            changed |= JSPROP_ENUMERATE;
        if (desc.hasGet)
            changed |= JSPROP_GETTER | JSPROP_SHARED;
        if (desc.hasSet)
            changed |= JSPROP_SETTER | JSPROP_SHARED;

        attrs = (desc.attrs & changed) | (shape->attributes() & ~changed);
        if (desc.hasGet) {
            getter = desc.getter();
        } else {
            getter = (shape->isMethod() || (shape->hasDefaultGetter() && !shape->hasGetterValue()))
                     ? PropertyStub
                     : shape->getter();
        }
        if (desc.hasSet) {
            setter = desc.setter();
        } else {
            setter = (shape->hasDefaultSetter() && !shape->hasSetterValue())
                     ? StrictPropertyStub
                     : shape->setter();
        }
    }

    *rval = true;

    /*
     * Since "data" properties implemented using native C functions may rely on
     * side effects during setting, we must make them aware that they have been
     * "assigned"; deleting the property before redefining it does the trick.
     * See bug 539766, where we ran into problems when we redefined
     * arguments.length without making the property aware that its value had
     * been changed (which would have happened if we had deleted it before
     * redefining it or we had invoked its setter to change its value).
     */
    if (callDelProperty) {
        Value dummy = UndefinedValue();
        if (!CallJSPropertyOp(cx, obj2->getClass()->delProperty, obj2, desc.id, &dummy))
            return false;
    }

    return js_DefineProperty(cx, obj, desc.id, &v, getter, setter, attrs);
}

static JSBool
DefinePropertyOnArray(JSContext *cx, JSObject *obj, const PropDesc &desc,
                      bool throwError, bool *rval)
{
    /*
     * We probably should optimize dense array property definitions where
     * the descriptor describes a traditional array property (enumerable,
     * configurable, writable, numeric index or length without altering its
     * attributes).  Such definitions are probably unlikely, so we don't bother
     * for now.
     */
    if (obj->isDenseArray() && !obj->makeDenseArraySlow(cx))
        return JS_FALSE;

    jsuint oldLen = obj->getArrayLength();

    if (JSID_IS_ATOM(desc.id, cx->runtime->atomState.lengthAtom)) {
        /*
         * Our optimization of storage of the length property of arrays makes
         * it very difficult to properly implement defining the property.  For
         * now simply throw an exception (NB: not merely Reject) on any attempt
         * to define the "length" property, rather than attempting to implement
         * some difficult-for-authors-to-grasp subset of that functionality.
         */
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_DEFINE_ARRAY_LENGTH);
        return JS_FALSE;
    }

    uint32 index;
    if (js_IdIsIndex(desc.id, &index)) {
        /*
        // Disabled until we support defining "length":
        if (index >= oldLen && lengthPropertyNotWritable())
            return ThrowTypeError(cx, JSMSG_CANT_APPEND_TO_ARRAY);
         */
        if (!DefinePropertyOnObject(cx, obj, desc, false, rval))
            return JS_FALSE;
        if (!*rval)
            return Reject(cx, obj, JSMSG_CANT_DEFINE_ARRAY_INDEX, throwError, rval);

        if (index >= oldLen) {
            JS_ASSERT(index != UINT32_MAX);
            obj->setArrayLength(index + 1);
        }

        *rval = true;
        return JS_TRUE;
    }

    return DefinePropertyOnObject(cx, obj, desc, throwError, rval);
}

static JSBool
DefineProperty(JSContext *cx, JSObject *obj, const PropDesc &desc, bool throwError,
               bool *rval)
{
    if (obj->isArray())
        return DefinePropertyOnArray(cx, obj, desc, throwError, rval);

    if (obj->getOps()->lookupProperty) {
        if (obj->isProxy())
            return JSProxy::defineProperty(cx, obj, desc.id, desc.pd);
        return Reject(cx, obj, JSMSG_OBJECT_NOT_EXTENSIBLE, throwError, rval);
    }

    return DefinePropertyOnObject(cx, obj, desc, throwError, rval);
}

JSBool
js_DefineOwnProperty(JSContext *cx, JSObject *obj, jsid id,
                     const Value &descriptor, JSBool *bp)
{
    AutoPropDescArrayRooter descs(cx);
    PropDesc *desc = descs.append();
    if (!desc || !desc->initialize(cx, id, descriptor))
        return false;

    bool rval;
    if (!DefineProperty(cx, obj, *desc, true, &rval))
        return false;
    *bp = !!rval;
    return true;
}

/* ES5 15.2.3.6: Object.defineProperty(O, P, Attributes) */
static JSBool
obj_defineProperty(JSContext* cx, uintN argc, Value* vp)
{
    /* 15.2.3.6 steps 1 and 5. */
    JSObject *obj;
    if (!GetFirstArgumentAsObject(cx, argc, vp, "Object.defineProperty", &obj))
        return JS_FALSE;
    vp->setObject(*obj);

    /* 15.2.3.6 step 2. */
    AutoIdRooter nameidr(cx);
    if (!ValueToId(cx, argc >= 2 ? vp[3] : UndefinedValue(), nameidr.addr()))
        return JS_FALSE;

    /* 15.2.3.6 step 3. */
    const Value &descval = argc >= 3 ? vp[4] : UndefinedValue();

    /* 15.2.3.6 step 4 */
    JSBool junk;
    return js_DefineOwnProperty(cx, obj, nameidr.id(), descval, &junk);
}

static bool
DefineProperties(JSContext *cx, JSObject *obj, JSObject *props)
{
    AutoIdArray ida(cx, JS_Enumerate(cx, props));
    if (!ida)
        return false;

     AutoPropDescArrayRooter descs(cx);
     size_t len = ida.length();
     for (size_t i = 0; i < len; i++) {
         jsid id = ida[i];
         PropDesc* desc = descs.append();
         AutoValueRooter tvr(cx);
         if (!desc ||
             !JS_GetPropertyById(cx, props, id, tvr.jsval_addr()) ||
             !desc->initialize(cx, id, tvr.value())) {
             return false;
         }
     }

     bool dummy;
     for (size_t i = 0; i < len; i++) {
         if (!DefineProperty(cx, obj, descs[i], true, &dummy))
             return false;
     }

     return true;
}

extern JSBool
js_PopulateObject(JSContext *cx, JSObject *newborn, JSObject *props)
{
    return DefineProperties(cx, newborn, props);
}

/* ES5 15.2.3.7: Object.defineProperties(O, Properties) */
static JSBool
obj_defineProperties(JSContext* cx, uintN argc, Value* vp)
{
    /* 15.2.3.6 steps 1 and 5. */
    JSObject *obj;
    if (!GetFirstArgumentAsObject(cx, argc, vp, "Object.defineProperties", &obj))
        return false;
    vp->setObject(*obj);

    if (argc < 2) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_MORE_ARGS_NEEDED,
                             "Object.defineProperties", "0", "s");
        return false;
    }

    JSObject* props = js_ValueToNonNullObject(cx, vp[3]);
    if (!props)
        return false;
    vp[3].setObject(*props);

    return DefineProperties(cx, obj, props);
}

/* ES5 15.2.3.5: Object.create(O [, Properties]) */
static JSBool
obj_create(JSContext *cx, uintN argc, Value *vp)
{
    if (argc == 0) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_MORE_ARGS_NEEDED,
                             "Object.create", "0", "s");
        return JS_FALSE;
    }

    const Value &v = vp[2];
    if (!v.isObjectOrNull()) {
        char *bytes = DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, v, NULL);
        if (!bytes)
            return JS_FALSE;
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_UNEXPECTED_TYPE,
                             bytes, "not an object or null");
        JS_free(cx, bytes);
        return JS_FALSE;
    }

    /*
     * Use the callee's global as the parent of the new object to avoid dynamic
     * scoping (i.e., using the caller's global).
     */
    JSObject *obj = NewNonFunction<WithProto::Given>(cx, &js_ObjectClass, v.toObjectOrNull(),
                                                        vp->toObject().getGlobal());
    if (!obj)
        return JS_FALSE;
    vp->setObject(*obj); /* Root and prepare for eventual return. */

    /* 15.2.3.5 step 4. */
    if (argc > 1 && !vp[3].isUndefined()) {
        if (vp[3].isPrimitive()) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NOT_NONNULL_OBJECT);
            return JS_FALSE;
        }

        JSObject *props = &vp[3].toObject();
        AutoIdArray ida(cx, JS_Enumerate(cx, props));
        if (!ida)
            return JS_FALSE;

        AutoPropDescArrayRooter descs(cx);
        size_t len = ida.length();
        for (size_t i = 0; i < len; i++) {
            jsid id = ida[i];
            PropDesc *desc = descs.append();
            if (!desc || !JS_GetPropertyById(cx, props, id, Jsvalify(&vp[1])) ||
                !desc->initialize(cx, id, vp[1])) {
                return JS_FALSE;
            }
        }

        bool dummy;
        for (size_t i = 0; i < len; i++) {
            if (!DefineProperty(cx, obj, descs[i], true, &dummy))
                return JS_FALSE;
        }
    }

    /* 5. Return obj. */
    return JS_TRUE;
}

static JSBool
obj_getOwnPropertyNames(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj;
    if (!GetFirstArgumentAsObject(cx, argc, vp, "Object.getOwnPropertyNames", &obj))
        return false;

    AutoIdVector keys(cx);
    if (!GetPropertyNames(cx, obj, JSITER_OWNONLY | JSITER_HIDDEN, &keys))
        return false;

    AutoValueVector vals(cx);
    if (!vals.resize(keys.length()))
        return false;

    for (size_t i = 0, len = keys.length(); i < len; i++) {
         jsid id = keys[i];
         if (JSID_IS_INT(id)) {
             JSString *str = js_ValueToString(cx, Int32Value(JSID_TO_INT(id)));
             if (!str)
                 return false;
             vals[i].setString(str);
         } else if (JSID_IS_ATOM(id)) {
             vals[i].setString(JSID_TO_STRING(id));
         } else {
             vals[i].setObject(*JSID_TO_OBJECT(id));
         }
    }

    JSObject *aobj = NewDenseCopiedArray(cx, vals.length(), vals.begin());
    if (!aobj)
        return false;

    vp->setObject(*aobj);
    return true;
}

static JSBool
obj_isExtensible(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj;
    if (!GetFirstArgumentAsObject(cx, argc, vp, "Object.isExtensible", &obj))
        return false;

    vp->setBoolean(obj->isExtensible());
    return true;
}

static JSBool
obj_preventExtensions(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj;
    if (!GetFirstArgumentAsObject(cx, argc, vp, "Object.preventExtensions", &obj))
        return false;

    vp->setObject(*obj);
    if (!obj->isExtensible())
        return true;

    AutoIdVector props(cx);
    return obj->preventExtensions(cx, &props);
}

bool
JSObject::sealOrFreeze(JSContext *cx, ImmutabilityType it)
{
    assertSameCompartment(cx, this);
    JS_ASSERT(it == SEAL || it == FREEZE);

    AutoIdVector props(cx);
    if (isExtensible()) {
        if (!preventExtensions(cx, &props))
            return false;
    } else {
        if (!GetPropertyNames(cx, this, JSITER_HIDDEN | JSITER_OWNONLY, &props))
            return false;
    }

    /* preventExtensions must slowify dense arrays, so we can assign to holes without checks. */
    JS_ASSERT(!isDenseArray());

    for (size_t i = 0, len = props.length(); i < len; i++) {
        jsid id = props[i];

        uintN attrs;
        if (!getAttributes(cx, id, &attrs))
            return false;

        /* Make all attributes permanent; if freezing, make data attributes read-only. */
        uintN new_attrs;
        if (it == FREEZE && !(attrs & (JSPROP_GETTER | JSPROP_SETTER)))
            new_attrs = JSPROP_PERMANENT | JSPROP_READONLY;
        else
            new_attrs = JSPROP_PERMANENT;

        /* If we already have the attributes we need, skip the setAttributes call. */
        if ((attrs | new_attrs) == attrs)
            continue;

        attrs |= new_attrs;
        if (!setAttributes(cx, id, &attrs))
            return false;
    }

    return true;
}

static JSBool
obj_freeze(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj;
    if (!GetFirstArgumentAsObject(cx, argc, vp, "Object.freeze", &obj))
        return false;

    vp->setObject(*obj);

    return obj->freeze(cx);
}

static JSBool
obj_isFrozen(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj;
    if (!GetFirstArgumentAsObject(cx, argc, vp, "Object.preventExtensions", &obj))
        return false;

    vp->setBoolean(false);

    if (obj->isExtensible())
        return true; /* The JavaScript value returned is false. */

    AutoIdVector props(cx);
    if (!GetPropertyNames(cx, obj, JSITER_HIDDEN | JSITER_OWNONLY, &props))
        return false;

    for (size_t i = 0, len = props.length(); i < len; i++) {
        jsid id = props[i];

        uintN attrs = 0;
        if (!obj->getAttributes(cx, id, &attrs))
            return false;

        /* The property must be non-configurable and either read-only or an accessor. */
        if (!(attrs & JSPROP_PERMANENT) ||
            !(attrs & (JSPROP_READONLY | JSPROP_GETTER | JSPROP_SETTER)))
            return true; /* The JavaScript value returned is false. */
    }

    /* It really was sealed, so return true. */
    vp->setBoolean(true);
    return true;
}

static JSBool
obj_seal(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj;
    if (!GetFirstArgumentAsObject(cx, argc, vp, "Object.seal", &obj))
        return false;

    vp->setObject(*obj);

    return obj->seal(cx);
}

static JSBool
obj_isSealed(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj;
    if (!GetFirstArgumentAsObject(cx, argc, vp, "Object.isSealed", &obj))
        return false;

    /* Assume not sealed until proven otherwise. */
    vp->setBoolean(false);

    if (obj->isExtensible())
        return true; /* The JavaScript value returned is false. */

    AutoIdVector props(cx);
    if (!GetPropertyNames(cx, obj, JSITER_HIDDEN | JSITER_OWNONLY, &props))
        return false;

    for (size_t i = 0, len = props.length(); i < len; i++) {
        jsid id = props[i];

        uintN attrs;
        if (!obj->getAttributes(cx, id, &attrs))
            return false;

        if (!(attrs & JSPROP_PERMANENT))
            return true; /* The JavaScript value returned is false. */
    }

    /* It really was sealed, so return true. */
    vp->setBoolean(true);
    return true;
}

#if JS_HAS_OBJ_WATCHPOINT
const char js_watch_str[] = "watch";
const char js_unwatch_str[] = "unwatch";
#endif
const char js_hasOwnProperty_str[] = "hasOwnProperty";
const char js_isPrototypeOf_str[] = "isPrototypeOf";
const char js_propertyIsEnumerable_str[] = "propertyIsEnumerable";

static JSFunctionSpec object_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,             obj_toSource,                0,0),
#endif
    JS_FN(js_toString_str,             obj_toString,                0,0),
    JS_FN(js_toLocaleString_str,       obj_toLocaleString,          0,0),
    JS_FN(js_valueOf_str,              obj_valueOf,                 0,0),
#if JS_HAS_OBJ_WATCHPOINT
    JS_FN(js_watch_str,                obj_watch,                   2,0),
    JS_FN(js_unwatch_str,              obj_unwatch,                 1,0),
#endif
    JS_FN(js_hasOwnProperty_str,       obj_hasOwnProperty,          1,0),
    JS_FN(js_isPrototypeOf_str,        obj_isPrototypeOf,           1,0),
    JS_FN(js_propertyIsEnumerable_str, obj_propertyIsEnumerable,    1,0),
#if OLD_GETTER_SETTER_METHODS
    JS_FN(js_defineGetter_str,         js_obj_defineGetter,         2,0),
    JS_FN(js_defineSetter_str,         js_obj_defineSetter,         2,0),
    JS_FN(js_lookupGetter_str,         obj_lookupGetter,            1,0),
    JS_FN(js_lookupSetter_str,         obj_lookupSetter,            1,0),
#endif
    JS_FS_END
};

static JSFunctionSpec object_static_methods[] = {
    JS_FN("getPrototypeOf",            obj_getPrototypeOf,          1,0),
    JS_FN("getOwnPropertyDescriptor",  obj_getOwnPropertyDescriptor,2,0),
    JS_FN("keys",                      obj_keys,                    1,0),
    JS_FN("defineProperty",            obj_defineProperty,          3,0),
    JS_FN("defineProperties",          obj_defineProperties,        2,0),
    JS_FN("create",                    obj_create,                  2,0),
    JS_FN("getOwnPropertyNames",       obj_getOwnPropertyNames,     1,0),
    JS_FN("isExtensible",              obj_isExtensible,            1,0),
    JS_FN("preventExtensions",         obj_preventExtensions,       1,0),
    JS_FN("freeze",                    obj_freeze,                  1,0),
    JS_FN("isFrozen",                  obj_isFrozen,                1,0),
    JS_FN("seal",                      obj_seal,                    1,0),
    JS_FN("isSealed",                  obj_isSealed,                1,0),
    JS_FS_END
};

JSBool
js_Object(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj;
    if (argc == 0) {
        /* Trigger logic below to construct a blank object. */
        obj = NULL;
    } else {
        /* If argv[0] is null or undefined, obj comes back null. */
        if (!js_ValueToObjectOrNull(cx, vp[2], &obj))
            return JS_FALSE;
    }
    if (!obj) {
        /* Make an object whether this was called with 'new' or not. */
        JS_ASSERT(!argc || vp[2].isNull() || vp[2].isUndefined());
        gc::FinalizeKind kind = NewObjectGCKind(cx, &js_ObjectClass);
        obj = NewBuiltinClassInstance(cx, &js_ObjectClass, kind);
        if (!obj)
            return JS_FALSE;
    }
    vp->setObject(*obj);
    return JS_TRUE;
}

JSObject*
js_CreateThis(JSContext *cx, JSObject *callee)
{
    Class *clasp = callee->getClass();

    Class *newclasp = &js_ObjectClass;
    if (clasp == &js_FunctionClass) {
        JSFunction *fun = callee->getFunctionPrivate();
        if (fun->isNative() && fun->u.n.clasp)
            newclasp = fun->u.n.clasp;
    }

    Value protov;
    if (!callee->getProperty(cx, ATOM_TO_JSID(cx->runtime->atomState.classPrototypeAtom), &protov))
        return NULL;

    JSObject *proto = protov.isObjectOrNull() ? protov.toObjectOrNull() : NULL;
    JSObject *parent = callee->getParent();
    gc::FinalizeKind kind = NewObjectGCKind(cx, newclasp);
    JSObject *obj = NewObject<WithProto::Class>(cx, newclasp, proto, parent, kind);
    if (obj)
        obj->syncSpecialEquality();
    return obj;
}

JSObject *
js_CreateThisForFunctionWithProto(JSContext *cx, JSObject *callee, JSObject *proto)
{
    gc::FinalizeKind kind = NewObjectGCKind(cx, &js_ObjectClass);
    return NewNonFunction<WithProto::Class>(cx, &js_ObjectClass, proto, callee->getParent(), kind);
}

JSObject *
js_CreateThisForFunction(JSContext *cx, JSObject *callee)
{
    Value protov;
    if (!callee->getProperty(cx,
                             ATOM_TO_JSID(cx->runtime->atomState.classPrototypeAtom),
                             &protov)) {
        return NULL;
    }
    JSObject *proto = protov.isObject() ? &protov.toObject() : NULL;
    return js_CreateThisForFunctionWithProto(cx, callee, proto);
}

#ifdef JS_TRACER

static JS_ALWAYS_INLINE JSObject*
NewObjectWithClassProto(JSContext *cx, Class *clasp, JSObject *proto,
                        /*gc::FinalizeKind*/ unsigned _kind)
{
    JS_ASSERT(clasp->isNative());
    gc::FinalizeKind kind = gc::FinalizeKind(_kind);

    JSObject* obj = js_NewGCObject(cx, kind);
    if (!obj)
        return NULL;

    if (!obj->initSharingEmptyShape(cx, clasp, proto, proto->getParent(), NULL, kind))
        return NULL;
    return obj;
}

JSObject* FASTCALL
js_Object_tn(JSContext* cx, JSObject* proto)
{
    JS_ASSERT(!(js_ObjectClass.flags & JSCLASS_HAS_PRIVATE));
    return NewObjectWithClassProto(cx, &js_ObjectClass, proto, FINALIZE_OBJECT8);
}

JS_DEFINE_TRCINFO_1(js_Object,
    (2, (extern, CONSTRUCTOR_RETRY, js_Object_tn, CONTEXT, CALLEE_PROTOTYPE, 0,
         nanojit::ACCSET_STORE_ANY)))

JSObject* FASTCALL
js_InitializerObject(JSContext* cx, JSObject *proto, JSObject *baseobj)
{
    if (!baseobj) {
        gc::FinalizeKind kind = GuessObjectGCKind(0, false);
        return NewObjectWithClassProto(cx, &js_ObjectClass, proto, kind);
    }

    return CopyInitializerObject(cx, baseobj);
}

JS_DEFINE_CALLINFO_3(extern, OBJECT, js_InitializerObject, CONTEXT, OBJECT, OBJECT,
                     0, nanojit::ACCSET_STORE_ANY)

JSObject* FASTCALL
js_String_tn(JSContext* cx, JSObject* proto, JSString* str)
{
    JS_ASSERT(JS_ON_TRACE(cx));
    JSObject *obj = NewObjectWithClassProto(cx, &js_StringClass, proto, FINALIZE_OBJECT2);
    if (!obj)
        return NULL;
    obj->setPrimitiveThis(StringValue(str));
    return obj;
}
JS_DEFINE_CALLINFO_3(extern, OBJECT, js_String_tn, CONTEXT, CALLEE_PROTOTYPE, STRING, 0,
                     nanojit::ACCSET_STORE_ANY)

JSObject * FASTCALL
js_CreateThisFromTrace(JSContext *cx, JSObject *ctor, uintN protoSlot)
{
#ifdef DEBUG
    JS_ASSERT(ctor->isFunction());
    JS_ASSERT(ctor->getFunctionPrivate()->isInterpreted());
    jsid id = ATOM_TO_JSID(cx->runtime->atomState.classPrototypeAtom);
    const Shape *shape = ctor->nativeLookup(id);
    JS_ASSERT(shape->slot == protoSlot);
    JS_ASSERT(!shape->configurable());
    JS_ASSERT(!shape->isMethod());
#endif

    JSObject *parent = ctor->getParent();
    JSObject *proto;
    const Value &protov = ctor->getSlotRef(protoSlot);
    if (protov.isObject()) {
        proto = &protov.toObject();
    } else {
        /*
         * GetInterpretedFunctionPrototype found that ctor.prototype is
         * primitive. Use Object.prototype for proto, per ES5 13.2.2 step 7.
         */
        if (!js_GetClassPrototype(cx, parent, JSProto_Object, &proto))
            return NULL;
    }

    gc::FinalizeKind kind = NewObjectGCKind(cx, &js_ObjectClass);
    return NewNativeClassInstance(cx, &js_ObjectClass, proto, parent, kind);
}
JS_DEFINE_CALLINFO_3(extern, CONSTRUCTOR_RETRY, js_CreateThisFromTrace, CONTEXT, OBJECT, UINTN, 0,
                     nanojit::ACCSET_STORE_ANY)

#else  /* !JS_TRACER */

# define js_Object_trcinfo NULL

#endif /* !JS_TRACER */

/*
 * Given pc pointing after a property accessing bytecode, return true if the
 * access is "object-detecting" in the sense used by web scripts, e.g., when
 * checking whether document.all is defined.
 */
JS_REQUIRES_STACK JSBool
Detecting(JSContext *cx, jsbytecode *pc)
{
    JSScript *script;
    jsbytecode *endpc;
    JSOp op;
    JSAtom *atom;

    script = cx->fp()->script();
    endpc = script->code + script->length;
    for (;; pc += js_CodeSpec[op].length) {
        JS_ASSERT_IF(!cx->fp()->hasImacropc(), script->code <= pc && pc < endpc);

        /* General case: a branch or equality op follows the access. */
        op = js_GetOpcode(cx, script, pc);
        if (js_CodeSpec[op].format & JOF_DETECTING)
            return JS_TRUE;

        switch (op) {
          case JSOP_NULL:
            /*
             * Special case #1: handle (document.all == null).  Don't sweat
             * about JS1.2's revision of the equality operators here.
             */
            if (++pc < endpc) {
                op = js_GetOpcode(cx, script, pc);
                return *pc == JSOP_EQ || *pc == JSOP_NE;
            }
            return JS_FALSE;

          case JSOP_GETGNAME:
          case JSOP_NAME:
            /*
             * Special case #2: handle (document.all == undefined).  Don't
             * worry about someone redefining undefined, which was added by
             * Edition 3, so is read/write for backward compatibility.
             */
            GET_ATOM_FROM_BYTECODE(script, pc, 0, atom);
            if (atom == cx->runtime->atomState.typeAtoms[JSTYPE_VOID] &&
                (pc += js_CodeSpec[op].length) < endpc) {
                op = js_GetOpcode(cx, script, pc);
                return op == JSOP_EQ || op == JSOP_NE ||
                       op == JSOP_STRICTEQ || op == JSOP_STRICTNE;
            }
            return JS_FALSE;

          default:
            /*
             * At this point, anything but an extended atom index prefix means
             * we're not detecting.
             */
            if (!(js_CodeSpec[op].format & JOF_INDEXBASE))
                return JS_FALSE;
            break;
        }
    }
}

/*
 * Infer lookup flags from the currently executing bytecode. This does
 * not attempt to infer JSRESOLVE_WITH, because the current bytecode
 * does not indicate whether we are in a with statement. Return defaultFlags
 * if a currently executing bytecode cannot be determined.
 */
uintN
js_InferFlags(JSContext *cx, uintN defaultFlags)
{
#ifdef JS_TRACER
    if (JS_ON_TRACE(cx))
        return JS_TRACE_MONITOR_ON_TRACE(cx)->bailExit->lookupFlags;
#endif

    JS_ASSERT_NOT_ON_TRACE(cx);

    jsbytecode *pc;
    const JSCodeSpec *cs;
    uint32 format;
    uintN flags = 0;

    JSStackFrame *const fp = js_GetTopStackFrame(cx);
    if (!fp || !(pc = cx->regs->pc))
        return defaultFlags;
    cs = &js_CodeSpec[js_GetOpcode(cx, fp->script(), pc)];
    format = cs->format;
    if (JOF_MODE(format) != JOF_NAME)
        flags |= JSRESOLVE_QUALIFIED;
    if ((format & (JOF_SET | JOF_FOR)) || fp->isAssigning()) {
        flags |= JSRESOLVE_ASSIGNING;
    } else if (cs->length >= 0) {
        pc += cs->length;
        JSScript *script = cx->fp()->script();
        if (pc < script->code + script->length && Detecting(cx, pc))
            flags |= JSRESOLVE_DETECTING;
    }
    if (format & JOF_DECLARING)
        flags |= JSRESOLVE_DECLARING;
    return flags;
}

/*
 * ObjectOps and Class for with-statement stack objects.
 */
static JSBool
with_LookupProperty(JSContext *cx, JSObject *obj, jsid id, JSObject **objp,
                    JSProperty **propp)
{
    /* Fixes bug 463997 */
    uintN flags = cx->resolveFlags;
    if (flags == JSRESOLVE_INFER)
        flags = js_InferFlags(cx, flags);
    flags |= JSRESOLVE_WITH;
    JSAutoResolveFlags rf(cx, flags);
    return obj->getProto()->lookupProperty(cx, id, objp, propp);
}

static JSBool
with_GetProperty(JSContext *cx, JSObject *obj, JSObject *receiver, jsid id, Value *vp)
{
    return obj->getProto()->getProperty(cx, id, vp);
}

static JSBool
with_SetProperty(JSContext *cx, JSObject *obj, jsid id, Value *vp, JSBool strict)
{
    return obj->getProto()->setProperty(cx, id, vp, strict);
}

static JSBool
with_GetAttributes(JSContext *cx, JSObject *obj, jsid id, uintN *attrsp)
{
    return obj->getProto()->getAttributes(cx, id, attrsp);
}

static JSBool
with_SetAttributes(JSContext *cx, JSObject *obj, jsid id, uintN *attrsp)
{
    return obj->getProto()->setAttributes(cx, id, attrsp);
}

static JSBool
with_DeleteProperty(JSContext *cx, JSObject *obj, jsid id, Value *rval, JSBool strict)
{
    return obj->getProto()->deleteProperty(cx, id, rval, strict);
}

static JSBool
with_Enumerate(JSContext *cx, JSObject *obj, JSIterateOp enum_op,
               Value *statep, jsid *idp)
{
    return obj->getProto()->enumerate(cx, enum_op, statep, idp);
}

static JSType
with_TypeOf(JSContext *cx, JSObject *obj)
{
    return JSTYPE_OBJECT;
}

static JSObject *
with_ThisObject(JSContext *cx, JSObject *obj)
{
    return obj->getWithThis();
}

Class js_WithClass = {
    "With",
    JSCLASS_HAS_PRIVATE | JSCLASS_HAS_RESERVED_SLOTS(2) | JSCLASS_IS_ANONYMOUS,
    PropertyStub,         /* addProperty */
    PropertyStub,         /* delProperty */
    PropertyStub,         /* getProperty */
    StrictPropertyStub,   /* setProperty */
    EnumerateStub,
    ResolveStub,
    ConvertStub,
    NULL,                 /* finalize */
    NULL,                 /* reserved    */
    NULL,                 /* checkAccess */
    NULL,                 /* call        */
    NULL,                 /* construct   */
    NULL,                 /* xdrObject   */
    NULL,                 /* hasInstance */
    NULL,                 /* mark        */
    JS_NULL_CLASS_EXT,
    {
        with_LookupProperty,
        NULL,             /* defineProperty */
        with_GetProperty,
        with_SetProperty,
        with_GetAttributes,
        with_SetAttributes,
        with_DeleteProperty,
        with_Enumerate,
        with_TypeOf,
        NULL,             /* trace */
        NULL,             /* fix   */
        with_ThisObject,
        NULL,             /* clear */
    }
};

JS_REQUIRES_STACK JSObject *
js_NewWithObject(JSContext *cx, JSObject *proto, JSObject *parent, jsint depth)
{
    JSObject *obj;

    obj = js_NewGCObject(cx, FINALIZE_OBJECT2);
    if (!obj)
        return NULL;

    JSStackFrame *priv = js_FloatingFrameIfGenerator(cx, cx->fp());

    obj->init(cx, &js_WithClass, proto, parent, priv, false);
    obj->setMap(cx->compartment->emptyWithShape);
    OBJ_SET_BLOCK_DEPTH(cx, obj, depth);

    AutoObjectRooter tvr(cx, obj);
    JSObject *thisp = proto->thisObject(cx);
    if (!thisp)
        return NULL;

    assertSameCompartment(cx, obj, thisp);

    obj->setWithThis(thisp);
    return obj;
}

JSObject *
js_NewBlockObject(JSContext *cx)
{
    /*
     * Null obj's proto slot so that Object.prototype.* does not pollute block
     * scopes and to give the block object its own scope.
     */
    JSObject *blockObj = js_NewGCObject(cx, FINALIZE_OBJECT2);
    if (!blockObj)
        return NULL;

    blockObj->init(cx, &js_BlockClass, NULL, NULL, NULL, false);
    blockObj->setMap(cx->compartment->emptyBlockShape);
    return blockObj;
}

JSObject *
js_CloneBlockObject(JSContext *cx, JSObject *proto, JSStackFrame *fp)
{
    JS_ASSERT(proto->isStaticBlock());

    size_t count = OBJ_BLOCK_COUNT(cx, proto);
    gc::FinalizeKind kind = gc::GetGCObjectKind(count + 1);

    JSObject *clone = js_NewGCObject(cx, kind);
    if (!clone)
        return NULL;

    JSStackFrame *priv = js_FloatingFrameIfGenerator(cx, fp);

    /* The caller sets parent on its own. */
    clone->init(cx, &js_BlockClass, proto, NULL, priv, false);

    clone->setMap(proto->map);
    if (!clone->ensureInstanceReservedSlots(cx, count + 1))
        return NULL;

    clone->setSlot(JSSLOT_BLOCK_DEPTH, proto->getSlot(JSSLOT_BLOCK_DEPTH));

    JS_ASSERT(clone->isClonedBlock());
    return clone;
}

JS_REQUIRES_STACK JSBool
js_PutBlockObject(JSContext *cx, JSBool normalUnwind)
{
    JSStackFrame *const fp = cx->fp();
    JSObject *obj = &fp->scopeChain();
    JS_ASSERT(obj->isClonedBlock());
    JS_ASSERT(obj->getPrivate() == js_FloatingFrameIfGenerator(cx, cx->fp()));

    /* Block objects should have all reserved slots allocated early. */
    uintN count = OBJ_BLOCK_COUNT(cx, obj);
    JS_ASSERT(obj->numSlots() >= JSSLOT_BLOCK_DEPTH + 1 + count);

    /* The block and its locals must be on the current stack for GC safety. */
    uintN depth = OBJ_BLOCK_DEPTH(cx, obj);
    JS_ASSERT(depth <= size_t(cx->regs->sp - fp->base()));
    JS_ASSERT(count <= size_t(cx->regs->sp - fp->base() - depth));

    /* See comments in CheckDestructuring from jsparse.cpp. */
    JS_ASSERT(count >= 1);

    if (normalUnwind) {
        uintN slot = JSSLOT_BLOCK_FIRST_FREE_SLOT;
        depth += fp->numFixed();
        memcpy(obj->getSlots() + slot, fp->slots() + depth, count * sizeof(Value));
    }

    /* We must clear the private slot even with errors. */
    obj->setPrivate(NULL);
    fp->setScopeChainNoCallObj(*obj->getParent());
    return normalUnwind;
}

static JSBool
block_getProperty(JSContext *cx, JSObject *obj, jsid id, Value *vp)
{
    /*
     * Block objects are never exposed to script, and the engine handles them
     * with care. So unlike other getters, this one can assert (rather than
     * check) certain invariants about obj.
     */
    JS_ASSERT(obj->isClonedBlock());
    uintN index = (uintN) JSID_TO_INT(id);
    JS_ASSERT(index < OBJ_BLOCK_COUNT(cx, obj));

    JSStackFrame *fp = (JSStackFrame *) obj->getPrivate();
    if (fp) {
        fp = js_LiveFrameIfGenerator(fp);
        index += fp->numFixed() + OBJ_BLOCK_DEPTH(cx, obj);
        JS_ASSERT(index < fp->numSlots());
        *vp = fp->slots()[index];
        return true;
    }

    /* Values are in slots immediately following the class-reserved ones. */
    JS_ASSERT(obj->getSlot(JSSLOT_FREE(&js_BlockClass) + index) == *vp);
    return true;
}

static JSBool
block_setProperty(JSContext *cx, JSObject *obj, jsid id, JSBool strict, Value *vp)
{
    JS_ASSERT(obj->isClonedBlock());
    uintN index = (uintN) JSID_TO_INT(id);
    JS_ASSERT(index < OBJ_BLOCK_COUNT(cx, obj));

    JSStackFrame *fp = (JSStackFrame *) obj->getPrivate();
    if (fp) {
        fp = js_LiveFrameIfGenerator(fp);
        index += fp->numFixed() + OBJ_BLOCK_DEPTH(cx, obj);
        JS_ASSERT(index < fp->numSlots());
        fp->slots()[index] = *vp;
        return true;
    }

    /*
     * The value in *vp will be written back to the slot in obj that was
     * allocated when this let binding was defined.
     */
    return true;
}

const Shape *
JSObject::defineBlockVariable(JSContext *cx, jsid id, intN index)
{
    JS_ASSERT(isStaticBlock());

    /* Use JSPROP_ENUMERATE to aid the disassembler. */
    uint32 slot = JSSLOT_FREE(&js_BlockClass) + index;
    const Shape *shape = addProperty(cx, id,
                                     block_getProperty, block_setProperty,
                                     slot, JSPROP_ENUMERATE | JSPROP_PERMANENT,
                                     Shape::HAS_SHORTID, index);
    if (!shape)
        return NULL;
    if (slot >= numSlots() && !growSlots(cx, slot + 1))
        return NULL;
    return shape;
}

static size_t
GetObjectSize(JSObject *obj)
{
    return (obj->isFunction() && !obj->getPrivate())
           ? sizeof(JSFunction)
           : sizeof(JSObject) + sizeof(js::Value) * obj->numFixedSlots();
}

bool
JSObject::copyPropertiesFrom(JSContext *cx, JSObject *obj)
{
    // If we're not native, then we cannot copy properties.
    JS_ASSERT(isNative() == obj->isNative());
    if (!isNative())
        return true;

    AutoShapeVector shapes(cx);
    for (Shape::Range r(obj->lastProperty()); !r.empty(); r.popFront()) {
        if (!shapes.append(&r.front()))
            return false;
    }

    size_t n = shapes.length();
    while (n > 0) {
        const Shape *shape = shapes[--n];
        uintN attrs = shape->attributes();
        PropertyOp getter = shape->getter();
        if ((attrs & JSPROP_GETTER) && !cx->compartment->wrap(cx, &getter))
            return false;
        StrictPropertyOp setter = shape->setter();
        if ((attrs & JSPROP_SETTER) && !cx->compartment->wrap(cx, &setter))
            return false;
        Value v = shape->hasSlot() ? obj->getSlot(shape->slot) : UndefinedValue();
        if (!cx->compartment->wrap(cx, &v))
            return false;
        if (!defineProperty(cx, shape->id, v, getter, setter, attrs))
            return false;
    }
    return true;
}

static bool
CopySlots(JSContext *cx, JSObject *from, JSObject *to)
{
    JS_ASSERT(!from->isNative() && !to->isNative());
    size_t nslots = from->numSlots();
    if (to->ensureSlots(cx, nslots))
        return false;

    size_t n = 0;
    if (to->isWrapper() &&
        (JSWrapper::wrapperHandler(to)->flags() & JSWrapper::CROSS_COMPARTMENT)) {
        to->slots[0] = from->slots[0];
        to->slots[1] = from->slots[1];
        n = 2;
    }

    for (; n < nslots; ++n) {
        Value v = from->slots[n];
        if (!cx->compartment->wrap(cx, &v))
            return false;
        to->slots[n] = v;
    }
    return true;
}

JSObject *
JSObject::clone(JSContext *cx, JSObject *proto, JSObject *parent)
{
    /*
     * We can only clone native objects and proxies. Dense arrays are slowified if
     * we try to clone them.
     */
    if (!isNative()) {
        if (isDenseArray()) {
            if (!makeDenseArraySlow(cx))
                return NULL;
        } else if (!isProxy()) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_CANT_CLONE_OBJECT);
            return NULL;
        }
    }
    JSObject *clone = NewObject<WithProto::Given>(cx, getClass(),
                                                  proto, parent,
                                                  gc::FinalizeKind(finalizeKind()));
    if (!clone)
        return NULL;
    if (isNative()) {
        if (clone->isFunction() && (compartment() != clone->compartment())) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_CANT_CLONE_OBJECT);
            return NULL;
        }

        if (getClass()->flags & JSCLASS_HAS_PRIVATE)
            clone->setPrivate(getPrivate());
    } else {
        JS_ASSERT(isProxy());
        if (!CopySlots(cx, this, clone))
            return NULL;
    }
    return clone;
}

static void
TradeGuts(JSObject *a, JSObject *b)
{
    JS_ASSERT(a->compartment() == b->compartment());
    JS_ASSERT(a->isFunction() == b->isFunction());

    /*
     * Regexp guts are more complicated -- we would need to migrate the
     * refcounted JIT code blob for them across compartments instead of just
     * swapping guts.
     */
    JS_ASSERT(!a->isRegExp() && !b->isRegExp());

    bool aInline = !a->hasSlotsArray();
    bool bInline = !b->hasSlotsArray();

    /* Trade the guts of the objects. */
    const size_t size = GetObjectSize(a);
    if (size == GetObjectSize(b)) {
        /*
         * If the objects are the same size, then we make no assumptions about
         * whether they have dynamically allocated slots and instead just copy
         * them over wholesale.
         */
        char tmp[tl::Max<sizeof(JSFunction), sizeof(JSObject_Slots16)>::result];
        JS_ASSERT(size <= sizeof(tmp));

        memcpy(tmp, a, size);
        memcpy(a, b, size);
        memcpy(b, tmp, size);

        /* Fixup pointers for inline slots on the objects. */
        if (aInline)
            b->slots = b->fixedSlots();
        if (bInline)
            a->slots = a->fixedSlots();
    } else {
        /*
         * If the objects are of differing sizes, then we only copy over the
         * JSObject portion (things like class, etc.) and leave it to
         * JSObject::clone to copy over the dynamic slots for us.
         */
        if (a->isFunction()) {
            JSFunction tmp;
            memcpy(&tmp, a, sizeof tmp);
            memcpy(a, b, sizeof tmp);
            memcpy(b, &tmp, sizeof tmp);
        } else {
            JSObject tmp;
            memcpy(&tmp, a, sizeof tmp);
            memcpy(a, b, sizeof tmp);
            memcpy(b, &tmp, sizeof tmp);
        }

        JS_ASSERT(!aInline);
        JS_ASSERT(!bInline);
    }
}

/*
 * Use this method with extreme caution. It trades the guts of two objects and updates
 * scope ownership. This operation is not thread-safe, just as fast array to slow array
 * transitions are inherently not thread-safe. Don't perform a swap operation on objects
 * shared across threads or, or bad things will happen. You have been warned.
 */
bool
JSObject::swap(JSContext *cx, JSObject *other)
{
    /*
     * If we are swapping objects with a different number of builtin slots, force
     * both to not use their inline slots.
     */
    if (GetObjectSize(this) != GetObjectSize(other)) {
        if (!hasSlotsArray()) {
            if (!allocSlots(cx, numSlots()))
                return false;
        }
        if (!other->hasSlotsArray()) {
            if (!other->allocSlots(cx, other->numSlots()))
                return false;
        }
    }

    if (this->compartment() == other->compartment()) {
        TradeGuts(this, other);
        return true;
    }

    JSObject *thisClone;
    JSObject *otherClone;
    {
        AutoCompartment ac(cx, other);
        if (!ac.enter())
            return false;
        thisClone = this->clone(cx, other->getProto(), other->getParent());
        if (!thisClone || !thisClone->copyPropertiesFrom(cx, this))
            return false;
    }
    {
        AutoCompartment ac(cx, this);
        if (!ac.enter())
            return false;
        otherClone = other->clone(cx, other->getProto(), other->getParent());
        if (!otherClone || !otherClone->copyPropertiesFrom(cx, other))
            return false;
    }
    TradeGuts(this, otherClone);
    TradeGuts(other, thisClone);

    return true;
}

#if JS_HAS_XDR

#define NO_PARENT_INDEX ((uint32)-1)

uint32
FindObjectIndex(JSObjectArray *array, JSObject *obj)
{
    size_t i;

    if (array) {
        i = array->length;
        do {

            if (array->vector[--i] == obj)
                return i;
        } while (i != 0);
    }

    return NO_PARENT_INDEX;
}

JSBool
js_XDRBlockObject(JSXDRState *xdr, JSObject **objp)
{
    JSContext *cx;
    uint32 parentId;
    JSObject *obj, *parent;
    uintN depth, count;
    uint32 depthAndCount;
    const Shape *shape;

    cx = xdr->cx;
#ifdef __GNUC__
    obj = NULL;         /* quell GCC overwarning */
#endif

    if (xdr->mode == JSXDR_ENCODE) {
        obj = *objp;
        parent = obj->getParent();
        parentId = JSScript::isValidOffset(xdr->script->objectsOffset)
                   ? FindObjectIndex(xdr->script->objects(), parent)
                   : NO_PARENT_INDEX;
        depth = (uint16)OBJ_BLOCK_DEPTH(cx, obj);
        count = (uint16)OBJ_BLOCK_COUNT(cx, obj);
        depthAndCount = (uint32)(depth << 16) | count;
    }
#ifdef __GNUC__ /* suppress bogus gcc warnings */
    else count = 0;
#endif

    /* First, XDR the parent atomid. */
    if (!JS_XDRUint32(xdr, &parentId))
        return JS_FALSE;

    if (xdr->mode == JSXDR_DECODE) {
        obj = js_NewBlockObject(cx);
        if (!obj)
            return JS_FALSE;
        *objp = obj;

        /*
         * If there's a parent id, then get the parent out of our script's
         * object array. We know that we XDR block object in outer-to-inner
         * order, which means that getting the parent now will work.
         */
        if (parentId == NO_PARENT_INDEX)
            parent = NULL;
        else
            parent = xdr->script->getObject(parentId);
        obj->setParent(parent);
    }

    AutoObjectRooter tvr(cx, obj);

    if (!JS_XDRUint32(xdr, &depthAndCount))
        return false;

    if (xdr->mode == JSXDR_DECODE) {
        depth = (uint16)(depthAndCount >> 16);
        count = (uint16)depthAndCount;
        obj->setSlot(JSSLOT_BLOCK_DEPTH, Value(Int32Value(depth)));

        /*
         * XDR the block object's properties. We know that there are 'count'
         * properties to XDR, stored as id/shortid pairs.
         */
        for (uintN i = 0; i < count; i++) {
            JSAtom *atom;
            uint16 shortid;

            /* XDR the real id, then the shortid. */
            if (!js_XDRAtom(xdr, &atom) || !JS_XDRUint16(xdr, &shortid))
                return false;

            if (!obj->defineBlockVariable(cx, ATOM_TO_JSID(atom), shortid))
                return false;
        }
    } else {
        AutoShapeVector shapes(cx);
        shapes.growBy(count);

        for (Shape::Range r(obj->lastProperty()); !r.empty(); r.popFront()) {
            shape = &r.front();
            shapes[shape->shortid] = shape;
        }

        /*
         * XDR the block object's properties. We know that there are 'count'
         * properties to XDR, stored as id/shortid pairs.
         */
        for (uintN i = 0; i < count; i++) {
            shape = shapes[i];
            JS_ASSERT(shape->getter() == block_getProperty);

            jsid propid = shape->id;
            JS_ASSERT(JSID_IS_ATOM(propid));
            JSAtom *atom = JSID_TO_ATOM(propid);

            uint16 shortid = uint16(shape->shortid);
            JS_ASSERT(shortid == i);

            /* XDR the real id, then the shortid. */
            if (!js_XDRAtom(xdr, &atom) || !JS_XDRUint16(xdr, &shortid))
                return false;
        }
    }
    return true;
}

#endif

Class js_BlockClass = {
    "Block",
    JSCLASS_HAS_PRIVATE | JSCLASS_HAS_RESERVED_SLOTS(1) | JSCLASS_IS_ANONYMOUS,
    PropertyStub,         /* addProperty */
    PropertyStub,         /* delProperty */
    PropertyStub,         /* getProperty */
    StrictPropertyStub,   /* setProperty */
    EnumerateStub,
    ResolveStub,
    ConvertStub
};

JSObject *
js_InitObjectClass(JSContext *cx, JSObject *obj)
{
    JSObject *proto = js_InitClass(cx, obj, NULL, &js_ObjectClass, js_Object, 1,
                                   object_props, object_methods, NULL, object_static_methods);
    if (!proto)
        return NULL;

    /* ECMA (15.1.2.1) says 'eval' is a property of the global object. */
    jsid id = ATOM_TO_JSID(cx->runtime->atomState.evalAtom);
    if (!js_DefineFunction(cx, obj, id, eval, 1, JSFUN_STUB_GSOPS))
        return NULL;

    return proto;
}

static bool
DefineStandardSlot(JSContext *cx, JSObject *obj, JSProtoKey key, JSAtom *atom,
                   const Value &v, uint32 attrs, bool &named)
{
    jsid id = ATOM_TO_JSID(atom);

    if (key != JSProto_Null) {
        /*
         * Initializing an actual standard class on a global object. If the
         * property is not yet present, force it into a new one bound to a
         * reserved slot. Otherwise, go through the normal property path.
         */
        JS_ASSERT(obj->isGlobal());
        JS_ASSERT(obj->isNative());

        if (!obj->ensureClassReservedSlots(cx))
            return false;

        const Shape *shape = obj->nativeLookup(id);
        if (!shape) {
            uint32 slot = 2 * JSProto_LIMIT + key;
            if (!js_SetReservedSlot(cx, obj, slot, v))
                return false;
            if (!obj->addProperty(cx, id, PropertyStub, StrictPropertyStub, slot, attrs, 0, 0))
                return false;

            named = true;
            return true;
        }
    }

    named = obj->defineProperty(cx, id, v, PropertyStub, StrictPropertyStub, attrs);
    return named;
}

namespace js {

JSObject *
DefineConstructorAndPrototype(JSContext *cx, JSObject *obj, JSProtoKey key, JSAtom *atom,
                              JSObject *protoProto, Class *clasp,
                              Native constructor, uintN nargs,
                              JSPropertySpec *ps, JSFunctionSpec *fs,
                              JSPropertySpec *static_ps, JSFunctionSpec *static_fs)
{
    /*
     * Create a prototype object for this class.
     *
     * FIXME: lazy standard (built-in) class initialization and even older
     * eager boostrapping code rely on all of these properties:
     *
     * 1. NewObject attempting to compute a default prototype object when
     *    passed null for proto; and
     *
     * 2. NewObject tolerating no default prototype (null proto slot value)
     *    due to this js_InitClass call coming from js_InitFunctionClass on an
     *    otherwise-uninitialized global.
     *
     * 3. NewObject allocating a JSFunction-sized GC-thing when clasp is
     *    &js_FunctionClass, not a JSObject-sized (smaller) GC-thing.
     *
     * The JS_NewObjectForGivenProto and JS_NewObject APIs also allow clasp to
     * be &js_FunctionClass (we could break compatibility easily). But fixing
     * (3) is not enough without addressing the bootstrapping dependency on (1)
     * and (2).
     */
    JSObject *proto = NewObject<WithProto::Class>(cx, clasp, protoProto, obj);
    if (!proto)
        return NULL;

    proto->syncSpecialEquality();

    /* After this point, control must exit via label bad or out. */
    AutoObjectRooter tvr(cx, proto);

    JSObject *ctor;
    bool named = false;
    if (!constructor) {
        /*
         * Lacking a constructor, name the prototype (e.g., Math) unless this
         * class (a) is anonymous, i.e. for internal use only; (b) the class
         * of obj (the global object) is has a reserved slot indexed by key;
         * and (c) key is not the null key.
         */
        if (!(clasp->flags & JSCLASS_IS_ANONYMOUS) || !obj->isGlobal() || key == JSProto_Null) {
            uint32 attrs = (clasp->flags & JSCLASS_IS_ANONYMOUS)
                           ? JSPROP_READONLY | JSPROP_PERMANENT
                           : 0;
            if (!DefineStandardSlot(cx, obj, key, atom, ObjectValue(*proto), attrs, named))
                goto bad;
        }

        ctor = proto;
    } else {
        JSFunction *fun = js_NewFunction(cx, NULL, constructor, nargs, JSFUN_CONSTRUCTOR, obj, atom);
        if (!fun)
            goto bad;

        AutoValueRooter tvr2(cx, ObjectValue(*fun));
        if (!DefineStandardSlot(cx, obj, key, atom, tvr2.value(), 0, named))
            goto bad;

        /*
         * Remember the class this function is a constructor for so that
         * we know to create an object of this class when we call the
         * constructor.
         */
        FUN_CLASP(fun) = clasp;

        /*
         * Optionally construct the prototype object, before the class has
         * been fully initialized.  Allow the ctor to replace proto with a
         * different object, as is done for operator new -- and as at least
         * XML support requires.
         */
        ctor = FUN_OBJECT(fun);
        if (clasp->flags & JSCLASS_CONSTRUCT_PROTOTYPE) {
            Value rval;
            if (!InvokeConstructorWithGivenThis(cx, proto, ObjectOrNullValue(ctor),
                                                0, NULL, &rval)) {
                goto bad;
            }
            if (rval.isObject() && &rval.toObject() != proto)
                proto = &rval.toObject();
        }

        /* Connect constructor and prototype by named properties. */
        if (!js_SetClassPrototype(cx, ctor, proto,
                                  JSPROP_READONLY | JSPROP_PERMANENT)) {
            goto bad;
        }

        /* Bootstrap Function.prototype (see also JS_InitStandardClasses). */
        if (ctor->getClass() == clasp)
            ctor->setProto(proto);
    }

    /* Add properties and methods to the prototype and the constructor. */
    if ((ps && !JS_DefineProperties(cx, proto, ps)) ||
        (fs && !JS_DefineFunctions(cx, proto, fs)) ||
        (static_ps && !JS_DefineProperties(cx, ctor, static_ps)) ||
        (static_fs && !JS_DefineFunctions(cx, ctor, static_fs))) {
        goto bad;
    }

    /*
     * Pre-brand the prototype and constructor if they have built-in methods.
     * This avoids extra shape guard branch exits in the tracejitted code.
     */
    if (fs)
        proto->brand(cx);
    if (ctor != proto && static_fs)
        ctor->brand(cx);

    /*
     * Make sure proto's emptyShape is available to be shared by objects of
     * this class.  JSObject::emptyShape is a one-slot cache. If we omit this,
     * some other class could snap it up. (The risk is particularly great for
     * Object.prototype.)
     *
     * All callers of JSObject::initSharingEmptyShape depend on this.
     *
     * FIXME: bug 592296 -- js_InitArrayClass should pass &js_SlowArrayClass
     * and make the Array.prototype slow from the start.
     */
    JS_ASSERT_IF(proto->clasp != clasp,
                 clasp == &js_ArrayClass && proto->clasp == &js_SlowArrayClass);
    if (!proto->getEmptyShape(cx, proto->clasp, FINALIZE_OBJECT0))
        goto bad;

    if (clasp->flags & (JSCLASS_FREEZE_PROTO|JSCLASS_FREEZE_CTOR)) {
        JS_ASSERT_IF(ctor == proto, !(clasp->flags & JSCLASS_FREEZE_CTOR));
        if (proto && (clasp->flags & JSCLASS_FREEZE_PROTO) && !proto->freeze(cx))
            goto bad;
        if (ctor && (clasp->flags & JSCLASS_FREEZE_CTOR) && !ctor->freeze(cx))
            goto bad;
    }

    /* If this is a standard class, cache its prototype. */
    if (key != JSProto_Null && !js_SetClassObject(cx, obj, key, ctor, proto))
        goto bad;

    return proto;

bad:
    if (named) {
        Value rval;
        obj->deleteProperty(cx, ATOM_TO_JSID(atom), &rval, false);
    }
    return NULL;
}

}

JSObject *
js_InitClass(JSContext *cx, JSObject *obj, JSObject *protoProto,
             Class *clasp, Native constructor, uintN nargs,
             JSPropertySpec *ps, JSFunctionSpec *fs,
             JSPropertySpec *static_ps, JSFunctionSpec *static_fs)
{
    JSAtom *atom = js_Atomize(cx, clasp->name, strlen(clasp->name), 0);
    if (!atom)
        return NULL;

    /*
     * All instances of the class will inherit properties from the prototype
     * object we are about to create (in DefineConstructorAndPrototype), which
     * in turn will inherit from protoProto.
     *
     * When initializing a standard class (other than Object), if protoProto is
     * null, default to the Object prototype object. The engine's internal uses
     * of js_InitClass depend on this nicety. Note that in
     * js_InitFunctionAndObjectClasses, we specially hack the resolving table
     * and then depend on js_GetClassPrototype here leaving protoProto NULL and
     * returning true.
     */
    JSProtoKey key = JSCLASS_CACHED_PROTO_KEY(clasp);
    if (key != JSProto_Null &&
        !protoProto &&
        !js_GetClassPrototype(cx, obj, JSProto_Object, &protoProto)) {
        return NULL;
    }

    return DefineConstructorAndPrototype(cx, obj, key, atom, protoProto, clasp, constructor, nargs,
                                         ps, fs, static_ps, static_fs);
}

bool
JSObject::allocSlots(JSContext *cx, size_t newcap)
{
    uint32 oldcap = numSlots();

    JS_ASSERT(newcap >= oldcap && !hasSlotsArray());

    if (newcap > NSLOTS_LIMIT) {
        if (!JS_ON_TRACE(cx))
            js_ReportAllocationOverflow(cx);
        return false;
    }

    Value *tmpslots = (Value*) cx->malloc(newcap * sizeof(Value));
    if (!tmpslots)
        return false;  /* Leave slots at inline buffer. */
    slots = tmpslots;
    capacity = newcap;

    /* Copy over anything from the inline buffer. */
    memcpy(slots, fixedSlots(), oldcap * sizeof(Value));
    ClearValueRange(slots + oldcap, newcap - oldcap, isDenseArray());
    return true;
}

bool
JSObject::growSlots(JSContext *cx, size_t newcap)
{
    /*
     * When an object with CAPACITY_DOUBLING_MAX or fewer slots needs to
     * grow, double its capacity, to add N elements in amortized O(N) time.
     *
     * Above this limit, grow by 12.5% each time. Speed is still amortized
     * O(N), with a higher constant factor, and we waste less space.
     */
    static const size_t CAPACITY_DOUBLING_MAX = 1024 * 1024;
    static const size_t CAPACITY_CHUNK = CAPACITY_DOUBLING_MAX / sizeof(Value);

    uint32 oldcap = numSlots();
    JS_ASSERT(oldcap < newcap);

    uint32 nextsize = (oldcap <= CAPACITY_DOUBLING_MAX)
                    ? oldcap * 2
                    : oldcap + (oldcap >> 3);

    uint32 actualCapacity = JS_MAX(newcap, nextsize);
    if (actualCapacity >= CAPACITY_CHUNK)
        actualCapacity = JS_ROUNDUP(actualCapacity, CAPACITY_CHUNK);
    else if (actualCapacity < SLOT_CAPACITY_MIN)
        actualCapacity = SLOT_CAPACITY_MIN;

    /* Don't let nslots get close to wrapping around uint32. */
    if (actualCapacity >= NSLOTS_LIMIT) {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    /* If nothing was allocated yet, treat it as initial allocation. */
    if (!hasSlotsArray())
        return allocSlots(cx, actualCapacity);

    Value *tmpslots = (Value*) cx->realloc(slots, oldcap * sizeof(Value), actualCapacity * sizeof(Value));
    if (!tmpslots)
        return false;    /* Leave dslots as its old size. */
    slots = tmpslots;
    capacity = actualCapacity;

    /* Initialize the additional slots we added. */
    ClearValueRange(slots + oldcap, actualCapacity - oldcap, isDenseArray());
    return true;
}

void
JSObject::shrinkSlots(JSContext *cx, size_t newcap)
{
    uint32 oldcap = numSlots();
    JS_ASSERT(newcap <= oldcap);
    JS_ASSERT(newcap >= slotSpan());

    if (oldcap <= SLOT_CAPACITY_MIN || !hasSlotsArray()) {
        /* We won't shrink the slots any more.  Clear excess holes. */
        ClearValueRange(slots + newcap, oldcap - newcap, isDenseArray());
        return;
    }

    uint32 fill = newcap;
    if (newcap < SLOT_CAPACITY_MIN)
        newcap = SLOT_CAPACITY_MIN;
    if (newcap < numFixedSlots())
        newcap = numFixedSlots();

    Value *tmpslots = (Value*) cx->realloc(slots, newcap * sizeof(Value));
    if (!tmpslots)
        return;  /* Leave slots at its old size. */
    slots = tmpslots;
    capacity = newcap;

    if (fill < newcap) {
        /* Clear any excess holes if we tried to shrink below SLOT_CAPACITY_MIN. */
        ClearValueRange(slots + fill, newcap - fill, isDenseArray());
    }
}

bool
JSObject::ensureInstanceReservedSlots(JSContext *cx, size_t nreserved)
{
    JS_ASSERT_IF(isNative(),
                 isBlock() || isCall() || (isFunction() && isBoundFunction()));

    uintN nslots = JSSLOT_FREE(clasp) + nreserved;
    return nslots <= numSlots() || allocSlots(cx, nslots);
}

static JSObject *
js_InitNullClass(JSContext *cx, JSObject *obj)
{
    JS_ASSERT(0);
    return NULL;
}

#define JS_PROTO(name,code,init) extern JSObject *init(JSContext *, JSObject *);
#include "jsproto.tbl"
#undef JS_PROTO

static JSObjectOp lazy_prototype_init[JSProto_LIMIT] = {
#define JS_PROTO(name,code,init) init,
#include "jsproto.tbl"
#undef JS_PROTO
};

namespace js {

bool
SetProto(JSContext *cx, JSObject *obj, JSObject *proto, bool checkForCycles)
{
    JS_ASSERT_IF(!checkForCycles, obj != proto);
    JS_ASSERT(obj->isExtensible());

    if (obj->isNative()) {
        if (!obj->ensureClassReservedSlots(cx))
            return false;
    }

    /*
     * Regenerate property cache shape ids for all of the scopes along the
     * old prototype chain to invalidate their property cache entries, in
     * case any entries were filled by looking up through obj.
     */
    JSObject *oldproto = obj;
    while (oldproto && oldproto->isNative()) {
        oldproto->protoShapeChange(cx);
        oldproto = oldproto->getProto();
    }

    if (!proto || !checkForCycles) {
        obj->setProto(proto);
    } else if (!SetProtoCheckingForCycles(cx, obj, proto)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CYCLIC_VALUE, js_proto_str);
        return false;
    }
    return true;
}

}

JSBool
js_GetClassObject(JSContext *cx, JSObject *obj, JSProtoKey key,
                  JSObject **objp)
{
    JSObject *cobj;
    JSResolvingKey rkey;
    JSResolvingEntry *rentry;
    uint32 generation;
    JSObjectOp init;
    Value v;

    obj = obj->getGlobal();
    if (!obj->isGlobal()) {
        *objp = NULL;
        return JS_TRUE;
    }

    v = obj->getReservedSlot(key);
    if (v.isObject()) {
        *objp = &v.toObject();
        return JS_TRUE;
    }

    rkey.obj = obj;
    rkey.id = ATOM_TO_JSID(cx->runtime->atomState.classAtoms[key]);
    if (!js_StartResolving(cx, &rkey, JSRESFLAG_LOOKUP, &rentry))
        return JS_FALSE;
    if (!rentry) {
        /* Already caching key in obj -- suppress recursion. */
        *objp = NULL;
        return JS_TRUE;
    }
    generation = cx->resolvingTable->generation;

    JSBool ok = true;
    cobj = NULL;
    init = lazy_prototype_init[key];
    if (init) {
        if (!init(cx, obj)) {
            ok = JS_FALSE;
        } else {
            v = obj->getReservedSlot(key);
            if (v.isObject())
                cobj = &v.toObject();
        }
    }

    js_StopResolving(cx, &rkey, JSRESFLAG_LOOKUP, rentry, generation);
    *objp = cobj;
    return ok;
}

JSBool
js_SetClassObject(JSContext *cx, JSObject *obj, JSProtoKey key, JSObject *cobj, JSObject *proto)
{
    JS_ASSERT(!obj->getParent());
    if (!obj->isGlobal())
        return JS_TRUE;

    return js_SetReservedSlot(cx, obj, key, ObjectOrNullValue(cobj)) &&
           js_SetReservedSlot(cx, obj, JSProto_LIMIT + key, ObjectOrNullValue(proto));
}

JSBool
js_FindClassObject(JSContext *cx, JSObject *start, JSProtoKey protoKey,
                   Value *vp, Class *clasp)
{
    JSStackFrame *fp;
    JSObject *obj, *cobj, *pobj;
    jsid id;
    JSProperty *prop;
    const Shape *shape;

    /*
     * Find the global object. Use cx->fp() directly to avoid falling off
     * trace; all JIT-elided stack frames have the same global object as
     * cx->fp().
     */
    VOUCH_DOES_NOT_REQUIRE_STACK();
    if (!start && (fp = cx->maybefp()) != NULL)
        start = &fp->scopeChain();

    if (start) {
        /* Find the topmost object in the scope chain. */
        do {
            obj = start;
            start = obj->getParent();
        } while (start);
    } else {
        obj = cx->globalObject;
        if (!obj) {
            vp->setUndefined();
            return JS_TRUE;
        }
    }

    OBJ_TO_INNER_OBJECT(cx, obj);
    if (!obj)
        return JS_FALSE;

    if (protoKey != JSProto_Null) {
        JS_ASSERT(JSProto_Null < protoKey);
        JS_ASSERT(protoKey < JSProto_LIMIT);
        if (!js_GetClassObject(cx, obj, protoKey, &cobj))
            return JS_FALSE;
        if (cobj) {
            vp->setObject(*cobj);
            return JS_TRUE;
        }
        id = ATOM_TO_JSID(cx->runtime->atomState.classAtoms[protoKey]);
    } else {
        JSAtom *atom = js_Atomize(cx, clasp->name, strlen(clasp->name), 0);
        if (!atom)
            return false;
        id = ATOM_TO_JSID(atom);
    }

    JS_ASSERT(obj->isNative());
    if (js_LookupPropertyWithFlags(cx, obj, id, JSRESOLVE_CLASSNAME,
                                   &pobj, &prop) < 0) {
        return JS_FALSE;
    }
    Value v = UndefinedValue();
    if (prop && pobj->isNative()) {
        shape = (Shape *) prop;
        if (pobj->containsSlot(shape->slot)) {
            v = pobj->nativeGetSlot(shape->slot);
            if (v.isPrimitive())
                v.setUndefined();
        }
    }
    *vp = v;
    return JS_TRUE;
}

JSObject *
js_ConstructObject(JSContext *cx, Class *clasp, JSObject *proto, JSObject *parent,
                   uintN argc, Value *argv)
{
    AutoArrayRooter argtvr(cx, argc, argv);

    JSProtoKey protoKey = GetClassProtoKey(clasp);

    /* Protect constructor in case a crazy getter for .prototype uproots it. */
    AutoValueRooter tvr(cx);
    if (!js_FindClassObject(cx, parent, protoKey, tvr.addr(), clasp))
        return NULL;

    const Value &cval = tvr.value();
    if (tvr.value().isPrimitive()) {
        js_ReportIsNotFunction(cx, tvr.addr(), JSV2F_CONSTRUCT | JSV2F_SEARCH_STACK);
        return NULL;
    }

    /*
     * If proto is NULL, set it to Constructor.prototype, just like JSOP_NEW
     * does, likewise for the new object's parent.
     */
    JSObject *ctor = &cval.toObject();
    if (!parent)
        parent = ctor->getParent();
    if (!proto) {
        Value rval;
        if (!ctor->getProperty(cx, ATOM_TO_JSID(cx->runtime->atomState.classPrototypeAtom),
                               &rval)) {
            return NULL;
        }
        if (rval.isObjectOrNull())
            proto = rval.toObjectOrNull();
    }

    JSObject *obj = NewObject<WithProto::Class>(cx, clasp, proto, parent);
    if (!obj)
        return NULL;

    obj->syncSpecialEquality();

    Value rval;
    if (!InvokeConstructorWithGivenThis(cx, obj, cval, argc, argv, &rval))
        return NULL;

    if (rval.isPrimitive())
        return obj;

    /*
     * If the instance's class differs from what was requested, throw a type
     * error.  If the given class has both the JSCLASS_HAS_PRIVATE and the
     * JSCLASS_CONSTRUCT_PROTOTYPE flags, and the instance does not have its
     * private data set at this point, then the constructor was replaced and
     * we should throw a type error.
     */
    obj = &rval.toObject();
    if (obj->getClass() != clasp ||
        (!(~clasp->flags & (JSCLASS_HAS_PRIVATE |
                            JSCLASS_CONSTRUCT_PROTOTYPE)) &&
         !obj->getPrivate())) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_WRONG_CONSTRUCTOR, clasp->name);
        return NULL;
    }
    return obj;
}

bool
JSObject::allocSlot(JSContext *cx, uint32 *slotp)
{
    uint32 slot = slotSpan();
    JS_ASSERT(slot >= JSSLOT_FREE(clasp));

    /*
     * If this object is in dictionary mode and it has a property table, try to
     * pull a free slot from the property table's slot-number freelist.
     */
    if (inDictionaryMode() && lastProp->hasTable()) {
        uint32 &last = lastProp->getTable()->freelist;
        if (last != SHAPE_INVALID_SLOT) {
#ifdef DEBUG
            JS_ASSERT(last < slot);
            uint32 next = getSlot(last).toPrivateUint32();
            JS_ASSERT_IF(next != SHAPE_INVALID_SLOT, next < slot);
#endif

            *slotp = last;

            Value &vref = getSlotRef(last);
            last = vref.toPrivateUint32();
            vref.setUndefined();
            return true;
        }
    }

    if (slot >= numSlots() && !growSlots(cx, slot + 1))
        return false;

    /* JSObject::growSlots or JSObject::freeSlot should set the free slots to void. */
    JS_ASSERT(getSlot(slot).isUndefined());
    *slotp = slot;
    return true;
}

bool
JSObject::freeSlot(JSContext *cx, uint32 slot)
{
    uint32 limit = slotSpan();
    JS_ASSERT(slot < limit);

    Value &vref = getSlotRef(slot);
    if (inDictionaryMode() && lastProp->hasTable()) {
        uint32 &last = lastProp->getTable()->freelist;

        /* Can't afford to check the whole freelist, but let's check the head. */
        JS_ASSERT_IF(last != SHAPE_INVALID_SLOT, last < limit && last != slot);

        /*
         * Freeing a slot other than the last one mapped by this object's
         * shape (and not a reserved slot; see bug 595230): push the slot onto
         * the dictionary property table's freelist. We want to let the last
         * slot be freed by shrinking the dslots vector; see js_TraceObject.
         */
        if (JSSLOT_FREE(clasp) <= slot && slot + 1 < limit) {
            JS_ASSERT_IF(last != SHAPE_INVALID_SLOT, last < slotSpan());
            vref.setPrivateUint32(last);
            last = slot;
            return true;
        }
    }
    vref.setUndefined();
    return false;
}

/* JSBOXEDWORD_INT_MAX as a string */
#define JSBOXEDWORD_INT_MAX_STRING "1073741823"

/*
 * Convert string indexes that convert to int jsvals as ints to save memory.
 * Care must be taken to use this macro every time a property name is used, or
 * else double-sets, incorrect property cache misses, or other mistakes could
 * occur.
 */
jsid
js_CheckForStringIndex(jsid id)
{
    if (!JSID_IS_ATOM(id))
        return id;

    JSAtom *atom = JSID_TO_ATOM(id);
    JSString *str = ATOM_TO_STRING(atom);
    const jschar *s = str->flatChars();
    jschar ch = *s;

    JSBool negative = (ch == '-');
    if (negative)
        ch = *++s;

    if (!JS7_ISDEC(ch))
        return id;

    size_t n = str->flatLength() - negative;
    if (n > sizeof(JSBOXEDWORD_INT_MAX_STRING) - 1)
        return id;

    const jschar *cp = s;
    const jschar *end = s + n;

    jsuint index = JS7_UNDEC(*cp++);
    jsuint oldIndex = 0;
    jsuint c = 0;

    if (index != 0) {
        while (JS7_ISDEC(*cp)) {
            oldIndex = index;
            c = JS7_UNDEC(*cp);
            index = 10 * index + c;
            cp++;
        }
    }

    /*
     * Non-integer indexes can't be represented as integers.  Also, distinguish
     * index "-0" from "0", because JSBOXEDWORD_INT cannot.
     */
    if (cp != end || (negative && index == 0))
        return id;

    if (negative) {
        if (oldIndex < -(JSID_INT_MIN / 10) ||
            (oldIndex == -(JSID_INT_MIN / 10) && c <= (-JSID_INT_MIN % 10)))
        {
            id = INT_TO_JSID(-jsint(index));
        }
    } else {
        if (oldIndex < JSID_INT_MAX / 10 ||
            (oldIndex == JSID_INT_MAX / 10 && c <= (JSID_INT_MAX % 10)))
        {
            id = INT_TO_JSID(jsint(index));
        }
    }

    return id;
}

static JSBool
PurgeProtoChain(JSContext *cx, JSObject *obj, jsid id)
{
    const Shape *shape;

    while (obj) {
        if (!obj->isNative()) {
            obj = obj->getProto();
            continue;
        }
        shape = obj->nativeLookup(id);
        if (shape) {
            PCMETER(JS_PROPERTY_CACHE(cx).pcpurges++);
            obj->shadowingShapeChange(cx, *shape);

            if (!obj->getParent()) {
                /*
                 * All scope chains end in a global object, so this will change
                 * the global shape. jstracer.cpp assumes that the global shape
                 * never changes on trace, so we must deep-bail here.
                 */
                LeaveTrace(cx);
            }
            return JS_TRUE;
        }
        obj = obj->getProto();
    }
    return JS_FALSE;
}

void
js_PurgeScopeChainHelper(JSContext *cx, JSObject *obj, jsid id)
{
    JS_ASSERT(obj->isDelegate());
    PurgeProtoChain(cx, obj->getProto(), id);

    /*
     * We must purge the scope chain only for Call objects as they are the only
     * kind of cacheable non-global object that can gain properties after outer
     * properties with the same names have been cached or traced. Call objects
     * may gain such properties via eval introducing new vars; see bug 490364.
     */
    if (obj->isCall()) {
        while ((obj = obj->getParent()) != NULL) {
            if (PurgeProtoChain(cx, obj, id))
                break;
        }
    }
}

const Shape *
js_AddNativeProperty(JSContext *cx, JSObject *obj, jsid id,
                     PropertyOp getter, StrictPropertyOp setter, uint32 slot,
                     uintN attrs, uintN flags, intN shortid)
{
    JS_ASSERT(!(flags & Shape::METHOD));

    /*
     * Purge the property cache of now-shadowed id in obj's scope chain. Do
     * this optimistically (assuming no failure below) before locking obj, so
     * we can lock the shadowed scope.
     */
    js_PurgeScopeChain(cx, obj, id);

    if (!obj->ensureClassReservedSlots(cx))
        return NULL;

    /* Convert string indices to integers if appropriate. */
    id = js_CheckForStringIndex(id);
    return obj->putProperty(cx, id, getter, setter, slot, attrs, flags, shortid);
}

const Shape *
js_ChangeNativePropertyAttrs(JSContext *cx, JSObject *obj,
                             const Shape *shape, uintN attrs, uintN mask,
                             PropertyOp getter, StrictPropertyOp setter)
{
    if (!obj->ensureClassReservedSlots(cx))
        return NULL;

    /*
     * Check for freezing an object with shape-memoized methods here, on a
     * shape-by-shape basis. Note that getter may be a pun of the method's
     * joined function object value, to indicate "no getter change". In this
     * case we must null getter to get the desired PropertyStub behavior.
     */
    if ((attrs & JSPROP_READONLY) && shape->isMethod()) {
        JSObject *funobj = &shape->methodObject();
        Value v = ObjectValue(*funobj);

        shape = obj->methodReadBarrier(cx, *shape, &v);
        if (!shape)
            return NULL;

        if (CastAsObject(getter) == funobj) {
            JS_ASSERT(!(attrs & JSPROP_GETTER));
            getter = NULL;
        }
    }

    return obj->changeProperty(cx, shape, attrs, mask, getter, setter);
}

JSBool
js_DefineProperty(JSContext *cx, JSObject *obj, jsid id, const Value *value,
                  PropertyOp getter, StrictPropertyOp setter, uintN attrs)
{
    return js_DefineNativeProperty(cx, obj, id, *value, getter, setter, attrs,
                                   0, 0, NULL);
}

/*
 * Backward compatibility requires allowing addProperty hooks to mutate the
 * nominal initial value of a slotful property, while GC safety wants that
 * value to be stored before the call-out through the hook.  Optimize to do
 * both while saving cycles for classes that stub their addProperty hook.
 */
static inline bool
CallAddPropertyHook(JSContext *cx, Class *clasp, JSObject *obj, const Shape *shape, Value *vp)
{
    if (clasp->addProperty != PropertyStub) {
        Value nominal = *vp;

        if (!CallJSPropertyOp(cx, clasp->addProperty, obj, shape->id, vp))
            return false;
        if (*vp != nominal) {
            if (obj->containsSlot(shape->slot))
                obj->nativeSetSlot(shape->slot, *vp);
        }
    }
    return true;
}

JSBool
js_DefineNativeProperty(JSContext *cx, JSObject *obj, jsid id, const Value &value,
                        PropertyOp getter, StrictPropertyOp setter, uintN attrs,
                        uintN flags, intN shortid, JSProperty **propp,
                        uintN defineHow /* = 0 */)
{
    JS_ASSERT((defineHow & ~(JSDNP_CACHE_RESULT | JSDNP_DONT_PURGE | JSDNP_SET_METHOD)) == 0);
    LeaveTraceIfGlobalObject(cx, obj);

    /* Convert string indices to integers if appropriate. */
    id = js_CheckForStringIndex(id);

    /*
     * If defining a getter or setter, we must check for its counterpart and
     * update the attributes and property ops.  A getter or setter is really
     * only half of a property.
     */
    const Shape *shape = NULL;
    if (attrs & (JSPROP_GETTER | JSPROP_SETTER)) {
        JSObject *pobj;
        JSProperty *prop;

        /*
         * If JS_THREADSAFE and id is found, js_LookupProperty returns with
         * shape non-null and pobj locked.  If pobj == obj, the property is
         * already in obj and obj has its own (mutable) scope.  So if we are
         * defining a getter whose setter was already defined, or vice versa,
         * finish the job via obj->changeProperty, and refresh the property
         * cache line for (obj, id) to map shape.
         */
        if (!js_LookupProperty(cx, obj, id, &pobj, &prop))
            return JS_FALSE;
        shape = (Shape *) prop;
        if (shape && pobj == obj && shape->isAccessorDescriptor()) {
            shape = obj->changeProperty(cx, shape, attrs,
                                        JSPROP_GETTER | JSPROP_SETTER,
                                        (attrs & JSPROP_GETTER)
                                        ? getter
                                        : shape->getter(),
                                        (attrs & JSPROP_SETTER)
                                        ? setter
                                        : shape->setter());

            if (!shape)
                return false;
        } else if (prop) {
            prop = NULL;
            shape = NULL;
        }
    }

    /*
     * Purge the property cache of any properties named by id that are about
     * to be shadowed in obj's scope chain unless it is known a priori that it
     * is not possible. We do this before locking obj to avoid nesting locks.
     */
    if (!(defineHow & JSDNP_DONT_PURGE))
        js_PurgeScopeChain(cx, obj, id);

    /*
     * Check whether a readonly property or setter is being defined on a known
     * prototype object. See the comment in jscntxt.h before protoHazardShape's
     * member declaration.
     */
    if (obj->isDelegate() && (attrs & (JSPROP_READONLY | JSPROP_SETTER)))
        cx->runtime->protoHazardShape = js_GenerateShape(cx);

    /* Use the object's class getter and setter by default. */
    Class *clasp = obj->getClass();
    if (!(defineHow & JSDNP_SET_METHOD)) {
        if (!getter && !(attrs & JSPROP_GETTER))
            getter = clasp->getProperty;
        if (!setter && !(attrs & JSPROP_SETTER))
            setter = clasp->setProperty;
    }

    /* Get obj's own scope if it has one, or create a new one for obj. */
    if (!obj->ensureClassReservedSlots(cx))
        return false;

    /*
     * Make a local copy of value, in case a method barrier needs to update the
     * value to define, and just so addProperty can mutate its inout parameter.
     */
    Value valueCopy = value;
    bool adding = false;

    if (!shape) {
        /* Add a new property, or replace an existing one of the same id. */
        if (defineHow & JSDNP_SET_METHOD) {
            JS_ASSERT(clasp == &js_ObjectClass);
            JS_ASSERT(IsFunctionObject(value));
            JS_ASSERT(!(attrs & (JSPROP_GETTER | JSPROP_SETTER)));
            JS_ASSERT(!getter && !setter);

            JSObject *funobj = &value.toObject();
            if (FUN_OBJECT(GET_FUNCTION_PRIVATE(cx, funobj)) == funobj) {
                flags |= Shape::METHOD;
                getter = CastAsPropertyOp(funobj);
            }
        }

        if (const Shape *existingShape = obj->nativeLookup(id)) {
            if (existingShape->hasSlot())
                AbortRecordingIfUnexpectedGlobalWrite(cx, obj, existingShape->slot);

            if (existingShape->isMethod() &&
                ObjectValue(existingShape->methodObject()) == valueCopy)
            {
                /*
                 * Redefining an existing shape-memoized method object without
                 * changing the property's value, perhaps to change attributes.
                 * Clone now via the method read barrier.
                 *
                 * But first, assert that our caller is not trying to preserve
                 * the joined function object value as the getter object for
                 * the redefined property. The joined function object cannot
                 * yet have leaked, so only an internal code path could attempt
                 * such a thing. Any such path would be a bug to fix.
                 */
                JS_ASSERT(existingShape->getter() != getter);

                if (!obj->methodReadBarrier(cx, *existingShape, &valueCopy))
                    return false;
            }
        } else {
            adding = true;
        }

        uint32 oldShape = obj->shape();
        shape = obj->putProperty(cx, id, getter, setter, SHAPE_INVALID_SLOT,
                                 attrs, flags, shortid);
        if (!shape)
            return false;

        /*
         * If shape is a joined method, the above call to putProperty suffices
         * to update the object's shape id if need be (because the shape's hash
         * identity includes the method value).
         *
         * But if scope->branded(), the object's shape id may not have changed
         * and we may be overwriting a cached function-valued property (note
         * how methodWriteBarrier checks previous vs. would-be current value).
         * See bug 560998.
         */
        if (obj->shape() == oldShape && obj->branded() && shape->slot != SHAPE_INVALID_SLOT) {
#ifdef DEBUG
            const Shape *newshape =
#endif
                obj->methodWriteBarrier(cx, *shape, valueCopy);
            JS_ASSERT(newshape == shape);
        }
    }

    /* Store valueCopy before calling addProperty, in case the latter GC's. */
    if (obj->containsSlot(shape->slot))
        obj->nativeSetSlot(shape->slot, valueCopy);

    /* XXXbe called with lock held */
    if (!CallAddPropertyHook(cx, clasp, obj, shape, &valueCopy)) {
        obj->removeProperty(cx, id);
        return false;
    }

    if (defineHow & JSDNP_CACHE_RESULT) {
        JS_ASSERT_NOT_ON_TRACE(cx);
        if (adding) {
            JS_PROPERTY_CACHE(cx).fill(cx, obj, 0, 0, obj, shape, true);
            TRACE_1(AddProperty, obj);
        }
    }
    if (propp)
        *propp = (JSProperty *) shape;
    return true;

#ifdef JS_TRACER
  error: // TRACE_1 jumps here on error.
#endif
    return false;
}

#define SCOPE_DEPTH_ACCUM(bs,val)                                             \
    JS_SCOPE_DEPTH_METERING(JS_BASIC_STATS_ACCUM(bs, val))

/*
 * Call obj's resolve hook. obj is a native object and the caller holds its
 * scope lock.
 *
 * cx, start, id, and flags are the parameters initially passed to the ongoing
 * lookup; objp and propp are its out parameters. obj is an object along
 * start's prototype chain.
 *
 * There are four possible outcomes:
 *
 *   - On failure, report an error or exception, unlock obj, and return false.
 *
 *   - If we are alrady resolving a property of *curobjp, set *recursedp = true,
 *     unlock obj, and return true.
 *
 *   - If the resolve hook finds or defines the sought property, set *objp and
 *     *propp appropriately, set *recursedp = false, and return true with *objp's
 *     lock held.
 *
 *   - Otherwise no property was resolved. Set *propp = NULL and *recursedp = false
 *     and return true.
 */
static JSBool
CallResolveOp(JSContext *cx, JSObject *start, JSObject *obj, jsid id, uintN flags,
              JSObject **objp, JSProperty **propp, bool *recursedp)
{
    Class *clasp = obj->getClass();
    JSResolveOp resolve = clasp->resolve;

    /*
     * Avoid recursion on (obj, id) already being resolved on cx.
     *
     * Once we have successfully added an entry for (obj, key) to
     * cx->resolvingTable, control must go through cleanup: before
     * returning.  But note that JS_DHASH_ADD may find an existing
     * entry, in which case we bail to suppress runaway recursion.
     */
    JSResolvingKey key = {obj, id};
    JSResolvingEntry *entry;
    if (!js_StartResolving(cx, &key, JSRESFLAG_LOOKUP, &entry))
        return false;
    if (!entry) {
        /* Already resolving id in obj -- suppress recursion. */
        *recursedp = true;
        return true;
    }
    uint32 generation = cx->resolvingTable->generation;
    *recursedp = false;

    *propp = NULL;

    JSBool ok;
    const Shape *shape = NULL;
    if (clasp->flags & JSCLASS_NEW_RESOLVE) {
        JSNewResolveOp newresolve = (JSNewResolveOp)resolve;
        if (flags == JSRESOLVE_INFER)
            flags = js_InferFlags(cx, 0);
        JSObject *obj2 = (clasp->flags & JSCLASS_NEW_RESOLVE_GETS_START) ? start : NULL;

        {
            /* Protect id and all atoms from a GC nested in resolve. */
            AutoKeepAtoms keep(cx->runtime);
            ok = newresolve(cx, obj, id, flags, &obj2);
        }
        if (!ok)
            goto cleanup;

        if (obj2) {
            /* Resolved: lookup id again for backward compatibility. */
            if (!obj2->isNative()) {
                /* Whoops, newresolve handed back a foreign obj2. */
                JS_ASSERT(obj2 != obj);
                ok = obj2->lookupProperty(cx, id, objp, propp);
                if (!ok || *propp)
                    goto cleanup;
            } else {
                /*
                 * Require that obj2 not be empty now, as we do for old-style
                 * resolve.  If it doesn't, then id was not truly resolved, and
                 * we'll find it in the proto chain, or miss it if obj2's proto
                 * is not on obj's proto chain.  That last case is a "too bad!"
                 * case.
                 */
                if (!obj2->nativeEmpty())
                    shape = obj2->nativeLookup(id);
            }
            if (shape) {
                JS_ASSERT(!obj2->nativeEmpty());
                obj = obj2;
            }
        }
    } else {
        /*
         * Old resolve always requires id re-lookup if obj is not empty after
         * resolve returns.
         */
        ok = resolve(cx, obj, id);
        if (!ok)
            goto cleanup;
        JS_ASSERT(obj->isNative());
        if (!obj->nativeEmpty())
            shape = obj->nativeLookup(id);
    }

cleanup:
    if (ok && shape) {
        *objp = obj;
        *propp = (JSProperty *) shape;
    }
    js_StopResolving(cx, &key, JSRESFLAG_LOOKUP, entry, generation);
    return ok;
}

static JS_ALWAYS_INLINE int
js_LookupPropertyWithFlagsInline(JSContext *cx, JSObject *obj, jsid id, uintN flags,
                                 JSObject **objp, JSProperty **propp)
{
    /* We should not get string indices which aren't already integers here. */
    JS_ASSERT(id == js_CheckForStringIndex(id));

    /* Search scopes starting with obj and following the prototype link. */
    JSObject *start = obj;
    int protoIndex;
    for (protoIndex = 0; ; protoIndex++) {
        const Shape *shape = obj->nativeLookup(id);
        if (shape) {
            SCOPE_DEPTH_ACCUM(&cx->runtime->protoLookupDepthStats, protoIndex);
            *objp = obj;
            *propp = (JSProperty *) shape;
            return protoIndex;
        }

        /* Try obj's class resolve hook if id was not found in obj's scope. */
        if (!shape && obj->getClass()->resolve != JS_ResolveStub) {
            bool recursed;
            if (!CallResolveOp(cx, start, obj, id, flags, objp, propp, &recursed))
                return -1;
            if (recursed)
                break;
            if (*propp) {
                /* Recalculate protoIndex in case it was resolved on some other object. */
                protoIndex = 0;
                for (JSObject *proto = start; proto && proto != *objp; proto = proto->getProto())
                    protoIndex++;
                SCOPE_DEPTH_ACCUM(&cx->runtime->protoLookupDepthStats, protoIndex);
                return protoIndex;
            }
        }

        JSObject *proto = obj->getProto();
        if (!proto)
            break;
        if (!proto->isNative()) {
            if (!proto->lookupProperty(cx, id, objp, propp))
                return -1;
#ifdef DEBUG
            /*
             * Non-native objects must have either non-native lookup results,
             * or else native results from the non-native's prototype chain.
             *
             * See JSStackFrame::getValidCalleeObject, where we depend on this
             * fact to force a prototype-delegated joined method accessed via
             * arguments.callee through the delegating |this| object's method
             * read barrier.
             */
            if (*propp && (*objp)->isNative()) {
                while ((proto = proto->getProto()) != *objp)
                    JS_ASSERT(proto);
            }
#endif
            return protoIndex + 1;
        }

        obj = proto;
    }

    *objp = NULL;
    *propp = NULL;
    return protoIndex;
}

JS_FRIEND_API(JSBool)
js_LookupProperty(JSContext *cx, JSObject *obj, jsid id, JSObject **objp,
                  JSProperty **propp)
{
    /* Convert string indices to integers if appropriate. */
    id = js_CheckForStringIndex(id);

    return js_LookupPropertyWithFlagsInline(cx, obj, id, cx->resolveFlags, objp, propp) >= 0;
}

int
js_LookupPropertyWithFlags(JSContext *cx, JSObject *obj, jsid id, uintN flags,
                           JSObject **objp, JSProperty **propp)
{
    /* Convert string indices to integers if appropriate. */
    id = js_CheckForStringIndex(id);

    return js_LookupPropertyWithFlagsInline(cx, obj, id, flags, objp, propp);
}

PropertyCacheEntry *
js_FindPropertyHelper(JSContext *cx, jsid id, JSBool cacheResult,
                      JSObject **objp, JSObject **pobjp, JSProperty **propp)
{
    JSObject *scopeChain, *obj, *parent, *pobj;
    PropertyCacheEntry *entry;
    int scopeIndex, protoIndex;
    JSProperty *prop;

    JS_ASSERT_IF(cacheResult, !JS_ON_TRACE(cx));
    scopeChain = &js_GetTopStackFrame(cx)->scopeChain();

    /* Scan entries on the scope chain that we can cache across. */
    entry = JS_NO_PROP_CACHE_FILL;
    obj = scopeChain;
    parent = obj->getParent();
    for (scopeIndex = 0;
         parent
         ? IsCacheableNonGlobalScope(obj)
         : !obj->getOps()->lookupProperty;
         ++scopeIndex) {
        protoIndex =
            js_LookupPropertyWithFlags(cx, obj, id, cx->resolveFlags,
                                       &pobj, &prop);
        if (protoIndex < 0)
            return NULL;

        if (prop) {
#ifdef DEBUG
            if (parent) {
                Class *clasp = obj->getClass();
                JS_ASSERT(pobj->isNative());
                JS_ASSERT(pobj->getClass() == clasp);
                if (clasp == &js_BlockClass) {
                    /*
                     * A block instance on the scope chain is immutable and it
                     * shares its shapes with its compile-time prototype.
                     */
                    JS_ASSERT(pobj == obj);
                    JS_ASSERT(pobj->isClonedBlock());
                    JS_ASSERT(protoIndex == 0);
                } else {
                    /* Call and DeclEnvClass objects have no prototypes. */
                    JS_ASSERT(!obj->getProto());
                    JS_ASSERT(protoIndex == 0);
                }
            } else {
                JS_ASSERT(obj->isNative());
            }
#endif
            /*
             * We must check if pobj is native as a global object can have
             * non-native prototype.
             */
            if (cacheResult && pobj->isNative()) {
                entry = JS_PROPERTY_CACHE(cx).fill(cx, scopeChain, scopeIndex,
                                                   protoIndex, pobj,
                                                   (Shape *) prop);
            }
            SCOPE_DEPTH_ACCUM(&cx->runtime->scopeSearchDepthStats, scopeIndex);
            goto out;
        }

        if (!parent) {
            pobj = NULL;
            goto out;
        }
        obj = parent;
        parent = obj->getParent();
    }

    for (;;) {
        if (!obj->lookupProperty(cx, id, &pobj, &prop))
            return NULL;
        if (prop) {
            PCMETER(JS_PROPERTY_CACHE(cx).nofills++);
            goto out;
        }

        /*
         * We conservatively assume that a resolve hook could mutate the scope
         * chain during JSObject::lookupProperty. So we read parent here again.
         */
        parent = obj->getParent();
        if (!parent) {
            pobj = NULL;
            break;
        }
        obj = parent;
    }

  out:
    JS_ASSERT(!!pobj == !!prop);
    *objp = obj;
    *pobjp = pobj;
    *propp = prop;
    return entry;
}

/*
 * On return, if |*pobjp| is a native object, then |*propp| is a |Shape *|.
 * Otherwise, its type and meaning depends on the host object's implementation.
 */
JS_FRIEND_API(JSBool)
js_FindProperty(JSContext *cx, jsid id, JSObject **objp, JSObject **pobjp,
                JSProperty **propp)
{
    return !!js_FindPropertyHelper(cx, id, false, objp, pobjp, propp);
}

JSObject *
js_FindIdentifierBase(JSContext *cx, JSObject *scopeChain, jsid id)
{
    /*
     * This function should not be called for a global object or from the
     * trace and should have a valid cache entry for native scopeChain.
     */
    JS_ASSERT(scopeChain->getParent());
    JS_ASSERT(!JS_ON_TRACE(cx));

    JSObject *obj = scopeChain;

    /*
     * Loop over cacheable objects on the scope chain until we find a
     * property. We also stop when we reach the global object skipping any
     * farther checks or lookups. For details see the JSOP_BINDNAME case of
     * js_Interpret.
     *
     * The test order here matters because IsCacheableNonGlobalScope
     * must not be passed a global object (i.e. one with null parent).
     */
    for (int scopeIndex = 0;
         !obj->getParent() || IsCacheableNonGlobalScope(obj);
         scopeIndex++) {
        JSObject *pobj;
        JSProperty *prop;
        int protoIndex = js_LookupPropertyWithFlags(cx, obj, id,
                                                    cx->resolveFlags,
                                                    &pobj, &prop);
        if (protoIndex < 0)
            return NULL;
        if (prop) {
            if (!pobj->isNative()) {
                JS_ASSERT(!obj->getParent());
                return obj;
            }
            JS_ASSERT_IF(obj->getParent(), pobj->getClass() == obj->getClass());
#ifdef DEBUG
            PropertyCacheEntry *entry =
#endif
                JS_PROPERTY_CACHE(cx).fill(cx, scopeChain, scopeIndex, protoIndex, pobj,
                                           (Shape *) prop);
            JS_ASSERT(entry);
            return obj;
        }

        JSObject *parent = obj->getParent();
        if (!parent)
            return obj;
        obj = parent;
    }

    /* Loop until we find a property or reach the global object. */
    do {
        JSObject *pobj;
        JSProperty *prop;
        if (!obj->lookupProperty(cx, id, &pobj, &prop))
            return NULL;
        if (prop)
            break;

        /*
         * We conservatively assume that a resolve hook could mutate the scope
         * chain during JSObject::lookupProperty. So we must check if parent is
         * not null here even if it wasn't before the lookup.
         */
        JSObject *parent = obj->getParent();
        if (!parent)
            break;
        obj = parent;
    } while (obj->getParent());
    return obj;
}

static JS_ALWAYS_INLINE JSBool
js_NativeGetInline(JSContext *cx, JSObject *receiver, JSObject *obj, JSObject *pobj,
                   const Shape *shape, uintN getHow, Value *vp)
{
    LeaveTraceIfGlobalObject(cx, pobj);

    uint32 slot;
    int32 sample;

    JS_ASSERT(pobj->isNative());

    slot = shape->slot;
    if (slot != SHAPE_INVALID_SLOT) {
        *vp = pobj->nativeGetSlot(slot);
        JS_ASSERT(!vp->isMagic());
    } else {
        vp->setUndefined();
    }
    if (shape->hasDefaultGetter())
        return true;

    if (JS_UNLIKELY(shape->isMethod()) && (getHow & JSGET_NO_METHOD_BARRIER)) {
        JS_ASSERT(&shape->methodObject() == &vp->toObject());
        return true;
    }

    sample = cx->runtime->propertyRemovals;
    {
        AutoShapeRooter tvr(cx, shape);
        AutoObjectRooter tvr2(cx, pobj);
        if (!shape->get(cx, receiver, obj, pobj, vp))
            return false;
    }

    if (pobj->containsSlot(slot) &&
        (JS_LIKELY(cx->runtime->propertyRemovals == sample) ||
         pobj->nativeContains(*shape))) {
        if (!pobj->methodWriteBarrier(cx, *shape, *vp))
            return false;
        pobj->nativeSetSlot(slot, *vp);
    }

    return true;
}

JSBool
js_NativeGet(JSContext *cx, JSObject *obj, JSObject *pobj, const Shape *shape, uintN getHow,
             Value *vp)
{
    return js_NativeGetInline(cx, obj, obj, pobj, shape, getHow, vp);
}

JSBool
js_NativeSet(JSContext *cx, JSObject *obj, const Shape *shape, bool added, bool strict, Value *vp)
{
    LeaveTraceIfGlobalObject(cx, obj);

    uint32 slot;
    int32 sample;

    JS_ASSERT(obj->isNative());

    slot = shape->slot;
    if (slot != SHAPE_INVALID_SLOT) {
        JS_ASSERT(obj->containsSlot(slot));

        /* If shape has a stub setter, keep obj locked and just store *vp. */
        if (shape->hasDefaultSetter()) {
            if (!added) {
                AbortRecordingIfUnexpectedGlobalWrite(cx, obj, slot);

                /* FIXME: This should pass *shape, not slot, but see bug 630354. */
                if (!obj->methodWriteBarrier(cx, slot, *vp))
                    return false;
            }
            obj->nativeSetSlot(slot, *vp);
            return true;
        }
    } else {
        /*
         * Allow API consumers to create shared properties with stub setters.
         * Such properties effectively function as data descriptors which are
         * not writable, so attempting to set such a property should do nothing
         * or throw if we're in strict mode.
         */
        if (!shape->hasGetterValue() && shape->hasDefaultSetter())
            return js_ReportGetterOnlyAssignment(cx);
    }

    sample = cx->runtime->propertyRemovals;
    {
        AutoShapeRooter tvr(cx, shape);
        if (!shape->set(cx, obj, strict, vp))
            return false;

        JS_ASSERT_IF(!obj->inDictionaryMode(), shape->slot == slot);
        slot = shape->slot;
    }

    if (obj->containsSlot(slot) &&
        (JS_LIKELY(cx->runtime->propertyRemovals == sample) ||
         obj->nativeContains(*shape))) {
        if (!added) {
            AbortRecordingIfUnexpectedGlobalWrite(cx, obj, slot);
            if (!obj->methodWriteBarrier(cx, *shape, *vp))
                return false;
        }
        obj->setSlot(slot, *vp);
    }

    return true;
}

static JS_ALWAYS_INLINE bool
js_GetPropertyHelperWithShapeInline(JSContext *cx, JSObject *obj, JSObject *receiver, jsid id,
                                    uintN getHow, Value *vp,
                                    const Shape **shapeOut, JSObject **holderOut)
{
    JSObject *aobj, *obj2;
    int protoIndex;
    JSProperty *prop;
    const Shape *shape;

    JS_ASSERT_IF(getHow & JSGET_CACHE_RESULT, !JS_ON_TRACE(cx));

    *shapeOut = NULL;

    /* Convert string indices to integers if appropriate. */
    id = js_CheckForStringIndex(id);

    aobj = js_GetProtoIfDenseArray(obj);
    /* This call site is hot -- use the always-inlined variant of js_LookupPropertyWithFlags(). */
    protoIndex = js_LookupPropertyWithFlagsInline(cx, aobj, id, cx->resolveFlags,
                                                  &obj2, &prop);
    if (protoIndex < 0)
        return JS_FALSE;

    *holderOut = obj2;

    if (!prop) {
        vp->setUndefined();

        if (!CallJSPropertyOp(cx, obj->getClass()->getProperty, obj, id, vp))
            return JS_FALSE;

        PCMETER(getHow & JSGET_CACHE_RESULT && JS_PROPERTY_CACHE(cx).nofills++);

        /*
         * Give a strict warning if foo.bar is evaluated by a script for an
         * object foo with no property named 'bar'.
         */
        jsbytecode *pc;
        if (vp->isUndefined() && ((pc = js_GetCurrentBytecodePC(cx)) != NULL)) {
            JSOp op;
            uintN flags;

            op = (JSOp) *pc;
            if (op == JSOP_TRAP) {
                JS_ASSERT_NOT_ON_TRACE(cx);
                op = JS_GetTrapOpcode(cx, cx->fp()->script(), pc);
            }
            if (op == JSOP_GETXPROP) {
                flags = JSREPORT_ERROR;
            } else {
                if (!cx->hasStrictOption() ||
                    (op != JSOP_GETPROP && op != JSOP_GETELEM) ||
                    js_CurrentPCIsInImacro(cx)) {
                    return JS_TRUE;
                }

                /*
                 * XXX do not warn about missing __iterator__ as the function
                 * may be called from JS_GetMethodById. See bug 355145.
                 */
                if (JSID_IS_ATOM(id, cx->runtime->atomState.iteratorAtom))
                    return JS_TRUE;

                /* Do not warn about tests like (obj[prop] == undefined). */
                if (cx->resolveFlags == JSRESOLVE_INFER) {
                    LeaveTrace(cx);
                    pc += js_CodeSpec[op].length;
                    if (Detecting(cx, pc))
                        return JS_TRUE;
                } else if (cx->resolveFlags & JSRESOLVE_DETECTING) {
                    return JS_TRUE;
                }

                flags = JSREPORT_WARNING | JSREPORT_STRICT;
            }

            /* Ok, bad undefined property reference: whine about it. */
            if (!js_ReportValueErrorFlags(cx, flags, JSMSG_UNDEFINED_PROP,
                                          JSDVG_IGNORE_STACK, IdToValue(id),
                                          NULL, NULL, NULL)) {
                return JS_FALSE;
            }
        }
        return JS_TRUE;
    }

    if (!obj2->isNative()) {
        return obj2->isProxy()
               ? JSProxy::get(cx, obj2, receiver, id, vp)
               : obj2->getProperty(cx, id, vp);
    }

    shape = (Shape *) prop;
    *shapeOut = shape;

    if (getHow & JSGET_CACHE_RESULT) {
        JS_ASSERT_NOT_ON_TRACE(cx);
        JS_PROPERTY_CACHE(cx).fill(cx, aobj, 0, protoIndex, obj2, shape);
    }

    /* This call site is hot -- use the always-inlined variant of js_NativeGet(). */
    if (!js_NativeGetInline(cx, receiver, obj, obj2, shape, getHow, vp))
        return JS_FALSE;

    return JS_TRUE;
}

bool
js_GetPropertyHelperWithShape(JSContext *cx, JSObject *obj, JSObject *receiver, jsid id,
                              uint32 getHow, Value *vp,
                              const Shape **shapeOut, JSObject **holderOut)
{
    return js_GetPropertyHelperWithShapeInline(cx, obj, receiver, id, getHow, vp,
                                               shapeOut, holderOut);
}

static JS_ALWAYS_INLINE JSBool
js_GetPropertyHelperInline(JSContext *cx, JSObject *obj, JSObject *receiver, jsid id,
                           uint32 getHow, Value *vp)
{
    const Shape *shape;
    JSObject *holder;
    return js_GetPropertyHelperWithShapeInline(cx, obj, receiver, id, getHow, vp, &shape, &holder);
}

JSBool
js_GetPropertyHelper(JSContext *cx, JSObject *obj, jsid id, uint32 getHow, Value *vp)
{
    return js_GetPropertyHelperInline(cx, obj, obj, id, getHow, vp);
}

JSBool
js_GetProperty(JSContext *cx, JSObject *obj, JSObject *receiver, jsid id, Value *vp)
{
    /* This call site is hot -- use the always-inlined variant of js_GetPropertyHelper(). */
    return js_GetPropertyHelperInline(cx, obj, receiver, id, JSGET_METHOD_BARRIER, vp);
}

JSBool
js::GetPropertyDefault(JSContext *cx, JSObject *obj, jsid id, const Value &def, Value *vp)
{
    JSProperty *prop;
    JSObject *obj2;
    if (js_LookupPropertyWithFlags(cx, obj, id, JSRESOLVE_QUALIFIED, &obj2, &prop) < 0)
        return false;

    if (!prop) {
        *vp = def;
        return true;
    }

    return js_GetProperty(cx, obj2, id, vp);
}

JSBool
js_GetMethod(JSContext *cx, JSObject *obj, jsid id, uintN getHow, Value *vp)
{
    JSAutoResolveFlags rf(cx, JSRESOLVE_QUALIFIED);

    PropertyIdOp op = obj->getOps()->getProperty;
    if (!op) {
#if JS_HAS_XML_SUPPORT
        JS_ASSERT(!obj->isXML());
#endif
        return js_GetPropertyHelper(cx, obj, id, getHow, vp);
    }
    JS_ASSERT_IF(getHow & JSGET_CACHE_RESULT, obj->isDenseArray());
#if JS_HAS_XML_SUPPORT
    if (obj->isXML())
        return js_GetXMLMethod(cx, obj, id, vp);
#endif
    return op(cx, obj, obj, id, vp);
}

JS_FRIEND_API(bool)
js_CheckUndeclaredVarAssignment(JSContext *cx, JSString *propname)
{
    JSStackFrame *const fp = js_GetTopStackFrame(cx);
    if (!fp)
        return true;

    /* If neither cx nor the code is strict, then no check is needed. */
    if (!(fp->isScriptFrame() && fp->script()->strictModeCode) &&
        !cx->hasStrictOption()) {
        return true;
    }

    JSAutoByteString bytes(cx, propname);
    return !!bytes &&
           JS_ReportErrorFlagsAndNumber(cx,
                                        (JSREPORT_WARNING | JSREPORT_STRICT
                                         | JSREPORT_STRICT_MODE_ERROR),
                                        js_GetErrorMessage, NULL,
                                        JSMSG_UNDECLARED_VAR, bytes.ptr());
}

bool
JSObject::reportReadOnly(JSContext* cx, jsid id, uintN report)
{
    return js_ReportValueErrorFlags(cx, report, JSMSG_READ_ONLY,
                                    JSDVG_IGNORE_STACK, IdToValue(id), NULL,
                                    NULL, NULL);
}

bool
JSObject::reportNotConfigurable(JSContext* cx, jsid id, uintN report)
{
    return js_ReportValueErrorFlags(cx, report, JSMSG_CANT_DELETE,
                                    JSDVG_IGNORE_STACK, IdToValue(id), NULL,
                                    NULL, NULL);
}

bool
JSObject::reportNotExtensible(JSContext *cx, uintN report)
{
    return js_ReportValueErrorFlags(cx, report, JSMSG_OBJECT_NOT_EXTENSIBLE,
                                    JSDVG_IGNORE_STACK, ObjectValue(*this),
                                    NULL, NULL, NULL);
}

JSBool
js_SetPropertyHelper(JSContext *cx, JSObject *obj, jsid id, uintN defineHow,
                     Value *vp, JSBool strict)
{
    int protoIndex;
    JSObject *pobj;
    JSProperty *prop;
    const Shape *shape;
    uintN attrs, flags;
    intN shortid;
    Class *clasp;
    PropertyOp getter;
    StrictPropertyOp setter;
    bool added;

    JS_ASSERT((defineHow &
               ~(JSDNP_CACHE_RESULT | JSDNP_SET_METHOD | JSDNP_UNQUALIFIED)) == 0);
    if (defineHow & JSDNP_CACHE_RESULT)
        JS_ASSERT_NOT_ON_TRACE(cx);

    /* Convert string indices to integers if appropriate. */
    id = js_CheckForStringIndex(id);

    protoIndex = js_LookupPropertyWithFlags(cx, obj, id, cx->resolveFlags,
                                            &pobj, &prop);
    if (protoIndex < 0)
        return JS_FALSE;
    if (prop) {
        if (!pobj->isNative()) {
            if (pobj->isProxy()) {
                AutoPropertyDescriptorRooter pd(cx);
                if (!JSProxy::getPropertyDescriptor(cx, pobj, id, true, &pd))
                    return false;

                if (pd.attrs & JSPROP_SHARED)
                    return CallSetter(cx, obj, id, pd.setter, pd.attrs, pd.shortid, strict, vp);

                if (pd.attrs & JSPROP_READONLY) {
                    if (strict)
                        return obj->reportReadOnly(cx, id);
                    if (cx->hasStrictOption())
                        return obj->reportReadOnly(cx, id, JSREPORT_STRICT | JSREPORT_WARNING);
                    return true;
                }
            }

            prop = NULL;
        }
    } else {
        /* We should never add properties to lexical blocks.  */
        JS_ASSERT(!obj->isBlock());

        if (!obj->getParent() &&
            (defineHow & JSDNP_UNQUALIFIED) &&
            !js_CheckUndeclaredVarAssignment(cx, JSID_TO_STRING(id))) {
            return JS_FALSE;
        }
    }
    shape = (Shape *) prop;

    /*
     * Now either shape is null, meaning id was not found in obj or one of its
     * prototypes; or shape is non-null, meaning id was found directly in pobj.
     */
    attrs = JSPROP_ENUMERATE;
    flags = 0;
    shortid = 0;
    clasp = obj->getClass();
    getter = clasp->getProperty;
    setter = clasp->setProperty;

    if (shape) {
        /* ES5 8.12.4 [[Put]] step 2. */
        if (shape->isAccessorDescriptor()) {
            if (shape->hasDefaultSetter())
                return js_ReportGetterOnlyAssignment(cx);
        } else {
            JS_ASSERT(shape->isDataDescriptor());

            if (!shape->writable()) {
                PCMETER((defineHow & JSDNP_CACHE_RESULT) && JS_PROPERTY_CACHE(cx).rofills++);

                /* Error in strict mode code, warn with strict option, otherwise do nothing. */
                if (strict)
                    return obj->reportReadOnly(cx, id);
                if (cx->hasStrictOption())
                    return obj->reportReadOnly(cx, id, JSREPORT_STRICT | JSREPORT_WARNING);
                return JS_TRUE;
            }
        }

        attrs = shape->attributes();
        if (pobj != obj) {
            /*
             * We found id in a prototype object: prepare to share or shadow.
             */
            if (!shape->shadowable()) {
                if (defineHow & JSDNP_CACHE_RESULT)
                    JS_PROPERTY_CACHE(cx).fill(cx, obj, 0, protoIndex, pobj, shape);

                if (shape->hasDefaultSetter() && !shape->hasGetterValue())
                    return JS_TRUE;

                return shape->set(cx, obj, strict, vp);
            }

            /*
             * Preserve attrs except JSPROP_SHARED, getter, and setter when
             * shadowing any property that has no slot (is shared). We must
             * clear the shared attribute for the shadowing shape so that the
             * property in obj that it defines has a slot to retain the value
             * being set, in case the setter simply cannot operate on instances
             * of obj's class by storing the value in some class-specific
             * location.
             *
             * A subset of slotless shared properties is the set of properties
             * with shortids, which must be preserved too. An old API requires
             * that the property's getter and setter receive the shortid, not
             * id, when they are called on the shadowing property that we are
             * about to create in obj.
             */
            if (!shape->hasSlot()) {
                defineHow &= ~JSDNP_SET_METHOD;
                if (shape->hasShortID()) {
                    flags = Shape::HAS_SHORTID;
                    shortid = shape->shortid;
                }
                attrs &= ~JSPROP_SHARED;
                getter = shape->getter();
                setter = shape->setter();
            } else {
                /* Restore attrs to the ECMA default for new properties. */
                attrs = JSPROP_ENUMERATE;
            }

            /*
             * Forget we found the proto-property now that we've copied any
             * needed member values.
             */
            shape = NULL;
        }

        JS_ASSERT_IF(shape && shape->isMethod(), pobj->hasMethodBarrier());
        JS_ASSERT_IF(shape && shape->isMethod(),
                     &pobj->getSlot(shape->slot).toObject() == &shape->methodObject());
        if (shape && (defineHow & JSDNP_SET_METHOD)) {
            /*
             * JSOP_SETMETHOD is assigning to an existing own property. If it
             * is an identical method property, do nothing. Otherwise downgrade
             * to ordinary assignment. Either way, do not fill the property
             * cache, as the interpreter has no fast path for these unusual
             * cases.
             */
            bool identical = shape->isMethod() && &shape->methodObject() == &vp->toObject();
            if (!identical) {
                shape = obj->methodShapeChange(cx, *shape);
                if (!shape)
                    return false;

                JSObject *funobj = &vp->toObject();
                JSFunction *fun = funobj->getFunctionPrivate();
                if (fun == funobj) {
                    funobj = CloneFunctionObject(cx, fun, fun->parent);
                    if (!funobj)
                        return JS_FALSE;
                    vp->setObject(*funobj);
                }
            }
            return identical || js_NativeSet(cx, obj, shape, false, strict, vp);
        }
    }

    added = false;
    if (!shape) {
        if (!obj->isExtensible()) {
            /* Error in strict mode code, warn with strict option, otherwise do nothing. */
            if (strict)
                return obj->reportNotExtensible(cx);
            if (cx->hasStrictOption())
                return obj->reportNotExtensible(cx, JSREPORT_STRICT | JSREPORT_WARNING);
            return JS_TRUE;
        }

        /*
         * Purge the property cache of now-shadowed id in obj's scope chain.
         * Do this early, before locking obj to avoid nesting locks.
         */
        js_PurgeScopeChain(cx, obj, id);

        /* Find or make a property descriptor with the right heritage. */
        if (!obj->ensureClassReservedSlots(cx))
            return JS_FALSE;

        /*
         * Check for Object class here to avoid defining a method on a class
         * with magic resolve, addProperty, getProperty, etc. hooks.
         */
        if ((defineHow & JSDNP_SET_METHOD) && obj->canHaveMethodBarrier()) {
            JS_ASSERT(IsFunctionObject(*vp));
            JS_ASSERT(!(attrs & (JSPROP_GETTER | JSPROP_SETTER)));

            JSObject *funobj = &vp->toObject();
            JSFunction *fun = GET_FUNCTION_PRIVATE(cx, funobj);
            if (fun == funobj) {
                flags |= Shape::METHOD;
                getter = CastAsPropertyOp(funobj);
            }
        }

        shape = obj->putProperty(cx, id, getter, setter, SHAPE_INVALID_SLOT,
                                 attrs, flags, shortid);
        if (!shape)
            return JS_FALSE;

        if (defineHow & JSDNP_CACHE_RESULT)
            TRACE_1(AddProperty, obj);

        /*
         * Initialize the new property value (passed to setter) to undefined.
         * Note that we store before calling addProperty, to match the order
         * in js_DefineNativeProperty.
         */
        if (obj->containsSlot(shape->slot))
            obj->nativeSetSlot(shape->slot, UndefinedValue());

        /* XXXbe called with obj locked */
        if (!CallAddPropertyHook(cx, clasp, obj, shape, vp)) {
            obj->removeProperty(cx, id);
            return JS_FALSE;
        }
        added = true;
    }

    if (defineHow & JSDNP_CACHE_RESULT)
        JS_PROPERTY_CACHE(cx).fill(cx, obj, 0, 0, obj, shape, added);

    return js_NativeSet(cx, obj, shape, added, strict, vp);

#ifdef JS_TRACER
  error: // TRACE_1 jumps here in case of error.
    return JS_FALSE;
#endif
}

JSBool
js_SetProperty(JSContext *cx, JSObject *obj, jsid id, Value *vp, JSBool strict)
{
    return js_SetPropertyHelper(cx, obj, id, 0, vp, strict);
}

JSBool
js_GetAttributes(JSContext *cx, JSObject *obj, jsid id, uintN *attrsp)
{
    JSProperty *prop;
    if (!js_LookupProperty(cx, obj, id, &obj, &prop))
        return false;
    if (!prop) {
        *attrsp = 0;
        return true;
    }
    if (!obj->isNative())
        return obj->getAttributes(cx, id, attrsp);

    const Shape *shape = (Shape *)prop;
    *attrsp = shape->attributes();
    return true;
}

JSBool
js_SetNativeAttributes(JSContext *cx, JSObject *obj, Shape *shape, uintN attrs)
{
    JS_ASSERT(obj->isNative());
    return !!js_ChangeNativePropertyAttrs(cx, obj, shape, attrs, 0,
                                          shape->getter(), shape->setter());
}

JSBool
js_SetAttributes(JSContext *cx, JSObject *obj, jsid id, uintN *attrsp)
{
    JSProperty *prop;
    if (!js_LookupProperty(cx, obj, id, &obj, &prop))
        return false;
    if (!prop)
        return true;
    return obj->isNative()
           ? js_SetNativeAttributes(cx, obj, (Shape *) prop, *attrsp)
           : obj->setAttributes(cx, id, attrsp);
}

JSBool
js_DeleteProperty(JSContext *cx, JSObject *obj, jsid id, Value *rval, JSBool strict)
{
    JSObject *proto;
    JSProperty *prop;
    const Shape *shape;

    rval->setBoolean(true);

    /* Convert string indices to integers if appropriate. */
    id = js_CheckForStringIndex(id);

    if (!js_LookupProperty(cx, obj, id, &proto, &prop))
        return false;
    if (!prop || proto != obj) {
        /*
         * If the property was found in a native prototype, check whether it's
         * shared and permanent.  Such a property stands for direct properties
         * in all delegating objects, matching ECMA semantics without bloating
         * each delegating object.
         */
        if (prop && proto->isNative()) {
            shape = (Shape *)prop;
            if (shape->isSharedPermanent()) {
                if (strict)
                    return obj->reportNotConfigurable(cx, id);
                rval->setBoolean(false);
                return true;
            }
        }

        /*
         * If no property, or the property comes unshared or impermanent from
         * a prototype, call the class's delProperty hook, passing rval as the
         * result parameter.
         */
        return CallJSPropertyOp(cx, obj->getClass()->delProperty, obj, id, rval);
    }

    shape = (Shape *)prop;
    if (!shape->configurable()) {
        if (strict)
            return obj->reportNotConfigurable(cx, id);
        rval->setBoolean(false);
        return true;
    }

    if (!CallJSPropertyOp(cx, obj->getClass()->delProperty, obj, SHAPE_USERID(shape), rval))
        return false;

    if (obj->containsSlot(shape->slot)) {
        const Value &v = obj->nativeGetSlot(shape->slot);
        GC_POKE(cx, v);

        /*
         * Delete is rare enough that we can take the hit of checking for an
         * active cloned method function object that must be homed to a callee
         * slot on the active stack frame before this delete completes, in case
         * someone saved the clone and checks it against foo.caller for a foo
         * called from the active method.
         *
         * We do not check suspended frames. They can't be reached via caller,
         * so the only way they could have the method's joined function object
         * as callee is through an API abusage. We break any such edge case.
         */
        if (obj->hasMethodBarrier()) {
            JSObject *funobj;

            if (IsFunctionObject(v, &funobj)) {
                JSFunction *fun = GET_FUNCTION_PRIVATE(cx, funobj);

                if (fun != funobj) {
                    for (JSStackFrame *fp = cx->maybefp(); fp; fp = fp->prev()) {
                        if (fp->isFunctionFrame() &&
                            &fp->callee() == &fun->compiledFunObj() &&
                            fp->thisValue().isObject())
                        {
                            JSObject *tmp = &fp->thisValue().toObject();
                            do {
                                if (tmp == obj) {
                                    fp->calleeValue().setObject(*funobj);
                                    break;
                                }
                            } while ((tmp = tmp->getProto()) != NULL);
                        }
                    }
                }
            }
        }
    }

    return obj->removeProperty(cx, id) && js_SuppressDeletedProperty(cx, obj, id);
}

namespace js {

JSObject *
HasNativeMethod(JSObject *obj, jsid methodid, Native native)
{
    const Shape *shape = obj->nativeLookup(methodid);
    if (!shape || !shape->hasDefaultGetter() || !obj->containsSlot(shape->slot))
        return NULL;

    const Value &fval = obj->nativeGetSlot(shape->slot);
    JSObject *funobj;
    if (!IsFunctionObject(fval, &funobj) || funobj->getFunctionPrivate()->maybeNative() != native)
        return NULL;

    return funobj;
}

bool
DefaultValue(JSContext *cx, JSObject *obj, JSType hint, Value *vp)
{
    JS_ASSERT(hint != JSTYPE_OBJECT && hint != JSTYPE_FUNCTION);

    Value v = ObjectValue(*obj);
    if (hint == JSTYPE_STRING) {
        /* Optimize (new String(...)).toString(). */
        if (obj->getClass() == &js_StringClass &&
            ClassMethodIsNative(cx, obj,
                                 &js_StringClass,
                                 ATOM_TO_JSID(cx->runtime->atomState.toStringAtom),
                                 js_str_toString)) {
            *vp = obj->getPrimitiveThis();
            return true;
        }

        if (!js_TryMethod(cx, obj, cx->runtime->atomState.toStringAtom, 0, NULL, &v))
            return false;
        if (!v.isPrimitive()) {
            if (!obj->getClass()->convert(cx, obj, hint, &v))
                return false;
        }
    } else {
        /* Optimize (new String(...)).valueOf(). */
        Class *clasp = obj->getClass();
        if ((clasp == &js_StringClass &&
             ClassMethodIsNative(cx, obj, &js_StringClass,
                                 ATOM_TO_JSID(cx->runtime->atomState.valueOfAtom),
                                 js_str_toString)) ||
            (clasp == &js_NumberClass &&
             ClassMethodIsNative(cx, obj, &js_NumberClass,
                                 ATOM_TO_JSID(cx->runtime->atomState.valueOfAtom),
                                 js_num_valueOf))) {
            *vp = obj->getPrimitiveThis();
            return true;
        }

        if (!obj->getClass()->convert(cx, obj, hint, &v))
            return false;
        if (v.isObject()) {
            JS_ASSERT(hint != TypeOfValue(cx, v));
            if (!js_TryMethod(cx, obj, cx->runtime->atomState.toStringAtom, 0, NULL, &v))
                return false;
        }
    }
    if (v.isObject()) {
        /* Avoid recursive death when decompiling in js_ReportValueError. */
        JSString *str;
        if (hint == JSTYPE_STRING) {
            str = JS_InternString(cx, obj->getClass()->name);
            if (!str)
                return false;
        } else {
            str = NULL;
        }
        vp->setObject(*obj);
        js_ReportValueError2(cx, JSMSG_CANT_CONVERT_TO,
                             JSDVG_SEARCH_STACK, *vp, str,
                             (hint == JSTYPE_VOID)
                             ? "primitive type"
                             : JS_TYPE_STR(hint));
        return false;
    }
    *vp = v;
    return true;
}

} /* namespace js */

JS_FRIEND_API(JSBool)
js_Enumerate(JSContext *cx, JSObject *obj, JSIterateOp enum_op, Value *statep, jsid *idp)
{
    /* If the class has a custom JSCLASS_NEW_ENUMERATE hook, call it. */
    Class *clasp = obj->getClass();
    JSEnumerateOp enumerate = clasp->enumerate;
    if (clasp->flags & JSCLASS_NEW_ENUMERATE) {
        JS_ASSERT(enumerate != JS_EnumerateStub);
        return ((NewEnumerateOp) enumerate)(cx, obj, enum_op, statep, idp);
    }

    if (!enumerate(cx, obj))
        return false;

    /* Tell InitNativeIterator to treat us like a native object. */
    JS_ASSERT(enum_op == JSENUMERATE_INIT || enum_op == JSENUMERATE_INIT_ALL);
    statep->setMagic(JS_NATIVE_ENUMERATE);
    return true;
}

namespace js {

JSBool
CheckAccess(JSContext *cx, JSObject *obj, jsid id, JSAccessMode mode,
            Value *vp, uintN *attrsp)
{
    JSBool writing;
    JSObject *pobj;
    JSProperty *prop;
    Class *clasp;
    const Shape *shape;
    JSSecurityCallbacks *callbacks;
    CheckAccessOp check;

    while (JS_UNLIKELY(obj->getClass() == &js_WithClass))
        obj = obj->getProto();

    writing = (mode & JSACC_WRITE) != 0;
    switch (mode & JSACC_TYPEMASK) {
      case JSACC_PROTO:
        pobj = obj;
        if (!writing)
            vp->setObjectOrNull(obj->getProto());
        *attrsp = JSPROP_PERMANENT;
        break;

      case JSACC_PARENT:
        JS_ASSERT(!writing);
        pobj = obj;
        vp->setObject(*obj->getParent());
        *attrsp = JSPROP_READONLY | JSPROP_PERMANENT;
        break;

      default:
        if (!obj->lookupProperty(cx, id, &pobj, &prop))
            return JS_FALSE;
        if (!prop) {
            if (!writing)
                vp->setUndefined();
            *attrsp = 0;
            pobj = obj;
            break;
        }

        if (!pobj->isNative()) {
            if (!writing) {
                    vp->setUndefined();
                *attrsp = 0;
            }
            break;
        }

        shape = (Shape *)prop;
        *attrsp = shape->attributes();
        if (!writing) {
            if (pobj->containsSlot(shape->slot))
                *vp = pobj->nativeGetSlot(shape->slot);
            else
                vp->setUndefined();
        }
    }

    /*
     * If obj's class has a stub (null) checkAccess hook, use the per-runtime
     * checkObjectAccess callback, if configured.
     *
     * We don't want to require all classes to supply a checkAccess hook; we
     * need that hook only for certain classes used when precompiling scripts
     * and functions ("brutal sharing").  But for general safety of built-in
     * magic properties like __proto__, we route all access checks, even for
     * classes that stub out checkAccess, through the global checkObjectAccess
     * hook.  This covers precompilation-based sharing and (possibly
     * unintended) runtime sharing across trust boundaries.
     */
    clasp = pobj->getClass();
    check = clasp->checkAccess;
    if (!check) {
        callbacks = JS_GetSecurityCallbacks(cx);
        check = callbacks ? Valueify(callbacks->checkObjectAccess) : NULL;
    }
    return !check || check(cx, pobj, id, mode, vp);
}

}

JSType
js_TypeOf(JSContext *cx, JSObject *obj)
{
    /*
     * ECMA 262, 11.4.3 says that any native object that implements
     * [[Call]] should be of type "function". However, RegExp is of
     * type "object", not "function", for Web compatibility.
     */
    if (obj->isCallable()) {
        return (obj->getClass() != &js_RegExpClass)
               ? JSTYPE_FUNCTION
               : JSTYPE_OBJECT;
    }

    return JSTYPE_OBJECT;
}

bool
js_IsDelegate(JSContext *cx, JSObject *obj, const Value &v)
{
    if (v.isPrimitive())
        return false;
    JSObject *obj2 = &v.toObject();
    while ((obj2 = obj2->getProto()) != NULL) {
        if (obj2 == obj)
            return true;
    }
    return false;
}

bool
js::FindClassPrototype(JSContext *cx, JSObject *scopeobj, JSProtoKey protoKey,
                       JSObject **protop, Class *clasp)
{
    Value v;
    if (!js_FindClassObject(cx, scopeobj, protoKey, &v, clasp))
        return false;

    if (IsFunctionObject(v)) {
        JSObject *ctor = &v.toObject();
        if (!ctor->getProperty(cx, ATOM_TO_JSID(cx->runtime->atomState.classPrototypeAtom), &v))
            return false;
    }

    *protop = v.isObject() ? &v.toObject() : NULL;
    return true;
}

/*
 * The first part of this function has been hand-expanded and optimized into
 * NewBuiltinClassInstance in jsobjinlines.h.
 */
JSBool
js_GetClassPrototype(JSContext *cx, JSObject *scopeobj, JSProtoKey protoKey,
                     JSObject **protop, Class *clasp)
{
    VOUCH_DOES_NOT_REQUIRE_STACK();
    JS_ASSERT(JSProto_Null <= protoKey);
    JS_ASSERT(protoKey < JSProto_LIMIT);

    if (protoKey != JSProto_Null) {
        if (!scopeobj) {
            if (cx->hasfp())
                scopeobj = &cx->fp()->scopeChain();
            if (!scopeobj) {
                scopeobj = cx->globalObject;
                if (!scopeobj) {
                    *protop = NULL;
                    return true;
                }
            }
        }
        scopeobj = scopeobj->getGlobal();
        if (scopeobj->isGlobal()) {
            const Value &v = scopeobj->getReservedSlot(JSProto_LIMIT + protoKey);
            if (v.isObject()) {
                *protop = &v.toObject();
                return true;
            }
        }
    }

    return FindClassPrototype(cx, scopeobj, protoKey, protop, clasp);
}

JSBool
js_SetClassPrototype(JSContext *cx, JSObject *ctor, JSObject *proto, uintN attrs)
{
    /*
     * Use the given attributes for the prototype property of the constructor,
     * as user-defined constructors have a DontDelete prototype (which may be
     * reset), while native or "system" constructors have DontEnum | ReadOnly |
     * DontDelete.
     */
    if (!ctor->defineProperty(cx, ATOM_TO_JSID(cx->runtime->atomState.classPrototypeAtom),
                              ObjectOrNullValue(proto), PropertyStub, StrictPropertyStub, attrs)) {
        return JS_FALSE;
    }

    /*
     * ECMA says that Object.prototype.constructor, or f.prototype.constructor
     * for a user-defined function f, is DontEnum.
     */
    return proto->defineProperty(cx, ATOM_TO_JSID(cx->runtime->atomState.constructorAtom),
                                 ObjectOrNullValue(ctor), PropertyStub, StrictPropertyStub, 0);
}

JSObject *
PrimitiveToObject(JSContext *cx, const Value &v)
{
    JS_ASSERT(v.isPrimitive());

    Class *clasp;
    if (v.isNumber()) {
        clasp = &js_NumberClass;
    } else if (v.isString()) {
        clasp = &js_StringClass;
    } else {
        JS_ASSERT(v.isBoolean());
        clasp = &js_BooleanClass;
    }

    JSObject *obj = NewBuiltinClassInstance(cx, clasp);
    if (!obj)
        return NULL;

    obj->setPrimitiveThis(v);
    return obj;
}

JSBool
js_PrimitiveToObject(JSContext *cx, Value *vp)
{
    JSObject *obj = PrimitiveToObject(cx, *vp);
    if (!obj)
        return false;

    vp->setObject(*obj);
    return true;
}

JSBool
js_ValueToObjectOrNull(JSContext *cx, const Value &v, JSObject **objp)
{
    JSObject *obj;

    if (v.isObjectOrNull()) {
        obj = v.toObjectOrNull();
    } else if (v.isUndefined()) {
        obj = NULL;
    } else {
        obj = PrimitiveToObject(cx, v);
        if (!obj)
            return false;
    }
    *objp = obj;
    return true;
}

namespace js {

/* Callers must handle the already-object case . */
JSObject *
ToObjectSlow(JSContext *cx, Value *vp)
{
    JS_ASSERT(!vp->isMagic());
    JS_ASSERT(!vp->isObject());

    if (vp->isNullOrUndefined()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_CONVERT_TO,
                            vp->isNull() ? "null" : "undefined", "object");
        return NULL;
    }

    JSObject *obj = PrimitiveToObject(cx, *vp);
    if (obj)
        vp->setObject(*obj);
    return obj;
}

}

JSObject *
js_ValueToNonNullObject(JSContext *cx, const Value &v)
{
    JSObject *obj;

    if (!js_ValueToObjectOrNull(cx, v, &obj))
        return NULL;
    if (!obj)
        js_ReportIsNullOrUndefined(cx, JSDVG_SEARCH_STACK, v, NULL);
    return obj;
}

JSBool
js_TryValueOf(JSContext *cx, JSObject *obj, JSType type, Value *rval)
{
    Value argv[1];

    argv[0].setString(ATOM_TO_STRING(cx->runtime->atomState.typeAtoms[type]));
    return js_TryMethod(cx, obj, cx->runtime->atomState.valueOfAtom,
                        1, argv, rval);
}

JSBool
js_TryMethod(JSContext *cx, JSObject *obj, JSAtom *atom,
             uintN argc, Value *argv, Value *rval)
{
    JS_CHECK_RECURSION(cx, return JS_FALSE);

    /*
     * Report failure only if an appropriate method was found, and calling it
     * returned failure.  We propagate failure in this case to make exceptions
     * behave properly.
     */
    JSErrorReporter older = JS_SetErrorReporter(cx, NULL);
    jsid id = ATOM_TO_JSID(atom);
    Value fval;
    JSBool ok = js_GetMethod(cx, obj, id, JSGET_NO_METHOD_BARRIER, &fval);
    JS_SetErrorReporter(cx, older);
    if (!ok)
        return false;

    if (fval.isPrimitive())
        return JS_TRUE;
    return ExternalInvoke(cx, ObjectValue(*obj), fval, argc, argv, rval);
}

#if JS_HAS_XDR

JSBool
js_XDRObject(JSXDRState *xdr, JSObject **objp)
{
    JSContext *cx;
    JSAtom *atom;
    Class *clasp;
    uint32 classId, classDef;
    JSProtoKey protoKey;
    JSObject *proto;

    cx = xdr->cx;
    atom = NULL;
    if (xdr->mode == JSXDR_ENCODE) {
        clasp = (*objp)->getClass();
        classId = JS_XDRFindClassIdByName(xdr, clasp->name);
        classDef = !classId;
        if (classDef) {
            if (!JS_XDRRegisterClass(xdr, Jsvalify(clasp), &classId))
                return JS_FALSE;
            protoKey = JSCLASS_CACHED_PROTO_KEY(clasp);
            if (protoKey != JSProto_Null) {
                classDef |= (protoKey << 1);
            } else {
                atom = js_Atomize(cx, clasp->name, strlen(clasp->name), 0);
                if (!atom)
                    return JS_FALSE;
            }
        }
    } else {
        clasp = NULL;           /* quell GCC overwarning */
        classDef = 0;
    }

    /*
     * XDR a flag word, which could be 0 for a class use, in which case no
     * name follows, only the id in xdr's class registry; 1 for a class def,
     * in which case the flag word is followed by the class name transferred
     * from or to atom; or a value greater than 1, an odd number that when
     * divided by two yields the JSProtoKey for class.  In the last case, as
     * in the 0 classDef case, no name is transferred via atom.
     */
    if (!JS_XDRUint32(xdr, &classDef))
        return JS_FALSE;
    if (classDef == 1 && !js_XDRAtom(xdr, &atom))
        return JS_FALSE;

    if (!JS_XDRUint32(xdr, &classId))
        return JS_FALSE;

    if (xdr->mode == JSXDR_DECODE) {
        if (classDef) {
            /* NB: we know that JSProto_Null is 0 here, for backward compat. */
            protoKey = (JSProtoKey) (classDef >> 1);
            if (!js_GetClassPrototype(cx, NULL, protoKey, &proto, clasp))
                return JS_FALSE;
            clasp = proto->getClass();
            if (!JS_XDRRegisterClass(xdr, Jsvalify(clasp), &classId))
                return JS_FALSE;
        } else {
            clasp = Valueify(JS_XDRFindClassById(xdr, classId));
            if (!clasp) {
                char numBuf[12];
                JS_snprintf(numBuf, sizeof numBuf, "%ld", (long)classId);
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_CANT_FIND_CLASS, numBuf);
                return JS_FALSE;
            }
        }
    }

    if (!clasp->xdrObject) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_CANT_XDR_CLASS, clasp->name);
        return JS_FALSE;
    }
    return clasp->xdrObject(xdr, objp);
}

#endif /* JS_HAS_XDR */

#ifdef JS_DUMP_SCOPE_METERS

#include <stdio.h>

JSBasicStats js_entry_count_bs = JS_INIT_STATIC_BASIC_STATS;

static void
MeterEntryCount(uintN count)
{
    JS_BASIC_STATS_ACCUM(&js_entry_count_bs, count);
}

void
js_DumpScopeMeters(JSRuntime *rt)
{
    static FILE *logfp;
    if (!logfp)
        logfp = fopen("/tmp/scope.stats", "a");

    {
        double mean, sigma;

        mean = JS_MeanAndStdDevBS(&js_entry_count_bs, &sigma);

        fprintf(logfp, "scopes %u entries %g mean %g sigma %g max %u",
                js_entry_count_bs.num, js_entry_count_bs.sum, mean, sigma,
                js_entry_count_bs.max);
    }

    JS_DumpHistogram(&js_entry_count_bs, logfp);
    JS_BASIC_STATS_INIT(&js_entry_count_bs);
    fflush(logfp);
}
#endif

#ifdef DEBUG
void
js_PrintObjectSlotName(JSTracer *trc, char *buf, size_t bufsize)
{
    JS_ASSERT(trc->debugPrinter == js_PrintObjectSlotName);

    JSObject *obj = (JSObject *)trc->debugPrintArg;
    uint32 slot = (uint32)trc->debugPrintIndex;

    const Shape *shape;
    if (obj->isNative()) {
        shape = obj->lastProperty();
        while (shape->previous() && shape->slot != slot)
            shape = shape->previous();
        if (shape->slot != slot)
            shape = NULL;
    } else {
        shape = NULL;
    }

    if (!shape) {
        const char *slotname = NULL;
        if (obj->isGlobal()) {
#define JS_PROTO(name,code,init)                                              \
    if ((code) == slot) { slotname = js_##name##_str; goto found; }
#include "jsproto.tbl"
#undef JS_PROTO
        }
      found:
        if (slotname)
            JS_snprintf(buf, bufsize, "CLASS_OBJECT(%s)", slotname);
        else
            JS_snprintf(buf, bufsize, "**UNKNOWN SLOT %ld**", (long)slot);
    } else {
        jsid id = shape->id;
        if (JSID_IS_INT(id)) {
            JS_snprintf(buf, bufsize, "%ld", (long)JSID_TO_INT(id));
        } else if (JSID_IS_ATOM(id)) {
            PutEscapedString(buf, bufsize, JSID_TO_ATOM(id), 0);
        } else {
            JS_snprintf(buf, bufsize, "**FINALIZED ATOM KEY**");
        }
    }
}
#endif

void
js_TraceObject(JSTracer *trc, JSObject *obj)
{
    JS_ASSERT(obj->isNative());

    JSContext *cx = trc->context;
    if (obj->hasSlotsArray() && !obj->nativeEmpty() && IS_GC_MARKING_TRACER(trc)) {
        /*
         * Trim overlong dslots allocations from the GC, to avoid thrashing in
         * case of delete-happy code that settles down at a given population.
         * The !obj->nativeEmpty() guard above is due to the bug described by
         * the FIXME comment below.
         */
        size_t slots = obj->slotSpan();
        if (obj->numSlots() != slots)
            obj->shrinkSlots(cx, slots);
    }

#ifdef JS_DUMP_SCOPE_METERS
    MeterEntryCount(obj->propertyCount);
#endif

    obj->trace(trc);

    if (!JS_CLIST_IS_EMPTY(&cx->runtime->watchPointList))
        js_TraceWatchPoints(trc, obj);

    /* No one runs while the GC is running, so we can use LOCKED_... here. */
    Class *clasp = obj->getClass();
    if (clasp->mark) {
        if (clasp->flags & JSCLASS_MARK_IS_TRACE)
            ((JSTraceOp) clasp->mark)(trc, obj);
        else if (IS_GC_MARKING_TRACER(trc))
            (void) clasp->mark(cx, obj, trc);
    }
    if (clasp->flags & JSCLASS_IS_GLOBAL) {
        JSCompartment *compartment = obj->getCompartment();
        compartment->mark(trc);
    }

    /*
     * NB: clasp->mark could mutate something (which would be a bug, but we are
     * defensive), so don't hoist this above calling clasp->mark.
     */
    uint32 nslots = Min(obj->numSlots(), obj->slotSpan());
    for (uint32 i = 0; i != nslots; ++i) {
        const Value &v = obj->getSlot(i);
        JS_SET_TRACING_DETAILS(trc, js_PrintObjectSlotName, obj, i);
        MarkValueRaw(trc, v);
    }
}

void
js_ClearNative(JSContext *cx, JSObject *obj)
{
    /*
     * Clear obj of all obj's properties. FIXME: we do not clear reserved slots
     * lying below JSSLOT_FREE(clasp). JS_ClearScope does that.
     */
    if (!obj->nativeEmpty()) {
        /* Now that we're done using real properties, clear obj. */
        obj->clear(cx);

        /* Clear slot values since obj->clear reset our shape to empty. */
        uint32 freeslot = JSSLOT_FREE(obj->getClass());
        uint32 n = obj->numSlots();
        for (uint32 i = freeslot; i < n; ++i)
            obj->setSlot(i, UndefinedValue());
    }
}

bool
js_GetReservedSlot(JSContext *cx, JSObject *obj, uint32 slot, Value *vp)
{
    if (!obj->isNative()) {
        vp->setUndefined();
        return true;
    }

    if (slot < obj->numSlots())
        *vp = obj->getSlot(slot);
    else
        vp->setUndefined();
    return true;
}

bool
js_SetReservedSlot(JSContext *cx, JSObject *obj, uint32 slot, const Value &v)
{
    if (!obj->isNative())
        return true;

    Class *clasp = obj->getClass();

    if (slot >= obj->numSlots()) {
        uint32 nslots = JSSLOT_FREE(clasp);
        JS_ASSERT(slot < nslots);
        if (!obj->allocSlots(cx, nslots))
            return false;
    }

    obj->setSlot(slot, v);
    GC_POKE(cx, JS_NULL);
    return true;
}

JSObject *
JSObject::getGlobal() const
{
    JSObject *obj = const_cast<JSObject *>(this);
    while (JSObject *parent = obj->getParent())
        obj = parent;
    return obj;
}

JSBool
js_ReportGetterOnlyAssignment(JSContext *cx)
{
    return JS_ReportErrorFlagsAndNumber(cx,
                                        JSREPORT_WARNING | JSREPORT_STRICT |
                                        JSREPORT_STRICT_MODE_ERROR,
                                        js_GetErrorMessage, NULL,
                                        JSMSG_GETTER_ONLY);
}

JS_FRIEND_API(JSBool)
js_GetterOnlyPropertyStub(JSContext *cx, JSObject *obj, jsid id, JSBool strict, jsval *vp)
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_GETTER_ONLY);
    return JS_FALSE;
}

#ifdef DEBUG

/*
 * Routines to print out values during debugging.  These are FRIEND_API to help
 * the debugger find them and to support temporarily hacking js_Dump* calls
 * into other code.
 */

void
dumpChars(const jschar *s, size_t n)
{
    size_t i;

    if (n == (size_t) -1) {
        while (s[++n]) ;
    }

    fputc('"', stderr);
    for (i = 0; i < n; i++) {
        if (s[i] == '\n')
            fprintf(stderr, "\\n");
        else if (s[i] == '\t')
            fprintf(stderr, "\\t");
        else if (s[i] >= 32 && s[i] < 127)
            fputc(s[i], stderr);
        else if (s[i] <= 255)
            fprintf(stderr, "\\x%02x", (unsigned int) s[i]);
        else
            fprintf(stderr, "\\u%04x", (unsigned int) s[i]);
    }
    fputc('"', stderr);
}

JS_FRIEND_API(void)
js_DumpChars(const jschar *s, size_t n)
{
    fprintf(stderr, "jschar * (%p) = ", (void *) s);
    dumpChars(s, n);
    fputc('\n', stderr);
}

void
dumpString(JSString *str)
{
    if (const jschar *chars = str->getChars(NULL))
        dumpChars(chars, str->length());
    else
        fprintf(stderr, "(oom in dumpString)");
}

JS_FRIEND_API(void)
js_DumpString(JSString *str)
{
    if (const jschar *chars = str->getChars(NULL)) {
        fprintf(stderr, "JSString* (%p) = jschar * (%p) = ",
                (void *) str, (void *) chars);
        dumpString(str);
    } else {
        fprintf(stderr, "(oom in JS_DumpString)");
    }
    fputc('\n', stderr);
}

JS_FRIEND_API(void)
js_DumpAtom(JSAtom *atom)
{
    fprintf(stderr, "JSAtom* (%p) = ", (void *) atom);
    js_DumpString(ATOM_TO_STRING(atom));
}

void
dumpValue(const Value &v)
{
    if (v.isNull())
        fprintf(stderr, "null");
    else if (v.isUndefined())
        fprintf(stderr, "undefined");
    else if (v.isInt32())
        fprintf(stderr, "%d", v.toInt32());
    else if (v.isDouble())
        fprintf(stderr, "%g", v.toDouble());
    else if (v.isString())
        dumpString(v.toString());
    else if (v.isObject() && v.toObject().isFunction()) {
        JSObject *funobj = &v.toObject();
        JSFunction *fun = GET_FUNCTION_PRIVATE(cx, funobj);
        if (fun->atom) {
            fputs("<function ", stderr);
            FileEscapedString(stderr, ATOM_TO_STRING(fun->atom), 0);
        } else {
            fputs("<unnamed function", stderr);
        }
        if (fun->isInterpreted()) {
            JSScript *script = fun->script();
            fprintf(stderr, " (%s:%u)",
                    script->filename ? script->filename : "", script->lineno);
        }
        fprintf(stderr, " at %p (JSFunction at %p)>", (void *) funobj, (void *) fun);
    } else if (v.isObject()) {
        JSObject *obj = &v.toObject();
        Class *clasp = obj->getClass();
        fprintf(stderr, "<%s%s at %p>",
                clasp->name,
                (clasp == &js_ObjectClass) ? "" : " object",
                (void *) obj);
    } else if (v.isBoolean()) {
        if (v.toBoolean())
            fprintf(stderr, "true");
        else
            fprintf(stderr, "false");
    } else if (v.isMagic()) {
        fprintf(stderr, "<invalid");
#ifdef DEBUG
        switch (v.whyMagic()) {
          case JS_ARRAY_HOLE:        fprintf(stderr, " array hole");         break;
          case JS_ARGS_HOLE:         fprintf(stderr, " args hole");          break;
          case JS_NATIVE_ENUMERATE:  fprintf(stderr, " native enumeration"); break;
          case JS_NO_ITER_VALUE:     fprintf(stderr, " no iter value");      break;
          case JS_GENERATOR_CLOSING: fprintf(stderr, " generator closing");  break;
          default:                   fprintf(stderr, " ?!");                 break;
        }
#endif
        fprintf(stderr, ">");
    } else {
        fprintf(stderr, "unexpected value");
    }
}

JS_FRIEND_API(void)
js_DumpValue(const Value &val)
{
    dumpValue(val);
    fputc('\n', stderr);
}

JS_FRIEND_API(void)
js_DumpId(jsid id)
{
    fprintf(stderr, "jsid %p = ", (void *) JSID_BITS(id));
    dumpValue(IdToValue(id));
    fputc('\n', stderr);
}

static void
DumpProperty(JSObject *obj, const Shape &shape)
{
    jsid id = shape.id;
    uint8 attrs = shape.attributes();

    fprintf(stderr, "    ((Shape *) %p) ", (void *) &shape);
    if (attrs & JSPROP_ENUMERATE) fprintf(stderr, "enumerate ");
    if (attrs & JSPROP_READONLY) fprintf(stderr, "readonly ");
    if (attrs & JSPROP_PERMANENT) fprintf(stderr, "permanent ");
    if (attrs & JSPROP_SHARED) fprintf(stderr, "shared ");
    if (shape.isAlias()) fprintf(stderr, "alias ");
    if (shape.isMethod()) fprintf(stderr, "method=%p ", (void *) &shape.methodObject());

    if (shape.hasGetterValue())
        fprintf(stderr, "getterValue=%p ", (void *) shape.getterObject());
    else if (!shape.hasDefaultGetter())
        fprintf(stderr, "getterOp=%p ", JS_FUNC_TO_DATA_PTR(void *, shape.getterOp()));

    if (shape.hasSetterValue())
        fprintf(stderr, "setterValue=%p ", (void *) shape.setterObject());
    else if (shape.setterOp() == js_watch_set)
        fprintf(stderr, "setterOp=js_watch_set ");
    else if (!shape.hasDefaultSetter())
        fprintf(stderr, "setterOp=%p ", JS_FUNC_TO_DATA_PTR(void *, shape.setterOp()));

    if (JSID_IS_ATOM(id))
        dumpString(JSID_TO_STRING(id));
    else if (JSID_IS_INT(id))
        fprintf(stderr, "%d", (int) JSID_TO_INT(id));
    else
        fprintf(stderr, "unknown jsid %p", (void *) JSID_BITS(id));
    fprintf(stderr, ": slot %d", shape.slot);
    if (obj->containsSlot(shape.slot)) {
        fprintf(stderr, " = ");
        dumpValue(obj->getSlot(shape.slot));
    } else if (shape.slot != SHAPE_INVALID_SLOT) {
        fprintf(stderr, " (INVALID!)");
    }
    fprintf(stderr, "\n");
}

JS_FRIEND_API(void)
js_DumpObject(JSObject *obj)
{
    fprintf(stderr, "object %p\n", (void *) obj);
    Class *clasp = obj->getClass();
    fprintf(stderr, "class %p %s\n", (void *)clasp, clasp->name);

    fprintf(stderr, "flags:");
    uint32 flags = obj->flags;
    if (flags & JSObject::DELEGATE) fprintf(stderr, " delegate");
    if (flags & JSObject::SYSTEM) fprintf(stderr, " system");
    if (flags & JSObject::NOT_EXTENSIBLE) fprintf(stderr, " not_extensible");
    if (flags & JSObject::BRANDED) fprintf(stderr, " branded");
    if (flags & JSObject::GENERIC) fprintf(stderr, " generic");
    if (flags & JSObject::METHOD_BARRIER) fprintf(stderr, " method_barrier");
    if (flags & JSObject::INDEXED) fprintf(stderr, " indexed");
    if (flags & JSObject::OWN_SHAPE) fprintf(stderr, " own_shape");
    if (flags & JSObject::HAS_EQUALITY) fprintf(stderr, " has_equality");

    bool anyFlags = flags != 0;
    if (obj->isNative()) {
        if (obj->inDictionaryMode()) {
            fprintf(stderr, " inDictionaryMode");
            anyFlags = true;
        }
        if (obj->hasPropertyTable()) {
            fprintf(stderr, " hasPropertyTable");
            anyFlags = true;
        }
    }
    if (!anyFlags)
        fprintf(stderr, " none");
    fprintf(stderr, "\n");

    if (obj->isDenseArray()) {
        unsigned slots = JS_MIN(obj->getArrayLength(), obj->getDenseArrayCapacity());
        fprintf(stderr, "elements\n");
        for (unsigned i = 0; i < slots; i++) {
            fprintf(stderr, " %3d: ", i);
            dumpValue(obj->getDenseArrayElement(i));
            fprintf(stderr, "\n");
            fflush(stderr);
        }
        return;
    }

    fprintf(stderr, "proto ");
    dumpValue(ObjectOrNullValue(obj->getProto()));
    fputc('\n', stderr);

    fprintf(stderr, "parent ");
    dumpValue(ObjectOrNullValue(obj->getParent()));
    fputc('\n', stderr);

    if (clasp->flags & JSCLASS_HAS_PRIVATE)
        fprintf(stderr, "private %p\n", obj->getPrivate());

    if (!obj->isNative())
        fprintf(stderr, "not native\n");

    unsigned reservedEnd = JSCLASS_RESERVED_SLOTS(clasp);
    unsigned slots = obj->slotSpan();
    unsigned stop = obj->isNative() ? reservedEnd : slots;
    if (stop > 0)
        fprintf(stderr, obj->isNative() ? "reserved slots:\n" : "slots:\n");
    for (unsigned i = 0; i < stop; i++) {
        fprintf(stderr, " %3d ", i);
        if (i < reservedEnd)
            fprintf(stderr, "(reserved) ");
        fprintf(stderr, "= ");
        dumpValue(obj->getSlot(i));
        fputc('\n', stderr);
    }

    if (obj->isNative()) {
        fprintf(stderr, "properties:\n");
        Vector<const Shape *, 8, SystemAllocPolicy> props;
        for (Shape::Range r = obj->lastProperty()->all(); !r.empty(); r.popFront())
            props.append(&r.front());
        for (size_t i = props.length(); i-- != 0;)
            DumpProperty(obj, *props[i]);
    }
    fputc('\n', stderr);
}

static void
MaybeDumpObject(const char *name, JSObject *obj)
{
    if (obj) {
        fprintf(stderr, "  %s: ", name);
        dumpValue(ObjectValue(*obj));
        fputc('\n', stderr);
    }
}

static void
MaybeDumpValue(const char *name, const Value &v)
{
    if (!v.isNull()) {
        fprintf(stderr, "  %s: ", name);
        dumpValue(v);
        fputc('\n', stderr);
    }
}

JS_FRIEND_API(void)
js_DumpStackFrame(JSContext *cx, JSStackFrame *start)
{
    /* This should only called during live debugging. */
    VOUCH_DOES_NOT_REQUIRE_STACK();

    if (!start)
        start = cx->maybefp();
    FrameRegsIter i(cx);
    while (!i.done() && i.fp() != start)
        ++i;

    if (i.done()) {
        fprintf(stderr, "fp = %p not found in cx = %p\n", (void *)start, (void *)cx);
        return;
    }

    for (; !i.done(); ++i) {
        JSStackFrame *const fp = i.fp();

        fprintf(stderr, "JSStackFrame at %p\n", (void *) fp);
        if (fp->isFunctionFrame()) {
            fprintf(stderr, "callee fun: ");
            dumpValue(ObjectValue(fp->callee()));
        } else {
            fprintf(stderr, "global frame, no callee");
        }
        fputc('\n', stderr);

        if (fp->isScriptFrame()) {
            fprintf(stderr, "file %s line %u\n",
                    fp->script()->filename, (unsigned) fp->script()->lineno);
        }

        if (jsbytecode *pc = i.pc()) {
            if (!fp->isScriptFrame()) {
                fprintf(stderr, "*** pc && !script, skipping frame\n\n");
                continue;
            }
            if (fp->hasImacropc()) {
                fprintf(stderr, "  pc in imacro at %p\n  called from ", pc);
                pc = fp->imacropc();
            } else {
                fprintf(stderr, "  ");
            }
            fprintf(stderr, "pc = %p\n", pc);
            fprintf(stderr, "  current op: %s\n", js_CodeName[*pc]);
        }
        Value *sp = i.sp();
        fprintf(stderr, "  slots: %p\n", (void *) fp->slots());
        fprintf(stderr, "  sp:    %p = slots + %u\n", (void *) sp, (unsigned) (sp - fp->slots()));
        if (sp - fp->slots() < 10000) { // sanity
            for (Value *p = fp->slots(); p < sp; p++) {
                fprintf(stderr, "    %p: ", (void *) p);
                dumpValue(*p);
                fputc('\n', stderr);
            }
        }
        if (fp->isFunctionFrame() && !fp->isEvalFrame()) {
            fprintf(stderr, "  actuals: %p (%u) ", (void *) fp->actualArgs(), (unsigned) fp->numActualArgs());
            fprintf(stderr, "  formals: %p (%u)\n", (void *) fp->formalArgs(), (unsigned) fp->numFormalArgs());
        }
        MaybeDumpObject("callobj", fp->maybeCallObj());
        MaybeDumpObject("argsobj", fp->maybeArgsObj());
        if (!fp->isDummyFrame()) {
            MaybeDumpValue("this", fp->thisValue());
            fprintf(stderr, "  rval: ");
            dumpValue(fp->returnValue());
        } else {
            fprintf(stderr, "dummy frame");
        }
        fputc('\n', stderr);

        fprintf(stderr, "  flags:");
        if (fp->isConstructing())
            fprintf(stderr, " constructing");
        if (fp->hasOverriddenArgs())
            fprintf(stderr, " overridden_args");
        if (fp->isAssigning())
            fprintf(stderr, " assigning");
        if (fp->isDebuggerFrame())
            fprintf(stderr, " debugger");
        if (fp->isEvalFrame())
            fprintf(stderr, " eval");
        if (fp->isYielding())
            fprintf(stderr, " yielding");
        if (fp->isGeneratorFrame())
            fprintf(stderr, " generator");
        fputc('\n', stderr);

        fprintf(stderr, "  scopeChain: (JSObject *) %p\n", (void *) &fp->scopeChain());

        fputc('\n', stderr);
    }
}

#endif /* DEBUG */

