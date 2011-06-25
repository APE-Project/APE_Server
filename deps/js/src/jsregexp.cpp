/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set sw=4 ts=8 et tw=99:
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
 * JS regular expressions, after Perl.
 */
#include <stdlib.h>
#include <string.h>
#include "jstypes.h"
#include "jsstdint.h"
#include "jsutil.h"
#include "jsapi.h"
#include "jscntxt.h"
#include "jsgc.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsregexp.h"
#include "jsstr.h"
#include "jsvector.h"

#include "jsobjinlines.h"
#include "jsregexpinlines.h"

#include "yarr/RegexParser.h"

#ifdef JS_TRACER
#include "jstracer.h"
using namespace avmplus;
using namespace nanojit;
#endif

using namespace js;
using namespace js::gc;

/*
 * RegExpStatics allocates memory -- in order to keep the statics stored
 * per-global and not leak, we create a js::Class to wrap the C++ instance and
 * provide an appropriate finalizer. We store an instance of that js::Class in
 * a global reserved slot.
 */

static void
resc_finalize(JSContext *cx, JSObject *obj)
{
    RegExpStatics *res = static_cast<RegExpStatics *>(obj->getPrivate());
    cx->destroy<RegExpStatics>(res);
}

static void
resc_trace(JSTracer *trc, JSObject *obj)
{
    void *pdata = obj->getPrivate();
    JS_ASSERT(pdata);
    RegExpStatics *res = static_cast<RegExpStatics *>(pdata);
    res->mark(trc);
}

Class js::regexp_statics_class = {
    "RegExpStatics", 
    JSCLASS_HAS_PRIVATE | JSCLASS_MARK_IS_TRACE,
    PropertyStub,         /* addProperty */
    PropertyStub,         /* delProperty */
    PropertyStub,         /* getProperty */
    StrictPropertyStub,   /* setProperty */
    EnumerateStub,
    ResolveStub,
    ConvertStub,
    resc_finalize,
    NULL,                 /* reserved0   */
    NULL,                 /* checkAccess */
    NULL,                 /* call        */
    NULL,                 /* construct   */
    NULL,                 /* xdrObject   */
    NULL,                 /* hasInstance */
    JS_CLASS_TRACE(resc_trace)
};

/*
 * Replace the regexp internals of |obj| with |newRegExp|.
 * Decref the replaced regexp internals.
 * Note that the refcount of |newRegExp| is unchanged.
 */
static void
SwapObjectRegExp(JSContext *cx, JSObject *obj, AlreadyIncRefed<RegExp> newRegExp)
{
    RegExp *oldRegExp = RegExp::extractFrom(obj);
#ifdef DEBUG
    if (oldRegExp)
        assertSameCompartment(cx, obj, oldRegExp->compartment);
    assertSameCompartment(cx, obj, newRegExp->compartment);
#endif

    obj->setPrivate(newRegExp.get());
    obj->zeroRegExpLastIndex();
    if (oldRegExp)
        oldRegExp->decref(cx);
}

JSObject * JS_FASTCALL
js_CloneRegExpObject(JSContext *cx, JSObject *obj, JSObject *proto)
{
    JS_ASSERT(obj->getClass() == &js_RegExpClass);
    JS_ASSERT(proto);
    JS_ASSERT(proto->getClass() == &js_RegExpClass);

    JSObject *clone = NewNativeClassInstance(cx, &js_RegExpClass, proto, proto->getParent());
    if (!clone)
        return NULL;

    /* 
     * This clone functionality does not duplicate the JITted code blob, which is necessary for
     * cross-compartment cloning functionality.
     */
    assertSameCompartment(cx, obj, clone);

    RegExpStatics *res = cx->regExpStatics();
    RegExp *re = RegExp::extractFrom(obj);
    {
        uint32 origFlags = re->getFlags();
        uint32 staticsFlags = res->getFlags();
        if ((origFlags & staticsFlags) != staticsFlags) {
            /*
             * This regex is lacking flags from the statics, so we must recompile with the new
             * flags instead of increffing.
             */
            AlreadyIncRefed<RegExp> clone = RegExp::create(cx, re->getSource(), origFlags | staticsFlags);
            if (!clone)
                return NULL;
            re = clone.get();
        } else {
            re->incref(cx);
        }
    }
    JS_ASSERT(re);
    clone->setPrivate(re);
    clone->zeroRegExpLastIndex();
    return clone;
}

#ifdef JS_TRACER
JS_DEFINE_CALLINFO_3(extern, OBJECT, js_CloneRegExpObject, CONTEXT, OBJECT, OBJECT, 0,
                     ACCSET_STORE_ANY)
#endif

JSBool
js_ObjectIsRegExp(JSObject *obj)
{
    return obj->isRegExp();
}

/*
 * js::RegExp
 */

void
RegExp::handleYarrError(JSContext *cx, int error)
{
    switch (error) {
      case JSC::Yarr::NoError:
        JS_NOT_REACHED("Precondition violation: an error must have occurred.");
        return;
#define COMPILE_EMSG(__code, __msg) \
      case JSC::Yarr::__code: \
        JS_ReportErrorFlagsAndNumberUC(cx, JSREPORT_ERROR, js_GetErrorMessage, NULL, __msg); \
        return
      COMPILE_EMSG(PatternTooLarge, JSMSG_REGEXP_TOO_COMPLEX);
      COMPILE_EMSG(QuantifierOutOfOrder, JSMSG_BAD_QUANTIFIER);
      COMPILE_EMSG(QuantifierWithoutAtom, JSMSG_BAD_QUANTIFIER);
      COMPILE_EMSG(MissingParentheses, JSMSG_MISSING_PAREN);
      COMPILE_EMSG(ParenthesesUnmatched, JSMSG_UNMATCHED_RIGHT_PAREN);
      COMPILE_EMSG(ParenthesesTypeInvalid, JSMSG_BAD_QUANTIFIER); /* "(?" with bad next char */
      COMPILE_EMSG(CharacterClassUnmatched, JSMSG_BAD_CLASS_RANGE);
      COMPILE_EMSG(CharacterClassOutOfOrder, JSMSG_BAD_CLASS_RANGE);
      COMPILE_EMSG(CharacterClassRangeSingleChar, JSMSG_BAD_CLASS_RANGE);
      COMPILE_EMSG(EscapeUnterminated, JSMSG_TRAILING_SLASH);
      COMPILE_EMSG(QuantifierTooLarge, JSMSG_BAD_QUANTIFIER);
      COMPILE_EMSG(HitRecursionLimit, JSMSG_REGEXP_TOO_COMPLEX);
#undef COMPILE_EMSG
      default:
        JS_NOT_REACHED("Precondition violation: unknown Yarr error code.");
    }
}

void
RegExp::handlePCREError(JSContext *cx, int error)
{
#define REPORT(msg_) \
    JS_ReportErrorFlagsAndNumberUC(cx, JSREPORT_ERROR, js_GetErrorMessage, NULL, msg_); \
    return
    switch (error) {
      case -2: REPORT(JSMSG_REGEXP_TOO_COMPLEX);
      case 0: JS_NOT_REACHED("Precondition violation: an error must have occurred.");
      case 1: REPORT(JSMSG_TRAILING_SLASH);
      case 2: REPORT(JSMSG_TRAILING_SLASH);
      case 3: REPORT(JSMSG_REGEXP_TOO_COMPLEX);
      case 4: REPORT(JSMSG_BAD_QUANTIFIER);
      case 5: REPORT(JSMSG_BAD_QUANTIFIER);
      case 6: REPORT(JSMSG_BAD_CLASS_RANGE);
      case 7: REPORT(JSMSG_REGEXP_TOO_COMPLEX);
      case 8: REPORT(JSMSG_BAD_CLASS_RANGE);
      case 9: REPORT(JSMSG_BAD_QUANTIFIER);
      case 10: REPORT(JSMSG_UNMATCHED_RIGHT_PAREN);
      case 11: REPORT(JSMSG_REGEXP_TOO_COMPLEX);
      case 12: REPORT(JSMSG_UNMATCHED_RIGHT_PAREN);
      case 13: REPORT(JSMSG_REGEXP_TOO_COMPLEX);
      case 14: REPORT(JSMSG_MISSING_PAREN);
      case 15: REPORT(JSMSG_BAD_BACKREF);
      case 16: REPORT(JSMSG_REGEXP_TOO_COMPLEX);
      case 17: REPORT(JSMSG_REGEXP_TOO_COMPLEX);
      default:
        JS_NOT_REACHED("Precondition violation: unknown PCRE error code.");
    }
#undef REPORT
}

bool
RegExp::parseFlags(JSContext *cx, JSString *flagStr, uintN *flagsOut)
{
    size_t n = flagStr->length();
    const jschar *s = flagStr->getChars(cx);
    if (!s)
        return false;

    *flagsOut = 0;
    for (size_t i = 0; i < n; i++) {
#define HANDLE_FLAG(name_)                                                    \
        JS_BEGIN_MACRO                                                        \
            if (*flagsOut & (name_))                                          \
                goto bad_flag;                                                \
            *flagsOut |= (name_);                                             \
        JS_END_MACRO
        switch (s[i]) {
          case 'i': HANDLE_FLAG(JSREG_FOLD); break;
          case 'g': HANDLE_FLAG(JSREG_GLOB); break;
          case 'm': HANDLE_FLAG(JSREG_MULTILINE); break;
          case 'y': HANDLE_FLAG(JSREG_STICKY); break;
          default:
          bad_flag:
          {
            char charBuf[2];
            charBuf[0] = char(s[i]);
            charBuf[1] = '\0';
            JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR, js_GetErrorMessage, NULL,
                                         JSMSG_BAD_REGEXP_FLAG, charBuf);
            return false;
          }
        }
#undef HANDLE_FLAG
    }
    return true;
}

AlreadyIncRefed<RegExp>
RegExp::createFlagged(JSContext *cx, JSString *str, JSString *opt)
{
    if (!opt)
        return create(cx, str, 0);
    uintN flags = 0;
    if (!parseFlags(cx, opt, &flags))
        return AlreadyIncRefed<RegExp>(NULL);
    return create(cx, str, flags);
}

/*
 * RegExp instance properties.
 */
#define DEFINE_GETTER(name, code)                                              \
    static JSBool                                                              \
    name(JSContext *cx, JSObject *obj, jsid id, Value *vp)                     \
    {                                                                          \
        while (obj->getClass() != &js_RegExpClass) {                           \
            obj = obj->getProto();                                             \
            if (!obj)                                                          \
                return true;                                                   \
        }                                                                      \
        RegExp *re = RegExp::extractFrom(obj);                                 \
        code;                                                                  \
        return true;                                                           \
    }

/* lastIndex is stored in the object, re = re silences the compiler warning. */
DEFINE_GETTER(lastIndex_getter,  re = re; *vp = obj->getRegExpLastIndex())
DEFINE_GETTER(source_getter,     *vp = StringValue(re->getSource()))
DEFINE_GETTER(global_getter,     *vp = BooleanValue(re->global()))
DEFINE_GETTER(ignoreCase_getter, *vp = BooleanValue(re->ignoreCase()))
DEFINE_GETTER(multiline_getter,  *vp = BooleanValue(re->multiline()))
DEFINE_GETTER(sticky_getter,     *vp = BooleanValue(re->sticky()))

static JSBool
lastIndex_setter(JSContext *cx, JSObject *obj, jsid id, JSBool strict, Value *vp)
{
    while (obj->getClass() != &js_RegExpClass) {
        obj = obj->getProto();
        if (!obj)
            return true;
    }
    obj->setRegExpLastIndex(*vp);
    return true;
}

static const struct LazyProp {
    const char *name;
    uint16 atomOffset;
    PropertyOp getter;
} lazyRegExpProps[] = {
    { js_source_str,     ATOM_OFFSET(source),     source_getter },
    { js_global_str,     ATOM_OFFSET(global),     global_getter },
    { js_ignoreCase_str, ATOM_OFFSET(ignoreCase), ignoreCase_getter },
    { js_multiline_str,  ATOM_OFFSET(multiline),  multiline_getter },
    { js_sticky_str,     ATOM_OFFSET(sticky),     sticky_getter }
};

static JSBool
regexp_resolve(JSContext *cx, JSObject *obj, jsid id, uint32 flags, JSObject **objp)
{
    JS_ASSERT(obj->isRegExp());

    if (!JSID_IS_ATOM(id))
        return JS_TRUE;

    if (id == ATOM_TO_JSID(cx->runtime->atomState.lastIndexAtom)) {
        if (!js_DefineNativeProperty(cx, obj, id, UndefinedValue(),
                                     lastIndex_getter, lastIndex_setter,
                                     JSPROP_PERMANENT | JSPROP_SHARED,
                                     0, 0, NULL)) {
            return false;
        }
        *objp = obj;
        return true;
    }

    for (size_t i = 0; i < JS_ARRAY_LENGTH(lazyRegExpProps); i++) {
        const LazyProp &lazy = lazyRegExpProps[i];
        JSAtom *atom = OFFSET_TO_ATOM(cx->runtime, lazy.atomOffset);
        if (id == ATOM_TO_JSID(atom)) {
            if (!js_DefineNativeProperty(cx, obj, id, UndefinedValue(),
                                         lazy.getter, NULL,
                                         JSPROP_PERMANENT | JSPROP_SHARED | JSPROP_READONLY,
                                         0, 0, NULL)) {
                return false;
            }
            *objp = obj;
            return true;
        }
    }

    return true;
}

/*
 * RegExp static properties.
 *
 * RegExp class static properties and their Perl counterparts:
 *
 *  RegExp.input                $_
 *  RegExp.multiline            $*
 *  RegExp.lastMatch            $&
 *  RegExp.lastParen            $+
 *  RegExp.leftContext          $`
 *  RegExp.rightContext         $'
 */

#define DEFINE_STATIC_GETTER(name, code)                                        \
    static JSBool                                                               \
    name(JSContext *cx, JSObject *obj, jsid id, jsval *vp)                      \
    {                                                                           \
        RegExpStatics *res = cx->regExpStatics();                               \
        code;                                                                   \
    }

DEFINE_STATIC_GETTER(static_input_getter,        return res->createPendingInput(cx, Valueify(vp)))
DEFINE_STATIC_GETTER(static_multiline_getter,    *vp = BOOLEAN_TO_JSVAL(res->multiline());
                                                 return true)
DEFINE_STATIC_GETTER(static_lastMatch_getter,    return res->createLastMatch(cx, Valueify(vp)))
DEFINE_STATIC_GETTER(static_lastParen_getter,    return res->createLastParen(cx, Valueify(vp)))
DEFINE_STATIC_GETTER(static_leftContext_getter,  return res->createLeftContext(cx, Valueify(vp)))
DEFINE_STATIC_GETTER(static_rightContext_getter, return res->createRightContext(cx, Valueify(vp)))

DEFINE_STATIC_GETTER(static_paren1_getter,       return res->createParen(cx, 1, Valueify(vp)))
DEFINE_STATIC_GETTER(static_paren2_getter,       return res->createParen(cx, 2, Valueify(vp)))
DEFINE_STATIC_GETTER(static_paren3_getter,       return res->createParen(cx, 3, Valueify(vp)))
DEFINE_STATIC_GETTER(static_paren4_getter,       return res->createParen(cx, 4, Valueify(vp)))
DEFINE_STATIC_GETTER(static_paren5_getter,       return res->createParen(cx, 5, Valueify(vp)))
DEFINE_STATIC_GETTER(static_paren6_getter,       return res->createParen(cx, 6, Valueify(vp)))
DEFINE_STATIC_GETTER(static_paren7_getter,       return res->createParen(cx, 7, Valueify(vp)))
DEFINE_STATIC_GETTER(static_paren8_getter,       return res->createParen(cx, 8, Valueify(vp)))
DEFINE_STATIC_GETTER(static_paren9_getter,       return res->createParen(cx, 9, Valueify(vp)))

#define DEFINE_STATIC_SETTER(name, code)                                        \
    static JSBool                                                               \
    name(JSContext *cx, JSObject *obj, jsid id, JSBool strict, jsval *vp)       \
    {                                                                           \
        RegExpStatics *res = cx->regExpStatics();                               \
        code;                                                                   \
        return true;                                                            \
    }

DEFINE_STATIC_SETTER(static_input_setter,
                     if (!JSVAL_IS_STRING(*vp) && !JS_ConvertValue(cx, *vp, JSTYPE_STRING, vp))
                         return false;
                     res->setPendingInput(JSVAL_TO_STRING(*vp)))
DEFINE_STATIC_SETTER(static_multiline_setter,
                     if (!JSVAL_IS_BOOLEAN(*vp) && !JS_ConvertValue(cx, *vp, JSTYPE_BOOLEAN, vp))
                         return false;
                     res->setMultiline(!!JSVAL_TO_BOOLEAN(*vp)))

const uint8 REGEXP_STATIC_PROP_ATTRS    = JSPROP_PERMANENT | JSPROP_SHARED | JSPROP_ENUMERATE;
const uint8 RO_REGEXP_STATIC_PROP_ATTRS = REGEXP_STATIC_PROP_ATTRS | JSPROP_READONLY;

static JSPropertySpec regexp_static_props[] = {
    {"input",        0, REGEXP_STATIC_PROP_ATTRS,    static_input_getter, static_input_setter},
    {"multiline",    0, REGEXP_STATIC_PROP_ATTRS,    static_multiline_getter,
                                                     static_multiline_setter},
    {"lastMatch",    0, RO_REGEXP_STATIC_PROP_ATTRS, static_lastMatch_getter,    NULL},
    {"lastParen",    0, RO_REGEXP_STATIC_PROP_ATTRS, static_lastParen_getter,    NULL},
    {"leftContext",  0, RO_REGEXP_STATIC_PROP_ATTRS, static_leftContext_getter,  NULL},
    {"rightContext", 0, RO_REGEXP_STATIC_PROP_ATTRS, static_rightContext_getter, NULL},
    {"$1",           0, RO_REGEXP_STATIC_PROP_ATTRS, static_paren1_getter,       NULL},
    {"$2",           0, RO_REGEXP_STATIC_PROP_ATTRS, static_paren2_getter,       NULL},
    {"$3",           0, RO_REGEXP_STATIC_PROP_ATTRS, static_paren3_getter,       NULL},
    {"$4",           0, RO_REGEXP_STATIC_PROP_ATTRS, static_paren4_getter,       NULL},
    {"$5",           0, RO_REGEXP_STATIC_PROP_ATTRS, static_paren5_getter,       NULL},
    {"$6",           0, RO_REGEXP_STATIC_PROP_ATTRS, static_paren6_getter,       NULL},
    {"$7",           0, RO_REGEXP_STATIC_PROP_ATTRS, static_paren7_getter,       NULL},
    {"$8",           0, RO_REGEXP_STATIC_PROP_ATTRS, static_paren8_getter,       NULL},
    {"$9",           0, RO_REGEXP_STATIC_PROP_ATTRS, static_paren9_getter,       NULL},
    {0,0,0,0,0}
};

static void
regexp_finalize(JSContext *cx, JSObject *obj)
{
    RegExp *re = RegExp::extractFrom(obj);
    if (!re)
        return;
    re->decref(cx);
}

/* Forward static prototype. */
static JSBool
regexp_exec_sub(JSContext *cx, JSObject *obj, uintN argc, Value *argv, JSBool test, Value *rval);

static JSBool
regexp_call(JSContext *cx, uintN argc, Value *vp)
{
    return regexp_exec_sub(cx, &JS_CALLEE(cx, vp).toObject(), argc, JS_ARGV(cx, vp), false, vp);
}

#if JS_HAS_XDR

#include "jsxdrapi.h"

JSBool
js_XDRRegExpObject(JSXDRState *xdr, JSObject **objp)
{
    JSString *source = 0;
    uint32 flagsword = 0;

    if (xdr->mode == JSXDR_ENCODE) {
        JS_ASSERT(objp);
        RegExp *re = RegExp::extractFrom(*objp);
        if (!re)
            return false;
        source = re->getSource();
        flagsword = re->getFlags();
    }
    if (!JS_XDRString(xdr, &source) || !JS_XDRUint32(xdr, &flagsword))
        return false;
    if (xdr->mode == JSXDR_DECODE) {
        JSObject *obj = NewBuiltinClassInstance(xdr->cx, &js_RegExpClass);
        if (!obj)
            return false;
        obj->clearParent();
        obj->clearProto();
        AlreadyIncRefed<RegExp> re = RegExp::create(xdr->cx, source, flagsword);
        if (!re)
            return false;
        obj->setPrivate(re.get());
        obj->zeroRegExpLastIndex();
        *objp = obj;
    }
    return true;
}

#else  /* !JS_HAS_XDR */

#define js_XDRRegExpObject NULL

#endif /* !JS_HAS_XDR */

static void
regexp_trace(JSTracer *trc, JSObject *obj)
{
    RegExp *re = RegExp::extractFrom(obj);
    if (re && re->getSource())
        MarkString(trc, re->getSource(), "source");
}

static JSBool
regexp_enumerate(JSContext *cx, JSObject *obj)
{
    JS_ASSERT(obj->isRegExp());

    jsval v;
    if (!JS_LookupPropertyById(cx, obj, ATOM_TO_JSID(cx->runtime->atomState.lastIndexAtom), &v))
        return false;

    for (size_t i = 0; i < JS_ARRAY_LENGTH(lazyRegExpProps); i++) {
        const LazyProp &lazy = lazyRegExpProps[i];
        jsid id = ATOM_TO_JSID(OFFSET_TO_ATOM(cx->runtime, lazy.atomOffset));
        if (!JS_LookupPropertyById(cx, obj, id, &v))
            return false;
    }

    return true;
}

js::Class js_RegExpClass = {
    js_RegExp_str,
    JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE |
    JSCLASS_HAS_RESERVED_SLOTS(JSObject::REGEXP_CLASS_RESERVED_SLOTS) |
    JSCLASS_MARK_IS_TRACE | JSCLASS_HAS_CACHED_PROTO(JSProto_RegExp),
    PropertyStub,         /* addProperty */
    PropertyStub,         /* delProperty */
    PropertyStub,         /* getProperty */
    StrictPropertyStub,   /* setProperty */
    regexp_enumerate,
    reinterpret_cast<JSResolveOp>(regexp_resolve),
    ConvertStub,
    regexp_finalize,
    NULL,                 /* reserved0 */
    NULL,                 /* checkAccess */
    regexp_call,
    NULL,                 /* construct */
    js_XDRRegExpObject,
    NULL,                 /* hasInstance */
    JS_CLASS_TRACE(regexp_trace)
};

/*
 * RegExp instance methods.
 */

JSBool
js_regexp_toString(JSContext *cx, JSObject *obj, Value *vp)
{
    if (!InstanceOf(cx, obj, &js_RegExpClass, vp + 2))
        return false;

    RegExp *re = RegExp::extractFrom(obj);
    if (!re) {
        *vp = StringValue(cx->runtime->emptyString);
        return true;
    }

    JSLinearString *src = re->getSource();
    StringBuffer sb(cx);
    if (size_t len = src->length()) {
        if (!sb.reserve(len + 2))
            return false;
        JS_ALWAYS_TRUE(sb.append('/'));
        JS_ALWAYS_TRUE(sb.append(src->chars(), len));
        JS_ALWAYS_TRUE(sb.append('/'));
    } else {
        if (!sb.append("/(?:)/"))
            return false;
    }
    if (re->global() && !sb.append('g'))
        return false;
    if (re->ignoreCase() && !sb.append('i'))
        return false;
    if (re->multiline() && !sb.append('m'))
        return false;
    if (re->sticky() && !sb.append('y'))
        return false;

    JSFlatString *str = sb.finishString();
    if (!str)
        return false;
    *vp = StringValue(str);
    return true;
}

static JSBool
regexp_toString(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj)
        return false;
    return js_regexp_toString(cx, obj, vp);
}

/*
 * Return:
 * - The original if no escaping need be performed.
 * - A new string if escaping need be performed.
 * - NULL on error.
 */
static JSString *
EscapeNakedForwardSlashes(JSContext *cx, JSString *unescaped)
{
    size_t oldLen = unescaped->length();
    const jschar *oldChars = unescaped->getChars(cx);
    if (!oldChars)
        return NULL;
    JS::Anchor<JSString *> anchor(unescaped);

    js::Vector<jschar, 128> newChars(cx);
    for (const jschar *it = oldChars; it < oldChars + oldLen; ++it) {
        if (*it == '/' && (it == oldChars || it[-1] != '\\')) {
            if (!newChars.length()) {
                if (!newChars.reserve(oldLen + 1))
                    return NULL;
                JS_ALWAYS_TRUE(newChars.append(oldChars, size_t(it - oldChars)));
            }
            if (!newChars.append('\\'))
                return NULL;
        }

        if (!newChars.empty() && !newChars.append(*it))
            return NULL;
    }

    if (newChars.length()) {
        size_t len = newChars.length();
        if (!newChars.append('\0'))
            return NULL;
        jschar *chars = newChars.extractRawBuffer();
        JSString *escaped = js_NewString(cx, chars, len);
        if (!escaped)
            cx->free(chars);
        return escaped;
    }
    return unescaped;
}

static bool
SwapRegExpInternals(JSContext *cx, JSObject *obj, Value *rval, JSString *str, uint32 flags = 0)
{
    flags |= cx->regExpStatics()->getFlags();
    AlreadyIncRefed<RegExp> re = RegExp::create(cx, str, flags);
    if (!re)
        return false;
    SwapObjectRegExp(cx, obj, re);
    *rval = ObjectValue(*obj);
    return true;
}

static JSBool
regexp_exec_sub(JSContext *cx, JSObject *obj, uintN argc, Value *argv, JSBool test, Value *rval)
{
    if (!InstanceOf(cx, obj, &js_RegExpClass, argv))
        return false;

    RegExp *re = RegExp::extractFrom(obj);
    if (!re)
        return true;

    /* 
     * Code execution under this call could swap out the guts of |obj|, so we
     * have to take a defensive refcount here.
     */
    AutoRefCount<RegExp> arc(cx, NeedsIncRef<RegExp>(re));

    jsdouble lastIndex;
    if (re->global() || re->sticky()) {
        const Value v = obj->getRegExpLastIndex();
        if (v.isInt32()) {
            lastIndex = v.toInt32();
        } else {
            if (v.isDouble())
                lastIndex = v.toDouble();
            else if (!ValueToNumber(cx, v, &lastIndex))
                return JS_FALSE;
            lastIndex = js_DoubleToInteger(lastIndex);
        }
    } else {
        lastIndex = 0;
    }

    RegExpStatics *res = cx->regExpStatics();

    JSString *input;
    if (argc) {
        input = js_ValueToString(cx, argv[0]);
        if (!input)
            return false;
        argv[0] = StringValue(input);
    } else {
        /* Need to grab input from statics. */
        input = res->getPendingInput();
        if (!input) {
            JSAutoByteString sourceBytes(cx, re->getSource());
            if (!!sourceBytes) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NO_INPUT,
                                     sourceBytes.ptr(),
                                     re->global() ? "g" : "",
                                     re->ignoreCase() ? "i" : "",
                                     re->multiline() ? "m" : "",
                                     re->sticky() ? "y" : "");
            }
            return false;
        }
    }

    if (lastIndex < 0 || input->length() < lastIndex) {
        obj->zeroRegExpLastIndex();
        *rval = NullValue();
        return true;
    }

    size_t lastIndexInt(lastIndex);
    if (!re->execute(cx, res, input, &lastIndexInt, !!test, rval))
        return false;

    /* Update lastIndex. */
    if (re->global() || (!rval->isNull() && re->sticky())) {
        if (rval->isNull())
            obj->zeroRegExpLastIndex();
        else
            obj->setRegExpLastIndex(lastIndexInt);
    }

    return true;
}

JSBool
js_regexp_exec(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj)
        return false;
    return regexp_exec_sub(cx, obj, argc, vp + 2, JS_FALSE, vp);
}

JSBool
js_regexp_test(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj)
        return false;
    if (!regexp_exec_sub(cx, obj, argc, vp + 2, JS_TRUE, vp))
        return false;
    if (!vp->isTrue())
        vp->setBoolean(false);
    return true;
}

/*
 * Compile new js::RegExp guts for obj.
 *
 * Per ECMAv5 15.10.4.1, we act on combinations of (pattern, flags) as
 * arguments:
 *
 *  RegExp, undefined => flags := pattern.flags
 *  RegExp, _ => throw TypeError
 *  _ => pattern := ToString(pattern) if defined(pattern) else ''
 *       flags := ToString(flags) if defined(flags) else ''
 */
static bool
CompileRegExpAndSwap(JSContext *cx, JSObject *obj, uintN argc, Value *argv, Value *rval)
{
    if (argc == 0)
        return SwapRegExpInternals(cx, obj, rval, cx->runtime->emptyString);

    Value sourceValue = argv[0];
    if (sourceValue.isObject() && sourceValue.toObject().getClass() == &js_RegExpClass) {
        /*
         * If we get passed in a RegExp object we return a new object with the
         * same RegExp (internal matcher program) guts.
         * Note: the regexp static flags are not taken into consideration here.
         */
        JSObject &sourceObj = sourceValue.toObject();
        if (argc >= 2 && !argv[1].isUndefined()) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NEWREGEXP_FLAGGED);
            return false;
        }
        RegExp *re = RegExp::extractFrom(&sourceObj);
        if (!re)
            return false;

        re->incref(cx);
        SwapObjectRegExp(cx, obj, AlreadyIncRefed<RegExp>(re));

        *rval = ObjectValue(*obj);
        return true;
    }

    JSString *sourceStr;
    if (sourceValue.isUndefined()) {
        sourceStr = cx->runtime->emptyString;
    } else {
        /* Coerce to string and compile. */
        sourceStr = js_ValueToString(cx, sourceValue);
        if (!sourceStr)
            return false;
    }  

    uintN flags = 0;
    if (argc > 1 && !argv[1].isUndefined()) {
        JSString *flagStr = js_ValueToString(cx, argv[1]);
        if (!flagStr)
            return false;
        argv[1].setString(flagStr);
        if (!RegExp::parseFlags(cx, flagStr, &flags))
            return false;
    }

    JSString *escapedSourceStr = EscapeNakedForwardSlashes(cx, sourceStr);
    if (!escapedSourceStr)
        return false;

    return SwapRegExpInternals(cx, obj, rval, escapedSourceStr, flags);
}

static JSBool
regexp_compile(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj || !InstanceOf(cx, obj, &js_RegExpClass, JS_ARGV(cx, vp)))
        return false;

    return CompileRegExpAndSwap(cx, obj, argc, JS_ARGV(cx, vp), &JS_RVAL(cx, vp));
}

static JSBool
regexp_construct(JSContext *cx, uintN argc, Value *vp)
{
    Value *argv = JS_ARGV(cx, vp);

    if (!IsConstructing(vp)) {
        /*
         * If first arg is regexp and no flags are given, just return the arg.
         * Otherwise, delegate to the standard constructor.
         * See ECMAv5 15.10.3.1.
         */
        if (argc >= 1 && argv[0].isObject() && argv[0].toObject().isRegExp() &&
            (argc == 1 || argv[1].isUndefined())) {
            *vp = argv[0];
            return true;
        }
    }

    JSObject *obj = NewBuiltinClassInstance(cx, &js_RegExpClass);
    if (!obj)
        return false;

    return CompileRegExpAndSwap(cx, obj, argc, argv, &JS_RVAL(cx, vp));
}

static JSFunctionSpec regexp_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,  regexp_toString,    0,0),
#endif
    JS_FN(js_toString_str,  regexp_toString,    0,0),
    JS_FN("compile",        regexp_compile,     2,0),
    JS_FN("exec",           js_regexp_exec,     1,0),
    JS_FN("test",           js_regexp_test,     1,0),
    JS_FS_END
};

/* Similar to SwapRegExpInternals. */
static bool
InitRegExpClassCompile(JSContext *cx, JSObject *obj)
{
    AlreadyIncRefed<RegExp> re = RegExp::create(cx, cx->runtime->emptyString, 0);
    if (!re)
        return false;
    SwapObjectRegExp(cx, obj, re);
    return true;
}

JSObject *
js_InitRegExpClass(JSContext *cx, JSObject *obj)
{
    JSObject *proto = js_InitClass(cx, obj, NULL, &js_RegExpClass, regexp_construct, 2,
                                   NULL, regexp_methods, regexp_static_props, NULL);
    if (!proto)
        return NULL;

    JSObject *ctor = JS_GetConstructor(cx, proto);
    if (!ctor)
        return NULL;

    /* Give RegExp.prototype private data so it matches the empty string. */
    if (!JS_AliasProperty(cx, ctor, "input",        "$_") ||
        !JS_AliasProperty(cx, ctor, "multiline",    "$*") ||
        !JS_AliasProperty(cx, ctor, "lastMatch",    "$&") ||
        !JS_AliasProperty(cx, ctor, "lastParen",    "$+") ||
        !JS_AliasProperty(cx, ctor, "leftContext",  "$`") ||
        !JS_AliasProperty(cx, ctor, "rightContext", "$'") ||
        !InitRegExpClassCompile(cx, proto)) {
        return NULL;
    }

    return proto;
}
