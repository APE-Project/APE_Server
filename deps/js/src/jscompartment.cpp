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
 *   Mozilla Foundation
 * Portions created by the Initial Developer are Copyright (C) 2010
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

#include "jscntxt.h"
#include "jscompartment.h"
#include "jsgc.h"
#include "jsiter.h"
#include "jsproxy.h"
#include "jsscope.h"
#include "jstracer.h"
#include "jswrapper.h"
#include "assembler/wtf/Platform.h"
#include "methodjit/MethodJIT.h"
#include "methodjit/PolyIC.h"
#include "methodjit/MonoIC.h"

#include "jsgcinlines.h"

#if ENABLE_YARR_JIT
#include "assembler/jit/ExecutableAllocator.h"
#endif

using namespace js;
using namespace js::gc;

JSCompartment::JSCompartment(JSRuntime *rt)
  : rt(rt),
    principals(NULL),
    gcBytes(0),
    gcTriggerBytes(0),
    gcLastBytes(0),
    data(NULL),
    active(false),
#ifdef JS_METHODJIT
    jaegerCompartment(NULL),
#endif
    propertyTree(thisForCtor()),
    debugMode(rt->debugMode),
#if ENABLE_YARR_JIT
    regExpAllocator(NULL),
#endif
    mathCache(NULL),
    marked(false)
{
    JS_INIT_CLIST(&scripts);

#ifdef JS_TRACER
    /* InitJIT expects this area to be zero'd. */
    PodZero(&traceMonitor);
#endif

    PodArrayZero(scriptsToGC);
}

JSCompartment::~JSCompartment()
{
    Shape::finishEmptyShapes(this);
    propertyTree.finish();

#if ENABLE_YARR_JIT
    js_delete(regExpAllocator);
#endif

#if defined JS_TRACER
    FinishJIT(&traceMonitor);
#endif

#ifdef JS_METHODJIT
    js_delete(jaegerCompartment);
#endif

    js_delete(mathCache);

#ifdef DEBUG
    for (size_t i = 0; i != JS_ARRAY_LENGTH(scriptsToGC); ++i)
        JS_ASSERT(!scriptsToGC[i]);
#endif
}

bool
JSCompartment::init()
{
    chunk = NULL;
    for (unsigned i = 0; i < FINALIZE_LIMIT; i++)
        arenas[i].init();
    for (unsigned i = 0; i < FINALIZE_LIMIT; i++)
        freeLists.finalizables[i] = NULL;
#ifdef JS_GCMETER
    memset(&compartmentStats, 0, sizeof(JSGCArenaStats) * FINALIZE_LIMIT);
#endif
    if (!crossCompartmentWrappers.init())
        return false;

    if (!propertyTree.init())
        return false;

#ifdef DEBUG
    if (rt->meterEmptyShapes()) {
        if (!emptyShapes.init())
            return false;
    }
#endif

    if (!Shape::initEmptyShapes(this))
        return false;

#ifdef JS_TRACER
    if (!InitJIT(&traceMonitor))
        return false;
#endif

    if (!toSourceCache.init())
        return false;

#if ENABLE_YARR_JIT
    regExpAllocator = JSC::ExecutableAllocator::create();
    if (!regExpAllocator)
        return false;
#endif

    if (!backEdgeTable.init())
        return false;

#ifdef JS_METHODJIT
    if (!(jaegerCompartment = js_new<mjit::JaegerCompartment>()))
        return false;
    return jaegerCompartment->Initialize();
#else
    return true;
#endif
}

bool
JSCompartment::arenaListsAreEmpty()
{
  for (unsigned i = 0; i < FINALIZE_LIMIT; i++) {
       if (!arenas[i].isEmpty())
           return false;
  }
  return true;
}

static bool
IsCrossCompartmentWrapper(JSObject *wrapper)
{
    return wrapper->isWrapper() &&
           !!(JSWrapper::wrapperHandler(wrapper)->flags() & JSWrapper::CROSS_COMPARTMENT);
}

bool
JSCompartment::wrap(JSContext *cx, Value *vp)
{
    JS_ASSERT(cx->compartment == this);

    uintN flags = 0;

    JS_CHECK_RECURSION(cx, return false);

    /* Only GC things have to be wrapped or copied. */
    if (!vp->isMarkable())
        return true;

    if (vp->isString()) {
        JSString *str = vp->toString();

        /* Static strings do not have to be wrapped. */
        if (JSString::isStatic(str))
            return true;

        /* If the string is already in this compartment, we are done. */
        if (str->asCell()->compartment() == this)
            return true;

        /* If the string is an atom, we don't have to copy. */
        if (str->isAtomized()) {
            JS_ASSERT(str->asCell()->compartment() == cx->runtime->atomsCompartment);
            return true;
        }
    }

    /*
     * Wrappers should really be parented to the wrapped parent of the wrapped
     * object, but in that case a wrapped global object would have a NULL
     * parent without being a proper global object (JSCLASS_IS_GLOBAL). Instead
,
     * we parent all wrappers to the global object in their home compartment.
     * This loses us some transparency, and is generally very cheesy.
     */
    JSObject *global;
    if (cx->hasfp()) {
        global = cx->fp()->scopeChain().getGlobal();
    } else {
        global = cx->globalObject;
        OBJ_TO_INNER_OBJECT(cx, global);
        if (!global)
            return false;
    }

    /* Unwrap incoming objects. */
    if (vp->isObject()) {
        JSObject *obj = &vp->toObject();

        /* If the object is already in this compartment, we are done. */
        if (obj->compartment() == this)
            return true;

        /* Translate StopIteration singleton. */
        if (obj->getClass() == &js_StopIterationClass)
            return js_FindClassObject(cx, NULL, JSProto_StopIteration, vp);

        /* Don't unwrap an outer window proxy. */
        if (!obj->getClass()->ext.innerObject) {
            obj = vp->toObject().unwrap(&flags);
            vp->setObject(*obj);
            if (obj->getCompartment() == this)
                return true;

            if (cx->runtime->preWrapObjectCallback) {
                obj = cx->runtime->preWrapObjectCallback(cx, global, obj, flags);
                if (!obj)
                    return false;
            }

            vp->setObject(*obj);
            if (obj->getCompartment() == this)
                return true;
        } else {
            if (cx->runtime->preWrapObjectCallback) {
                obj = cx->runtime->preWrapObjectCallback(cx, global, obj, flags);
                if (!obj)
                    return false;
            }

            JS_ASSERT(!obj->isWrapper() || obj->getClass()->ext.innerObject);
            vp->setObject(*obj);
        }

#ifdef DEBUG
        {
            JSObject *outer = obj;
            OBJ_TO_OUTER_OBJECT(cx, outer);
            JS_ASSERT(outer && outer == obj);
        }
#endif
    }

    /* If we already have a wrapper for this value, use it. */
    if (WrapperMap::Ptr p = crossCompartmentWrappers.lookup(*vp)) {
        *vp = p->value;
        if (vp->isObject()) {
            JSObject *obj = &vp->toObject();
            JS_ASSERT(IsCrossCompartmentWrapper(obj));
            if (obj->getParent() != global) {
                do {
                    obj->setParent(global);
                    obj = obj->getProto();
                } while (obj && IsCrossCompartmentWrapper(obj));
            }
        }
        return true;
    }

    if (vp->isString()) {
        Value orig = *vp;
        JSString *str = vp->toString();
        const jschar *chars = str->getChars(cx);
        if (!chars)
            return false;
        JSString *wrapped = js_NewStringCopyN(cx, chars, str->length());
        if (!wrapped)
            return false;
        vp->setString(wrapped);
        return crossCompartmentWrappers.put(orig, *vp);
    }

    JSObject *obj = &vp->toObject();

    /*
     * Recurse to wrap the prototype. Long prototype chains will run out of
     * stack, causing an error in CHECK_RECURSE.
     *
     * Wrapping the proto before creating the new wrapper and adding it to the
     * cache helps avoid leaving a bad entry in the cache on OOM. But note that
     * if we wrapped both proto and parent, we would get infinite recursion
     * here (since Object.prototype->parent->proto leads to Object.prototype
     * itself).
     */
    JSObject *proto = obj->getProto();
    if (!wrap(cx, &proto))
        return false;

    /*
     * We hand in the original wrapped object into the wrap hook to allow
     * the wrap hook to reason over what wrappers are currently applied
     * to the object.
     */
    JSObject *wrapper = cx->runtime->wrapObjectCallback(cx, obj, proto, global, flags);
    if (!wrapper)
        return false;

    vp->setObject(*wrapper);

    wrapper->setProto(proto);
    if (!crossCompartmentWrappers.put(wrapper->getProxyPrivate(), *vp))
        return false;

    wrapper->setParent(global);
    return true;
}

bool
JSCompartment::wrap(JSContext *cx, JSString **strp)
{
    AutoValueRooter tvr(cx, StringValue(*strp));
    if (!wrap(cx, tvr.addr()))
        return false;
    *strp = tvr.value().toString();
    return true;
}

bool
JSCompartment::wrap(JSContext *cx, JSObject **objp)
{
    if (!*objp)
        return true;
    AutoValueRooter tvr(cx, ObjectValue(**objp));
    if (!wrap(cx, tvr.addr()))
        return false;
    *objp = &tvr.value().toObject();
    return true;
}

bool
JSCompartment::wrapId(JSContext *cx, jsid *idp)
{
    if (JSID_IS_INT(*idp))
        return true;
    AutoValueRooter tvr(cx, IdToValue(*idp));
    if (!wrap(cx, tvr.addr()))
        return false;
    return ValueToId(cx, tvr.value(), idp);
}

bool
JSCompartment::wrap(JSContext *cx, PropertyOp *propp)
{
    Value v = CastAsObjectJsval(*propp);
    if (!wrap(cx, &v))
        return false;
    *propp = CastAsPropertyOp(v.toObjectOrNull());
    return true;
}

bool
JSCompartment::wrap(JSContext *cx, StrictPropertyOp *propp)
{
    Value v = CastAsObjectJsval(*propp);
    if (!wrap(cx, &v))
        return false;
    *propp = CastAsStrictPropertyOp(v.toObjectOrNull());
    return true;
}

bool
JSCompartment::wrap(JSContext *cx, PropertyDescriptor *desc)
{
    return wrap(cx, &desc->obj) &&
           (!(desc->attrs & JSPROP_GETTER) || wrap(cx, &desc->getter)) &&
           (!(desc->attrs & JSPROP_SETTER) || wrap(cx, &desc->setter)) &&
           wrap(cx, &desc->value);
}

bool
JSCompartment::wrap(JSContext *cx, AutoIdVector &props)
{
    jsid *vector = props.begin();
    jsint length = props.length();
    for (size_t n = 0; n < size_t(length); ++n) {
        if (!wrapId(cx, &vector[n]))
            return false;
    }
    return true;
}

#if defined JS_METHODJIT && defined JS_MONOIC
/*
 * Check if the pool containing the code for jit should be destroyed, per the
 * heuristics in JSCompartment::sweep.
 */
static inline bool
ScriptPoolDestroyed(JSContext *cx, mjit::JITScript *jit,
                    uint32 releaseInterval, uint32 &counter)
{
    JSC::ExecutablePool *pool = jit->code.m_executablePool;
    if (pool->m_gcNumber != cx->runtime->gcNumber) {
        /*
         * The m_destroy flag may have been set in a previous GC for a pool which had
         * references we did not remove (e.g. from the compartment's ExecutableAllocator)
         * and is still around. Forget we tried to destroy it in such cases.
         */
        pool->m_destroy = false;
        pool->m_gcNumber = cx->runtime->gcNumber;
        if (--counter == 0) {
            pool->m_destroy = true;
            counter = releaseInterval;
        }
    }
    return pool->m_destroy;
}
#endif

/*
 * This method marks pointers that cross compartment boundaries. It should be
 * called only by per-compartment GCs, since full GCs naturally follow pointers
 * across compartments.
 */
void
JSCompartment::markCrossCompartment(JSTracer *trc)
{
    for (WrapperMap::Enum e(crossCompartmentWrappers); !e.empty(); e.popFront())
        MarkValue(trc, e.front().key, "cross-compartment wrapper");
}

void
JSCompartment::mark(JSTracer *trc)
{
    if (IS_GC_MARKING_TRACER(trc)) {
        JSRuntime *rt = trc->context->runtime;

        if (rt->gcCurrentCompartment && rt->gcCurrentCompartment != this)
            return;

        if (marked)
            return;
        marked = true;
    }

    if (emptyArgumentsShape)
        emptyArgumentsShape->trace(trc);
    if (emptyBlockShape)
        emptyBlockShape->trace(trc);
    if (emptyCallShape)
        emptyCallShape->trace(trc);
    if (emptyDeclEnvShape)
        emptyDeclEnvShape->trace(trc);
    if (emptyEnumeratorShape)
        emptyEnumeratorShape->trace(trc);
    if (emptyWithShape)
        emptyWithShape->trace(trc);
}

void
JSCompartment::sweep(JSContext *cx, uint32 releaseInterval)
{
    chunk = NULL;
    /* Remove dead wrappers from the table. */
    for (WrapperMap::Enum e(crossCompartmentWrappers); !e.empty(); e.popFront()) {
        JS_ASSERT_IF(IsAboutToBeFinalized(cx, e.front().key.toGCThing()) &&
                     !IsAboutToBeFinalized(cx, e.front().value.toGCThing()),
                     e.front().key.isString());
        if (IsAboutToBeFinalized(cx, e.front().key.toGCThing()) ||
            IsAboutToBeFinalized(cx, e.front().value.toGCThing())) {
            e.removeFront();
        }
    }

#ifdef JS_TRACER
    traceMonitor.sweep(cx);
#endif

#if defined JS_METHODJIT && defined JS_MONOIC

    /*
     * The release interval is the frequency with which we should try to destroy
     * executable pools by releasing all JIT code in them, zero to never destroy pools.
     * Initialize counter so that the first pool will be destroyed, and eventually drive
     * the amount of JIT code in never-used compartments to zero. Don't discard anything
     * for compartments which currently have active stack frames.
     */
    uint32 counter = 1;
    bool discardScripts = !active && releaseInterval != 0;

    for (JSCList *cursor = scripts.next; cursor != &scripts; cursor = cursor->next) {
        JSScript *script = reinterpret_cast<JSScript *>(cursor);
        if (script->hasJITCode()) {
            mjit::ic::SweepCallICs(cx, script, discardScripts);
            if (discardScripts) {
                if (script->jitNormal &&
                    ScriptPoolDestroyed(cx, script->jitNormal, releaseInterval, counter)) {
                    mjit::ReleaseScriptCode(cx, script);
                    continue;
                }
                if (script->jitCtor &&
                    ScriptPoolDestroyed(cx, script->jitCtor, releaseInterval, counter)) {
                    mjit::ReleaseScriptCode(cx, script);
                }
            }
        }
    }

#endif /* JS_METHODJIT && JS_MONOIC */

    active = false;
}

void
JSCompartment::purge(JSContext *cx)
{
    freeLists.purge();
    dtoaCache.purge();

    /* Destroy eval'ed scripts. */
    js_DestroyScriptsToGC(cx, this);

    nativeIterCache.purge();
    toSourceCache.clear();

#ifdef JS_TRACER
    /*
     * If we are about to regenerate shapes, we have to flush the JIT cache,
     * which will eventually abort any current recording.
     */
    if (cx->runtime->gcRegenShapes)
        traceMonitor.needFlush = JS_TRUE;
#endif

#ifdef JS_METHODJIT
    for (JSScript *script = (JSScript *)scripts.next;
         &script->links != &scripts;
         script = (JSScript *)script->links.next) {
        if (script->hasJITCode()) {
# if defined JS_POLYIC
            mjit::ic::PurgePICs(cx, script);
# endif
# if defined JS_MONOIC
            /*
             * MICs do not refer to data which can be GC'ed and do not generate stubs
             * which might need to be discarded, but are sensitive to shape regeneration.
             */
            if (cx->runtime->gcRegenShapes)
                mjit::ic::PurgeMICs(cx, script);
# endif
        }
    }
#endif
}

MathCache *
JSCompartment::allocMathCache(JSContext *cx)
{
    JS_ASSERT(!mathCache);
    mathCache = js_new<MathCache>();
    if (!mathCache)
        js_ReportOutOfMemory(cx);
    return mathCache;
}

size_t
JSCompartment::backEdgeCount(jsbytecode *pc) const
{
    if (BackEdgeMap::Ptr p = backEdgeTable.lookup(pc))
        return p->value;

    return 0;
}

size_t
JSCompartment::incBackEdgeCount(jsbytecode *pc)
{
    if (BackEdgeMap::AddPtr p = backEdgeTable.lookupForAdd(pc)) {
        p->value++;
        return p->value;
    } else {
        backEdgeTable.add(p, pc, 1);
        return 1;
    }
}

