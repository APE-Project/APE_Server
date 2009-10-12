/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
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

#ifndef jsscope_h___
#define jsscope_h___
/*
 * JS symbol tables.
 */
#include "jstypes.h"
#include "jslock.h"
#include "jsfun.h"
#include "jsobj.h"
#include "jsprvtd.h"
#include "jspubtd.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4800)
#endif

JS_BEGIN_EXTERN_C

/*
 * Given P independent, non-unique properties each of size S words mapped by
 * all scopes in a runtime, construct a property tree of N nodes each of size
 * S+L words (L for tree linkage).  A nominal L value is 2 for leftmost-child
 * and right-sibling links.  We hope that the N < P by enough that the space
 * overhead of L, and the overhead of scope entries pointing at property tree
 * nodes, is worth it.
 *
 * The tree construction goes as follows.  If any empty scope in the runtime
 * has a property X added to it, find or create a node under the tree root
 * labeled X, and set scope->lastProp to point at that node.  If any non-empty
 * scope whose most recently added property is labeled Y has another property
 * labeled Z added, find or create a node for Z under the node that was added
 * for Y, and set scope->lastProp to point at that node.
 *
 * A property is labeled by its members' values: id, getter, setter, slot,
 * attributes, tiny or short id, and a field telling for..in order.  Note that
 * labels are not unique in the tree, but they are unique among a node's kids
 * (barring rare and benign multi-threaded race condition outcomes, see below)
 * and along any ancestor line from the tree root to a given leaf node (except
 * for the hard case of duplicate formal parameters to a function).
 *
 * Thus the root of the tree represents all empty scopes, and the first ply
 * of the tree represents all scopes containing one property, etc.  Each node
 * in the tree can stand for any number of scopes having the same ordered set
 * of properties, where that node was the last added to the scope.  (We need
 * not store the root of the tree as a node, and do not -- all we need are
 * links to its kids.)
 *
 * Sidebar on for..in loop order: ECMA requires no particular order, but this
 * implementation has promised and delivered property definition order, and
 * compatibility is king.  We could use an order number per property, which
 * would require a sort in js_Enumerate, and an entry order generation number
 * per scope.  An order number beats a list, which should be doubly-linked for
 * O(1) delete.  An even better scheme is to use a parent link in the property
 * tree, so that the ancestor line can be iterated from scope->lastProp when
 * filling in a JSIdArray from back to front.  This parent link also helps the
 * GC to sweep properties iteratively.
 *
 * What if a property Y is deleted from a scope?  If Y is the last property in
 * the scope, we simply adjust the scope's lastProp member after we remove the
 * scope's hash-table entry pointing at that property node.  The parent link
 * mentioned in the for..in sidebar above makes this adjustment O(1).  But if
 * Y comes between X and Z in the scope, then we might have to "fork" the tree
 * at X, leaving X->Y->Z in case other scopes have those properties added in
 * that order; and to finish the fork, we'd add a node labeled Z with the path
 * X->Z, if it doesn't exist.  This could lead to lots of extra nodes, and to
 * O(n^2) growth when deleting lots of properties.
 *
 * Rather, for O(1) growth all around, we should share the path X->Y->Z among
 * scopes having those three properties added in that order, and among scopes
 * having only X->Z where Y was deleted.  All such scopes have a lastProp that
 * points to the Z child of Y.  But a scope in which Y was deleted does not
 * have a table entry for Y, and when iterating that scope by traversing the
 * ancestor line from Z, we will have to test for a table entry for each node,
 * skipping nodes that lack entries.
 *
 * What if we add Y again?  X->Y->Z->Y is wrong and we'll enumerate Y twice.
 * Therefore we must fork in such a case, if not earlier.  Because delete is
 * "bursty", we should not fork eagerly.  Delaying a fork till we are at risk
 * of adding Y after it was deleted already requires a flag in the JSScope, to
 * wit, SCOPE_MIDDLE_DELETE.
 *
 * What about thread safety?  If the property tree operations done by requests
 * are find-node and insert-node, then the only hazard is duplicate insertion.
 * This is harmless except for minor bloat.  When all requests have ended or
 * been suspended, the GC is free to sweep the tree after marking all nodes
 * reachable from scopes, performing remove-node operations as needed.
 *
 * Is the property tree worth it compared to property storage in each table's
 * entries?  To decide, we must find the relation <> between the words used
 * with a property tree and the words required without a tree.
 *
 * Model all scopes as one super-scope of capacity T entries (T a power of 2).
 * Let alpha be the load factor of this double hash-table.  With the property
 * tree, each entry in the table is a word-sized pointer to a node that can be
 * shared by many scopes.  But all such pointers are overhead compared to the
 * situation without the property tree, where the table stores property nodes
 * directly, as entries each of size S words.  With the property tree, we need
 * L=2 extra words per node for siblings and kids pointers.  Without the tree,
 * (1-alpha)*S*T words are wasted on free or removed sentinel-entries required
 * by double hashing.
 *
 * Therefore,
 *
 *      (property tree)                 <> (no property tree)
 *      N*(S+L) + T                     <> S*T
 *      N*(S+L) + T                     <> P*S + (1-alpha)*S*T
 *      N*(S+L) + alpha*T + (1-alpha)*T <> P*S + (1-alpha)*S*T
 *
 * Note that P is alpha*T by definition, so
 *
 *      N*(S+L) + P + (1-alpha)*T <> P*S + (1-alpha)*S*T
 *      N*(S+L)                   <> P*S - P + (1-alpha)*S*T - (1-alpha)*T
 *      N*(S+L)                   <> (P + (1-alpha)*T) * (S-1)
 *      N*(S+L)                   <> (P + (1-alpha)*P/alpha) * (S-1)
 *      N*(S+L)                   <> P * (1/alpha) * (S-1)
 *
 * Let N = P*beta for a compression ratio beta, beta <= 1:
 *
 *      P*beta*(S+L) <> P * (1/alpha) * (S-1)
 *      beta*(S+L)   <> (S-1)/alpha
 *      beta         <> (S-1)/((S+L)*alpha)
 *
 * For S = 6 (32-bit architectures) and L = 2, the property tree wins iff
 *
 *      beta < 5/(8*alpha)
 *
 * We ensure that alpha <= .75, so the property tree wins if beta < .83_.  An
 * average beta from recent Mozilla browser startups was around .6.
 *
 * Can we reduce L?  Observe that the property tree degenerates into a list of
 * lists if at most one property Y follows X in all scopes.  In or near such a
 * case, we waste a word on the right-sibling link outside of the root ply of
 * the tree.  Note also that the root ply tends to be large, so O(n^2) growth
 * searching it is likely, indicating the need for hashing (but with increased
 * thread safety costs).
 *
 * If only K out of N nodes in the property tree have more than one child, we
 * could eliminate the sibling link and overlay a children list or hash-table
 * pointer on the leftmost-child link (which would then be either null or an
 * only-child link; the overlay could be tagged in the low bit of the pointer,
 * or flagged elsewhere in the property tree node, although such a flag must
 * not be considered when comparing node labels during tree search).
 *
 * For such a system, L = 1 + (K * averageChildrenTableSize) / N instead of 2.
 * If K << N, L approaches 1 and the property tree wins if beta < .95.
 *
 * We observe that fan-out below the root ply of the property tree appears to
 * have extremely low degree (see the MeterPropertyTree code that histograms
 * child-counts in jsscope.c), so instead of a hash-table we use a linked list
 * of child node pointer arrays ("kid chunks").  The details are isolated in
 * jsscope.c; others must treat JSScopeProperty.kids as opaque.  We leave it
 * strongly typed for debug-ability of the common (null or one-kid) cases.
 *
 * One final twist (can you stand it?): the mean number of entries per scope
 * in Mozilla is < 5, with a large standard deviation (~8).  Instead of always
 * allocating scope->table, we leave it null while initializing all the other
 * scope members as if it were non-null and minimal-length.  Until a property
 * is added that crosses the threshold of 6 or more entries for hashing, or
 * until a "middle delete" occurs, we use linear search from scope->lastProp
 * to find a given id, and save on the space overhead of a hash table.
 */

struct JSScope : public JSObjectMap
{
#ifdef JS_THREADSAFE
    JSTitle         title;              /* lock state */
#endif
    JSObject        *object;            /* object that owns this scope */
    jsrefcount      nrefs;              /* count of all referencing objects */
    uint32          freeslot;           /* index of next free slot in object */
    JSScope         *emptyScope;        /* cache for getEmptyScope below */
    uint8           flags;              /* flags, see below */
    int8            hashShift;          /* multiplicative hash shift */

    uint16          spare;              /* reserved */
    uint32          entryCount;         /* number of entries in table */
    uint32          removedCount;       /* removed entry sentinels in table */
    JSScopeProperty **table;            /* table of ptrs to shared tree nodes */
    JSScopeProperty *lastProp;          /* pointer to last property added */

  private:
    void initMinimal(JSContext *cx, uint32 newShape);
    bool createTable(JSContext *cx, bool report);
    bool changeTable(JSContext *cx, int change);
    void reportReadOnlyScope(JSContext *cx);
    void generateOwnShape(JSContext *cx);
    JSScopeProperty **searchTable(jsid id, bool adding);
    inline JSScopeProperty **search(jsid id, bool adding);
    JSScope *createEmptyScope(JSContext *cx, JSClass *clasp);

  public:
    explicit JSScope(const JSObjectOps *ops, JSObject *obj = NULL)
      : JSObjectMap(ops, 0), object(obj) {}

    /* Create a mutable, owned, empty scope. */
    static JSScope *create(JSContext *cx, const JSObjectOps *ops, JSClass *clasp,
                           JSObject *obj, uint32 shape);

    static void destroy(JSContext *cx, JSScope *scope);

    /*
     * Return an immutable, shareable, empty scope with the same ops as this
     * and the same freeslot as this had when empty.
     *
     * If |this| is the scope of an object |proto|, the resulting scope can be
     * used as the scope of a new object whose prototype is |proto|.
     */
    JSScope *getEmptyScope(JSContext *cx, JSClass *clasp) {
        if (emptyScope) {
            emptyScope->hold();
            return emptyScope;
        }
        return createEmptyScope(cx, clasp);
    }

    bool getEmptyScopeShape(JSContext *cx, JSClass *clasp, uint32 *shapep) {
        if (emptyScope) {
            *shapep = emptyScope->shape;
            return true;
        }
        JSScope *e = getEmptyScope(cx, clasp);
        if (!e)
            return false;
        *shapep = e->shape;
        e->drop(cx, NULL);
        return true;
    }

    inline void hold();
    inline bool drop(JSContext *cx, JSObject *obj);

    JSScopeProperty *lookup(jsid id);
    bool has(JSScopeProperty *sprop);

    JSScopeProperty *add(JSContext *cx, jsid id,
                         JSPropertyOp getter, JSPropertyOp setter,
                         uint32 slot, uintN attrs,
                         uintN flags, intN shortid);

    JSScopeProperty *change(JSContext *cx, JSScopeProperty *sprop,
                            uintN attrs, uintN mask,
                            JSPropertyOp getter, JSPropertyOp setter);

    bool remove(JSContext *cx, jsid id);
    void clear(JSContext *cx);

    void extend(JSContext *cx, JSScopeProperty *sprop);

    /*
     * Read barrier to clone a joined function object stored as a method.
     * Defined inline further below.
     */
    inline bool methodReadBarrier(JSContext *cx, JSScopeProperty *sprop, jsval *vp);

    /*
     * Write barrier to check for a method value change. Defined inline below
     * after methodReadBarrier. Two flavors to handle JSOP_*GVAR, which deals
     * in slots not sprops, while not deoptimizing to map slot to sprop unless
     * flags show this is necessary. The methodShapeChange overload (directly
     * below) parallels this.
     */
    inline bool methodWriteBarrier(JSContext *cx, JSScopeProperty *sprop, jsval v);
    inline bool methodWriteBarrier(JSContext *cx, uint32 slot, jsval v);

    void trace(JSTracer *trc);

    void brandingShapeChange(JSContext *cx, uint32 slot, jsval v);
    void deletingShapeChange(JSContext *cx, JSScopeProperty *sprop);
    bool methodShapeChange(JSContext *cx, JSScopeProperty *sprop, jsval toval);
    bool methodShapeChange(JSContext *cx, uint32 slot, jsval toval);
    void protoShapeChange(JSContext *cx);
    void replacingShapeChange(JSContext *cx, JSScopeProperty *sprop, JSScopeProperty *newsprop);
    void sealingShapeChange(JSContext *cx);
    void shadowingShapeChange(JSContext *cx, JSScopeProperty *sprop);

/* By definition, hashShift = JS_DHASH_BITS - log2(capacity). */
#define SCOPE_CAPACITY(scope)           JS_BIT(JS_DHASH_BITS-(scope)->hashShift)

    enum {
        MIDDLE_DELETE           = 0x0001,
        SEALED                  = 0x0002,
        BRANDED                 = 0x0004,
        INDEXED_PROPERTIES      = 0x0008,
        OWN_SHAPE               = 0x0010,
        METHOD_BARRIER          = 0x0020,

        /*
         * This flag toggles with each shape-regenerating GC cycle.
         * See JSRuntime::gcRegenShapesScopeFlag.
         */
        SHAPE_REGEN             = 0x0040
    };

    bool hadMiddleDelete()      { return flags & MIDDLE_DELETE; }
    void setMiddleDelete()      { flags |= MIDDLE_DELETE; }
    void clearMiddleDelete()    { flags &= ~MIDDLE_DELETE; }

    /*
     * Don't define clearSealed, as it can't be done safely because JS_LOCK_OBJ
     * will avoid taking the lock if the object owns its scope and the scope is
     * sealed.
     */
    bool sealed()               { return flags & SEALED; }
    void setSealed()            { flags |= SEALED; }

    /*
     * A branded scope's object contains plain old methods (function-valued
     * properties without magic getters and setters), and its scope->shape
     * evolves whenever a function value changes.
     */
    bool branded()              { return flags & BRANDED; }
    void setBranded()           { flags |= BRANDED; }

    bool hadIndexedProperties() { return flags & INDEXED_PROPERTIES; }
    void setIndexedProperties() { flags |= INDEXED_PROPERTIES; }

    bool hasOwnShape()          { return flags & OWN_SHAPE; }
    void setOwnShape()          { flags |= OWN_SHAPE; }

    bool hasRegenFlag(uint8 regenFlag) { return (flags & SHAPE_REGEN) == regenFlag; }

    /*
     * A scope has a method barrier when some compiler-created "null closure"
     * function objects (functions that do not use lexical bindings above their
     * scope, only free variable names) that have a correct JSSLOT_PARENT value
     * thanks to the COMPILE_N_GO optimization are stored as newly added direct
     * property values.
     *
     * The de-facto standard JS language requires each evaluation of such a
     * closure to result in a unique (according to === and observable effects)
     * function object. ES3 tried to allow implementations to "join" such
     * objects to a single compiler-created object, but this makes an overt
     * mutation hazard, also an "identity hazard" against interoperation among
     * implementations that join and do not join.
     *
     * To stay compatible with the de-facto standard, we store the compiler-
     * created function object as the method value, set the METHOD_BARRIER
     * flag, and brand the scope with a predictable shape that reflects its
     * method values, which are cached and traced without being loaded, based
     * on shape-qualified cache hit logic and equivalent trace guards. See
     * BRANDED above.
     *
     * This means scope->hasMethodBarrier() => scope->branded(), but of course
     * not the other way around.
     *
     * Then when reading from a scope for which scope->hasMethodBarrier() is
     * true, we count on the scope's qualified/guarded shape being unique and
     * add a read barrier that clones the compiler-created function object on
     * demand, reshaping the scope.
     *
     * This read barrier is bypassed when evaluating the callee sub-expression
     * of a call expression (see the JOF_CALLOP opcodes in jsopcode.tbl), since
     * such ops do not present an identity or mutation hazard.
     */
    bool hasMethodBarrier()     { return flags & METHOD_BARRIER; }
    void setMethodBarrier()     { flags |= METHOD_BARRIER | BRANDED; }

    bool owned()                { return object != NULL; }
};

inline bool
JS_IS_SCOPE_LOCKED(JSContext *cx, JSScope *scope)
{
    return JS_IS_TITLE_LOCKED(cx, &scope->title);
}

inline JSScope *
OBJ_SCOPE(JSObject *obj)
{
    JS_ASSERT(OBJ_IS_NATIVE(obj));
    return (JSScope *) obj->map;
}

inline uint32
OBJ_SHAPE(JSObject *obj)
{
    JS_ASSERT(obj->map->shape != JSObjectMap::SHAPELESS);
    return obj->map->shape;
}

/*
 * A little information hiding for scope->lastProp, in case it ever becomes
 * a tagged pointer again.
 */
#define SCOPE_LAST_PROP(scope)                                                \
    (JS_ASSERT_IF((scope)->lastProp, !JSVAL_IS_NULL((scope)->lastProp->id)),  \
     (scope)->lastProp)
#define SCOPE_REMOVE_LAST_PROP(scope)                                         \
    (JS_ASSERT_IF((scope)->lastProp->parent,                                  \
                  !JSVAL_IS_NULL((scope)->lastProp->parent->id)),             \
     (scope)->lastProp = (scope)->lastProp->parent)

/*
 * Helpers for reinterpreting JSPropertyOp as JSObject* for scripted getters
 * and setters.
 */
inline JSObject *
js_CastAsObject(JSPropertyOp op)
{
    return JS_FUNC_TO_DATA_PTR(JSObject *, op);
}

inline jsval
js_CastAsObjectJSVal(JSPropertyOp op)
{
    return OBJECT_TO_JSVAL(JS_FUNC_TO_DATA_PTR(JSObject *, op));
}

inline JSPropertyOp
js_CastAsPropertyOp(JSObject *object)
{
    return JS_DATA_TO_FUNC_PTR(JSPropertyOp, object);
}

struct JSScopeProperty {
    jsid            id;                 /* int-tagged jsval/untagged JSAtom* */
    JSPropertyOp    getter;             /* getter and setter hooks or objects */
    JSPropertyOp    setter;             /* getter is JSObject* and setter is 0
                                           if sprop->isMethod() */
    uint32          slot;               /* abstract index in object slots */
    uint8           attrs;              /* attributes, see jsapi.h JSPROP_* */
    uint8           flags;              /* flags, see below for defines */
    int16           shortid;            /* tinyid, or local arg/var index */
    JSScopeProperty *parent;            /* parent node, reverse for..in order */
    JSScopeProperty *kids;              /* null, single child, or a tagged ptr
                                           to many-kids data structure */
    uint32          shape;              /* property cache shape identifier */

/* Bits stored in sprop->flags. */
#define SPROP_MARK                      0x01
#define SPROP_IS_ALIAS                  0x02
#define SPROP_HAS_SHORTID               0x04
#define SPROP_FLAG_SHAPE_REGEN          0x08
#define SPROP_IS_METHOD                 0x10

    bool isMethod() const {
        return flags & SPROP_IS_METHOD;
    }
    JSObject *methodObject() const {
        JS_ASSERT(isMethod());
        return js_CastAsObject(getter);
    }
    jsval methodValue() const {
        JS_ASSERT(isMethod());
        return js_CastAsObjectJSVal(getter);
    }

    bool hasGetterObject() const {
        return attrs & JSPROP_GETTER;
    }
    JSObject *getterObject() const {
        JS_ASSERT(hasGetterObject());
        return js_CastAsObject(getter);
    }
    jsval getterValue() const {
        JS_ASSERT(hasGetterObject());
        return js_CastAsObjectJSVal(getter);
    }

    bool hasSetterObject() const {
        return attrs & JSPROP_SETTER;
    }
    JSObject *setterObject() const {
        JS_ASSERT(hasSetterObject());
        return js_CastAsObject(setter);
    }
    jsval setterValue() const {
        JS_ASSERT(hasSetterObject());
        return js_CastAsObjectJSVal(setter);
    }

    bool get(JSContext* cx, JSObject* obj, JSObject *pobj, jsval* vp);
    bool set(JSContext* cx, JSObject* obj, jsval* vp);

    void trace(JSTracer *trc);
};

/* JSScopeProperty pointer tag bit indicating a collision. */
#define SPROP_COLLISION                 ((jsuword)1)
#define SPROP_REMOVED                   ((JSScopeProperty *) SPROP_COLLISION)

/* Macros to get and set sprop pointer values and collision flags. */
#define SPROP_IS_FREE(sprop)            ((sprop) == NULL)
#define SPROP_IS_REMOVED(sprop)         ((sprop) == SPROP_REMOVED)
#define SPROP_IS_LIVE(sprop)            ((sprop) > SPROP_REMOVED)
#define SPROP_FLAG_COLLISION(spp,sprop) (*(spp) = (JSScopeProperty *)         \
                                         ((jsuword)(sprop) | SPROP_COLLISION))
#define SPROP_HAD_COLLISION(sprop)      ((jsuword)(sprop) & SPROP_COLLISION)
#define SPROP_FETCH(spp)                SPROP_CLEAR_COLLISION(*(spp))

#define SPROP_CLEAR_COLLISION(sprop)                                          \
    ((JSScopeProperty *) ((jsuword)(sprop) & ~SPROP_COLLISION))

#define SPROP_STORE_PRESERVING_COLLISION(spp, sprop)                          \
    (*(spp) = (JSScopeProperty *) ((jsuword)(sprop)                           \
                                   | SPROP_HAD_COLLISION(*(spp))))

inline JSScopeProperty *
JSScope::lookup(jsid id)
{
    return SPROP_FETCH(search(id, false));
}

inline bool
JSScope::has(JSScopeProperty *sprop)
{
    return lookup(sprop->id) == sprop;
}

/*
 * If SPROP_HAS_SHORTID is set in sprop->flags, we use sprop->shortid rather
 * than id when calling sprop's getter or setter.
 */
#define SPROP_USERID(sprop)                                                   \
    (((sprop)->flags & SPROP_HAS_SHORTID) ? INT_TO_JSVAL((sprop)->shortid)    \
                                          : ID_TO_VALUE((sprop)->id))

#define SPROP_INVALID_SLOT              0xffffffff

#define SLOT_IN_SCOPE(slot,scope)         ((slot) < (scope)->freeslot)
#define SPROP_HAS_VALID_SLOT(sprop,scope) SLOT_IN_SCOPE((sprop)->slot, scope)

#define SPROP_HAS_STUB_GETTER(sprop)    (!(sprop)->getter)
#define SPROP_HAS_STUB_SETTER(sprop)    (!(sprop)->setter)

#define SPROP_HAS_STUB_GETTER_OR_IS_METHOD(sprop)                             \
    (SPROP_HAS_STUB_GETTER(sprop) || (sprop)->isMethod())

#ifndef JS_THREADSAFE
# define js_GenerateShape(cx, gcLocked)    js_GenerateShape (cx)
#endif

extern uint32
js_GenerateShape(JSContext *cx, bool gcLocked);

#ifdef JS_DUMP_PROPTREE_STATS
struct JSScopeStats {
    jsrefcount          searches;
    jsrefcount          hits;
    jsrefcount          misses;
    jsrefcount          hashes;
    jsrefcount          steps;
    jsrefcount          stepHits;
    jsrefcount          stepMisses;
    jsrefcount          adds;
    jsrefcount          redundantAdds;
    jsrefcount          addFailures;
    jsrefcount          changeFailures;
    jsrefcount          compresses;
    jsrefcount          grows;
    jsrefcount          removes;
    jsrefcount          removeFrees;
    jsrefcount          uselessRemoves;
    jsrefcount          shrinks;
};

extern JS_FRIEND_DATA(JSScopeStats) js_scope_stats;

# define METER(x)       JS_ATOMIC_INCREMENT(&js_scope_stats.x)
#else
# define METER(x)       /* nothing */
#endif

inline JSScopeProperty **
JSScope::search(jsid id, bool adding)
{
    JSScopeProperty *sprop, **spp;

    METER(searches);
    if (!table) {
        /* Not enough properties to justify hashing: search from lastProp. */
        JS_ASSERT(!hadMiddleDelete());
        for (spp = &lastProp; (sprop = *spp); spp = &sprop->parent) {
            if (sprop->id == id) {
                METER(hits);
                return spp;
            }
        }
        METER(misses);
        return spp;
    }
    return searchTable(id, adding);
}

#undef METER

inline void
JSScope::hold()
{
    JS_ASSERT(nrefs >= 0);
    JS_ATOMIC_INCREMENT(&nrefs);
}

inline bool
JSScope::drop(JSContext *cx, JSObject *obj)
{
#ifdef JS_THREADSAFE
    /* We are called from only js_ShareWaitingTitles and js_FinalizeObject. */
    JS_ASSERT(!obj || CX_THREAD_IS_RUNNING_GC(cx));
#endif
    JS_ASSERT(nrefs > 0);
    --nrefs;

    if (nrefs == 0) {
        destroy(cx, this);
        return false;
    }
    if (object == obj)
        object = NULL;
    return true;
}

inline void
JSScope::extend(JSContext *cx, JSScopeProperty *sprop)
{
    js_LeaveTraceIfGlobalObject(cx, object);
    shape = (!lastProp || shape == lastProp->shape)
            ? sprop->shape
            : js_GenerateShape(cx, false);
    ++entryCount;
    lastProp = sprop;

    jsuint index;
    if (js_IdIsIndex(sprop->id, &index))
        setIndexedProperties();

    if (sprop->isMethod())
        setMethodBarrier();
}

/*
 * Property read barrier for deferred cloning of compiler-created function
 * objects optimized as typically non-escaping, ad-hoc methods in obj.
 *
 * Called only from JSScopeProperty::get when sprop->isMethod(), or JIT-
 * equivalent code. sprop->isMethod() implies that scope->hasMethodBarrier()
 * for the scope containing that sprop.
 */
inline bool
JSScope::methodReadBarrier(JSContext *cx, JSScopeProperty *sprop, jsval *vp)
{
    JS_ASSERT(hasMethodBarrier());
    JS_ASSERT(has(sprop));
    JS_ASSERT(sprop->isMethod());
    JS_ASSERT(sprop->methodValue() == *vp);

    JS_ASSERT(object->getClass() == &js_ObjectClass);

    JSObject *funobj = JSVAL_TO_OBJECT(*vp);
    JSFunction *fun = GET_FUNCTION_PRIVATE(cx, funobj);
    JS_ASSERT(FUN_OBJECT(fun) == funobj && FUN_NULL_CLOSURE(fun));

    funobj = js_CloneFunctionObject(cx, fun, OBJ_GET_PARENT(cx, funobj));
    if (!funobj)
        return false;
    *vp = OBJECT_TO_JSVAL(funobj);
    return js_SetPropertyHelper(cx, object, sprop->id, 0, vp);
}

inline bool
JSScope::methodWriteBarrier(JSContext *cx, JSScopeProperty *sprop, jsval v)
{
    if (branded()) {
        jsval prev = LOCKED_OBJ_GET_SLOT(object, sprop->slot);

        if (prev != v && VALUE_IS_FUNCTION(cx, prev))
            return methodShapeChange(cx, sprop, v);
    }
    return true;
}

inline bool
JSScope::methodWriteBarrier(JSContext *cx, uint32 slot, jsval v)
{
    if (branded()) {
        jsval prev = LOCKED_OBJ_GET_SLOT(object, slot);

        if (prev != v && VALUE_IS_FUNCTION(cx, prev))
            return methodShapeChange(cx, slot, v);
    }
    return true;
}

inline void
JSScope::trace(JSTracer *trc)
{
    JSContext *cx = trc->context;
    JSScopeProperty *sprop = lastProp;
    uint8 regenFlag = cx->runtime->gcRegenShapesScopeFlag;
    if (IS_GC_MARKING_TRACER(trc) && cx->runtime->gcRegenShapes && hasRegenFlag(regenFlag)) {
        /*
         * Either this scope has its own shape, which must be regenerated, or
         * it must have the same shape as lastProp.
         */
        uint32 newShape;

        if (sprop) {
            if (!(sprop->flags & SPROP_FLAG_SHAPE_REGEN)) {
                sprop->shape = js_RegenerateShapeForGC(cx);
                sprop->flags |= SPROP_FLAG_SHAPE_REGEN;
            }
            newShape = sprop->shape;
        }
        if (!sprop || hasOwnShape()) {
            newShape = js_RegenerateShapeForGC(cx);
            JS_ASSERT_IF(sprop, newShape != sprop->shape);
        }
        shape = newShape;
        flags ^= JSScope::SHAPE_REGEN;

        /* Also regenerate the shapes of empty scopes, in case they are not shared. */
        for (JSScope *empty = emptyScope;
             empty && empty->hasRegenFlag(regenFlag);
             empty = empty->emptyScope) {
            empty->shape = js_RegenerateShapeForGC(cx);
            empty->flags ^= JSScope::SHAPE_REGEN;
        }
    }
    if (sprop) {
        JS_ASSERT(has(sprop));

        /* Trace scope's property tree ancestor line. */
        do {
            if (hadMiddleDelete() && !has(sprop))
                continue;
            sprop->trace(trc);
        } while ((sprop = sprop->parent) != NULL);
    }
}

inline bool
JSScopeProperty::get(JSContext* cx, JSObject* obj, JSObject *pobj, jsval* vp)
{
    JS_ASSERT(!SPROP_HAS_STUB_GETTER(this));
    JS_ASSERT(!JSVAL_IS_NULL(this->id));

    if (attrs & JSPROP_GETTER) {
        JS_ASSERT(!isMethod());
        jsval fval = getterValue();
        return js_InternalGetOrSet(cx, obj, id, fval, JSACC_READ, 0, 0, vp);
    }

    if (isMethod()) {
        *vp = methodValue();

        JSScope *scope = OBJ_SCOPE(pobj);
        JS_ASSERT(scope->object == pobj);
        return scope->methodReadBarrier(cx, this, vp);
    }

    /*
     * JSObjectOps is private, so we know there are only two implementations
     * of the thisObject hook: with objects and XPConnect wrapped native
     * objects.  XPConnect objects don't expect the hook to be called here,
     * but with objects do.
     */
    if (STOBJ_GET_CLASS(obj) == &js_WithClass)
        obj = obj->map->ops->thisObject(cx, obj);
    return getter(cx, obj, SPROP_USERID(this), vp);
}

inline bool
JSScopeProperty::set(JSContext* cx, JSObject* obj, jsval* vp)
{
    JS_ASSERT_IF(SPROP_HAS_STUB_SETTER(this), attrs & JSPROP_GETTER);

    if (attrs & JSPROP_SETTER) {
        jsval fval = setterValue();
        return js_InternalGetOrSet(cx, obj, id, fval, JSACC_WRITE, 1, vp, vp);
    }

    if (attrs & JSPROP_GETTER) {
        js_ReportGetterOnlyAssignment(cx);
        return false;
    }

    /* See the comment in JSScopeProperty::get as to why we can check for With. */
    if (STOBJ_GET_CLASS(obj) == &js_WithClass)
        obj = obj->map->ops->thisObject(cx, obj);
    return setter(cx, obj, SPROP_USERID(this), vp);
}

/* Macro for common expression to test for shared permanent attributes. */
#define SPROP_IS_SHARED_PERMANENT(sprop)                                      \
    ((~(sprop)->attrs & (JSPROP_SHARED | JSPROP_PERMANENT)) == 0)

extern JSScope *
js_GetMutableScope(JSContext *cx, JSObject *obj);

extern void
js_TraceId(JSTracer *trc, jsid id);

extern void
js_SweepScopeProperties(JSContext *cx);

extern bool
js_InitPropertyTree(JSRuntime *rt);

extern void
js_FinishPropertyTree(JSRuntime *rt);

JS_END_EXTERN_C

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif /* jsscope_h___ */
