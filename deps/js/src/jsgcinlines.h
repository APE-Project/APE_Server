/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
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
 * The Original Code is SpiderMonkey code.
 *
 * The Initial Developer of the Original Code is
 * Mozilla Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2010
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
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

#ifndef jsgcinlines_h___
#define jsgcinlines_h___

#include "jsgc.h"
#include "jscntxt.h"
#include "jscompartment.h"

#include "jslock.h"
#include "jstl.h"

#ifdef JS_GCMETER
# define METER(x)               ((void) (x))
# define METER_IF(condition, x) ((void) ((condition) && (x)))
#else
# define METER(x)               ((void) 0)
# define METER_IF(condition, x) ((void) 0)
#endif

namespace js {
namespace gc {

/* Capacity for slotsToThingKind */
const size_t SLOTS_TO_THING_KIND_LIMIT = 17;

/* Get the best kind to use when making an object with the given slot count. */
static inline FinalizeKind
GetGCObjectKind(size_t numSlots)
{
    extern FinalizeKind slotsToThingKind[];

    if (numSlots >= SLOTS_TO_THING_KIND_LIMIT)
        return FINALIZE_OBJECT0;
    return slotsToThingKind[numSlots];
}

/* Get the number of fixed slots and initial capacity associated with a kind. */
static inline size_t
GetGCKindSlots(FinalizeKind thingKind)
{
    /* Using a switch in hopes that thingKind will usually be a compile-time constant. */
    switch (thingKind) {
      case FINALIZE_OBJECT0:
        return 0;
      case FINALIZE_OBJECT2:
        return 2;
      case FINALIZE_OBJECT4:
        return 4;
      case FINALIZE_OBJECT8:
        return 8;
      case FINALIZE_OBJECT12:
        return 12;
      case FINALIZE_OBJECT16:
        return 16;
      default:
        JS_NOT_REACHED("Bad object finalize kind");
        return 0;
    }
}

} /* namespace gc */
} /* namespace js */

/*
 * Allocates a new GC thing. After a successful allocation the caller must
 * fully initialize the thing before calling any function that can potentially
 * trigger GC. This will ensure that GC tracing never sees junk values stored
 * in the partially initialized thing.
 */

template <typename T>
JS_ALWAYS_INLINE T *
NewFinalizableGCThing(JSContext *cx, unsigned thingKind)
{
    JS_ASSERT(thingKind < js::gc::FINALIZE_LIMIT);
#ifdef JS_THREADSAFE
    JS_ASSERT_IF((cx->compartment == cx->runtime->atomsCompartment),
                 (thingKind == js::gc::FINALIZE_STRING) ||
                 (thingKind == js::gc::FINALIZE_SHORT_STRING));
#endif

    METER(cx->compartment->compartmentStats[thingKind].alloc++);
    do {
        js::gc::FreeCell *cell = cx->compartment->freeLists.getNext(thingKind);
        if (cell) {
            CheckGCFreeListLink(cell);
            return (T *)cell;
        }
        if (!RefillFinalizableFreeList(cx, thingKind))
            return NULL;
    } while (true);
}

#undef METER
#undef METER_IF

inline JSObject *
js_NewGCObject(JSContext *cx, js::gc::FinalizeKind kind)
{
    JS_ASSERT(kind >= js::gc::FINALIZE_OBJECT0 && kind <= js::gc::FINALIZE_OBJECT_LAST);
    JSObject *obj = NewFinalizableGCThing<JSObject>(cx, kind);
    if (obj)
        obj->capacity = js::gc::GetGCKindSlots(kind);
    return obj;
}

inline JSString *
js_NewGCString(JSContext *cx)
{
    return NewFinalizableGCThing<JSString>(cx, js::gc::FINALIZE_STRING);    
}

inline JSShortString *
js_NewGCShortString(JSContext *cx)
{
    return NewFinalizableGCThing<JSShortString>(cx, js::gc::FINALIZE_SHORT_STRING);
}

inline JSExternalString *
js_NewGCExternalString(JSContext *cx, uintN type)
{
    JS_ASSERT(type < JSExternalString::TYPE_LIMIT);
    JSExternalString *str = NewFinalizableGCThing<JSExternalString>(cx, js::gc::FINALIZE_EXTERNAL_STRING);
    return str;
}

inline JSFunction*
js_NewGCFunction(JSContext *cx)
{
    JSFunction *fun = NewFinalizableGCThing<JSFunction>(cx, js::gc::FINALIZE_FUNCTION);
    if (fun)
        fun->capacity = JSObject::FUN_CLASS_RESERVED_SLOTS;
    return fun;
}

#if JS_HAS_XML_SUPPORT
inline JSXML *
js_NewGCXML(JSContext *cx)
{
    return NewFinalizableGCThing<JSXML>(cx, js::gc::FINALIZE_XML);
}
#endif

namespace js {
namespace gc {

static JS_ALWAYS_INLINE void
TypedMarker(JSTracer *trc, JSXML *thing);

static JS_ALWAYS_INLINE void
TypedMarker(JSTracer *trc, JSObject *thing);

static JS_ALWAYS_INLINE void
TypedMarker(JSTracer *trc, JSFunction *thing);

static JS_ALWAYS_INLINE void
TypedMarker(JSTracer *trc, JSShortString *thing);

static JS_ALWAYS_INLINE void
TypedMarker(JSTracer *trc, JSString *thing);

template<typename T>
static JS_ALWAYS_INLINE void
Mark(JSTracer *trc, T *thing)
{
    JS_ASSERT(thing);
    JS_ASSERT(JS_IS_VALID_TRACE_KIND(GetGCThingTraceKind(thing)));
    JS_ASSERT(trc->debugPrinter || trc->debugPrintArg);

    /* Per-Compartment GC only with GCMarker and no custom JSTracer */
    JS_ASSERT_IF(trc->context->runtime->gcCurrentCompartment, IS_GC_MARKING_TRACER(trc));

    JSRuntime *rt = trc->context->runtime;
    /* Don't mark things outside a compartment if we are in a per-compartment GC */
    if (rt->gcCurrentCompartment && thing->asCell()->compartment() != rt->gcCurrentCompartment)
        goto out;

    if (!IS_GC_MARKING_TRACER(trc)) {
        uint32 kind = GetGCThingTraceKind(thing);
        trc->callback(trc, thing, kind);
        goto out;
    }

    TypedMarker(trc, thing);

  out:
#ifdef DEBUG
    trc->debugPrinter = NULL;
    trc->debugPrintArg = NULL;
#endif
    return;     /* to avoid out: right_curl when DEBUG is not defined */
}

static inline void
MarkString(JSTracer *trc, JSString *str)
{
    JS_ASSERT(str);
    if (JSString::isStatic(str))
        return;
    JS_ASSERT(GetArena<JSString>((Cell *)str)->assureThingIsAligned((JSString *)str));
    Mark(trc, str);
}

static inline void
MarkString(JSTracer *trc, JSString *str, const char *name)
{
    JS_ASSERT(str);
    JS_SET_TRACING_NAME(trc, name);
    MarkString(trc, str);
}

static inline void
MarkObject(JSTracer *trc, JSObject &obj, const char *name)
{
    JS_ASSERT(trc);
    JS_ASSERT(&obj);
    JS_SET_TRACING_NAME(trc, name);
    JS_ASSERT(GetArena<JSObject>((Cell *)&obj)->assureThingIsAligned(&obj) ||
              GetArena<JSObject_Slots2>((Cell *)&obj)->assureThingIsAligned(&obj) ||
              GetArena<JSObject_Slots4>((Cell *)&obj)->assureThingIsAligned(&obj) ||
              GetArena<JSObject_Slots8>((Cell *)&obj)->assureThingIsAligned(&obj) ||
              GetArena<JSObject_Slots12>((Cell *)&obj)->assureThingIsAligned(&obj) ||
              GetArena<JSObject_Slots16>((Cell *)&obj)->assureThingIsAligned(&obj) ||
              GetArena<JSFunction>((Cell *)&obj)->assureThingIsAligned(&obj));
    Mark(trc, &obj);
}

static inline void
MarkChildren(JSTracer *trc, JSObject *obj)
{
    /* If obj has no map, it must be a newborn. */
    if (!obj->map)
        return;

    /* Trace universal (ops-independent) members. */
    if (JSObject *proto = obj->getProto())
        MarkObject(trc, *proto, "proto");
    if (JSObject *parent = obj->getParent())
        MarkObject(trc, *parent, "parent");

    if (obj->emptyShapes) {
        int count = FINALIZE_OBJECT_LAST - FINALIZE_OBJECT0 + 1;
        for (int i = 0; i < count; i++) {
            if (obj->emptyShapes[i])
                obj->emptyShapes[i]->trace(trc);
        }
    }

    /* Delegate to ops or the native marking op. */
    TraceOp op = obj->getOps()->trace;
    (op ? op : js_TraceObject)(trc, obj);
}

static inline void
MarkChildren(JSTracer *trc, JSString *str)
{
    if (str->isDependent())
        MarkString(trc, str->dependentBase(), "base");
    else if (str->isRope()) {
        MarkString(trc, str->ropeLeft(), "left child");
        MarkString(trc, str->ropeRight(), "right child");
    }
}

#ifdef JS_HAS_XML_SUPPORT
static inline void
MarkChildren(JSTracer *trc, JSXML *xml)
{
    js_TraceXML(trc, xml);
}
#endif

static inline bool
RecursionTooDeep(GCMarker *gcmarker) {
#ifdef JS_GC_ASSUME_LOW_C_STACK
    return true;
#else
    int stackDummy;
    return !JS_CHECK_STACK_SIZE(gcmarker->stackLimit, &stackDummy);
#endif
}

static JS_ALWAYS_INLINE void
TypedMarker(JSTracer *trc, JSXML *thing)
{
    if (!reinterpret_cast<Cell *>(thing)->markIfUnmarked(reinterpret_cast<GCMarker *>(trc)->getMarkColor()))
        return;
    GCMarker *gcmarker = static_cast<GCMarker *>(trc);
    if (RecursionTooDeep(gcmarker)) {
        gcmarker->delayMarkingChildren(thing);
    } else {
        MarkChildren(trc, thing);
    }
}

static JS_ALWAYS_INLINE void
TypedMarker(JSTracer *trc, JSObject *thing)
{
    JS_ASSERT(thing);
    JS_ASSERT(JSTRACE_OBJECT == GetFinalizableTraceKind(thing->asCell()->arena()->header()->thingKind));

    GCMarker *gcmarker = static_cast<GCMarker *>(trc);
    if (!thing->markIfUnmarked(gcmarker->getMarkColor()))
        return;
    
    if (RecursionTooDeep(gcmarker)) {
        gcmarker->delayMarkingChildren(thing);
    } else {
        MarkChildren(trc, thing);
    }
}

static JS_ALWAYS_INLINE void
TypedMarker(JSTracer *trc, JSFunction *thing)
{
    JS_ASSERT(thing);
    JS_ASSERT(JSTRACE_OBJECT == GetFinalizableTraceKind(thing->asCell()->arena()->header()->thingKind));

    GCMarker *gcmarker = static_cast<GCMarker *>(trc);
    if (!thing->markIfUnmarked(gcmarker->getMarkColor()))
        return;

    if (RecursionTooDeep(gcmarker)) {
        gcmarker->delayMarkingChildren(thing);
    } else {
        MarkChildren(trc, static_cast<JSObject *>(thing));
    }
}

static JS_ALWAYS_INLINE void
TypedMarker(JSTracer *trc, JSShortString *thing)
{
    /*
     * A short string cannot refer to other GC things so we don't have
     * anything to mark if the string was unmarked and ignore the
     * markIfUnmarked result.
     */
    (void) thing->asCell()->markIfUnmarked();
}

}  /* namespace gc */

namespace detail {

static JS_ALWAYS_INLINE JSString *
Tag(JSString *str)
{
    JS_ASSERT(!(size_t(str) & 1));
    return (JSString *)(size_t(str) | 1);
}

static JS_ALWAYS_INLINE bool
Tagged(JSString *str)
{
    return (size_t(str) & 1) != 0;
}

static JS_ALWAYS_INLINE JSString *
Untag(JSString *str)
{
    JS_ASSERT((size_t(str) & 1) == 1);
    return (JSString *)(size_t(str) & ~size_t(1));
}

static JS_ALWAYS_INLINE void
NonRopeTypedMarker(JSRuntime *rt, JSString *str)
{
    /* N.B. The base of a dependent string is not necessarily flat. */
    JS_ASSERT(!str->isRope());

    if (rt->gcCurrentCompartment) {
        for (;;) {
            if (JSString::isStatic(str))
                break;

            /* 
             * If we perform single-compartment GC don't mark Strings outside the current compartment.
             * Dependent Strings are not shared between compartments and they can't be in the atomsCompartment.
             */
            if (str->asCell()->compartment() != rt->gcCurrentCompartment) {
                JS_ASSERT(str->asCell()->compartment() == rt->atomsCompartment);
                break;
            }
            if (!str->asCell()->markIfUnmarked())
                break;
            if (!str->isDependent())
                break;
            str = str->dependentBase();
        }
    } else {
        while (!JSString::isStatic(str) &&
               str->asCell()->markIfUnmarked() &&
               str->isDependent()) {
            str = str->dependentBase();
        }
    }
}

}  /* namespace detail */

namespace gc {

static JS_ALWAYS_INLINE void
TypedMarker(JSTracer *trc, JSString *str)
{
    using namespace detail;
    JSRuntime *rt = trc->context->runtime;
    JS_ASSERT(!JSString::isStatic(str));
#ifdef DEBUG
    JSCompartment *strComp = str->asCell()->compartment();
#endif
    if (!str->isRope()) {
        NonRopeTypedMarker(rt, str);
        return;
    }

    /*
     * This function must not fail, so a simple stack-based traversal must not
     * be used (since it may oom if the stack grows large). Instead, strings
     * are temporarily mutated to embed parent pointers as they are traversed.
     * This algorithm is homomorphic to JSString::flatten.
     */
    JSString *parent = NULL;
    first_visit_node: {
        JS_ASSERT(strComp == str->asCell()->compartment() || str->asCell()->compartment() == rt->atomsCompartment);
        JS_ASSERT(!JSString::isStatic(str));
        if (!str->asCell()->markIfUnmarked())
            goto finish_node;
        JSString *left = str->ropeLeft();
        if (left->isRope()) {
            JS_ASSERT(!Tagged(str->u.left) && !Tagged(str->s.right));
            str->u.left = Tag(parent);
            parent = str;
            str = left;
            goto first_visit_node;
        }
        JS_ASSERT_IF(!JSString::isStatic(left), 
                     strComp == left->asCell()->compartment()
                     || left->asCell()->compartment() == rt->atomsCompartment);
        NonRopeTypedMarker(rt, left);
    }
    visit_right_child: {
        JSString *right = str->ropeRight();
        if (right->isRope()) {
            JS_ASSERT(!Tagged(str->u.left) && !Tagged(str->s.right));
            str->s.right = Tag(parent);
            parent = str;
            str = right;
            goto first_visit_node;
        }
        JS_ASSERT_IF(!JSString::isStatic(right), 
                     strComp == right->asCell()->compartment()
                     || right->asCell()->compartment() == rt->atomsCompartment);
        NonRopeTypedMarker(rt, right);
    }
    finish_node: {
        if (!parent)
            return;
        if (Tagged(parent->u.left)) {
            JS_ASSERT(!Tagged(parent->s.right));
            JSString *nextParent = Untag(parent->u.left);
            parent->u.left = str;
            str = parent;
            parent = nextParent;
            goto visit_right_child;
        }
        JS_ASSERT(Tagged(parent->s.right));
        JSString *nextParent = Untag(parent->s.right);
        parent->s.right = str;
        str = parent;
        parent = nextParent;
        goto finish_node;
    }
}

static inline void
MarkAtomRange(JSTracer *trc, size_t len, JSAtom **vec, const char *name)
{
    for (uint32 i = 0; i < len; i++) {
        if (JSAtom *atom = vec[i]) {
            JS_SET_TRACING_INDEX(trc, name, i);
            JSString *str = ATOM_TO_STRING(atom);
            if (!JSString::isStatic(str))
                Mark(trc, str);
        }
    }
}

static inline void
MarkObjectRange(JSTracer *trc, size_t len, JSObject **vec, const char *name)
{
    for (uint32 i = 0; i < len; i++) {
        if (JSObject *obj = vec[i]) {
            JS_SET_TRACING_INDEX(trc, name, i);
            Mark(trc, obj);
        }
    }
}

static inline void
MarkId(JSTracer *trc, jsid id)
{
    if (JSID_IS_STRING(id)) {
        JSString *str = JSID_TO_STRING(id);
        if (!JSString::isStatic(str))
            Mark(trc, str);
    }
    else if (JS_UNLIKELY(JSID_IS_OBJECT(id)))
        Mark(trc, JSID_TO_OBJECT(id));
}

static inline void
MarkId(JSTracer *trc, jsid id, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkId(trc, id);
}

static inline void
MarkIdRange(JSTracer *trc, jsid *beg, jsid *end, const char *name)
{
    for (jsid *idp = beg; idp != end; ++idp) {
        JS_SET_TRACING_INDEX(trc, name, (idp - beg));
        MarkId(trc, *idp);
    }
}

static inline void
MarkIdRange(JSTracer *trc, size_t len, jsid *vec, const char *name)
{
    MarkIdRange(trc, vec, vec + len, name);
}

static inline void
MarkKind(JSTracer *trc, void *thing, uint32 kind)
{
    JS_ASSERT(thing);
    JS_ASSERT(kind == GetGCThingTraceKind(thing));
    switch (kind) {
        case JSTRACE_OBJECT:
            Mark(trc, reinterpret_cast<JSObject *>(thing));
            break;
        case JSTRACE_STRING:
            MarkString(trc, reinterpret_cast<JSString *>(thing));
            break;
#if JS_HAS_XML_SUPPORT
        case JSTRACE_XML:
            Mark(trc, reinterpret_cast<JSXML *>(thing));
            break;
#endif
        default:
            JS_ASSERT(false);
    }
}

/* N.B. Assumes JS_SET_TRACING_NAME/INDEX has already been called. */
static inline void
MarkValueRaw(JSTracer *trc, const js::Value &v)
{
    if (v.isMarkable()) {
        JS_ASSERT(v.toGCThing());
        return MarkKind(trc, v.toGCThing(), v.gcKind());
    }
}

static inline void
MarkValue(JSTracer *trc, const js::Value &v, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkValueRaw(trc, v);
}

static inline void
MarkValueRange(JSTracer *trc, Value *beg, Value *end, const char *name)
{
    for (Value *vp = beg; vp < end; ++vp) {
        JS_SET_TRACING_INDEX(trc, name, vp - beg);
        MarkValueRaw(trc, *vp);
    }
}

static inline void
MarkValueRange(JSTracer *trc, size_t len, Value *vec, const char *name)
{
    MarkValueRange(trc, vec, vec + len, name);
}

static inline void
MarkShapeRange(JSTracer *trc, const Shape **beg, const Shape **end, const char *name)
{
    for (const Shape **sp = beg; sp < end; ++sp) {
        JS_SET_TRACING_INDEX(trc, name, sp - beg);
        (*sp)->trace(trc);
    }
}

static inline void
MarkShapeRange(JSTracer *trc, size_t len, const Shape **vec, const char *name)
{
    MarkShapeRange(trc, vec, vec + len, name);
}

/* N.B. Assumes JS_SET_TRACING_NAME/INDEX has already been called. */
static inline void
MarkGCThing(JSTracer *trc, void *thing, uint32 kind)
{
    if (!thing)
        return;

    MarkKind(trc, thing, kind);
}

static inline void
MarkGCThing(JSTracer *trc, void *thing)
{
    if (!thing)
        return;
    MarkKind(trc, thing, GetGCThingTraceKind(thing));
}

static inline void
MarkGCThing(JSTracer *trc, void *thing, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkGCThing(trc, thing);
}

static inline void
MarkGCThing(JSTracer *trc, void *thing, const char *name, size_t index)
{
    JS_SET_TRACING_INDEX(trc, name, index);
    MarkGCThing(trc, thing);
}

static inline void
Mark(JSTracer *trc, void *thing, uint32 kind, const char *name)
{
    JS_ASSERT(thing);
    JS_SET_TRACING_NAME(trc, name);
    MarkKind(trc, thing, kind);
}

}}

#endif /* jsgcinlines_h___ */
