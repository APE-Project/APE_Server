/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
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
 * JavaScript bytecode interpreter.
 */
#include "jsstddef.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "jstypes.h"
#include "jsarena.h" /* Added by JSIFY */
#include "jsutil.h" /* Added by JSIFY */
#include "jsprf.h"
#include "jsapi.h"
#include "jsarray.h"
#include "jsatom.h"
#include "jsbool.h"
#include "jscntxt.h"
#include "jsconfig.h"
#include "jsdbgapi.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsinterp.h"
#include "jsiter.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jsscan.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsstr.h"

#ifdef INCLUDE_MOZILLA_DTRACE
#include "jsdtracef.h"
#endif

#if JS_HAS_XML_SUPPORT
#include "jsxml.h"
#endif

#ifdef js_invoke_c__

uint32
js_GenerateShape(JSContext *cx, JSBool gcLocked, JSScopeProperty *sprop)
{
    JSRuntime *rt;
    uint32 shape;
    JSTempValueRooter tvr;

    rt = cx->runtime;
    shape = JS_ATOMIC_INCREMENT(&rt->shapeGen);
    JS_ASSERT(shape != 0);
    if (shape & SHAPE_OVERFLOW_BIT) {
        rt->gcPoke = JS_TRUE;
        if (sprop)
            JS_PUSH_TEMP_ROOT_SPROP(cx, sprop, &tvr);
        js_GC(cx, gcLocked ? GC_LOCK_HELD : GC_NORMAL);
        if (sprop)
            JS_POP_TEMP_ROOT(cx, &tvr);
        shape = JS_ATOMIC_INCREMENT(&rt->shapeGen);
        JS_ASSERT(shape != 0);
        JS_ASSERT_IF(shape & SHAPE_OVERFLOW_BIT,
                     JS_PROPERTY_CACHE(cx).disabled);
    }
    return shape;
}

void
js_FillPropertyCache(JSContext *cx, JSObject *obj, jsuword kshape,
                     uintN scopeIndex, uintN protoIndex,
                     JSObject *pobj, JSScopeProperty *sprop,
                     JSPropCacheEntry **entryp)
{
    JSPropertyCache *cache;
    jsbytecode *pc;
    JSScope *scope;
    JSOp op;
    const JSCodeSpec *cs;
    jsuword vword;
    ptrdiff_t pcoff;
    jsuword khash;
    JSAtom *atom;
    JSPropCacheEntry *entry;

    cache = &JS_PROPERTY_CACHE(cx);
    pc = cx->fp->regs->pc;
    if (cache->disabled) {
        PCMETER(cache->disfills++);
        *entryp = NULL;
        return;
    }

    /*
     * Check for fill from js_SetPropertyHelper where the setter removed sprop
     * from pobj's scope (via unwatch or delete, e.g.).
     */
    scope = OBJ_SCOPE(pobj);
    JS_ASSERT(scope->object == pobj);
    if (!SCOPE_HAS_PROPERTY(scope, sprop)) {
        PCMETER(cache->oddfills++);
        *entryp = NULL;
        return;
    }

    /*
     * Check for overdeep scope and prototype chain. Because resolve, getter,
     * and setter hooks can change the prototype chain using JS_SetPrototype
     * after js_LookupPropertyWithFlags has returned the nominal protoIndex,
     * we have to validate protoIndex if it is non-zero. If it is zero, then
     * we know thanks to the SCOPE_HAS_PROPERTY test above, and from the fact
     * that obj == pobj, that protoIndex is invariant.
     *
     * The scopeIndex can't be wrong. We require JS_SetParent calls to happen
     * before any running script might consult a parent-linked scope chain. If
     * this requirement is not satisfied, the fill in progress will never hit,
     * but vcap vs. scope shape tests ensure nothing malfunctions.
     */
    JS_ASSERT_IF(scopeIndex == 0 && protoIndex == 0, obj == pobj);
    if (protoIndex != 0) {
        JSObject *tmp;

        JS_ASSERT(pobj != obj);
        protoIndex = 1;
        tmp = obj;
        for (;;) {
            tmp = OBJ_GET_PROTO(cx, tmp);
            if (!tmp) {
                PCMETER(cache->noprotos++);
                *entryp = NULL;
                return;
            }
            if (tmp == pobj)
                break;
            ++protoIndex;
        }
    }
    if (scopeIndex > PCVCAP_SCOPEMASK || protoIndex > PCVCAP_PROTOMASK) {
        PCMETER(cache->longchains++);
        *entryp = NULL;
        return;
    }

    /*
     * Optimize the cached vword based on our parameters and the current pc's
     * opcode format flags.
     */
    op = (JSOp) *pc;
    cs = &js_CodeSpec[op];

    do {
        /*
         * Check for a prototype "plain old method" callee computation. What
         * is a plain old method? It's a function-valued property with stub
         * getter and setter, so get of a function is idempotent and set is
         * transparent.
         */
        if (cs->format & JOF_CALLOP) {
            if (SPROP_HAS_STUB_GETTER(sprop) &&
                SPROP_HAS_VALID_SLOT(sprop, scope)) {
                jsval v;

                v = LOCKED_OBJ_GET_SLOT(pobj, sprop->slot);
                if (VALUE_IS_FUNCTION(cx, v)) {
                    /*
                     * Great, we have a function-valued prototype property
                     * where the getter is JS_PropertyStub. The type id in
                     * pobj's scope does not evolve with changes to property
                     * values, however.
                     *
                     * So here, on first cache fill for this method, we brand
                     * the scope with a new shape and set the SCOPE_BRANDED
                     * flag.  Once this scope flag is set, any write that adds
                     * or deletes a function-valued plain old property in
                     * scope->object will result in shape being regenerated.
                     */
                    if (!SCOPE_IS_BRANDED(scope)) {
                        PCMETER(cache->brandfills++);
#ifdef DEBUG_notme
                        fprintf(stderr,
                            "branding %p (%s) for funobj %p (%s), kshape %lu\n",
                            pobj, LOCKED_OBJ_GET_CLASS(pobj)->name,
                            JSVAL_TO_OBJECT(v),
                            JS_GetFunctionName(GET_FUNCTION_PRIVATE(cx,
                                                 JSVAL_TO_OBJECT(v))),
                            kshape);
#endif
                        SCOPE_MAKE_UNIQUE_SHAPE(cx, scope);
                        SCOPE_SET_BRANDED(scope);
                        kshape = scope->shape;
                    }
                    vword = JSVAL_OBJECT_TO_PCVAL(v);
                    break;
                }
            }
        }

        /* If getting a value via a stub getter, we can cache the slot. */
        if (!(cs->format & JOF_SET) &&
            SPROP_HAS_STUB_GETTER(sprop) &&
            SPROP_HAS_VALID_SLOT(sprop, scope)) {
            /* Great, let's cache sprop's slot and use it on cache hit. */
            vword = SLOT_TO_PCVAL(sprop->slot);
        } else {
            /* Best we can do is to cache sprop (still a nice speedup). */
            vword = SPROP_TO_PCVAL(sprop);
        }
    } while (0);

    khash = PROPERTY_CACHE_HASH_PC(pc, kshape);
    if (obj == pobj) {
        JS_ASSERT(kshape != 0 || scope->shape != 0);
        JS_ASSERT(scopeIndex == 0 && protoIndex == 0);
        JS_ASSERT(OBJ_SCOPE(obj)->object == obj);
        if (!(cs->format & JOF_SET))
            kshape = scope->shape;
    } else {
        if (op == JSOP_LENGTH) {
            atom = cx->runtime->atomState.lengthAtom;
        } else {
            pcoff = (JOF_TYPE(cs->format) == JOF_SLOTATOM) ? 2 : 0;
            GET_ATOM_FROM_BYTECODE(cx->fp->script, pc, pcoff, atom);
        }
        JS_ASSERT_IF(scopeIndex == 0,
                     protoIndex != 1 || OBJ_GET_PROTO(cx, obj) == pobj);
        if (scopeIndex != 0 || protoIndex != 1) {
            khash = PROPERTY_CACHE_HASH_ATOM(atom, obj, pobj);
            PCMETER(if (PCVCAP_TAG(cache->table[khash].vcap) <= 1)
                        cache->pcrecycles++);
            pc = (jsbytecode *) atom;
            kshape = (jsuword) obj;
        }
    }

    entry = &cache->table[khash];
    PCMETER(if (entry != *entryp) cache->modfills++);
    PCMETER(if (!PCVAL_IS_NULL(entry->vword)) cache->recycles++);
    entry->kpc = pc;
    entry->kshape = kshape;
    entry->vcap = PCVCAP_MAKE(scope->shape, scopeIndex, protoIndex);
    entry->vword = vword;
    *entryp = entry;

    cache->empty = JS_FALSE;
    PCMETER(cache->fills++);
}

JSAtom *
js_FullTestPropertyCache(JSContext *cx, jsbytecode *pc,
                         JSObject **objp, JSObject **pobjp,
                         JSPropCacheEntry **entryp)
{
    JSOp op;
    const JSCodeSpec *cs;
    ptrdiff_t pcoff;
    JSAtom *atom;
    JSObject *obj, *pobj, *tmp;
    JSPropCacheEntry *entry;
    uint32 vcap;

    JS_ASSERT(JS_UPTRDIFF(pc, cx->fp->script->code) < cx->fp->script->length);

    op = (JSOp) *pc;
    cs = &js_CodeSpec[op];
    if (op == JSOP_LENGTH) {
        atom = cx->runtime->atomState.lengthAtom;
    } else {
        pcoff = (JOF_TYPE(cs->format) == JOF_SLOTATOM) ? 2 : 0;
        GET_ATOM_FROM_BYTECODE(cx->fp->script, pc, pcoff, atom);
    }

    obj = *objp;
    JS_ASSERT(OBJ_IS_NATIVE(obj));
    entry = &JS_PROPERTY_CACHE(cx).table[PROPERTY_CACHE_HASH_ATOM(atom, obj, NULL)];
    *entryp = entry;
    vcap = entry->vcap;

    if (entry->kpc != (jsbytecode *) atom) {
        PCMETER(JS_PROPERTY_CACHE(cx).idmisses++);

#ifdef DEBUG_notme
        entry = &JS_PROPERTY_CACHE(cx)
                 .table[PROPERTY_CACHE_HASH_PC(pc, OBJ_SCOPE(obj)->shape)];
        fprintf(stderr,
                "id miss for %s from %s:%u"
                " (pc %u, kpc %u, kshape %u, shape %u)\n",
                js_AtomToPrintableString(cx, atom),
                cx->fp->script->filename,
                js_PCToLineNumber(cx, cx->fp->script, pc),
                pc - cx->fp->script->code,
                entry->kpc - cx->fp->script->code,
                entry->kshape,
                OBJ_SCOPE(obj)->shape);
                js_Disassemble1(cx, cx->fp->script, pc,
                        PTRDIFF(pc, cx->fp->script->code, jsbytecode),
                        JS_FALSE, stderr);
#endif

        return atom;
    }

    if (entry->kshape != (jsuword) obj) {
        PCMETER(JS_PROPERTY_CACHE(cx).komisses++);
        return atom;
    }

    pobj = obj;
    JS_LOCK_OBJ(cx, pobj);

    if (JOF_MODE(cs->format) == JOF_NAME) {
        while (vcap & (PCVCAP_SCOPEMASK << PCVCAP_PROTOBITS)) {
            tmp = LOCKED_OBJ_GET_PARENT(pobj);
            if (!tmp || !OBJ_IS_NATIVE(tmp))
                break;
            JS_UNLOCK_OBJ(cx, pobj);
            pobj = tmp;
            JS_LOCK_OBJ(cx, pobj);
            vcap -= PCVCAP_PROTOSIZE;
        }

        *objp = pobj;
    }

    while (vcap & PCVCAP_PROTOMASK) {
        tmp = LOCKED_OBJ_GET_PROTO(pobj);
        if (!tmp || !OBJ_IS_NATIVE(tmp))
            break;
        JS_UNLOCK_OBJ(cx, pobj);
        pobj = tmp;
        JS_LOCK_OBJ(cx, pobj);
        --vcap;
    }

    if (PCVCAP_SHAPE(vcap) == OBJ_SCOPE(pobj)->shape) {
#ifdef DEBUG
        jsid id = ATOM_TO_JSID(atom);

        CHECK_FOR_STRING_INDEX(id);
        JS_ASSERT(SCOPE_GET_PROPERTY(OBJ_SCOPE(pobj), id));
        JS_ASSERT(OBJ_SCOPE(pobj)->object == pobj);
#endif
        *pobjp = pobj;
        return NULL;
    }

    PCMETER(JS_PROPERTY_CACHE(cx).vcmisses++);
    JS_UNLOCK_OBJ(cx, pobj);
    return atom;
}

#ifdef DEBUG
#define ASSERT_CACHE_IS_EMPTY(cache)                                          \
    JS_BEGIN_MACRO                                                            \
        JSPropertyCache *cache_ = (cache);                                    \
        uintN i_;                                                             \
        JS_ASSERT(cache_->empty);                                             \
        for (i_ = 0; i_ < PROPERTY_CACHE_SIZE; i_++) {                        \
            JS_ASSERT(!cache_->table[i_].kpc);                                \
            JS_ASSERT(!cache_->table[i_].kshape);                             \
            JS_ASSERT(!cache_->table[i_].vcap);                               \
            JS_ASSERT(!cache_->table[i_].vword);                              \
        }                                                                     \
    JS_END_MACRO
#else
#define ASSERT_CACHE_IS_EMPTY(cache) ((void)0)
#endif

JS_STATIC_ASSERT(PCVAL_NULL == 0);

void
js_FlushPropertyCache(JSContext *cx)
{
    JSPropertyCache *cache;

    cache = &JS_PROPERTY_CACHE(cx);
    if (cache->empty) {
        ASSERT_CACHE_IS_EMPTY(cache);
        return;
    }

    memset(cache->table, 0, sizeof cache->table);
    cache->empty = JS_TRUE;

#ifdef JS_PROPERTY_CACHE_METERING
  { static FILE *fp;
    if (!fp)
        fp = fopen("/tmp/propcache.stats", "w");
    if (fp) {
        fputs("Property cache stats for ", fp);
#ifdef JS_THREADSAFE
        fprintf(fp, "thread %lu, ", (unsigned long) cx->thread->id);
#endif
        fprintf(fp, "GC %u\n", cx->runtime->gcNumber);

# define P(mem) fprintf(fp, "%11s %10lu\n", #mem, (unsigned long)cache->mem)
        P(fills);
        P(nofills);
        P(rofills);
        P(disfills);
        P(oddfills);
        P(modfills);
        P(brandfills);
        P(noprotos);
        P(longchains);
        P(recycles);
        P(pcrecycles);
        P(tests);
        P(pchits);
        P(protopchits);
        P(initests);
        P(inipchits);
        P(inipcmisses);
        P(settests);
        P(addpchits);
        P(setpchits);
        P(setpcmisses);
        P(slotchanges);
        P(setmisses);
        P(idmisses);
        P(komisses);
        P(vcmisses);
        P(misses);
        P(flushes);
# undef P

        fprintf(fp, "hit rates: pc %g%% (proto %g%%), set %g%%, ini %g%%, full %g%%\n",
                (100. * cache->pchits) / cache->tests,
                (100. * cache->protopchits) / cache->tests,
                (100. * (cache->addpchits + cache->setpchits))
                / cache->settests,
                (100. * cache->inipchits) / cache->initests,
                (100. * (cache->tests - cache->misses)) / cache->tests);
        fflush(fp);
    }
  }
#endif

    PCMETER(cache->flushes++);
}

void
js_FlushPropertyCacheForScript(JSContext *cx, JSScript *script)
{
    JSPropertyCache *cache;
    JSPropCacheEntry *entry;

    cache = &JS_PROPERTY_CACHE(cx);
    for (entry = cache->table; entry < cache->table + PROPERTY_CACHE_SIZE;
         entry++) {
        if (JS_UPTRDIFF(entry->kpc, script->code) < script->length) {
            entry->kpc = NULL;
            entry->kshape = 0;
#ifdef DEBUG
            entry->vcap = entry->vword = 0;
#endif
        }
    }
}

void
js_DisablePropertyCache(JSContext *cx)
{
    JS_ASSERT(JS_PROPERTY_CACHE(cx).disabled >= 0);
    ++JS_PROPERTY_CACHE(cx).disabled;
}

void
js_EnablePropertyCache(JSContext *cx)
{
    --JS_PROPERTY_CACHE(cx).disabled;
    JS_ASSERT(JS_PROPERTY_CACHE(cx).disabled >= 0);
}

/*
 * Check if the current arena has enough space to fit nslots after sp and, if
 * so, reserve the necessary space.
 */
static JSBool
AllocateAfterSP(JSContext *cx, jsval *sp, uintN nslots)
{
    uintN surplus;
    jsval *sp2;

    JS_ASSERT((jsval *) cx->stackPool.current->base <= sp);
    JS_ASSERT(sp <= (jsval *) cx->stackPool.current->avail);
    surplus = (jsval *) cx->stackPool.current->avail - sp;
    if (nslots <= surplus)
        return JS_TRUE;

    /*
     * No room before current->avail, check if the arena has enough space to
     * fit the missing slots before the limit.
     */
    if (nslots > (size_t) ((jsval *) cx->stackPool.current->limit - sp))
        return JS_FALSE;

    JS_ARENA_ALLOCATE_CAST(sp2, jsval *, &cx->stackPool,
                           (nslots - surplus) * sizeof(jsval));
    JS_ASSERT(sp2 == sp + surplus);
    return JS_TRUE;
}

jsval *
js_AllocRawStack(JSContext *cx, uintN nslots, void **markp)
{
    jsval *sp;

    if (!cx->stackPool.first.next) {
        int64 *timestamp;

        JS_ARENA_ALLOCATE_CAST(timestamp, int64 *,
                               &cx->stackPool, sizeof *timestamp);
        if (!timestamp) {
            js_ReportOutOfScriptQuota(cx);
            return NULL;
        }
        *timestamp = JS_Now();
    }

    if (markp)
        *markp = JS_ARENA_MARK(&cx->stackPool);
    JS_ARENA_ALLOCATE_CAST(sp, jsval *, &cx->stackPool, nslots * sizeof(jsval));
    if (!sp)
        js_ReportOutOfScriptQuota(cx);
    return sp;
}

void
js_FreeRawStack(JSContext *cx, void *mark)
{
    JS_ARENA_RELEASE(&cx->stackPool, mark);
}

JS_FRIEND_API(jsval *)
js_AllocStack(JSContext *cx, uintN nslots, void **markp)
{
    jsval *sp;
    JSArena *a;
    JSStackHeader *sh;

    /* Callers don't check for zero nslots: we do to avoid empty segments. */
    if (nslots == 0) {
        *markp = NULL;
        return (jsval *) JS_ARENA_MARK(&cx->stackPool);
    }

    /* Allocate 2 extra slots for the stack segment header we'll likely need. */
    sp = js_AllocRawStack(cx, 2 + nslots, markp);
    if (!sp)
        return NULL;

    /* Try to avoid another header if we can piggyback on the last segment. */
    a = cx->stackPool.current;
    sh = cx->stackHeaders;
    if (sh && JS_STACK_SEGMENT(sh) + sh->nslots == sp) {
        /* Extend the last stack segment, give back the 2 header slots. */
        sh->nslots += nslots;
        a->avail -= 2 * sizeof(jsval);
    } else {
        /*
         * Need a new stack segment, so allocate and push a stack segment
         * header from the 2 extra slots.
         */
        sh = (JSStackHeader *)sp;
        sh->nslots = nslots;
        sh->down = cx->stackHeaders;
        cx->stackHeaders = sh;
        sp += 2;
    }

    /*
     * Store JSVAL_NULL using memset, to let compilers optimize as they see
     * fit, in case a caller allocates and pushes GC-things one by one, which
     * could nest a last-ditch GC that will scan this segment.
     */
    memset(sp, 0, nslots * sizeof(jsval));
    return sp;
}

JS_FRIEND_API(void)
js_FreeStack(JSContext *cx, void *mark)
{
    JSStackHeader *sh;
    jsuword slotdiff;

    /* Check for zero nslots allocation special case. */
    if (!mark)
        return;

    /* We can assert because js_FreeStack always balances js_AllocStack. */
    sh = cx->stackHeaders;
    JS_ASSERT(sh);

    /* If mark is in the current segment, reduce sh->nslots, else pop sh. */
    slotdiff = JS_UPTRDIFF(mark, JS_STACK_SEGMENT(sh)) / sizeof(jsval);
    if (slotdiff < (jsuword)sh->nslots)
        sh->nslots = slotdiff;
    else
        cx->stackHeaders = sh->down;

    /* Release the stackPool space allocated since mark was set. */
    JS_ARENA_RELEASE(&cx->stackPool, mark);
}

JSObject *
js_GetScopeChain(JSContext *cx, JSStackFrame *fp)
{
    JSObject *obj, *cursor, *clonedChild, *parent;
    JSTempValueRooter tvr;

    obj = fp->blockChain;
    if (!obj) {
        /*
         * Don't force a call object for a lightweight function call, but do
         * insist that there is a call object for a heavyweight function call.
         */
        JS_ASSERT(!fp->fun ||
                  !(fp->fun->flags & JSFUN_HEAVYWEIGHT) ||
                  fp->callobj);
        JS_ASSERT(fp->scopeChain);
        return fp->scopeChain;
    }

    /*
     * We have one or more lexical scopes to reflect into fp->scopeChain, so
     * make sure there's a call object at the current head of the scope chain,
     * if this frame is a call frame.
     */
    if (fp->fun && !fp->callobj) {
        JS_ASSERT(OBJ_GET_CLASS(cx, fp->scopeChain) != &js_BlockClass ||
                  OBJ_GET_PRIVATE(cx, fp->scopeChain) != fp);
        if (!js_GetCallObject(cx, fp, fp->scopeChain))
            return NULL;
    }

    /*
     * Clone the block chain. To avoid recursive cloning we set the parent of
     * the cloned child after we clone the parent. In the following loop when
     * clonedChild is null it indicates the first iteration when no special GC
     * rooting is necessary. On the second and the following iterations we
     * have to protect cloned so far chain against the GC during cloning of
     * the cursor object.
     */
    cursor = obj;
    clonedChild = NULL;
    for (;;) {
        parent = OBJ_GET_PARENT(cx, cursor);

        /*
         * We pass fp->scopeChain and not null even if we override the parent
         * slot later as null triggers useless calculations of slot's value in
         * js_NewObject that js_CloneBlockObject calls.
         */
        cursor = js_CloneBlockObject(cx, cursor, fp->scopeChain, fp);
        if (!cursor) {
            if (clonedChild)
                JS_POP_TEMP_ROOT(cx, &tvr);
            return NULL;
        }
        if (!clonedChild) {
            /*
             * The first iteration. Check if other follow and root obj if so
             * to protect the whole cloned chain against GC.
             */
            obj = cursor;
            if (!parent)
                break;
            JS_PUSH_TEMP_ROOT_OBJECT(cx, obj, &tvr);
        } else {
            /*
             * Avoid OBJ_SET_PARENT overhead as clonedChild cannot escape to
             * other threads.
             */
            STOBJ_SET_PARENT(clonedChild, cursor);
            if (!parent) {
                JS_ASSERT(tvr.u.value == OBJECT_TO_JSVAL(obj));
                JS_POP_TEMP_ROOT(cx, &tvr);
                break;
            }
        }
        clonedChild = cursor;
        cursor = parent;
    }
    fp->flags |= JSFRAME_POP_BLOCKS;
    fp->scopeChain = obj;
    fp->blockChain = NULL;
    return obj;
}

JSBool
js_GetPrimitiveThis(JSContext *cx, jsval *vp, JSClass *clasp, jsval *thisvp)
{
    jsval v;
    JSObject *obj;

    v = vp[1];
    if (JSVAL_IS_OBJECT(v)) {
        obj = JS_THIS_OBJECT(cx, vp);
        if (!JS_InstanceOf(cx, obj, clasp, vp + 2))
            return JS_FALSE;
        v = OBJ_GET_SLOT(cx, obj, JSSLOT_PRIVATE);
    }
    *thisvp = v;
    return JS_TRUE;
}

/*
 * ECMA requires "the global object", but in embeddings such as the browser,
 * which have multiple top-level objects (windows, frames, etc. in the DOM),
 * we prefer fun's parent.  An example that causes this code to run:
 *
 *   // in window w1
 *   function f() { return this }
 *   function g() { return f }
 *
 *   // in window w2
 *   var h = w1.g()
 *   alert(h() == w1)
 *
 * The alert should display "true".
 */
JSObject *
js_ComputeGlobalThis(JSContext *cx, JSBool lazy, jsval *argv)
{
    JSObject *thisp;

    if (JSVAL_IS_PRIMITIVE(argv[-2]) ||
        !OBJ_GET_PARENT(cx, JSVAL_TO_OBJECT(argv[-2]))) {
        thisp = cx->globalObject;
    } else {
        JSStackFrame *fp;
        jsid id;
        jsval v;
        uintN attrs;
        JSBool ok;
        JSObject *parent;

        /*
         * Walk up the parent chain, first checking that the running script
         * has access to the callee's parent object. Note that if lazy, the
         * running script whose principals we want to check is the script
         * associated with fp->down, not with fp.
         *
         * FIXME: 417851 -- this access check should not be required, as it
         * imposes a performance penalty on all js_ComputeGlobalThis calls,
         * and it represents a maintenance hazard.
         */
        fp = cx->fp;    /* quell GCC overwarning */
        if (lazy) {
            JS_ASSERT(fp->argv == argv);
            fp->dormantNext = cx->dormantFrameChain;
            cx->dormantFrameChain = fp;
            cx->fp = fp->down;
            fp->down = NULL;
        }
        thisp = JSVAL_TO_OBJECT(argv[-2]);
        id = ATOM_TO_JSID(cx->runtime->atomState.parentAtom);

        ok = OBJ_CHECK_ACCESS(cx, thisp, id, JSACC_PARENT, &v, &attrs);
        if (lazy) {
            cx->dormantFrameChain = fp->dormantNext;
            fp->dormantNext = NULL;
            fp->down = cx->fp;
            cx->fp = fp;
        }
        if (!ok)
            return NULL;

        thisp = JSVAL_IS_VOID(v)
                ? OBJ_GET_PARENT(cx, thisp)
                : JSVAL_TO_OBJECT(v);
        while ((parent = OBJ_GET_PARENT(cx, thisp)) != NULL)
            thisp = parent;
    }

    OBJ_TO_OUTER_OBJECT(cx, thisp);
    if (!thisp)
        return NULL;
    argv[-1] = OBJECT_TO_JSVAL(thisp);
    return thisp;
}

static JSObject *
ComputeThis(JSContext *cx, JSBool lazy, jsval *argv)
{
    JSObject *thisp;

    JS_ASSERT(!JSVAL_IS_NULL(argv[-1]));
    if (!JSVAL_IS_OBJECT(argv[-1])) {
        if (!js_PrimitiveToObject(cx, &argv[-1]))
            return NULL;
        thisp = JSVAL_TO_OBJECT(argv[-1]);
    } else {
        thisp = JSVAL_TO_OBJECT(argv[-1]);
        if (OBJ_GET_CLASS(cx, thisp) == &js_CallClass ||
            OBJ_GET_CLASS(cx, thisp) == &js_BlockClass) {
            return js_ComputeGlobalThis(cx, lazy, argv);
        }

        if (thisp->map->ops->thisObject) {
            /* Some objects (e.g., With) delegate 'this' to another object. */
            thisp = thisp->map->ops->thisObject(cx, thisp);
            if (!thisp)
                return NULL;
        }
        OBJ_TO_OUTER_OBJECT(cx, thisp);
        if (!thisp)
            return NULL;
        argv[-1] = OBJECT_TO_JSVAL(thisp);
    }
    return thisp;
}

JSObject *
js_ComputeThis(JSContext *cx, JSBool lazy, jsval *argv)
{
    if (JSVAL_IS_NULL(argv[-1]))
        return js_ComputeGlobalThis(cx, lazy, argv);
    return ComputeThis(cx, lazy, argv);
}

#if JS_HAS_NO_SUCH_METHOD

#define JSSLOT_FOUND_FUNCTION   JSSLOT_PRIVATE
#define JSSLOT_SAVED_ID         (JSSLOT_PRIVATE + 1)

JSClass js_NoSuchMethodClass = {
    "NoSuchMethod",
    JSCLASS_HAS_RESERVED_SLOTS(2) | JSCLASS_IS_ANONYMOUS |
    JSCLASS_HAS_CACHED_PROTO(JSProto_NoSuchMethod),
    JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,   JS_PropertyStub,
    JS_EnumerateStub, JS_ResolveStub,   JS_ConvertStub,    JS_FinalizeStub,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

JSObject*
js_InitNoSuchMethodClass(JSContext *cx, JSObject* obj)
{
    JSObject *proto;

    proto = JS_InitClass(cx, obj, NULL, &js_NoSuchMethodClass, NULL, 0, NULL,
                         NULL, NULL, NULL);
    if (!proto)
        return NULL;

    OBJ_SET_PROTO(cx, proto, NULL);
    return proto;
}

/*
 * When JSOP_CALLPROP or JSOP_CALLELEM does not find the method property of
 * the base object, we search for the __noSuchMethod__ method in the base.
 * If it exists, we store the method and the property's id into an object of
 * NoSuchMethod class and store this object into the callee's stack slot.
 * Later, js_Invoke will recognise such an object and transfer control to
 * NoSuchMethod that invokes the method like:
 *
 *   this.__noSuchMethod__(id, args)
 *
 * where id is the name of the method that this invocation attempted to
 * call by name, and args is an Array containing this invocation's actual
 * parameters.
 */
JSBool
js_OnUnknownMethod(JSContext *cx, jsval *vp)
{
    JSObject *obj;
    jsid id;
    JSTempValueRooter tvr;
    JSBool ok;

    JS_ASSERT(!JSVAL_IS_PRIMITIVE(vp[1]));
    obj = JSVAL_TO_OBJECT(vp[1]);
    JS_PUSH_SINGLE_TEMP_ROOT(cx, JSVAL_NULL, &tvr);

    /* From here on, control must flow through label out:. */
    id = ATOM_TO_JSID(cx->runtime->atomState.noSuchMethodAtom);
#if JS_HAS_XML_SUPPORT
    if (OBJECT_IS_XML(cx, obj)) {
        JSXMLObjectOps *ops;

        ops = (JSXMLObjectOps *) obj->map->ops;
        obj = ops->getMethod(cx, obj, id, &tvr.u.value);
        if (!obj) {
            ok = JS_FALSE;
            goto out;
        }
        vp[1] = OBJECT_TO_JSVAL(obj);
    } else
#endif
    {
        ok = OBJ_GET_PROPERTY(cx, obj, id, &tvr.u.value);
        if (!ok)
            goto out;
    }
    if (JSVAL_IS_PRIMITIVE(tvr.u.value)) {
        vp[0] = tvr.u.value;
    } else {
#if JS_HAS_XML_SUPPORT
        /* Extract the function name from function::name qname. */
        if (!JSVAL_IS_PRIMITIVE(vp[0])) {
            obj = JSVAL_TO_OBJECT(vp[0]);
            ok = js_IsFunctionQName(cx, obj, &id);
            if (!ok)
                goto out;
            if (id != 0)
                vp[0] = ID_TO_VALUE(id);
        }
#endif
        obj = js_NewObject(cx, &js_NoSuchMethodClass, NULL, NULL, 0);
        if (!obj) {
            ok = JS_FALSE;
            goto out;
        }
        obj->fslots[JSSLOT_FOUND_FUNCTION] = tvr.u.value;
        obj->fslots[JSSLOT_SAVED_ID] = vp[0];
        vp[0] = OBJECT_TO_JSVAL(obj);
    }
    ok = JS_TRUE;

  out:
    JS_POP_TEMP_ROOT(cx, &tvr);
    return ok;
}

static JSBool
NoSuchMethod(JSContext *cx, uintN argc, jsval *vp, uint32 flags)
{
    jsval *invokevp;
    void *mark;
    JSBool ok;
    JSObject *obj, *argsobj;

    invokevp = js_AllocStack(cx, 2 + 2, &mark);
    if (!invokevp)
        return JS_FALSE;

    JS_ASSERT(!JSVAL_IS_PRIMITIVE(vp[0]));
    JS_ASSERT(!JSVAL_IS_PRIMITIVE(vp[1]));
    obj = JSVAL_TO_OBJECT(vp[0]);
    JS_ASSERT(STOBJ_GET_CLASS(obj) == &js_NoSuchMethodClass);

    invokevp[0] = obj->fslots[JSSLOT_FOUND_FUNCTION];
    invokevp[1] = vp[1];
    invokevp[2] = obj->fslots[JSSLOT_SAVED_ID];
    argsobj = js_NewArrayObject(cx, argc, vp + 2);
    if (!argsobj) {
        ok = JS_FALSE;
    } else {
        invokevp[3] = OBJECT_TO_JSVAL(argsobj);
        ok = (flags & JSINVOKE_CONSTRUCT)
             ? js_InvokeConstructor(cx, 2, invokevp)
             : js_Invoke(cx, 2, invokevp, flags);
        vp[0] = invokevp[0];
    }
    js_FreeStack(cx, mark);
    return ok;
}

#endif /* JS_HAS_NO_SUCH_METHOD */

/*
 * We check if the function accepts a primitive value as |this|. For that we
 * use a table that maps value's tag into the corresponding function flag.
 */
JS_STATIC_ASSERT(JSVAL_INT == 1);
JS_STATIC_ASSERT(JSVAL_DOUBLE == 2);
JS_STATIC_ASSERT(JSVAL_STRING == 4);
JS_STATIC_ASSERT(JSVAL_BOOLEAN == 6);

const uint16 js_PrimitiveTestFlags[] = {
    JSFUN_THISP_NUMBER,     /* INT     */
    JSFUN_THISP_NUMBER,     /* DOUBLE  */
    JSFUN_THISP_NUMBER,     /* INT     */
    JSFUN_THISP_STRING,     /* STRING  */
    JSFUN_THISP_NUMBER,     /* INT     */
    JSFUN_THISP_BOOLEAN,    /* BOOLEAN */
    JSFUN_THISP_NUMBER      /* INT     */
};

/*
 * Find a function reference and its 'this' object implicit first parameter
 * under argc arguments on cx's stack, and call the function.  Push missing
 * required arguments, allocate declared local variables, and pop everything
 * when done.  Then push the return value.
 */
JS_FRIEND_API(JSBool)
js_Invoke(JSContext *cx, uintN argc, jsval *vp, uintN flags)
{
    void *mark;
    JSStackFrame frame;
    jsval *sp, *argv, *newvp;
    jsval v;
    JSObject *funobj, *parent;
    JSBool ok;
    JSClass *clasp;
    JSObjectOps *ops;
    JSNative native;
    JSFunction *fun;
    JSScript *script;
    uintN nslots, nvars, i, skip;
    uint32 rootedArgsFlag;
    JSInterpreterHook hook;
    void *hookData;

    /* [vp .. vp + 2 + argc) must belong to the last JS stack arena. */
    JS_ASSERT((jsval *) cx->stackPool.current->base <= vp);
    JS_ASSERT(vp + 2 + argc <= (jsval *) cx->stackPool.current->avail);

    /*
     * Mark the top of stack and load frequently-used registers. After this
     * point the control should flow through label out2: to return.
     */
    mark = JS_ARENA_MARK(&cx->stackPool);
    v = *vp;

    if (JSVAL_IS_PRIMITIVE(v))
        goto bad;

    funobj = JSVAL_TO_OBJECT(v);
    parent = OBJ_GET_PARENT(cx, funobj);
    clasp = OBJ_GET_CLASS(cx, funobj);
    if (clasp != &js_FunctionClass) {
#if JS_HAS_NO_SUCH_METHOD
        if (clasp == &js_NoSuchMethodClass) {
            ok = NoSuchMethod(cx, argc, vp, flags);
            goto out2;
        }
#endif

        /* Function is inlined, all other classes use object ops. */
        ops = funobj->map->ops;

        /*
         * XXX this makes no sense -- why convert to function if clasp->call?
         * XXX better to call that hook without converting
         * XXX the only thing that needs fixing is liveconnect
         *
         * Try converting to function, for closure and API compatibility.
         * We attempt the conversion under all circumstances for 1.2, but
         * only if there is a call op defined otherwise.
         */
        if ((ops == &js_ObjectOps) ? clasp->call : ops->call) {
            ok = clasp->convert(cx, funobj, JSTYPE_FUNCTION, &v);
            if (!ok)
                goto out2;

            if (VALUE_IS_FUNCTION(cx, v)) {
                /* Make vp refer to funobj to keep it available as argv[-2]. */
                *vp = v;
                funobj = JSVAL_TO_OBJECT(v);
                parent = OBJ_GET_PARENT(cx, funobj);
                goto have_fun;
            }
        }
        fun = NULL;
        script = NULL;
        nslots = nvars = 0;

        /* Try a call or construct native object op. */
        if (flags & JSINVOKE_CONSTRUCT) {
            if (!JSVAL_IS_OBJECT(vp[1])) {
                ok = js_PrimitiveToObject(cx, &vp[1]);
                if (!ok)
                    goto out2;
            }
            native = ops->construct;
        } else {
            native = ops->call;
        }
        if (!native)
            goto bad;
    } else {
have_fun:
        /* Get private data and set derived locals from it. */
        fun = GET_FUNCTION_PRIVATE(cx, funobj);
        nslots = FUN_MINARGS(fun);
        nslots = (nslots > argc) ? nslots - argc : 0;
        if (FUN_INTERPRETED(fun)) {
            native = NULL;
            script = fun->u.i.script;
            nvars = fun->u.i.nvars;
        } else {
            native = fun->u.n.native;
            script = NULL;
            nvars = 0;
            nslots += fun->u.n.extra;
        }

        if (JSFUN_BOUND_METHOD_TEST(fun->flags)) {
            /* Handle bound method special case. */
            vp[1] = OBJECT_TO_JSVAL(parent);
        } else if (!JSVAL_IS_OBJECT(vp[1])) {
            JS_ASSERT(!(flags & JSINVOKE_CONSTRUCT));
            if (PRIMITIVE_THIS_TEST(fun, vp[1]))
                goto init_slots;
        }
    }

    if (flags & JSINVOKE_CONSTRUCT) {
        JS_ASSERT(!JSVAL_IS_PRIMITIVE(vp[1]));
    } else {
        /*
         * We must call js_ComputeThis in case we are not called from the
         * interpreter, where a prior bytecode has computed an appropriate
         * |this| already.
         *
         * But we need to compute |this| eagerly only for so-called "slow"
         * (i.e., not fast) native functions. Fast natives must use either
         * JS_THIS or JS_THIS_OBJECT, and scripted functions will go through
         * the appropriate this-computing bytecode, e.g., JSOP_THIS.
         */
        if (native && (!fun || !(fun->flags & JSFUN_FAST_NATIVE))) {
            if (!js_ComputeThis(cx, JS_FALSE, vp + 2)) {
                ok = JS_FALSE;
                goto out2;
            }
            flags |= JSFRAME_COMPUTED_THIS;
        }
    }

  init_slots:
    argv = vp + 2;
    sp = argv + argc;

    rootedArgsFlag = JSFRAME_ROOTED_ARGV;
    if (nslots != 0) {
        /*
         * The extra slots required by the function continue with argument
         * slots. Thus, when the last stack pool arena does not have room to
         * fit nslots right after sp and AllocateAfterSP fails, we have to copy
         * [vp..vp+2+argc) slots and clear rootedArgsFlag to root the copy.
         */
        if (!AllocateAfterSP(cx, sp, nslots)) {
            rootedArgsFlag = 0;
            newvp = js_AllocRawStack(cx, 2 + argc + nslots, NULL);
            if (!newvp) {
                ok = JS_FALSE;
                goto out2;
            }
            memcpy(newvp, vp, (2 + argc) * sizeof(jsval));
            argv = newvp + 2;
            sp = argv + argc;
        }

        /* Push void to initialize missing args. */
        i = nslots;
        do {
            *sp++ = JSVAL_VOID;
        } while (--i != 0);
    }

    if (native && fun && (fun->flags & JSFUN_FAST_NATIVE)) {
        JSTempValueRooter tvr;
#ifdef DEBUG_NOT_THROWING
        JSBool alreadyThrowing = cx->throwing;
#endif
#if JS_HAS_LVALUE_RETURN
        /* Set by JS_SetCallReturnValue2, used to return reference types. */
        cx->rval2set = JS_FALSE;
#endif
        /* Root the slots that are not covered by [vp..vp+2+argc). */
        skip = rootedArgsFlag ? 2 + argc : 0;
        JS_PUSH_TEMP_ROOT(cx, 2 + argc + nslots - skip, argv - 2 + skip, &tvr);
        ok = ((JSFastNative) native)(cx, argc, argv - 2);

        /*
         * To avoid extra checks we always copy the result to *vp even if we
         * have not copied argv and vp == argv - 2.
         */
        *vp = argv[-2];
        JS_POP_TEMP_ROOT(cx, &tvr);

        JS_RUNTIME_METER(cx->runtime, nativeCalls);
#ifdef DEBUG_NOT_THROWING
        if (ok && !alreadyThrowing)
            ASSERT_NOT_THROWING(cx);
#endif
        goto out2;
    }

    /* Now allocate stack space for local variables of interpreted function. */
    if (nvars) {
        if (!AllocateAfterSP(cx, sp, nvars)) {
            /* NB: Discontinuity between argv and vars. */
            sp = js_AllocRawStack(cx, nvars, NULL);
            if (!sp) {
                ok = JS_FALSE;
                goto out2;
            }
        }

        /* Push void to initialize local variables. */
        i = nvars;
        do {
            *sp++ = JSVAL_VOID;
        } while (--i != 0);
    }

    /*
     * Initialize the frame.
     *
     * To set thisp we use an explicit cast and not JSVAL_TO_OBJECT, as vp[1]
     * can be a primitive value here for those native functions specified with
     * JSFUN_THISP_(NUMBER|STRING|BOOLEAN) flags.
     */
    frame.thisp = (JSObject *)vp[1];
    frame.varobj = NULL;
    frame.callobj = frame.argsobj = NULL;
    frame.script = script;
    frame.callee = funobj;
    frame.fun = fun;
    frame.argc = argc;
    frame.argv = argv;

    /* Default return value for a constructor is the new object. */
    frame.rval = (flags & JSINVOKE_CONSTRUCT) ? vp[1] : JSVAL_VOID;
    frame.nvars = nvars;
    frame.vars = sp - nvars;
    frame.down = cx->fp;
    frame.annotation = NULL;
    frame.scopeChain = NULL;    /* set below for real, after cx->fp is set */
    frame.regs = NULL;
    frame.spbase = NULL;
    frame.sharpDepth = 0;
    frame.sharpArray = NULL;
    frame.flags = flags | rootedArgsFlag;
    frame.dormantNext = NULL;
    frame.xmlNamespace = NULL;
    frame.blockChain = NULL;

    /* From here on, control must flow through label out: to return. */
    cx->fp = &frame;

    /* Init these now in case we goto out before first hook call. */
    hook = cx->debugHooks->callHook;
    hookData = NULL;

    /* call the hook if present */
    if (hook && (native || script))
        hookData = hook(cx, &frame, JS_TRUE, 0, cx->debugHooks->callHookData);

    /* Call the function, either a native method or an interpreted script. */
    if (native) {
#ifdef DEBUG_NOT_THROWING
        JSBool alreadyThrowing = cx->throwing;
#endif

#if JS_HAS_LVALUE_RETURN
        /* Set by JS_SetCallReturnValue2, used to return reference types. */
        cx->rval2set = JS_FALSE;
#endif

        /* If native, use caller varobj and scopeChain for eval. */
        JS_ASSERT(!frame.varobj);
        JS_ASSERT(!frame.scopeChain);
        if (frame.down) {
            frame.varobj = frame.down->varobj;
            frame.scopeChain = frame.down->scopeChain;
        }

        /* But ensure that we have a scope chain. */
        if (!frame.scopeChain)
            frame.scopeChain = parent;

        ok = native(cx, frame.thisp, argc, frame.argv, &frame.rval);
        JS_RUNTIME_METER(cx->runtime, nativeCalls);
#ifdef DEBUG_NOT_THROWING
        if (ok && !alreadyThrowing)
            ASSERT_NOT_THROWING(cx);
#endif
    } else if (script) {
        /* Use parent scope so js_GetCallObject can find the right "Call". */
        frame.scopeChain = parent;
        if (JSFUN_HEAVYWEIGHT_TEST(fun->flags)) {
            /* Scope with a call object parented by the callee's parent. */
            if (!js_GetCallObject(cx, &frame, parent)) {
                ok = JS_FALSE;
                goto out;
            }
        }
        ok = js_Interpret(cx);
    } else {
        /* fun might be onerror trying to report a syntax error in itself. */
        frame.scopeChain = NULL;
        ok = JS_TRUE;
    }

out:
    if (hookData) {
        hook = cx->debugHooks->callHook;
        if (hook)
            hook(cx, &frame, JS_FALSE, &ok, hookData);
    }

    /* If frame has a call object, sync values and clear back-pointer. */
    if (frame.callobj)
        ok &= js_PutCallObject(cx, &frame);

    /* If frame has an arguments object, sync values and clear back-pointer. */
    if (frame.argsobj)
        ok &= js_PutArgsObject(cx, &frame);

    *vp = frame.rval;

    /* Restore cx->fp now that we're done releasing frame objects. */
    cx->fp = frame.down;

out2:
    /* Pop everything we may have allocated off the stack. */
    JS_ARENA_RELEASE(&cx->stackPool, mark);
    if (!ok)
        *vp = JSVAL_NULL;
    return ok;

bad:
    js_ReportIsNotFunction(cx, vp, flags & JSINVOKE_FUNFLAGS);
    ok = JS_FALSE;
    goto out2;
}

JSBool
js_InternalInvoke(JSContext *cx, JSObject *obj, jsval fval, uintN flags,
                  uintN argc, jsval *argv, jsval *rval)
{
    jsval *invokevp;
    void *mark;
    JSBool ok;

    invokevp = js_AllocStack(cx, 2 + argc, &mark);
    if (!invokevp)
        return JS_FALSE;

    invokevp[0] = fval;
    invokevp[1] = OBJECT_TO_JSVAL(obj);
    memcpy(invokevp + 2, argv, argc * sizeof *argv);

    ok = js_Invoke(cx, argc, invokevp, flags);
    if (ok) {
        /*
         * Store *rval in the a scoped local root if a scope is open, else in
         * the lastInternalResult pigeon-hole GC root, solely so users of
         * js_InternalInvoke and its direct and indirect (js_ValueToString for
         * example) callers do not need to manage roots for local, temporary
         * references to such results.
         */
        *rval = *invokevp;
        if (JSVAL_IS_GCTHING(*rval) && *rval != JSVAL_NULL) {
            if (cx->localRootStack) {
                if (js_PushLocalRoot(cx, cx->localRootStack, *rval) < 0)
                    ok = JS_FALSE;
            } else {
                cx->weakRoots.lastInternalResult = *rval;
            }
        }
    }

    js_FreeStack(cx, mark);
    return ok;
}

JSBool
js_InternalGetOrSet(JSContext *cx, JSObject *obj, jsid id, jsval fval,
                    JSAccessMode mode, uintN argc, jsval *argv, jsval *rval)
{
    /*
     * js_InternalInvoke could result in another try to get or set the same id
     * again, see bug 355497.
     */
    JS_CHECK_RECURSION(cx, return JS_FALSE);

    /*
     * Check general (not object-ops/class-specific) access from the running
     * script to obj.id only if id has a scripted getter or setter that we're
     * about to invoke.  If we don't check this case, nothing else will -- no
     * other native code has the chance to check.
     *
     * Contrast this non-native (scripted) case with native getter and setter
     * accesses, where the native itself must do an access check, if security
     * policies requires it.  We make a checkAccess or checkObjectAccess call
     * back to the embedding program only in those cases where we're not going
     * to call an embedding-defined native function, getter, setter, or class
     * hook anyway.  Where we do call such a native, there's no need for the
     * engine to impose a separate access check callback on all embeddings --
     * many embeddings have no security policy at all.
     */
    JS_ASSERT(mode == JSACC_READ || mode == JSACC_WRITE);
    if (cx->runtime->checkObjectAccess &&
        VALUE_IS_FUNCTION(cx, fval) &&
        FUN_INTERPRETED(GET_FUNCTION_PRIVATE(cx, JSVAL_TO_OBJECT(fval))) &&
        !cx->runtime->checkObjectAccess(cx, obj, ID_TO_VALUE(id), mode,
                                        &fval)) {
        return JS_FALSE;
    }

    return js_InternalCall(cx, obj, fval, argc, argv, rval);
}

JSBool
js_Execute(JSContext *cx, JSObject *chain, JSScript *script,
           JSStackFrame *down, uintN flags, jsval *result)
{
    JSInterpreterHook hook;
    void *hookData, *mark;
    JSStackFrame *oldfp, frame;
    JSObject *obj, *tmp;
    JSBool ok;

#ifdef INCLUDE_MOZILLA_DTRACE
    if (JAVASCRIPT_EXECUTE_START_ENABLED())
        jsdtrace_execute_start(script);
#endif

    hook = cx->debugHooks->executeHook;
    hookData = mark = NULL;
    oldfp = cx->fp;
    frame.script = script;
    if (down) {
        /* Propagate arg/var state for eval and the debugger API. */
        frame.callobj = down->callobj;
        frame.argsobj = down->argsobj;
        frame.varobj = down->varobj;
        frame.callee = down->callee;
        frame.fun = down->fun;
        frame.thisp = down->thisp;
        if (down->flags & JSFRAME_COMPUTED_THIS)
            flags |= JSFRAME_COMPUTED_THIS;
        frame.argc = down->argc;
        frame.argv = down->argv;
        frame.nvars = down->nvars;
        frame.vars = down->vars;
        frame.annotation = down->annotation;
        frame.sharpArray = down->sharpArray;
    } else {
        frame.callobj = frame.argsobj = NULL;
        obj = chain;
        if (cx->options & JSOPTION_VAROBJFIX) {
            while ((tmp = OBJ_GET_PARENT(cx, obj)) != NULL)
                obj = tmp;
        }
        frame.varobj = obj;
        frame.callee = NULL;
        frame.fun = NULL;
        frame.thisp = chain;
        frame.argc = 0;
        frame.argv = NULL;
        frame.nvars = script->ngvars;
        if (script->regexpsOffset != 0)
            frame.nvars += JS_SCRIPT_REGEXPS(script)->length;
        if (frame.nvars != 0) {
            frame.vars = js_AllocRawStack(cx, frame.nvars, &mark);
            if (!frame.vars) {
                ok = JS_FALSE;
                goto out;
            }
            memset(frame.vars, 0, frame.nvars * sizeof(jsval));
        } else {
            frame.vars = NULL;
        }
        frame.annotation = NULL;
        frame.sharpArray = NULL;
    }
    frame.rval = JSVAL_VOID;
    frame.down = down;
    frame.scopeChain = chain;
    frame.regs = NULL;
    frame.spbase = NULL;
    frame.sharpDepth = 0;
    frame.flags = flags;
    frame.dormantNext = NULL;
    frame.xmlNamespace = NULL;
    frame.blockChain = NULL;

    /*
     * Here we wrap the call to js_Interpret with code to (conditionally)
     * save and restore the old stack frame chain into a chain of 'dormant'
     * frame chains.  Since we are replacing cx->fp, we were running into
     * the problem that if GC was called under this frame, some of the GC
     * things associated with the old frame chain (available here only in
     * the C variable 'oldfp') were not rooted and were being collected.
     *
     * So, now we preserve the links to these 'dormant' frame chains in cx
     * before calling js_Interpret and cleanup afterwards.  The GC walks
     * these dormant chains and marks objects in the same way that it marks
     * objects in the primary cx->fp chain.
     */
    if (oldfp && oldfp != down) {
        JS_ASSERT(!oldfp->dormantNext);
        oldfp->dormantNext = cx->dormantFrameChain;
        cx->dormantFrameChain = oldfp;
    }

    cx->fp = &frame;
    if (!down) {
        OBJ_TO_OUTER_OBJECT(cx, frame.thisp);
        if (!frame.thisp) {
            ok = JS_FALSE;
            goto out2;
        }
        frame.flags |= JSFRAME_COMPUTED_THIS;
    }

    if (hook) {
        hookData = hook(cx, &frame, JS_TRUE, 0,
                        cx->debugHooks->executeHookData);
    }

    ok = js_Interpret(cx);
    *result = frame.rval;

    if (hookData) {
        hook = cx->debugHooks->executeHook;
        if (hook)
            hook(cx, &frame, JS_FALSE, &ok, hookData);
    }

out2:
    if (mark)
        js_FreeRawStack(cx, mark);
    cx->fp = oldfp;

    if (oldfp && oldfp != down) {
        JS_ASSERT(cx->dormantFrameChain == oldfp);
        cx->dormantFrameChain = oldfp->dormantNext;
        oldfp->dormantNext = NULL;
    }

out:
#ifdef INCLUDE_MOZILLA_DTRACE
    if (JAVASCRIPT_EXECUTE_DONE_ENABLED())
        jsdtrace_execute_done(script);
#endif
    return ok;
}

#if JS_HAS_EXPORT_IMPORT
/*
 * If id is JSVAL_VOID, import all exported properties from obj.
 */
JSBool
js_ImportProperty(JSContext *cx, JSObject *obj, jsid id)
{
    JSBool ok;
    JSIdArray *ida;
    JSProperty *prop;
    JSObject *obj2, *target, *funobj, *closure;
    uintN attrs;
    jsint i;
    jsval value;

    if (JSVAL_IS_VOID(id)) {
        ida = JS_Enumerate(cx, obj);
        if (!ida)
            return JS_FALSE;
        ok = JS_TRUE;
        if (ida->length == 0)
            goto out;
    } else {
        ida = NULL;
        if (!OBJ_LOOKUP_PROPERTY(cx, obj, id, &obj2, &prop))
            return JS_FALSE;
        if (!prop) {
            js_ReportValueError(cx, JSMSG_NOT_DEFINED,
                                JSDVG_IGNORE_STACK, ID_TO_VALUE(id), NULL);
            return JS_FALSE;
        }
        ok = OBJ_GET_ATTRIBUTES(cx, obj, id, prop, &attrs);
        OBJ_DROP_PROPERTY(cx, obj2, prop);
        if (!ok)
            return JS_FALSE;
        if (!(attrs & JSPROP_EXPORTED)) {
            js_ReportValueError(cx, JSMSG_NOT_EXPORTED,
                                JSDVG_IGNORE_STACK, ID_TO_VALUE(id), NULL);
            return JS_FALSE;
        }
    }

    target = cx->fp->varobj;
    i = 0;
    do {
        if (ida) {
            id = ida->vector[i];
            ok = OBJ_GET_ATTRIBUTES(cx, obj, id, NULL, &attrs);
            if (!ok)
                goto out;
            if (!(attrs & JSPROP_EXPORTED))
                continue;
        }
        ok = OBJ_CHECK_ACCESS(cx, obj, id, JSACC_IMPORT, &value, &attrs);
        if (!ok)
            goto out;
        if (VALUE_IS_FUNCTION(cx, value)) {
            funobj = JSVAL_TO_OBJECT(value);
            closure = js_CloneFunctionObject(cx,
                                             GET_FUNCTION_PRIVATE(cx, funobj),
                                             obj);
            if (!closure) {
                ok = JS_FALSE;
                goto out;
            }
            value = OBJECT_TO_JSVAL(closure);
        }

        /*
         * Handle the case of importing a property that refers to a local
         * variable or formal parameter of a function activation.  These
         * properties are accessed by opcodes using stack slot numbers
         * generated by the compiler rather than runtime name-lookup.  These
         * local references, therefore, bypass the normal scope chain lookup.
         * So, instead of defining a new property in the activation object,
         * modify the existing value in the stack slot.
         */
        if (OBJ_GET_CLASS(cx, target) == &js_CallClass) {
            ok = OBJ_LOOKUP_PROPERTY(cx, target, id, &obj2, &prop);
            if (!ok)
                goto out;
        } else {
            prop = NULL;
        }
        if (prop && target == obj2) {
            ok = OBJ_SET_PROPERTY(cx, target, id, &value);
        } else {
            ok = OBJ_DEFINE_PROPERTY(cx, target, id, value,
                                     JS_PropertyStub, JS_PropertyStub,
                                     attrs & ~(JSPROP_EXPORTED |
                                               JSPROP_GETTER |
                                               JSPROP_SETTER),
                                     NULL);
        }
        if (prop)
            OBJ_DROP_PROPERTY(cx, obj2, prop);
        if (!ok)
            goto out;
    } while (ida && ++i < ida->length);

out:
    if (ida)
        JS_DestroyIdArray(cx, ida);
    return ok;
}
#endif /* JS_HAS_EXPORT_IMPORT */

JSBool
js_CheckRedeclaration(JSContext *cx, JSObject *obj, jsid id, uintN attrs,
                      JSObject **objp, JSProperty **propp)
{
    JSObject *obj2;
    JSProperty *prop;
    uintN oldAttrs, report;
    JSBool isFunction;
    jsval value;
    const char *type, *name;

    /*
     * Both objp and propp must be either null or given. When given, *propp
     * must be null. This way we avoid an extra "if (propp) *propp = NULL" for
     * the common case of a non-existing property.
     */
    JS_ASSERT(!objp == !propp);
    JS_ASSERT_IF(propp, !*propp);

    /* The JSPROP_INITIALIZER case below may generate a warning. Since we must
     * drop the property before reporting it, we insists on !propp to avoid
     * looking up the property again after the reporting is done.
     */
    JS_ASSERT_IF(attrs & JSPROP_INITIALIZER, attrs == JSPROP_INITIALIZER);
    JS_ASSERT_IF(attrs == JSPROP_INITIALIZER, !propp);

    if (!OBJ_LOOKUP_PROPERTY(cx, obj, id, &obj2, &prop))
        return JS_FALSE;
    if (!prop)
        return JS_TRUE;

    /* Use prop as a speedup hint to OBJ_GET_ATTRIBUTES. */
    if (!OBJ_GET_ATTRIBUTES(cx, obj2, id, prop, &oldAttrs)) {
        OBJ_DROP_PROPERTY(cx, obj2, prop);
        return JS_FALSE;
    }

    /*
     * If our caller doesn't want prop, drop it (we don't need it any longer).
     */
    if (!propp) {
        OBJ_DROP_PROPERTY(cx, obj2, prop);
        prop = NULL;
    } else {
        *objp = obj2;
        *propp = prop;
    }

    if (attrs == JSPROP_INITIALIZER) {
        /* Allow the new object to override properties. */
        if (obj2 != obj)
            return JS_TRUE;

        /* The property must be dropped already. */
        JS_ASSERT(!prop);
        report = JSREPORT_WARNING | JSREPORT_STRICT;
    } else {
        /* We allow redeclaring some non-readonly properties. */
        if (((oldAttrs | attrs) & JSPROP_READONLY) == 0) {
            /* Allow redeclaration of variables and functions. */
            if (!(attrs & (JSPROP_GETTER | JSPROP_SETTER)))
                return JS_TRUE;

            /*
             * Allow adding a getter only if a property already has a setter
             * but no getter and similarly for adding a setter. That is, we
             * allow only the following transitions:
             *
             *   no-property --> getter --> getter + setter
             *   no-property --> setter --> getter + setter
             */
            if ((~(oldAttrs ^ attrs) & (JSPROP_GETTER | JSPROP_SETTER)) == 0)
                return JS_TRUE;

            /*
             * Allow redeclaration of an impermanent property (in which case
             * anyone could delete it and redefine it, willy-nilly).
             */
            if (!(oldAttrs & JSPROP_PERMANENT))
                return JS_TRUE;
        }
        if (prop)
            OBJ_DROP_PROPERTY(cx, obj2, prop);

        report = JSREPORT_ERROR;
        isFunction = (oldAttrs & (JSPROP_GETTER | JSPROP_SETTER)) != 0;
        if (!isFunction) {
            if (!OBJ_GET_PROPERTY(cx, obj, id, &value))
                return JS_FALSE;
            isFunction = VALUE_IS_FUNCTION(cx, value);
        }
    }

    type = (attrs == JSPROP_INITIALIZER)
           ? "property"
           : (oldAttrs & attrs & JSPROP_GETTER)
           ? js_getter_str
           : (oldAttrs & attrs & JSPROP_SETTER)
           ? js_setter_str
           : (oldAttrs & JSPROP_READONLY)
           ? js_const_str
           : isFunction
           ? js_function_str
           : js_var_str;
    name = js_ValueToPrintableString(cx, ID_TO_VALUE(id));
    if (!name)
        return JS_FALSE;
    return JS_ReportErrorFlagsAndNumber(cx, report,
                                        js_GetErrorMessage, NULL,
                                        JSMSG_REDECLARED_VAR,
                                        type, name);
}

JSBool
js_StrictlyEqual(JSContext *cx, jsval lval, jsval rval)
{
    jsval ltag = JSVAL_TAG(lval), rtag = JSVAL_TAG(rval);
    jsdouble ld, rd;

    if (ltag == rtag) {
        if (ltag == JSVAL_STRING) {
            JSString *lstr = JSVAL_TO_STRING(lval),
                     *rstr = JSVAL_TO_STRING(rval);
            return js_EqualStrings(lstr, rstr);
        }
        if (ltag == JSVAL_DOUBLE) {
            ld = *JSVAL_TO_DOUBLE(lval);
            rd = *JSVAL_TO_DOUBLE(rval);
            return JSDOUBLE_COMPARE(ld, ==, rd, JS_FALSE);
        }
        if (ltag == JSVAL_OBJECT &&
            lval != rval &&
            !JSVAL_IS_NULL(lval) &&
            !JSVAL_IS_NULL(rval)) {
            JSObject *lobj, *robj;

            lobj = js_GetWrappedObject(cx, JSVAL_TO_OBJECT(lval));
            robj = js_GetWrappedObject(cx, JSVAL_TO_OBJECT(rval));
            lval = OBJECT_TO_JSVAL(lobj);
            rval = OBJECT_TO_JSVAL(robj);
        }
        return lval == rval;
    }
    if (ltag == JSVAL_DOUBLE && JSVAL_IS_INT(rval)) {
        ld = *JSVAL_TO_DOUBLE(lval);
        rd = JSVAL_TO_INT(rval);
        return JSDOUBLE_COMPARE(ld, ==, rd, JS_FALSE);
    }
    if (JSVAL_IS_INT(lval) && rtag == JSVAL_DOUBLE) {
        ld = JSVAL_TO_INT(lval);
        rd = *JSVAL_TO_DOUBLE(rval);
        return JSDOUBLE_COMPARE(ld, ==, rd, JS_FALSE);
    }
    return lval == rval;
}

JSBool
js_InvokeConstructor(JSContext *cx, uintN argc, jsval *vp)
{
    JSFunction *fun, *fun2;
    JSObject *obj, *obj2, *proto, *parent;
    jsval lval, rval;
    JSClass *clasp;

    fun = NULL;
    obj2 = NULL;
    lval = *vp;
    if (!JSVAL_IS_OBJECT(lval) ||
        (obj2 = JSVAL_TO_OBJECT(lval)) == NULL ||
        /* XXX clean up to avoid special cases above ObjectOps layer */
        OBJ_GET_CLASS(cx, obj2) == &js_FunctionClass ||
        !obj2->map->ops->construct)
    {
        fun = js_ValueToFunction(cx, vp, JSV2F_CONSTRUCT);
        if (!fun)
            return JS_FALSE;
    }

    clasp = &js_ObjectClass;
    if (!obj2) {
        proto = parent = NULL;
        fun = NULL;
    } else {
        /*
         * Get the constructor prototype object for this function.
         * Use the nominal 'this' parameter slot, vp[1], as a local
         * root to protect this prototype, in case it has no other
         * strong refs.
         */
        if (!OBJ_GET_PROPERTY(cx, obj2,
                              ATOM_TO_JSID(cx->runtime->atomState
                                           .classPrototypeAtom),
                              &vp[1])) {
            return JS_FALSE;
        }
        rval = vp[1];
        proto = JSVAL_IS_OBJECT(rval) ? JSVAL_TO_OBJECT(rval) : NULL;
        parent = OBJ_GET_PARENT(cx, obj2);

        if (OBJ_GET_CLASS(cx, obj2) == &js_FunctionClass) {
            fun2 = GET_FUNCTION_PRIVATE(cx, obj2);
            if (!FUN_INTERPRETED(fun2) && fun2->u.n.clasp)
                clasp = fun2->u.n.clasp;
        }
    }
    obj = js_NewObject(cx, clasp, proto, parent, 0);
    if (!obj)
        return JS_FALSE;

    /* Now we have an object with a constructor method; call it. */
    vp[1] = OBJECT_TO_JSVAL(obj);
    if (!js_Invoke(cx, argc, vp, JSINVOKE_CONSTRUCT)) {
        cx->weakRoots.newborn[GCX_OBJECT] = NULL;
        return JS_FALSE;
    }

    /* Check the return value and if it's primitive, force it to be obj. */
    rval = *vp;
    if (JSVAL_IS_PRIMITIVE(rval)) {
        if (!fun) {
            /* native [[Construct]] returning primitive is error */
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_BAD_NEW_RESULT,
                                 js_ValueToPrintableString(cx, rval));
            return JS_FALSE;
        }
        *vp = OBJECT_TO_JSVAL(obj);
    }

    JS_RUNTIME_METER(cx->runtime, constructs);
    return JS_TRUE;
}

JSBool
js_InternNonIntElementId(JSContext *cx, JSObject *obj, jsval idval, jsid *idp)
{
    JS_ASSERT(!JSVAL_IS_INT(idval));

#if JS_HAS_XML_SUPPORT
    if (!JSVAL_IS_PRIMITIVE(idval)) {
        if (OBJECT_IS_XML(cx, obj)) {
            *idp = OBJECT_JSVAL_TO_JSID(idval);
            return JS_TRUE;
        }
        if (!js_IsFunctionQName(cx, JSVAL_TO_OBJECT(idval), idp))
            return JS_FALSE;
        if (*idp != 0)
            return JS_TRUE;
    }
#endif

    return js_ValueToStringId(cx, idval, idp);
}

/*
 * Enter the new with scope using an object at sp[-1] and associate the depth
 * of the with block with sp + stackIndex.
 */
JSBool
js_EnterWith(JSContext *cx, jsint stackIndex)
{
    JSStackFrame *fp;
    jsval *sp;
    JSObject *obj, *parent, *withobj;

    fp = cx->fp;
    sp = fp->regs->sp;
    JS_ASSERT(stackIndex < 0);
    JS_ASSERT(fp->spbase <= sp + stackIndex);

    if (!JSVAL_IS_PRIMITIVE(sp[-1])) {
        obj = JSVAL_TO_OBJECT(sp[-1]);
    } else {
        obj = js_ValueToNonNullObject(cx, sp[-1]);
        if (!obj)
            return JS_FALSE;
        sp[-1] = OBJECT_TO_JSVAL(obj);
    }

    parent = js_GetScopeChain(cx, fp);
    if (!parent)
        return JS_FALSE;

    OBJ_TO_INNER_OBJECT(cx, obj);
    if (!obj)
        return JS_FALSE;

    withobj = js_NewWithObject(cx, obj, parent,
                               sp + stackIndex - fp->spbase);
    if (!withobj)
        return JS_FALSE;

    fp->scopeChain = withobj;
    js_DisablePropertyCache(cx);
    return JS_TRUE;
}

void
js_LeaveWith(JSContext *cx)
{
    JSObject *withobj;

    withobj = cx->fp->scopeChain;
    JS_ASSERT(OBJ_GET_CLASS(cx, withobj) == &js_WithClass);
    JS_ASSERT(OBJ_GET_PRIVATE(cx, withobj) == cx->fp);
    JS_ASSERT(OBJ_BLOCK_DEPTH(cx, withobj) >= 0);
    cx->fp->scopeChain = OBJ_GET_PARENT(cx, withobj);
    JS_SetPrivate(cx, withobj, NULL);
    js_EnablePropertyCache(cx);
}

JSClass *
js_IsActiveWithOrBlock(JSContext *cx, JSObject *obj, int stackDepth)
{
    JSClass *clasp;

    clasp = OBJ_GET_CLASS(cx, obj);
    if ((clasp == &js_WithClass || clasp == &js_BlockClass) &&
        OBJ_GET_PRIVATE(cx, obj) == cx->fp &&
        OBJ_BLOCK_DEPTH(cx, obj) >= stackDepth) {
        return clasp;
    }
    return NULL;
}

jsint
js_CountWithBlocks(JSContext *cx, JSStackFrame *fp)
{
    jsint n;
    JSObject *obj;
    JSClass *clasp;

    n = 0;
    for (obj = fp->scopeChain;
         (clasp = js_IsActiveWithOrBlock(cx, obj, 0)) != NULL;
         obj = OBJ_GET_PARENT(cx, obj)) {
        if (clasp == &js_WithClass)
            ++n;
    }
    return n;
}

/*
 * Unwind block and scope chains to match the given depth. The function sets
 * fp->sp on return to stackDepth.
 */
JSBool
js_UnwindScope(JSContext *cx, JSStackFrame *fp, jsint stackDepth,
               JSBool normalUnwind)
{
    JSObject *obj;
    JSClass *clasp;

    JS_ASSERT(stackDepth >= 0);
    JS_ASSERT(fp->spbase + stackDepth <= fp->regs->sp);

    for (obj = fp->blockChain; obj; obj = OBJ_GET_PARENT(cx, obj)) {
        JS_ASSERT(OBJ_GET_CLASS(cx, obj) == &js_BlockClass);
        if (OBJ_BLOCK_DEPTH(cx, obj) < stackDepth)
            break;
    }
    fp->blockChain = obj;

    for (;;) {
        obj = fp->scopeChain;
        clasp = js_IsActiveWithOrBlock(cx, obj, stackDepth);
        if (!clasp)
            break;
        if (clasp == &js_BlockClass) {
            /* Don't fail until after we've updated all stacks. */
            normalUnwind &= js_PutBlockObject(cx, normalUnwind);
        } else {
            js_LeaveWith(cx);
        }
    }

    fp->regs->sp = fp->spbase + stackDepth;
    return normalUnwind;
}

JSBool
js_DoIncDec(JSContext *cx, const JSCodeSpec *cs, jsval *vp, jsval *vp2)
{
    jsval v;
    jsdouble d;

    v = *vp;
    if (JSVAL_IS_DOUBLE(v)) {
        d = *JSVAL_TO_DOUBLE(v);
    } else if (JSVAL_IS_INT(v)) {
        d = JSVAL_TO_INT(v);
    } else {
        d = js_ValueToNumber(cx, vp);
        if (JSVAL_IS_NULL(*vp))
            return JS_FALSE;
        JS_ASSERT(JSVAL_IS_NUMBER(*vp) || *vp == JSVAL_TRUE);

        /* Store the result of v conversion back in vp for post increments. */
        if ((cs->format & JOF_POST) &&
            *vp == JSVAL_TRUE
            && !js_NewNumberInRootedValue(cx, d, vp)) {
            return JS_FALSE;
        }
    }

    (cs->format & JOF_INC) ? d++ : d--;
    if (!js_NewNumberInRootedValue(cx, d, vp2))
        return JS_FALSE;

    if (!(cs->format & JOF_POST))
        *vp = *vp2;
    return JS_TRUE;
}

#ifdef JS_OPMETER

# include <stdlib.h>

# define HIST_NSLOTS            8

/*
 * The second dimension is hardcoded at 256 because we know that many bits fit
 * in a byte, and mainly to optimize away multiplying by JSOP_LIMIT to address
 * any particular row.
 */
static uint32 succeeds[JSOP_LIMIT][256];
static uint32 slot_ops[JSOP_LIMIT][HIST_NSLOTS];

void
js_MeterOpcodePair(JSOp op1, JSOp op2)
{
    if (op1 != JSOP_STOP)
        ++succeeds[op1][op2];
}

void
js_MeterSlotOpcode(JSOp op, uint32 slot)
{
    if (slot < HIST_NSLOTS)
        ++slot_ops[op][slot];
}

typedef struct Edge {
    const char  *from;
    const char  *to;
    uint32      count;
} Edge;

static int
compare_edges(const void *a, const void *b)
{
    const Edge *ea = (const Edge *) a;
    const Edge *eb = (const Edge *) b;

    return (int32)eb->count - (int32)ea->count;
}

void
js_DumpOpMeters()
{
    const char *name, *from, *style;
    FILE *fp;
    uint32 total, count;
    uint32 i, j, nedges;
    Edge *graph;

    name = getenv("JS_OPMETER_FILE");
    if (!name)
        name = "/tmp/ops.dot";
    fp = fopen(name, "w");
    if (!fp) {
        perror(name);
        return;
    }

    total = nedges = 0;
    for (i = 0; i < JSOP_LIMIT; i++) {
        for (j = 0; j < JSOP_LIMIT; j++) {
            count = succeeds[i][j];
            if (count != 0) {
                total += count;
                ++nedges;
            }
        }
    }

# define SIGNIFICANT(count,total) (200. * (count) >= (total))

    graph = (Edge *) calloc(nedges, sizeof graph[0]);
    for (i = nedges = 0; i < JSOP_LIMIT; i++) {
        from = js_CodeName[i];
        for (j = 0; j < JSOP_LIMIT; j++) {
            count = succeeds[i][j];
            if (count != 0 && SIGNIFICANT(count, total)) {
                graph[nedges].from = from;
                graph[nedges].to = js_CodeName[j];
                graph[nedges].count = count;
                ++nedges;
            }
        }
    }
    qsort(graph, nedges, sizeof(Edge), compare_edges);

# undef SIGNIFICANT

    fputs("digraph {\n", fp);
    for (i = 0, style = NULL; i < nedges; i++) {
        JS_ASSERT(i == 0 || graph[i-1].count >= graph[i].count);
        if (!style || graph[i-1].count != graph[i].count) {
            style = (i > nedges * .75) ? "dotted" :
                    (i > nedges * .50) ? "dashed" :
                    (i > nedges * .25) ? "solid" : "bold";
        }
        fprintf(fp, "  %s -> %s [label=\"%lu\" style=%s]\n",
                graph[i].from, graph[i].to,
                (unsigned long)graph[i].count, style);
    }
    free(graph);
    fputs("}\n", fp);
    fclose(fp);

    name = getenv("JS_OPMETER_HIST");
    if (!name)
        name = "/tmp/ops.hist";
    fp = fopen(name, "w");
    if (!fp) {
        perror(name);
        return;
    }
    fputs("bytecode", fp);
    for (j = 0; j < HIST_NSLOTS; j++)
        fprintf(fp, "  slot %1u", (unsigned)j);
    putc('\n', fp);
    fputs("========", fp);
    for (j = 0; j < HIST_NSLOTS; j++)
        fputs(" =======", fp);
    putc('\n', fp);
    for (i = 0; i < JSOP_LIMIT; i++) {
        for (j = 0; j < HIST_NSLOTS; j++) {
            if (slot_ops[i][j] != 0) {
                /* Reuse j in the next loop, since we break after. */
                fprintf(fp, "%-8.8s", js_CodeName[i]);
                for (j = 0; j < HIST_NSLOTS; j++)
                    fprintf(fp, " %7lu", (unsigned long)slot_ops[i][j]);
                putc('\n', fp);
                break;
            }
        }
    }
    fclose(fp);
}

#endif /* JS_OPSMETER */

#else /* !defined js_invoke_c__ */

#define PUSH(v)         (*regs.sp++ = (v))
#define PUSH_OPND(v)    PUSH(v)
#define STORE_OPND(n,v) (regs.sp[n] = (v))
#define POP()           (*--regs.sp)
#define POP_OPND()      POP()
#define FETCH_OPND(n)   (regs.sp[n])

/*
 * Push the jsdouble d using sp from the lexical environment. Try to convert d
 * to a jsint that fits in a jsval, otherwise GC-alloc space for it and push a
 * reference.
 */
#define STORE_NUMBER(cx, n, d)                                                \
    JS_BEGIN_MACRO                                                            \
        jsint i_;                                                             \
                                                                              \
        if (JSDOUBLE_IS_INT(d, i_) && INT_FITS_IN_JSVAL(i_))                  \
            regs.sp[n] = INT_TO_JSVAL(i_);                                    \
        else if (!js_NewDoubleInRootedValue(cx, d, &regs.sp[n]))              \
            goto error;                                                       \
    JS_END_MACRO

#define STORE_INT(cx, n, i)                                                   \
    JS_BEGIN_MACRO                                                            \
        if (INT_FITS_IN_JSVAL(i))                                             \
            regs.sp[n] = INT_TO_JSVAL(i);                                     \
        else if (!js_NewDoubleInRootedValue(cx, (jsdouble) (i), &regs.sp[n])) \
            goto error;                                                       \
    JS_END_MACRO

#define STORE_UINT(cx, n, u)                                                  \
    JS_BEGIN_MACRO                                                            \
        if ((u) <= JSVAL_INT_MAX)                                             \
            regs.sp[n] = INT_TO_JSVAL(u);                                     \
        else if (!js_NewDoubleInRootedValue(cx, (jsdouble) (u), &regs.sp[n])) \
            goto error;                                                       \
    JS_END_MACRO

#define FETCH_NUMBER(cx, n, d)                                                \
    JS_BEGIN_MACRO                                                            \
        jsval v_;                                                             \
                                                                              \
        v_ = FETCH_OPND(n);                                                   \
        VALUE_TO_NUMBER(cx, n, v_, d);                                        \
    JS_END_MACRO

#define FETCH_INT(cx, n, i)                                                   \
    JS_BEGIN_MACRO                                                            \
        jsval v_;                                                             \
                                                                              \
        v_= FETCH_OPND(n);                                                    \
        if (JSVAL_IS_INT(v_)) {                                               \
            i = JSVAL_TO_INT(v_);                                             \
        } else {                                                              \
            i = js_ValueToECMAInt32(cx, &regs.sp[n]);                         \
            if (JSVAL_IS_NULL(regs.sp[n]))                                    \
                goto error;                                                   \
        }                                                                     \
    JS_END_MACRO

#define FETCH_UINT(cx, n, ui)                                                 \
    JS_BEGIN_MACRO                                                            \
        jsval v_;                                                             \
                                                                              \
        v_= FETCH_OPND(n);                                                    \
        if (JSVAL_IS_INT(v_)) {                                               \
            ui = (uint32) JSVAL_TO_INT(v_);                                   \
        } else {                                                              \
            ui = js_ValueToECMAUint32(cx, &regs.sp[n]);                       \
            if (JSVAL_IS_NULL(regs.sp[n]))                                    \
                goto error;                                                   \
        }                                                                     \
    JS_END_MACRO

/*
 * Optimized conversion macros that test for the desired type in v before
 * homing sp and calling a conversion function.
 */
#define VALUE_TO_NUMBER(cx, n, v, d)                                          \
    JS_BEGIN_MACRO                                                            \
        JS_ASSERT(v == regs.sp[n]);                                           \
        if (JSVAL_IS_INT(v)) {                                                \
            d = (jsdouble)JSVAL_TO_INT(v);                                    \
        } else if (JSVAL_IS_DOUBLE(v)) {                                      \
            d = *JSVAL_TO_DOUBLE(v);                                          \
        } else {                                                              \
            d = js_ValueToNumber(cx, &regs.sp[n]);                            \
            if (JSVAL_IS_NULL(regs.sp[n]))                                    \
                goto error;                                                   \
            JS_ASSERT(JSVAL_IS_NUMBER(regs.sp[n]) ||                          \
                      regs.sp[n] == JSVAL_TRUE);                              \
        }                                                                     \
    JS_END_MACRO

#define POP_BOOLEAN(cx, v, b)                                                 \
    JS_BEGIN_MACRO                                                            \
        v = FETCH_OPND(-1);                                                   \
        if (v == JSVAL_NULL) {                                                \
            b = JS_FALSE;                                                     \
        } else if (JSVAL_IS_BOOLEAN(v)) {                                     \
            b = JSVAL_TO_BOOLEAN(v);                                          \
        } else {                                                              \
            b = js_ValueToBoolean(v);                                         \
        }                                                                     \
        regs.sp--;                                                            \
    JS_END_MACRO

#define VALUE_TO_OBJECT(cx, n, v, obj)                                        \
    JS_BEGIN_MACRO                                                            \
        if (!JSVAL_IS_PRIMITIVE(v)) {                                         \
            obj = JSVAL_TO_OBJECT(v);                                         \
        } else {                                                              \
            obj = js_ValueToNonNullObject(cx, v);                             \
            if (!obj)                                                         \
                goto error;                                                   \
            STORE_OPND(n, OBJECT_TO_JSVAL(obj));                              \
        }                                                                     \
    JS_END_MACRO

#define FETCH_OBJECT(cx, n, v, obj)                                           \
    JS_BEGIN_MACRO                                                            \
        v = FETCH_OPND(n);                                                    \
        VALUE_TO_OBJECT(cx, n, v, obj);                                       \
    JS_END_MACRO

#define DEFAULT_VALUE(cx, n, hint, v)                                         \
    JS_BEGIN_MACRO                                                            \
        JS_ASSERT(!JSVAL_IS_PRIMITIVE(v));                                    \
        JS_ASSERT(v == regs.sp[n]);                                           \
        if (!OBJ_DEFAULT_VALUE(cx, JSVAL_TO_OBJECT(v), hint, &regs.sp[n]))    \
            goto error;                                                       \
        v = regs.sp[n];                                                       \
    JS_END_MACRO

/*
 * Quickly test if v is an int from the [-2**29, 2**29) range, that is, when
 * the lowest bit of v is 1 and the bits 30 and 31 are both either 0 or 1. For
 * such v we can do increment or decrement via adding or subtracting two
 * without checking that the result overflows JSVAL_INT_MIN or JSVAL_INT_MAX.
 */
#define CAN_DO_FAST_INC_DEC(v)     (((((v) << 1) ^ v) & 0x80000001) == 1)

JS_STATIC_ASSERT(JSVAL_INT == 1);
JS_STATIC_ASSERT(JSVAL_INT & JSVAL_VOID);
JS_STATIC_ASSERT(!CAN_DO_FAST_INC_DEC(JSVAL_VOID));
JS_STATIC_ASSERT(!CAN_DO_FAST_INC_DEC(INT_TO_JSVAL(JSVAL_INT_MIN)));
JS_STATIC_ASSERT(!CAN_DO_FAST_INC_DEC(INT_TO_JSVAL(JSVAL_INT_MAX)));

/*
 * Conditional assert to detect failure to clear a pending exception that is
 * suppressed (or unintentional suppression of a wanted exception).
 */
#if defined DEBUG_brendan || defined DEBUG_mrbkap || defined DEBUG_shaver
# define DEBUG_NOT_THROWING 1
#endif

#ifdef DEBUG_NOT_THROWING
# define ASSERT_NOT_THROWING(cx) JS_ASSERT(!(cx)->throwing)
#else
# define ASSERT_NOT_THROWING(cx) /* nothing */
#endif

/*
 * Define JS_OPMETER to instrument bytecode succession, generating a .dot file
 * on shutdown that shows the graph of significant predecessor/successor pairs
 * executed, where the edge labels give the succession counts.  The .dot file
 * is named by the JS_OPMETER_FILE envariable, and defaults to /tmp/ops.dot.
 *
 * Bonus feature: JS_OPMETER also enables counters for stack-addressing ops
 * such as JSOP_GETVAR, JSOP_INCARG, via METER_SLOT_OP.  The resulting counts
 * are written to JS_OPMETER_HIST, defaulting to /tmp/ops.hist.
 */
#ifndef JS_OPMETER
# define METER_OP_INIT(op)      /* nothing */
# define METER_OP_PAIR(op1,op2) /* nothing */
# define METER_SLOT_OP(op,slot) /* nothing */
#else

/*
 * The second dimension is hardcoded at 256 because we know that many bits fit
 * in a byte, and mainly to optimize away multiplying by JSOP_LIMIT to address
 * any particular row.
 */
# define METER_OP_INIT(op)      ((op) = JSOP_STOP)
# define METER_OP_PAIR(op1,op2) (js_MeterOpcodePair(op1, op2))
# define METER_SLOT_OP(op,slot) (js_MeterSlotOpcode(op, slot))

#endif

#define MAX_INLINE_CALL_COUNT 3000

/*
 * Threaded interpretation via computed goto appears to be well-supported by
 * GCC 3 and higher.  IBM's C compiler when run with the right options (e.g.,
 * -qlanglvl=extended) also supports threading.  Ditto the SunPro C compiler.
 * Currently it's broken for JS_VERSION < 160, though this isn't worth fixing.
 * Add your compiler support macros here.
 */
#ifndef JS_THREADED_INTERP
# if JS_VERSION >= 160 && (                                                   \
    __GNUC__ >= 3 ||                                                          \
    (__IBMC__ >= 700 && defined __IBM_COMPUTED_GOTO) ||                       \
    __SUNPRO_C >= 0x570)
#  define JS_THREADED_INTERP 1
# else
#  define JS_THREADED_INTERP 0
# endif
#endif

/*
 * Ensure that the intrepreter switch can close call-bytecode cases in the
 * same way as non-call bytecodes.
 */
JS_STATIC_ASSERT(JSOP_NAME_LENGTH == JSOP_CALLNAME_LENGTH);
JS_STATIC_ASSERT(JSOP_GETGVAR_LENGTH == JSOP_CALLGVAR_LENGTH);
JS_STATIC_ASSERT(JSOP_GETVAR_LENGTH == JSOP_CALLVAR_LENGTH);
JS_STATIC_ASSERT(JSOP_GETARG_LENGTH == JSOP_CALLARG_LENGTH);
JS_STATIC_ASSERT(JSOP_GETLOCAL_LENGTH == JSOP_CALLLOCAL_LENGTH);
JS_STATIC_ASSERT(JSOP_XMLNAME_LENGTH == JSOP_CALLXMLNAME_LENGTH);

/*
 * Same for JSOP_SETNAME and JSOP_SETPROP, which differ only slightly but
 * remain distinct for the decompiler.
 */
JS_STATIC_ASSERT(JSOP_SETNAME_LENGTH == JSOP_SETPROP_LENGTH);

/* Ensure we can share deffun and closure code. */
JS_STATIC_ASSERT(JSOP_DEFFUN_LENGTH == JSOP_CLOSURE_LENGTH);


/* See comments in FETCH_SHIFT macro. */
JS_STATIC_ASSERT((JSVAL_TO_INT(JSVAL_VOID) & 31) == 0);

JSBool
js_Interpret(JSContext *cx)
{
    JSRuntime *rt;
    JSStackFrame *fp;
    JSScript *script;
    uintN inlineCallCount;
    JSAtom **atoms;
    JSVersion currentVersion, originalVersion;
    void *mark;
    JSFrameRegs regs;
    JSObject *obj, *obj2, *parent;
    JSBool ok, cond;
    JSTrapHandler interruptHandler;
    jsint len;
    jsbytecode *endpc, *pc2;
    JSOp op, op2;
    jsatomid index;
    JSAtom *atom;
    uintN argc, attrs, flags;
    uint32 slot;
    jsval *vp, lval, rval, ltmp, rtmp;
    jsid id;
    JSObject *iterobj;
    JSProperty *prop;
    JSScopeProperty *sprop;
    JSString *str, *str2;
    jsint i, j;
    jsdouble d, d2;
    JSClass *clasp;
    JSFunction *fun;
    JSType type;
#if !JS_THREADED_INTERP && defined DEBUG
    FILE *tracefp = NULL;
#endif
#if JS_HAS_EXPORT_IMPORT
    JSIdArray *ida;
#endif
    jsint low, high, off, npairs;
    JSBool match;
#if JS_HAS_GETTER_SETTER
    JSPropertyOp getter, setter;
#endif

#ifdef __GNUC__
# define JS_EXTENSION __extension__
# define JS_EXTENSION_(s) __extension__ ({ s; })
#else
# define JS_EXTENSION
# define JS_EXTENSION_(s) s
#endif

#if JS_THREADED_INTERP
    static void *normalJumpTable[] = {
# define OPDEF(op,val,name,token,length,nuses,ndefs,prec,format) \
        JS_EXTENSION &&L_##op,
# include "jsopcode.tbl"
# undef OPDEF
    };

    static void *interruptJumpTable[] = {
# define OPDEF(op,val,name,token,length,nuses,ndefs,prec,format)              \
        JS_EXTENSION &&interrupt,
# include "jsopcode.tbl"
# undef OPDEF
    };

    register void **jumpTable = normalJumpTable;

    METER_OP_INIT(op);      /* to nullify first METER_OP_PAIR */

# define DO_OP()            JS_EXTENSION_(goto *jumpTable[op])
# define DO_NEXT_OP(n)      do { METER_OP_PAIR(op, regs.pc[n]);               \
                                 op = (JSOp) *(regs.pc += (n));               \
                                 DO_OP(); } while (0)
# define BEGIN_CASE(OP)     L_##OP:
# define END_CASE(OP)       DO_NEXT_OP(OP##_LENGTH);
# define END_VARLEN_CASE    DO_NEXT_OP(len);
# define EMPTY_CASE(OP)     BEGIN_CASE(OP) op = (JSOp) *++regs.pc; DO_OP();
#else
# define DO_OP()            goto do_op
# define DO_NEXT_OP(n)      goto advance_pc
# define BEGIN_CASE(OP)     case OP:
# define END_CASE(OP)       break;
# define END_VARLEN_CASE    break;
# define EMPTY_CASE(OP)     BEGIN_CASE(OP) END_CASE(OP)
#endif

    /* Check for too deep a C stack. */
    JS_CHECK_RECURSION(cx, return JS_FALSE);

    rt = cx->runtime;

    /* Set registerized frame pointer and derived script pointer. */
    fp = cx->fp;
    script = fp->script;
    JS_ASSERT(script->length != 0);

    /* Count of JS function calls that nest in this C js_Interpret frame. */
    inlineCallCount = 0;

    /*
     * Initialize the index segment register used by LOAD_ATOM and
     * GET_FULL_INDEX macros bellow. As a register we use a pointer based on
     * the atom map to turn frequently executed LOAD_ATOM into simple array
     * access. For less frequent object and regexp loads we have to recover
     * the segment from atoms pointer first.
     */
    atoms = script->atomMap.vector;

#define LOAD_ATOM(PCOFF)                                                      \
    JS_BEGIN_MACRO                                                            \
        JS_ASSERT((size_t)(atoms - script->atomMap.vector) <                  \
                  (size_t)(script->atomMap.length -                           \
                           GET_INDEX(regs.pc + PCOFF)));                      \
        atom = atoms[GET_INDEX(regs.pc + PCOFF)];                             \
    JS_END_MACRO

#define GET_FULL_INDEX(PCOFF)                                                 \
    (atoms - script->atomMap.vector + GET_INDEX(regs.pc + PCOFF))

#define LOAD_OBJECT(PCOFF)                                                    \
    JS_GET_SCRIPT_OBJECT(script, GET_FULL_INDEX(PCOFF), obj)

#define LOAD_FUNCTION(PCOFF)                                                  \
    JS_GET_SCRIPT_FUNCTION(script, GET_FULL_INDEX(PCOFF), fun)

    /*
     * Prepare to call a user-supplied branch handler, and abort the script
     * if it returns false.
     */
#define CHECK_BRANCH(len)                                                     \
    JS_BEGIN_MACRO                                                            \
        if (len <= 0 && (cx->operationCount -= JSOW_SCRIPT_JUMP) <= 0) {      \
            if (!js_ResetOperationCount(cx))                                  \
                goto error;                                                   \
        }                                                                     \
    JS_END_MACRO

    /*
     * Optimized Get and SetVersion for proper script language versioning.
     *
     * If any native method or JSClass/JSObjectOps hook calls js_SetVersion
     * and changes cx->version, the effect will "stick" and we will stop
     * maintaining currentVersion.  This is relied upon by testsuites, for
     * the most part -- web browsers select version before compiling and not
     * at run-time.
     */
    currentVersion = (JSVersion) script->version;
    originalVersion = (JSVersion) cx->version;
    if (currentVersion != originalVersion)
        js_SetVersion(cx, currentVersion);

    ++cx->interpLevel;
#ifdef DEBUG
    fp->pcDisabledSave = JS_PROPERTY_CACHE(cx).disabled;
#endif

    /*
     * From this point control must flow through the label exit2.
     *
     * Load the debugger's interrupt hook here and after calling out to native
     * functions (but not to getters, setters, or other native hooks), so we do
     * not have to reload it each time through the interpreter loop -- we hope
     * the compiler can keep it in a register when it is non-null.
     */
#if JS_THREADED_INTERP
# define LOAD_JUMP_TABLE()                                                    \
    (jumpTable = interruptHandler ? interruptJumpTable : normalJumpTable)
#else
# define LOAD_JUMP_TABLE()      ((void) 0)
#endif

#define LOAD_INTERRUPT_HANDLER(cx)                                            \
    JS_BEGIN_MACRO                                                            \
        interruptHandler = (cx)->debugHooks->interruptHandler;                \
        LOAD_JUMP_TABLE();                                                    \
    JS_END_MACRO

    LOAD_INTERRUPT_HANDLER(cx);

     /*
     * Initialize the pc register and allocate operand stack slots for the
     * script's worst-case depth, unless we're resuming a generator.
     */
    if (JS_LIKELY(!fp->spbase)) {
        ASSERT_NOT_THROWING(cx);
        JS_ASSERT(!fp->regs);
        fp->spbase = js_AllocRawStack(cx, script->depth, &mark);
        if (!fp->spbase) {
            ok = JS_FALSE;
            goto exit2;
        }
        JS_ASSERT(mark);
        regs.pc = script->code;
        regs.sp = fp->spbase;
        fp->regs = &regs;
    } else {
        JSGenerator *gen;

        JS_ASSERT(fp->flags & JSFRAME_GENERATOR);
        mark = NULL;
        gen = FRAME_TO_GENERATOR(fp);
        JS_ASSERT(fp->regs == &gen->savedRegs);
        regs = gen->savedRegs;
        fp->regs = &regs;
        JS_ASSERT((size_t) (regs.pc - script->code) <= script->length);
        JS_ASSERT((size_t) (regs.sp - fp->spbase) <= script->depth);
        JS_ASSERT(JS_PROPERTY_CACHE(cx).disabled >= 0);
        JS_PROPERTY_CACHE(cx).disabled += js_CountWithBlocks(cx, fp);

        /*
         * To support generator_throw and to catch ignored exceptions,
         * fail if cx->throwing is set.
         */
        if (cx->throwing) {
#ifdef DEBUG_NOT_THROWING
            if (cx->exception != JSVAL_ARETURN) {
                printf("JS INTERPRETER CALLED WITH PENDING EXCEPTION %lx\n",
                       (unsigned long) cx->exception);
            }
#endif
            goto error;
        }
    }

#if JS_THREADED_INTERP

    /*
     * This is a loop, but it does not look like a loop.  The loop-closing
     * jump is distributed throughout interruptJumpTable, and comes back to
     * the interrupt label.  The dispatch on op is through normalJumpTable.
     * The trick is LOAD_INTERRUPT_HANDLER setting jumpTable appropriately.
     *
     * It is important that "op" be initialized before the interrupt label
     * because it is possible for "op" to be specially assigned during the
     * normally processing of an opcode while looping (in particular, this
     * happens in JSOP_TRAP while debugging).  We rely on DO_NEXT_OP to
     * correctly manage "op" in all other cases.
     */
    op = (JSOp) *regs.pc;
    if (interruptHandler) {
interrupt:
        switch (interruptHandler(cx, script, regs.pc, &rval,
                                 cx->debugHooks->interruptHandlerData)) {
          case JSTRAP_ERROR:
            goto error;
          case JSTRAP_CONTINUE:
            break;
          case JSTRAP_RETURN:
            fp->rval = rval;
            ok = JS_TRUE;
            goto forced_return;
          case JSTRAP_THROW:
            cx->throwing = JS_TRUE;
            cx->exception = rval;
            goto error;
          default:;
        }
        LOAD_INTERRUPT_HANDLER(cx);
    }

    JS_ASSERT((uintN)op < (uintN)JSOP_LIMIT);
    JS_EXTENSION_(goto *normalJumpTable[op]);

#else  /* !JS_THREADED_INTERP */

    for (;;) {
        op = (JSOp) *regs.pc;
      do_op:
        len = js_CodeSpec[op].length;

#ifdef DEBUG
        tracefp = (FILE *) cx->tracefp;
        if (tracefp) {
            intN nuses, n;

            fprintf(tracefp, "%4u: ", js_PCToLineNumber(cx, script, regs.pc));
            js_Disassemble1(cx, script, regs.pc,
                            PTRDIFF(regs.pc, script->code, jsbytecode),
                            JS_FALSE, tracefp);
            nuses = js_CodeSpec[op].nuses;
            if (nuses) {
                for (n = -nuses; n < 0; n++) {
                    char *bytes = js_DecompileValueGenerator(cx, n, regs.sp[n],
                                                             NULL);
                    if (bytes) {
                        fprintf(tracefp, "%s %s",
                                (n == -nuses) ? "  inputs:" : ",",
                                bytes);
                        JS_free(cx, bytes);
                    }
                }
                fprintf(tracefp, " @ %d\n", regs.sp - fp->spbase);
            }
        }
#endif /* DEBUG */

        if (interruptHandler) {
            switch (interruptHandler(cx, script, regs.pc, &rval,
                                     cx->debugHooks->interruptHandlerData)) {
              case JSTRAP_ERROR:
                goto error;
              case JSTRAP_CONTINUE:
                break;
              case JSTRAP_RETURN:
                fp->rval = rval;
                ok = JS_TRUE;
                goto forced_return;
              case JSTRAP_THROW:
                cx->throwing = JS_TRUE;
                cx->exception = rval;
                goto error;
              default:;
            }
            LOAD_INTERRUPT_HANDLER(cx);
        }

        switch (op) {

#endif /* !JS_THREADED_INTERP */

          EMPTY_CASE(JSOP_NOP)

          EMPTY_CASE(JSOP_GROUP)

          /* EMPTY_CASE is not used here as JSOP_LINENO_LENGTH == 3. */
          BEGIN_CASE(JSOP_LINENO)
          END_CASE(JSOP_LINENO)

          BEGIN_CASE(JSOP_PUSH)
            PUSH_OPND(JSVAL_VOID);
          END_CASE(JSOP_PUSH)

          BEGIN_CASE(JSOP_POP)
            regs.sp--;
          END_CASE(JSOP_POP)

          BEGIN_CASE(JSOP_POPN)
            regs.sp -= GET_UINT16(regs.pc);
#ifdef DEBUG
            JS_ASSERT(fp->spbase <= regs.sp);
            obj = fp->blockChain;
            JS_ASSERT_IF(obj,
                         OBJ_BLOCK_DEPTH(cx, obj) + OBJ_BLOCK_COUNT(cx, obj)
                         <= (size_t) (regs.sp - fp->spbase));
            for (obj = fp->scopeChain; obj; obj = OBJ_GET_PARENT(cx, obj)) {
                clasp = OBJ_GET_CLASS(cx, obj);
                if (clasp != &js_BlockClass && clasp != &js_WithClass)
                    continue;
                if (OBJ_GET_PRIVATE(cx, obj) != fp)
                    break;
                JS_ASSERT(fp->spbase + OBJ_BLOCK_DEPTH(cx, obj)
                                     + ((clasp == &js_BlockClass)
                                        ? OBJ_BLOCK_COUNT(cx, obj)
                                        : 1)
                          <= regs.sp);
            }
#endif
          END_CASE(JSOP_POPN)

          BEGIN_CASE(JSOP_SWAP)
            rtmp = regs.sp[-1];
            regs.sp[-1] = regs.sp[-2];
            regs.sp[-2] = rtmp;
          END_CASE(JSOP_SWAP)

          BEGIN_CASE(JSOP_SETRVAL)
          BEGIN_CASE(JSOP_POPV)
            ASSERT_NOT_THROWING(cx);
            fp->rval = POP_OPND();
          END_CASE(JSOP_POPV)

          BEGIN_CASE(JSOP_ENTERWITH)
            if (!js_EnterWith(cx, -1))
                goto error;

            /*
             * We must ensure that different "with" blocks have different
             * stack depth associated with them. This allows the try handler
             * search to properly recover the scope chain. Thus we must keep
             * the stack at least at the current level.
             *
             * We set sp[-1] to the current "with" object to help asserting
             * the enter/leave balance in [leavewith].
             */
            regs.sp[-1] = OBJECT_TO_JSVAL(fp->scopeChain);
          END_CASE(JSOP_ENTERWITH)

          BEGIN_CASE(JSOP_LEAVEWITH)
            JS_ASSERT(regs.sp[-1] == OBJECT_TO_JSVAL(fp->scopeChain));
            regs.sp--;
            js_LeaveWith(cx);
          END_CASE(JSOP_LEAVEWITH)

          BEGIN_CASE(JSOP_RETURN)
            CHECK_BRANCH(-1);
            fp->rval = POP_OPND();
            /* FALL THROUGH */

          BEGIN_CASE(JSOP_RETRVAL)    /* fp->rval already set */
          BEGIN_CASE(JSOP_STOP)
            /*
             * When the inlined frame exits with an exception or an error, ok
             * will be false after the inline_return label.
             */
            ASSERT_NOT_THROWING(cx);
            JS_ASSERT(regs.sp == fp->spbase);
            ok = JS_TRUE;
            if (inlineCallCount)
          inline_return:
            {
                JSInlineFrame *ifp = (JSInlineFrame *) fp;
                void *hookData = ifp->hookData;

                JS_ASSERT(JS_PROPERTY_CACHE(cx).disabled == fp->pcDisabledSave);
                JS_ASSERT(!fp->blockChain);
                JS_ASSERT(!js_IsActiveWithOrBlock(cx, fp->scopeChain, 0));

                if (hookData) {
                    JSInterpreterHook hook;
                    JSBool status;

                    hook = cx->debugHooks->callHook;
                    if (hook) {
                        /*
                         * Do not pass &ok directly as exposing the address
                         * inhibits optimizations and uninitialised warnings.
                         */
                        status = ok;
                        hook(cx, fp, JS_FALSE, &status, hookData);
                        ok = status;
                        LOAD_INTERRUPT_HANDLER(cx);
                    }
                }

                /*
                 * If fp has a call object, sync values and clear the back-
                 * pointer. This can happen for a lightweight function if it
                 * calls eval unexpectedly (in a way that is hidden from the
                 * compiler). See bug 325540.
                 */
                if (fp->callobj)
                    ok &= js_PutCallObject(cx, fp);

                if (fp->argsobj)
                    ok &= js_PutArgsObject(cx, fp);

#ifdef INCLUDE_MOZILLA_DTRACE
                /* DTrace function return, inlines */
                if (JAVASCRIPT_FUNCTION_RVAL_ENABLED())
                    jsdtrace_function_rval(cx, fp, fp->fun);
                if (JAVASCRIPT_FUNCTION_RETURN_ENABLED())
                    jsdtrace_function_return(cx, fp, fp->fun);
#endif

                /* Restore context version only if callee hasn't set version. */
                if (JS_LIKELY(cx->version == currentVersion)) {
                    currentVersion = ifp->callerVersion;
                    if (currentVersion != cx->version)
                        js_SetVersion(cx, currentVersion);
                }

                /* Restore caller's registers. */
                regs = ifp->callerRegs;

                /* Store the return value in the caller's operand frame. */
                regs.sp -= 1 + (size_t) ifp->frame.argc;
                regs.sp[-1] = fp->rval;

                /* Restore cx->fp and release the inline frame's space. */
                cx->fp = fp = fp->down;
                JS_ASSERT(fp->regs == &ifp->callerRegs);
                fp->regs = &regs;
                JS_ARENA_RELEASE(&cx->stackPool, ifp->mark);

                /* Restore the calling script's interpreter registers. */
                script = fp->script;
                atoms = script->atomMap.vector;

                /* Resume execution in the calling frame. */
                inlineCallCount--;
                if (JS_LIKELY(ok)) {
                    JS_ASSERT(js_CodeSpec[*regs.pc].length == JSOP_CALL_LENGTH);
                    len = JSOP_CALL_LENGTH;
                    DO_NEXT_OP(len);
                }
                goto error;
            }
            goto exit;

          BEGIN_CASE(JSOP_DEFAULT)
            (void) POP();
            /* FALL THROUGH */
          BEGIN_CASE(JSOP_GOTO)
            len = GET_JUMP_OFFSET(regs.pc);
            CHECK_BRANCH(len);
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_IFEQ)
            POP_BOOLEAN(cx, rval, cond);
            if (cond == JS_FALSE) {
                len = GET_JUMP_OFFSET(regs.pc);
                CHECK_BRANCH(len);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_IFEQ)

          BEGIN_CASE(JSOP_IFNE)
            POP_BOOLEAN(cx, rval, cond);
            if (cond != JS_FALSE) {
                len = GET_JUMP_OFFSET(regs.pc);
                CHECK_BRANCH(len);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_IFNE)

          BEGIN_CASE(JSOP_OR)
            POP_BOOLEAN(cx, rval, cond);
            if (cond == JS_TRUE) {
                len = GET_JUMP_OFFSET(regs.pc);
                PUSH_OPND(rval);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_OR)

          BEGIN_CASE(JSOP_AND)
            POP_BOOLEAN(cx, rval, cond);
            if (cond == JS_FALSE) {
                len = GET_JUMP_OFFSET(regs.pc);
                PUSH_OPND(rval);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_AND)

          BEGIN_CASE(JSOP_DEFAULTX)
            (void) POP();
            /* FALL THROUGH */
          BEGIN_CASE(JSOP_GOTOX)
            len = GET_JUMPX_OFFSET(regs.pc);
            CHECK_BRANCH(len);
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_IFEQX)
            POP_BOOLEAN(cx, rval, cond);
            if (cond == JS_FALSE) {
                len = GET_JUMPX_OFFSET(regs.pc);
                CHECK_BRANCH(len);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_IFEQX)

          BEGIN_CASE(JSOP_IFNEX)
            POP_BOOLEAN(cx, rval, cond);
            if (cond != JS_FALSE) {
                len = GET_JUMPX_OFFSET(regs.pc);
                CHECK_BRANCH(len);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_IFNEX)

          BEGIN_CASE(JSOP_ORX)
            POP_BOOLEAN(cx, rval, cond);
            if (cond == JS_TRUE) {
                len = GET_JUMPX_OFFSET(regs.pc);
                PUSH_OPND(rval);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_ORX)

          BEGIN_CASE(JSOP_ANDX)
            POP_BOOLEAN(cx, rval, cond);
            if (cond == JS_FALSE) {
                len = GET_JUMPX_OFFSET(regs.pc);
                PUSH_OPND(rval);
                DO_NEXT_OP(len);
            }
          END_CASE(JSOP_ANDX)

/*
 * If the index value at sp[n] is not an int that fits in a jsval, it could
 * be an object (an XML QName, AttributeName, or AnyName), but only if we are
 * compiling with JS_HAS_XML_SUPPORT.  Otherwise convert the index value to a
 * string atom id.
 */
#define FETCH_ELEMENT_ID(obj, n, id)                                          \
    JS_BEGIN_MACRO                                                            \
        jsval idval_ = FETCH_OPND(n);                                         \
        if (JSVAL_IS_INT(idval_)) {                                           \
            id = INT_JSVAL_TO_JSID(idval_);                                   \
        } else {                                                              \
            if (!js_InternNonIntElementId(cx, obj, idval_, &id))              \
                goto error;                                                   \
            regs.sp[n] = ID_TO_VALUE(id);                                     \
        }                                                                     \
    JS_END_MACRO

          BEGIN_CASE(JSOP_IN)
            rval = FETCH_OPND(-1);
            if (JSVAL_IS_PRIMITIVE(rval)) {
                js_ReportValueError(cx, JSMSG_IN_NOT_OBJECT, -1, rval, NULL);
                goto error;
            }
            obj = JSVAL_TO_OBJECT(rval);
            FETCH_ELEMENT_ID(obj, -2, id);
            if (!OBJ_LOOKUP_PROPERTY(cx, obj, id, &obj2, &prop))
                goto error;
            regs.sp--;
            STORE_OPND(-1, BOOLEAN_TO_JSVAL(prop != NULL));
            if (prop)
                OBJ_DROP_PROPERTY(cx, obj2, prop);
          END_CASE(JSOP_IN)

          BEGIN_CASE(JSOP_FOREACH)
            flags = JSITER_ENUMERATE | JSITER_FOREACH;
            goto value_to_iter;

#if JS_HAS_DESTRUCTURING
          BEGIN_CASE(JSOP_FOREACHKEYVAL)
            flags = JSITER_ENUMERATE | JSITER_FOREACH | JSITER_KEYVALUE;
            goto value_to_iter;
#endif

          BEGIN_CASE(JSOP_FORIN)
            /*
             * Set JSITER_ENUMERATE to indicate that for-in loop should use
             * the enumeration protocol's iterator for compatibility if an
             * explicit iterator is not given via the optional __iterator__
             * method.
             */
            flags = JSITER_ENUMERATE;

          value_to_iter:
            JS_ASSERT(regs.sp > fp->spbase);
            if (!js_ValueToIterator(cx, flags, &regs.sp[-1]))
                goto error;
            JS_ASSERT(!JSVAL_IS_PRIMITIVE(regs.sp[-1]));
            JS_ASSERT(JSOP_FORIN_LENGTH == js_CodeSpec[op].length);
          END_CASE(JSOP_FORIN)

          BEGIN_CASE(JSOP_FORPROP)
            /*
             * Handle JSOP_FORPROP first, so the cost of the goto do_forinloop
             * is not paid for the more common cases.
             */
            LOAD_ATOM(0);
            id = ATOM_TO_JSID(atom);
            i = -2;
            goto do_forinloop;

          BEGIN_CASE(JSOP_FORNAME)
            LOAD_ATOM(0);
            id = ATOM_TO_JSID(atom);
            /* FALL THROUGH */

          BEGIN_CASE(JSOP_FORARG)
          BEGIN_CASE(JSOP_FORVAR)
          BEGIN_CASE(JSOP_FORCONST)
          BEGIN_CASE(JSOP_FORLOCAL)
            /*
             * JSOP_FORARG and JSOP_FORVAR don't require any lval computation
             * here, because they address slots on the stack (in fp->args and
             * fp->vars, respectively).  Same applies to JSOP_FORLOCAL, which
             * addresses fp->spbase.
             */
            /* FALL THROUGH */

          BEGIN_CASE(JSOP_FORELEM)
            /*
             * JSOP_FORELEM simply initializes or updates the iteration state
             * and leaves the index expression evaluation and assignment to the
             * enumerator until after the next property has been acquired, via
             * a JSOP_ENUMELEM bytecode.
             */
            i = -1;

          do_forinloop:
            /*
             * Reach under the top of stack to find our property iterator, a
             * JSObject that contains the iteration state.
             */
            JS_ASSERT(!JSVAL_IS_PRIMITIVE(regs.sp[i]));
            iterobj = JSVAL_TO_OBJECT(regs.sp[i]);

            if (!js_CallIteratorNext(cx, iterobj, &rval))
                goto error;
            if (rval == JSVAL_HOLE) {
                rval = JSVAL_FALSE;
                goto end_forinloop;
            }

            switch (op) {
              case JSOP_FORARG:
                slot = GET_ARGNO(regs.pc);
                JS_ASSERT(slot < fp->fun->nargs);
                fp->argv[slot] = rval;
                break;

              case JSOP_FORVAR:
                slot = GET_VARNO(regs.pc);
                JS_ASSERT(slot < fp->fun->u.i.nvars);
                fp->vars[slot] = rval;
                break;

              case JSOP_FORCONST:
                /* Don't update the const slot. */
                break;

              case JSOP_FORLOCAL:
                slot = GET_UINT16(regs.pc);
                JS_ASSERT(slot < script->depth);
                vp = &fp->spbase[slot];
                GC_POKE(cx, *vp);
                *vp = rval;
                break;

              case JSOP_FORELEM:
                /* FORELEM is not a SET operation, it's more like BINDNAME. */
                PUSH_OPND(rval);
                break;

              case JSOP_FORPROP:
                /*
                 * We fetch object here to ensure that the iterator is called
                 * even if lval is null or undefined that throws in
                 * FETCH_OBJECT. See bug 372331.
                 */
                FETCH_OBJECT(cx, -1, lval, obj);
                goto set_for_property;

              default:
                JS_ASSERT(op == JSOP_FORNAME);

                /*
                 * We find property here after the iterator call to ensure
                 * that we take into account side effects of the iterator
                 * call. See bug 372331.
                 */

                if (!js_FindProperty(cx, id, &obj, &obj2, &prop))
                    goto error;
                if (prop)
                    OBJ_DROP_PROPERTY(cx, obj2, prop);

              set_for_property:
                /* Set the variable obj[id] to refer to rval. */
                fp->flags |= JSFRAME_ASSIGNING;
                ok = OBJ_SET_PROPERTY(cx, obj, id, &rval);
                fp->flags &= ~JSFRAME_ASSIGNING;
                if (!ok)
                    goto error;
                break;
            }

            /* Push true to keep looping through properties. */
            rval = JSVAL_TRUE;

          end_forinloop:
            regs.sp += i + 1;
            PUSH_OPND(rval);
            len = js_CodeSpec[op].length;
            DO_NEXT_OP(len);

          BEGIN_CASE(JSOP_DUP)
            JS_ASSERT(regs.sp > fp->spbase);
            rval = FETCH_OPND(-1);
            PUSH(rval);
          END_CASE(JSOP_DUP)

          BEGIN_CASE(JSOP_DUP2)
            JS_ASSERT(regs.sp - 2 >= fp->spbase);
            lval = FETCH_OPND(-2);
            rval = FETCH_OPND(-1);
            PUSH(lval);
            PUSH(rval);
          END_CASE(JSOP_DUP2)

#define PROPERTY_OP(n, call)                                                  \
    JS_BEGIN_MACRO                                                            \
        /* Fetch the left part and resolve it to a non-null object. */        \
        FETCH_OBJECT(cx, n, lval, obj);                                       \
                                                                              \
        /* Get or set the property. */                                        \
        if (!call)                                                            \
            goto error;                                                       \
    JS_END_MACRO

#define ELEMENT_OP(n, call)                                                   \
    JS_BEGIN_MACRO                                                            \
        /* Fetch the left part and resolve it to a non-null object. */        \
        FETCH_OBJECT(cx, n - 1, lval, obj);                                   \
                                                                              \
        /* Fetch index and convert it to id suitable for use with obj. */     \
        FETCH_ELEMENT_ID(obj, n, id);                                         \
                                                                              \
        /* Get or set the element. */                                         \
        if (!call)                                                            \
            goto error;                                                       \
    JS_END_MACRO

#define NATIVE_GET(cx,obj,pobj,sprop,vp)                                      \
    JS_BEGIN_MACRO                                                            \
        if (SPROP_HAS_STUB_GETTER(sprop)) {                                   \
            /* Fast path for Object instance properties. */                   \
            JS_ASSERT((sprop)->slot != SPROP_INVALID_SLOT ||                  \
                      !SPROP_HAS_STUB_SETTER(sprop));                         \
            *vp = ((sprop)->slot != SPROP_INVALID_SLOT)                       \
                  ? LOCKED_OBJ_GET_SLOT(pobj, (sprop)->slot)                  \
                  : JSVAL_VOID;                                               \
        } else {                                                              \
            if (!js_NativeGet(cx, obj, pobj, sprop, vp))                      \
                goto error;                                                   \
        }                                                                     \
    JS_END_MACRO

#define NATIVE_SET(cx,obj,sprop,vp)                                           \
    JS_BEGIN_MACRO                                                            \
        if (SPROP_HAS_STUB_SETTER(sprop) &&                                   \
            (sprop)->slot != SPROP_INVALID_SLOT) {                            \
            /* Fast path for, e.g., Object instance properties. */            \
            LOCKED_OBJ_WRITE_BARRIER(cx, obj, (sprop)->slot, *vp);            \
        } else {                                                              \
            if (!js_NativeSet(cx, obj, sprop, vp))                            \
                goto error;                                                   \
        }                                                                     \
    JS_END_MACRO

/*
 * Deadlocks or else bad races are likely if JS_THREADSAFE, so we must rely on
 * single-thread DEBUG js shell testing to verify property cache hits.
 */
#if defined DEBUG && !defined JS_THREADSAFE
# define ASSERT_VALID_PROPERTY_CACHE_HIT(pcoff,obj,pobj,entry)                \
    do {                                                                      \
        JSAtom *atom_;                                                        \
        JSObject *obj_, *pobj_;                                               \
        JSProperty *prop_;                                                    \
        JSScopeProperty *sprop_;                                              \
        uint32 sample_ = rt->gcNumber;                                        \
        if (pcoff >= 0)                                                       \
            GET_ATOM_FROM_BYTECODE(script, regs.pc, pcoff, atom_);            \
        else                                                                  \
            atom_ = rt->atomState.lengthAtom;                                 \
        if (JOF_OPMODE(*regs.pc) == JOF_NAME) {                               \
            ok = js_FindProperty(cx, ATOM_TO_JSID(atom_), &obj_, &pobj_,      \
                                 &prop_);                                     \
        } else {                                                              \
            obj_ = obj;                                                       \
            ok = js_LookupProperty(cx, obj, ATOM_TO_JSID(atom_), &pobj_,      \
                                   &prop_);                                   \
        }                                                                     \
        if (!ok)                                                              \
            goto error;                                                       \
        if (rt->gcNumber != sample_)                                          \
            break;                                                            \
        JS_ASSERT(prop_);                                                     \
        JS_ASSERT(pobj_ == pobj);                                             \
        sprop_ = (JSScopeProperty *) prop_;                                   \
        if (PCVAL_IS_SLOT(entry->vword)) {                                    \
            JS_ASSERT(PCVAL_TO_SLOT(entry->vword) == sprop_->slot);           \
        } else if (PCVAL_IS_SPROP(entry->vword)) {                            \
            JS_ASSERT(PCVAL_TO_SPROP(entry->vword) == sprop_);                \
        } else {                                                              \
            jsval v_;                                                         \
            JS_ASSERT(PCVAL_IS_OBJECT(entry->vword));                         \
            JS_ASSERT(entry->vword != PCVAL_NULL);                            \
            JS_ASSERT(SCOPE_IS_BRANDED(OBJ_SCOPE(pobj)));                     \
            JS_ASSERT(SPROP_HAS_STUB_GETTER(sprop_));                         \
            JS_ASSERT(SPROP_HAS_VALID_SLOT(sprop_, OBJ_SCOPE(pobj_)));        \
            v_ = LOCKED_OBJ_GET_SLOT(pobj_, sprop_->slot);                    \
            JS_ASSERT(VALUE_IS_FUNCTION(cx, v_));                             \
            JS_ASSERT(PCVAL_TO_OBJECT(entry->vword) == JSVAL_TO_OBJECT(v_));  \
        }                                                                     \
        OBJ_DROP_PROPERTY(cx, pobj_, prop_);                                  \
    } while (0)
#else
# define ASSERT_VALID_PROPERTY_CACHE_HIT(pcoff,obj,pobj,entry) ((void) 0)
#endif

          BEGIN_CASE(JSOP_SETCONST)
            LOAD_ATOM(0);
            obj = fp->varobj;
            rval = FETCH_OPND(-1);
            if (!OBJ_DEFINE_PROPERTY(cx, obj, ATOM_TO_JSID(atom), rval,
                                     JS_PropertyStub, JS_PropertyStub,
                                     JSPROP_ENUMERATE | JSPROP_PERMANENT |
                                     JSPROP_READONLY,
                                     NULL)) {
                goto error;
            }
            STORE_OPND(-1, rval);
          END_CASE(JSOP_SETCONST)

#if JS_HAS_DESTRUCTURING
          BEGIN_CASE(JSOP_ENUMCONSTELEM)
            rval = FETCH_OPND(-3);
            FETCH_OBJECT(cx, -2, lval, obj);
            FETCH_ELEMENT_ID(obj, -1, id);
            if (!OBJ_DEFINE_PROPERTY(cx, obj, id, rval,
                                     JS_PropertyStub, JS_PropertyStub,
                                     JSPROP_ENUMERATE | JSPROP_PERMANENT |
                                     JSPROP_READONLY,
                                     NULL)) {
                goto error;
            }
            regs.sp -= 3;
          END_CASE(JSOP_ENUMCONSTELEM)
#endif

          BEGIN_CASE(JSOP_BINDNAME)
            do {
                JSPropCacheEntry *entry;

                obj = fp->scopeChain;
                if (JS_LIKELY(OBJ_IS_NATIVE(obj))) {
                    PROPERTY_CACHE_TEST(cx, regs.pc, obj, obj2, entry, atom);
                    if (!atom) {
                        ASSERT_VALID_PROPERTY_CACHE_HIT(0, obj, obj2, entry);
                        JS_UNLOCK_OBJ(cx, obj2);
                        break;
                    }
                } else {
                    entry = NULL;
                    LOAD_ATOM(0);
                }
                id = ATOM_TO_JSID(atom);
                obj = js_FindIdentifierBase(cx, id, entry);
                if (!obj)
                    goto error;
            } while (0);
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_BINDNAME)

#define BITWISE_OP(OP)                                                        \
    JS_BEGIN_MACRO                                                            \
        FETCH_INT(cx, -2, i);                                                 \
        FETCH_INT(cx, -1, j);                                                 \
        i = i OP j;                                                           \
        regs.sp--;                                                            \
        STORE_INT(cx, -1, i);                                                 \
    JS_END_MACRO

          BEGIN_CASE(JSOP_BITOR)
            BITWISE_OP(|);
          END_CASE(JSOP_BITOR)

          BEGIN_CASE(JSOP_BITXOR)
            BITWISE_OP(^);
          END_CASE(JSOP_BITXOR)

          BEGIN_CASE(JSOP_BITAND)
            BITWISE_OP(&);
          END_CASE(JSOP_BITAND)

#define RELATIONAL_OP(OP)                                                     \
    JS_BEGIN_MACRO                                                            \
        rval = FETCH_OPND(-1);                                                \
        lval = FETCH_OPND(-2);                                                \
        /* Optimize for two int-tagged operands (typical loop control). */    \
        if ((lval & rval) & JSVAL_INT) {                                      \
            ltmp = lval ^ JSVAL_VOID;                                         \
            rtmp = rval ^ JSVAL_VOID;                                         \
            if (ltmp && rtmp) {                                               \
                cond = JSVAL_TO_INT(lval) OP JSVAL_TO_INT(rval);              \
            } else {                                                          \
                d  = ltmp ? JSVAL_TO_INT(lval) : *rt->jsNaN;                  \
                d2 = rtmp ? JSVAL_TO_INT(rval) : *rt->jsNaN;                  \
                cond = JSDOUBLE_COMPARE(d, OP, d2, JS_FALSE);                 \
            }                                                                 \
        } else {                                                              \
            if (!JSVAL_IS_PRIMITIVE(lval))                                    \
                DEFAULT_VALUE(cx, -2, JSTYPE_NUMBER, lval);                   \
            if (!JSVAL_IS_PRIMITIVE(rval))                                    \
                DEFAULT_VALUE(cx, -1, JSTYPE_NUMBER, rval);                   \
            if (JSVAL_IS_STRING(lval) && JSVAL_IS_STRING(rval)) {             \
                str  = JSVAL_TO_STRING(lval);                                 \
                str2 = JSVAL_TO_STRING(rval);                                 \
                cond = js_CompareStrings(str, str2) OP 0;                     \
            } else {                                                          \
                VALUE_TO_NUMBER(cx, -2, lval, d);                             \
                VALUE_TO_NUMBER(cx, -1, rval, d2);                            \
                cond = JSDOUBLE_COMPARE(d, OP, d2, JS_FALSE);                 \
            }                                                                 \
        }                                                                     \
        regs.sp--;                                                            \
        STORE_OPND(-1, BOOLEAN_TO_JSVAL(cond));                               \
    JS_END_MACRO

/*
 * NB: These macros can't use JS_BEGIN_MACRO/JS_END_MACRO around their bodies
 * because they begin if/else chains, so callers must not put semicolons after
 * the call expressions!
 */
#if JS_HAS_XML_SUPPORT
#define XML_EQUALITY_OP(OP)                                                   \
    if ((ltmp == JSVAL_OBJECT &&                                              \
         (obj2 = JSVAL_TO_OBJECT(lval)) &&                                    \
         OBJECT_IS_XML(cx, obj2)) ||                                          \
        (rtmp == JSVAL_OBJECT &&                                              \
         (obj2 = JSVAL_TO_OBJECT(rval)) &&                                    \
         OBJECT_IS_XML(cx, obj2))) {                                          \
        JSXMLObjectOps *ops;                                                  \
                                                                              \
        ops = (JSXMLObjectOps *) obj2->map->ops;                              \
        if (obj2 == JSVAL_TO_OBJECT(rval))                                    \
            rval = lval;                                                      \
        if (!ops->equality(cx, obj2, rval, &cond))                            \
            goto error;                                                       \
        cond = cond OP JS_TRUE;                                               \
    } else

#define EXTENDED_EQUALITY_OP(OP)                                              \
    if (ltmp == JSVAL_OBJECT &&                                               \
        (obj2 = JSVAL_TO_OBJECT(lval)) &&                                     \
        ((clasp = OBJ_GET_CLASS(cx, obj2))->flags & JSCLASS_IS_EXTENDED)) {   \
        JSExtendedClass *xclasp;                                              \
                                                                              \
        xclasp = (JSExtendedClass *) clasp;                                   \
        if (!xclasp->equality(cx, obj2, rval, &cond))                         \
            goto error;                                                       \
        cond = cond OP JS_TRUE;                                               \
    } else
#else
#define XML_EQUALITY_OP(OP)             /* nothing */
#define EXTENDED_EQUALITY_OP(OP)        /* nothing */
#endif

#define EQUALITY_OP(OP, IFNAN)                                                \
    JS_BEGIN_MACRO                                                            \
        rval = FETCH_OPND(-1);                                                \
        lval = FETCH_OPND(-2);                                                \
        ltmp = JSVAL_TAG(lval);                                               \
        rtmp = JSVAL_TAG(rval);                                               \
        XML_EQUALITY_OP(OP)                                                   \
        if (ltmp == rtmp) {                                                   \
            if (ltmp == JSVAL_STRING) {                                       \
                str  = JSVAL_TO_STRING(lval);                                 \
                str2 = JSVAL_TO_STRING(rval);                                 \
                cond = js_EqualStrings(str, str2) OP JS_TRUE;                 \
            } else if (ltmp == JSVAL_DOUBLE) {                                \
                d  = *JSVAL_TO_DOUBLE(lval);                                  \
                d2 = *JSVAL_TO_DOUBLE(rval);                                  \
                cond = JSDOUBLE_COMPARE(d, OP, d2, IFNAN);                    \
            } else {                                                          \
                EXTENDED_EQUALITY_OP(OP)                                      \
                /* Handle all undefined (=>NaN) and int combinations. */      \
                cond = lval OP rval;                                          \
            }                                                                 \
        } else {                                                              \
            if (JSVAL_IS_NULL(lval) || JSVAL_IS_VOID(lval)) {                 \
                cond = (JSVAL_IS_NULL(rval) || JSVAL_IS_VOID(rval)) OP 1;     \
            } else if (JSVAL_IS_NULL(rval) || JSVAL_IS_VOID(rval)) {          \
                cond = 1 OP 0;                                                \
            } else {                                                          \
                if (ltmp == JSVAL_OBJECT) {                                   \
                    DEFAULT_VALUE(cx, -2, JSTYPE_VOID, lval);                 \
                    ltmp = JSVAL_TAG(lval);                                   \
                } else if (rtmp == JSVAL_OBJECT) {                            \
                    DEFAULT_VALUE(cx, -1, JSTYPE_VOID, rval);                 \
                    rtmp = JSVAL_TAG(rval);                                   \
                }                                                             \
                if (ltmp == JSVAL_STRING && rtmp == JSVAL_STRING) {           \
                    str  = JSVAL_TO_STRING(lval);                             \
                    str2 = JSVAL_TO_STRING(rval);                             \
                    cond = js_EqualStrings(str, str2) OP JS_TRUE;             \
                } else {                                                      \
                    VALUE_TO_NUMBER(cx, -2, lval, d);                         \
                    VALUE_TO_NUMBER(cx, -1, rval, d2);                        \
                    cond = JSDOUBLE_COMPARE(d, OP, d2, IFNAN);                \
                }                                                             \
            }                                                                 \
        }                                                                     \
        regs.sp--;                                                            \
        STORE_OPND(-1, BOOLEAN_TO_JSVAL(cond));                               \
    JS_END_MACRO

          BEGIN_CASE(JSOP_EQ)
            EQUALITY_OP(==, JS_FALSE);
          END_CASE(JSOP_EQ)

          BEGIN_CASE(JSOP_NE)
            EQUALITY_OP(!=, JS_TRUE);
          END_CASE(JSOP_NE)

#define STRICT_EQUALITY_OP(OP)                                                \
    JS_BEGIN_MACRO                                                            \
        rval = FETCH_OPND(-1);                                                \
        lval = FETCH_OPND(-2);                                                \
        cond = js_StrictlyEqual(cx, lval, rval) OP JS_TRUE;                   \
        regs.sp--;                                                            \
        STORE_OPND(-1, BOOLEAN_TO_JSVAL(cond));                               \
    JS_END_MACRO

          BEGIN_CASE(JSOP_STRICTEQ)
            STRICT_EQUALITY_OP(==);
          END_CASE(JSOP_STRICTEQ)

          BEGIN_CASE(JSOP_STRICTNE)
            STRICT_EQUALITY_OP(!=);
          END_CASE(JSOP_STRICTNE)

          BEGIN_CASE(JSOP_CASE)
            STRICT_EQUALITY_OP(==);
            (void) POP();
            if (cond) {
                len = GET_JUMP_OFFSET(regs.pc);
                CHECK_BRANCH(len);
                DO_NEXT_OP(len);
            }
            PUSH(lval);
          END_CASE(JSOP_CASE)

          BEGIN_CASE(JSOP_CASEX)
            STRICT_EQUALITY_OP(==);
            (void) POP();
            if (cond) {
                len = GET_JUMPX_OFFSET(regs.pc);
                CHECK_BRANCH(len);
                DO_NEXT_OP(len);
            }
            PUSH(lval);
          END_CASE(JSOP_CASEX)

          BEGIN_CASE(JSOP_LT)
            RELATIONAL_OP(<);
          END_CASE(JSOP_LT)

          BEGIN_CASE(JSOP_LE)
            RELATIONAL_OP(<=);
          END_CASE(JSOP_LE)

          BEGIN_CASE(JSOP_GT)
            RELATIONAL_OP(>);
          END_CASE(JSOP_GT)

          BEGIN_CASE(JSOP_GE)
            RELATIONAL_OP(>=);
          END_CASE(JSOP_GE)

#undef EQUALITY_OP
#undef RELATIONAL_OP

/*
 * We do not check for JSVAL_VOID here since ToInt32(undefined) == 0
 * and (JSVAL_TO_INT(JSVAL_VOID) & 31) == 0. The static assert before
 * js_Interpret ensures this.
 */
#define FETCH_SHIFT(shift)                                                    \
    JS_BEGIN_MACRO                                                            \
        jsval v_;                                                             \
                                                                              \
        v_ = FETCH_OPND(-1);                                                  \
        if (v_ & JSVAL_INT) {                                                 \
            shift = JSVAL_TO_INT(v_);                                         \
        } else {                                                              \
            shift = js_ValueToECMAInt32(cx, &regs.sp[-1]);                    \
            if (JSVAL_IS_NULL(regs.sp[-1]))                                   \
                goto error;                                                   \
        }                                                                     \
        shift &= 31;                                                          \
    JS_END_MACRO

#define SIGNED_SHIFT_OP(OP)                                                   \
    JS_BEGIN_MACRO                                                            \
        FETCH_INT(cx, -2, i);                                                 \
        FETCH_SHIFT(j);                                                       \
        i = i OP j;                                                           \
        regs.sp--;                                                            \
        STORE_INT(cx, -1, i);                                                 \
    JS_END_MACRO

          BEGIN_CASE(JSOP_LSH)
            SIGNED_SHIFT_OP(<<);
          END_CASE(JSOP_LSH)

          BEGIN_CASE(JSOP_RSH)
            SIGNED_SHIFT_OP(>>);
          END_CASE(JSOP_RSH)

          BEGIN_CASE(JSOP_URSH)
          {
            uint32 u;

            FETCH_UINT(cx, -2, u);
            FETCH_SHIFT(j);
            u >>= j;
            regs.sp--;
            STORE_UINT(cx, -1, u);
          }
          END_CASE(JSOP_URSH)

#undef BITWISE_OP
#undef SIGNED_SHIFT_OP

          BEGIN_CASE(JSOP_ADD)
            rval = FETCH_OPND(-1);
            lval = FETCH_OPND(-2);
#if JS_HAS_XML_SUPPORT
            if (!JSVAL_IS_PRIMITIVE(lval) &&
                (obj2 = JSVAL_TO_OBJECT(lval), OBJECT_IS_XML(cx, obj2)) &&
                VALUE_IS_XML(cx, rval)) {
                JSXMLObjectOps *ops;

                ops = (JSXMLObjectOps *) obj2->map->ops;
                if (!ops->concatenate(cx, obj2, rval, &rval))
                    goto error;
                regs.sp--;
                STORE_OPND(-1, rval);
            } else
#endif
            {
                if (!JSVAL_IS_PRIMITIVE(lval))
                    DEFAULT_VALUE(cx, -2, JSTYPE_VOID, lval);
                if (!JSVAL_IS_PRIMITIVE(rval))
                    DEFAULT_VALUE(cx, -1, JSTYPE_VOID, rval);
                if ((cond = JSVAL_IS_STRING(lval)) || JSVAL_IS_STRING(rval)) {
                    if (cond) {
                        str = JSVAL_TO_STRING(lval);
                        str2 = js_ValueToString(cx, rval);
                        if (!str2)
                            goto error;
                        regs.sp[-1] = STRING_TO_JSVAL(str2);
                    } else {
                        str2 = JSVAL_TO_STRING(rval);
                        str = js_ValueToString(cx, lval);
                        if (!str)
                            goto error;
                        regs.sp[-2] = STRING_TO_JSVAL(str);
                    }
                    str = js_ConcatStrings(cx, str, str2);
                    if (!str)
                        goto error;
                    regs.sp--;
                    STORE_OPND(-1, STRING_TO_JSVAL(str));
                } else {
                    VALUE_TO_NUMBER(cx, -2, lval, d);
                    VALUE_TO_NUMBER(cx, -1, rval, d2);
                    d += d2;
                    regs.sp--;
                    STORE_NUMBER(cx, -1, d);
                }
            }
          END_CASE(JSOP_ADD)

#define BINARY_OP(OP)                                                         \
    JS_BEGIN_MACRO                                                            \
        FETCH_NUMBER(cx, -2, d);                                              \
        FETCH_NUMBER(cx, -1, d2);                                             \
        d = d OP d2;                                                          \
        regs.sp--;                                                            \
        STORE_NUMBER(cx, -1, d);                                              \
    JS_END_MACRO

          BEGIN_CASE(JSOP_SUB)
            BINARY_OP(-);
          END_CASE(JSOP_SUB)

          BEGIN_CASE(JSOP_MUL)
            BINARY_OP(*);
          END_CASE(JSOP_MUL)

          BEGIN_CASE(JSOP_DIV)
            FETCH_NUMBER(cx, -1, d2);
            FETCH_NUMBER(cx, -2, d);
            regs.sp--;
            if (d2 == 0) {
#ifdef XP_WIN
                /* XXX MSVC miscompiles such that (NaN == 0) */
                if (JSDOUBLE_IS_NaN(d2))
                    rval = DOUBLE_TO_JSVAL(rt->jsNaN);
                else
#endif
                if (d == 0 || JSDOUBLE_IS_NaN(d))
                    rval = DOUBLE_TO_JSVAL(rt->jsNaN);
                else if ((JSDOUBLE_HI32(d) ^ JSDOUBLE_HI32(d2)) >> 31)
                    rval = DOUBLE_TO_JSVAL(rt->jsNegativeInfinity);
                else
                    rval = DOUBLE_TO_JSVAL(rt->jsPositiveInfinity);
                STORE_OPND(-1, rval);
            } else {
                d /= d2;
                STORE_NUMBER(cx, -1, d);
            }
          END_CASE(JSOP_DIV)

          BEGIN_CASE(JSOP_MOD)
            FETCH_NUMBER(cx, -1, d2);
            FETCH_NUMBER(cx, -2, d);
            regs.sp--;
            if (d2 == 0) {
                STORE_OPND(-1, DOUBLE_TO_JSVAL(rt->jsNaN));
            } else {
#ifdef XP_WIN
              /* Workaround MS fmod bug where 42 % (1/0) => NaN, not 42. */
              if (!(JSDOUBLE_IS_FINITE(d) && JSDOUBLE_IS_INFINITE(d2)))
#endif
                d = fmod(d, d2);
                STORE_NUMBER(cx, -1, d);
            }
          END_CASE(JSOP_MOD)

          BEGIN_CASE(JSOP_NOT)
            POP_BOOLEAN(cx, rval, cond);
            PUSH_OPND(BOOLEAN_TO_JSVAL(!cond));
          END_CASE(JSOP_NOT)

          BEGIN_CASE(JSOP_BITNOT)
            FETCH_INT(cx, -1, i);
            i = ~i;
            STORE_INT(cx, -1, i);
          END_CASE(JSOP_BITNOT)

          BEGIN_CASE(JSOP_NEG)
            /*
             * Optimize the case of an int-tagged operand by noting that
             * INT_FITS_IN_JSVAL(i) => INT_FITS_IN_JSVAL(-i) unless i is 0
             * when -i is the negative zero which is jsdouble.
             */
            rval = FETCH_OPND(-1);
            if (JSVAL_IS_INT(rval) && (i = JSVAL_TO_INT(rval)) != 0) {
                i = -i;
                JS_ASSERT(INT_FITS_IN_JSVAL(i));
                regs.sp[-1] = INT_TO_JSVAL(i);
            } else {
                if (JSVAL_IS_DOUBLE(rval)) {
                    d = *JSVAL_TO_DOUBLE(rval);
                } else {
                    d = js_ValueToNumber(cx, &regs.sp[-1]);
                    if (JSVAL_IS_NULL(regs.sp[-1]))
                        goto error;
                    JS_ASSERT(JSVAL_IS_NUMBER(regs.sp[-1]) ||
                              regs.sp[-1] == JSVAL_TRUE);
                }
#ifdef HPUX
                /*
                 * Negation of a zero doesn't produce a negative
                 * zero on HPUX. Perform the operation by bit
                 * twiddling.
                 */
                JSDOUBLE_HI32(d) ^= JSDOUBLE_HI32_SIGNBIT;
#else
                d = -d;
#endif
                if (!js_NewNumberInRootedValue(cx, d, &regs.sp[-1]))
                    goto error;
            }
          END_CASE(JSOP_NEG)

          BEGIN_CASE(JSOP_POS)
            rval = FETCH_OPND(-1);
            if (!JSVAL_IS_NUMBER(rval)) {
                d = js_ValueToNumber(cx, &regs.sp[-1]);
                rval = regs.sp[-1];
                if (JSVAL_IS_NULL(rval))
                    goto error;
                if (rval == JSVAL_TRUE) {
                    if (!js_NewNumberInRootedValue(cx, d, &regs.sp[-1]))
                        goto error;
                } else {
                    JS_ASSERT(JSVAL_IS_NUMBER(rval));
                }
            }
          END_CASE(JSOP_POS)

          BEGIN_CASE(JSOP_NEW)
            /* Get immediate argc and find the constructor function. */
            argc = GET_ARGC(regs.pc);
            vp = regs.sp - (2 + argc);
            JS_ASSERT(vp >= fp->spbase);

            if (!js_InvokeConstructor(cx, argc, vp))
                goto error;
            regs.sp = vp + 1;
            LOAD_INTERRUPT_HANDLER(cx);
          END_CASE(JSOP_NEW)

          BEGIN_CASE(JSOP_DELNAME)
            LOAD_ATOM(0);
            id = ATOM_TO_JSID(atom);
            if (!js_FindProperty(cx, id, &obj, &obj2, &prop))
                goto error;

            /* ECMA says to return true if name is undefined or inherited. */
            PUSH_OPND(JSVAL_TRUE);
            if (prop) {
                OBJ_DROP_PROPERTY(cx, obj2, prop);
                if (!OBJ_DELETE_PROPERTY(cx, obj, id, &regs.sp[-1]))
                    goto error;
            }
          END_CASE(JSOP_DELNAME)

          BEGIN_CASE(JSOP_DELPROP)
            LOAD_ATOM(0);
            id = ATOM_TO_JSID(atom);
            PROPERTY_OP(-1, OBJ_DELETE_PROPERTY(cx, obj, id, &rval));
            STORE_OPND(-1, rval);
          END_CASE(JSOP_DELPROP)

          BEGIN_CASE(JSOP_DELELEM)
            ELEMENT_OP(-1, OBJ_DELETE_PROPERTY(cx, obj, id, &rval));
            regs.sp--;
            STORE_OPND(-1, rval);
          END_CASE(JSOP_DELELEM)

          BEGIN_CASE(JSOP_TYPEOFEXPR)
          BEGIN_CASE(JSOP_TYPEOF)
            rval = FETCH_OPND(-1);
            type = JS_TypeOfValue(cx, rval);
            atom = rt->atomState.typeAtoms[type];
            STORE_OPND(-1, ATOM_KEY(atom));
          END_CASE(JSOP_TYPEOF)

          BEGIN_CASE(JSOP_VOID)
            STORE_OPND(-1, JSVAL_VOID);
          END_CASE(JSOP_VOID)

          BEGIN_CASE(JSOP_INCELEM)
          BEGIN_CASE(JSOP_DECELEM)
          BEGIN_CASE(JSOP_ELEMINC)
          BEGIN_CASE(JSOP_ELEMDEC)
            /*
             * Delay fetching of id until we have the object to ensure
             * the proper evaluation order. See bug 372331.
             */
            id = 0;
            i = -2;
            goto fetch_incop_obj;

          BEGIN_CASE(JSOP_INCPROP)
          BEGIN_CASE(JSOP_DECPROP)
          BEGIN_CASE(JSOP_PROPINC)
          BEGIN_CASE(JSOP_PROPDEC)
            LOAD_ATOM(0);
            id = ATOM_TO_JSID(atom);
            i = -1;

          fetch_incop_obj:
            FETCH_OBJECT(cx, i, lval, obj);
            if (id == 0)
                FETCH_ELEMENT_ID(obj, -1, id);
            goto do_incop;

          BEGIN_CASE(JSOP_INCNAME)
          BEGIN_CASE(JSOP_DECNAME)
          BEGIN_CASE(JSOP_NAMEINC)
          BEGIN_CASE(JSOP_NAMEDEC)
            LOAD_ATOM(0);
            id = ATOM_TO_JSID(atom);
            if (!js_FindProperty(cx, id, &obj, &obj2, &prop))
                goto error;
            if (!prop)
                goto atom_not_defined;
            OBJ_DROP_PROPERTY(cx, obj2, prop);

          do_incop:
          {
            const JSCodeSpec *cs;
            jsval v;

            /*
             * We need a root to store the value to leave on the stack until
             * we have done with OBJ_SET_PROPERTY.
             */
            PUSH_OPND(JSVAL_NULL);
            if (!OBJ_GET_PROPERTY(cx, obj, id, &regs.sp[-1]))
                goto error;

            cs = &js_CodeSpec[op];
            JS_ASSERT(cs->ndefs == 1);
            JS_ASSERT((cs->format & JOF_TMPSLOT_MASK) == JOF_TMPSLOT2);
            v = regs.sp[-1];
            if (JS_LIKELY(CAN_DO_FAST_INC_DEC(v))) {
                jsval incr;

                incr = (cs->format & JOF_INC) ? 2 : -2;
                if (cs->format & JOF_POST) {
                    regs.sp[-1] = v + incr;
                } else {
                    v += incr;
                    regs.sp[-1] = v;
                }
                fp->flags |= JSFRAME_ASSIGNING;
                ok = OBJ_SET_PROPERTY(cx, obj, id, &regs.sp[-1]);
                fp->flags &= ~JSFRAME_ASSIGNING;
                if (!ok)
                    goto error;

                /*
                 * We must set regs.sp[-1] to v for both post and pre increments
                 * as the setter overwrites regs.sp[-1].
                 */
                regs.sp[-1] = v;
            } else {
                /* We need an extra root for the result. */
                PUSH_OPND(JSVAL_NULL);
                if (!js_DoIncDec(cx, cs, &regs.sp[-2], &regs.sp[-1]))
                    goto error;
                fp->flags |= JSFRAME_ASSIGNING;
                ok = OBJ_SET_PROPERTY(cx, obj, id, &regs.sp[-1]);
                fp->flags &= ~JSFRAME_ASSIGNING;
                if (!ok)
                    goto error;
                regs.sp--;
            }

            if (cs->nuses == 0) {
                /* regs.sp[-1] already contains the result of name increment. */
            } else {
                rtmp = regs.sp[-1];
                regs.sp -= cs->nuses;
                regs.sp[-1] = rtmp;
            }
            len = cs->length;
            DO_NEXT_OP(len);
          }

          {
            jsval incr, incr2;

            /* Position cases so the most frequent i++ does not need a jump. */
          BEGIN_CASE(JSOP_DECARG)
            incr = -2; incr2 = -2; goto do_arg_incop;
          BEGIN_CASE(JSOP_ARGDEC)
            incr = -2; incr2 =  0; goto do_arg_incop;
          BEGIN_CASE(JSOP_INCARG)
            incr =  2; incr2 =  2; goto do_arg_incop;
          BEGIN_CASE(JSOP_ARGINC)
            incr =  2; incr2 =  0;

          do_arg_incop:
            slot = GET_ARGNO(regs.pc);
            JS_ASSERT(slot < fp->fun->nargs);
            METER_SLOT_OP(op, slot);
            vp = fp->argv + slot;
            goto do_int_fast_incop;

          BEGIN_CASE(JSOP_DECLOCAL)
            incr = -2; incr2 = -2; goto do_local_incop;
          BEGIN_CASE(JSOP_LOCALDEC)
            incr = -2; incr2 =  0; goto do_local_incop;
          BEGIN_CASE(JSOP_INCLOCAL)
            incr =  2; incr2 =  2; goto do_local_incop;
          BEGIN_CASE(JSOP_LOCALINC)
            incr =  2; incr2 =  0;

          do_local_incop:
            slot = GET_UINT16(regs.pc);
            JS_ASSERT(slot < script->depth);
            vp = fp->spbase + slot;
            goto do_int_fast_incop;

          BEGIN_CASE(JSOP_DECVAR)
            incr = -2; incr2 = -2; goto do_var_incop;
          BEGIN_CASE(JSOP_VARDEC)
            incr = -2; incr2 =  0; goto do_var_incop;
          BEGIN_CASE(JSOP_INCVAR)
            incr =  2; incr2 =  2; goto do_var_incop;
          BEGIN_CASE(JSOP_VARINC)
            incr =  2; incr2 =  0;

          /*
           * do_var_incop comes right before do_int_fast_incop as we want to
           * avoid an extra jump for variable cases as var++ is more frequent
           * than arg++ or local++;
           */
          do_var_incop:
            slot = GET_VARNO(regs.pc);
            JS_ASSERT(slot < fp->fun->u.i.nvars);
            METER_SLOT_OP(op, slot);
            vp = fp->vars + slot;

          do_int_fast_incop:
            rval = *vp;
            if (JS_LIKELY(CAN_DO_FAST_INC_DEC(rval))) {
                *vp = rval + incr;
                PUSH_OPND(rval + incr2);
            } else {
                PUSH_OPND(rval);
                if (!js_DoIncDec(cx, &js_CodeSpec[op], &regs.sp[-1], vp))
                    goto error;
            }
            len = JSOP_INCARG_LENGTH;
            JS_ASSERT(len == js_CodeSpec[op].length);
            DO_NEXT_OP(len);
          }

/* NB: This macro doesn't use JS_BEGIN_MACRO/JS_END_MACRO around its body. */
#define FAST_GLOBAL_INCREMENT_OP(SLOWOP,INCR,INCR2)                           \
    op2 = SLOWOP;                                                             \
    incr = INCR;                                                              \
    incr2 = INCR2;                                                            \
    goto do_global_incop

          {
            jsval incr, incr2;

          BEGIN_CASE(JSOP_DECGVAR)
            FAST_GLOBAL_INCREMENT_OP(JSOP_DECNAME, -2, -2);
          BEGIN_CASE(JSOP_GVARDEC)
            FAST_GLOBAL_INCREMENT_OP(JSOP_NAMEDEC, -2,  0);
          BEGIN_CASE(JSOP_INCGVAR)
              FAST_GLOBAL_INCREMENT_OP(JSOP_INCNAME,  2,  2);
          BEGIN_CASE(JSOP_GVARINC)
            FAST_GLOBAL_INCREMENT_OP(JSOP_NAMEINC,  2,  0);

#undef FAST_GLOBAL_INCREMENT_OP

          do_global_incop:
            JS_ASSERT((js_CodeSpec[op].format & JOF_TMPSLOT_MASK) ==
                      JOF_TMPSLOT2);
            slot = GET_VARNO(regs.pc);
            JS_ASSERT(slot < fp->nvars);
            METER_SLOT_OP(op, slot);
            lval = fp->vars[slot];
            if (JSVAL_IS_NULL(lval)) {
                op = op2;
                DO_OP();
            }
            slot = JSVAL_TO_INT(lval);
            rval = OBJ_GET_SLOT(cx, fp->varobj, slot);
            if (JS_LIKELY(CAN_DO_FAST_INC_DEC(rval))) {
                PUSH_OPND(rval + incr2);
                rval += incr;
            } else {
                PUSH_OPND(rval);
                PUSH_OPND(JSVAL_NULL);  /* Extra root */
                if (!js_DoIncDec(cx, &js_CodeSpec[op], &regs.sp[-2], &regs.sp[-1]))
                    goto error;
                rval = regs.sp[-1];
                --regs.sp;
            }
            OBJ_SET_SLOT(cx, fp->varobj, slot, rval);
            len = JSOP_INCGVAR_LENGTH;  /* all gvar incops are same length */
            JS_ASSERT(len == js_CodeSpec[op].length);
            DO_NEXT_OP(len);
          }

#define COMPUTE_THIS(cx, fp, obj)                                             \
    JS_BEGIN_MACRO                                                            \
        if (fp->flags & JSFRAME_COMPUTED_THIS) {                              \
            obj = fp->thisp;                                                  \
        } else {                                                              \
            obj = js_ComputeThis(cx, JS_TRUE, fp->argv);                      \
            if (!obj)                                                         \
                goto error;                                                   \
            fp->thisp = obj;                                                  \
            fp->flags |= JSFRAME_COMPUTED_THIS;                               \
        }                                                                     \
    JS_END_MACRO

          BEGIN_CASE(JSOP_THIS)
            COMPUTE_THIS(cx, fp, obj);
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_THIS)

          BEGIN_CASE(JSOP_GETTHISPROP)
            i = 0;
            COMPUTE_THIS(cx, fp, obj);
            PUSH(JSVAL_NULL);
            len = JSOP_GETTHISPROP_LENGTH;
            goto do_getprop_with_obj;

#undef COMPUTE_THIS

          BEGIN_CASE(JSOP_GETARGPROP)
            i = ARGNO_LEN;
            slot = GET_ARGNO(regs.pc);
            JS_ASSERT(slot < fp->fun->nargs);
            PUSH_OPND(fp->argv[slot]);
            len = JSOP_GETARGPROP_LENGTH;
            goto do_getprop_body;

          BEGIN_CASE(JSOP_GETVARPROP)
            i = VARNO_LEN;
            slot = GET_VARNO(regs.pc);
            JS_ASSERT(slot < fp->fun->u.i.nvars);
            PUSH_OPND(fp->vars[slot]);
            len = JSOP_GETVARPROP_LENGTH;
            goto do_getprop_body;

          BEGIN_CASE(JSOP_GETLOCALPROP)
            i = UINT16_LEN;
            slot = GET_UINT16(regs.pc);
            JS_ASSERT(slot < script->depth);
            PUSH_OPND(fp->spbase[slot]);
            len = JSOP_GETLOCALPROP_LENGTH;
            goto do_getprop_body;

          BEGIN_CASE(JSOP_GETPROP)
          BEGIN_CASE(JSOP_GETXPROP)
            i = 0;
            len = JSOP_GETPROP_LENGTH;

          do_getprop_body:
            lval = FETCH_OPND(-1);

          do_getprop_with_lval:
            VALUE_TO_OBJECT(cx, -1, lval, obj);

          do_getprop_with_obj:
            do {
                JSPropCacheEntry *entry;

                if (JS_LIKELY(obj->map->ops->getProperty == js_GetProperty)) {
                    PROPERTY_CACHE_TEST(cx, regs.pc, obj, obj2, entry, atom);
                    if (!atom) {
                        ASSERT_VALID_PROPERTY_CACHE_HIT(i, obj, obj2, entry);
                        if (PCVAL_IS_OBJECT(entry->vword)) {
                            rval = PCVAL_OBJECT_TO_JSVAL(entry->vword);
                        } else if (PCVAL_IS_SLOT(entry->vword)) {
                            slot = PCVAL_TO_SLOT(entry->vword);
                            JS_ASSERT(slot < obj2->map->freeslot);
                            rval = LOCKED_OBJ_GET_SLOT(obj2, slot);
                        } else {
                            JS_ASSERT(PCVAL_IS_SPROP(entry->vword));
                            sprop = PCVAL_TO_SPROP(entry->vword);
                            NATIVE_GET(cx, obj, obj2, sprop, &rval);
                        }
                        JS_UNLOCK_OBJ(cx, obj2);
                        break;
                    }
                } else {
                    entry = NULL;
                    if (i < 0)
                        atom = rt->atomState.lengthAtom;
                    else
                        LOAD_ATOM(i);
                }
                id = ATOM_TO_JSID(atom);
                if (entry
                    ? !js_GetPropertyHelper(cx, obj, id, &rval, &entry)
                    : !OBJ_GET_PROPERTY(cx, obj, id, &rval)) {
                    goto error;
                }
            } while (0);

            STORE_OPND(-1, rval);
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_LENGTH)
            lval = FETCH_OPND(-1);
            if (JSVAL_IS_STRING(lval)) {
                str = JSVAL_TO_STRING(lval);
                regs.sp[-1] = INT_TO_JSVAL(JSSTRING_LENGTH(str));
            } else if (!JSVAL_IS_PRIMITIVE(lval) &&
                       (obj = JSVAL_TO_OBJECT(lval), OBJ_IS_ARRAY(cx, obj))) {
                jsuint length;

                /*
                 * We know that the array is created with only its 'length'
                 * private data in a fixed slot at JSSLOT_ARRAY_LENGTH. See
                 * also JSOP_ARRAYPUSH, far below.
                 */
                length = obj->fslots[JSSLOT_ARRAY_LENGTH];
                if (length <= JSVAL_INT_MAX) {
                    regs.sp[-1] = INT_TO_JSVAL(length);
                } else if (!js_NewDoubleInRootedValue(cx, (jsdouble) length,
                                                      &regs.sp[-1])) {
                    goto error;
                }
            } else {
                i = -1;
                len = JSOP_LENGTH_LENGTH;
                goto do_getprop_with_lval;
            }
          END_CASE(JSOP_LENGTH)

          BEGIN_CASE(JSOP_CALLPROP)
          {
            JSObject *aobj;
            JSPropCacheEntry *entry;

            lval = FETCH_OPND(-1);
            if (!JSVAL_IS_PRIMITIVE(lval)) {
                obj = JSVAL_TO_OBJECT(lval);
            } else {
                if (JSVAL_IS_STRING(lval)) {
                    i = JSProto_String;
                } else if (JSVAL_IS_NUMBER(lval)) {
                    i = JSProto_Number;
                } else if (JSVAL_IS_BOOLEAN(lval)) {
                    i = JSProto_Boolean;
                } else {
                    JS_ASSERT(JSVAL_IS_NULL(lval) || JSVAL_IS_VOID(lval));
                    js_ReportIsNullOrUndefined(cx, -1, lval, NULL);
                    goto error;
                }

                if (!js_GetClassPrototype(cx, NULL, INT_TO_JSID(i), &obj))
                    goto error;
            }

            aobj = OBJ_IS_DENSE_ARRAY(cx, obj) ? OBJ_GET_PROTO(cx, obj) : obj;
            if (JS_LIKELY(aobj->map->ops->getProperty == js_GetProperty)) {
                PROPERTY_CACHE_TEST(cx, regs.pc, aobj, obj2, entry, atom);
                if (!atom) {
                    ASSERT_VALID_PROPERTY_CACHE_HIT(0, aobj, obj2, entry);
                    if (PCVAL_IS_OBJECT(entry->vword)) {
                        rval = PCVAL_OBJECT_TO_JSVAL(entry->vword);
                    } else if (PCVAL_IS_SLOT(entry->vword)) {
                        slot = PCVAL_TO_SLOT(entry->vword);
                        JS_ASSERT(slot < obj2->map->freeslot);
                        rval = LOCKED_OBJ_GET_SLOT(obj2, slot);
                    } else {
                        JS_ASSERT(PCVAL_IS_SPROP(entry->vword));
                        sprop = PCVAL_TO_SPROP(entry->vword);
                        NATIVE_GET(cx, obj, obj2, sprop, &rval);
                    }
                    JS_UNLOCK_OBJ(cx, obj2);
                    STORE_OPND(-1, rval);
                    PUSH_OPND(lval);
                    goto end_callprop;
                }
            } else {
                entry = NULL;
                LOAD_ATOM(0);
            }

            /*
             * Cache miss: use the immediate atom that was loaded for us under
             * PROPERTY_CACHE_TEST.
             */
            id = ATOM_TO_JSID(atom);
            PUSH(JSVAL_NULL);
            if (!JSVAL_IS_PRIMITIVE(lval)) {
#if JS_HAS_XML_SUPPORT
                /* Special-case XML object method lookup, per ECMA-357. */
                if (OBJECT_IS_XML(cx, obj)) {
                    JSXMLObjectOps *ops;

                    ops = (JSXMLObjectOps *) obj->map->ops;
                    obj = ops->getMethod(cx, obj, id, &rval);
                    if (!obj)
                        goto error;
                } else
#endif
                if (JS_LIKELY(aobj->map->ops->getProperty == js_GetProperty)
                    ? !js_GetPropertyHelper(cx, aobj, id, &rval, &entry)
                    : !OBJ_GET_PROPERTY(cx, obj, id, &rval)) {
                    goto error;
                }
                STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
                STORE_OPND(-2, rval);
            } else {
                JS_ASSERT(obj->map->ops->getProperty == js_GetProperty);
                if (!js_GetPropertyHelper(cx, obj, id, &rval, &entry))
                    goto error;
                STORE_OPND(-1, lval);
                STORE_OPND(-2, rval);
            }

          end_callprop:
            /* Wrap primitive lval in object clothing if necessary. */
            if (JSVAL_IS_PRIMITIVE(lval)) {
                /* FIXME: https://bugzilla.mozilla.org/show_bug.cgi?id=412571 */
                if (!VALUE_IS_FUNCTION(cx, rval) ||
                    (obj = JSVAL_TO_OBJECT(rval),
                     fun = GET_FUNCTION_PRIVATE(cx, obj),
                     !PRIMITIVE_THIS_TEST(fun, lval))) {
                    if (!js_PrimitiveToObject(cx, &regs.sp[-1]))
                        goto error;
                }
            }
#if JS_HAS_NO_SUCH_METHOD
            if (JS_UNLIKELY(rval == JSVAL_VOID)) {
                LOAD_ATOM(0);
                regs.sp[-2] = ATOM_KEY(atom);
                if (!js_OnUnknownMethod(cx, regs.sp - 2))
                    goto error;
            }
#endif
          }
          END_CASE(JSOP_CALLPROP)

          BEGIN_CASE(JSOP_SETNAME)
          BEGIN_CASE(JSOP_SETPROP)
            rval = FETCH_OPND(-1);
            lval = FETCH_OPND(-2);
            JS_ASSERT(!JSVAL_IS_PRIMITIVE(lval) || op == JSOP_SETPROP);
            VALUE_TO_OBJECT(cx, -2, lval, obj);

            do {
                JSPropCacheEntry *entry;

                entry = NULL;
                atom = NULL;
                if (JS_LIKELY(obj->map->ops->setProperty == js_SetProperty)) {
                    JSPropertyCache *cache = &JS_PROPERTY_CACHE(cx);
                    uint32 kshape = OBJ_SCOPE(obj)->shape;

                    /*
                     * Open-code JS_PROPERTY_CACHE_TEST, specializing for two
                     * important set-property cases. First:
                     *
                     *   function f(a, b, c) {
                     *     var o = {p:a, q:b, r:c};
                     *     return o;
                     *   }
                     *
                     * or similar real-world cases, which evolve a newborn
                     * native object predicatably through some bounded number
                     * of property additions. And second:
                     *
                     *   o.p = x;
                     *
                     * in a frequently executed method or loop body, where p
                     * will (possibly after the first iteration) always exist
                     * in native object o.
                     */
                    entry = &cache->table[PROPERTY_CACHE_HASH_PC(regs.pc,
                                                                 kshape)];
                    PCMETER(cache->tests++);
                    PCMETER(cache->settests++);
                    if (entry->kpc == regs.pc && entry->kshape == kshape) {
                        JSScope *scope;

                        JS_LOCK_OBJ(cx, obj);
                        scope = OBJ_SCOPE(obj);
                        if (scope->shape == kshape) {
                            JS_ASSERT(PCVAL_IS_SPROP(entry->vword));
                            sprop = PCVAL_TO_SPROP(entry->vword);
                            JS_ASSERT(!(sprop->attrs & JSPROP_READONLY));
                            JS_ASSERT(!(sprop->attrs & JSPROP_SHARED));
                            JS_ASSERT(!SCOPE_IS_SEALED(OBJ_SCOPE(obj)));

                            if (scope->object == obj) {
                                /*
                                 * Fastest path: the cached sprop is already
                                 * in scope. Just NATIVE_SET and break to get
                                 * out of the do-while(0).
                                 */
                                if (sprop == scope->lastProp ||
                                    SCOPE_HAS_PROPERTY(scope, sprop)) {
                                    PCMETER(cache->pchits++);
                                    PCMETER(cache->setpchits++);
                                    NATIVE_SET(cx, obj, sprop, &rval);
                                    JS_UNLOCK_SCOPE(cx, scope);
                                    break;
                                }
                            } else {
                                scope = js_GetMutableScope(cx, obj);
                                if (!scope) {
                                    JS_UNLOCK_OBJ(cx, obj);
                                    goto error;
                                }
                            }

                            if (sprop->parent == scope->lastProp &&
                                !SCOPE_HAD_MIDDLE_DELETE(scope) &&
                                SPROP_HAS_STUB_SETTER(sprop) &&
                                (slot = sprop->slot) == scope->map.freeslot) {
                                /*
                                 * Fast path: adding a plain old property that
                                 * was once at the frontier of the property
                                 * tree, whose slot is next to claim among the
                                 * allocated slots in obj, where scope->table
                                 * has not been created yet.
                                 *
                                 * We may want to remove hazard conditions
                                 * above and inline compensation code here,
                                 * depending on real-world workloads.
                                 */
                                JS_ASSERT(!(LOCKED_OBJ_GET_CLASS(obj)->flags &
                                            JSCLASS_SHARE_ALL_PROPERTIES));

                                PCMETER(cache->pchits++);
                                PCMETER(cache->addpchits++);

                                /*
                                 * Beware classes such as Function that use
                                 * the reserveSlots hook to allocate a number
                                 * of reserved slots that may vary with obj.
                                 */
                                if (slot < STOBJ_NSLOTS(obj) &&
                                    !OBJ_GET_CLASS(cx, obj)->reserveSlots) {
                                    ++scope->map.freeslot;
                                } else {
                                    if (!js_AllocSlot(cx, obj, &slot)) {
                                        JS_UNLOCK_SCOPE(cx, scope);
                                        goto error;
                                    }
                                }

                                /*
                                 * If this obj's number of reserved slots
                                 * differed, or if something created a hash
                                 * table for scope, we must pay the price of
                                 * js_AddScopeProperty.
                                 *
                                 * If slot does not match the cached sprop's
                                 * slot, update the cache entry in the hope
                                 * that obj and other instances with the same
                                 * number of reserved slots are now "hot".
                                 */
                                if (slot != sprop->slot || scope->table) {
                                    JSScopeProperty *sprop2 =
                                        js_AddScopeProperty(cx, scope,
                                                            sprop->id,
                                                            sprop->getter,
                                                            sprop->setter,
                                                            slot,
                                                            sprop->attrs,
                                                            sprop->flags,
                                                            sprop->shortid);
                                    if (!sprop2) {
                                        js_FreeSlot(cx, obj, slot);
                                        JS_UNLOCK_SCOPE(cx, scope);
                                        goto error;
                                    }
                                    if (sprop2 != sprop) {
                                        PCMETER(cache->slotchanges++);
                                        JS_ASSERT(slot != sprop->slot &&
                                                  slot == sprop2->slot &&
                                                  sprop2->id == sprop->id);
                                        entry->vword = SPROP_TO_PCVAL(sprop2);
                                    }
                                    sprop = sprop2;
                                } else {
                                    SCOPE_EXTEND_SHAPE(cx, scope, sprop);
                                    ++scope->entryCount;
                                    scope->lastProp = sprop;
                                }

                                GC_WRITE_BARRIER(cx, scope,
                                                 LOCKED_OBJ_GET_SLOT(obj, slot),
                                                 rval);
                                LOCKED_OBJ_SET_SLOT(obj, slot, rval);
                                JS_UNLOCK_SCOPE(cx, scope);
                                break;
                            }

                            PCMETER(cache->setpcmisses++);
                            atom = NULL;
                        }

                        JS_UNLOCK_OBJ(cx, obj);
                    }

                    atom = js_FullTestPropertyCache(cx, regs.pc, &obj, &obj2,
                                                    &entry);
                    if (atom) {
                        PCMETER(cache->misses++);
                        PCMETER(cache->setmisses++);
                    } else {
                        ASSERT_VALID_PROPERTY_CACHE_HIT(0, obj, obj2, entry);
                        if (obj == obj2) {
                            if (PCVAL_IS_SLOT(entry->vword)) {
                                slot = PCVAL_TO_SLOT(entry->vword);
                                JS_ASSERT(slot < obj->map->freeslot);
                                LOCKED_OBJ_WRITE_BARRIER(cx, obj, slot, rval);
                            } else if (PCVAL_IS_SPROP(entry->vword)) {
                                sprop = PCVAL_TO_SPROP(entry->vword);
                                JS_ASSERT(!(sprop->attrs & JSPROP_READONLY));
                                JS_ASSERT(!SCOPE_IS_SEALED(OBJ_SCOPE(obj2)));
                                NATIVE_SET(cx, obj, sprop, &rval);
                            }
                        }
                        JS_UNLOCK_OBJ(cx, obj2);
                        if (obj == obj2 && !PCVAL_IS_OBJECT(entry->vword))
                            break;
                    }
                }

                if (!atom)
                    LOAD_ATOM(0);
                id = ATOM_TO_JSID(atom);
                if (entry
                    ? !js_SetPropertyHelper(cx, obj, id, &rval, &entry)
                    : !OBJ_SET_PROPERTY(cx, obj, id, &rval)) {
                    goto error;
                }
            } while (0);

            regs.sp--;
            STORE_OPND(-1, rval);
          END_CASE(JSOP_SETPROP)

          BEGIN_CASE(JSOP_GETELEM)
            /* Open-coded ELEMENT_OP optimized for strings and dense arrays. */
            lval = FETCH_OPND(-2);
            rval = FETCH_OPND(-1);
            if (JSVAL_IS_STRING(lval) && JSVAL_IS_INT(rval)) {
                str = JSVAL_TO_STRING(lval);
                i = JSVAL_TO_INT(rval);
                if ((size_t)i < JSSTRING_LENGTH(str)) {
                    str = js_GetUnitString(cx, str, (size_t)i);
                    if (!str)
                        goto error;
                    rval = STRING_TO_JSVAL(str);
                    goto end_getelem;
                }
            }

            VALUE_TO_OBJECT(cx, -2, lval, obj);
            if (JSVAL_IS_INT(rval)) {
                if (OBJ_IS_DENSE_ARRAY(cx, obj)) {
                    jsuint length;

                    length = ARRAY_DENSE_LENGTH(obj);
                    i = JSVAL_TO_INT(rval);
                    if ((jsuint)i < length &&
                        i < obj->fslots[JSSLOT_ARRAY_LENGTH]) {
                        rval = obj->dslots[i];
                        if (rval != JSVAL_HOLE)
                            goto end_getelem;

                        /* Reload rval from the stack in the rare hole case. */
                        rval = FETCH_OPND(-1);
                    }
                }
                id = INT_JSVAL_TO_JSID(rval);
            } else {
                if (!js_InternNonIntElementId(cx, obj, rval, &id))
                    goto error;
            }

            if (!OBJ_GET_PROPERTY(cx, obj, id, &rval))
                goto error;
          end_getelem:
            regs.sp--;
            STORE_OPND(-1, rval);
          END_CASE(JSOP_GETELEM)

          BEGIN_CASE(JSOP_CALLELEM)
            /*
             * FIXME: JSOP_CALLELEM should call getMethod on XML objects as
             * CALLPROP does. See bug 362910.
             */
            ELEMENT_OP(-1, OBJ_GET_PROPERTY(cx, obj, id, &rval));
#if JS_HAS_NO_SUCH_METHOD
            if (JS_UNLIKELY(rval == JSVAL_VOID)) {
                regs.sp[-2] = regs.sp[-1];
                regs.sp[-1] = OBJECT_TO_JSVAL(obj);
                if (!js_OnUnknownMethod(cx, regs.sp - 2))
                    goto error;
            } else
#endif
            {
                STORE_OPND(-2, rval);
                STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
            }
          END_CASE(JSOP_CALLELEM)

          BEGIN_CASE(JSOP_SETELEM)
            rval = FETCH_OPND(-1);
            FETCH_OBJECT(cx, -3, lval, obj);
            FETCH_ELEMENT_ID(obj, -2, id);
            if (OBJ_IS_DENSE_ARRAY(cx, obj) && JSID_IS_INT(id)) {
                jsuint length;

                length = ARRAY_DENSE_LENGTH(obj);
                i = JSID_TO_INT(id);
                if ((jsuint)i < length) {
                    if (obj->dslots[i] == JSVAL_HOLE) {
                        if (i >= obj->fslots[JSSLOT_ARRAY_LENGTH])
                            obj->fslots[JSSLOT_ARRAY_LENGTH] = i + 1;
                        obj->fslots[JSSLOT_ARRAY_COUNT]++;
                    }
                    obj->dslots[i] = rval;
                    goto end_setelem;
                }
            }
            if (!OBJ_SET_PROPERTY(cx, obj, id, &rval))
                goto error;
        end_setelem:
            regs.sp -= 2;
            STORE_OPND(-1, rval);
          END_CASE(JSOP_SETELEM)

          BEGIN_CASE(JSOP_ENUMELEM)
            /* Funky: the value to set is under the [obj, id] pair. */
            rval = FETCH_OPND(-3);
            FETCH_OBJECT(cx, -2, lval, obj);
            FETCH_ELEMENT_ID(obj, -1, id);
            if (!OBJ_SET_PROPERTY(cx, obj, id, &rval))
                goto error;
            regs.sp -= 3;
          END_CASE(JSOP_ENUMELEM)

          BEGIN_CASE(JSOP_CALL)
          BEGIN_CASE(JSOP_EVAL)
            argc = GET_ARGC(regs.pc);
            vp = regs.sp - (argc + 2);
            lval = *vp;
            if (VALUE_IS_FUNCTION(cx, lval)) {
                obj = JSVAL_TO_OBJECT(lval);
                fun = GET_FUNCTION_PRIVATE(cx, obj);

                if (FUN_INTERPRETED(fun)) {
                    uintN nframeslots, nvars, missing;
                    JSArena *a;
                    jsuword nbytes;
                    void *newmark;
                    jsval *newsp;
                    JSInlineFrame *newifp;
                    JSInterpreterHook hook;

                    /* Restrict recursion of lightweight functions. */
                    if (inlineCallCount == MAX_INLINE_CALL_COUNT) {
                        js_ReportOverRecursed(cx);
                        goto error;
                    }

                    /* Compute the total number of stack slots needed by fun. */
                    nframeslots = JS_HOWMANY(sizeof(JSInlineFrame),
                                             sizeof(jsval));
                    nvars = fun->u.i.nvars;
                    script = fun->u.i.script;
                    atoms = script->atomMap.vector;
                    nbytes = (nframeslots + nvars + script->depth) *
                             sizeof(jsval);

                    /* Allocate missing expected args adjacent to actuals. */
                    a = cx->stackPool.current;
                    newmark = (void *) a->avail;
                    if (fun->nargs <= argc) {
                        missing = 0;
                    } else {
                        newsp = vp + 2 + fun->nargs;
                        JS_ASSERT(newsp > regs.sp);
                        if ((jsuword) newsp <= a->limit) {
                            if ((jsuword) newsp > a->avail)
                                a->avail = (jsuword) newsp;
                            do {
                                *--newsp = JSVAL_VOID;
                            } while (newsp != regs.sp);
                            missing = 0;
                        } else {
                            missing = fun->nargs - argc;
                            nbytes += (2 + fun->nargs) * sizeof(jsval);
                        }
                    }

                    /* Allocate the inline frame with its vars and operands. */
                    if (a->avail + nbytes <= a->limit) {
                        newsp = (jsval *) a->avail;
                        a->avail += nbytes;
                        JS_ASSERT(missing == 0);
                    } else {
                        JS_ARENA_ALLOCATE_CAST(newsp, jsval *, &cx->stackPool,
                                               nbytes);
                        if (!newsp) {
                            js_ReportOutOfScriptQuota(cx);
                            goto bad_inline_call;
                        }

                        /*
                         * Move args if missing overflow arena a, then push
                         * any missing args.
                         */
                        if (missing) {
                            memcpy(newsp, vp, (2 + argc) * sizeof(jsval));
                            vp = newsp;
                            newsp = vp + 2 + argc;
                            do {
                                *newsp++ = JSVAL_VOID;
                            } while (--missing != 0);
                        }
                    }

                    /* Claim space for the stack frame and initialize it. */
                    newifp = (JSInlineFrame *) newsp;
                    newsp += nframeslots;
                    newifp->frame.callobj = NULL;
                    newifp->frame.argsobj = NULL;
                    newifp->frame.varobj = NULL;
                    newifp->frame.script = script;
                    newifp->frame.callee = obj;
                    newifp->frame.fun = fun;
                    newifp->frame.argc = argc;
                    newifp->frame.argv = vp + 2;
                    newifp->frame.rval = JSVAL_VOID;
                    newifp->frame.nvars = nvars;
                    newifp->frame.vars = newsp;
                    newifp->frame.down = fp;
                    newifp->frame.annotation = NULL;
                    newifp->frame.scopeChain = parent = OBJ_GET_PARENT(cx, obj);
                    newifp->frame.sharpDepth = 0;
                    newifp->frame.sharpArray = NULL;
                    newifp->frame.flags = 0;
                    newifp->frame.dormantNext = NULL;
                    newifp->frame.xmlNamespace = NULL;
                    newifp->frame.blockChain = NULL;
#ifdef DEBUG
                    newifp->frame.pcDisabledSave =
                        JS_PROPERTY_CACHE(cx).disabled;
#endif
                    newifp->mark = newmark;

                    /* Compute the 'this' parameter now that argv is set. */
                    JS_ASSERT(!JSFUN_BOUND_METHOD_TEST(fun->flags));
                    JS_ASSERT(JSVAL_IS_OBJECT(vp[1]));
                    newifp->frame.thisp = (JSObject *)vp[1];

                    /* Push void to initialize local variables. */
                    while (nvars--)
                        *newsp++ = JSVAL_VOID;

                    newifp->frame.regs = NULL;
                    newifp->frame.spbase = NULL;

                    /* Scope with a call object parented by callee's parent. */
                    if (JSFUN_HEAVYWEIGHT_TEST(fun->flags) &&
                        !js_GetCallObject(cx, &newifp->frame, parent)) {
                        goto bad_inline_call;
                    }

                    /* Switch version if currentVersion wasn't overridden. */
                    newifp->callerVersion = (JSVersion) cx->version;
                    if (JS_LIKELY(cx->version == currentVersion)) {
                        currentVersion = (JSVersion) script->version;
                        if (currentVersion != cx->version)
                            js_SetVersion(cx, currentVersion);
                    }

                    /* Push the frame and set interpreter registers. */
                    newifp->callerRegs = regs;
                    fp->regs = &newifp->callerRegs;
                    regs.sp = newifp->frame.spbase = newsp;
                    regs.pc = script->code;
                    newifp->frame.regs = &regs;
                    cx->fp = fp = &newifp->frame;

                    /* Call the debugger hook if present. */
                    hook = cx->debugHooks->callHook;
                    if (hook) {
                        newifp->hookData = hook(cx, &newifp->frame, JS_TRUE, 0,
                                                cx->debugHooks->callHookData);
                        LOAD_INTERRUPT_HANDLER(cx);
                    } else {
                        newifp->hookData = NULL;
                    }

                    inlineCallCount++;
                    JS_RUNTIME_METER(rt, inlineCalls);

#ifdef INCLUDE_MOZILLA_DTRACE
                    /* DTrace function entry, inlines */
                    if (JAVASCRIPT_FUNCTION_ENTRY_ENABLED())
                        jsdtrace_function_entry(cx, fp, fun);
                    if (JAVASCRIPT_FUNCTION_INFO_ENABLED())
                        jsdtrace_function_info(cx, fp, fp->down, fun);
                    if (JAVASCRIPT_FUNCTION_ARGS_ENABLED())
                        jsdtrace_function_args(cx, fp, fun);
#endif

                    /* Load first op and dispatch it (safe since JSOP_STOP). */
                    op = (JSOp) *regs.pc;
                    DO_OP();

                  bad_inline_call:
                    JS_ASSERT(fp->regs == &regs);
                    script = fp->script;
                    atoms = script->atomMap.vector;
                    js_FreeRawStack(cx, newmark);
                    goto error;
                }

#ifdef INCLUDE_MOZILLA_DTRACE
                /* DTrace function entry, non-inlines */
                if (VALUE_IS_FUNCTION(cx, lval)) {
                    if (JAVASCRIPT_FUNCTION_ENTRY_ENABLED())
                        jsdtrace_function_entry(cx, fp, fun);
                    if (JAVASCRIPT_FUNCTION_INFO_ENABLED())
                        jsdtrace_function_info(cx, fp, fp, fun);
                    if (JAVASCRIPT_FUNCTION_ARGS_ENABLED())
                        jsdtrace_function_args(cx, fp, fun);
                }
#endif

                if (fun->flags & JSFUN_FAST_NATIVE) {
                    JS_ASSERT(fun->u.n.extra == 0);
                    if (argc < fun->u.n.minargs) {
                        uintN nargs;

                        /*
                         * If we can't fit missing args and local roots in
                         * this frame's operand stack, take the slow path.
                         */
                        nargs = fun->u.n.minargs - argc;
                        if (regs.sp + nargs > fp->spbase + script->depth)
                            goto do_invoke;
                        do {
                            PUSH(JSVAL_VOID);
                        } while (--nargs != 0);
                    }

                    JS_ASSERT(JSVAL_IS_OBJECT(vp[1]) ||
                              PRIMITIVE_THIS_TEST(fun, vp[1]));

                    ok = ((JSFastNative) fun->u.n.native)(cx, argc, vp);
#ifdef INCLUDE_MOZILLA_DTRACE
                    if (VALUE_IS_FUNCTION(cx, lval)) {
                        if (JAVASCRIPT_FUNCTION_RVAL_ENABLED())
                            jsdtrace_function_rval(cx, fp, fun);
                        if (JAVASCRIPT_FUNCTION_RETURN_ENABLED())
                            jsdtrace_function_return(cx, fp, fun);
                    }
#endif
                    if (!ok)
                        goto error;
                    regs.sp = vp + 1;
                    goto end_call;
                }
            }

          do_invoke:
            ok = js_Invoke(cx, argc, vp, 0);
#ifdef INCLUDE_MOZILLA_DTRACE
            /* DTrace function return, non-inlines */
            if (VALUE_IS_FUNCTION(cx, lval)) {
                if (JAVASCRIPT_FUNCTION_RVAL_ENABLED())
                    jsdtrace_function_rval(cx, fp, fun);
                if (JAVASCRIPT_FUNCTION_RETURN_ENABLED())
                    jsdtrace_function_return(cx, fp, fun);
            }
#endif
            regs.sp = vp + 1;
            LOAD_INTERRUPT_HANDLER(cx);
            if (!ok)
                goto error;
            JS_RUNTIME_METER(rt, nonInlineCalls);

          end_call:
#if JS_HAS_LVALUE_RETURN
            if (cx->rval2set) {
                /*
                 * Use the stack depth we didn't claim in our budget, but that
                 * we know is there on account of [fun, this] already having
                 * been pushed, at a minimum (if no args).  Those two slots
                 * have been popped and [rval] has been pushed, which leaves
                 * one more slot for rval2 before we might overflow.
                 *
                 * NB: rval2 must be the property identifier, and rval the
                 * object from which to get the property.  The pair form an
                 * ECMA "reference type", which can be used on the right- or
                 * left-hand side of assignment ops.  Note well: only native
                 * methods can return reference types.  See JSOP_SETCALL just
                 * below for the left-hand-side case.
                 */
                PUSH_OPND(cx->rval2);
                ELEMENT_OP(-1, OBJ_GET_PROPERTY(cx, obj, id, &rval));

                regs.sp--;
                STORE_OPND(-1, rval);
                cx->rval2set = JS_FALSE;
            }
#endif /* JS_HAS_LVALUE_RETURN */
          END_CASE(JSOP_CALL)

#if JS_HAS_LVALUE_RETURN
          BEGIN_CASE(JSOP_SETCALL)
            argc = GET_ARGC(regs.pc);
            vp = regs.sp - argc - 2;
            ok = js_Invoke(cx, argc, vp, 0);
            regs.sp = vp + 1;
            LOAD_INTERRUPT_HANDLER(cx);
            if (!ok)
                goto error;
            if (!cx->rval2set) {
                op2 = (JSOp) regs.pc[JSOP_SETCALL_LENGTH];
                if (op2 != JSOP_DELELEM) {
                    JS_ASSERT(!(js_CodeSpec[op2].format & JOF_DEL));
                    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                         JSMSG_BAD_LEFTSIDE_OF_ASS);
                    goto error;
                }

                /*
                 * Store true as the result of the emulated delete of a
                 * non-existent property. NB: We don't METER_OP_PAIR here;
                 * it doesn't seem worth the code for this obscure case.
                 */
                *vp = JSVAL_TRUE;
                regs.pc += JSOP_SETCALL_LENGTH + JSOP_DELELEM_LENGTH;
                op = (JSOp) *regs.pc;
                DO_OP();
            }
            PUSH_OPND(cx->rval2);
            cx->rval2set = JS_FALSE;
          END_CASE(JSOP_SETCALL)
#endif

          BEGIN_CASE(JSOP_NAME)
          BEGIN_CASE(JSOP_CALLNAME)
          {
            JSPropCacheEntry *entry;

            obj = fp->scopeChain;
            if (JS_LIKELY(OBJ_IS_NATIVE(obj))) {
                PROPERTY_CACHE_TEST(cx, regs.pc, obj, obj2, entry, atom);
                if (!atom) {
                    ASSERT_VALID_PROPERTY_CACHE_HIT(0, obj, obj2, entry);
                    if (PCVAL_IS_OBJECT(entry->vword)) {
                        rval = PCVAL_OBJECT_TO_JSVAL(entry->vword);
                        JS_UNLOCK_OBJ(cx, obj2);
                        goto do_push_rval;
                    }

                    if (PCVAL_IS_SLOT(entry->vword)) {
                        slot = PCVAL_TO_SLOT(entry->vword);
                        JS_ASSERT(slot < obj2->map->freeslot);
                        rval = LOCKED_OBJ_GET_SLOT(obj2, slot);
                        JS_UNLOCK_OBJ(cx, obj2);
                        goto do_push_rval;
                    }

                    JS_ASSERT(PCVAL_IS_SPROP(entry->vword));
                    sprop = PCVAL_TO_SPROP(entry->vword);
                    goto do_native_get;
                }
            } else {
                entry = NULL;
                LOAD_ATOM(0);
            }

            id = ATOM_TO_JSID(atom);
            if (js_FindPropertyHelper(cx, id, &obj, &obj2, &prop, &entry) < 0)
                goto error;
            if (!prop) {
                /* Kludge to allow (typeof foo == "undefined") tests. */
                len = JSOP_NAME_LENGTH;
                endpc = script->code + script->length;
                for (pc2 = regs.pc + len; pc2 < endpc; pc2++) {
                    op2 = (JSOp)*pc2;
                    if (op2 == JSOP_TYPEOF) {
                        PUSH_OPND(JSVAL_VOID);
                        DO_NEXT_OP(len);
                    }
                    if (op2 != JSOP_GROUP)
                        break;
                }
                goto atom_not_defined;
            }

            /* Take the slow path if prop was not found in a native object. */
            if (!OBJ_IS_NATIVE(obj) || !OBJ_IS_NATIVE(obj2)) {
                OBJ_DROP_PROPERTY(cx, obj2, prop);
                if (!OBJ_GET_PROPERTY(cx, obj, id, &rval))
                    goto error;
                entry = NULL;
            } else {
                sprop = (JSScopeProperty *)prop;
          do_native_get:
                NATIVE_GET(cx, obj, obj2, sprop, &rval);
                OBJ_DROP_PROPERTY(cx, obj2, (JSProperty *) sprop);
            }

          do_push_rval:
            PUSH_OPND(rval);
            if (op == JSOP_CALLNAME)
                PUSH_OPND(OBJECT_TO_JSVAL(obj));
          }
          END_CASE(JSOP_NAME)

          BEGIN_CASE(JSOP_UINT16)
            i = (jsint) GET_UINT16(regs.pc);
            rval = INT_TO_JSVAL(i);
            PUSH_OPND(rval);
          END_CASE(JSOP_UINT16)

          BEGIN_CASE(JSOP_UINT24)
            i = (jsint) GET_UINT24(regs.pc);
            rval = INT_TO_JSVAL(i);
            PUSH_OPND(rval);
          END_CASE(JSOP_UINT24)

          BEGIN_CASE(JSOP_INT8)
            i = GET_INT8(regs.pc);
            rval = INT_TO_JSVAL(i);
            PUSH_OPND(rval);
          END_CASE(JSOP_INT8)

          BEGIN_CASE(JSOP_INT32)
            i = GET_INT32(regs.pc);
            rval = INT_TO_JSVAL(i);
            PUSH_OPND(rval);
          END_CASE(JSOP_INT32)

          BEGIN_CASE(JSOP_INDEXBASE)
            /*
             * Here atoms can exceed script->atomMap.length as we use atoms
             * as a segment register for object literals as well.
             */
            atoms += GET_INDEXBASE(regs.pc);
          END_CASE(JSOP_INDEXBASE)

          BEGIN_CASE(JSOP_INDEXBASE1)
          BEGIN_CASE(JSOP_INDEXBASE2)
          BEGIN_CASE(JSOP_INDEXBASE3)
            atoms += (op - JSOP_INDEXBASE1 + 1) << 16;
          END_CASE(JSOP_INDEXBASE3)

          BEGIN_CASE(JSOP_RESETBASE0)
          BEGIN_CASE(JSOP_RESETBASE)
            atoms = script->atomMap.vector;
          END_CASE(JSOP_RESETBASE)

          BEGIN_CASE(JSOP_DOUBLE)
          BEGIN_CASE(JSOP_STRING)
            LOAD_ATOM(0);
            PUSH_OPND(ATOM_KEY(atom));
          END_CASE(JSOP_DOUBLE)

          BEGIN_CASE(JSOP_OBJECT)
            LOAD_OBJECT(0);
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_OBJECT)

          BEGIN_CASE(JSOP_REGEXP)
          {
            JSObject *funobj;

            /*
             * Push a regexp object for the atom mapped by the bytecode at pc,
             * cloning the literal's regexp object if necessary, to simulate in
             * the pre-compile/execute-later case what ECMA specifies for the
             * compile-and-go case: that scanning each regexp literal creates
             * a single corresponding RegExp object.
             *
             * To support pre-compilation transparently, we must handle the
             * case where a regexp object literal is used in a different global
             * at execution time from the global with which it was scanned at
             * compile time.  We do this by re-wrapping the JSRegExp private
             * data struct with a cloned object having the right prototype and
             * parent, and having its own lastIndex property value storage.
             *
             * Unlike JSOP_DEFFUN and other prolog bytecodes that may clone
             * literal objects, we don't want to pay a script prolog execution
             * price for all regexp literals in a script (many may not be used
             * by a particular execution of that script, depending on control
             * flow), so we initialize lazily here.
             *
             * XXX This code is specific to regular expression objects.  If we
             * need a similar op for other kinds of object literals, we should
             * push cloning down under JSObjectOps and reuse code here.
             */
            index = GET_FULL_INDEX(0);
            JS_ASSERT(index < JS_SCRIPT_REGEXPS(script)->length);

            slot = index;
            if (fp->fun) {
                /*
                 * We're in function code, not global or eval code (in eval
                 * code, JSOP_REGEXP is never emitted). The cloned funobj
                 * contains script->regexps->nregexps reserved slot for the
                 * cloned regexps, see fun_reserveSlots, jsfun.c.
                 */
                funobj = fp->callee;
                slot += JSCLASS_RESERVED_SLOTS(&js_FunctionClass);
                if (!JS_GetReservedSlot(cx, funobj, slot, &rval))
                    goto error;
                if (JSVAL_IS_VOID(rval))
                    rval = JSVAL_NULL;
            } else {
                /*
                 * We're in global code.  The code generator already arranged
                 * via script->nregexps to reserve a global variable slot
                 * at cloneIndex.  All global variable slots are initialized
                 * to null, not void, for faster testing in JSOP_*GVAR cases.
                 */
                slot += script->ngvars;
                rval = fp->vars[slot];
#ifdef __GNUC__
                funobj = NULL;  /* suppress bogus gcc warnings */
#endif
            }

            if (JSVAL_IS_NULL(rval)) {
                /* Compute the current global object in obj2. */
                obj2 = fp->scopeChain;
                while ((parent = OBJ_GET_PARENT(cx, obj2)) != NULL)
                    obj2 = parent;

                /*
                 * If obj's parent is not obj2, we must clone obj so that it
                 * has the right parent, and therefore, the right prototype.
                 *
                 * Yes, this means we assume that the correct RegExp.prototype
                 * to which regexp instances (including literals) delegate can
                 * be distinguished solely by the instance's parent, which was
                 * set to the parent of the RegExp constructor function object
                 * when the instance was created.  In other words,
                 *
                 *   (/x/.__parent__ == RegExp.__parent__) implies
                 *   (/x/.__proto__ == RegExp.prototype)
                 *
                 * (unless you assign a different object to RegExp.prototype
                 * at runtime, in which case, ECMA doesn't specify operation,
                 * and you get what you deserve).
                 *
                 * This same coupling between instance parent and constructor
                 * parent turns up everywhere (see jsobj.c's FindClassObject,
                 * js_ConstructObject, and js_NewObject).  It's fundamental to
                 * the design of the language when you consider multiple global
                 * objects and separate compilation and execution, even though
                 * it is not specified fully in ECMA.
                 */
                JS_GET_SCRIPT_REGEXP(script, index, obj);
                if (OBJ_GET_PARENT(cx, obj) != obj2) {
                    obj = js_CloneRegExpObject(cx, obj, obj2);
                    if (!obj)
                        goto error;
                }
                rval = OBJECT_TO_JSVAL(obj);

                /* Store the regexp object value in its cloneIndex slot. */
                if (fp->fun) {
                    if (!JS_SetReservedSlot(cx, funobj, slot, rval))
                        goto error;
                } else {
                    fp->vars[slot] = rval;
                }
            }

            PUSH_OPND(rval);
          }
          END_CASE(JSOP_REGEXP)

          BEGIN_CASE(JSOP_ZERO)
            PUSH_OPND(JSVAL_ZERO);
          END_CASE(JSOP_ZERO)

          BEGIN_CASE(JSOP_ONE)
            PUSH_OPND(JSVAL_ONE);
          END_CASE(JSOP_ONE)

          BEGIN_CASE(JSOP_NULL)
            PUSH_OPND(JSVAL_NULL);
          END_CASE(JSOP_NULL)

          BEGIN_CASE(JSOP_FALSE)
            PUSH_OPND(JSVAL_FALSE);
          END_CASE(JSOP_FALSE)

          BEGIN_CASE(JSOP_TRUE)
            PUSH_OPND(JSVAL_TRUE);
          END_CASE(JSOP_TRUE)

          BEGIN_CASE(JSOP_TABLESWITCH)
            pc2 = regs.pc;
            len = GET_JUMP_OFFSET(pc2);

            /*
             * ECMAv2+ forbids conversion of discriminant, so we will skip to
             * the default case if the discriminant isn't already an int jsval.
             * (This opcode is emitted only for dense jsint-domain switches.)
             */
            rval = POP_OPND();
            if (!JSVAL_IS_INT(rval))
                DO_NEXT_OP(len);
            i = JSVAL_TO_INT(rval);

            pc2 += JUMP_OFFSET_LEN;
            low = GET_JUMP_OFFSET(pc2);
            pc2 += JUMP_OFFSET_LEN;
            high = GET_JUMP_OFFSET(pc2);

            i -= low;
            if ((jsuint)i < (jsuint)(high - low + 1)) {
                pc2 += JUMP_OFFSET_LEN + JUMP_OFFSET_LEN * i;
                off = (jsint) GET_JUMP_OFFSET(pc2);
                if (off)
                    len = off;
            }
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_TABLESWITCHX)
            pc2 = regs.pc;
            len = GET_JUMPX_OFFSET(pc2);

            /*
             * ECMAv2+ forbids conversion of discriminant, so we will skip to
             * the default case if the discriminant isn't already an int jsval.
             * (This opcode is emitted only for dense jsint-domain switches.)
             */
            rval = POP_OPND();
            if (!JSVAL_IS_INT(rval))
                DO_NEXT_OP(len);
            i = JSVAL_TO_INT(rval);

            pc2 += JUMPX_OFFSET_LEN;
            low = GET_JUMP_OFFSET(pc2);
            pc2 += JUMP_OFFSET_LEN;
            high = GET_JUMP_OFFSET(pc2);

            i -= low;
            if ((jsuint)i < (jsuint)(high - low + 1)) {
                pc2 += JUMP_OFFSET_LEN + JUMPX_OFFSET_LEN * i;
                off = (jsint) GET_JUMPX_OFFSET(pc2);
                if (off)
                    len = off;
            }
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_LOOKUPSWITCHX)
            off = JUMPX_OFFSET_LEN;
            goto do_lookup_switch;

          BEGIN_CASE(JSOP_LOOKUPSWITCH)
            off = JUMP_OFFSET_LEN;

          do_lookup_switch:
            /*
             * JSOP_LOOKUPSWITCH and JSOP_LOOKUPSWITCHX are never used if
             * any atom index in it would exceed 64K limit.
             */
            JS_ASSERT(atoms == script->atomMap.vector);
            pc2 = regs.pc;
            lval = POP_OPND();

            if (!JSVAL_IS_NUMBER(lval) &&
                !JSVAL_IS_STRING(lval) &&
                !JSVAL_IS_BOOLEAN(lval)) {
                goto end_lookup_switch;
            }

            pc2 += off;
            npairs = (jsint) GET_UINT16(pc2);
            pc2 += UINT16_LEN;
            JS_ASSERT(npairs);  /* empty switch uses JSOP_TABLESWITCH */

#define SEARCH_PAIRS(MATCH_CODE)                                              \
    for (;;) {                                                                \
        JS_ASSERT(GET_INDEX(pc2) < script->atomMap.length);                   \
        atom = atoms[GET_INDEX(pc2)];                                         \
        rval = ATOM_KEY(atom);                                                \
        MATCH_CODE                                                            \
        pc2 += INDEX_LEN;                                                     \
        if (match)                                                            \
            break;                                                            \
        pc2 += off;                                                           \
        if (--npairs == 0) {                                                  \
            pc2 = regs.pc;                                                    \
            break;                                                            \
        }                                                                     \
    }
            if (JSVAL_IS_STRING(lval)) {
                str = JSVAL_TO_STRING(lval);
                SEARCH_PAIRS(
                    match = (JSVAL_IS_STRING(rval) &&
                             ((str2 = JSVAL_TO_STRING(rval)) == str ||
                              js_EqualStrings(str2, str)));
                )
            } else if (JSVAL_IS_DOUBLE(lval)) {
                d = *JSVAL_TO_DOUBLE(lval);
                SEARCH_PAIRS(
                    match = (JSVAL_IS_DOUBLE(rval) &&
                             *JSVAL_TO_DOUBLE(rval) == d);
                )
            } else {
                SEARCH_PAIRS(
                    match = (lval == rval);
                )
            }
#undef SEARCH_PAIRS

          end_lookup_switch:
            len = (op == JSOP_LOOKUPSWITCH)
                  ? GET_JUMP_OFFSET(pc2)
                  : GET_JUMPX_OFFSET(pc2);
          END_VARLEN_CASE

          EMPTY_CASE(JSOP_CONDSWITCH)

#if JS_HAS_EXPORT_IMPORT
          BEGIN_CASE(JSOP_EXPORTALL)
            obj = fp->varobj;
            ida = JS_Enumerate(cx, obj);
            if (!ida)
                goto error;
            ok = JS_TRUE;
            for (i = 0; i != ida->length; i++) {
                id = ida->vector[i];
                ok = OBJ_LOOKUP_PROPERTY(cx, obj, id, &obj2, &prop);
                if (!ok)
                    break;
                if (!prop)
                    continue;
                ok = OBJ_GET_ATTRIBUTES(cx, obj, id, prop, &attrs);
                if (ok) {
                    attrs |= JSPROP_EXPORTED;
                    ok = OBJ_SET_ATTRIBUTES(cx, obj, id, prop, &attrs);
                }
                OBJ_DROP_PROPERTY(cx, obj2, prop);
                if (!ok)
                    break;
            }
            JS_ASSERT(ok == (i == ida->length));
            JS_DestroyIdArray(cx, ida);
            if (!ok)
                goto error;
          END_CASE(JSOP_EXPORTALL)

          BEGIN_CASE(JSOP_EXPORTNAME)
            LOAD_ATOM(0);
            id = ATOM_TO_JSID(atom);
            obj = fp->varobj;
            if (!OBJ_LOOKUP_PROPERTY(cx, obj, id, &obj2, &prop))
                goto error;
            if (!prop) {
                if (!OBJ_DEFINE_PROPERTY(cx, obj, id, JSVAL_VOID,
                                         JS_PropertyStub, JS_PropertyStub,
                                         JSPROP_EXPORTED, NULL)) {
                    goto error;
                }
            } else {
                ok = OBJ_GET_ATTRIBUTES(cx, obj, id, prop, &attrs);
                if (ok) {
                    attrs |= JSPROP_EXPORTED;
                    ok = OBJ_SET_ATTRIBUTES(cx, obj, id, prop, &attrs);
                }
                OBJ_DROP_PROPERTY(cx, obj2, prop);
                if (!ok)
                    goto error;
            }
          END_CASE(JSOP_EXPORTNAME)

          BEGIN_CASE(JSOP_IMPORTALL)
            id = (jsid) JSVAL_VOID;
            PROPERTY_OP(-1, js_ImportProperty(cx, obj, id));
            regs.sp--;
          END_CASE(JSOP_IMPORTALL)

          BEGIN_CASE(JSOP_IMPORTPROP)
            /* Get an immediate atom naming the property. */
            LOAD_ATOM(0);
            id = ATOM_TO_JSID(atom);
            PROPERTY_OP(-1, js_ImportProperty(cx, obj, id));
            regs.sp--;
          END_CASE(JSOP_IMPORTPROP)

          BEGIN_CASE(JSOP_IMPORTELEM)
            ELEMENT_OP(-1, js_ImportProperty(cx, obj, id));
            regs.sp -= 2;
          END_CASE(JSOP_IMPORTELEM)
#endif /* JS_HAS_EXPORT_IMPORT */

          BEGIN_CASE(JSOP_TRAP)
            switch (JS_HandleTrap(cx, script, regs.pc, &rval)) {
              case JSTRAP_ERROR:
                goto error;
              case JSTRAP_CONTINUE:
                JS_ASSERT(JSVAL_IS_INT(rval));
                op = (JSOp) JSVAL_TO_INT(rval);
                JS_ASSERT((uintN)op < (uintN)JSOP_LIMIT);
                LOAD_INTERRUPT_HANDLER(cx);
                DO_OP();
              case JSTRAP_RETURN:
                fp->rval = rval;
                ok = JS_TRUE;
                goto forced_return;
              case JSTRAP_THROW:
                cx->throwing = JS_TRUE;
                cx->exception = rval;
                goto error;
              default:;
            }
            LOAD_INTERRUPT_HANDLER(cx);
          END_CASE(JSOP_TRAP)

          BEGIN_CASE(JSOP_ARGUMENTS)
            if (!js_GetArgsValue(cx, fp, &rval))
                goto error;
            PUSH_OPND(rval);
          END_CASE(JSOP_ARGUMENTS)

          BEGIN_CASE(JSOP_ARGSUB)
            id = INT_TO_JSID(GET_ARGNO(regs.pc));
            if (!js_GetArgsProperty(cx, fp, id, &rval))
                goto error;
            PUSH_OPND(rval);
          END_CASE(JSOP_ARGSUB)

          BEGIN_CASE(JSOP_ARGCNT)
            id = ATOM_TO_JSID(rt->atomState.lengthAtom);
            if (!js_GetArgsProperty(cx, fp, id, &rval))
                goto error;
            PUSH_OPND(rval);
          END_CASE(JSOP_ARGCNT)

          BEGIN_CASE(JSOP_GETARG)
          BEGIN_CASE(JSOP_CALLARG)
            slot = GET_ARGNO(regs.pc);
            JS_ASSERT(slot < fp->fun->nargs);
            METER_SLOT_OP(op, slot);
            PUSH_OPND(fp->argv[slot]);
            if (op == JSOP_CALLARG)
                PUSH_OPND(JSVAL_NULL);
          END_CASE(JSOP_GETARG)

          BEGIN_CASE(JSOP_SETARG)
            slot = GET_ARGNO(regs.pc);
            JS_ASSERT(slot < fp->fun->nargs);
            METER_SLOT_OP(op, slot);
            vp = &fp->argv[slot];
            GC_POKE(cx, *vp);
            *vp = FETCH_OPND(-1);
          END_CASE(JSOP_SETARG)

          BEGIN_CASE(JSOP_GETVAR)
          BEGIN_CASE(JSOP_CALLVAR)
            slot = GET_VARNO(regs.pc);
            JS_ASSERT(slot < fp->fun->u.i.nvars);
            METER_SLOT_OP(op, slot);
            PUSH_OPND(fp->vars[slot]);
            if (op == JSOP_CALLVAR)
                PUSH_OPND(JSVAL_NULL);
          END_CASE(JSOP_GETVAR)

          BEGIN_CASE(JSOP_SETVAR)
            slot = GET_VARNO(regs.pc);
            JS_ASSERT(slot < fp->fun->u.i.nvars);
            METER_SLOT_OP(op, slot);
            vp = &fp->vars[slot];
            GC_POKE(cx, *vp);
            *vp = FETCH_OPND(-1);
          END_CASE(JSOP_SETVAR)

          BEGIN_CASE(JSOP_GETGVAR)
          BEGIN_CASE(JSOP_CALLGVAR)
            slot = GET_VARNO(regs.pc);
            JS_ASSERT(slot < fp->nvars);
            METER_SLOT_OP(op, slot);
            lval = fp->vars[slot];
            if (JSVAL_IS_NULL(lval)) {
                op = (op == JSOP_GETGVAR) ? JSOP_NAME : JSOP_CALLNAME;
                DO_OP();
            }
            slot = JSVAL_TO_INT(lval);
            obj = fp->varobj;
            rval = OBJ_GET_SLOT(cx, obj, slot);
            PUSH_OPND(rval);
            if (op == JSOP_CALLGVAR)
                PUSH_OPND(OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_GETGVAR)

          BEGIN_CASE(JSOP_SETGVAR)
            slot = GET_VARNO(regs.pc);
            JS_ASSERT(slot < fp->nvars);
            METER_SLOT_OP(op, slot);
            rval = FETCH_OPND(-1);
            lval = fp->vars[slot];
            obj = fp->varobj;
            if (JSVAL_IS_NULL(lval)) {
                /*
                 * Inline-clone and deoptimize JSOP_SETNAME code here because
                 * JSOP_SETGVAR has arity 1: [rval], not arity 2: [obj, rval]
                 * as JSOP_SETNAME does, where [obj] is due to JSOP_BINDNAME.
                 */
                LOAD_ATOM(0);
                id = ATOM_TO_JSID(atom);
                if (!OBJ_SET_PROPERTY(cx, obj, id, &rval))
                    goto error;
                STORE_OPND(-1, rval);
            } else {
                slot = JSVAL_TO_INT(lval);
                JS_LOCK_OBJ(cx, obj);
                LOCKED_OBJ_WRITE_BARRIER(cx, obj, slot, rval);
                JS_UNLOCK_OBJ(cx, obj);
            }
          END_CASE(JSOP_SETGVAR)

          BEGIN_CASE(JSOP_DEFCONST)
          BEGIN_CASE(JSOP_DEFVAR)
            index = GET_INDEX(regs.pc);
            atom = atoms[index];

            /*
             * index is relative to atoms at this point but for global var
             * code below we need the absolute value.
             */
            index += atoms - script->atomMap.vector;
            obj = fp->varobj;
            attrs = JSPROP_ENUMERATE;
            if (!(fp->flags & JSFRAME_EVAL))
                attrs |= JSPROP_PERMANENT;
            if (op == JSOP_DEFCONST)
                attrs |= JSPROP_READONLY;

            /* Lookup id in order to check for redeclaration problems. */
            id = ATOM_TO_JSID(atom);
            prop = NULL;
            if (!js_CheckRedeclaration(cx, obj, id, attrs, &obj2, &prop))
                goto error;

            /* Bind a variable only if it's not yet defined. */
            if (!prop) {
                if (!OBJ_DEFINE_PROPERTY(cx, obj, id, JSVAL_VOID,
                                         JS_PropertyStub, JS_PropertyStub,
                                         attrs, &prop)) {
                    goto error;
                }
                JS_ASSERT(prop);
                obj2 = obj;
            }

            /*
             * Try to optimize a property we either just created, or found
             * directly in the global object, that is permanent, has a slot,
             * and has stub getter and setter, into a "fast global" accessed
             * by the JSOP_*GVAR opcodes.
             */
            if (index < script->ngvars &&
                obj2 == obj &&
                OBJ_IS_NATIVE(obj)) {
                sprop = (JSScopeProperty *) prop;
                if ((sprop->attrs & JSPROP_PERMANENT) &&
                    SPROP_HAS_VALID_SLOT(sprop, OBJ_SCOPE(obj)) &&
                    SPROP_HAS_STUB_GETTER(sprop) &&
                    SPROP_HAS_STUB_SETTER(sprop)) {
                    /*
                     * Fast globals use fp->vars to map the global name's
                     * atom index to the permanent fp->varobj slot number,
                     * tagged as a jsval.  The atom index for the global's
                     * name literal is identical to its fp->vars index.
                     */
                    fp->vars[index] = INT_TO_JSVAL(sprop->slot);
                }
            }

            OBJ_DROP_PROPERTY(cx, obj2, prop);
          END_CASE(JSOP_DEFVAR)

          BEGIN_CASE(JSOP_DEFFUN)
            LOAD_FUNCTION(0);

            /*
             * We must be at top-level (either outermost block that forms a
             * function's body, or a global) scope, not inside an expression
             * (JSOP_{ANON,NAMED}FUNOBJ) or compound statement (JSOP_CLOSURE)
             * in the same compilation unit (ECMA Program). We also not inside
             * an eval script.
             *
             * If static link is not current scope, clone fun's object to link
             * to the current scope via parent.  This clause exists to enable
             * sharing of compiled functions among multiple equivalent scopes,
             * splitting the cost of compilation evenly among the scopes and
             * amortizing it over a number of executions.  Examples include XUL
             * scripts and event handlers shared among Mozilla chrome windows,
             * and server-side JS user-defined functions shared among requests.
             *
             * NB: The Script object exposes compile and exec in the language,
             * such that this clause introduces an incompatible change from old
             * JS versions that supported Script.  Such a JS version supported
             * executing a script that defined and called functions scoped by
             * the compile-time static link, not by the exec-time scope chain.
             *
             * We sacrifice compatibility, breaking such scripts, in order to
             * promote compile-cost sharing and amortizing, and because Script
             * is not and will not be standardized.
             */
            JS_ASSERT(!fp->blockChain);
            JS_ASSERT((fp->flags & JSFRAME_EVAL) == 0);
            JS_ASSERT(fp->scopeChain == fp->varobj);
            obj2 = fp->scopeChain;

            /*
             * ECMA requires functions defined when entering Global code to be
             * permanent.
             */
            attrs = JSPROP_ENUMERATE | JSPROP_PERMANENT;

          do_deffun:
            /*
             * The common code for JSOP_DEFFUN and JSOP_CLOSURE.
             *
             * Clone the function object with the current scope chain as the
             * clone's parent.  The original function object is the prototype
             * of the clone.  Do this only if re-parenting; the compiler may
             * have seen the right parent already and created a sufficiently
             * well-scoped function object.
             */
            obj = FUN_OBJECT(fun);
            if (OBJ_GET_PARENT(cx, obj) != obj2) {
                obj = js_CloneFunctionObject(cx, fun, obj2);
                if (!obj)
                    goto error;
            }

            /*
             * Protect obj from any GC hiding below OBJ_DEFINE_PROPERTY.  All
             * paths from here must flow through the "Restore fp->scopeChain"
             * code below the OBJ_DEFINE_PROPERTY call.
             */
            fp->scopeChain = obj;
            rval = OBJECT_TO_JSVAL(obj);

            /*
             * Load function flags that are also property attributes.  Getters
             * and setters do not need a slot, their value is stored elsewhere
             * in the property itself, not in obj slots.
             */
            flags = JSFUN_GSFLAG2ATTR(fun->flags);
            if (flags) {
                attrs |= flags | JSPROP_SHARED;
                rval = JSVAL_VOID;
            }

            /*
             * We define the function as a property of the variable object and
             * not the current scope chain even for the case of function
             * expression statements and functions defined by eval inside let
             * or with blocks.
             */
            parent = fp->varobj;

            /*
             * Check for a const property of the same name -- or any kind
             * of property if executing with the strict option.  We check
             * here at runtime as well as at compile-time, to handle eval
             * as well as multiple HTML script tags.
             */
            id = ATOM_TO_JSID(fun->atom);
            ok = js_CheckRedeclaration(cx, parent, id, attrs, NULL, NULL);
            if (ok) {
                if (attrs == JSPROP_ENUMERATE) {
                    JS_ASSERT(fp->flags & JSFRAME_EVAL);
                    JS_ASSERT(op == JSOP_CLOSURE);
                    ok = OBJ_SET_PROPERTY(cx, parent, id, &rval);
                } else {
                    ok = OBJ_DEFINE_PROPERTY(cx, parent, id, rval,
                                             (flags & JSPROP_GETTER)
                                             ? JS_EXTENSION (JSPropertyOp) obj
                                             : JS_PropertyStub,
                                             (flags & JSPROP_SETTER)
                                             ? JS_EXTENSION (JSPropertyOp) obj
                                             : JS_PropertyStub,
                                             attrs,
                                             NULL);
                }
            }

            /* Restore fp->scopeChain now that obj is defined in fp->varobj. */
            fp->scopeChain = obj2;
            if (!ok) {
                cx->weakRoots.newborn[GCX_OBJECT] = NULL;
                goto error;
            }
          END_CASE(JSOP_DEFFUN)

          BEGIN_CASE(JSOP_DEFLOCALFUN)
            LOAD_FUNCTION(VARNO_LEN);

            /*
             * Define a local function (i.e., one nested at the top level of
             * another function), parented by the current scope chain, and
             * stored in a local variable slot that the compiler allocated.
             * This is an optimization over JSOP_DEFFUN that avoids requiring
             * a call object for the outer function's activation.
             */
            slot = GET_VARNO(regs.pc);

            parent = js_GetScopeChain(cx, fp);
            if (!parent)
                goto error;

            obj = js_CloneFunctionObject(cx, fun, parent);
            if (!obj)
                goto error;

            fp->vars[slot] = OBJECT_TO_JSVAL(obj);
          END_CASE(JSOP_DEFLOCALFUN)

          BEGIN_CASE(JSOP_ANONFUNOBJ)
            /* Load the specified function object literal. */
            LOAD_FUNCTION(0);

            /* If re-parenting, push a clone of the function object. */
            parent = js_GetScopeChain(cx, fp);
            if (!parent)
                goto error;
            obj = FUN_OBJECT(fun);
            if (OBJ_GET_PARENT(cx, obj) != parent) {
                obj = js_CloneFunctionObject(cx, fun, parent);
                if (!obj)
                    goto error;
            }
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_ANONFUNOBJ)

          BEGIN_CASE(JSOP_NAMEDFUNOBJ)
            LOAD_FUNCTION(0);

            /*
             * ECMA ed. 3 FunctionExpression: function Identifier [etc.].
             *
             * 1. Create a new object as if by the expression new Object().
             * 2. Add Result(1) to the front of the scope chain.
             *
             * Step 2 is achieved by making the new object's parent be the
             * current scope chain, and then making the new object the parent
             * of the Function object clone.
             */
            obj2 = js_GetScopeChain(cx, fp);
            if (!obj2)
                goto error;
            parent = js_NewObject(cx, &js_ObjectClass, NULL, obj2, 0);
            if (!parent)
                goto error;

            /*
             * 3. Create a new Function object as specified in section 13.2
             * with [parameters and body specified by the function expression
             * that was parsed by the compiler into a Function object, and
             * saved in the script's atom map].
             *
             * Protect parent from the GC.
             */
            fp->scopeChain = parent;
            obj = js_CloneFunctionObject(cx, fun, parent);
            if (!obj)
                goto error;

            /*
             * Protect obj from any GC hiding below OBJ_DEFINE_PROPERTY.  All
             * paths from here must flow through the "Restore fp->scopeChain"
             * code below the OBJ_DEFINE_PROPERTY call.
             */
            fp->scopeChain = obj;
            rval = OBJECT_TO_JSVAL(obj);

            /*
             * 4. Create a property in the object Result(1).  The property's
             * name is [fun->atom, the identifier parsed by the compiler],
             * value is Result(3), and attributes are { DontDelete, ReadOnly }.
             */
            attrs = JSFUN_GSFLAG2ATTR(fun->flags);
            if (attrs) {
                attrs |= JSPROP_SHARED;
                rval = JSVAL_VOID;
            }
            ok = OBJ_DEFINE_PROPERTY(cx, parent, ATOM_TO_JSID(fun->atom), rval,
                                     (attrs & JSPROP_GETTER)
                                     ? JS_EXTENSION (JSPropertyOp) obj
                                     : JS_PropertyStub,
                                     (attrs & JSPROP_SETTER)
                                     ? JS_EXTENSION (JSPropertyOp) obj
                                     : JS_PropertyStub,
                                     attrs |
                                     JSPROP_ENUMERATE | JSPROP_PERMANENT |
                                     JSPROP_READONLY,
                                     NULL);

            /* Restore fp->scopeChain now that obj is defined in parent. */
            fp->scopeChain = obj2;
            if (!ok) {
                cx->weakRoots.newborn[GCX_OBJECT] = NULL;
                goto error;
            }

            /*
             * 5. Remove Result(1) from the front of the scope chain [no-op].
             * 6. Return Result(3).
             */
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_NAMEDFUNOBJ)

          BEGIN_CASE(JSOP_CLOSURE)
            /*
             * A top-level function inside eval or ECMA ed. 3 extension: a
             * named function expression statement in a compound statement
             * (not at the top statement level of global code, or at the top
             * level of a function body).
             */
            LOAD_FUNCTION(0);

            /*
             * Clone the function object with the current scope chain as the
             * clone's parent. Do this only if re-parenting; the compiler may
             * have seen the right parent already and created a sufficiently
             * well-scoped function object.
             */
            obj2 = js_GetScopeChain(cx, fp);
            if (!obj2)
                goto error;

            /*
             * ECMA requires that functions defined when entering Eval code to
             * be impermanent.
             */
            attrs = JSPROP_ENUMERATE;
            if (!(fp->flags & JSFRAME_EVAL))
                attrs |= JSPROP_PERMANENT;

            goto do_deffun;

#if JS_HAS_GETTER_SETTER
          BEGIN_CASE(JSOP_GETTER)
          BEGIN_CASE(JSOP_SETTER)
          do_getter_setter:
            op2 = (JSOp) *++regs.pc;
            switch (op2) {
              case JSOP_INDEXBASE:
                atoms += GET_INDEXBASE(regs.pc);
                regs.pc += JSOP_INDEXBASE_LENGTH - 1;
                goto do_getter_setter;
              case JSOP_INDEXBASE1:
              case JSOP_INDEXBASE2:
              case JSOP_INDEXBASE3:
                atoms += (op2 - JSOP_INDEXBASE1 + 1) << 16;
                goto do_getter_setter;

              case JSOP_SETNAME:
              case JSOP_SETPROP:
                LOAD_ATOM(0);
                id = ATOM_TO_JSID(atom);
                rval = FETCH_OPND(-1);
                i = -1;
                goto gs_pop_lval;

              case JSOP_SETELEM:
                rval = FETCH_OPND(-1);
                id = 0;
                i = -2;
              gs_pop_lval:
                FETCH_OBJECT(cx, i - 1, lval, obj);
                break;

              case JSOP_INITPROP:
                JS_ASSERT(regs.sp - fp->spbase >= 2);
                rval = FETCH_OPND(-1);
                i = -1;
                LOAD_ATOM(0);
                id = ATOM_TO_JSID(atom);
                goto gs_get_lval;

              default:
                JS_ASSERT(op2 == JSOP_INITELEM);

                JS_ASSERT(regs.sp - fp->spbase >= 3);
                rval = FETCH_OPND(-1);
                id = 0;
                i = -2;
              gs_get_lval:
                lval = FETCH_OPND(i-1);
                JS_ASSERT(JSVAL_IS_OBJECT(lval));
                obj = JSVAL_TO_OBJECT(lval);
                break;
            }

            /* Ensure that id has a type suitable for use with obj. */
            if (id == 0)
                FETCH_ELEMENT_ID(obj, i, id);

            if (JS_TypeOfValue(cx, rval) != JSTYPE_FUNCTION) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_BAD_GETTER_OR_SETTER,
                                     (op == JSOP_GETTER)
                                     ? js_getter_str
                                     : js_setter_str);
                goto error;
            }

            /*
             * Getters and setters are just like watchpoints from an access
             * control point of view.
             */
            if (!OBJ_CHECK_ACCESS(cx, obj, id, JSACC_WATCH, &rtmp, &attrs))
                goto error;

            if (op == JSOP_GETTER) {
                getter = JS_EXTENSION (JSPropertyOp) JSVAL_TO_OBJECT(rval);
                setter = JS_PropertyStub;
                attrs = JSPROP_GETTER;
            } else {
                getter = JS_PropertyStub;
                setter = JS_EXTENSION (JSPropertyOp) JSVAL_TO_OBJECT(rval);
                attrs = JSPROP_SETTER;
            }
            attrs |= JSPROP_ENUMERATE | JSPROP_SHARED;

            /* Check for a readonly or permanent property of the same name. */
            if (!js_CheckRedeclaration(cx, obj, id, attrs, NULL, NULL))
                goto error;

            if (!OBJ_DEFINE_PROPERTY(cx, obj, id, JSVAL_VOID, getter, setter,
                                     attrs, NULL)) {
                goto error;
            }

            regs.sp += i;
            if (js_CodeSpec[op2].ndefs)
                STORE_OPND(-1, rval);
            len = js_CodeSpec[op2].length;
            DO_NEXT_OP(len);
#endif /* JS_HAS_GETTER_SETTER */

          BEGIN_CASE(JSOP_NEWINIT)
            i = GET_INT8(regs.pc);
            JS_ASSERT(i == JSProto_Array || i == JSProto_Object);
            obj = (i == JSProto_Array)
                  ? js_NewArrayObject(cx, 0, NULL)
                  : js_NewObject(cx, &js_ObjectClass, NULL, NULL, 0);
            if (!obj)
                goto error;
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
            fp->sharpDepth++;
            LOAD_INTERRUPT_HANDLER(cx);
          END_CASE(JSOP_NEWINIT)

          BEGIN_CASE(JSOP_ENDINIT)
            if (--fp->sharpDepth == 0)
                fp->sharpArray = NULL;

            /* Re-set the newborn root to the top of this object tree. */
            JS_ASSERT(regs.sp - fp->spbase >= 1);
            lval = FETCH_OPND(-1);
            JS_ASSERT(JSVAL_IS_OBJECT(lval));
            cx->weakRoots.newborn[GCX_OBJECT] = JSVAL_TO_GCTHING(lval);
          END_CASE(JSOP_ENDINIT)

          BEGIN_CASE(JSOP_INITPROP)
            /* Load the property's initial value into rval. */
            JS_ASSERT(regs.sp - fp->spbase >= 2);
            rval = FETCH_OPND(-1);

            /* Load the object being initialized into lval/obj. */
            lval = FETCH_OPND(-2);
            obj = JSVAL_TO_OBJECT(lval);
            JS_ASSERT(OBJ_IS_NATIVE(obj));
            JS_ASSERT(!OBJ_GET_CLASS(cx, obj)->reserveSlots);
            JS_ASSERT(!(LOCKED_OBJ_GET_CLASS(obj)->flags &
                        JSCLASS_SHARE_ALL_PROPERTIES));

            do {
                JSScope *scope;
                uint32 kshape;
                JSPropertyCache *cache;
                JSPropCacheEntry *entry;

                JS_LOCK_OBJ(cx, obj);
                scope = OBJ_SCOPE(obj);
                JS_ASSERT(!SCOPE_IS_SEALED(scope));
                kshape = scope->shape;
                cache = &JS_PROPERTY_CACHE(cx);
                entry = &cache->table[PROPERTY_CACHE_HASH_PC(regs.pc, kshape)];
                PCMETER(cache->tests++);
                PCMETER(cache->initests++);

                if (entry->kpc == regs.pc && entry->kshape == kshape) {
                    PCMETER(cache->pchits++);
                    PCMETER(cache->inipchits++);

                    JS_ASSERT(PCVAL_IS_SPROP(entry->vword));
                    sprop = PCVAL_TO_SPROP(entry->vword);
                    JS_ASSERT(!(sprop->attrs & JSPROP_READONLY));

                    /*
                     * If this property has a non-stub setter, it must be
                     * __proto__, __parent__, or another "shared prototype"
                     * built-in. Force a miss to save code size here and let
                     * the standard code path take care of business.
                     */
                    if (!SPROP_HAS_STUB_SETTER(sprop))
                        goto do_initprop_miss;

                    if (scope->object != obj) {
                        scope = js_GetMutableScope(cx, obj);
                        if (!scope) {
                            JS_UNLOCK_OBJ(cx, obj);
                            goto error;
                        }
                    }

                    /*
                     * Detect a repeated property name and force a miss to
                     * share the strict warning code and cope with complexity
                     * managed by js_AddScopeProperty.
                     */
                    if (sprop->parent != scope->lastProp)
                        goto do_initprop_miss;

                    /*
                     * Otherwise this entry must be for a direct property of
                     * obj, not a proto-property, and there cannot have been
                     * any deletions of prior properties.
                     */
                    JS_ASSERT(PCVCAP_MAKE(sprop->shape, 0, 0) == entry->vcap);
                    JS_ASSERT(!SCOPE_HAD_MIDDLE_DELETE(scope));
                    JS_ASSERT(!scope->table ||
                              !SCOPE_HAS_PROPERTY(scope, sprop));

                    slot = sprop->slot;
                    JS_ASSERT(slot == scope->map.freeslot);
                    if (slot < STOBJ_NSLOTS(obj)) {
                        ++scope->map.freeslot;
                    } else {
                        if (!js_AllocSlot(cx, obj, &slot)) {
                            JS_UNLOCK_SCOPE(cx, scope);
                            goto error;
                        }
                        JS_ASSERT(slot == sprop->slot);
                    }

                    JS_ASSERT(!scope->lastProp ||
                              scope->shape == scope->lastProp->shape);
                    if (scope->table) {
                        JSScopeProperty *sprop2 =
                            js_AddScopeProperty(cx, scope, sprop->id,
                                                sprop->getter, sprop->setter,
                                                slot, sprop->attrs,
                                                sprop->flags, sprop->shortid);
                        if (!sprop2) {
                            js_FreeSlot(cx, obj, slot);
                            JS_UNLOCK_SCOPE(cx, scope);
                            goto error;
                        }
                        JS_ASSERT(sprop2 == sprop);
                    } else {
                        scope->shape = sprop->shape;
                        ++scope->entryCount;
                        scope->lastProp = sprop;
                    }

                    GC_WRITE_BARRIER(cx, scope,
                                     LOCKED_OBJ_GET_SLOT(obj, slot),
                                     rval);
                    LOCKED_OBJ_SET_SLOT(obj, slot, rval);
                    JS_UNLOCK_SCOPE(cx, scope);
                    break;
                }

              do_initprop_miss:
                PCMETER(cache->inipcmisses++);
                JS_UNLOCK_SCOPE(cx, scope);

                /* Get the immediate property name into id. */
                LOAD_ATOM(0);
                id = ATOM_TO_JSID(atom);

                /* Set the property named by obj[id] to rval. */
                if (!js_CheckRedeclaration(cx, obj, id, JSPROP_INITIALIZER,
                                           NULL, NULL)) {
                    goto error;
                }
                if (!js_SetPropertyHelper(cx, obj, id, &rval, &entry))
                    goto error;
            } while (0);

            /* Common tail for property cache hit and miss cases. */
            regs.sp--;
          END_CASE(JSOP_INITPROP);

          BEGIN_CASE(JSOP_INITELEM)
            /* Pop the element's value into rval. */
            JS_ASSERT(regs.sp - fp->spbase >= 3);
            rval = FETCH_OPND(-1);
            FETCH_ELEMENT_ID(obj, -2, id);

            /* Find the object being initialized at top of stack. */
            lval = FETCH_OPND(-3);
            JS_ASSERT(!JSVAL_IS_PRIMITIVE(lval));
            obj = JSVAL_TO_OBJECT(lval);

            /* Set the property named by obj[id] to rval. */
            if (!js_CheckRedeclaration(cx, obj, id, JSPROP_INITIALIZER, NULL,
                                       NULL)) {
                goto error;
            }
            if (!OBJ_SET_PROPERTY(cx, obj, id, &rval))
                goto error;
            regs.sp -= 2;
          END_CASE(JSOP_INITELEM);

#if JS_HAS_SHARP_VARS
          BEGIN_CASE(JSOP_DEFSHARP)
            obj = fp->sharpArray;
            if (!obj) {
                obj = js_NewArrayObject(cx, 0, NULL);
                if (!obj)
                    goto error;
                fp->sharpArray = obj;
            }
            i = (jsint) GET_UINT16(regs.pc);
            id = INT_TO_JSID(i);
            rval = FETCH_OPND(-1);
            if (JSVAL_IS_PRIMITIVE(rval)) {
                char numBuf[12];
                JS_snprintf(numBuf, sizeof numBuf, "%u", (unsigned) i);
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_BAD_SHARP_DEF, numBuf);
                goto error;
            }
            if (!OBJ_SET_PROPERTY(cx, obj, id, &rval))
                goto error;
          END_CASE(JSOP_DEFSHARP)

          BEGIN_CASE(JSOP_USESHARP)
            i = (jsint) GET_UINT16(regs.pc);
            id = INT_TO_JSID(i);
            obj = fp->sharpArray;
            if (!obj) {
                rval = JSVAL_VOID;
            } else {
                if (!OBJ_GET_PROPERTY(cx, obj, id, &rval))
                    goto error;
            }
            if (!JSVAL_IS_OBJECT(rval)) {
                char numBuf[12];

                JS_snprintf(numBuf, sizeof numBuf, "%u", (unsigned) i);
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_BAD_SHARP_USE, numBuf);
                goto error;
            }
            PUSH_OPND(rval);
          END_CASE(JSOP_USESHARP)
#endif /* JS_HAS_SHARP_VARS */

          /* No-ops for ease of decompilation and jit'ing. */
          EMPTY_CASE(JSOP_TRY)
          EMPTY_CASE(JSOP_FINALLY)

          BEGIN_CASE(JSOP_GOSUB)
            PUSH(JSVAL_FALSE);
            i = PTRDIFF(regs.pc, script->main, jsbytecode) + JSOP_GOSUB_LENGTH;
            len = GET_JUMP_OFFSET(regs.pc);
            PUSH(INT_TO_JSVAL(i));
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_GOSUBX)
            PUSH(JSVAL_FALSE);
            i = PTRDIFF(regs.pc, script->main, jsbytecode) + JSOP_GOSUBX_LENGTH;
            len = GET_JUMPX_OFFSET(regs.pc);
            PUSH(INT_TO_JSVAL(i));
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_RETSUB)
            rval = POP();
            lval = POP();
            JS_ASSERT(JSVAL_IS_BOOLEAN(lval));
            if (JSVAL_TO_BOOLEAN(lval)) {
                /*
                 * Exception was pending during finally, throw it *before* we
                 * adjust pc, because pc indexes into script->trynotes.  This
                 * turns out not to be necessary, but it seems clearer.  And
                 * it points out a FIXME: 350509, due to Igor Bukanov.
                 */
                cx->throwing = JS_TRUE;
                cx->exception = rval;
                goto error;
            }
            JS_ASSERT(JSVAL_IS_INT(rval));
            len = JSVAL_TO_INT(rval);
            regs.pc = script->main;
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_EXCEPTION)
            JS_ASSERT(cx->throwing);
            PUSH(cx->exception);
            cx->throwing = JS_FALSE;
          END_CASE(JSOP_EXCEPTION)

          BEGIN_CASE(JSOP_THROWING)
            JS_ASSERT(!cx->throwing);
            cx->throwing = JS_TRUE;
            cx->exception = POP_OPND();
          END_CASE(JSOP_THROWING)

          BEGIN_CASE(JSOP_THROW)
            JS_ASSERT(!cx->throwing);
            cx->throwing = JS_TRUE;
            cx->exception = POP_OPND();
            /* let the code at error try to catch the exception. */
            goto error;

          BEGIN_CASE(JSOP_SETLOCALPOP)
            /*
             * The stack must have a block with at least one local slot below
             * the exception object.
             */
            JS_ASSERT(regs.sp - fp->spbase >= 2);
            slot = GET_UINT16(regs.pc);
            JS_ASSERT(slot + 1 < script->depth);
            fp->spbase[slot] = POP_OPND();
          END_CASE(JSOP_SETLOCALPOP)

          BEGIN_CASE(JSOP_INSTANCEOF)
            rval = FETCH_OPND(-1);
            if (JSVAL_IS_PRIMITIVE(rval) ||
                !(obj = JSVAL_TO_OBJECT(rval))->map->ops->hasInstance) {
                js_ReportValueError(cx, JSMSG_BAD_INSTANCEOF_RHS,
                                    -1, rval, NULL);
                goto error;
            }
            lval = FETCH_OPND(-2);
            cond = JS_FALSE;
            if (!obj->map->ops->hasInstance(cx, obj, lval, &cond))
                goto error;
            regs.sp--;
            STORE_OPND(-1, BOOLEAN_TO_JSVAL(cond));
          END_CASE(JSOP_INSTANCEOF)

#if JS_HAS_DEBUGGER_KEYWORD
          BEGIN_CASE(JSOP_DEBUGGER)
          {
            JSTrapHandler handler = cx->debugHooks->debuggerHandler;
            if (handler) {
                switch (handler(cx, script, regs.pc, &rval,
                                cx->debugHooks->debuggerHandlerData)) {
                  case JSTRAP_ERROR:
                    goto error;
                  case JSTRAP_CONTINUE:
                    break;
                  case JSTRAP_RETURN:
                    fp->rval = rval;
                    ok = JS_TRUE;
                    goto forced_return;
                  case JSTRAP_THROW:
                    cx->throwing = JS_TRUE;
                    cx->exception = rval;
                    goto error;
                  default:;
                }
                LOAD_INTERRUPT_HANDLER(cx);
            }
          }
          END_CASE(JSOP_DEBUGGER)
#endif /* JS_HAS_DEBUGGER_KEYWORD */

#if JS_HAS_XML_SUPPORT
          BEGIN_CASE(JSOP_DEFXMLNS)
            rval = POP();
            if (!js_SetDefaultXMLNamespace(cx, rval))
                goto error;
          END_CASE(JSOP_DEFXMLNS)

          BEGIN_CASE(JSOP_ANYNAME)
            if (!js_GetAnyName(cx, &rval))
                goto error;
            PUSH_OPND(rval);
          END_CASE(JSOP_ANYNAME)

          BEGIN_CASE(JSOP_QNAMEPART)
            LOAD_ATOM(0);
            PUSH_OPND(ATOM_KEY(atom));
          END_CASE(JSOP_QNAMEPART)

          BEGIN_CASE(JSOP_QNAMECONST)
            LOAD_ATOM(0);
            rval = ATOM_KEY(atom);
            lval = FETCH_OPND(-1);
            obj = js_ConstructXMLQNameObject(cx, lval, rval);
            if (!obj)
                goto error;
            STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_QNAMECONST)

          BEGIN_CASE(JSOP_QNAME)
            rval = FETCH_OPND(-1);
            lval = FETCH_OPND(-2);
            obj = js_ConstructXMLQNameObject(cx, lval, rval);
            if (!obj)
                goto error;
            regs.sp--;
            STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_QNAME)

          BEGIN_CASE(JSOP_TOATTRNAME)
            rval = FETCH_OPND(-1);
            if (!js_ToAttributeName(cx, &rval))
                goto error;
            STORE_OPND(-1, rval);
          END_CASE(JSOP_TOATTRNAME)

          BEGIN_CASE(JSOP_TOATTRVAL)
            rval = FETCH_OPND(-1);
            JS_ASSERT(JSVAL_IS_STRING(rval));
            str = js_EscapeAttributeValue(cx, JSVAL_TO_STRING(rval), JS_FALSE);
            if (!str)
                goto error;
            STORE_OPND(-1, STRING_TO_JSVAL(str));
          END_CASE(JSOP_TOATTRVAL)

          BEGIN_CASE(JSOP_ADDATTRNAME)
          BEGIN_CASE(JSOP_ADDATTRVAL)
            rval = FETCH_OPND(-1);
            lval = FETCH_OPND(-2);
            str = JSVAL_TO_STRING(lval);
            str2 = JSVAL_TO_STRING(rval);
            str = js_AddAttributePart(cx, op == JSOP_ADDATTRNAME, str, str2);
            if (!str)
                goto error;
            regs.sp--;
            STORE_OPND(-1, STRING_TO_JSVAL(str));
          END_CASE(JSOP_ADDATTRNAME)

          BEGIN_CASE(JSOP_BINDXMLNAME)
            lval = FETCH_OPND(-1);
            if (!js_FindXMLProperty(cx, lval, &obj, &id))
                goto error;
            STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
            PUSH_OPND(ID_TO_VALUE(id));
          END_CASE(JSOP_BINDXMLNAME)

          BEGIN_CASE(JSOP_SETXMLNAME)
            obj = JSVAL_TO_OBJECT(FETCH_OPND(-3));
            rval = FETCH_OPND(-1);
            FETCH_ELEMENT_ID(obj, -2, id);
            if (!OBJ_SET_PROPERTY(cx, obj, id, &rval))
                goto error;
            regs.sp -= 2;
            STORE_OPND(-1, rval);
          END_CASE(JSOP_SETXMLNAME)

          BEGIN_CASE(JSOP_CALLXMLNAME)
          BEGIN_CASE(JSOP_XMLNAME)
            lval = FETCH_OPND(-1);
            if (!js_FindXMLProperty(cx, lval, &obj, &id))
                goto error;
            if (!OBJ_GET_PROPERTY(cx, obj, id, &rval))
                goto error;
            STORE_OPND(-1, rval);
            if (op == JSOP_CALLXMLNAME)
                PUSH_OPND(OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_XMLNAME)

          BEGIN_CASE(JSOP_DESCENDANTS)
          BEGIN_CASE(JSOP_DELDESC)
            FETCH_OBJECT(cx, -2, lval, obj);
            rval = FETCH_OPND(-1);
            if (!js_GetXMLDescendants(cx, obj, rval, &rval))
                goto error;

            if (op == JSOP_DELDESC) {
                regs.sp[-1] = rval;          /* set local root */
                if (!js_DeleteXMLListElements(cx, JSVAL_TO_OBJECT(rval)))
                    goto error;
                rval = JSVAL_TRUE;      /* always succeed */
            }

            regs.sp--;
            STORE_OPND(-1, rval);
          END_CASE(JSOP_DESCENDANTS)

          BEGIN_CASE(JSOP_FILTER)
            /*
             * We push the hole value before jumping to [enditer] so we can
             * detect the first iteration and direct js_StepXMLListFilter to
             * initialize filter's state.
             */
            PUSH_OPND(JSVAL_HOLE);
            len = GET_JUMP_OFFSET(regs.pc);
            JS_ASSERT(len > 0);
          END_VARLEN_CASE

          BEGIN_CASE(JSOP_ENDFILTER)
            cond = (regs.sp[-1] != JSVAL_HOLE);
            if (cond) {
                /* Exit the "with" block left from the previous iteration. */
                js_LeaveWith(cx);
            }
            if (!js_StepXMLListFilter(cx, cond))
                goto error;
            if (regs.sp[-1] != JSVAL_NULL) {
                /*
                 * Decrease sp after EnterWith returns as we use sp[-1] there
                 * to root temporaries.
                 */
                JS_ASSERT(VALUE_IS_XML(cx, regs.sp[-1]));
                if (!js_EnterWith(cx, -2))
                    goto error;
                regs.sp--;
                len = GET_JUMP_OFFSET(regs.pc);
                JS_ASSERT(len < 0);
                CHECK_BRANCH(len);
                DO_NEXT_OP(len);
            }
            regs.sp--;
          END_CASE(JSOP_ENDFILTER);

          EMPTY_CASE(JSOP_STARTXML)
          EMPTY_CASE(JSOP_STARTXMLEXPR)

          BEGIN_CASE(JSOP_TOXML)
            rval = FETCH_OPND(-1);
            obj = js_ValueToXMLObject(cx, rval);
            if (!obj)
                goto error;
            STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_TOXML)

          BEGIN_CASE(JSOP_TOXMLLIST)
            rval = FETCH_OPND(-1);
            obj = js_ValueToXMLListObject(cx, rval);
            if (!obj)
                goto error;
            STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_TOXMLLIST)

          BEGIN_CASE(JSOP_XMLTAGEXPR)
            rval = FETCH_OPND(-1);
            str = js_ValueToString(cx, rval);
            if (!str)
                goto error;
            STORE_OPND(-1, STRING_TO_JSVAL(str));
          END_CASE(JSOP_XMLTAGEXPR)

          BEGIN_CASE(JSOP_XMLELTEXPR)
            rval = FETCH_OPND(-1);
            if (VALUE_IS_XML(cx, rval)) {
                str = js_ValueToXMLString(cx, rval);
            } else {
                str = js_ValueToString(cx, rval);
                if (str)
                    str = js_EscapeElementValue(cx, str);
            }
            if (!str)
                goto error;
            STORE_OPND(-1, STRING_TO_JSVAL(str));
          END_CASE(JSOP_XMLELTEXPR)

          BEGIN_CASE(JSOP_XMLOBJECT)
            LOAD_OBJECT(0);
            obj = js_CloneXMLObject(cx, obj);
            if (!obj)
                goto error;
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_XMLOBJECT)

          BEGIN_CASE(JSOP_XMLCDATA)
            LOAD_ATOM(0);
            str = ATOM_TO_STRING(atom);
            obj = js_NewXMLSpecialObject(cx, JSXML_CLASS_TEXT, NULL, str);
            if (!obj)
                goto error;
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_XMLCDATA)

          BEGIN_CASE(JSOP_XMLCOMMENT)
            LOAD_ATOM(0);
            str = ATOM_TO_STRING(atom);
            obj = js_NewXMLSpecialObject(cx, JSXML_CLASS_COMMENT, NULL, str);
            if (!obj)
                goto error;
            PUSH_OPND(OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_XMLCOMMENT)

          BEGIN_CASE(JSOP_XMLPI)
            LOAD_ATOM(0);
            str = ATOM_TO_STRING(atom);
            rval = FETCH_OPND(-1);
            str2 = JSVAL_TO_STRING(rval);
            obj = js_NewXMLSpecialObject(cx,
                                         JSXML_CLASS_PROCESSING_INSTRUCTION,
                                         str, str2);
            if (!obj)
                goto error;
            STORE_OPND(-1, OBJECT_TO_JSVAL(obj));
          END_CASE(JSOP_XMLPI)

          BEGIN_CASE(JSOP_GETFUNNS)
            if (!js_GetFunctionNamespace(cx, &rval))
                goto error;
            PUSH_OPND(rval);
          END_CASE(JSOP_GETFUNNS)
#endif /* JS_HAS_XML_SUPPORT */

          BEGIN_CASE(JSOP_ENTERBLOCK)
            LOAD_OBJECT(0);
            JS_ASSERT(!OBJ_IS_CLONED_BLOCK(obj));
            JS_ASSERT(fp->spbase + OBJ_BLOCK_DEPTH(cx, obj) == regs.sp);
            vp = regs.sp + OBJ_BLOCK_COUNT(cx, obj);
            JS_ASSERT(regs.sp < vp);
            JS_ASSERT(vp <= fp->spbase + script->depth);
            while (regs.sp < vp) {
                STORE_OPND(0, JSVAL_VOID);
                regs.sp++;
            }

            /*
             * If this frame had to reflect the compile-time block chain into
             * the runtime scope chain, we can't optimize block scopes out of
             * runtime any longer, because an outer block that parents obj has
             * been cloned onto the scope chain.  To avoid re-cloning such a
             * parent and accumulating redundant clones via js_GetScopeChain,
             * we must clone each block eagerly on entry, and push it on the
             * scope chain, until this frame pops.
             */
            if (fp->flags & JSFRAME_POP_BLOCKS) {
                JS_ASSERT(!fp->blockChain);
                obj = js_CloneBlockObject(cx, obj, fp->scopeChain, fp);
                if (!obj)
                    goto error;
                fp->scopeChain = obj;
            } else {
                JS_ASSERT(!fp->blockChain ||
                          OBJ_GET_PARENT(cx, obj) == fp->blockChain);
                fp->blockChain = obj;
            }
          END_CASE(JSOP_ENTERBLOCK)

          BEGIN_CASE(JSOP_LEAVEBLOCKEXPR)
          BEGIN_CASE(JSOP_LEAVEBLOCK)
          {
#ifdef DEBUG
            jsval *blocksp = fp->spbase + OBJ_BLOCK_DEPTH(cx,
                                                          fp->blockChain
                                                          ? fp->blockChain
                                                          : fp->scopeChain);

            JS_ASSERT((size_t) (blocksp - fp->spbase) <= script->depth);
#endif
            if (fp->blockChain) {
                JS_ASSERT(OBJ_GET_CLASS(cx, fp->blockChain) == &js_BlockClass);
                fp->blockChain = OBJ_GET_PARENT(cx, fp->blockChain);
            } else {
                /*
                 * This block was cloned into fp->scopeChain, so clear its
                 * private data and sync its locals to their property slots.
                 */
                if (!js_PutBlockObject(cx, JS_TRUE))
                    goto error;
            }

            /*
             * We will move the result of the expression to the new topmost
             * stack slot.
             */
            if (op == JSOP_LEAVEBLOCKEXPR)
                rval = FETCH_OPND(-1);
            regs.sp -= GET_UINT16(regs.pc);
            if (op == JSOP_LEAVEBLOCKEXPR) {
                JS_ASSERT(blocksp == regs.sp - 1);
                STORE_OPND(-1, rval);
            } else {
                JS_ASSERT(blocksp == regs.sp);
            }
          }
          END_CASE(JSOP_LEAVEBLOCK)

          BEGIN_CASE(JSOP_GETLOCAL)
          BEGIN_CASE(JSOP_CALLLOCAL)
            slot = GET_UINT16(regs.pc);
            JS_ASSERT(slot < script->depth);
            PUSH_OPND(fp->spbase[slot]);
            if (op == JSOP_CALLLOCAL)
                PUSH_OPND(JSVAL_NULL);
          END_CASE(JSOP_GETLOCAL)

          BEGIN_CASE(JSOP_SETLOCAL)
            slot = GET_UINT16(regs.pc);
            JS_ASSERT(slot < script->depth);
            vp = &fp->spbase[slot];
            GC_POKE(cx, *vp);
            *vp = FETCH_OPND(-1);
          END_CASE(JSOP_SETLOCAL)

          BEGIN_CASE(JSOP_ENDITER)
            /*
             * Decrease the stack pointer even when !ok, see comments in the
             * exception capturing code for details.
             */
            ok = js_CloseIterator(cx, regs.sp[-1]);
            regs.sp--;
            if (!ok)
                goto error;
          END_CASE(JSOP_ENDITER)

#if JS_HAS_GENERATORS
          BEGIN_CASE(JSOP_GENERATOR)
            ASSERT_NOT_THROWING(cx);
            regs.pc += JSOP_GENERATOR_LENGTH;
            obj = js_NewGenerator(cx, fp);
            if (!obj)
                goto error;
            JS_ASSERT(!fp->callobj && !fp->argsobj);
            fp->rval = OBJECT_TO_JSVAL(obj);
            ok = JS_TRUE;
            if (inlineCallCount != 0)
                goto inline_return;
            goto exit;

          BEGIN_CASE(JSOP_YIELD)
            ASSERT_NOT_THROWING(cx);
            if (FRAME_TO_GENERATOR(fp)->state == JSGEN_CLOSING) {
                js_ReportValueError(cx, JSMSG_BAD_GENERATOR_YIELD,
                                    JSDVG_SEARCH_STACK, fp->argv[-2], NULL);
                goto error;
            }
            fp->rval = FETCH_OPND(-1);
            fp->flags |= JSFRAME_YIELDING;
            regs.pc += JSOP_YIELD_LENGTH;
            ok = JS_TRUE;
            goto exit;

          BEGIN_CASE(JSOP_ARRAYPUSH)
            slot = GET_UINT16(regs.pc);
            JS_ASSERT(slot < script->depth);
            lval = fp->spbase[slot];
            obj  = JSVAL_TO_OBJECT(lval);
            JS_ASSERT(OBJ_GET_CLASS(cx, obj) == &js_ArrayClass);
            rval = FETCH_OPND(-1);

            /*
             * We know that the array is created with only a 'length' private
             * data slot at JSSLOT_ARRAY_LENGTH, and that previous iterations
             * of the comprehension have added the only properties directly in
             * the array object.
             */
            i = obj->fslots[JSSLOT_ARRAY_LENGTH];
            if (i == ARRAY_INIT_LIMIT) {
                JS_ReportErrorNumberUC(cx, js_GetErrorMessage, NULL,
                                       JSMSG_ARRAY_INIT_TOO_BIG);
                goto error;
            }
            id = INT_TO_JSID(i);
            if (!OBJ_SET_PROPERTY(cx, obj, id, &rval))
                goto error;
            regs.sp--;
          END_CASE(JSOP_ARRAYPUSH)
#endif /* JS_HAS_GENERATORS */

#if JS_THREADED_INTERP
          L_JSOP_BACKPATCH:
          L_JSOP_BACKPATCH_POP:

# if !JS_HAS_GENERATORS
          L_JSOP_GENERATOR:
          L_JSOP_YIELD:
          L_JSOP_ARRAYPUSH:
# endif

# if !JS_HAS_DESTRUCTURING
          L_JSOP_FOREACHKEYVAL:
          L_JSOP_ENUMCONSTELEM:
# endif

# if !JS_HAS_XML_SUPPORT
          L_JSOP_CALLXMLNAME:
          L_JSOP_STARTXMLEXPR:
          L_JSOP_STARTXML:
          L_JSOP_DELDESC:
          L_JSOP_GETFUNNS:
          L_JSOP_XMLPI:
          L_JSOP_XMLCOMMENT:
          L_JSOP_XMLCDATA:
          L_JSOP_XMLOBJECT:
          L_JSOP_XMLELTEXPR:
          L_JSOP_XMLTAGEXPR:
          L_JSOP_TOXMLLIST:
          L_JSOP_TOXML:
          L_JSOP_ENDFILTER:
          L_JSOP_FILTER:
          L_JSOP_DESCENDANTS:
          L_JSOP_XMLNAME:
          L_JSOP_SETXMLNAME:
          L_JSOP_BINDXMLNAME:
          L_JSOP_ADDATTRVAL:
          L_JSOP_ADDATTRNAME:
          L_JSOP_TOATTRVAL:
          L_JSOP_TOATTRNAME:
          L_JSOP_QNAME:
          L_JSOP_QNAMECONST:
          L_JSOP_QNAMEPART:
          L_JSOP_ANYNAME:
          L_JSOP_DEFXMLNS:
# endif

          L_JSOP_UNUSED117:

#else /* !JS_THREADED_INTERP */
          default:
#endif
          {
            char numBuf[12];
            JS_snprintf(numBuf, sizeof numBuf, "%d", op);
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_BAD_BYTECODE, numBuf);
            goto error;
          }

#if !JS_THREADED_INTERP

        } /* switch (op) */

    advance_pc:
        regs.pc += len;

#ifdef DEBUG
        if (tracefp) {
            intN ndefs, n;
            jsval *siter;

            /*
             * op may be invalid here when a catch or finally handler jumps to
             * advance_pc.
             */
            op = (JSOp) regs.pc[-len];
            ndefs = js_CodeSpec[op].ndefs;
            if (ndefs) {
                if (op == JSOP_FORELEM && regs.sp[-1] == JSVAL_FALSE)
                    --ndefs;
                for (n = -ndefs; n < 0; n++) {
                    char *bytes = js_DecompileValueGenerator(cx, n, regs.sp[n],
                                                             NULL);
                    if (bytes) {
                        fprintf(tracefp, "%s %s",
                                (n == -ndefs) ? "  output:" : ",",
                                bytes);
                        JS_free(cx, bytes);
                    }
                }
                fprintf(tracefp, " @ %d\n", regs.sp - fp->spbase);
            }
            fprintf(tracefp, "  stack: ");
            for (siter = fp->spbase; siter < regs.sp; siter++) {
                str = js_ValueToString(cx, *siter);
                if (!str)
                    fputs("<null>", tracefp);
                else
                    js_FileEscapedString(tracefp, str, 0);
                fputc(' ', tracefp);
            }
            fputc('\n', tracefp);
        }
#endif /* DEBUG */
    }
#endif /* !JS_THREADED_INTERP */

  error:
    JS_ASSERT((size_t)(regs.pc - script->code) < script->length);
    if (!cx->throwing) {
        /* This is an error, not a catchable exception, quit the frame ASAP. */
        ok = JS_FALSE;
    } else {
        JSTrapHandler handler;
        JSTryNote *tn, *tnlimit;
        uint32 offset;

        /* Call debugger throw hook if set. */
        handler = cx->debugHooks->throwHook;
        if (handler) {
            switch (handler(cx, script, regs.pc, &rval,
                            cx->debugHooks->throwHookData)) {
              case JSTRAP_ERROR:
                cx->throwing = JS_FALSE;
                goto error;
              case JSTRAP_RETURN:
                cx->throwing = JS_FALSE;
                fp->rval = rval;
                ok = JS_TRUE;
                goto forced_return;
              case JSTRAP_THROW:
                cx->exception = rval;
              case JSTRAP_CONTINUE:
              default:;
            }
            LOAD_INTERRUPT_HANDLER(cx);
        }

        /*
         * Look for a try block in script that can catch this exception.
         */
        if (script->trynotesOffset == 0)
            goto no_catch;

        offset = (uint32)(regs.pc - script->main);
        tn = JS_SCRIPT_TRYNOTES(script)->vector;
        tnlimit = tn + JS_SCRIPT_TRYNOTES(script)->length;
        do {
            if (offset - tn->start >= tn->length)
                continue;

            /*
             * We have a note that covers the exception pc but we must check
             * whether the interpreter has already executed the corresponding
             * handler. This is possible when the executed bytecode
             * implements break or return from inside a for-in loop.
             *
             * In this case the emitter generates additional [enditer] and
             * [gosub] opcodes to close all outstanding iterators and execute
             * the finally blocks. If such an [enditer] throws an exception,
             * its pc can still be inside several nested for-in loops and
             * try-finally statements even if we have already closed the
             * corresponding iterators and invoked the finally blocks.
             *
             * To address this, we make [enditer] always decrease the stack
             * even when its implementation throws an exception. Thus already
             * executed [enditer] and [gosub] opcodes will have try notes
             * with the stack depth exceeding the current one and this
             * condition is what we use to filter them out.
             */
            if (tn->stackDepth > regs.sp - fp->spbase)
                continue;

            /*
             * Set pc to the first bytecode after the the try note to point
             * to the beginning of catch or finally or to [enditer] closing
             * the for-in loop.
             */
            regs.pc = (script)->main + tn->start + tn->length;

            ok = js_UnwindScope(cx, fp, tn->stackDepth, JS_TRUE);
            JS_ASSERT(fp->regs->sp == fp->spbase + tn->stackDepth);
            if (!ok) {
                /*
                 * Restart the handler search with updated pc and stack depth
                 * to properly notify the debugger.
                 */
                goto error;
            }

            switch (tn->kind) {
              case JSTN_CATCH:
                JS_ASSERT(*regs.pc == JSOP_ENTERBLOCK);

#if JS_HAS_GENERATORS
                /* Catch cannot intercept the closing of a generator. */
                if (JS_UNLIKELY(cx->exception == JSVAL_ARETURN))
                    break;
#endif

                /*
                 * Don't clear cx->throwing to save cx->exception from GC
                 * until it is pushed to the stack via [exception] in the
                 * catch block.
                 */
                len = 0;
                DO_NEXT_OP(len);

              case JSTN_FINALLY:
                /*
                 * Push (true, exception) pair for finally to indicate that
                 * [retsub] should rethrow the exception.
                 */
                PUSH(JSVAL_TRUE);
                PUSH(cx->exception);
                cx->throwing = JS_FALSE;
                len = 0;
                DO_NEXT_OP(len);

              case JSTN_ITER:
                /*
                 * This is similar to JSOP_ENDITER in the interpreter loop
                 * except the code now uses a reserved stack slot to save and
                 * restore the exception.
                 */
                JS_ASSERT(*regs.pc == JSOP_ENDITER);
                PUSH(cx->exception);
                cx->throwing = JS_FALSE;
                ok = js_CloseIterator(cx, regs.sp[-2]);
                regs.sp -= 2;
                if (!ok)
                    goto error;
                cx->throwing = JS_TRUE;
                cx->exception = regs.sp[1];
            }
        } while (++tn != tnlimit);

      no_catch:
        /*
         * Propagate the exception or error to the caller unless the exception
         * is an asynchronous return from a generator.
         */
        ok = JS_FALSE;
#if JS_HAS_GENERATORS
        if (JS_UNLIKELY(cx->throwing && cx->exception == JSVAL_ARETURN)) {
            cx->throwing = JS_FALSE;
            ok = JS_TRUE;
            fp->rval = JSVAL_VOID;
        }
#endif
    }

  forced_return:
    /*
     * Unwind the scope making sure that ok stays false even when UnwindScope
     * returns true.
     *
     * When a trap handler returns JSTRAP_RETURN, we jump here with ok set to
     * true bypassing any finally blocks.
     */
    ok &= js_UnwindScope(cx, fp, 0, ok || cx->throwing);
    JS_ASSERT(regs.sp == fp->spbase);

    if (inlineCallCount)
        goto inline_return;

  exit:
    /*
     * At this point we are inevitably leaving an interpreted function or a
     * top-level script, and returning to one of:
     * (a) an "out of line" call made through js_Invoke;
     * (b) a js_Execute activation;
     * (c) a generator (SendToGenerator, jsiter.c).
     *
     * We must not be in an inline frame. The check above ensures that for the
     * error case and for a normal return, the code jumps directly to parent's
     * frame pc.
     */
    JS_ASSERT(inlineCallCount == 0);

    JS_ASSERT(fp->spbase);
    JS_ASSERT(fp->regs == &regs);
    if (JS_LIKELY(mark != NULL)) {
        JS_ASSERT(!fp->blockChain);
        JS_ASSERT(!js_IsActiveWithOrBlock(cx, fp->scopeChain, 0));
        JS_ASSERT(!(fp->flags & JSFRAME_GENERATOR));
        fp->spbase = NULL;
        fp->regs = NULL;
        js_FreeRawStack(cx, mark);
    } else {
        JS_ASSERT(fp->flags & JSFRAME_GENERATOR);
        if (fp->flags & JSFRAME_YIELDING) {
            JSGenerator *gen;

            gen = FRAME_TO_GENERATOR(fp);
            gen->savedRegs = regs;
            gen->frame.regs = &gen->savedRegs;
            JS_PROPERTY_CACHE(cx).disabled -= js_CountWithBlocks(cx, fp);
            JS_ASSERT(JS_PROPERTY_CACHE(cx).disabled >= 0);
        } else {
            fp->regs = NULL;
            fp->spbase = NULL;
        }
    }

  exit2:
    JS_ASSERT(JS_PROPERTY_CACHE(cx).disabled == fp->pcDisabledSave);
    if (cx->version == currentVersion && currentVersion != originalVersion)
        js_SetVersion(cx, originalVersion);
    cx->interpLevel--;
    return ok;

  atom_not_defined:
    {
        const char *printable;

        printable = js_AtomToPrintableString(cx, atom);
        if (printable)
            js_ReportIsNotDefined(cx, printable);
        goto error;
    }
}

#endif /* !defined js_invoke_c__ */
