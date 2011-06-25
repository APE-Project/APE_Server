/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
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
 * The Original Code is Mozilla SpiderMonkey JavaScript 1.9 code, released
 * May 28, 2008.
 *
 * The Initial Developer of the Original Code is
 *   Brendan Eich <brendan@mozilla.org>
 *
 * Contributor(s):
 *   David Anderson <danderson@mozilla.com>
 *   David Mandelin <dmandelin@mozilla.com>
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

#include "jscntxt.h"
#include "jsscope.h"
#include "jsobj.h"
#include "jslibmath.h"
#include "jsiter.h"
#include "jsnum.h"
#include "jsxml.h"
#include "jsstaticcheck.h"
#include "jsbool.h"
#include "assembler/assembler/MacroAssemblerCodeRef.h"
#include "jsiter.h"
#include "jstypes.h"
#include "methodjit/Compiler.h"
#include "methodjit/StubCalls.h"
#include "jstracer.h"

#include "jsinterpinlines.h"
#include "jspropertycache.h"
#include "jspropertycacheinlines.h"
#include "jsscopeinlines.h"
#include "jsscriptinlines.h"
#include "jsstrinlines.h"
#include "jsobjinlines.h"
#include "jscntxtinlines.h"
#include "jsatominlines.h"
#include "StubCalls-inl.h"
#include "jsfuninlines.h"
#include "jstypedarray.h"

#ifdef XP_WIN
# include "jswin.h"
#endif

#include "jsautooplen.h"

using namespace js;
using namespace js::mjit;
using namespace JSC;

void JS_FASTCALL
stubs::BindName(VMFrame &f)
{
    PropertyCacheEntry *entry;

    /* Fast-path should have caught this. See comment in interpreter. */
    JS_ASSERT(f.fp()->scopeChain().getParent());

    JSAtom *atom;
    JSObject *obj2;
    JSContext *cx = f.cx;
    JSObject *obj = &f.fp()->scopeChain();
    JS_PROPERTY_CACHE(cx).test(cx, f.regs.pc, obj, obj2, entry, atom);
    if (atom) {
        jsid id = ATOM_TO_JSID(atom);
        obj = js_FindIdentifierBase(cx, &f.fp()->scopeChain(), id);
        if (!obj)
            THROW();
    }
    f.regs.sp++;
    f.regs.sp[-1].setObject(*obj);
}

void JS_FASTCALL
stubs::BindNameNoCache(VMFrame &f, JSAtom *atom)
{
    JSObject *obj = js_FindIdentifierBase(f.cx, &f.fp()->scopeChain(), ATOM_TO_JSID(atom));
    if (!obj)
        THROW();
    f.regs.sp[0].setObject(*obj);
}

JSObject * JS_FASTCALL
stubs::BindGlobalName(VMFrame &f)
{
    return f.fp()->scopeChain().getGlobal();
}

template<JSBool strict>
void JS_FASTCALL
stubs::SetName(VMFrame &f, JSAtom *origAtom)
{
    JSContext *cx = f.cx;

    Value rval = f.regs.sp[-1];
    Value &lref = f.regs.sp[-2];
    JSObject *obj = ValueToObject(cx, &lref);
    if (!obj)
        THROW();

    do {
        PropertyCache *cache = &JS_PROPERTY_CACHE(cx);

        /*
         * Probe the property cache, specializing for two important
         * set-property cases. First:
         *
         *   function f(a, b, c) {
         *     var o = {p:a, q:b, r:c};
         *     return o;
         *   }
         *
         * or similar real-world cases, which evolve a newborn native
         * object predicatably through some bounded number of property
         * additions. And second:
         *
         *   o.p = x;
         *
         * in a frequently executed method or loop body, where p will
         * (possibly after the first iteration) always exist in native
         * object o.
         */
        PropertyCacheEntry *entry;
        JSObject *obj2;
        JSAtom *atom;
        if (cache->testForSet(cx, f.regs.pc, obj, &entry, &obj2, &atom)) {
            /*
             * Property cache hit, only partially confirmed by testForSet. We
             * know that the entry applies to regs.pc and that obj's shape
             * matches.
             *
             * The entry predicts either a new property to be added directly to
             * obj by this set, or on an existing "own" property, or on a
             * prototype property that has a setter.
             */
            const Shape *shape = entry->vword.toShape();
            JS_ASSERT_IF(shape->isDataDescriptor(), shape->writable());
            JS_ASSERT_IF(shape->hasSlot(), entry->vcapTag() == 0);

            /*
             * Fastest path: check whether obj already has the cached shape and
             * call NATIVE_SET and break to get out of the do-while(0). But we
             * can call NATIVE_SET only for a direct or proto-setter hit.
             */
            if (!entry->adding()) {
                if (entry->vcapTag() == 0 ||
                    ((obj2 = obj->getProto()) && obj2->shape() == entry->vshape()))
                {
#ifdef DEBUG
                    if (entry->directHit()) {
                        JS_ASSERT(obj->nativeContains(*shape));
                    } else {
                        JS_ASSERT(obj2->nativeContains(*shape));
                        JS_ASSERT(entry->vcapTag() == 1);
                        JS_ASSERT(entry->kshape != entry->vshape());
                        JS_ASSERT(!shape->hasSlot());
                    }
#endif

                    PCMETER(cache->pchits++);
                    PCMETER(cache->setpchits++);
                    NATIVE_SET(cx, obj, shape, entry, strict, &rval);
                    break;
                }
            } else {
                JS_ASSERT(obj->isExtensible());

                if (obj->nativeEmpty()) {
                    if (!obj->ensureClassReservedSlotsForEmptyObject(cx))
                        THROW();
                }

                uint32 slot;
                if (shape->previous() == obj->lastProperty() &&
                    entry->vshape() == cx->runtime->protoHazardShape &&
                    shape->hasDefaultSetter()) {
                    slot = shape->slot;
                    JS_ASSERT(slot == obj->slotSpan());

                    /*
                     * Fast path: adding a plain old property that was once at
                     * the frontier of the property tree, whose slot is next to
                     * claim among the already-allocated slots in obj, where
                     * shape->table has not been created yet.
                     */
                    PCMETER(cache->pchits++);
                    PCMETER(cache->addpchits++);

                    if (slot < obj->numSlots()) {
                        JS_ASSERT(obj->getSlot(slot).isUndefined());
                    } else {
                        if (!obj->allocSlot(cx, &slot))
                            THROW();
                        JS_ASSERT(slot == shape->slot);
                    }

                    /* Simply extend obj's property tree path with shape! */
                    obj->extend(cx, shape);

                    /*
                     * No method change check here because here we are adding a
                     * new property, not updating an existing slot's value that
                     * might contain a method of a branded shape.
                     */
                    obj->setSlot(slot, rval);

                    /*
                     * Purge the property cache of the id we may have just
                     * shadowed in obj's scope and proto chains.
                     */
                    js_PurgeScopeChain(cx, obj, shape->id);
                    break;
                }
            }
            PCMETER(cache->setpcmisses++);

            atom = origAtom;
        } else {
            JS_ASSERT(atom);
        }

        jsid id = ATOM_TO_JSID(atom);
        if (entry && JS_LIKELY(!obj->getOps()->setProperty)) {
            uintN defineHow;
            JSOp op = JSOp(*f.regs.pc);
            if (op == JSOP_SETMETHOD)
                defineHow = JSDNP_CACHE_RESULT | JSDNP_SET_METHOD;
            else if (op == JSOP_SETNAME)
                defineHow = JSDNP_CACHE_RESULT | JSDNP_UNQUALIFIED;
            else
                defineHow = JSDNP_CACHE_RESULT;
            if (!js_SetPropertyHelper(cx, obj, id, defineHow, &rval, strict))
                THROW();
        } else {
            if (!obj->setProperty(cx, id, &rval, strict))
                THROW();
        }
    } while (0);

    f.regs.sp[-2] = f.regs.sp[-1];
}

template void JS_FASTCALL stubs::SetName<true>(VMFrame &f, JSAtom *origAtom);
template void JS_FASTCALL stubs::SetName<false>(VMFrame &f, JSAtom *origAtom);

template<JSBool strict>
void JS_FASTCALL
stubs::SetPropNoCache(VMFrame &f, JSAtom *atom)
{
    JSObject *obj = ValueToObject(f.cx, &f.regs.sp[-2]);
    if (!obj)
        THROW();
    Value rval = f.regs.sp[-1];
    if (!obj->setProperty(f.cx, ATOM_TO_JSID(atom), &f.regs.sp[-1], strict))
        THROW();
    f.regs.sp[-2] = rval;
}

template void JS_FASTCALL stubs::SetPropNoCache<true>(VMFrame &f, JSAtom *origAtom);
template void JS_FASTCALL stubs::SetPropNoCache<false>(VMFrame &f, JSAtom *origAtom);

template<JSBool strict>
void JS_FASTCALL
stubs::SetGlobalNameNoCache(VMFrame &f, JSAtom *atom)
{
    JSContext *cx = f.cx;

    Value rval = f.regs.sp[-1];
    Value &lref = f.regs.sp[-2];
    JSObject *obj = ValueToObject(cx, &lref);
    if (!obj)
        THROW();
    jsid id = ATOM_TO_JSID(atom);
    if (!obj->setProperty(cx, id, &rval, strict))
        THROW();

    f.regs.sp[-2] = f.regs.sp[-1];
}

template void JS_FASTCALL stubs::SetGlobalNameNoCache<true>(VMFrame &f, JSAtom *atom);
template void JS_FASTCALL stubs::SetGlobalNameNoCache<false>(VMFrame &f, JSAtom *atom);

template<JSBool strict>
void JS_FASTCALL
stubs::SetGlobalName(VMFrame &f, JSAtom *atom)
{
    SetName<strict>(f, atom);
}

template void JS_FASTCALL stubs::SetGlobalName<true>(VMFrame &f, JSAtom *atom);
template void JS_FASTCALL stubs::SetGlobalName<false>(VMFrame &f, JSAtom *atom);

static inline void
PushImplicitThis(VMFrame &f, JSObject *obj, Value &rval)
{
    Value thisv;

    if (!ComputeImplicitThis(f.cx, obj, rval, &thisv))
        return;
    *f.regs.sp++ = thisv;
}

static JSObject *
NameOp(VMFrame &f, JSObject *obj, bool callname = false)
{
    JSContext *cx = f.cx;

    const Shape *shape;
    Value rval;

    PropertyCacheEntry *entry;
    JSObject *obj2;
    JSAtom *atom;
    JS_PROPERTY_CACHE(cx).test(cx, f.regs.pc, obj, obj2, entry, atom);
    if (!atom) {
        if (entry->vword.isFunObj()) {
            rval.setObject(entry->vword.toFunObj());
        } else if (entry->vword.isSlot()) {
            uintN slot = entry->vword.toSlot();
            rval = obj2->nativeGetSlot(slot);
        } else {
            JS_ASSERT(entry->vword.isShape());
            shape = entry->vword.toShape();
            NATIVE_GET(cx, obj, obj2, shape, JSGET_METHOD_BARRIER, &rval, return NULL);
        }

        JS_ASSERT(obj->isGlobal() || IsCacheableNonGlobalScope(obj));
    } else {
        jsid id;
        id = ATOM_TO_JSID(atom);
        JSProperty *prop;
        if (!js_FindPropertyHelper(cx, id, true, &obj, &obj2, &prop))
            return NULL;
        if (!prop) {
            /* Kludge to allow (typeof foo == "undefined") tests. */
            JSOp op2 = js_GetOpcode(cx, f.fp()->script(), f.regs.pc + JSOP_NAME_LENGTH);
            if (op2 == JSOP_TYPEOF) {
                f.regs.sp++;
                f.regs.sp[-1].setUndefined();
                return obj;
            }
            ReportAtomNotDefined(cx, atom);
            return NULL;
        }

        /* Take the slow path if prop was not found in a native object. */
        if (!obj->isNative() || !obj2->isNative()) {
            if (!obj->getProperty(cx, id, &rval))
                return NULL;
        } else {
            shape = (Shape *)prop;
            JSObject *normalized = obj;
            if (normalized->getClass() == &js_WithClass && !shape->hasDefaultGetter())
                normalized = js_UnwrapWithObject(cx, normalized);
            NATIVE_GET(cx, normalized, obj2, shape, JSGET_METHOD_BARRIER, &rval, return NULL);
        }
    }

    *f.regs.sp++ = rval;

    if (callname)
        PushImplicitThis(f, obj, rval);

    return obj;
}

void JS_FASTCALL
stubs::Name(VMFrame &f)
{
    if (!NameOp(f, &f.fp()->scopeChain()))
        THROW();
}

void JS_FASTCALL
stubs::GetGlobalName(VMFrame &f)
{
    JSObject *globalObj = f.fp()->scopeChain().getGlobal();
    if (!NameOp(f, globalObj))
         THROW();
}

void JS_FASTCALL
stubs::GetElem(VMFrame &f)
{
    JSContext *cx = f.cx;
    JSFrameRegs &regs = f.regs;

    Value &lref = regs.sp[-2];
    Value &rref = regs.sp[-1];
    if (lref.isString() && rref.isInt32()) {
        JSString *str = lref.toString();
        int32_t i = rref.toInt32();
        if ((size_t)i < str->length()) {
            str = JSString::getUnitString(cx, str, (size_t)i);
            if (!str)
                THROW();
            f.regs.sp[-2].setString(str);
            return;
        }
    }

    JSObject *obj = ValueToObject(cx, &lref);
    if (!obj)
        THROW();

    const Value *copyFrom;
    Value rval;
    jsid id;
    if (rref.isInt32()) {
        int32_t i = rref.toInt32();
        if (obj->isDenseArray()) {
            jsuint idx = jsuint(i);

            if (idx < obj->getArrayLength() &&
                idx < obj->getDenseArrayCapacity()) {
                copyFrom = obj->addressOfDenseArrayElement(idx);
                if (!copyFrom->isMagic())
                    goto end_getelem;
            }
        } else if (obj->isArguments()) {
            uint32 arg = uint32(i);

            if (arg < obj->getArgsInitialLength()) {
                copyFrom = obj->addressOfArgsElement(arg);
                if (!copyFrom->isMagic()) {
                    if (JSStackFrame *afp = (JSStackFrame *) obj->getPrivate())
                        copyFrom = &afp->canonicalActualArg(arg);
                    goto end_getelem;
                }
            }
        }
        if (JS_LIKELY(INT_FITS_IN_JSID(i)))
            id = INT_TO_JSID(i);
        else
            goto intern_big_int;

    } else {
        int32_t i;
        if (ValueFitsInInt32(rref, &i) && INT_FITS_IN_JSID(i)) {
            id = INT_TO_JSID(i);
        } else {
          intern_big_int:
            if (!js_InternNonIntElementId(cx, obj, rref, &id))
                THROW();
        }
    }

    if (!obj->getProperty(cx, id, &rval))
        THROW();
    copyFrom = &rval;

  end_getelem:
    f.regs.sp[-2] = *copyFrom;
}

static inline bool
FetchElementId(VMFrame &f, JSObject *obj, const Value &idval, jsid &id, Value *vp)
{
    int32_t i_;
    if (ValueFitsInInt32(idval, &i_) && INT_FITS_IN_JSID(i_)) {
        id = INT_TO_JSID(i_);
        return true;
    }
    return !!js_InternNonIntElementId(f.cx, obj, idval, &id, vp);
}

void JS_FASTCALL
stubs::CallElem(VMFrame &f)
{
    JSContext *cx = f.cx;
    JSFrameRegs &regs = f.regs;

    /* Find the object on which to look for |this|'s properties. */
    Value thisv = regs.sp[-2];
    JSObject *thisObj = ValuePropertyBearer(cx, thisv, -2);
    if (!thisObj)
        THROW();

    /* Fetch index and convert it to id suitable for use with thisObj. */
    jsid id;
    if (!FetchElementId(f, thisObj, regs.sp[-1], id, &regs.sp[-2]))
        THROW();

    /* Get or set the element. */
    if (!js_GetMethod(cx, thisObj, id, JSGET_NO_METHOD_BARRIER, &regs.sp[-2]))
        THROW();

#if JS_HAS_NO_SUCH_METHOD
    if (JS_UNLIKELY(regs.sp[-2].isUndefined()) && thisv.isObject()) {
        regs.sp[-2] = regs.sp[-1];
        regs.sp[-1].setObject(*thisObj);
        if (!js_OnUnknownMethod(cx, regs.sp - 2))
            THROW();
    } else
#endif
    {
        regs.sp[-1] = thisv;
    }
}

template<JSBool strict>
void JS_FASTCALL
stubs::SetElem(VMFrame &f)
{
    JSContext *cx = f.cx;
    JSFrameRegs &regs = f.regs;

    Value &objval = regs.sp[-3];
    Value &idval  = regs.sp[-2];
    Value rval    = regs.sp[-1];

    JSObject *obj;
    jsid id;

    obj = ValueToObject(cx, &objval);
    if (!obj)
        THROW();

    if (!FetchElementId(f, obj, idval, id, &regs.sp[-2]))
        THROW();

    do {
        if (obj->isDenseArray() && JSID_IS_INT(id)) {
            jsuint length = obj->getDenseArrayCapacity();
            jsint i = JSID_TO_INT(id);
            if ((jsuint)i < length) {
                if (obj->getDenseArrayElement(i).isMagic(JS_ARRAY_HOLE)) {
                    if (js_PrototypeHasIndexedProperties(cx, obj))
                        break;
                    if ((jsuint)i >= obj->getArrayLength())
                        obj->setArrayLength(i + 1);
                }
                obj->setDenseArrayElement(i, rval);
                goto end_setelem;
            }
        }
    } while (0);
    if (!obj->setProperty(cx, id, &rval, strict))
        THROW();
  end_setelem:
    /* :FIXME: Moving the assigned object into the lowest stack slot
     * is a temporary hack. What we actually want is an implementation
     * of popAfterSet() that allows popping more than one value;
     * this logic can then be handled in Compiler.cpp. */
    regs.sp[-3] = regs.sp[-1];
}

template void JS_FASTCALL stubs::SetElem<true>(VMFrame &f);
template void JS_FASTCALL stubs::SetElem<false>(VMFrame &f);

void JS_FASTCALL
stubs::CallName(VMFrame &f)
{
    JSObject *obj = NameOp(f, &f.fp()->scopeChain(), true);
    if (!obj)
        THROW();
}

/*
 * Push the implicit this value, with the assumption that the callee
 * (which is on top of the stack) was read as a property from the
 * global object.
 */
void JS_FASTCALL
stubs::PushImplicitThisForGlobal(VMFrame &f)
{
    return PushImplicitThis(f, f.fp()->scopeChain().getGlobal(), f.regs.sp[-1]);
}

void JS_FASTCALL
stubs::BitOr(VMFrame &f)
{
    int32_t i, j;

    if (!ValueToECMAInt32(f.cx, f.regs.sp[-2], &i) ||
        !ValueToECMAInt32(f.cx, f.regs.sp[-1], &j)) {
        THROW();
    }
    i = i | j;
    f.regs.sp[-2].setInt32(i);
}

void JS_FASTCALL
stubs::BitXor(VMFrame &f)
{
    int32_t i, j;

    if (!ValueToECMAInt32(f.cx, f.regs.sp[-2], &i) ||
        !ValueToECMAInt32(f.cx, f.regs.sp[-1], &j)) {
        THROW();
    }
    i = i ^ j;
    f.regs.sp[-2].setInt32(i);
}

void JS_FASTCALL
stubs::BitAnd(VMFrame &f)
{
    int32_t i, j;

    if (!ValueToECMAInt32(f.cx, f.regs.sp[-2], &i) ||
        !ValueToECMAInt32(f.cx, f.regs.sp[-1], &j)) {
        THROW();
    }
    i = i & j;
    f.regs.sp[-2].setInt32(i);
}

void JS_FASTCALL
stubs::BitNot(VMFrame &f)
{
    int32_t i;

    if (!ValueToECMAInt32(f.cx, f.regs.sp[-1], &i))
        THROW();
    i = ~i;
    f.regs.sp[-1].setInt32(i);
}

void JS_FASTCALL
stubs::Lsh(VMFrame &f)
{
    int32_t i, j;
    if (!ValueToECMAInt32(f.cx, f.regs.sp[-2], &i))
        THROW();
    if (!ValueToECMAInt32(f.cx, f.regs.sp[-1], &j))
        THROW();
    i = i << (j & 31);
    f.regs.sp[-2].setInt32(i);
}

void JS_FASTCALL
stubs::Rsh(VMFrame &f)
{
    int32_t i, j;
    if (!ValueToECMAInt32(f.cx, f.regs.sp[-2], &i))
        THROW();
    if (!ValueToECMAInt32(f.cx, f.regs.sp[-1], &j))
        THROW();
    i = i >> (j & 31);
    f.regs.sp[-2].setInt32(i);
}

void JS_FASTCALL
stubs::Ursh(VMFrame &f)
{
    uint32_t u;
    if (!ValueToECMAUint32(f.cx, f.regs.sp[-2], &u))
        THROW();
    int32_t j;
    if (!ValueToECMAInt32(f.cx, f.regs.sp[-1], &j))
        THROW();

    u >>= (j & 31);

	f.regs.sp[-2].setNumber(uint32(u));
}

template<JSBool strict>
void JS_FASTCALL
stubs::DefFun(VMFrame &f, JSFunction *fun)
{
    JSObject *obj2;

    JSContext *cx = f.cx;
    JSStackFrame *fp = f.fp();

    /*
     * A top-level function defined in Global or Eval code (see ECMA-262
     * Ed. 3), or else a SpiderMonkey extension: a named function statement in
     * a compound statement (not at the top statement level of global code, or
     * at the top level of a function body).
     */
    JSObject *obj = FUN_OBJECT(fun);

    if (FUN_NULL_CLOSURE(fun)) {
        /*
         * Even a null closure needs a parent for principals finding.
         * FIXME: bug 476950, although debugger users may also demand some kind
         * of scope link for debugger-assisted eval-in-frame.
         */
        obj2 = &fp->scopeChain();
    } else {
        JS_ASSERT(!fun->isFlatClosure());

        obj2 = GetScopeChainFast(cx, fp, JSOP_DEFFUN, JSOP_DEFFUN_LENGTH);
        if (!obj2)
            THROW();
    }

    /*
     * If static link is not current scope, clone fun's object to link to the
     * current scope via parent. We do this to enable sharing of compiled
     * functions among multiple equivalent scopes, amortizing the cost of
     * compilation over a number of executions.  Examples include XUL scripts
     * and event handlers shared among Firefox or other Mozilla app chrome
     * windows, and user-defined JS functions precompiled and then shared among
     * requests in server-side JS.
     */
    if (obj->getParent() != obj2) {
        obj = CloneFunctionObject(cx, fun, obj2);
        if (!obj)
            THROW();
    }

    /*
     * ECMA requires functions defined when entering Eval code to be
     * impermanent.
     */
    uintN attrs = fp->isEvalFrame()
                  ? JSPROP_ENUMERATE
                  : JSPROP_ENUMERATE | JSPROP_PERMANENT;

    /*
     * We define the function as a property of the variable object and not the
     * current scope chain even for the case of function expression statements
     * and functions defined by eval inside let or with blocks.
     */
    JSObject *parent = &fp->varobj(cx);

    /* ES5 10.5 (NB: with subsequent errata). */
    jsid id = ATOM_TO_JSID(fun->atom);
    JSProperty *prop = NULL;
    JSObject *pobj;
    if (!parent->lookupProperty(cx, id, &pobj, &prop))
        THROW();

    Value rval = ObjectValue(*obj);

    do {
        /* Steps 5d, 5f. */
        if (!prop || pobj != parent) {
            if (!parent->defineProperty(cx, id, rval, PropertyStub, StrictPropertyStub, attrs))
                THROW();
            break;
        }

        /* Step 5e. */
        JS_ASSERT(parent->isNative());
        Shape *shape = reinterpret_cast<Shape *>(prop);
        if (parent->isGlobal()) {
            if (shape->configurable()) {
                if (!parent->defineProperty(cx, id, rval, PropertyStub, StrictPropertyStub, attrs))
                    THROW();
                break;
            }

            if (shape->isAccessorDescriptor() || !shape->writable() || !shape->enumerable()) {
                JSAutoByteString bytes;
                if (const char *name = js_ValueToPrintable(cx, IdToValue(id), &bytes)) {
                    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                         JSMSG_CANT_REDEFINE_PROP, name);
                }
                THROW();
            }
        }

        /*
         * Non-global properties, and global properties which we aren't simply
         * redefining, must be set.  First, this preserves their attributes.
         * Second, this will produce warnings and/or errors as necessary if the
         * specified Call object property is not writable (const).
         */

        /* Step 5f. */
        if (!parent->setProperty(cx, id, &rval, strict))
            THROW();
    } while (false);
}

template void JS_FASTCALL stubs::DefFun<true>(VMFrame &f, JSFunction *fun);
template void JS_FASTCALL stubs::DefFun<false>(VMFrame &f, JSFunction *fun);

#define DEFAULT_VALUE(cx, n, hint, v)                                         \
    JS_BEGIN_MACRO                                                            \
        JS_ASSERT(v.isObject());                                              \
        JS_ASSERT(v == regs.sp[n]);                                           \
        if (!DefaultValue(cx, &v.toObject(), hint, &regs.sp[n]))              \
            THROWV(JS_FALSE);                                                 \
        v = regs.sp[n];                                                       \
    JS_END_MACRO

#define RELATIONAL(OP)                                                        \
    JS_BEGIN_MACRO                                                            \
        JSContext *cx = f.cx;                                                 \
        JSFrameRegs &regs = f.regs;                                           \
        Value rval = regs.sp[-1];                                             \
        Value lval = regs.sp[-2];                                             \
        bool cond;                                                            \
        if (lval.isObject())                                                  \
            DEFAULT_VALUE(cx, -2, JSTYPE_NUMBER, lval);                       \
        if (rval.isObject())                                                  \
            DEFAULT_VALUE(cx, -1, JSTYPE_NUMBER, rval);                       \
        if (lval.isString() && rval.isString()) {                             \
            JSString *l = lval.toString(), *r = rval.toString();              \
            JSBool cmp;                                                       \
            if (!CompareStrings(cx, l, r, &cmp))                              \
                THROWV(JS_FALSE);                                             \
            cond = cmp OP 0;                                                  \
        } else {                                                              \
            double l, r;                                                      \
            if (!ValueToNumber(cx, lval, &l) ||                               \
                !ValueToNumber(cx, rval, &r)) {                               \
                THROWV(JS_FALSE);                                             \
            }                                                                 \
            cond = JSDOUBLE_COMPARE(l, OP, r, false);                         \
        }                                                                     \
        regs.sp[-2].setBoolean(cond);                                         \
        return cond;                                                          \
    JS_END_MACRO

JSBool JS_FASTCALL
stubs::LessThan(VMFrame &f)
{
    RELATIONAL(<);
}

JSBool JS_FASTCALL
stubs::LessEqual(VMFrame &f)
{
    RELATIONAL(<=);
}

JSBool JS_FASTCALL
stubs::GreaterThan(VMFrame &f)
{
    RELATIONAL(>);
}

JSBool JS_FASTCALL
stubs::GreaterEqual(VMFrame &f)
{
    RELATIONAL(>=);
}

JSBool JS_FASTCALL
stubs::ValueToBoolean(VMFrame &f)
{
    return js_ValueToBoolean(f.regs.sp[-1]);
}

void JS_FASTCALL
stubs::Not(VMFrame &f)
{
    JSBool b = !js_ValueToBoolean(f.regs.sp[-1]);
    f.regs.sp[-1].setBoolean(b);
}

template <JSBool EQ, bool IFNAN>
static inline bool
StubEqualityOp(VMFrame &f)
{
    JSContext *cx = f.cx;
    JSFrameRegs &regs = f.regs;

    Value rval = regs.sp[-1];
    Value lval = regs.sp[-2];

    JSBool cond;

    /* The string==string case is easily the hottest;  try it first. */
    if (lval.isString() && rval.isString()) {
        JSString *l = lval.toString();
        JSString *r = rval.toString();
        JSBool equal;
        if (!EqualStrings(cx, l, r, &equal))
            return false;
        cond = equal == EQ;
    } else
#if JS_HAS_XML_SUPPORT
    if ((lval.isObject() && lval.toObject().isXML()) ||
        (rval.isObject() && rval.toObject().isXML())) {
        if (!js_TestXMLEquality(cx, lval, rval, &cond))
            return false;
        cond = cond == EQ;
    } else
#endif

    if (SameType(lval, rval)) {
        JS_ASSERT(!lval.isString());    /* this case is handled above */
        if (lval.isDouble()) {
            double l = lval.toDouble();
            double r = rval.toDouble();
            if (EQ)
                cond = JSDOUBLE_COMPARE(l, ==, r, IFNAN);
            else
                cond = JSDOUBLE_COMPARE(l, !=, r, IFNAN);
        } else if (lval.isObject()) {
            JSObject *l = &lval.toObject(), *r = &rval.toObject();
            l->assertSpecialEqualitySynced();
            if (EqualityOp eq = l->getClass()->ext.equality) {
                if (!eq(cx, l, &rval, &cond))
                    return false;
                cond = cond == EQ;
            } else {
                cond = (l == r) == EQ;
            }
        } else if (lval.isNullOrUndefined()) {
            cond = EQ;
        } else {
            cond = (lval.payloadAsRawUint32() == rval.payloadAsRawUint32()) == EQ;
        }
    } else {
        if (lval.isNullOrUndefined()) {
            cond = rval.isNullOrUndefined() == EQ;
        } else if (rval.isNullOrUndefined()) {
            cond = !EQ;
        } else {
            if (lval.isObject()) {
                if (!DefaultValue(cx, &lval.toObject(), JSTYPE_VOID, &regs.sp[-2]))
                    return false;
                lval = regs.sp[-2];
            }

            if (rval.isObject()) {
                if (!DefaultValue(cx, &rval.toObject(), JSTYPE_VOID, &regs.sp[-1]))
                    return false;
                rval = regs.sp[-1];
            }

            /*
             * The string==string case is repeated because DefaultValue() can
             * convert lval/rval to strings.
             */
            if (lval.isString() && rval.isString()) {
                JSString *l = lval.toString();
                JSString *r = rval.toString();
                JSBool equal;
                if (!EqualStrings(cx, l, r, &equal))
                    return false;
                cond = equal == EQ;
            } else {
                double l, r;
                if (!ValueToNumber(cx, lval, &l) ||
                    !ValueToNumber(cx, rval, &r)) {
                    return false;
                }

                if (EQ)
                    cond = JSDOUBLE_COMPARE(l, ==, r, false);
                else
                    cond = JSDOUBLE_COMPARE(l, !=, r, true);
            }
        }
    }

    regs.sp[-2].setBoolean(cond);
    return true;
}

JSBool JS_FASTCALL
stubs::Equal(VMFrame &f)
{
    if (!StubEqualityOp<JS_TRUE, false>(f))
        THROWV(JS_FALSE);
    return f.regs.sp[-2].toBoolean();
}

JSBool JS_FASTCALL
stubs::NotEqual(VMFrame &f)
{
    if (!StubEqualityOp<JS_FALSE, true>(f))
        THROWV(JS_FALSE);
    return f.regs.sp[-2].toBoolean();
}

static inline bool
DefaultValue(VMFrame &f, JSType hint, Value &v, int n)
{
    JS_ASSERT(v.isObject());
    if (!DefaultValue(f.cx, &v.toObject(), hint, &f.regs.sp[n]))
        return false;
    v = f.regs.sp[n];
    return true;
}

void JS_FASTCALL
stubs::Add(VMFrame &f)
{
    JSContext *cx = f.cx;
    JSFrameRegs &regs = f.regs;
    Value rval = regs.sp[-1];
    Value lval = regs.sp[-2];

    /* The string + string case is easily the hottest;  try it first. */
    bool lIsString = lval.isString();
    bool rIsString = rval.isString();
    JSString *lstr, *rstr;
    if (lIsString && rIsString) {
        lstr = lval.toString();
        rstr = rval.toString();
        goto string_concat;

    } else
#if JS_HAS_XML_SUPPORT
    if (lval.isObject() && lval.toObject().isXML() &&
        rval.isObject() && rval.toObject().isXML()) {
        if (!js_ConcatenateXML(cx, &lval.toObject(), &rval.toObject(), &rval))
            THROW();
        regs.sp--;
        regs.sp[-1] = rval;
    } else
#endif
    {
        /* These can convert lval/rval to strings. */
        if (lval.isObject() && !DefaultValue(f, JSTYPE_VOID, lval, -2))
            THROW();
        if (rval.isObject() && !DefaultValue(f, JSTYPE_VOID, rval, -1))
            THROW();
        if ((lIsString = lval.isString()) || (rIsString = rval.isString())) {
            if (lIsString) {
                lstr = lval.toString();
            } else {
                lstr = js_ValueToString(cx, lval);
                if (!lstr)
                    THROW();
                regs.sp[-2].setString(lstr);
            }
            if (rIsString) {
                rstr = rval.toString();
            } else {
                rstr = js_ValueToString(cx, rval);
                if (!rstr)
                    THROW();
                regs.sp[-1].setString(rstr);
            }
            goto string_concat;

        } else {
            double l, r;
            if (!ValueToNumber(cx, lval, &l) || !ValueToNumber(cx, rval, &r))
                THROW();
            l += r;
            regs.sp--;
            regs.sp[-1].setNumber(l);
        }
    }
    return;

  string_concat:
    JSString *str = js_ConcatStrings(cx, lstr, rstr);
    if (!str)
        THROW();
    regs.sp--;
    regs.sp[-1].setString(str);
}


void JS_FASTCALL
stubs::Sub(VMFrame &f)
{
    JSContext *cx = f.cx;
    JSFrameRegs &regs = f.regs;
    double d1, d2;
    if (!ValueToNumber(cx, regs.sp[-2], &d1) ||
        !ValueToNumber(cx, regs.sp[-1], &d2)) {
        THROW();
    }
    double d = d1 - d2;
    regs.sp[-2].setNumber(d);
}

void JS_FASTCALL
stubs::Mul(VMFrame &f)
{
    JSContext *cx = f.cx;
    JSFrameRegs &regs = f.regs;
    double d1, d2;
    if (!ValueToNumber(cx, regs.sp[-2], &d1) ||
        !ValueToNumber(cx, regs.sp[-1], &d2)) {
        THROW();
    }
    double d = d1 * d2;
    regs.sp[-2].setNumber(d);
}

void JS_FASTCALL
stubs::Div(VMFrame &f)
{
    JSContext *cx = f.cx;
    JSRuntime *rt = cx->runtime;
    JSFrameRegs &regs = f.regs;

    double d1, d2;
    if (!ValueToNumber(cx, regs.sp[-2], &d1) ||
        !ValueToNumber(cx, regs.sp[-1], &d2)) {
        THROW();
    }
    if (d2 == 0) {
        const Value *vp;
#ifdef XP_WIN
        /* XXX MSVC miscompiles such that (NaN == 0) */
        if (JSDOUBLE_IS_NaN(d2))
            vp = &rt->NaNValue;
        else
#endif
        if (d1 == 0 || JSDOUBLE_IS_NaN(d1))
            vp = &rt->NaNValue;
        else if (JSDOUBLE_IS_NEG(d1) != JSDOUBLE_IS_NEG(d2))
            vp = &rt->negativeInfinityValue;
        else
            vp = &rt->positiveInfinityValue;
        regs.sp[-2] = *vp;
    } else {
        d1 /= d2;
        regs.sp[-2].setNumber(d1);
    }
}

void JS_FASTCALL
stubs::Mod(VMFrame &f)
{
    JSContext *cx = f.cx;
    JSFrameRegs &regs = f.regs;

    Value &lref = regs.sp[-2];
    Value &rref = regs.sp[-1];
    int32_t l, r;
    if (lref.isInt32() && rref.isInt32() &&
        (l = lref.toInt32()) >= 0 && (r = rref.toInt32()) > 0) {
        int32_t mod = l % r;
        regs.sp[-2].setInt32(mod);
    } else {
        double d1, d2;
        if (!ValueToNumber(cx, regs.sp[-2], &d1) ||
            !ValueToNumber(cx, regs.sp[-1], &d2)) {
            THROW();
        }
        if (d2 == 0) {
            regs.sp[-2].setDouble(js_NaN);
        } else {
            d1 = js_fmod(d1, d2);
            regs.sp[-2].setDouble(d1);
        }
    }
}

void JS_FASTCALL
stubs::Debugger(VMFrame &f, jsbytecode *pc)
{
    JSDebuggerHandler handler = f.cx->debugHooks->debuggerHandler;
    if (handler) {
        Value rval;
        switch (handler(f.cx, f.cx->fp()->script(), pc, Jsvalify(&rval),
                        f.cx->debugHooks->debuggerHandlerData)) {
          case JSTRAP_THROW:
            f.cx->setPendingException(rval);
            THROW();

          case JSTRAP_RETURN:
            f.cx->clearPendingException();
            f.cx->fp()->setReturnValue(rval);
#if (defined(JS_NO_FASTCALL) && defined(JS_CPU_X86)) || defined(_WIN64)
            *f.returnAddressLocation() = JS_FUNC_TO_DATA_PTR(void *,
                                         f.cx->jaegerCompartment()->forceReturnFastTrampoline());
#else
            *f.returnAddressLocation() = JS_FUNC_TO_DATA_PTR(void *,
                                         f.cx->jaegerCompartment()->forceReturnTrampoline());
#endif
            break;

          case JSTRAP_ERROR:
            f.cx->clearPendingException();
            THROW();

          default:
            break;
        }
    }
}

void JS_FASTCALL
stubs::Interrupt(VMFrame &f, jsbytecode *pc)
{
    if (!js_HandleExecutionInterrupt(f.cx))
        THROW();
}

void JS_FASTCALL
stubs::Trap(VMFrame &f, uint32 trapTypes)
{
    Value rval;
    jsbytecode *pc = f.cx->regs->pc;

    /*
     * Trap may be called for a single-step interrupt trap and/or a
     * regular trap. Try the single-step first, and if it lets control
     * flow through or does not exist, do the regular trap.
     */
    JSTrapStatus result = JSTRAP_CONTINUE;
    if (trapTypes & JSTRAP_SINGLESTEP) {
        /*
         * single step mode may be paused without recompiling by
         * setting the interruptHook to NULL.
         */
        JSInterruptHook hook = f.cx->debugHooks->interruptHook;
        if (hook)
            result = hook(f.cx, f.cx->fp()->script(), pc, Jsvalify(&rval),
                          f.cx->debugHooks->interruptHookData);
    }

    if (result == JSTRAP_CONTINUE && (trapTypes & JSTRAP_TRAP))
        result = JS_HandleTrap(f.cx, f.cx->fp()->script(), pc, Jsvalify(&rval));

    switch (result) {
      case JSTRAP_THROW:
        f.cx->setPendingException(rval);
        THROW();

      case JSTRAP_RETURN:
        f.cx->clearPendingException();
        f.cx->fp()->setReturnValue(rval);
#if (defined(JS_NO_FASTCALL) && defined(JS_CPU_X86)) || defined(_WIN64)
        *f.returnAddressLocation() = JS_FUNC_TO_DATA_PTR(void *,
                                     f.cx->jaegerCompartment()->forceReturnFastTrampoline());
#else
        *f.returnAddressLocation() = JS_FUNC_TO_DATA_PTR(void *,
                                     f.cx->jaegerCompartment()->forceReturnTrampoline());
#endif
        break;

      case JSTRAP_ERROR:
        f.cx->clearPendingException();
        THROW();

      default:
        break;
    }
}

void JS_FASTCALL
stubs::This(VMFrame &f)
{
    if (!f.fp()->computeThis(f.cx))
        THROW();
    f.regs.sp[-1] = f.fp()->thisValue();
}

void JS_FASTCALL
stubs::Neg(VMFrame &f)
{
    double d;
    if (!ValueToNumber(f.cx, f.regs.sp[-1], &d))
        THROW();
    d = -d;
    f.regs.sp[-1].setNumber(d);
}

JSObject * JS_FASTCALL
stubs::NewInitArray(VMFrame &f, uint32 count)
{
    JSObject *obj = NewDenseAllocatedArray(f.cx, count);
    if (!obj)
        THROWV(NULL);

    return obj;
}

JSObject * JS_FASTCALL
stubs::NewInitObject(VMFrame &f, JSObject *baseobj)
{
    JSContext *cx = f.cx;

    if (!baseobj) {
        gc::FinalizeKind kind = GuessObjectGCKind(0, false);
        JSObject *obj = NewBuiltinClassInstance(cx, &js_ObjectClass, kind);
        if (!obj)
            THROWV(NULL);
        return obj;
    }

    JSObject *obj = CopyInitializerObject(cx, baseobj);

    if (!obj)
        THROWV(NULL);
    return obj;
}

void JS_FASTCALL
stubs::InitElem(VMFrame &f, uint32 last)
{
    JSContext *cx = f.cx;
    JSFrameRegs &regs = f.regs;

    /* Pop the element's value into rval. */
    JS_ASSERT(regs.sp - f.fp()->base() >= 3);
    const Value &rref = regs.sp[-1];

    /* Find the object being initialized at top of stack. */
    const Value &lref = regs.sp[-3];
    JS_ASSERT(lref.isObject());
    JSObject *obj = &lref.toObject();

    /* Fetch id now that we have obj. */
    jsid id;
    const Value &idval = regs.sp[-2];
    if (!FetchElementId(f, obj, idval, id, &regs.sp[-2]))
        THROW();

    /*
     * If rref is a hole, do not call JSObject::defineProperty. In this case,
     * obj must be an array, so if the current op is the last element
     * initialiser, set the array length to one greater than id.
     */
    if (rref.isMagic(JS_ARRAY_HOLE)) {
        JS_ASSERT(obj->isArray());
        JS_ASSERT(JSID_IS_INT(id));
        JS_ASSERT(jsuint(JSID_TO_INT(id)) < JS_ARGS_LENGTH_MAX);
        if (last && !js_SetLengthProperty(cx, obj, (jsuint) (JSID_TO_INT(id) + 1)))
            THROW();
    } else {
        if (!obj->defineProperty(cx, id, rref, NULL, NULL, JSPROP_ENUMERATE))
            THROW();
    }
}

void JS_FASTCALL
stubs::GetUpvar(VMFrame &f, uint32 ck)
{
    /* :FIXME: We can do better, this stub isn't needed. */
    uint32 staticLevel = f.fp()->script()->staticLevel;
    UpvarCookie cookie;
    cookie.fromInteger(ck);
    f.regs.sp[0] = GetUpvar(f.cx, staticLevel, cookie);
}

JSObject * JS_FASTCALL
stubs::DefLocalFun(VMFrame &f, JSFunction *fun)
{
    /*
     * Define a local function (i.e., one nested at the top level of another
     * function), parented by the current scope chain, stored in a local
     * variable slot that the compiler allocated.  This is an optimization over
     * JSOP_DEFFUN that avoids requiring a call object for the outer function's
     * activation.
     */
    JS_ASSERT(fun->isInterpreted());
    JS_ASSERT(!FUN_FLAT_CLOSURE(fun));
    JSObject *obj = FUN_OBJECT(fun);

    if (FUN_NULL_CLOSURE(fun)) {
        obj = CloneFunctionObject(f.cx, fun, &f.fp()->scopeChain());
        if (!obj)
            THROWV(NULL);
    } else {
        JSObject *parent = GetScopeChainFast(f.cx, f.fp(), JSOP_DEFLOCALFUN,
                                             JSOP_DEFLOCALFUN_LENGTH);
        if (!parent)
            THROWV(NULL);

        if (obj->getParent() != parent) {
            obj = CloneFunctionObject(f.cx, fun, parent);
            if (!obj)
                THROWV(NULL);
        }
    }

    return obj;
}

JSObject * JS_FASTCALL
stubs::DefLocalFun_FC(VMFrame &f, JSFunction *fun)
{
    JSObject *obj = js_NewFlatClosure(f.cx, fun, JSOP_DEFLOCALFUN_FC, JSOP_DEFLOCALFUN_FC_LENGTH);
    if (!obj)
        THROWV(NULL);
    return obj;
}

JSObject * JS_FASTCALL
stubs::RegExp(VMFrame &f, JSObject *regex)
{
    /*
     * Push a regexp object cloned from the regexp literal object mapped by the
     * bytecode at pc. ES5 finally fixed this bad old ES3 design flaw which was
     * flouted by many browser-based implementations.
     *
     * We avoid the GetScopeChain call here and pass fp->scopeChain() as
     * js_GetClassPrototype uses the latter only to locate the global.
     */
    JSObject *proto;
    if (!js_GetClassPrototype(f.cx, &f.fp()->scopeChain(), JSProto_RegExp, &proto))
        THROWV(NULL);
    JS_ASSERT(proto);
    JSObject *obj = js_CloneRegExpObject(f.cx, regex, proto);
    if (!obj)
        THROWV(NULL);
    return obj;
}

JSObject * JS_FASTCALL
stubs::LambdaForInit(VMFrame &f, JSFunction *fun)
{
    JSObject *obj = FUN_OBJECT(fun);
    if (FUN_NULL_CLOSURE(fun) && obj->getParent() == &f.fp()->scopeChain()) {
        fun->setMethodAtom(f.fp()->script()->getAtom(GET_SLOTNO(f.regs.pc)));
        return obj;
    }
    return Lambda(f, fun);
}

JSObject * JS_FASTCALL
stubs::LambdaForSet(VMFrame &f, JSFunction *fun)
{
    JSObject *obj = FUN_OBJECT(fun);
    if (FUN_NULL_CLOSURE(fun) && obj->getParent() == &f.fp()->scopeChain()) {
        const Value &lref = f.regs.sp[-1];
        if (lref.isObject() && lref.toObject().canHaveMethodBarrier()) {
            fun->setMethodAtom(f.fp()->script()->getAtom(GET_SLOTNO(f.regs.pc)));
            return obj;
        }
    }
    return Lambda(f, fun);
}

JSObject * JS_FASTCALL
stubs::LambdaJoinableForCall(VMFrame &f, JSFunction *fun)
{
    JSObject *obj = FUN_OBJECT(fun);
    if (FUN_NULL_CLOSURE(fun) && obj->getParent() == &f.fp()->scopeChain()) {
        /*
         * Array.prototype.sort and String.prototype.replace are
         * optimized as if they are special form. We know that they
         * won't leak the joined function object in obj, therefore
         * we don't need to clone that compiler- created function
         * object for identity/mutation reasons.
         */
        int iargc = GET_ARGC(f.regs.pc);

        /*
         * Note that we have not yet pushed obj as the final argument,
         * so regs.sp[1 - (iargc + 2)], and not regs.sp[-(iargc + 2)],
         * is the callee for this JSOP_CALL.
         */
        const Value &cref = f.regs.sp[1 - (iargc + 2)];
        JSObject *callee;

        if (IsFunctionObject(cref, &callee)) {
            JSFunction *calleeFun = callee->getFunctionPrivate();
            Native native = calleeFun->maybeNative();

            if (native) {
                if (iargc == 1 && native == array_sort)
                    return obj;
                if (iargc == 2 && native == str_replace)
                    return obj;
            }
        }
    }
    return Lambda(f, fun);
}

JSObject * JS_FASTCALL
stubs::LambdaJoinableForNull(VMFrame &f, JSFunction *fun)
{
    JSObject *obj = FUN_OBJECT(fun);
    if (FUN_NULL_CLOSURE(fun) && obj->getParent() == &f.fp()->scopeChain()) {
        jsbytecode *pc2 = f.regs.pc + JSOP_NULL_LENGTH;
        JSOp op2 = JSOp(*pc2);

        if (op2 == JSOP_CALL && GET_ARGC(pc2) == 0)
            return obj;
    }
    return Lambda(f, fun);
}

JSObject * JS_FASTCALL
stubs::Lambda(VMFrame &f, JSFunction *fun)
{
    JSObject *obj = FUN_OBJECT(fun);

    JSObject *parent;
    if (FUN_NULL_CLOSURE(fun)) {
        parent = &f.fp()->scopeChain();
    } else {
        parent = GetScopeChainFast(f.cx, f.fp(), JSOP_LAMBDA, JSOP_LAMBDA_LENGTH);
        if (!parent)
            THROWV(NULL);
    }

    obj = CloneFunctionObject(f.cx, fun, parent);
    if (!obj)
        THROWV(NULL);

    return obj;
}

/* Test whether v is an int in the range [-2^31 + 1, 2^31 - 2] */
static JS_ALWAYS_INLINE bool
CanIncDecWithoutOverflow(int32_t i)
{
    return (i > JSVAL_INT_MIN) && (i < JSVAL_INT_MAX);
}

template <int32 N, bool POST, JSBool strict>
static inline bool
ObjIncOp(VMFrame &f, JSObject *obj, jsid id)
{
    JSContext *cx = f.cx;
    JSStackFrame *fp = f.fp();

    f.regs.sp[0].setNull();
    f.regs.sp++;
    if (!obj->getProperty(cx, id, &f.regs.sp[-1]))
        return false;

    Value &ref = f.regs.sp[-1];
    int32_t tmp;
    if (JS_LIKELY(ref.isInt32() && CanIncDecWithoutOverflow(tmp = ref.toInt32()))) {
        if (POST)
            ref.getInt32Ref() = tmp + N;
        else
            ref.getInt32Ref() = tmp += N;
        fp->setAssigning();
        JSBool ok = obj->setProperty(cx, id, &ref, strict);
        fp->clearAssigning();
        if (!ok)
            return false;

        /*
         * We must set regs.sp[-1] to tmp for both post and pre increments
         * as the setter overwrites regs.sp[-1].
         */
        ref.setInt32(tmp);
    } else {
        Value v;
        double d;
        if (!ValueToNumber(cx, ref, &d))
            return false;
        if (POST) {
            ref.setDouble(d);
            d += N;
        } else {
            d += N;
            ref.setDouble(d);
        }
        v.setDouble(d);
        fp->setAssigning();
        JSBool ok = obj->setProperty(cx, id, &v, strict);
        fp->clearAssigning();
        if (!ok)
            return false;
    }

    return true;
}

template <int32 N, bool POST, JSBool strict>
static inline bool
NameIncDec(VMFrame &f, JSObject *obj, JSAtom *origAtom)
{
    JSContext *cx = f.cx;

    JSAtom *atom;
    JSObject *obj2;
    JSProperty *prop;
    PropertyCacheEntry *entry;
    JS_PROPERTY_CACHE(cx).test(cx, f.regs.pc, obj, obj2, entry, atom);
    if (!atom) {
        if (obj == obj2 && entry->vword.isSlot()) {
            uint32 slot = entry->vword.toSlot();
            Value &rref = obj->nativeGetSlotRef(slot);
            int32_t tmp;
            if (JS_LIKELY(rref.isInt32() && CanIncDecWithoutOverflow(tmp = rref.toInt32()))) {
                int32_t inc = tmp + N;
                if (!POST)
                    tmp = inc;
                rref.getInt32Ref() = inc;
                f.regs.sp[0].setInt32(tmp);
                return true;
            }
        }
        atom = origAtom;
    }

    jsid id = ATOM_TO_JSID(atom);
    if (!js_FindPropertyHelper(cx, id, true, &obj, &obj2, &prop))
        return false;
    if (!prop) {
        ReportAtomNotDefined(cx, atom);
        return false;
    }
    return ObjIncOp<N, POST, strict>(f, obj, id);
}

template<JSBool strict>
void JS_FASTCALL
stubs::PropInc(VMFrame &f, JSAtom *atom)
{
    JSObject *obj = ValueToObject(f.cx, &f.regs.sp[-1]);
    if (!obj)
        THROW();
    if (!ObjIncOp<1, true, strict>(f, obj, ATOM_TO_JSID(atom)))
        THROW();
    f.regs.sp[-2] = f.regs.sp[-1];
}

template void JS_FASTCALL stubs::PropInc<true>(VMFrame &f, JSAtom *atom);
template void JS_FASTCALL stubs::PropInc<false>(VMFrame &f, JSAtom *atom);

template<JSBool strict>
void JS_FASTCALL
stubs::PropDec(VMFrame &f, JSAtom *atom)
{
    JSObject *obj = ValueToObject(f.cx, &f.regs.sp[-1]);
    if (!obj)
        THROW();
    if (!ObjIncOp<-1, true, strict>(f, obj, ATOM_TO_JSID(atom)))
        THROW();
    f.regs.sp[-2] = f.regs.sp[-1];
}

template void JS_FASTCALL stubs::PropDec<true>(VMFrame &f, JSAtom *atom);
template void JS_FASTCALL stubs::PropDec<false>(VMFrame &f, JSAtom *atom);

template<JSBool strict>
void JS_FASTCALL
stubs::IncProp(VMFrame &f, JSAtom *atom)
{
    JSObject *obj = ValueToObject(f.cx, &f.regs.sp[-1]);
    if (!obj)
        THROW();
    if (!ObjIncOp<1, false, strict>(f, obj, ATOM_TO_JSID(atom)))
        THROW();
    f.regs.sp[-2] = f.regs.sp[-1];
}

template void JS_FASTCALL stubs::IncProp<true>(VMFrame &f, JSAtom *atom);
template void JS_FASTCALL stubs::IncProp<false>(VMFrame &f, JSAtom *atom);

template<JSBool strict>
void JS_FASTCALL
stubs::DecProp(VMFrame &f, JSAtom *atom)
{
    JSObject *obj = ValueToObject(f.cx, &f.regs.sp[-1]);
    if (!obj)
        THROW();
    if (!ObjIncOp<-1, false, strict>(f, obj, ATOM_TO_JSID(atom)))
        THROW();
    f.regs.sp[-2] = f.regs.sp[-1];
}

template void JS_FASTCALL stubs::DecProp<true>(VMFrame &f, JSAtom *atom);
template void JS_FASTCALL stubs::DecProp<false>(VMFrame &f, JSAtom *atom);

template<JSBool strict>
void JS_FASTCALL
stubs::ElemInc(VMFrame &f)
{
    JSObject *obj = ValueToObject(f.cx, &f.regs.sp[-2]);
    if (!obj)
        THROW();
    jsid id;
    if (!FetchElementId(f, obj, f.regs.sp[-1], id, &f.regs.sp[-1]))
        THROW();
    if (!ObjIncOp<1, true, strict>(f, obj, id))
        THROW();
    f.regs.sp[-3] = f.regs.sp[-1];
}

template void JS_FASTCALL stubs::ElemInc<true>(VMFrame &f);
template void JS_FASTCALL stubs::ElemInc<false>(VMFrame &f);

template<JSBool strict>
void JS_FASTCALL
stubs::ElemDec(VMFrame &f)
{
    JSObject *obj = ValueToObject(f.cx, &f.regs.sp[-2]);
    if (!obj)
        THROW();
    jsid id;
    if (!FetchElementId(f, obj, f.regs.sp[-1], id, &f.regs.sp[-1]))
        THROW();
    if (!ObjIncOp<-1, true, strict>(f, obj, id))
        THROW();
    f.regs.sp[-3] = f.regs.sp[-1];
}

template void JS_FASTCALL stubs::ElemDec<true>(VMFrame &f);
template void JS_FASTCALL stubs::ElemDec<false>(VMFrame &f);

template<JSBool strict>
void JS_FASTCALL
stubs::IncElem(VMFrame &f)
{
    JSObject *obj = ValueToObject(f.cx, &f.regs.sp[-2]);
    if (!obj)
        THROW();
    jsid id;
    if (!FetchElementId(f, obj, f.regs.sp[-1], id, &f.regs.sp[-1]))
        THROW();
    if (!ObjIncOp<1, false, strict>(f, obj, id))
        THROW();
    f.regs.sp[-3] = f.regs.sp[-1];
}

template void JS_FASTCALL stubs::IncElem<true>(VMFrame &f);
template void JS_FASTCALL stubs::IncElem<false>(VMFrame &f);

template<JSBool strict>
void JS_FASTCALL
stubs::DecElem(VMFrame &f)
{
    JSObject *obj = ValueToObject(f.cx, &f.regs.sp[-2]);
    if (!obj)
        THROW();
    jsid id;
    if (!FetchElementId(f, obj, f.regs.sp[-1], id, &f.regs.sp[-1]))
        THROW();
    if (!ObjIncOp<-1, false, strict>(f, obj, id))
        THROW();
    f.regs.sp[-3] = f.regs.sp[-1];
}

template void JS_FASTCALL stubs::DecElem<true>(VMFrame &f);
template void JS_FASTCALL stubs::DecElem<false>(VMFrame &f);

template<JSBool strict>
void JS_FASTCALL
stubs::NameInc(VMFrame &f, JSAtom *atom)
{
    JSObject *obj = &f.fp()->scopeChain();
    if (!NameIncDec<1, true, strict>(f, obj, atom))
        THROW();
}

template void JS_FASTCALL stubs::NameInc<true>(VMFrame &f, JSAtom *atom);
template void JS_FASTCALL stubs::NameInc<false>(VMFrame &f, JSAtom *atom);

template<JSBool strict>
void JS_FASTCALL
stubs::NameDec(VMFrame &f, JSAtom *atom)
{
    JSObject *obj = &f.fp()->scopeChain();
    if (!NameIncDec<-1, true, strict>(f, obj, atom))
        THROW();
}

template void JS_FASTCALL stubs::NameDec<true>(VMFrame &f, JSAtom *atom);
template void JS_FASTCALL stubs::NameDec<false>(VMFrame &f, JSAtom *atom);

template<JSBool strict>
void JS_FASTCALL
stubs::IncName(VMFrame &f, JSAtom *atom)
{
    JSObject *obj = &f.fp()->scopeChain();
    if (!NameIncDec<1, false, strict>(f, obj, atom))
        THROW();
}

template void JS_FASTCALL stubs::IncName<true>(VMFrame &f, JSAtom *atom);
template void JS_FASTCALL stubs::IncName<false>(VMFrame &f, JSAtom *atom);

template<JSBool strict>
void JS_FASTCALL
stubs::DecName(VMFrame &f, JSAtom *atom)
{
    JSObject *obj = &f.fp()->scopeChain();
    if (!NameIncDec<-1, false, strict>(f, obj, atom))
        THROW();
}

template void JS_FASTCALL stubs::DecName<true>(VMFrame &f, JSAtom *atom);
template void JS_FASTCALL stubs::DecName<false>(VMFrame &f, JSAtom *atom);

template<JSBool strict>
void JS_FASTCALL
stubs::GlobalNameInc(VMFrame &f, JSAtom *atom)
{
    JSObject *obj = f.fp()->scopeChain().getGlobal();
    if (!NameIncDec<1, true, strict>(f, obj, atom))
        THROW();
}

template void JS_FASTCALL stubs::GlobalNameInc<true>(VMFrame &f, JSAtom *atom);
template void JS_FASTCALL stubs::GlobalNameInc<false>(VMFrame &f, JSAtom *atom);

template<JSBool strict>
void JS_FASTCALL
stubs::GlobalNameDec(VMFrame &f, JSAtom *atom)
{
    JSObject *obj = f.fp()->scopeChain().getGlobal();
    if (!NameIncDec<-1, true, strict>(f, obj, atom))
        THROW();
}

template void JS_FASTCALL stubs::GlobalNameDec<true>(VMFrame &f, JSAtom *atom);
template void JS_FASTCALL stubs::GlobalNameDec<false>(VMFrame &f, JSAtom *atom);

template<JSBool strict>
void JS_FASTCALL
stubs::IncGlobalName(VMFrame &f, JSAtom *atom)
{
    JSObject *obj = f.fp()->scopeChain().getGlobal();
    if (!NameIncDec<1, false, strict>(f, obj, atom))
        THROW();
}

template void JS_FASTCALL stubs::IncGlobalName<true>(VMFrame &f, JSAtom *atom);
template void JS_FASTCALL stubs::IncGlobalName<false>(VMFrame &f, JSAtom *atom);

template<JSBool strict>
void JS_FASTCALL
stubs::DecGlobalName(VMFrame &f, JSAtom *atom)
{
    JSObject *obj = f.fp()->scopeChain().getGlobal();
    if (!NameIncDec<-1, false, strict>(f, obj, atom))
        THROW();
}

template void JS_FASTCALL stubs::DecGlobalName<true>(VMFrame &f, JSAtom *atom);
template void JS_FASTCALL stubs::DecGlobalName<false>(VMFrame &f, JSAtom *atom);

static bool JS_FASTCALL
InlineGetProp(VMFrame &f)
{
    JSContext *cx = f.cx;
    JSFrameRegs &regs = f.regs;

    Value *vp = &f.regs.sp[-1];
    JSObject *obj = ValueToObject(f.cx, vp);
    if (!obj)
        return false;

    Value rval;
    do {
        /*
         * We do not impose the method read barrier if in an imacro,
         * assuming any property gets it does (e.g., for 'toString'
         * from JSOP_NEW) will not be leaked to the calling script.
         */
        JSObject *aobj = js_GetProtoIfDenseArray(obj);

        PropertyCacheEntry *entry;
        JSObject *obj2;
        JSAtom *atom;
        JS_PROPERTY_CACHE(cx).test(cx, regs.pc, aobj, obj2, entry, atom);
        if (!atom) {
            if (entry->vword.isFunObj()) {
                rval.setObject(entry->vword.toFunObj());
            } else if (entry->vword.isSlot()) {
                uint32 slot = entry->vword.toSlot();
                rval = obj2->nativeGetSlot(slot);
            } else {
                JS_ASSERT(entry->vword.isShape());
                const Shape *shape = entry->vword.toShape();
                NATIVE_GET(cx, obj, obj2, shape,
                        f.fp()->hasImacropc() ? JSGET_NO_METHOD_BARRIER : JSGET_METHOD_BARRIER,
                        &rval, return false);
            }
            break;
        }

        jsid id = ATOM_TO_JSID(atom);
        if (JS_LIKELY(!aobj->getOps()->getProperty)
                ? !js_GetPropertyHelper(cx, obj, id,
                    f.fp()->hasImacropc()
                    ? JSGET_CACHE_RESULT | JSGET_NO_METHOD_BARRIER
                    : JSGET_CACHE_RESULT | JSGET_METHOD_BARRIER,
                    &rval)
                : !obj->getProperty(cx, id, &rval)) {
            return false;
        }
    } while(0);

    regs.sp[-1] = rval;
    return true;
}

void JS_FASTCALL
stubs::GetProp(VMFrame &f)
{
    if (!InlineGetProp(f))
        THROW();
}

void JS_FASTCALL
stubs::GetPropNoCache(VMFrame &f, JSAtom *atom)
{
    JSContext *cx = f.cx;

    Value *vp = &f.regs.sp[-1];
    JSObject *obj = ValueToObject(cx, vp);
    if (!obj)
        THROW();

    if (!obj->getProperty(cx, ATOM_TO_JSID(atom), vp))
        THROW();
}

void JS_FASTCALL
stubs::CallProp(VMFrame &f, JSAtom *origAtom)
{
    JSContext *cx = f.cx;
    JSFrameRegs &regs = f.regs;

    Value lval;
    lval = regs.sp[-1];

    Value objv;
    if (lval.isObject()) {
        objv = lval;
    } else {
        JSProtoKey protoKey;
        if (lval.isString()) {
            protoKey = JSProto_String;
        } else if (lval.isNumber()) {
            protoKey = JSProto_Number;
        } else if (lval.isBoolean()) {
            protoKey = JSProto_Boolean;
        } else {
            JS_ASSERT(lval.isNull() || lval.isUndefined());
            js_ReportIsNullOrUndefined(cx, -1, lval, NULL);
            THROW();
        }
        JSObject *pobj;
        if (!js_GetClassPrototype(cx, NULL, protoKey, &pobj))
            THROW();
        objv.setObject(*pobj);
    }

    JSObject *aobj = js_GetProtoIfDenseArray(&objv.toObject());
    Value rval;

    PropertyCacheEntry *entry;
    JSObject *obj2;
    JSAtom *atom;
    JS_PROPERTY_CACHE(cx).test(cx, regs.pc, aobj, obj2, entry, atom);
    if (!atom) {
        if (entry->vword.isFunObj()) {
            rval.setObject(entry->vword.toFunObj());
        } else if (entry->vword.isSlot()) {
            uint32 slot = entry->vword.toSlot();
            rval = obj2->nativeGetSlot(slot);
        } else {
            JS_ASSERT(entry->vword.isShape());
            const Shape *shape = entry->vword.toShape();
            NATIVE_GET(cx, &objv.toObject(), obj2, shape, JSGET_NO_METHOD_BARRIER, &rval,
                       THROW());
        }
        regs.sp++;
        regs.sp[-2] = rval;
        regs.sp[-1] = lval;
    } else {
        /*
         * Cache miss: use the immediate atom that was loaded for us under
         * PropertyCache::test.
         */
        jsid id;
        id = ATOM_TO_JSID(origAtom);

        regs.sp++;
        regs.sp[-1].setNull();
        if (lval.isObject()) {
            if (!js_GetMethod(cx, &objv.toObject(), id,
                              JS_LIKELY(!aobj->getOps()->getProperty)
                              ? JSGET_CACHE_RESULT | JSGET_NO_METHOD_BARRIER
                              : JSGET_NO_METHOD_BARRIER,
                              &rval)) {
                THROW();
            }
            regs.sp[-1] = objv;
            regs.sp[-2] = rval;
        } else {
            JS_ASSERT(!objv.toObject().getOps()->getProperty);
            if (!js_GetPropertyHelper(cx, &objv.toObject(), id,
                                      JSGET_CACHE_RESULT | JSGET_NO_METHOD_BARRIER,
                                      &rval)) {
                THROW();
            }
            regs.sp[-1] = lval;
            regs.sp[-2] = rval;
        }
    }
#if JS_HAS_NO_SUCH_METHOD
    if (JS_UNLIKELY(rval.isUndefined()) && regs.sp[-1].isObject()) {
        regs.sp[-2].setString(ATOM_TO_STRING(origAtom));
        if (!js_OnUnknownMethod(cx, regs.sp - 2))
            THROW();
    }
#endif
}

void JS_FASTCALL
stubs::Length(VMFrame &f)
{
    JSFrameRegs &regs = f.regs;
    Value *vp = &regs.sp[-1];

    if (vp->isString()) {
        vp->setInt32(vp->toString()->length());
        return;
    } else if (vp->isObject()) {
        JSObject *obj = &vp->toObject();
        if (obj->isArray()) {
            jsuint length = obj->getArrayLength();
            regs.sp[-1].setNumber(length);
            return;
        } else if (obj->isArguments() && !obj->isArgsLengthOverridden()) {
            uint32 length = obj->getArgsInitialLength();
            JS_ASSERT(length < INT32_MAX);
            regs.sp[-1].setInt32(int32_t(length));
            return;
        }
    }

    if (!InlineGetProp(f))
        THROW();
}

void JS_FASTCALL
stubs::Iter(VMFrame &f, uint32 flags)
{
    if (!js_ValueToIterator(f.cx, flags, &f.regs.sp[-1]))
        THROW();
    JS_ASSERT(!f.regs.sp[-1].isPrimitive());
}

static void
InitPropOrMethod(VMFrame &f, JSAtom *atom, JSOp op)
{
    JSContext *cx = f.cx;
    JSRuntime *rt = cx->runtime;
    JSFrameRegs &regs = f.regs;

    /* Load the property's initial value into rval. */
    JS_ASSERT(regs.sp - f.fp()->base() >= 2);
    Value rval;
    rval = regs.sp[-1];

    /* Load the object being initialized into lval/obj. */
    JSObject *obj = &regs.sp[-2].toObject();
    JS_ASSERT(obj->isNative());

    /*
     * Probe the property cache.
     *
     * We can not assume that the object created by JSOP_NEWINIT is still
     * single-threaded as the debugger can access it from other threads.
     * So check first.
     *
     * On a hit, if the cached shape has a non-default setter, it must be
     * __proto__. If shape->previous() != obj->lastProperty(), there must be a
     * repeated property name. The fast path does not handle these two cases.
     */
    PropertyCacheEntry *entry;
    const Shape *shape;
    if (JS_PROPERTY_CACHE(cx).testForInit(rt, regs.pc, obj, &shape, &entry) &&
        shape->hasDefaultSetter() &&
        shape->previous() == obj->lastProperty())
    {
        /* Fast path. Property cache hit. */
        uint32 slot = shape->slot;

        JS_ASSERT(slot == obj->slotSpan());
        JS_ASSERT(slot >= JSSLOT_FREE(obj->getClass()));
        if (slot < obj->numSlots()) {
            JS_ASSERT(obj->getSlot(slot).isUndefined());
        } else {
            if (!obj->allocSlot(cx, &slot))
                THROW();
            JS_ASSERT(slot == shape->slot);
        }

        /* A new object, or one we just extended in a recent initprop op. */
        JS_ASSERT(!obj->lastProperty() ||
                  obj->shape() == obj->lastProperty()->shape);
        obj->extend(cx, shape);

        /*
         * No method change check here because here we are adding a new
         * property, not updating an existing slot's value that might
         * contain a method of a branded shape.
         */
        obj->nativeSetSlot(slot, rval);
    } else {
        PCMETER(JS_PROPERTY_CACHE(cx).inipcmisses++);

        /* Get the immediate property name into id. */
        jsid id = ATOM_TO_JSID(atom);

        uintN defineHow = (op == JSOP_INITMETHOD)
                          ? JSDNP_CACHE_RESULT | JSDNP_SET_METHOD
                          : JSDNP_CACHE_RESULT;
        if (!(JS_UNLIKELY(atom == cx->runtime->atomState.protoAtom)
              ? js_SetPropertyHelper(cx, obj, id, defineHow, &rval, false)
              : js_DefineNativeProperty(cx, obj, id, rval, NULL, NULL,
                                        JSPROP_ENUMERATE, 0, 0, NULL,
                                        defineHow))) {
            THROW();
        }
    }
}

void JS_FASTCALL
stubs::InitProp(VMFrame &f, JSAtom *atom)
{
    InitPropOrMethod(f, atom, JSOP_INITPROP);
}

void JS_FASTCALL
stubs::InitMethod(VMFrame &f, JSAtom *atom)
{
    InitPropOrMethod(f, atom, JSOP_INITMETHOD);
}

void JS_FASTCALL
stubs::IterNext(VMFrame &f)
{
    JS_ASSERT(f.regs.sp - 1 >= f.fp()->base());
    JS_ASSERT(f.regs.sp[-1].isObject());

    JSObject *iterobj = &f.regs.sp[-1].toObject();
    f.regs.sp[0].setNull();
    f.regs.sp++;
    if (!js_IteratorNext(f.cx, iterobj, &f.regs.sp[-1]))
        THROW();
}

JSBool JS_FASTCALL
stubs::IterMore(VMFrame &f)
{
    JS_ASSERT(f.regs.sp - 1 >= f.fp()->base());
    JS_ASSERT(f.regs.sp[-1].isObject());

    Value v;
    JSObject *iterobj = &f.regs.sp[-1].toObject();
    if (!js_IteratorMore(f.cx, iterobj, &v))
        THROWV(JS_FALSE);

    return v.toBoolean();
}

void JS_FASTCALL
stubs::EndIter(VMFrame &f)
{
    JS_ASSERT(f.regs.sp - 1 >= f.fp()->base());
    if (!js_CloseIterator(f.cx, &f.regs.sp[-1].toObject()))
        THROW();
}

JSString * JS_FASTCALL
stubs::TypeOf(VMFrame &f)
{
    const Value &ref = f.regs.sp[-1];
    JSType type = JS_TypeOfValue(f.cx, Jsvalify(ref));
    JSAtom *atom = f.cx->runtime->atomState.typeAtoms[type];
    return ATOM_TO_STRING(atom);
}

void JS_FASTCALL
stubs::StrictEq(VMFrame &f)
{
    const Value &rhs = f.regs.sp[-1];
    const Value &lhs = f.regs.sp[-2];
    JSBool equal;
    if (!StrictlyEqual(f.cx, lhs, rhs, &equal))
        THROW();
    f.regs.sp--;
    f.regs.sp[-1].setBoolean(equal == JS_TRUE);
}

void JS_FASTCALL
stubs::StrictNe(VMFrame &f)
{
    const Value &rhs = f.regs.sp[-1];
    const Value &lhs = f.regs.sp[-2];
    JSBool equal;
    if (!StrictlyEqual(f.cx, lhs, rhs, &equal))
        THROW();
    f.regs.sp--;
    f.regs.sp[-1].setBoolean(equal != JS_TRUE);
}

void JS_FASTCALL
stubs::Throw(VMFrame &f)
{
    JSContext *cx = f.cx;

    JS_ASSERT(!cx->isExceptionPending());
    cx->setPendingException(f.regs.sp[-1]);
    THROW();
}

JSObject * JS_FASTCALL
stubs::FlatLambda(VMFrame &f, JSFunction *fun)
{
    JSObject *obj = js_NewFlatClosure(f.cx, fun, JSOP_LAMBDA_FC, JSOP_LAMBDA_FC_LENGTH);
    if (!obj)
        THROWV(NULL);
    return obj;
}

void JS_FASTCALL
stubs::Arguments(VMFrame &f)
{
    f.regs.sp++;
    if (!js_GetArgsValue(f.cx, f.fp(), &f.regs.sp[-1]))
        THROW();
}

JSBool JS_FASTCALL
stubs::InstanceOf(VMFrame &f)
{
    JSContext *cx = f.cx;
    JSFrameRegs &regs = f.regs;

    const Value &rref = regs.sp[-1];
    if (rref.isPrimitive()) {
        js_ReportValueError(cx, JSMSG_BAD_INSTANCEOF_RHS,
                            -1, rref, NULL);
        THROWV(JS_FALSE);
    }
    JSObject *obj = &rref.toObject();
    const Value &lref = regs.sp[-2];
    JSBool cond = JS_FALSE;
    if (!HasInstance(cx, obj, &lref, &cond))
        THROWV(JS_FALSE);
    f.regs.sp[-2].setBoolean(cond);
    return cond;
}

void JS_FASTCALL
stubs::FastInstanceOf(VMFrame &f)
{
    const Value &lref = f.regs.sp[-1];

    if (lref.isPrimitive()) {
        /*
         * Throw a runtime error if instanceof is called on a function that
         * has a non-object as its .prototype value.
         */
        js_ReportValueError(f.cx, JSMSG_BAD_PROTOTYPE, -1, f.regs.sp[-2], NULL);
        THROW();
    }

    f.regs.sp[-3].setBoolean(js_IsDelegate(f.cx, &lref.toObject(), f.regs.sp[-3]));
}

void JS_FASTCALL
stubs::ArgCnt(VMFrame &f)
{
    JSContext *cx = f.cx;
    JSRuntime *rt = cx->runtime;
    JSStackFrame *fp = f.fp();

    jsid id = ATOM_TO_JSID(rt->atomState.lengthAtom);
    f.regs.sp++;
    if (!js_GetArgsProperty(cx, fp, id, &f.regs.sp[-1]))
        THROW();
}

void JS_FASTCALL
stubs::EnterBlock(VMFrame &f, JSObject *obj)
{
    JSFrameRegs &regs = f.regs;
#ifdef DEBUG
    JSStackFrame *fp = f.fp();
#endif

    JS_ASSERT(obj->isStaticBlock());
    JS_ASSERT(fp->base() + OBJ_BLOCK_DEPTH(cx, obj) == regs.sp);
    Value *vp = regs.sp + OBJ_BLOCK_COUNT(cx, obj);
    JS_ASSERT(regs.sp < vp);
    JS_ASSERT(vp <= fp->slots() + fp->script()->nslots);
    SetValueRangeToUndefined(regs.sp, vp);
    regs.sp = vp;

#ifdef DEBUG
    JSContext *cx = f.cx;

    /*
     * The young end of fp->scopeChain() may omit blocks if we haven't closed
     * over them, but if there are any closure blocks on fp->scopeChain(), they'd
     * better be (clones of) ancestors of the block we're entering now;
     * anything else we should have popped off fp->scopeChain() when we left its
     * static scope.
     */
    JSObject *obj2 = &fp->scopeChain();
    Class *clasp;
    while ((clasp = obj2->getClass()) == &js_WithClass)
        obj2 = obj2->getParent();
    if (clasp == &js_BlockClass &&
        obj2->getPrivate() == js_FloatingFrameIfGenerator(cx, fp)) {
        JSObject *youngestProto = obj2->getProto();
        JS_ASSERT(youngestProto->isStaticBlock());
        JSObject *parent = obj;
        while ((parent = parent->getParent()) != youngestProto)
            JS_ASSERT(parent);
    }
#endif
}

void JS_FASTCALL
stubs::LeaveBlock(VMFrame &f, JSObject *blockChain)
{
    JSContext *cx = f.cx;
    JSStackFrame *fp = f.fp();

#ifdef DEBUG
    JS_ASSERT(blockChain->isStaticBlock());
    uintN blockDepth = OBJ_BLOCK_DEPTH(cx, blockChain);

    JS_ASSERT(blockDepth <= StackDepth(fp->script()));
#endif
    /*
     * If we're about to leave the dynamic scope of a block that has been
     * cloned onto fp->scopeChain(), clear its private data, move its locals from
     * the stack into the clone, and pop it off the chain.
     */
    JSObject *obj = &fp->scopeChain();
    if (obj->getProto() == blockChain) {
        JS_ASSERT(obj->getClass() == &js_BlockClass);
        if (!js_PutBlockObject(cx, JS_TRUE))
            THROW();
    }
}

void * JS_FASTCALL
stubs::LookupSwitch(VMFrame &f, jsbytecode *pc)
{
    jsbytecode *jpc = pc;
    JSScript *script = f.fp()->script();
    bool ctor = f.fp()->isConstructing();

    /* This is correct because the compiler adjusts the stack beforehand. */
    Value lval = f.regs.sp[-1];

    if (!lval.isPrimitive()) {
        void* native = script->nativeCodeForPC(ctor, pc + GET_JUMP_OFFSET(pc));
        JS_ASSERT(native);
        return native;
    }

    JS_ASSERT(pc[0] == JSOP_LOOKUPSWITCH);

    pc += JUMP_OFFSET_LEN;
    uint32 npairs = GET_UINT16(pc);
    pc += UINT16_LEN;

    JS_ASSERT(npairs);

    if (lval.isString()) {
        JSLinearString *str = lval.toString()->ensureLinear(f.cx);
        if (!str)
            THROWV(NULL);
        for (uint32 i = 1; i <= npairs; i++) {
            Value rval = script->getConst(GET_INDEX(pc));
            pc += INDEX_LEN;
            if (rval.isString()) {
                JSLinearString *rhs = rval.toString()->assertIsLinear();
                if (rhs == str || EqualStrings(str, rhs)) {
                    void* native = script->nativeCodeForPC(ctor,
                                                           jpc + GET_JUMP_OFFSET(pc));
                    JS_ASSERT(native);
                    return native;
                }
            }
            pc += JUMP_OFFSET_LEN;
        }
    } else if (lval.isNumber()) {
        double d = lval.toNumber();
        for (uint32 i = 1; i <= npairs; i++) {
            Value rval = script->getConst(GET_INDEX(pc));
            pc += INDEX_LEN;
            if (rval.isNumber() && d == rval.toNumber()) {
                void* native = script->nativeCodeForPC(ctor,
                                                       jpc + GET_JUMP_OFFSET(pc));
                JS_ASSERT(native);
                return native;
            }
            pc += JUMP_OFFSET_LEN;
        }
    } else {
        for (uint32 i = 1; i <= npairs; i++) {
            Value rval = script->getConst(GET_INDEX(pc));
            pc += INDEX_LEN;
            if (lval == rval) {
                void* native = script->nativeCodeForPC(ctor,
                                                       jpc + GET_JUMP_OFFSET(pc));
                JS_ASSERT(native);
                return native;
            }
            pc += JUMP_OFFSET_LEN;
        }
    }

    void* native = script->nativeCodeForPC(ctor, jpc + GET_JUMP_OFFSET(jpc));
    JS_ASSERT(native);
    return native;
}

void * JS_FASTCALL
stubs::TableSwitch(VMFrame &f, jsbytecode *origPc)
{
    jsbytecode * const originalPC = origPc;
    jsbytecode *pc = originalPC;
    uint32 jumpOffset = GET_JUMP_OFFSET(pc);
    pc += JUMP_OFFSET_LEN;

    /* Note: compiler adjusts the stack beforehand. */
    Value rval = f.regs.sp[-1];

    jsint tableIdx;
    if (rval.isInt32()) {
        tableIdx = rval.toInt32();
    } else if (rval.isDouble()) {
        double d = rval.toDouble();
        if (d == 0) {
            /* Treat -0 (double) as 0. */
            tableIdx = 0;
        } else if (!JSDOUBLE_IS_INT32(d, (int32_t *)&tableIdx)) {
            goto finally;
        }
    } else {
        goto finally;
    }

    {
        jsint low = GET_JUMP_OFFSET(pc);
        pc += JUMP_OFFSET_LEN;
        jsint high = GET_JUMP_OFFSET(pc);
        pc += JUMP_OFFSET_LEN;

        tableIdx -= low;
        if ((jsuint) tableIdx < (jsuint)(high - low + 1)) {
            pc += JUMP_OFFSET_LEN * tableIdx;
            uint32 candidateOffset = GET_JUMP_OFFSET(pc);
            if (candidateOffset)
                jumpOffset = candidateOffset;
        }
    }

finally:
    /* Provide the native address. */
    JSScript* script = f.fp()->script();
    void* native = script->nativeCodeForPC(f.fp()->isConstructing(),
                                           originalPC + jumpOffset);
    JS_ASSERT(native);
    return native;
}

void JS_FASTCALL
stubs::Unbrand(VMFrame &f)
{
    const Value &thisv = f.regs.sp[-1];
    if (!thisv.isObject())
        return;
    JSObject *obj = &thisv.toObject();
    if (obj->isNative())
        obj->unbrand(f.cx);
}

void JS_FASTCALL
stubs::Pos(VMFrame &f)
{
    if (!ValueToNumber(f.cx, &f.regs.sp[-1]))
        THROW();
}

void JS_FASTCALL
stubs::ArgSub(VMFrame &f, uint32 n)
{
    jsid id = INT_TO_JSID(n);
    Value rval;
    if (!js_GetArgsProperty(f.cx, f.fp(), id, &rval))
        THROW();
    f.regs.sp[0] = rval;
}

void JS_FASTCALL
stubs::DelName(VMFrame &f, JSAtom *atom)
{
    jsid id = ATOM_TO_JSID(atom);
    JSObject *obj, *obj2;
    JSProperty *prop;
    if (!js_FindProperty(f.cx, id, &obj, &obj2, &prop))
        THROW();

    /* Strict mode code should never contain JSOP_DELNAME opcodes. */
    JS_ASSERT(!f.fp()->script()->strictModeCode);

    /* ECMA says to return true if name is undefined or inherited. */
    f.regs.sp++;
    f.regs.sp[-1] = BooleanValue(true);
    if (prop) {
        if (!obj->deleteProperty(f.cx, id, &f.regs.sp[-1], false))
            THROW();
    }
}

template<JSBool strict>
void JS_FASTCALL
stubs::DelProp(VMFrame &f, JSAtom *atom)
{
    JSContext *cx = f.cx;

    JSObject *obj = ValueToObject(cx, &f.regs.sp[-1]);
    if (!obj)
        THROW();

    Value rval;
    if (!obj->deleteProperty(cx, ATOM_TO_JSID(atom), &rval, strict))
        THROW();

    f.regs.sp[-1] = rval;
}

template void JS_FASTCALL stubs::DelProp<true>(VMFrame &f, JSAtom *atom);
template void JS_FASTCALL stubs::DelProp<false>(VMFrame &f, JSAtom *atom);

template<JSBool strict>
void JS_FASTCALL
stubs::DelElem(VMFrame &f)
{
    JSContext *cx = f.cx;

    JSObject *obj = ValueToObject(cx, &f.regs.sp[-2]);
    if (!obj)
        THROW();

    jsid id;
    if (!FetchElementId(f, obj, f.regs.sp[-1], id, &f.regs.sp[-1]))
        THROW();

    if (!obj->deleteProperty(cx, id, &f.regs.sp[-2], strict))
        THROW();
}

void JS_FASTCALL
stubs::DefVarOrConst(VMFrame &f, JSAtom *atom)
{
    JSContext *cx = f.cx;
    JSStackFrame *fp = f.fp();

    JSObject *obj = &fp->varobj(cx);
    JS_ASSERT(!obj->getOps()->defineProperty);
    uintN attrs = JSPROP_ENUMERATE;
    if (!fp->isEvalFrame())
        attrs |= JSPROP_PERMANENT;

    /* Lookup id in order to check for redeclaration problems. */
    jsid id = ATOM_TO_JSID(atom);
    bool shouldDefine;
    if (JSOp(*f.regs.pc) == JSOP_DEFVAR) {
        /*
         * Redundant declaration of a |var|, even one for a non-writable
         * property like |undefined| in ES5, does nothing.
         */
        JSProperty *prop;
        JSObject *obj2;
        if (!obj->lookupProperty(cx, id, &obj2, &prop))
            THROW();
        shouldDefine = (!prop || obj2 != obj);
    } else {
        JS_ASSERT(JSOp(*f.regs.pc) == JSOP_DEFCONST);
        attrs |= JSPROP_READONLY;
        if (!CheckRedeclaration(cx, obj, id, attrs))
            THROW();

        /*
         * As attrs includes readonly, CheckRedeclaration can succeed only
         * if prop does not exist.
         */
        shouldDefine = true;
    }

    /* Bind a variable only if it's not yet defined. */
    if (shouldDefine && 
        !js_DefineNativeProperty(cx, obj, id, UndefinedValue(), PropertyStub, StrictPropertyStub,
                                     attrs, 0, 0, NULL)) {
        THROW();
    }
}

void JS_FASTCALL
stubs::SetConst(VMFrame &f, JSAtom *atom)
{
    JSContext *cx = f.cx;
    JSStackFrame *fp = f.fp();

    JSObject *obj = &fp->varobj(cx);
    const Value &ref = f.regs.sp[-1];
    if (!obj->defineProperty(cx, ATOM_TO_JSID(atom), ref,
                             PropertyStub, StrictPropertyStub,
                             JSPROP_ENUMERATE | JSPROP_PERMANENT | JSPROP_READONLY)) {
        THROW();
    }
}

JSBool JS_FASTCALL
stubs::In(VMFrame &f)
{
    JSContext *cx = f.cx;

    const Value &rref = f.regs.sp[-1];
    if (!rref.isObject()) {
        js_ReportValueError(cx, JSMSG_IN_NOT_OBJECT, -1, rref, NULL);
        THROWV(JS_FALSE);
    }

    JSObject *obj = &rref.toObject();
    jsid id;
    if (!FetchElementId(f, obj, f.regs.sp[-2], id, &f.regs.sp[-2]))
        THROWV(JS_FALSE);

    JSObject *obj2;
    JSProperty *prop;
    if (!obj->lookupProperty(cx, id, &obj2, &prop))
        THROWV(JS_FALSE);

    return !!prop;
}

template void JS_FASTCALL stubs::DelElem<true>(VMFrame &f);
template void JS_FASTCALL stubs::DelElem<false>(VMFrame &f);

void JS_FASTCALL
stubs::Exception(VMFrame &f)
{
    f.regs.sp[0] = f.cx->getPendingException();
    f.cx->clearPendingException();
}
template <bool Clamped>
int32 JS_FASTCALL
stubs::ConvertToTypedInt(JSContext *cx, Value *vp)
{
    JS_ASSERT(!vp->isInt32());

    if (vp->isDouble()) {
        if (Clamped)
            return js_TypedArray_uint8_clamp_double(vp->toDouble());
        return js_DoubleToECMAInt32(vp->toDouble());
    }

    if (vp->isNull() || vp->isObject() || vp->isUndefined())
        return 0;

    if (vp->isBoolean())
        return vp->toBoolean() ? 1 : 0;

    JS_ASSERT(vp->isString());

    int32 i32 = 0;
#ifdef DEBUG
    bool success = 
#endif
        StringToNumberType<jsint>(cx, vp->toString(), &i32);
    JS_ASSERT(success);

    return i32;
}

template int32 JS_FASTCALL stubs::ConvertToTypedInt<true>(JSContext *, Value *);
template int32 JS_FASTCALL stubs::ConvertToTypedInt<false>(JSContext *, Value *);

void JS_FASTCALL
stubs::ConvertToTypedFloat(JSContext *cx, Value *vp)
{
    JS_ASSERT(!vp->isDouble() && !vp->isInt32());

    if (vp->isNull()) {
        vp->setDouble(0);
    } else if (vp->isObject() || vp->isUndefined()) {
        vp->setDouble(js_NaN);
    } else if (vp->isBoolean()) {
        vp->setDouble(vp->toBoolean() ? 1 : 0);
    } else {
        JS_ASSERT(vp->isString());
        double d = 0;
#ifdef DEBUG
        bool success = 
#endif
            StringToNumberType<double>(cx, vp->toString(), &d);
        JS_ASSERT(success);
        vp->setDouble(d);
    }
}

