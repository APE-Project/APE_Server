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
 * Portions created by the Initial Developer are Copyright (C) 1999
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   John Bandhauer <jband@netscape.com> (original author)
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

/* Class used to manage the wrapped native objects within a JS scope. */

#include "xpcprivate.h"
#ifdef DEBUG
#include "XPCNativeWrapper.h"
#endif

/***************************************************************************/

#ifdef XPC_TRACK_SCOPE_STATS
static int DEBUG_TotalScopeCount;
static int DEBUG_TotalLiveScopeCount;
static int DEBUG_TotalMaxScopeCount;
static int DEBUG_TotalScopeTraversalCount;
static PRBool  DEBUG_DumpedStats;
#endif

#ifdef DEBUG
static void DEBUG_TrackNewScope(XPCWrappedNativeScope* scope)
{
#ifdef XPC_TRACK_SCOPE_STATS
    DEBUG_TotalScopeCount++;
    DEBUG_TotalLiveScopeCount++;
    if(DEBUG_TotalMaxScopeCount < DEBUG_TotalLiveScopeCount)
        DEBUG_TotalMaxScopeCount = DEBUG_TotalLiveScopeCount;
#endif
}

static void DEBUG_TrackDeleteScope(XPCWrappedNativeScope* scope)
{
#ifdef XPC_TRACK_SCOPE_STATS
    DEBUG_TotalLiveScopeCount--;
#endif
}

static void DEBUG_TrackScopeTraversal()
{
#ifdef XPC_TRACK_SCOPE_STATS
    DEBUG_TotalScopeTraversalCount++;
#endif
}

static void DEBUG_TrackScopeShutdown()
{
#ifdef XPC_TRACK_SCOPE_STATS
    if(!DEBUG_DumpedStats)
    {
        DEBUG_DumpedStats = PR_TRUE;
        printf("%d XPCWrappedNativeScope(s) were constructed.\n",
               DEBUG_TotalScopeCount);

        printf("%d XPCWrappedNativeScopes(s) max alive at one time.\n",
               DEBUG_TotalMaxScopeCount);

        printf("%d XPCWrappedNativeScope(s) alive now.\n" ,
               DEBUG_TotalLiveScopeCount);

        printf("%d traversals of Scope list.\n",
               DEBUG_TotalScopeTraversalCount);
    }
#endif
}
#else
#define DEBUG_TrackNewScope(scope) ((void)0)
#define DEBUG_TrackDeleteScope(scope) ((void)0)
#define DEBUG_TrackScopeTraversal() ((void)0)
#define DEBUG_TrackScopeShutdown() ((void)0)
#endif

/***************************************************************************/

XPCWrappedNativeScope* XPCWrappedNativeScope::gScopes = nsnull;
XPCWrappedNativeScope* XPCWrappedNativeScope::gDyingScopes = nsnull;

// static
XPCWrappedNativeScope*
XPCWrappedNativeScope::GetNewOrUsed(XPCCallContext& ccx, JSObject* aGlobal)
{

    XPCWrappedNativeScope* scope = FindInJSObjectScope(ccx, aGlobal, JS_TRUE);
    if(!scope)
        scope = new XPCWrappedNativeScope(ccx, aGlobal);
    else
    {
        // We need to call SetGlobal in order to refresh our cached
        // mPrototypeJSObject and mPrototypeJSFunction and to clear
        // mPrototypeNoHelper (so we get a new one if requested in the
        // new scope) in the case where the global object is being
        // reused (JS_ClearScope has been called).  NOTE: We are only
        // called by nsXPConnect::InitClasses.
        scope->SetGlobal(ccx, aGlobal);
    }
    return scope;
}

XPCWrappedNativeScope::XPCWrappedNativeScope(XPCCallContext& ccx,
                                             JSObject* aGlobal)
    :   mRuntime(ccx.GetRuntime()),
        mWrappedNativeMap(Native2WrappedNativeMap::newMap(XPC_NATIVE_MAP_SIZE)),
        mWrappedNativeProtoMap(ClassInfo2WrappedNativeProtoMap::newMap(XPC_NATIVE_PROTO_MAP_SIZE)),
        mMainThreadWrappedNativeProtoMap(ClassInfo2WrappedNativeProtoMap::newMap(XPC_NATIVE_PROTO_MAP_SIZE)),
        mWrapperMap(WrappedNative2WrapperMap::newMap(XPC_WRAPPER_MAP_SIZE)),
        mComponents(nsnull),
        mNext(nsnull),
        mGlobalJSObject(nsnull),
        mPrototypeJSObject(nsnull),
        mPrototypeJSFunction(nsnull),
        mPrototypeNoHelper(nsnull)
#ifndef XPCONNECT_STANDALONE
        , mScriptObjectPrincipal(nsnull)
#endif
{
    // add ourselves to the scopes list
    {   // scoped lock
        XPCAutoLock lock(mRuntime->GetMapLock());

#ifdef DEBUG
        for(XPCWrappedNativeScope* cur = gScopes; cur; cur = cur->mNext)
            NS_ASSERTION(aGlobal != cur->GetGlobalJSObject(), "dup object");
#endif

        mNext = gScopes;
        gScopes = this;

        // Grab the XPCContext associated with our context.
        mContext = XPCContext::GetXPCContext(ccx.GetJSContext());
        mContext->AddScope(this);
    }

    if(aGlobal)
        SetGlobal(ccx, aGlobal);

    DEBUG_TrackNewScope(this);
    MOZ_COUNT_CTOR(XPCWrappedNativeScope);
}

// static
JSBool
XPCWrappedNativeScope::IsDyingScope(XPCWrappedNativeScope *scope)
{
    for(XPCWrappedNativeScope *cur = gDyingScopes; cur; cur = cur->mNext)
    {
        if(scope == cur)
            return JS_TRUE;
    }
    return JS_FALSE;
}

void
XPCWrappedNativeScope::SetComponents(nsXPCComponents* aComponents)
{
    NS_IF_ADDREF(aComponents);
    NS_IF_RELEASE(mComponents);
    mComponents = aComponents;
}

// Dummy JS class to let wrappers w/o an xpc prototype share
// scopes. By doing this we avoid allocating a new scope for every
// wrapper on creation of the wrapper, and most wrappers won't need
// their own scope at all for the lifetime of the wrapper.
// WRAPPER_SLOTS is key here (even though there's never anything
// in the private data slot in these prototypes), as the number of
// reserved slots in this class needs to match that of the wrappers
// for the JS engine to share scopes.

JSClass XPC_WN_NoHelper_Proto_JSClass = {
    "XPC_WN_NoHelper_Proto_JSClass",// name;
    WRAPPER_SLOTS,                  // flags;

    /* Mandatory non-null function pointer members. */
    JS_PropertyStub,                // addProperty;
    JS_PropertyStub,                // delProperty;
    JS_PropertyStub,                // getProperty;
    JS_PropertyStub,                // setProperty;
    JS_EnumerateStub,               // enumerate;
    JS_ResolveStub,                 // resolve;
    JS_ConvertStub,                 // convert;
    nsnull,                         // finalize;

    /* Optionally non-null members start here. */
    XPC_WN_Proto_GetObjectOps,      // getObjectOps;
    nsnull,                         // checkAccess;
    nsnull,                         // call;
    nsnull,                         // construct;
    nsnull,                         // xdrObject;
    nsnull,                         // hasInstance;
    nsnull,                         // mark/trace;
    nsnull                          // spare;
};


void
XPCWrappedNativeScope::SetGlobal(XPCCallContext& ccx, JSObject* aGlobal)
{
    // We allow for calling this more than once. This feature is used by
    // nsXPConnect::InitClassesWithNewWrappedGlobal.

    mGlobalJSObject = aGlobal;
#ifndef XPCONNECT_STANDALONE
    mScriptObjectPrincipal = nsnull;
    // Now init our script object principal, if the new global has one

    const JSClass* jsClass = STOBJ_GET_CLASS(aGlobal);
    if(!(~jsClass->flags & (JSCLASS_HAS_PRIVATE |
                            JSCLASS_PRIVATE_IS_NSISUPPORTS)))
    {
        // Our global has an nsISupports native pointer.  Let's
        // see whether it's what we want.
        nsISupports* priv = (nsISupports*)xpc_GetJSPrivate(aGlobal);
        nsCOMPtr<nsIXPConnectWrappedNative> native =
            do_QueryInterface(priv);
        nsCOMPtr<nsIScriptObjectPrincipal> sop;
        if(native)
        {
            sop = do_QueryWrappedNative(native);
        }
        if(!sop)
        {
            sop = do_QueryInterface(priv);
        }
        mScriptObjectPrincipal = sop;
    }
#endif

    // Lookup 'globalObject.Object.prototype' for our wrapper's proto
    {
        AutoJSErrorAndExceptionEater eater(ccx); // scoped error eater

        jsval val;
        jsid idObj = mRuntime->GetStringID(XPCJSRuntime::IDX_OBJECT);
        jsid idFun = mRuntime->GetStringID(XPCJSRuntime::IDX_FUNCTION);
        jsid idProto = mRuntime->GetStringID(XPCJSRuntime::IDX_PROTOTYPE);

        if(JS_GetPropertyById(ccx, aGlobal, idObj, &val) &&
           !JSVAL_IS_PRIMITIVE(val) &&
           JS_GetPropertyById(ccx, JSVAL_TO_OBJECT(val), idProto, &val) &&
           !JSVAL_IS_PRIMITIVE(val))
        {
            mPrototypeJSObject = JSVAL_TO_OBJECT(val);
        }
        else
        {
            NS_ERROR("Can't get globalObject.Object.prototype");
        }

        if(JS_GetPropertyById(ccx, aGlobal, idFun, &val) &&
           !JSVAL_IS_PRIMITIVE(val) &&
           JS_GetPropertyById(ccx, JSVAL_TO_OBJECT(val), idProto, &val) &&
           !JSVAL_IS_PRIMITIVE(val))
        {
            mPrototypeJSFunction = JSVAL_TO_OBJECT(val);
        }
        else
        {
            NS_ERROR("Can't get globalObject.Function.prototype");
        }
    }

    // Clear the no helper wrapper prototype object so that a new one
    // gets created if needed.
    mPrototypeNoHelper = nsnull;
}

XPCWrappedNativeScope::~XPCWrappedNativeScope()
{
    MOZ_COUNT_DTOR(XPCWrappedNativeScope);
    DEBUG_TrackDeleteScope(this);

    // We can do additional cleanup assertions here...

    if(mWrappedNativeMap)
    {
        NS_ASSERTION(0 == mWrappedNativeMap->Count(), "scope has non-empty map");
        delete mWrappedNativeMap;
    }

    if(mWrappedNativeProtoMap)
    {
        NS_ASSERTION(0 == mWrappedNativeProtoMap->Count(), "scope has non-empty map");
        delete mWrappedNativeProtoMap;
    }

    if(mMainThreadWrappedNativeProtoMap)
    {
        NS_ASSERTION(0 == mMainThreadWrappedNativeProtoMap->Count(), "scope has non-empty map");
        delete mMainThreadWrappedNativeProtoMap;
    }

    if(mWrapperMap)
    {
        NS_ASSERTION(0 == mWrapperMap->Count(), "scope has non-empty map");
        delete mWrapperMap;
    }

    if(mContext)
        mContext->RemoveScope(this);

    // XXX we should assert that we are dead or that xpconnect has shutdown
    // XXX might not want to do this at xpconnect shutdown time???
    NS_IF_RELEASE(mComponents);
}

JSObject *
XPCWrappedNativeScope::GetPrototypeNoHelper(XPCCallContext& ccx)
{
    // We could create this prototype in SetGlobal(), but all scopes
    // don't need one, so we save ourselves a bit of space if we
    // create these when they're needed.
    if(!mPrototypeNoHelper)
    {
        mPrototypeNoHelper =
            xpc_NewSystemInheritingJSObject(ccx, &XPC_WN_NoHelper_Proto_JSClass,
                                            mPrototypeJSObject,
                                            mGlobalJSObject);

        NS_ASSERTION(mPrototypeNoHelper,
                     "Failed to create prototype for wrappers w/o a helper");
    }

    return mPrototypeNoHelper;
}

static JSDHashOperator
WrappedNativeJSGCThingTracer(JSDHashTable *table, JSDHashEntryHdr *hdr,
                             uint32 number, void *arg)
{
    XPCWrappedNative* wrapper = ((Native2WrappedNativeMap::Entry*)hdr)->value;
    if(wrapper->HasExternalReference() && !wrapper->IsWrapperExpired())
    {
        JSTracer* trc = (JSTracer *)arg;
        JS_CALL_OBJECT_TRACER(trc, wrapper->GetFlatJSObject(),
                              "XPCWrappedNative::mFlatJSObject");
    }

    return JS_DHASH_NEXT;
}

// static
void
XPCWrappedNativeScope::TraceJS(JSTracer* trc, XPCJSRuntime* rt)
{
    // FIXME The lock may not be necessary during tracing as that serializes
    // access to JS runtime. See bug 380139.
    XPCAutoLock lock(rt->GetMapLock());

    // Do JS_CallTracer for all wrapped natives with external references.
    for(XPCWrappedNativeScope* cur = gScopes; cur; cur = cur->mNext)
    {
        cur->mWrappedNativeMap->Enumerate(WrappedNativeJSGCThingTracer, trc);
    }
}

struct SuspectClosure
{
    SuspectClosure(JSContext *aCx, nsCycleCollectionTraversalCallback& aCb)
        : cx(aCx), cb(aCb)
    {
    }

    JSContext* cx;
    nsCycleCollectionTraversalCallback& cb;
};

static JSDHashOperator
WrappedNativeSuspecter(JSDHashTable *table, JSDHashEntryHdr *hdr,
                       uint32 number, void *arg)
{
    SuspectClosure* closure = static_cast<SuspectClosure*>(arg);
    XPCWrappedNative* wrapper = ((Native2WrappedNativeMap::Entry*)hdr)->value;
    if(wrapper->IsValid())
    {
        NS_ASSERTION(NS_IsMainThread(), 
                     "Suspecting wrapped natives from non-main thread");

        // Only record objects that might be part of a cycle as roots, unless
        // the callback wants all traces (a debug feature).
        if(!(closure->cb.WantAllTraces()) && 
           !JS_IsAboutToBeFinalized(closure->cx, wrapper->GetFlatJSObject()))
            return JS_DHASH_NEXT;

        closure->cb.NoteRoot(nsIProgrammingLanguage::JAVASCRIPT,
                             wrapper->GetFlatJSObject(),
                             nsXPConnect::GetXPConnect());
    }

    return JS_DHASH_NEXT;
}

// static
void
XPCWrappedNativeScope::SuspectAllWrappers(XPCJSRuntime* rt, JSContext* cx,
                                          nsCycleCollectionTraversalCallback& cb)
{
    XPCAutoLock lock(rt->GetMapLock());

    SuspectClosure closure(cx, cb);
    for(XPCWrappedNativeScope* cur = gScopes; cur; cur = cur->mNext)
    {
        cur->mWrappedNativeMap->Enumerate(WrappedNativeSuspecter, &closure);
    }
}

// static
void
XPCWrappedNativeScope::FinishedMarkPhaseOfGC(JSContext* cx, XPCJSRuntime* rt)
{
    // FIXME The lock may not be necessary since we are inside JSGC_MARK_END
    // callback and GX serializes access to JS runtime. See bug 380139.
    XPCAutoLock lock(rt->GetMapLock());

    // We are in JSGC_MARK_END and JSGC_FINALIZE_END must always follow it
    // calling FinishedFinalizationPhaseOfGC and clearing gDyingScopes in
    // KillDyingScopes.
    NS_ASSERTION(gDyingScopes == nsnull,
                 "JSGC_MARK_END without JSGC_FINALIZE_END");

    XPCWrappedNativeScope* prev = nsnull;
    XPCWrappedNativeScope* cur = gScopes;

    while(cur)
    {
        XPCWrappedNativeScope* next = cur->mNext;
        if(cur->mGlobalJSObject &&
           JS_IsAboutToBeFinalized(cx, cur->mGlobalJSObject))
        {
            cur->mGlobalJSObject = nsnull;
#ifndef XPCONNECT_STANDALONE
            cur->mScriptObjectPrincipal = nsnull;
#endif
            // Move this scope from the live list to the dying list.
            if(prev)
                prev->mNext = next;
            else
                gScopes = next;
            cur->mNext = gDyingScopes;
            gDyingScopes = cur;
            cur = nsnull;
        }
        else
        {
            if(cur->mPrototypeJSObject &&
               JS_IsAboutToBeFinalized(cx, cur->mPrototypeJSObject))
            {
                cur->mPrototypeJSObject = nsnull;
            }
            if(cur->mPrototypeJSFunction &&
               JS_IsAboutToBeFinalized(cx, cur->mPrototypeJSFunction))
            {
                cur->mPrototypeJSFunction = nsnull;
            }
            if(cur->mPrototypeNoHelper &&
               JS_IsAboutToBeFinalized(cx, cur->mPrototypeNoHelper))
            {
                cur->mPrototypeNoHelper = nsnull;
            }
        }
        if(cur)
            prev = cur;
        cur = next;
    }
}

// static
void
XPCWrappedNativeScope::FinishedFinalizationPhaseOfGC(JSContext* cx)
{
    XPCJSRuntime* rt = nsXPConnect::GetRuntimeInstance();

    // FIXME The lock may not be necessary since we are inside
    // JSGC_FINALIZE_END callback and at this point GC still serializes access
    // to JS runtime. See bug 380139.
    XPCAutoLock lock(rt->GetMapLock());
    KillDyingScopes();
}

static JSDHashOperator
WrappedNativeMarker(JSDHashTable *table, JSDHashEntryHdr *hdr,
                    uint32 number, void *arg)
{
    ((Native2WrappedNativeMap::Entry*)hdr)->value->Mark();
    return JS_DHASH_NEXT;
}

// We need to explicitly mark all the protos too because some protos may be
// alive in the hashtable but not currently in use by any wrapper
static JSDHashOperator
WrappedNativeProtoMarker(JSDHashTable *table, JSDHashEntryHdr *hdr,
                         uint32 number, void *arg)
{
    ((ClassInfo2WrappedNativeProtoMap::Entry*)hdr)->value->Mark();
    return JS_DHASH_NEXT;
}

// static
void
XPCWrappedNativeScope::MarkAllWrappedNativesAndProtos()
{
    for(XPCWrappedNativeScope* cur = gScopes; cur; cur = cur->mNext)
    {
        cur->mWrappedNativeMap->Enumerate(WrappedNativeMarker, nsnull);
        cur->mWrappedNativeProtoMap->Enumerate(WrappedNativeProtoMarker, nsnull);
        cur->mMainThreadWrappedNativeProtoMap->Enumerate(WrappedNativeProtoMarker, nsnull);
    }

    DEBUG_TrackScopeTraversal();
}

#ifdef DEBUG
static JSDHashOperator
ASSERT_WrappedNativeSetNotMarked(JSDHashTable *table, JSDHashEntryHdr *hdr,
                                 uint32 number, void *arg)
{
    ((Native2WrappedNativeMap::Entry*)hdr)->value->ASSERT_SetsNotMarked();
    return JS_DHASH_NEXT;
}

static JSDHashOperator
ASSERT_WrappedNativeProtoSetNotMarked(JSDHashTable *table, JSDHashEntryHdr *hdr,
                                      uint32 number, void *arg)
{
    ((ClassInfo2WrappedNativeProtoMap::Entry*)hdr)->value->ASSERT_SetNotMarked();
    return JS_DHASH_NEXT;
}

// static
void
XPCWrappedNativeScope::ASSERT_NoInterfaceSetsAreMarked()
{
    for(XPCWrappedNativeScope* cur = gScopes; cur; cur = cur->mNext)
    {
        cur->mWrappedNativeMap->Enumerate(
            ASSERT_WrappedNativeSetNotMarked, nsnull);
        cur->mWrappedNativeProtoMap->Enumerate(
            ASSERT_WrappedNativeProtoSetNotMarked, nsnull);
        cur->mMainThreadWrappedNativeProtoMap->Enumerate(
            ASSERT_WrappedNativeProtoSetNotMarked, nsnull);
    }
}
#endif

static JSDHashOperator
WrappedNativeTearoffSweeper(JSDHashTable *table, JSDHashEntryHdr *hdr,
                            uint32 number, void *arg)
{
    ((Native2WrappedNativeMap::Entry*)hdr)->value->SweepTearOffs();
    return JS_DHASH_NEXT;
}

// static
void
XPCWrappedNativeScope::SweepAllWrappedNativeTearOffs()
{
    for(XPCWrappedNativeScope* cur = gScopes; cur; cur = cur->mNext)
        cur->mWrappedNativeMap->Enumerate(WrappedNativeTearoffSweeper, nsnull);

    DEBUG_TrackScopeTraversal();
}

// static
void
XPCWrappedNativeScope::KillDyingScopes()
{
    // always called inside the lock!
    XPCWrappedNativeScope* cur = gDyingScopes;
    while(cur)
    {
        XPCWrappedNativeScope* next = cur->mNext;
        delete cur;
        cur = next;
    }
    gDyingScopes = nsnull;
}

struct ShutdownData
{
    ShutdownData(JSContext* acx)
        : cx(acx), wrapperCount(0),
          sharedProtoCount(0), nonSharedProtoCount(0) {}
    JSContext* cx;
    int wrapperCount;
    int sharedProtoCount;
    int nonSharedProtoCount;
};

static JSDHashOperator
WrappedNativeShutdownEnumerator(JSDHashTable *table, JSDHashEntryHdr *hdr,
                                uint32 number, void *arg)
{
    ShutdownData* data = (ShutdownData*) arg;
    XPCWrappedNative* wrapper = ((Native2WrappedNativeMap::Entry*)hdr)->value;

    if(wrapper->IsValid())
    {
        if(wrapper->HasProto() && !wrapper->HasSharedProto())
            data->nonSharedProtoCount++;
        wrapper->SystemIsBeingShutDown(data->cx);
        data->wrapperCount++;
    }
    return JS_DHASH_REMOVE;
}

static JSDHashOperator
WrappedNativeProtoShutdownEnumerator(JSDHashTable *table, JSDHashEntryHdr *hdr,
                                     uint32 number, void *arg)
{
    ShutdownData* data = (ShutdownData*) arg;
    ((ClassInfo2WrappedNativeProtoMap::Entry*)hdr)->value->
        SystemIsBeingShutDown(data->cx);
    data->sharedProtoCount++;
    return JS_DHASH_REMOVE;
}

//static
void
XPCWrappedNativeScope::SystemIsBeingShutDown(JSContext* cx)
{
    DEBUG_TrackScopeTraversal();
    DEBUG_TrackScopeShutdown();

    int liveScopeCount = 0;

    ShutdownData data(cx);

    XPCWrappedNativeScope* cur;

    // First move all the scopes to the dying list.

    cur = gScopes;
    while(cur)
    {
        XPCWrappedNativeScope* next = cur->mNext;
        cur->mNext = gDyingScopes;
        gDyingScopes = cur;
        cur = next;
        liveScopeCount++;
    }
    gScopes = nsnull;

    // Walk the unified dying list and call shutdown on all wrappers and protos

    for(cur = gDyingScopes; cur; cur = cur->mNext)
    {
        // Give the Components object a chance to try to clean up.
        if(cur->mComponents)
            cur->mComponents->SystemIsBeingShutDown();

        // Walk the protos first. Wrapper shutdown can leave dangling
        // proto pointers in the proto map.
        cur->mWrappedNativeProtoMap->
                Enumerate(WrappedNativeProtoShutdownEnumerator,  &data);
        cur->mMainThreadWrappedNativeProtoMap->
                Enumerate(WrappedNativeProtoShutdownEnumerator,  &data);
        cur->mWrappedNativeMap->
                Enumerate(WrappedNativeShutdownEnumerator,  &data);
    }

    // Now it is safe to kill all the scopes.
    KillDyingScopes();

#ifdef XPC_DUMP_AT_SHUTDOWN
    if(data.wrapperCount)
        printf("deleting nsXPConnect  with %d live XPCWrappedNatives\n",
               data.wrapperCount);
    if(data.sharedProtoCount + data.nonSharedProtoCount)
        printf("deleting nsXPConnect  with %d live XPCWrappedNativeProtos (%d shared)\n",
               data.sharedProtoCount + data.nonSharedProtoCount,
               data.sharedProtoCount);
    if(liveScopeCount)
        printf("deleting nsXPConnect  with %d live XPCWrappedNativeScopes\n",
               liveScopeCount);
#endif
}


/***************************************************************************/

static
XPCWrappedNativeScope*
GetScopeOfObject(JSObject* obj)
{
    nsISupports* supports;
    JSClass* clazz = STOBJ_GET_CLASS(obj);
    JSBool isWrapper = IS_WRAPPER_CLASS(clazz);

    if(isWrapper && IS_SLIM_WRAPPER_OBJECT(obj))
        return GetSlimWrapperProto(obj)->GetScope();

    if(!isWrapper || !(supports = (nsISupports*) xpc_GetJSPrivate(obj)))
    {
#ifdef DEBUG
        {
            if(!(~clazz->flags & (JSCLASS_HAS_PRIVATE |
                                  JSCLASS_PRIVATE_IS_NSISUPPORTS)) &&
               (supports = (nsISupports*) xpc_GetJSPrivate(obj)) &&
               !XPCNativeWrapper::IsNativeWrapperClass(clazz))
            {
                nsCOMPtr<nsIXPConnectWrappedNative> iface =
                    do_QueryInterface(supports);

                NS_ASSERTION(!iface, "Uh, how'd this happen?");
            }
        }
#endif

        return nsnull;
    }

#ifdef DEBUG
    {
        nsCOMPtr<nsIXPConnectWrappedNative> iface = do_QueryInterface(supports);

        NS_ASSERTION(iface, "Uh, how'd this happen?");
    }
#endif

    // obj is one of our nsXPConnectWrappedNative objects.
    return ((XPCWrappedNative*)supports)->GetScope();
}


#ifdef DEBUG
void DEBUG_CheckForComponentsInScope(JSContext* cx, JSObject* obj,
                                     JSBool OKIfNotInitialized,
                                     XPCJSRuntime* runtime)
{
    if(OKIfNotInitialized)
        return;

    if(!(JS_GetOptions(cx) & JSOPTION_PRIVATE_IS_NSISUPPORTS))
        return;

    const char* name = runtime->GetStringName(XPCJSRuntime::IDX_COMPONENTS);
    jsval prop;
    if(JS_LookupProperty(cx, obj, name, &prop) && !JSVAL_IS_PRIMITIVE(prop))
        return;

    // This is pretty much always bad. It usually means that native code is
    // making a callback to an interface implemented in JavaScript, but the
    // document where the JS object was created has already been cleared and the
    // global properties of that document's window are *gone*. Generally this
    // indicates a problem that should be addressed in the design and use of the
    // callback code.
#ifdef I_FOOLISHLY_WANT_TO_IGNORE_THIS_LIKE_THE_OTHER_CRAP_WE_PRINTF
    NS_WARNING("XPConnect is being called on a scope without a 'Components' property!");
#else
    NS_ERROR("XPConnect is being called on a scope without a 'Components' property!");
#endif
}
#else
#define DEBUG_CheckForComponentsInScope(ccx, obj, OKIfNotInitialized, runtime) \
    ((void)0)
#endif

// static
XPCWrappedNativeScope*
XPCWrappedNativeScope::FindInJSObjectScope(JSContext* cx, JSObject* obj,
                                           JSBool OKIfNotInitialized,
                                           XPCJSRuntime* runtime)
{
    XPCWrappedNativeScope* scope;

    if(!obj)
        return nsnull;

    // If this object is itself a wrapped native then we can get the
    // scope directly.

    scope = GetScopeOfObject(obj);
    if(scope)
        return scope;

    // Else we'll have to look up the parent chain to get the scope

    obj = JS_GetGlobalForObject(cx, obj);

    if(!runtime)
    {
        runtime = nsXPConnect::GetRuntimeInstance();
        NS_ASSERTION(runtime, "This should never be null!");
    }

    // XXX We are assuming that the scope count is low enough that traversing
    // the linked list is more reasonable then doing a hashtable lookup.
    XPCWrappedNativeScope* found = nsnull;
    {   // scoped lock
        XPCAutoLock lock(runtime->GetMapLock());

        DEBUG_TrackScopeTraversal();

        for(XPCWrappedNativeScope* cur = gScopes; cur; cur = cur->mNext)
        {
            if(obj == cur->GetGlobalJSObject())
            {
                found = cur;
                break;
            }
        }
    }

    if(found) {
        // This cannot be called within the map lock!
        DEBUG_CheckForComponentsInScope(cx, obj, OKIfNotInitialized, runtime);
        return found;
    }

    // Failure to find the scope is only OK if the caller told us it might fail.
    // This flag would only be set in the call from
    // XPCWrappedNativeScope::GetNewOrUsed
    NS_ASSERTION(OKIfNotInitialized, "No scope has this global object!");
    return nsnull;
}

/***************************************************************************/

static JSDHashOperator
WNProtoSecPolicyClearer(JSDHashTable *table, JSDHashEntryHdr *hdr,
                        uint32 number, void *arg)
{
    XPCWrappedNativeProto* proto =
        ((ClassInfo2WrappedNativeProtoMap::Entry*)hdr)->value;
    *(proto->GetSecurityInfoAddr()) = nsnull;
    return JS_DHASH_NEXT;
}

static JSDHashOperator
WNSecPolicyClearer(JSDHashTable *table, JSDHashEntryHdr *hdr,
                    uint32 number, void *arg)
{
    XPCWrappedNative* wrapper = ((Native2WrappedNativeMap::Entry*)hdr)->value;
    if(wrapper->HasProto() && !wrapper->HasSharedProto())
        *(wrapper->GetProto()->GetSecurityInfoAddr()) = nsnull;
    return JS_DHASH_NEXT;
}

// static
nsresult
XPCWrappedNativeScope::ClearAllWrappedNativeSecurityPolicies(XPCCallContext& ccx)
{
    // Hold the lock throughout.
    XPCAutoLock lock(ccx.GetRuntime()->GetMapLock());

    for(XPCWrappedNativeScope* cur = gScopes; cur; cur = cur->mNext)
    {
        cur->mWrappedNativeProtoMap->Enumerate(WNProtoSecPolicyClearer, nsnull);
        cur->mMainThreadWrappedNativeProtoMap->Enumerate(WNProtoSecPolicyClearer, nsnull);
        cur->mWrappedNativeMap->Enumerate(WNSecPolicyClearer, nsnull);
    }

    DEBUG_TrackScopeTraversal();

    return NS_OK;
}

static JSDHashOperator
WNProtoRemover(JSDHashTable *table, JSDHashEntryHdr *hdr,
               uint32 number, void *arg)
{
    XPCWrappedNativeProtoMap* detachedMap = (XPCWrappedNativeProtoMap*)arg;
    
    XPCWrappedNativeProto* proto = (XPCWrappedNativeProto*)
        ((ClassInfo2WrappedNativeProtoMap::Entry*)hdr)->value;

    detachedMap->Add(proto);

    return JS_DHASH_REMOVE;
}

void
XPCWrappedNativeScope::RemoveWrappedNativeProtos()
{
    XPCAutoLock al(mRuntime->GetMapLock());
    
    mWrappedNativeProtoMap->Enumerate(WNProtoRemover, 
        GetRuntime()->GetDetachedWrappedNativeProtoMap());
    mMainThreadWrappedNativeProtoMap->Enumerate(WNProtoRemover, 
        GetRuntime()->GetDetachedWrappedNativeProtoMap());
}

/***************************************************************************/

// static
void
XPCWrappedNativeScope::DebugDumpAllScopes(PRInt16 depth)
{
#ifdef DEBUG
    depth-- ;

    // get scope count.
    int count = 0;
    XPCWrappedNativeScope* cur;
    for(cur = gScopes; cur; cur = cur->mNext)
        count++ ;

    XPC_LOG_ALWAYS(("chain of %d XPCWrappedNativeScope(s)", count));
    XPC_LOG_INDENT();
        XPC_LOG_ALWAYS(("gDyingScopes @ %x", gDyingScopes));
        if(depth)
            for(cur = gScopes; cur; cur = cur->mNext)
                cur->DebugDump(depth);
    XPC_LOG_OUTDENT();
#endif
}

#ifdef DEBUG
static JSDHashOperator
WrappedNativeMapDumpEnumerator(JSDHashTable *table, JSDHashEntryHdr *hdr,
                               uint32 number, void *arg)
{
    ((Native2WrappedNativeMap::Entry*)hdr)->value->DebugDump(*(PRInt16*)arg);
    return JS_DHASH_NEXT;
}
static JSDHashOperator
WrappedNativeProtoMapDumpEnumerator(JSDHashTable *table, JSDHashEntryHdr *hdr,
                                    uint32 number, void *arg)
{
    ((ClassInfo2WrappedNativeProtoMap::Entry*)hdr)->value->DebugDump(*(PRInt16*)arg);
    return JS_DHASH_NEXT;
}
#endif

void
XPCWrappedNativeScope::DebugDump(PRInt16 depth)
{
#ifdef DEBUG
    depth-- ;
    XPC_LOG_ALWAYS(("XPCWrappedNativeScope @ %x", this));
    XPC_LOG_INDENT();
        XPC_LOG_ALWAYS(("mRuntime @ %x", mRuntime));
        XPC_LOG_ALWAYS(("mNext @ %x", mNext));
        XPC_LOG_ALWAYS(("mComponents @ %x", mComponents));
        XPC_LOG_ALWAYS(("mGlobalJSObject @ %x", mGlobalJSObject));
        XPC_LOG_ALWAYS(("mPrototypeJSObject @ %x", mPrototypeJSObject));
        XPC_LOG_ALWAYS(("mPrototypeJSFunction @ %x", mPrototypeJSFunction));
        XPC_LOG_ALWAYS(("mPrototypeNoHelper @ %x", mPrototypeNoHelper));

        XPC_LOG_ALWAYS(("mWrappedNativeMap @ %x with %d wrappers(s)", \
                         mWrappedNativeMap, \
                         mWrappedNativeMap ? mWrappedNativeMap->Count() : 0));
        // iterate contexts...
        if(depth && mWrappedNativeMap && mWrappedNativeMap->Count())
        {
            XPC_LOG_INDENT();
            mWrappedNativeMap->Enumerate(WrappedNativeMapDumpEnumerator, &depth);
            XPC_LOG_OUTDENT();
        }

        XPC_LOG_ALWAYS(("mWrappedNativeProtoMap @ %x with %d protos(s)", \
                         mWrappedNativeProtoMap, \
                         mWrappedNativeProtoMap ? mWrappedNativeProtoMap->Count() : 0));
        // iterate contexts...
        if(depth && mWrappedNativeProtoMap && mWrappedNativeProtoMap->Count())
        {
            XPC_LOG_INDENT();
            mWrappedNativeProtoMap->Enumerate(WrappedNativeProtoMapDumpEnumerator, &depth);
            XPC_LOG_OUTDENT();
        }

        XPC_LOG_ALWAYS(("mMainThreadWrappedNativeProtoMap @ %x with %d protos(s)", \
                         mMainThreadWrappedNativeProtoMap, \
                         mMainThreadWrappedNativeProtoMap ? mMainThreadWrappedNativeProtoMap->Count() : 0));
        // iterate contexts...
        if(depth && mMainThreadWrappedNativeProtoMap && mMainThreadWrappedNativeProtoMap->Count())
        {
            XPC_LOG_INDENT();
            mMainThreadWrappedNativeProtoMap->Enumerate(WrappedNativeProtoMapDumpEnumerator, &depth);
            XPC_LOG_OUTDENT();
        }
    XPC_LOG_OUTDENT();
#endif
}
