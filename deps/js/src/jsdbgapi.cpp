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
 * JS debugging API.
 */
#include <string.h>
#include "jsprvtd.h"
#include "jstypes.h"
#include "jsstdint.h"
#include "jsutil.h"
#include "jsclist.h"
#include "jshashtable.h"
#include "jsapi.h"
#include "jscntxt.h"
#include "jsversion.h"
#include "jsdbgapi.h"
#include "jsemit.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsinterp.h"
#include "jslock.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jsparse.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsstaticcheck.h"
#include "jsstr.h"
#include "jswrapper.h"

#include "jsatominlines.h"
#include "jsdbgapiinlines.h"
#include "jsinterpinlines.h"
#include "jsobjinlines.h"
#include "jsscopeinlines.h"
#include "jsscriptinlines.h"

#include "jsautooplen.h"

#include "methodjit/MethodJIT.h"
#include "methodjit/Retcon.h"

using namespace js;
using namespace js::gc;

typedef struct JSTrap {
    JSCList         links;
    JSScript        *script;
    jsbytecode      *pc;
    JSOp            op;
    JSTrapHandler   handler;
    jsval           closure;
} JSTrap;

#define DBG_LOCK(rt)            JS_ACQUIRE_LOCK((rt)->debuggerLock)
#define DBG_UNLOCK(rt)          JS_RELEASE_LOCK((rt)->debuggerLock)
#define DBG_LOCK_EVAL(rt,expr)  (DBG_LOCK(rt), (expr), DBG_UNLOCK(rt))

JS_PUBLIC_API(JSBool)
JS_GetDebugMode(JSContext *cx)
{
    return cx->compartment->debugMode;
}

JS_PUBLIC_API(JSBool)
JS_SetDebugMode(JSContext *cx, JSBool debug)
{
    return JS_SetDebugModeForCompartment(cx, cx->compartment, debug);
}

JS_PUBLIC_API(void)
JS_SetRuntimeDebugMode(JSRuntime *rt, JSBool debug)
{
    rt->debugMode = debug;
}

#ifdef DEBUG
static bool
CompartmentHasLiveScripts(JSCompartment *comp)
{
#ifdef JS_METHODJIT
# ifdef JS_THREADSAFE
    jsword currentThreadId = reinterpret_cast<jsword>(js_CurrentThreadId());
# endif
#endif

    // Unsynchronized context iteration is technically a race; but this is only
    // for debug asserts where such a race would be rare
    JSContext *iter = NULL;
    JSContext *icx;
    while ((icx = JS_ContextIterator(comp->rt, &iter))) {
#ifdef JS_THREADSAFE
        if (JS_GetContextThread(icx) != currentThreadId)
            continue;
#endif
        for (AllFramesIter i(icx); !i.done(); ++i) {
            JSScript *script = i.fp()->maybeScript();
            if (script && script->compartment == comp)
                return JS_TRUE;
        }
    }

    return JS_FALSE;
}
#endif

JS_FRIEND_API(JSBool)
JS_SetDebugModeForCompartment(JSContext *cx, JSCompartment *comp, JSBool debug)
{
    if (comp->debugMode == !!debug)
        return JS_TRUE;

    // This should only be called when no scripts are live. It would even be
    // incorrect to discard just the non-live scripts' JITScripts because they
    // might share ICs with live scripts (bug 632343).
    JS_ASSERT(!CompartmentHasLiveScripts(comp));

    // All scripts compiled from this point on should be in the requested debugMode.
    comp->debugMode = !!debug;

    // Discard JIT code for any scripts that change debugMode. This function
    // assumes that 'comp' is in the same thread as 'cx'.

#ifdef JS_METHODJIT
    JS::AutoEnterScriptCompartment ac;

    for (JSScript *script = (JSScript *)comp->scripts.next;
         &script->links != &comp->scripts;
         script = (JSScript *)script->links.next)
    {
        if (!script->debugMode == !debug)
            continue;

        /*
         * If compartment entry fails, debug mode is left partially on, leading
         * to a small performance overhead but no loss of correctness. We set
         * the debug flags to false so that the caller will not later attempt
         * to use debugging features.
         */
        if (!ac.entered() && !ac.enter(cx, script)) {
            comp->debugMode = JS_FALSE;
            return JS_FALSE;
        }

        mjit::ReleaseScriptCode(cx, script);
        script->debugMode = !!debug;
    }
#endif

    return JS_TRUE;
}

JS_FRIEND_API(JSBool)
js_SetSingleStepMode(JSContext *cx, JSScript *script, JSBool singleStep)
{
#ifdef JS_METHODJIT
    if (!script->singleStepMode == !singleStep)
        return JS_TRUE;
#endif

    JS_ASSERT_IF(singleStep, cx->compartment->debugMode);

#ifdef JS_METHODJIT
    /* request the next recompile to inject single step interrupts */
    script->singleStepMode = !!singleStep;

    js::mjit::JITScript *jit = script->jitNormal ? script->jitNormal : script->jitCtor;
    if (jit && script->singleStepMode != jit->singleStepMode) {
        js::mjit::Recompiler recompiler(cx, script);
        if (!recompiler.recompile()) {
            script->singleStepMode = !singleStep;
            return JS_FALSE;
        }
    }
#endif
    return JS_TRUE;
}

static JSBool
CheckDebugMode(JSContext *cx)
{
    JSBool debugMode = JS_GetDebugMode(cx);
    /*
     * :TODO:
     * This probably should be an assertion, since it's indicative of a severe
     * API misuse.
     */
    if (!debugMode) {
        JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR, js_GetErrorMessage,
                                     NULL, JSMSG_NEED_DEBUG_MODE);
    }
    return debugMode;
}

JS_PUBLIC_API(JSBool)
JS_SetSingleStepMode(JSContext *cx, JSScript *script, JSBool singleStep)
{
    if (!CheckDebugMode(cx))
        return JS_FALSE;

    return js_SetSingleStepMode(cx, script, singleStep);
}

/*
 * NB: FindTrap must be called with rt->debuggerLock acquired.
 */
static JSTrap *
FindTrap(JSRuntime *rt, JSScript *script, jsbytecode *pc)
{
    JSTrap *trap;

    for (trap = (JSTrap *)rt->trapList.next;
         &trap->links != &rt->trapList;
         trap = (JSTrap *)trap->links.next) {
        if (trap->script == script && trap->pc == pc)
            return trap;
    }
    return NULL;
}

jsbytecode *
js_UntrapScriptCode(JSContext *cx, JSScript *script)
{
    jsbytecode *code;
    JSRuntime *rt;
    JSTrap *trap;

    code = script->code;
    rt = cx->runtime;
    DBG_LOCK(rt);
    for (trap = (JSTrap *)rt->trapList.next;
         &trap->links !=
                &rt->trapList;
         trap = (JSTrap *)trap->links.next) {
        if (trap->script == script &&
            (size_t)(trap->pc - script->code) < script->length) {
            if (code == script->code) {
                jssrcnote *sn, *notes;
                size_t nbytes;

                nbytes = script->length * sizeof(jsbytecode);
                notes = script->notes();
                for (sn = notes; !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn))
                    continue;
                nbytes += (sn - notes + 1) * sizeof *sn;

                code = (jsbytecode *) cx->malloc(nbytes);
                if (!code)
                    break;
                memcpy(code, script->code, nbytes);
                JS_PURGE_GSN_CACHE(cx);
            }
            code[trap->pc - script->code] = trap->op;
        }
    }
    DBG_UNLOCK(rt);
    return code;
}

JS_PUBLIC_API(JSBool)
JS_SetTrap(JSContext *cx, JSScript *script, jsbytecode *pc,
           JSTrapHandler handler, jsval closure)
{
    JSTrap *junk, *trap, *twin;
    JSRuntime *rt;
    uint32 sample;

    if (!CheckDebugMode(cx))
        return JS_FALSE;

    JS_ASSERT((JSOp) *pc != JSOP_TRAP);
    junk = NULL;
    rt = cx->runtime;
    DBG_LOCK(rt);
    trap = FindTrap(rt, script, pc);
    if (trap) {
        JS_ASSERT(trap->script == script && trap->pc == pc);
        JS_ASSERT(*pc == JSOP_TRAP);
    } else {
        sample = rt->debuggerMutations;
        DBG_UNLOCK(rt);
        trap = (JSTrap *) cx->malloc(sizeof *trap);
        if (!trap)
            return JS_FALSE;
        trap->closure = JSVAL_NULL;
        DBG_LOCK(rt);
        twin = (rt->debuggerMutations != sample)
               ? FindTrap(rt, script, pc)
               : NULL;
        if (twin) {
            junk = trap;
            trap = twin;
        } else {
            JS_APPEND_LINK(&trap->links, &rt->trapList);
            ++rt->debuggerMutations;
            trap->script = script;
            trap->pc = pc;
            trap->op = (JSOp)*pc;
            *pc = JSOP_TRAP;
        }
    }
    trap->handler = handler;
    trap->closure = closure;
    DBG_UNLOCK(rt);
    if (junk)
        cx->free(junk);

#ifdef JS_METHODJIT
    if (script->hasJITCode()) {
        js::mjit::Recompiler recompiler(cx, script);
        if (!recompiler.recompile())
            return JS_FALSE;
    }
#endif

    return JS_TRUE;
}

JS_PUBLIC_API(JSOp)
JS_GetTrapOpcode(JSContext *cx, JSScript *script, jsbytecode *pc)
{
    JSRuntime *rt;
    JSTrap *trap;
    JSOp op;

    rt = cx->runtime;
    DBG_LOCK(rt);
    trap = FindTrap(rt, script, pc);
    op = trap ? trap->op : (JSOp) *pc;
    DBG_UNLOCK(rt);
    return op;
}

static void
DestroyTrapAndUnlock(JSContext *cx, JSTrap *trap)
{
    ++cx->runtime->debuggerMutations;
    JS_REMOVE_LINK(&trap->links);
    *trap->pc = (jsbytecode)trap->op;
    DBG_UNLOCK(cx->runtime);
    cx->free(trap);
}

JS_PUBLIC_API(void)
JS_ClearTrap(JSContext *cx, JSScript *script, jsbytecode *pc,
             JSTrapHandler *handlerp, jsval *closurep)
{
    JSTrap *trap;
    
    DBG_LOCK(cx->runtime);
    trap = FindTrap(cx->runtime, script, pc);
    if (handlerp)
        *handlerp = trap ? trap->handler : NULL;
    if (closurep)
        *closurep = trap ? trap->closure : JSVAL_NULL;
    if (trap)
        DestroyTrapAndUnlock(cx, trap);
    else
        DBG_UNLOCK(cx->runtime);

#ifdef JS_METHODJIT
    if (script->hasJITCode()) {
        mjit::Recompiler recompiler(cx, script);
        recompiler.recompile();
    }
#endif
}

JS_PUBLIC_API(void)
JS_ClearScriptTraps(JSContext *cx, JSScript *script)
{
    JSRuntime *rt;
    JSTrap *trap, *next;
    uint32 sample;

    rt = cx->runtime;
    DBG_LOCK(rt);
    for (trap = (JSTrap *)rt->trapList.next;
         &trap->links != &rt->trapList;
         trap = next) {
        next = (JSTrap *)trap->links.next;
        if (trap->script == script) {
            sample = rt->debuggerMutations;
            DestroyTrapAndUnlock(cx, trap);
            DBG_LOCK(rt);
            if (rt->debuggerMutations != sample + 1)
                next = (JSTrap *)rt->trapList.next;
        }
    }
    DBG_UNLOCK(rt);
}

JS_PUBLIC_API(void)
JS_ClearAllTraps(JSContext *cx)
{
    JSRuntime *rt;
    JSTrap *trap, *next;
    uint32 sample;

    rt = cx->runtime;
    DBG_LOCK(rt);
    for (trap = (JSTrap *)rt->trapList.next;
         &trap->links != &rt->trapList;
         trap = next) {
        next = (JSTrap *)trap->links.next;
        sample = rt->debuggerMutations;
        DestroyTrapAndUnlock(cx, trap);
        DBG_LOCK(rt);
        if (rt->debuggerMutations != sample + 1)
            next = (JSTrap *)rt->trapList.next;
    }
    DBG_UNLOCK(rt);
}

/*
 * NB: js_MarkTraps does not acquire cx->runtime->debuggerLock, since the
 * debugger should never be racing with the GC (i.e., the debugger must
 * respect the request model).
 */
void
js_MarkTraps(JSTracer *trc)
{
    JSRuntime *rt = trc->context->runtime;

    for (JSTrap *trap = (JSTrap *) rt->trapList.next;
         &trap->links != &rt->trapList;
         trap = (JSTrap *) trap->links.next) {
        MarkValue(trc, Valueify(trap->closure), "trap->closure");
    }
}

JS_PUBLIC_API(JSTrapStatus)
JS_HandleTrap(JSContext *cx, JSScript *script, jsbytecode *pc, jsval *rval)
{
    JSTrap *trap;
    jsint op;
    JSTrapStatus status;

    DBG_LOCK(cx->runtime);
    trap = FindTrap(cx->runtime, script, pc);
    JS_ASSERT(!trap || trap->handler);
    if (!trap) {
        op = (JSOp) *pc;
        DBG_UNLOCK(cx->runtime);

        /* Defend against "pc for wrong script" API usage error. */
        JS_ASSERT(op != JSOP_TRAP);

#ifdef JS_THREADSAFE
        /* If the API was abused, we must fail for want of the real op. */
        if (op == JSOP_TRAP)
            return JSTRAP_ERROR;

        /* Assume a race with a debugger thread and try to carry on. */
        *rval = INT_TO_JSVAL(op);
        return JSTRAP_CONTINUE;
#else
        /* Always fail if single-threaded (must be an API usage error). */
        return JSTRAP_ERROR;
#endif
    }
    DBG_UNLOCK(cx->runtime);

    /*
     * It's important that we not use 'trap->' after calling the callback --
     * the callback might remove the trap!
     */
    op = (jsint)trap->op;
    status = trap->handler(cx, script, pc, rval, trap->closure);
    if (status == JSTRAP_CONTINUE) {
        /* By convention, return the true op to the interpreter in rval. */
        *rval = INT_TO_JSVAL(op);
    }
    return status;
}

#ifdef JS_TRACER
static void
JITInhibitingHookChange(JSRuntime *rt, bool wasInhibited)
{
    if (wasInhibited) {
        if (!rt->debuggerInhibitsJIT()) {
            for (JSCList *cl = rt->contextList.next; cl != &rt->contextList; cl = cl->next)
                js_ContextFromLinkField(cl)->updateJITEnabled();
        }
    } else if (rt->debuggerInhibitsJIT()) {
        for (JSCList *cl = rt->contextList.next; cl != &rt->contextList; cl = cl->next)
            js_ContextFromLinkField(cl)->traceJitEnabled = false;
    }
}
#endif

JS_PUBLIC_API(JSBool)
JS_SetInterrupt(JSRuntime *rt, JSInterruptHook hook, void *closure)
{
#ifdef JS_TRACER
    {
        AutoLockGC lock(rt);
        bool wasInhibited = rt->debuggerInhibitsJIT();
#endif
        rt->globalDebugHooks.interruptHook = hook;
        rt->globalDebugHooks.interruptHookData = closure;
#ifdef JS_TRACER
        JITInhibitingHookChange(rt, wasInhibited);
    }
#endif
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_ClearInterrupt(JSRuntime *rt, JSInterruptHook *hoop, void **closurep)
{
#ifdef JS_TRACER
    AutoLockGC lock(rt);
    bool wasInhibited = rt->debuggerInhibitsJIT();
#endif
    if (hoop)
        *hoop = rt->globalDebugHooks.interruptHook;
    if (closurep)
        *closurep = rt->globalDebugHooks.interruptHookData;
    rt->globalDebugHooks.interruptHook = 0;
    rt->globalDebugHooks.interruptHookData = 0;
#ifdef JS_TRACER
    JITInhibitingHookChange(rt, wasInhibited);
#endif
    return JS_TRUE;
}

/************************************************************************/

struct JSWatchPoint {
    JSCList             links;
    JSObject            *object;        /* weak link, see js_SweepWatchPoints */
    const Shape         *shape;
    StrictPropertyOp    setter;
    JSWatchPointHandler handler;
    JSObject            *closure;
    uintN               flags;
};

#define JSWP_LIVE       0x1             /* live because set and not cleared */
#define JSWP_HELD       0x2             /* held while running handler/setter */

/*
 * NB: DropWatchPointAndUnlock releases cx->runtime->debuggerLock in all cases.
 */
static JSBool
DropWatchPointAndUnlock(JSContext *cx, JSWatchPoint *wp, uintN flag)
{
    bool ok = true;
    JSRuntime *rt = cx->runtime;

    wp->flags &= ~flag;
    if (wp->flags != 0) {
        DBG_UNLOCK(rt);
        return ok;
    }

    /*
     * Switch to the same compartment as the watch point, since changeProperty, below,
     * needs to have a compartment.
     */
    SwitchToCompartment sc(cx, wp->object);

    /* Remove wp from the list, then restore wp->shape->setter from wp. */
    ++rt->debuggerMutations;
    JS_REMOVE_LINK(&wp->links);
    DBG_UNLOCK(rt);

    /*
     * If the property isn't found on wp->object, then someone else must have deleted it,
     * and we don't need to change the property attributes.
     */
    const Shape *shape = wp->shape;
    const Shape *wprop = wp->object->nativeLookup(shape->id);
    if (wprop &&
        wprop->hasSetterValue() == shape->hasSetterValue() &&
        IsWatchedProperty(cx, wprop)) {
        shape = wp->object->changeProperty(cx, wprop, 0, wprop->attributes(),
                                           wprop->getter(), wp->setter);
        if (!shape)
            ok = false;
    }

    cx->free(wp);
    return ok;
}

/*
 * NB: js_TraceWatchPoints does not acquire cx->runtime->debuggerLock, since
 * the debugger should never be racing with the GC (i.e., the debugger must
 * respect the request model).
 */
void
js_TraceWatchPoints(JSTracer *trc, JSObject *obj)
{
    JSRuntime *rt;
    JSWatchPoint *wp;

    rt = trc->context->runtime;

    for (wp = (JSWatchPoint *)rt->watchPointList.next;
         &wp->links != &rt->watchPointList;
         wp = (JSWatchPoint *)wp->links.next) {
        if (wp->object == obj) {
            wp->shape->trace(trc);
            if (wp->shape->hasSetterValue() && wp->setter)
                MarkObject(trc, *CastAsObject(wp->setter), "wp->setter");
            MarkObject(trc, *wp->closure, "wp->closure");
        }
    }
}

void
js_SweepWatchPoints(JSContext *cx)
{
    JSRuntime *rt;
    JSWatchPoint *wp, *next;
    uint32 sample;

    rt = cx->runtime;
    DBG_LOCK(rt);
    for (wp = (JSWatchPoint *)rt->watchPointList.next;
         &wp->links != &rt->watchPointList;
         wp = next) {
        next = (JSWatchPoint *)wp->links.next;
        if (IsAboutToBeFinalized(cx, wp->object)) {
            sample = rt->debuggerMutations;

            /* Ignore failures. */
            DropWatchPointAndUnlock(cx, wp, JSWP_LIVE);
            DBG_LOCK(rt);
            if (rt->debuggerMutations != sample + 1)
                next = (JSWatchPoint *)rt->watchPointList.next;
        }
    }
    DBG_UNLOCK(rt);
}



/*
 * NB: LockedFindWatchPoint must be called with rt->debuggerLock acquired.
 */
static JSWatchPoint *
LockedFindWatchPoint(JSRuntime *rt, JSObject *obj, jsid id)
{
    JSWatchPoint *wp;

    for (wp = (JSWatchPoint *)rt->watchPointList.next;
         &wp->links != &rt->watchPointList;
         wp = (JSWatchPoint *)wp->links.next) {
        if (wp->object == obj && wp->shape->id == id)
            return wp;
    }
    return NULL;
}

static JSWatchPoint *
FindWatchPoint(JSRuntime *rt, JSObject *obj, jsid id)
{
    JSWatchPoint *wp;

    DBG_LOCK(rt);
    wp = LockedFindWatchPoint(rt, obj, id);
    DBG_UNLOCK(rt);
    return wp;
}

JSBool
js_watch_set(JSContext *cx, JSObject *obj, jsid id, JSBool strict, Value *vp)
{
    JSRuntime *rt = cx->runtime;
    DBG_LOCK(rt);
    for (JSWatchPoint *wp = (JSWatchPoint *)rt->watchPointList.next;
         &wp->links != &rt->watchPointList;
         wp = (JSWatchPoint *)wp->links.next) {
        const Shape *shape = wp->shape;
        if (wp->object == obj && SHAPE_USERID(shape) == id && !(wp->flags & JSWP_HELD)) {
            wp->flags |= JSWP_HELD;
            DBG_UNLOCK(rt);

            jsid propid = shape->id;
            shape = obj->nativeLookup(propid);
            JS_ASSERT(IsWatchedProperty(cx, shape));
            jsid userid = SHAPE_USERID(shape);

            /* Determine the property's old value. */
            bool ok;
            uint32 slot = shape->slot;
            Value old = obj->containsSlot(slot) ? obj->nativeGetSlot(slot) : UndefinedValue();
            const Shape *needMethodSlotWrite = NULL;
            if (shape->isMethod()) {
                /*
                 * We get here in two cases: (1) the existing watched property
                 * is a method; or (2) the watched property was deleted and is
                 * now in the middle of being re-added via JSOP_SETMETHOD. In
                 * both cases we must trip the method read barrier in order to
                 * avoid passing an uncloned function object to the handler.
                 *
                 * Case 2 is especially hairy. js_watch_set, uniquely, gets
                 * called in the middle of creating a method property, after
                 * shape is in obj but before the slot has been set. So in this
                 * case we must finish initializing the half-finished method
                 * property before triggering the method read barrier.
                 *
                 * Bonus weirdness: because this changes obj's shape,
                 * js_NativeSet (which is our caller) will not write to the
                 * slot, as it will appear the property was deleted and a new
                 * property added. We must write the slot ourselves -- however
                 * we must do it after calling the watchpoint handler. So set
                 * needMethodSlotWrite here and use it to write to the slot
                 * below, if the handler does not tinker with the property
                 * further.
                 */
                JS_ASSERT(!wp->setter);
                Value method = ObjectValue(shape->methodObject());
                if (old.isUndefined())
                    obj->nativeSetSlot(slot, method);
                ok = obj->methodReadBarrier(cx, *shape, &method);
                if (!ok)
                    goto out;
                wp->shape = shape = needMethodSlotWrite = obj->nativeLookup(propid);
                JS_ASSERT(shape->isDataDescriptor());
                JS_ASSERT(!shape->isMethod());
                if (old.isUndefined())
                    obj->nativeSetSlot(shape->slot, old);
                else
                    old = method;
            }

            {
                Conditionally<AutoShapeRooter> tvr(needMethodSlotWrite, cx, needMethodSlotWrite);

                /*
                 * Call the handler. This invalidates shape, so re-lookup the shape.
                 * NB: wp is held, so we can safely dereference it still.
                 */
                ok = wp->handler(cx, obj, propid, Jsvalify(old), Jsvalify(vp), wp->closure);
                if (!ok)
                    goto out;
                shape = obj->nativeLookup(propid);

                if (!shape) {
                    ok = true;
                } else if (wp->setter) {
                    /*
                     * Pass the output of the handler to the setter. Security wrappers
                     * prevent any funny business between watchpoints and setters.
                     */
                    ok = shape->hasSetterValue()
                         ? ExternalInvoke(cx, ObjectValue(*obj),
                                          ObjectValue(*CastAsObject(wp->setter)),
                                          1, vp, vp)
                         : CallJSPropertyOpSetter(cx, wp->setter, obj, userid, strict, vp);
                } else if (shape == needMethodSlotWrite) {
                    /* See comment above about needMethodSlotWrite. */
                    obj->nativeSetSlot(shape->slot, *vp);
                    ok = true;
                } else {
                    /*
                     * A property with the default setter might be either a method
                     * or an ordinary function-valued data property subject to the
                     * method write barrier.
                     *
                     * It is not the setter's job to call methodWriteBarrier,
                     * but js_watch_set must do so, because the caller will be
                     * fooled into not doing it: shape does *not* have the
                     * default setter and therefore seems not to be a method.
                     */
                    ok = obj->methodWriteBarrier(cx, *shape, *vp) != NULL;
                }
            }

        out:
            DBG_LOCK(rt);
            return DropWatchPointAndUnlock(cx, wp, JSWP_HELD) && ok;
        }
    }
    DBG_UNLOCK(rt);
    return true;
}

static JSBool
js_watch_set_wrapper(JSContext *cx, uintN argc, Value *vp)
{
    JSObject *obj = ToObject(cx, &vp[1]);
    if (!obj)
        return false;

    JSObject &funobj = JS_CALLEE(cx, vp).toObject();
    JSFunction *wrapper = funobj.getFunctionPrivate();
    jsid userid = ATOM_TO_JSID(wrapper->atom);

    JS_SET_RVAL(cx, vp, argc ? JS_ARGV(cx, vp)[0] : UndefinedValue());
    /*
     * The strictness we pass here doesn't matter, since we know that it's
     * a JS setter, which can't depend on the assigning code's strictness.
     */
    return js_watch_set(cx, obj, userid, false, vp);
}

namespace js {

bool
IsWatchedProperty(JSContext *cx, const Shape *shape)
{
    if (shape->hasSetterValue()) {
        JSObject *funobj = shape->setterObject();
        if (!funobj || !funobj->isFunction())
            return false;

        JSFunction *fun = funobj->getFunctionPrivate();
        return fun->maybeNative() == js_watch_set_wrapper;
    }
    return shape->setterOp() == js_watch_set;
}

}

/*
 * Return an appropriate setter to substitute for |setter| on a property
 * with attributes |attrs|, to implement a watchpoint on the property named
 * |id|.
 */
static StrictPropertyOp
WrapWatchedSetter(JSContext *cx, jsid id, uintN attrs, StrictPropertyOp setter)
{
    JSAtom *atom;
    JSFunction *wrapper;

    /* Wrap a C++ setter simply by returning our own C++ setter. */
    if (!(attrs & JSPROP_SETTER))
        return &js_watch_set;   /* & to silence schoolmarmish MSVC */

    /*
     * Wrap a JSObject * setter by constructing our own JSFunction * that saves the
     * property id as the function name, and calls js_watch_set.
     */
    if (JSID_IS_ATOM(id)) {
        atom = JSID_TO_ATOM(id);
    } else if (JSID_IS_INT(id)) {
        if (!js_ValueToStringId(cx, IdToValue(id), &id))
            return NULL;
        atom = JSID_TO_ATOM(id);
    } else {
        atom = NULL;
    }

    wrapper = js_NewFunction(cx, NULL, js_watch_set_wrapper, 1, 0,
                             setter ? CastAsObject(setter)->getParent() : NULL, atom);
    if (!wrapper)
        return NULL;
    return CastAsStrictPropertyOp(FUN_OBJECT(wrapper));
}

static const Shape *
UpdateWatchpointShape(JSContext *cx, JSWatchPoint *wp, const Shape *newShape)
{
    JS_ASSERT_IF(wp->shape, wp->shape->id == newShape->id);
    JS_ASSERT(!IsWatchedProperty(cx, newShape));

    /* Create a watching setter we can substitute for the new shape's setter. */
    StrictPropertyOp watchingSetter =
        WrapWatchedSetter(cx, newShape->id, newShape->attributes(), newShape->setter());
    if (!watchingSetter)
        return NULL;

    /*
     * Save the shape's setter; we don't know whether js_ChangeNativePropertyAttrs will
     * return a new shape, or mutate this one.
     */
    StrictPropertyOp originalSetter = newShape->setter();

    /*
     * Drop the watching setter into the object, in place of newShape. Note that a single
     * watchpoint-wrapped shape may correspond to more than one non-watchpoint shape: we
     * wrap all (JSPropertyOp, not JSObject *) setters with js_watch_set, so shapes that
     * differ only in their setter may all get wrapped to the same shape.
     */
    const Shape *watchingShape = 
        js_ChangeNativePropertyAttrs(cx, wp->object, newShape, 0, newShape->attributes(),
                                     newShape->getter(), watchingSetter);
    if (!watchingShape)
        return NULL;

    /* Update the watchpoint with the new shape and its original setter. */
    wp->setter = originalSetter;
    wp->shape = watchingShape;

    return watchingShape;
}

const Shape *
js_SlowPathUpdateWatchpointsForShape(JSContext *cx, JSObject *obj, const Shape *newShape)
{
    /*
     * The watchpoint code uses the normal property-modification functions to install its
     * own watchpoint-aware shapes. Those functions report those changes back to the
     * watchpoint code, just as they do user-level changes. So if this change is
     * installing a watchpoint-aware shape, it's something we asked for ourselves, and can
     * proceed without interference.
     */
    if (IsWatchedProperty(cx, newShape))
        return newShape;

    JSWatchPoint *wp = FindWatchPoint(cx->runtime, obj, newShape->id);
    if (!wp)
        return newShape;

    return UpdateWatchpointShape(cx, wp, newShape);
}

/*
 * Return the underlying setter for |shape| on |obj|, seeing through any
 * watchpoint-wrapping. Note that we need |obj| to disambiguate, since a single
 * watchpoint-wrapped shape may correspond to more than one non-watchpoint shape; see the
 * comments in UpdateWatchpointShape.
 */
static StrictPropertyOp
UnwrapSetter(JSContext *cx, JSObject *obj, const Shape *shape)
{
    /* If it's not a watched property, its setter is not wrapped. */
    if (!IsWatchedProperty(cx, shape))
        return shape->setter();

    /* Look up the watchpoint, from which we can retrieve the underlying setter. */
    JSWatchPoint *wp = FindWatchPoint(cx->runtime, obj, shape->id);

    /* 
     * Since we know |shape| is watched, we *must* find a watchpoint: we should never
     * leave wrapped setters lying around in shapes after removing a watchpoint.
     */
    JS_ASSERT(wp);

    return wp->setter;
}

JS_PUBLIC_API(JSBool)
JS_SetWatchPoint(JSContext *cx, JSObject *obj, jsid id,
                 JSWatchPointHandler handler, JSObject *closure)
{
    JSObject *origobj;
    Value v;
    uintN attrs;
    jsid propid;

    origobj = obj;
    OBJ_TO_INNER_OBJECT(cx, obj);
    if (!obj)
        return JS_FALSE;

    AutoValueRooter idroot(cx);
    if (JSID_IS_INT(id)) {
        propid = id;
    } else {
        if (!js_ValueToStringId(cx, IdToValue(id), &propid))
            return JS_FALSE;
        propid = js_CheckForStringIndex(propid);
        idroot.set(IdToValue(propid));
    }

    /*
     * If, by unwrapping and innerizing, we changed the object, check
     * again to make sure that we're allowed to set a watch point.
     */
    if (origobj != obj && !CheckAccess(cx, obj, propid, JSACC_WATCH, &v, &attrs))
        return JS_FALSE;

    if (!obj->isNative()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_WATCH,
                             obj->getClass()->name);
        return JS_FALSE;
    }

    JSObject *pobj;
    JSProperty *prop;
    if (!js_LookupProperty(cx, obj, propid, &pobj, &prop))
        return JS_FALSE;
    const Shape *shape = (Shape *) prop;
    JSRuntime *rt = cx->runtime;
    if (!shape) {
        /* Check for a deleted symbol watchpoint, which holds its property. */
        JSWatchPoint *wp = FindWatchPoint(rt, obj, propid);
        if (!wp) {
            /* Make a new property in obj so we can watch for the first set. */
            if (!js_DefineNativeProperty(cx, obj, propid, UndefinedValue(), NULL, NULL,
                                         JSPROP_ENUMERATE, 0, 0, &prop)) {
                return JS_FALSE;
            }
            shape = (Shape *) prop;
        }
    } else if (pobj != obj) {
        /* Clone the prototype property so we can watch the right object. */
        AutoValueRooter valroot(cx);
        PropertyOp getter;
        StrictPropertyOp setter;
        uintN attrs, flags;
        intN shortid;

        if (pobj->isNative()) {
            valroot.set(pobj->containsSlot(shape->slot)
                        ? pobj->nativeGetSlot(shape->slot)
                        : UndefinedValue());
            getter = shape->getter();
            setter = UnwrapSetter(cx, pobj, shape);
            attrs = shape->attributes();
            flags = shape->getFlags();
            shortid = shape->shortid;
        } else {
            if (!pobj->getProperty(cx, propid, valroot.addr()) ||
                !pobj->getAttributes(cx, propid, &attrs)) {
                return JS_FALSE;
            }
            getter = NULL;
            setter = NULL;
            flags = 0;
            shortid = 0;
        }

        /* Recall that obj is native, whether or not pobj is native. */
        if (!js_DefineNativeProperty(cx, obj, propid, valroot.value(),
                                     getter, setter, attrs, flags,
                                     shortid, &prop)) {
            return JS_FALSE;
        }
        shape = (Shape *) prop;
    }

    /*
     * At this point, prop/shape exists in obj, obj is locked, and we must
     * unlock the object before returning.
     */
    DBG_LOCK(rt);
    JSWatchPoint *wp = LockedFindWatchPoint(rt, obj, propid);
    if (!wp) {
        DBG_UNLOCK(rt);
        wp = (JSWatchPoint *) cx->malloc(sizeof *wp);
        if (!wp)
            return JS_FALSE;
        wp->handler = NULL;
        wp->closure = NULL;
        wp->object = obj;
        wp->shape = NULL;
        wp->flags = JSWP_LIVE;

        /* XXXbe nest in obj lock here */
        if (!UpdateWatchpointShape(cx, wp, shape)) {
            /* Self-link so DropWatchPointAndUnlock can JS_REMOVE_LINK it. */
            JS_INIT_CLIST(&wp->links);
            DBG_LOCK(rt);
            DropWatchPointAndUnlock(cx, wp, JSWP_LIVE);
            return JS_FALSE;
        }

        /*
         * Now that wp is fully initialized, append it to rt's wp list.
         * Because obj is locked we know that no other thread could have added
         * a watchpoint for (obj, propid).
         */
        DBG_LOCK(rt);
        JS_ASSERT(!LockedFindWatchPoint(rt, obj, propid));
        JS_APPEND_LINK(&wp->links, &rt->watchPointList);
        ++rt->debuggerMutations;
    }

    /*
     * Ensure that an object with watchpoints never has the same shape as an
     * object without them, even if the watched properties are deleted.
     */
    obj->watchpointOwnShapeChange(cx);

    wp->handler = handler;
    wp->closure = reinterpret_cast<JSObject*>(closure);
    DBG_UNLOCK(rt);
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_ClearWatchPoint(JSContext *cx, JSObject *obj, jsid id,
                   JSWatchPointHandler *handlerp, JSObject **closurep)
{
    JSRuntime *rt;
    JSWatchPoint *wp;

    rt = cx->runtime;
    DBG_LOCK(rt);
    for (wp = (JSWatchPoint *)rt->watchPointList.next;
         &wp->links != &rt->watchPointList;
         wp = (JSWatchPoint *)wp->links.next) {
        if (wp->object == obj && SHAPE_USERID(wp->shape) == id) {
            if (handlerp)
                *handlerp = wp->handler;
            if (closurep)
                *closurep = wp->closure;
            return DropWatchPointAndUnlock(cx, wp, JSWP_LIVE);
        }
    }
    DBG_UNLOCK(rt);
    if (handlerp)
        *handlerp = NULL;
    if (closurep)
        *closurep = NULL;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_ClearWatchPointsForObject(JSContext *cx, JSObject *obj)
{
    JSRuntime *rt;
    JSWatchPoint *wp, *next;
    uint32 sample;

    rt = cx->runtime;
    DBG_LOCK(rt);
    for (wp = (JSWatchPoint *)rt->watchPointList.next;
         &wp->links != &rt->watchPointList;
         wp = next) {
        next = (JSWatchPoint *)wp->links.next;
        if (wp->object == obj) {
            sample = rt->debuggerMutations;
            if (!DropWatchPointAndUnlock(cx, wp, JSWP_LIVE))
                return JS_FALSE;
            DBG_LOCK(rt);
            if (rt->debuggerMutations != sample + 1)
                next = (JSWatchPoint *)rt->watchPointList.next;
        }
    }
    DBG_UNLOCK(rt);
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_ClearAllWatchPoints(JSContext *cx)
{
    JSRuntime *rt;
    JSWatchPoint *wp, *next;
    uint32 sample;

    rt = cx->runtime;
    DBG_LOCK(rt);
    for (wp = (JSWatchPoint *)rt->watchPointList.next;
         &wp->links != &rt->watchPointList;
         wp = next) {
        next = (JSWatchPoint *)wp->links.next;
        sample = rt->debuggerMutations;
        if (!DropWatchPointAndUnlock(cx, wp, JSWP_LIVE))
            return JS_FALSE;
        DBG_LOCK(rt);
        if (rt->debuggerMutations != sample + 1)
            next = (JSWatchPoint *)rt->watchPointList.next;
    }
    DBG_UNLOCK(rt);
    return JS_TRUE;
}

/************************************************************************/

JS_PUBLIC_API(uintN)
JS_PCToLineNumber(JSContext *cx, JSScript *script, jsbytecode *pc)
{
    return js_PCToLineNumber(cx, script, pc);
}

JS_PUBLIC_API(jsbytecode *)
JS_LineNumberToPC(JSContext *cx, JSScript *script, uintN lineno)
{
    return js_LineNumberToPC(script, lineno);
}

JS_PUBLIC_API(jsbytecode *)
JS_EndPC(JSContext *cx, JSScript *script)
{
    return script->code + script->length;
}

JS_PUBLIC_API(uintN)
JS_GetFunctionArgumentCount(JSContext *cx, JSFunction *fun)
{
    return fun->nargs;
}

JS_PUBLIC_API(JSBool)
JS_FunctionHasLocalNames(JSContext *cx, JSFunction *fun)
{
    return fun->script()->bindings.hasLocalNames();
}

extern JS_PUBLIC_API(jsuword *)
JS_GetFunctionLocalNameArray(JSContext *cx, JSFunction *fun, void **markp)
{
    *markp = JS_ARENA_MARK(&cx->tempPool);
    return fun->script()->bindings.getLocalNameArray(cx, &cx->tempPool);
}

extern JS_PUBLIC_API(JSAtom *)
JS_LocalNameToAtom(jsuword w)
{
    return JS_LOCAL_NAME_TO_ATOM(w);
}

extern JS_PUBLIC_API(JSString *)
JS_AtomKey(JSAtom *atom)
{
    return ATOM_TO_STRING(atom);
}

extern JS_PUBLIC_API(void)
JS_ReleaseFunctionLocalNameArray(JSContext *cx, void *mark)
{
    JS_ARENA_RELEASE(&cx->tempPool, mark);
}

JS_PUBLIC_API(JSScript *)
JS_GetFunctionScript(JSContext *cx, JSFunction *fun)
{
    return FUN_SCRIPT(fun);
}

JS_PUBLIC_API(JSNative)
JS_GetFunctionNative(JSContext *cx, JSFunction *fun)
{
    return Jsvalify(fun->maybeNative());
}

JS_PUBLIC_API(JSPrincipals *)
JS_GetScriptPrincipals(JSContext *cx, JSScript *script)
{
    return script->principals;
}

/************************************************************************/

/*
 *  Stack Frame Iterator
 */
JS_PUBLIC_API(JSStackFrame *)
JS_FrameIterator(JSContext *cx, JSStackFrame **iteratorp)
{
    *iteratorp = (*iteratorp == NULL) ? js_GetTopStackFrame(cx) : (*iteratorp)->prev();
    return *iteratorp;
}

JS_PUBLIC_API(JSScript *)
JS_GetFrameScript(JSContext *cx, JSStackFrame *fp)
{
    return fp->maybeScript();
}

JS_PUBLIC_API(jsbytecode *)
JS_GetFramePC(JSContext *cx, JSStackFrame *fp)
{
    return fp->pc(cx);
}

JS_PUBLIC_API(JSStackFrame *)
JS_GetScriptedCaller(JSContext *cx, JSStackFrame *fp)
{
    return js_GetScriptedCaller(cx, fp);
}

JSPrincipals *
js_StackFramePrincipals(JSContext *cx, JSStackFrame *fp)
{
    JSSecurityCallbacks *callbacks;

    if (fp->isFunctionFrame()) {
        callbacks = JS_GetSecurityCallbacks(cx);
        if (callbacks && callbacks->findObjectPrincipals) {
            if (&fp->fun()->compiledFunObj() != &fp->callee())
                return callbacks->findObjectPrincipals(cx, &fp->callee());
            /* FALL THROUGH */
        }
    }
    if (fp->isScriptFrame())
        return fp->script()->principals;
    return NULL;
}

JSPrincipals *
js_EvalFramePrincipals(JSContext *cx, JSObject *callee, JSStackFrame *caller)
{
    JSPrincipals *principals, *callerPrincipals;
    JSSecurityCallbacks *callbacks;

    callbacks = JS_GetSecurityCallbacks(cx);
    if (callbacks && callbacks->findObjectPrincipals)
        principals = callbacks->findObjectPrincipals(cx, callee);
    else
        principals = NULL;
    if (!caller)
        return principals;
    callerPrincipals = js_StackFramePrincipals(cx, caller);
    return (callerPrincipals && principals &&
            callerPrincipals->subsume(callerPrincipals, principals))
           ? principals
           : callerPrincipals;
}

JS_PUBLIC_API(void *)
JS_GetFrameAnnotation(JSContext *cx, JSStackFrame *fp)
{
    if (fp->annotation() && fp->isScriptFrame()) {
        JSPrincipals *principals = js_StackFramePrincipals(cx, fp);

        if (principals && principals->globalPrivilegesEnabled(cx, principals)) {
            /*
             * Give out an annotation only if privileges have not been revoked
             * or disabled globally.
             */
            return fp->annotation();
        }
    }

    return NULL;
}

JS_PUBLIC_API(void)
JS_SetFrameAnnotation(JSContext *cx, JSStackFrame *fp, void *annotation)
{
    fp->setAnnotation(annotation);
}

JS_PUBLIC_API(void *)
JS_GetFramePrincipalArray(JSContext *cx, JSStackFrame *fp)
{
    JSPrincipals *principals;

    principals = js_StackFramePrincipals(cx, fp);
    if (!principals)
        return NULL;
    return principals->getPrincipalArray(cx, principals);
}

JS_PUBLIC_API(JSBool)
JS_IsScriptFrame(JSContext *cx, JSStackFrame *fp)
{
    return !fp->isDummyFrame();
}

/* this is deprecated, use JS_GetFrameScopeChain instead */
JS_PUBLIC_API(JSObject *)
JS_GetFrameObject(JSContext *cx, JSStackFrame *fp)
{
    return &fp->scopeChain();
}

JS_PUBLIC_API(JSObject *)
JS_GetFrameScopeChain(JSContext *cx, JSStackFrame *fp)
{
    JS_ASSERT(cx->stack().contains(fp));

    js::AutoCompartment ac(cx, &fp->scopeChain());
    if (!ac.enter())
        return NULL;

    /* Force creation of argument and call objects if not yet created */
    (void) JS_GetFrameCallObject(cx, fp);
    return GetScopeChain(cx, fp);
}

JS_PUBLIC_API(JSObject *)
JS_GetFrameCallObject(JSContext *cx, JSStackFrame *fp)
{
    JS_ASSERT(cx->stack().contains(fp));

    if (!fp->isFunctionFrame())
        return NULL;

    js::AutoCompartment ac(cx, &fp->scopeChain());
    if (!ac.enter())
        return NULL;

    /* Force creation of argument object if not yet created */
    (void) js_GetArgsObject(cx, fp);

    /*
     * XXX ill-defined: null return here means error was reported, unlike a
     *     null returned above or in the #else
     */
    return js_GetCallObject(cx, fp);
}

JS_PUBLIC_API(JSBool)
JS_GetFrameThis(JSContext *cx, JSStackFrame *fp, jsval *thisv)
{
    if (fp->isDummyFrame())
        return false;

    js::AutoCompartment ac(cx, &fp->scopeChain());
    if (!ac.enter())
        return false;

    if (!fp->computeThis(cx))
        return false;
    *thisv = Jsvalify(fp->thisValue());
    return true;
}

JS_PUBLIC_API(JSFunction *)
JS_GetFrameFunction(JSContext *cx, JSStackFrame *fp)
{
    return fp->maybeFun();
}

JS_PUBLIC_API(JSObject *)
JS_GetFrameFunctionObject(JSContext *cx, JSStackFrame *fp)
{
    if (!fp->isFunctionFrame())
        return NULL;

    JS_ASSERT(fp->callee().isFunction());
    JS_ASSERT(fp->callee().getPrivate() == fp->fun());
    return &fp->callee();
}

JS_PUBLIC_API(JSBool)
JS_IsConstructorFrame(JSContext *cx, JSStackFrame *fp)
{
    return fp->isConstructing();
}

JS_PUBLIC_API(JSObject *)
JS_GetFrameCalleeObject(JSContext *cx, JSStackFrame *fp)
{
    return fp->maybeCallee();
}

JS_PUBLIC_API(JSBool)
JS_GetValidFrameCalleeObject(JSContext *cx, JSStackFrame *fp, jsval *vp)
{
    Value v;

    if (!fp->getValidCalleeObject(cx, &v))
        return false;
    *vp = Jsvalify(v);
    return true;
}

JS_PUBLIC_API(JSBool)
JS_IsDebuggerFrame(JSContext *cx, JSStackFrame *fp)
{
    return fp->isDebuggerFrame();
}

JS_PUBLIC_API(jsval)
JS_GetFrameReturnValue(JSContext *cx, JSStackFrame *fp)
{
    return Jsvalify(fp->returnValue());
}

JS_PUBLIC_API(void)
JS_SetFrameReturnValue(JSContext *cx, JSStackFrame *fp, jsval rval)
{
#ifdef JS_METHODJIT
    JS_ASSERT_IF(fp->isScriptFrame(), fp->script()->debugMode);
#endif
    assertSameCompartment(cx, fp, rval);
    fp->setReturnValue(Valueify(rval));
}

/************************************************************************/

JS_PUBLIC_API(const char *)
JS_GetScriptFilename(JSContext *cx, JSScript *script)
{
    return script->filename;
}

JS_PUBLIC_API(uintN)
JS_GetScriptBaseLineNumber(JSContext *cx, JSScript *script)
{
    return script->lineno;
}

JS_PUBLIC_API(uintN)
JS_GetScriptLineExtent(JSContext *cx, JSScript *script)
{
    return js_GetScriptLineExtent(script);
}

JS_PUBLIC_API(JSVersion)
JS_GetScriptVersion(JSContext *cx, JSScript *script)
{
    return VersionNumber(script->getVersion());
}

/***************************************************************************/

JS_PUBLIC_API(void)
JS_SetNewScriptHook(JSRuntime *rt, JSNewScriptHook hook, void *callerdata)
{
    rt->globalDebugHooks.newScriptHook = hook;
    rt->globalDebugHooks.newScriptHookData = callerdata;
}

JS_PUBLIC_API(void)
JS_SetDestroyScriptHook(JSRuntime *rt, JSDestroyScriptHook hook,
                        void *callerdata)
{
    rt->globalDebugHooks.destroyScriptHook = hook;
    rt->globalDebugHooks.destroyScriptHookData = callerdata;
}

/***************************************************************************/

JS_PUBLIC_API(JSBool)
JS_EvaluateUCInStackFrame(JSContext *cx, JSStackFrame *fp,
                          const jschar *chars, uintN length,
                          const char *filename, uintN lineno,
                          jsval *rval)
{
    JS_ASSERT_NOT_ON_TRACE(cx);

    if (!CheckDebugMode(cx))
        return false;

    JSObject *scobj = JS_GetFrameScopeChain(cx, fp);
    if (!scobj)
        return false;

    js::AutoCompartment ac(cx, scobj);
    if (!ac.enter())
        return false;

    /*
     * NB: This function breaks the assumption that the compiler can see all
     * calls and properly compute a static level. In order to get around this,
     * we use a static level that will cause us not to attempt to optimize
     * variable references made by this frame.
     */
    JSScript *script = Compiler::compileScript(cx, scobj, fp, js_StackFramePrincipals(cx, fp),
                                               TCF_COMPILE_N_GO, chars, length,
                                               filename, lineno, cx->findVersion(),
                                               NULL, UpvarCookie::UPVAR_LEVEL_LIMIT);

    if (!script)
        return false;

    bool ok = Execute(cx, scobj, script, fp, JSFRAME_DEBUGGER | JSFRAME_EVAL, Valueify(rval));

    js_DestroyScript(cx, script);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_EvaluateInStackFrame(JSContext *cx, JSStackFrame *fp,
                        const char *bytes, uintN length,
                        const char *filename, uintN lineno,
                        jsval *rval)
{
    jschar *chars;
    JSBool ok;
    size_t len = length;
    
    if (!CheckDebugMode(cx))
        return JS_FALSE;

    chars = js_InflateString(cx, bytes, &len);
    if (!chars)
        return JS_FALSE;
    length = (uintN) len;
    ok = JS_EvaluateUCInStackFrame(cx, fp, chars, length, filename, lineno,
                                   rval);
    cx->free(chars);

    return ok;
}

/************************************************************************/

/* This all should be reworked to avoid requiring JSScopeProperty types. */

JS_PUBLIC_API(JSScopeProperty *)
JS_PropertyIterator(JSObject *obj, JSScopeProperty **iteratorp)
{
    const Shape *shape;

    /* The caller passes null in *iteratorp to get things started. */
    shape = (Shape *) *iteratorp;
    if (!shape) {
        shape = obj->lastProperty();
    } else {
        shape = shape->previous();
        if (!shape->previous()) {
            JS_ASSERT(JSID_IS_EMPTY(shape->id));
            shape = NULL;
        }
    }

    return *iteratorp = reinterpret_cast<JSScopeProperty *>(const_cast<Shape *>(shape));
}

JS_PUBLIC_API(JSBool)
JS_GetPropertyDesc(JSContext *cx, JSObject *obj, JSScopeProperty *sprop,
                   JSPropertyDesc *pd)
{
    assertSameCompartment(cx, obj);
    Shape *shape = (Shape *) sprop;
    pd->id = IdToJsval(shape->id);

    JSBool wasThrowing = cx->isExceptionPending();
    Value lastException = UndefinedValue();
    if (wasThrowing)
        lastException = cx->getPendingException();
    cx->clearPendingException();

    if (!js_GetProperty(cx, obj, shape->id, Valueify(&pd->value))) {
        if (!cx->isExceptionPending()) {
            pd->flags = JSPD_ERROR;
            pd->value = JSVAL_VOID;
        } else {
            pd->flags = JSPD_EXCEPTION;
            pd->value = Jsvalify(cx->getPendingException());
        }
    } else {
        pd->flags = 0;
    }

    if (wasThrowing)
        cx->setPendingException(lastException);

    pd->flags |= (shape->enumerable() ? JSPD_ENUMERATE : 0)
              |  (!shape->writable()  ? JSPD_READONLY  : 0)
              |  (!shape->configurable() ? JSPD_PERMANENT : 0);
    pd->spare = 0;
    if (shape->getter() == GetCallArg) {
        pd->slot = shape->shortid;
        pd->flags |= JSPD_ARGUMENT;
    } else if (shape->getter() == GetCallVar) {
        pd->slot = shape->shortid;
        pd->flags |= JSPD_VARIABLE;
    } else {
        pd->slot = 0;
    }
    pd->alias = JSVAL_VOID;

    if (obj->containsSlot(shape->slot)) {
        for (Shape::Range r = obj->lastProperty()->all(); !r.empty(); r.popFront()) {
            const Shape &aprop = r.front();
            if (&aprop != shape && aprop.slot == shape->slot) {
                pd->alias = IdToJsval(aprop.id);
                break;
            }
        }
    }
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_GetPropertyDescArray(JSContext *cx, JSObject *obj, JSPropertyDescArray *pda)
{
    assertSameCompartment(cx, obj);
    Class *clasp = obj->getClass();
    if (!obj->isNative() || (clasp->flags & JSCLASS_NEW_ENUMERATE)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_CANT_DESCRIBE_PROPS, clasp->name);
        return JS_FALSE;
    }
    if (!clasp->enumerate(cx, obj))
        return JS_FALSE;

    /* Return an empty pda early if obj has no own properties. */
    if (obj->nativeEmpty()) {
        pda->length = 0;
        pda->array = NULL;
        return JS_TRUE;
    }

    uint32 n = obj->propertyCount();
    JSPropertyDesc *pd = (JSPropertyDesc *) cx->malloc(size_t(n) * sizeof(JSPropertyDesc));
    if (!pd)
        return JS_FALSE;
    uint32 i = 0;
    for (Shape::Range r = obj->lastProperty()->all(); !r.empty(); r.popFront()) {
        if (!js_AddRoot(cx, Valueify(&pd[i].id), NULL))
            goto bad;
        if (!js_AddRoot(cx, Valueify(&pd[i].value), NULL))
            goto bad;
        Shape *shape = const_cast<Shape *>(&r.front());
        if (!JS_GetPropertyDesc(cx, obj, reinterpret_cast<JSScopeProperty *>(shape), &pd[i]))
            goto bad;
        if ((pd[i].flags & JSPD_ALIAS) && !js_AddRoot(cx, Valueify(&pd[i].alias), NULL))
            goto bad;
        if (++i == n)
            break;
    }
    pda->length = i;
    pda->array = pd;
    return JS_TRUE;

bad:
    pda->length = i + 1;
    pda->array = pd;
    JS_PutPropertyDescArray(cx, pda);
    return JS_FALSE;
}

JS_PUBLIC_API(void)
JS_PutPropertyDescArray(JSContext *cx, JSPropertyDescArray *pda)
{
    JSPropertyDesc *pd;
    uint32 i;

    pd = pda->array;
    for (i = 0; i < pda->length; i++) {
        js_RemoveRoot(cx->runtime, &pd[i].id);
        js_RemoveRoot(cx->runtime, &pd[i].value);
        if (pd[i].flags & JSPD_ALIAS)
            js_RemoveRoot(cx->runtime, &pd[i].alias);
    }
    cx->free(pd);
}

/************************************************************************/

JS_PUBLIC_API(JSBool)
JS_SetDebuggerHandler(JSRuntime *rt, JSDebuggerHandler handler, void *closure)
{
    rt->globalDebugHooks.debuggerHandler = handler;
    rt->globalDebugHooks.debuggerHandlerData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetSourceHandler(JSRuntime *rt, JSSourceHandler handler, void *closure)
{
    rt->globalDebugHooks.sourceHandler = handler;
    rt->globalDebugHooks.sourceHandlerData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetExecuteHook(JSRuntime *rt, JSInterpreterHook hook, void *closure)
{
    rt->globalDebugHooks.executeHook = hook;
    rt->globalDebugHooks.executeHookData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetCallHook(JSRuntime *rt, JSInterpreterHook hook, void *closure)
{
#ifdef JS_TRACER
    {
        AutoLockGC lock(rt);
        bool wasInhibited = rt->debuggerInhibitsJIT();
#endif
        rt->globalDebugHooks.callHook = hook;
        rt->globalDebugHooks.callHookData = closure;
#ifdef JS_TRACER
        JITInhibitingHookChange(rt, wasInhibited);
    }
#endif
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetThrowHook(JSRuntime *rt, JSThrowHook hook, void *closure)
{
    rt->globalDebugHooks.throwHook = hook;
    rt->globalDebugHooks.throwHookData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetDebugErrorHook(JSRuntime *rt, JSDebugErrorHook hook, void *closure)
{
    rt->globalDebugHooks.debugErrorHook = hook;
    rt->globalDebugHooks.debugErrorHookData = closure;
    return JS_TRUE;
}

/************************************************************************/

JS_PUBLIC_API(size_t)
JS_GetObjectTotalSize(JSContext *cx, JSObject *obj)
{
    return obj->slotsAndStructSize();
}

static size_t
GetAtomTotalSize(JSContext *cx, JSAtom *atom)
{
    size_t nbytes;

    nbytes = sizeof(JSAtom *) + sizeof(JSDHashEntryStub);
    nbytes += sizeof(JSString);
    nbytes += (ATOM_TO_STRING(atom)->flatLength() + 1) * sizeof(jschar);
    return nbytes;
}

JS_PUBLIC_API(size_t)
JS_GetFunctionTotalSize(JSContext *cx, JSFunction *fun)
{
    size_t nbytes;

    nbytes = sizeof *fun;
    nbytes += JS_GetObjectTotalSize(cx, FUN_OBJECT(fun));
    if (FUN_INTERPRETED(fun))
        nbytes += JS_GetScriptTotalSize(cx, fun->u.i.script);
    if (fun->atom)
        nbytes += GetAtomTotalSize(cx, fun->atom);
    return nbytes;
}

#include "jsemit.h"

JS_PUBLIC_API(size_t)
JS_GetScriptTotalSize(JSContext *cx, JSScript *script)
{
    size_t nbytes, pbytes;
    jsatomid i;
    jssrcnote *sn, *notes;
    JSObjectArray *objarray;
    JSPrincipals *principals;

    nbytes = sizeof *script;
    if (script->u.object)
        nbytes += JS_GetObjectTotalSize(cx, script->u.object);

    nbytes += script->length * sizeof script->code[0];
    nbytes += script->atomMap.length * sizeof script->atomMap.vector[0];
    for (i = 0; i < script->atomMap.length; i++)
        nbytes += GetAtomTotalSize(cx, script->atomMap.vector[i]);

    if (script->filename)
        nbytes += strlen(script->filename) + 1;

    notes = script->notes();
    for (sn = notes; !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn))
        continue;
    nbytes += (sn - notes + 1) * sizeof *sn;

    if (JSScript::isValidOffset(script->objectsOffset)) {
        objarray = script->objects();
        i = objarray->length;
        nbytes += sizeof *objarray + i * sizeof objarray->vector[0];
        do {
            nbytes += JS_GetObjectTotalSize(cx, objarray->vector[--i]);
        } while (i != 0);
    }

    if (JSScript::isValidOffset(script->regexpsOffset)) {
        objarray = script->regexps();
        i = objarray->length;
        nbytes += sizeof *objarray + i * sizeof objarray->vector[0];
        do {
            nbytes += JS_GetObjectTotalSize(cx, objarray->vector[--i]);
        } while (i != 0);
    }

    if (JSScript::isValidOffset(script->trynotesOffset)) {
        nbytes += sizeof(JSTryNoteArray) +
            script->trynotes()->length * sizeof(JSTryNote);
    }

    principals = script->principals;
    if (principals) {
        JS_ASSERT(principals->refcount);
        pbytes = sizeof *principals;
        if (principals->refcount > 1)
            pbytes = JS_HOWMANY(pbytes, principals->refcount);
        nbytes += pbytes;
    }

    return nbytes;
}

JS_PUBLIC_API(uint32)
JS_GetTopScriptFilenameFlags(JSContext *cx, JSStackFrame *fp)
{
    if (!fp)
        fp = js_GetTopStackFrame(cx);
    while (fp) {
        if (fp->isScriptFrame())
            return JS_GetScriptFilenameFlags(fp->script());
        fp = fp->prev();
    }
    return 0;
 }

JS_PUBLIC_API(uint32)
JS_GetScriptFilenameFlags(JSScript *script)
{
    JS_ASSERT(script);
    if (!script->filename)
        return JSFILENAME_NULL;
    return js_GetScriptFilenameFlags(script->filename);
}

JS_PUBLIC_API(JSBool)
JS_FlagScriptFilenamePrefix(JSRuntime *rt, const char *prefix, uint32 flags)
{
    if (!js_SaveScriptFilenameRT(rt, prefix, flags))
        return JS_FALSE;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_IsSystemObject(JSContext *cx, JSObject *obj)
{
    return obj->isSystem();
}

JS_PUBLIC_API(JSBool)
JS_MakeSystemObject(JSContext *cx, JSObject *obj)
{
    obj->setSystem();
    return true;
}

/************************************************************************/

JS_PUBLIC_API(JSObject *)
JS_UnwrapObject(JSContext *cx, JSObject *obj)
{
    return obj->unwrap();
}

/************************************************************************/

JS_FRIEND_API(void)
js_RevertVersion(JSContext *cx)
{
    cx->clearVersionOverride();
}

JS_PUBLIC_API(const JSDebugHooks *)
JS_GetGlobalDebugHooks(JSRuntime *rt)
{
    return &rt->globalDebugHooks;
}

const JSDebugHooks js_NullDebugHooks = {};

JS_PUBLIC_API(JSDebugHooks *)
JS_SetContextDebugHooks(JSContext *cx, const JSDebugHooks *hooks)
{
    JS_ASSERT(hooks);
    if (hooks != &cx->runtime->globalDebugHooks && hooks != &js_NullDebugHooks)
        LeaveTrace(cx);

#ifdef JS_TRACER
    AutoLockGC lock(cx->runtime);
#endif
    JSDebugHooks *old = const_cast<JSDebugHooks *>(cx->debugHooks);
    cx->debugHooks = hooks;
#ifdef JS_TRACER
    cx->updateJITEnabled();
#endif
    return old;
}

JS_PUBLIC_API(JSDebugHooks *)
JS_ClearContextDebugHooks(JSContext *cx)
{
    return JS_SetContextDebugHooks(cx, &js_NullDebugHooks);
}

JS_PUBLIC_API(JSBool)
JS_StartProfiling()
{
    return Probes::startProfiling();
}

JS_PUBLIC_API(void)
JS_StopProfiling()
{
    Probes::stopProfiling();
}

#ifdef MOZ_PROFILING

static JSBool
StartProfiling(JSContext *cx, uintN argc, jsval *vp)
{
    JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(JS_StartProfiling()));
    return true;
}

static JSBool
StopProfiling(JSContext *cx, uintN argc, jsval *vp)
{
    JS_StopProfiling();
    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return true;
}

#ifdef MOZ_SHARK

static JSBool
IgnoreAndReturnTrue(JSContext *cx, uintN argc, jsval *vp)
{
    JS_SET_RVAL(cx, vp, JSVAL_TRUE);
    return true;
}

#endif

static JSFunctionSpec profiling_functions[] = {
    JS_FN("startProfiling",  StartProfiling,      0,0),
    JS_FN("stopProfiling",   StopProfiling,       0,0),
#ifdef MOZ_SHARK
    /* Keep users of the old shark API happy. */
    JS_FN("connectShark",    IgnoreAndReturnTrue, 0,0),
    JS_FN("disconnectShark", IgnoreAndReturnTrue, 0,0),
    JS_FN("startShark",      StartProfiling,      0,0),
    JS_FN("stopShark",       StopProfiling,       0,0),
#endif
    JS_FS_END
};

#endif

JS_PUBLIC_API(JSBool)
JS_DefineProfilingFunctions(JSContext *cx, JSObject *obj)
{
#ifdef MOZ_PROFILING
    return JS_DefineFunctions(cx, obj, profiling_functions);
#else
    return true;
#endif
}

#ifdef MOZ_CALLGRIND

#include <valgrind/callgrind.h>

JS_FRIEND_API(JSBool)
js_StartCallgrind(JSContext *cx, uintN argc, jsval *vp)
{
    CALLGRIND_START_INSTRUMENTATION;
    CALLGRIND_ZERO_STATS;
    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return JS_TRUE;
}

JS_FRIEND_API(JSBool)
js_StopCallgrind(JSContext *cx, uintN argc, jsval *vp)
{
    CALLGRIND_STOP_INSTRUMENTATION;
    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return JS_TRUE;
}

JS_FRIEND_API(JSBool)
js_DumpCallgrind(JSContext *cx, uintN argc, jsval *vp)
{
    JSString *str;

    jsval *argv = JS_ARGV(cx, vp);
    if (argc > 0 && JSVAL_IS_STRING(argv[0])) {
        str = JSVAL_TO_STRING(argv[0]);
        JSAutoByteString bytes(cx, str);
        if (!!bytes) {
            CALLGRIND_DUMP_STATS_AT(bytes.ptr());
            return JS_TRUE;
        }
    }
    CALLGRIND_DUMP_STATS;

    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return JS_TRUE;
}

#endif /* MOZ_CALLGRIND */

#ifdef MOZ_VTUNE
#include <VTuneApi.h>

static const char *vtuneErrorMessages[] = {
  "unknown, error #0",
  "invalid 'max samples' field",
  "invalid 'samples per buffer' field",
  "invalid 'sample interval' field",
  "invalid path",
  "sample file in use",
  "invalid 'number of events' field",
  "unknown, error #7",
  "internal error",
  "bad event name",
  "VTStopSampling called without calling VTStartSampling",
  "no events selected for event-based sampling",
  "events selected cannot be run together",
  "no sampling parameters",
  "sample database already exists",
  "sampling already started",
  "time-based sampling not supported",
  "invalid 'sampling parameters size' field",
  "invalid 'event size' field",
  "sampling file already bound",
  "invalid event path",
  "invalid license",
  "invalid 'global options' field",

};

JS_FRIEND_API(JSBool)
js_StartVtune(JSContext *cx, uintN argc, jsval *vp)
{
    VTUNE_EVENT events[] = {
        { 1000000, 0, 0, 0, "CPU_CLK_UNHALTED.CORE" },
        { 1000000, 0, 0, 0, "INST_RETIRED.ANY" },
    };

    U32 n_events = sizeof(events) / sizeof(VTUNE_EVENT);
    char *default_filename = "mozilla-vtune.tb5";
    JSString *str;
    U32 status;

    VTUNE_SAMPLING_PARAMS params = {
        sizeof(VTUNE_SAMPLING_PARAMS),
        sizeof(VTUNE_EVENT),
        0, 0, /* Reserved fields */
        1,    /* Initialize in "paused" state */
        0,    /* Max samples, or 0 for "continuous" */
        4096, /* Samples per buffer */
        0.1,  /* Sampling interval in ms */
        1,    /* 1 for event-based sampling, 0 for time-based */

        n_events,
        events,
        default_filename,
    };

    jsval *argv = JS_ARGV(cx, vp);
    if (argc > 0 && JSVAL_IS_STRING(argv[0])) {
        str = JSVAL_TO_STRING(argv[0]);
        params.tb5Filename = js_DeflateString(cx, str->chars(), str->length());
    }

    status = VTStartSampling(&params);

    if (params.tb5Filename != default_filename)
        cx->free(params.tb5Filename);

    if (status != 0) {
        if (status == VTAPI_MULTIPLE_RUNS)
            VTStopSampling(0);
        if (status < sizeof(vtuneErrorMessages))
            JS_ReportError(cx, "Vtune setup error: %s",
                           vtuneErrorMessages[status]);
        else
            JS_ReportError(cx, "Vtune setup error: %d",
                           status);
        return false;
    }
    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return true;
}

JS_FRIEND_API(JSBool)
js_StopVtune(JSContext *cx, uintN argc, jsval *vp)
{
    U32 status = VTStopSampling(1);
    if (status) {
        if (status < sizeof(vtuneErrorMessages))
            JS_ReportError(cx, "Vtune shutdown error: %s",
                           vtuneErrorMessages[status]);
        else
            JS_ReportError(cx, "Vtune shutdown error: %d",
                           status);
        return false;
    }
    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return true;
}

JS_FRIEND_API(JSBool)
js_PauseVtune(JSContext *cx, uintN argc, jsval *vp)
{
    VTPause();
    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return true;
}

JS_FRIEND_API(JSBool)
js_ResumeVtune(JSContext *cx, uintN argc, jsval *vp)
{
    VTResume();
    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return true;
}

#endif /* MOZ_VTUNE */

#ifdef MOZ_TRACEVIS
/*
 * Ethogram - Javascript wrapper for TraceVis state
 *
 * ethology: The scientific study of animal behavior,
 *           especially as it occurs in a natural environment.
 * ethogram: A pictorial catalog of the behavioral patterns of
 *           an organism or a species.
 *
 */
#if defined(XP_WIN)
#include "jswin.h"
#else
#include <sys/time.h>
#endif
#include "jstracer.h"

#define ETHOGRAM_BUF_SIZE 65536

static JSBool
ethogram_construct(JSContext *cx, uintN argc, jsval *vp);
static void
ethogram_finalize(JSContext *cx, JSObject *obj);

static JSClass ethogram_class = {
    "Ethogram",
    JSCLASS_HAS_PRIVATE,
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, ethogram_finalize,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

struct EthogramEvent {
    TraceVisState s;
    TraceVisExitReason r;
    int ts;
    int tus;
    JSString *filename;
    int lineno;
};

static int
compare_strings(const void *k1, const void *k2)
{
    return strcmp((const char *) k1, (const char *) k2) == 0;
}

class EthogramEventBuffer {
private:
    EthogramEvent mBuf[ETHOGRAM_BUF_SIZE];
    int mReadPos;
    int mWritePos;
    JSObject *mFilenames;
    int mStartSecond;

    struct EthogramScriptEntry {
        char *filename;
        JSString *jsfilename;

        EthogramScriptEntry *next;
    };
    EthogramScriptEntry *mScripts;

public:
    friend JSBool
    ethogram_construct(JSContext *cx, uintN argc, jsval *vp);

    inline void push(TraceVisState s, TraceVisExitReason r, char *filename, int lineno) {
        mBuf[mWritePos].s = s;
        mBuf[mWritePos].r = r;
#if defined(XP_WIN)
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        unsigned long long raw_us = 0.1 *
            (((unsigned long long) now.dwHighDateTime << 32ULL) |
             (unsigned long long) now.dwLowDateTime);
        unsigned int sec = raw_us / 1000000L;
        unsigned int usec = raw_us % 1000000L;
        mBuf[mWritePos].ts = sec - mStartSecond;
        mBuf[mWritePos].tus = usec;
#else
        struct timeval tv;
        gettimeofday(&tv, NULL);
        mBuf[mWritePos].ts = tv.tv_sec - mStartSecond;
        mBuf[mWritePos].tus = tv.tv_usec;
#endif

        JSString *jsfilename = findScript(filename);
        mBuf[mWritePos].filename = jsfilename;
        mBuf[mWritePos].lineno = lineno;

        mWritePos = (mWritePos + 1) % ETHOGRAM_BUF_SIZE;
        if (mWritePos == mReadPos) {
            mReadPos = (mWritePos + 1) % ETHOGRAM_BUF_SIZE;
        }
    }

    inline EthogramEvent *pop() {
        EthogramEvent *e = &mBuf[mReadPos];
        mReadPos = (mReadPos + 1) % ETHOGRAM_BUF_SIZE;
        return e;
    }

    bool isEmpty() {
        return (mReadPos == mWritePos);
    }

    EthogramScriptEntry *addScript(JSContext *cx, JSObject *obj, char *filename, JSString *jsfilename) {
        JSHashNumber hash = JS_HashString(filename);
        JSHashEntry **hep = JS_HashTableRawLookup(traceVisScriptTable, hash, filename);
        if (*hep != NULL)
            return NULL;

        JS_HashTableRawAdd(traceVisScriptTable, hep, hash, filename, this);

        EthogramScriptEntry * entry = (EthogramScriptEntry *) JS_malloc(cx, sizeof(EthogramScriptEntry));
        if (entry == NULL)
            return NULL;

        entry->next = mScripts;
        mScripts = entry;
        entry->filename = filename;
        entry->jsfilename = jsfilename;

        return mScripts;
    }

    void removeScripts(JSContext *cx) {
        EthogramScriptEntry *se = mScripts;
        while (se != NULL) {
            char *filename = se->filename;

            JSHashNumber hash = JS_HashString(filename);
            JSHashEntry **hep = JS_HashTableRawLookup(traceVisScriptTable, hash, filename);
            JSHashEntry *he = *hep;
            if (he) {
                /* we hardly knew he */
                JS_HashTableRawRemove(traceVisScriptTable, hep, he);
            }

            EthogramScriptEntry *se_head = se;
            se = se->next;
            JS_free(cx, se_head);
        }
    }

    JSString *findScript(char *filename) {
        EthogramScriptEntry *se = mScripts;
        while (se != NULL) {
            if (compare_strings(se->filename, filename))
                return (se->jsfilename);
            se = se->next;
        }
        return NULL;
    }

    JSObject *filenames() {
        return mFilenames;
    }

    int length() {
        if (mWritePos < mReadPos)
            return (mWritePos + ETHOGRAM_BUF_SIZE) - mReadPos;
        else
            return mWritePos - mReadPos;
    }
};

static char jstv_empty[] = "<null>";

inline char *
jstv_Filename(JSStackFrame *fp)
{
    while (fp && !fp->isScriptFrame())
        fp = fp->prev();
    return (fp && fp->maybeScript() && fp->script()->filename)
           ? (char *)fp->script()->filename
           : jstv_empty;
}
inline uintN
jstv_Lineno(JSContext *cx, JSStackFrame *fp)
{
    while (fp && fp->pc(cx) == NULL)
        fp = fp->prev();
    return (fp && fp->pc(cx)) ? js_FramePCToLineNumber(cx, fp) : 0;
}

/* Collect states here and distribute to a matching buffer, if any */
JS_FRIEND_API(void)
js::StoreTraceVisState(JSContext *cx, TraceVisState s, TraceVisExitReason r)
{
    JSStackFrame *fp = cx->fp();

    char *script_file = jstv_Filename(fp);
    JSHashNumber hash = JS_HashString(script_file);

    JSHashEntry **hep = JS_HashTableRawLookup(traceVisScriptTable, hash, script_file);
    /* update event buffer, flag if overflowed */
    JSHashEntry *he = *hep;
    if (he) {
        EthogramEventBuffer *p;
        p = (EthogramEventBuffer *) he->value;

        p->push(s, r, script_file, jstv_Lineno(cx, fp));
    }
}

static JSBool
ethogram_construct(JSContext *cx, uintN argc, jsval *vp)
{
    EthogramEventBuffer *p;

    p = (EthogramEventBuffer *) JS_malloc(cx, sizeof(EthogramEventBuffer));
    if (!p)
        return JS_FALSE;

    p->mReadPos = p->mWritePos = 0;
    p->mScripts = NULL;
    p->mFilenames = JS_NewArrayObject(cx, 0, NULL);

#if defined(XP_WIN)
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    unsigned long long raw_us = 0.1 *
        (((unsigned long long) now.dwHighDateTime << 32ULL) |
         (unsigned long long) now.dwLowDateTime);
    unsigned int s = raw_us / 1000000L;
    p->mStartSecond = s;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    p->mStartSecond = tv.tv_sec;
#endif
    JSObject *obj;
    if (JS_IsConstructing(cx, vp)) {
        obj = JS_NewObject(cx, &ethogram_class, NULL, NULL);
        if (!obj)
            return JS_FALSE;
    } else {
        obj = JS_THIS_OBJECT(cx, vp);
    }

    jsval filenames = OBJECT_TO_JSVAL(p->filenames());
    if (!JS_DefineProperty(cx, obj, "filenames", filenames,
                           NULL, NULL, JSPROP_READONLY|JSPROP_PERMANENT))
        return JS_FALSE;

    JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(obj));
    JS_SetPrivate(cx, obj, p);
    return JS_TRUE;
}

static void
ethogram_finalize(JSContext *cx, JSObject *obj)
{
    EthogramEventBuffer *p;
    p = (EthogramEventBuffer *) JS_GetInstancePrivate(cx, obj, &ethogram_class, NULL);
    if (!p)
        return;

    p->removeScripts(cx);

    JS_free(cx, p);
}

static JSBool
ethogram_addScript(JSContext *cx, uintN argc, jsval *vp)
{
    JSString *str;
    char *filename = NULL;
    jsval *argv = JS_ARGV(cx, vp);
    JSObject *obj = JS_THIS_OBJECT(cx, vp);
    if (!obj)
        return false;
    if (argc < 1) {
        /* silently ignore no args */
        JS_SET_RVAL(cx, vp, JSVAL_VOID);
        return true;
    }
    if (JSVAL_IS_STRING(argv[0])) {
        str = JSVAL_TO_STRING(argv[0]);
        filename = js_DeflateString(cx, str->chars(), str->length());
        if (!filename)
            return false;
    }

    EthogramEventBuffer *p = (EthogramEventBuffer *) JS_GetInstancePrivate(cx, obj, &ethogram_class, argv);

    p->addScript(cx, obj, filename, str);
    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    jsval dummy;
    JS_CallFunctionName(cx, p->filenames(), "push", 1, argv, &dummy);
    return true;
}

static JSBool
ethogram_getAllEvents(JSContext *cx, uintN argc, jsval *vp)
{
    EthogramEventBuffer *p;
    jsval *argv = JS_ARGV(cx, vp);

    JSObject *obj = JS_THIS_OBJECT(cx, vp);
    if (!obj)
        return JS_FALSE;

    p = (EthogramEventBuffer *) JS_GetInstancePrivate(cx, obj, &ethogram_class, argv);
    if (!p)
        return JS_FALSE;

    if (p->isEmpty()) {
        JS_SET_RVAL(cx, vp, JSVAL_NULL);
        return JS_TRUE;
    }

    JSObject *rarray = JS_NewArrayObject(cx, 0, NULL);
    if (rarray == NULL) {
        JS_SET_RVAL(cx, vp, JSVAL_NULL);
        return JS_TRUE;
    }

    JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(rarray));

    for (int i = 0; !p->isEmpty(); i++) {

        JSObject *x = JS_NewObject(cx, NULL, NULL, NULL);
        if (x == NULL)
            return JS_FALSE;

        EthogramEvent *e = p->pop();

        jsval state = INT_TO_JSVAL(e->s);
        jsval reason = INT_TO_JSVAL(e->r);
        jsval ts = INT_TO_JSVAL(e->ts);
        jsval tus = INT_TO_JSVAL(e->tus);

        jsval filename = STRING_TO_JSVAL(e->filename);
        jsval lineno = INT_TO_JSVAL(e->lineno);

        if (!JS_SetProperty(cx, x, "state", &state))
            return JS_FALSE;
        if (!JS_SetProperty(cx, x, "reason", &reason))
            return JS_FALSE;
        if (!JS_SetProperty(cx, x, "ts", &ts))
            return JS_FALSE;
        if (!JS_SetProperty(cx, x, "tus", &tus))
            return JS_FALSE;

        if (!JS_SetProperty(cx, x, "filename", &filename))
            return JS_FALSE;
        if (!JS_SetProperty(cx, x, "lineno", &lineno))
            return JS_FALSE;

        jsval element = OBJECT_TO_JSVAL(x);
        JS_SetElement(cx, rarray, i, &element);
    }

    return JS_TRUE;
}

static JSBool
ethogram_getNextEvent(JSContext *cx, uintN argc, jsval *vp)
{
    EthogramEventBuffer *p;
    jsval *argv = JS_ARGV(cx, vp);

    JSObject *obj = JS_THIS_OBJECT(cx, vp);
    if (!obj)
        return JS_FALSE;

    p = (EthogramEventBuffer *) JS_GetInstancePrivate(cx, obj, &ethogram_class, argv);
    if (!p)
        return JS_FALSE;

    JSObject *x = JS_NewObject(cx, NULL, NULL, NULL);
    if (x == NULL)
        return JS_FALSE;

    if (p->isEmpty()) {
        JS_SET_RVAL(cx, vp, JSVAL_NULL);
        return JS_TRUE;
    }

    EthogramEvent *e = p->pop();
    jsval state = INT_TO_JSVAL(e->s);
    jsval reason = INT_TO_JSVAL(e->r);
    jsval ts = INT_TO_JSVAL(e->ts);
    jsval tus = INT_TO_JSVAL(e->tus);

    jsval filename = STRING_TO_JSVAL(e->filename);
    jsval lineno = INT_TO_JSVAL(e->lineno);

    if (!JS_SetProperty(cx, x, "state", &state))
        return JS_FALSE;
    if (!JS_SetProperty(cx, x, "reason", &reason))
        return JS_FALSE;
    if (!JS_SetProperty(cx, x, "ts", &ts))
        return JS_FALSE;
    if (!JS_SetProperty(cx, x, "tus", &tus))
        return JS_FALSE;
    if (!JS_SetProperty(cx, x, "filename", &filename))
        return JS_FALSE;

    if (!JS_SetProperty(cx, x, "lineno", &lineno))
        return JS_FALSE;

    JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(x));

    return JS_TRUE;
}

static JSFunctionSpec ethogram_methods[] = {
    JS_FN("addScript",    ethogram_addScript,    1,0),
    JS_FN("getAllEvents", ethogram_getAllEvents, 0,0),
    JS_FN("getNextEvent", ethogram_getNextEvent, 0,0),
    JS_FS_END
};

/*
 * An |Ethogram| organizes the output of a collection of files that should be
 * monitored together. A single object gets events for the group.
 */
JS_FRIEND_API(JSBool)
js_InitEthogram(JSContext *cx, uintN argc, jsval *vp)
{
    if (!traceVisScriptTable) {
        traceVisScriptTable = JS_NewHashTable(8, JS_HashString, compare_strings,
                                         NULL, NULL, NULL);
    }

    JS_InitClass(cx, JS_GetGlobalObject(cx), NULL, &ethogram_class,
                 ethogram_construct, 0, NULL, ethogram_methods,
                 NULL, NULL);

    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return true;
}

JS_FRIEND_API(JSBool)
js_ShutdownEthogram(JSContext *cx, uintN argc, jsval *vp)
{
    if (traceVisScriptTable)
        JS_HashTableDestroy(traceVisScriptTable);

    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return true;
}

#endif /* MOZ_TRACEVIS */

#ifdef MOZ_TRACE_JSCALLS

JS_PUBLIC_API(void)
JS_SetFunctionCallback(JSContext *cx, JSFunctionCallback fcb)
{
    cx->functionCallback = fcb;
}

JS_PUBLIC_API(JSFunctionCallback)
JS_GetFunctionCallback(JSContext *cx)
{
    return cx->functionCallback;
}

#endif /* MOZ_TRACE_JSCALLS */

