/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 sw=2 et tw=78: */
/* ***** BEGIN LICENSE BLOCK *****
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
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * The Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2006
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Johnny Stenback <jst@mozilla.org> (original author)
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
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

#include "xpcprivate.h"
#include "jsdbgapi.h"
#include "jsscript.h" // for js_ScriptClass
#include "XPCWrapper.h"
#include "jsregexp.h"
#include "nsJSPrincipals.h"

static JSBool
XPC_SJOW_AddProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp);

static JSBool
XPC_SJOW_DelProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp);

static JSBool
XPC_SJOW_GetProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp);

static JSBool
XPC_SJOW_SetProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp);

static JSBool
XPC_SJOW_Enumerate(JSContext *cx, JSObject *obj);

static JSBool
XPC_SJOW_NewResolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
                    JSObject **objp);

static JSBool
XPC_SJOW_Convert(JSContext *cx, JSObject *obj, JSType type, jsval *vp);

static void
XPC_SJOW_Finalize(JSContext *cx, JSObject *obj);

static JSBool
XPC_SJOW_CheckAccess(JSContext *cx, JSObject *obj, jsval id, JSAccessMode mode,
                     jsval *vp);

static JSBool
XPC_SJOW_Call(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
              jsval *rval);

JSBool
XPC_SJOW_Construct(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                   jsval *rval);

static JSBool
XPC_SJOW_Create(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                jsval *rval);

static JSBool
XPC_SJOW_Equality(JSContext *cx, JSObject *obj, jsval v, JSBool *bp);

static JSObject *
XPC_SJOW_Iterator(JSContext *cx, JSObject *obj, JSBool keysonly);

static JSObject *
XPC_SJOW_WrappedObject(JSContext *cx, JSObject *obj);

static inline
JSBool
ThrowException(nsresult ex, JSContext *cx)
{
  XPCThrower::Throw(ex, cx);

  return JS_FALSE;
}

// Find the subject and object principal. The argument
// subjectPrincipal can be null if the caller doesn't care about the
// subject principal, and secMgr can also be null if the caller
// doesn't need the security manager.
static nsresult
FindPrincipals(JSContext *cx, JSObject *obj, nsIPrincipal **objectPrincipal,
               nsIPrincipal **subjectPrincipal,
               nsIScriptSecurityManager **secMgr)
{
  XPCCallContext ccx(JS_CALLER, cx);

  if (!ccx.IsValid()) {
    return NS_ERROR_UNEXPECTED;
  }

  nsIScriptSecurityManager *ssm = XPCWrapper::GetSecurityManager();

  if (subjectPrincipal) {
    NS_IF_ADDREF(*subjectPrincipal = ssm->GetCxSubjectPrincipal(cx));
  }

  ssm->GetObjectPrincipal(cx, obj, objectPrincipal);

  if (secMgr) {
    NS_ADDREF(*secMgr = ssm);
  }

  return *objectPrincipal ? NS_OK : NS_ERROR_XPC_SECURITY_MANAGER_VETO;
}

static PRBool
CanCallerAccess(JSContext *cx, JSObject *unsafeObj)
{
  nsCOMPtr<nsIPrincipal> subjPrincipal, objPrincipal;
  nsCOMPtr<nsIScriptSecurityManager> ssm;
  nsresult rv = FindPrincipals(cx, unsafeObj, getter_AddRefs(objPrincipal),
                               getter_AddRefs(subjPrincipal),
                               getter_AddRefs(ssm));
  if (NS_FAILED(rv)) {
    return ThrowException(rv, cx);
  }

  // Assume that we're trusted if there's no running code.
  if (!subjPrincipal) {
    return PR_TRUE;
  }

  PRBool subsumes;
  rv = subjPrincipal->Subsumes(objPrincipal, &subsumes);

  if (NS_FAILED(rv) || !subsumes) {
    PRBool enabled = PR_FALSE;
    rv = ssm->IsCapabilityEnabled("UniversalXPConnect", &enabled);
    if (NS_FAILED(rv)) {
      return ThrowException(rv, cx);
    }

    if (!enabled) {
      return ThrowException(NS_ERROR_XPC_SECURITY_MANAGER_VETO, cx);
    }
  }

  return PR_TRUE;
}

// Reserved slot indexes on safe wrappers.

// Boolean value, initialized to false on object creation and true
// only while we're resolving a property on the object.
#define XPC_SJOW_SLOT_IS_RESOLVING           0

// Slot for holding on to the principal to use if a principal other
// than that of the unsafe object is desired for this wrapper
// (nsIPrincipal, strong reference).
#define XPC_SJOW_SLOT_PRINCIPAL              1


// Returns a weak reference.
static nsIPrincipal *
FindObjectPrincipals(JSContext *cx, JSObject *safeObj, JSObject *innerObj)
{
  // Check if we have a cached principal first.
  jsval v;
  if (!JS_GetReservedSlot(cx, safeObj, XPC_SJOW_SLOT_PRINCIPAL, &v)) {
    return nsnull;
  }

  if (!JSVAL_IS_VOID(v)) {
    // Found one! No need to do any more refcounting.
    return static_cast<nsIPrincipal *>(JSVAL_TO_PRIVATE(v));
  }

  nsCOMPtr<nsIPrincipal> objPrincipal;
  nsresult rv = FindPrincipals(cx, innerObj, getter_AddRefs(objPrincipal), nsnull,
                               nsnull);
  if (NS_FAILED(rv)) {
    return nsnull;
  }

  if (!JS_SetReservedSlot(cx, safeObj, XPC_SJOW_SLOT_PRINCIPAL,
                          PRIVATE_TO_JSVAL(objPrincipal.get()))) {
    return nsnull;
  }

  // The wrapper owns the principal now.
  return objPrincipal.forget().get();
}


// JS class for XPCSafeJSObjectWrapper (and this doubles as the
// constructor for XPCSafeJSObjectWrapper for the moment too...)

JSExtendedClass sXPC_SJOW_JSClass = {
  // JSClass (JSExtendedClass.base) initialization
  { "XPCSafeJSObjectWrapper",
    JSCLASS_NEW_RESOLVE | JSCLASS_IS_EXTENDED |
    JSCLASS_HAS_RESERVED_SLOTS(XPCWrapper::sNumSlots + 3),
    XPC_SJOW_AddProperty, XPC_SJOW_DelProperty,
    XPC_SJOW_GetProperty, XPC_SJOW_SetProperty,
    XPC_SJOW_Enumerate,   (JSResolveOp)XPC_SJOW_NewResolve,
    XPC_SJOW_Convert,     XPC_SJOW_Finalize,
    nsnull,               XPC_SJOW_CheckAccess,
    XPC_SJOW_Call,        XPC_SJOW_Create,
    nsnull,               nsnull,
    nsnull,               nsnull
  },
  // JSExtendedClass initialization
  XPC_SJOW_Equality,
  nsnull, // outerObject
  nsnull, // innerObject
  XPC_SJOW_Iterator,
  XPC_SJOW_WrappedObject,
  JSCLASS_NO_RESERVED_MEMBERS
};

static JSBool
XPC_SJOW_toString(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                  jsval *rval);


// Wrap a JS value in a safe wrapper of a function wrapper if
// needed. Note that rval must point to something rooted when calling
// this function.
static JSBool
WrapJSValue(JSContext *cx, JSObject *obj, jsval val, jsval *rval)
{
  JSBool ok = JS_TRUE;

  if (JSVAL_IS_PRIMITIVE(val)) {
    *rval = val;
  } else {
    // Construct a new safe wrapper. Note that it doesn't matter what
    // parent we pass in here, the construct hook will ensure we get
    // the right parent for the wrapper.
    JSObject *safeObj =
      ::JS_ConstructObjectWithArguments(cx, &sXPC_SJOW_JSClass.base, nsnull,
                                        nsnull, 1, &val);
    if (!safeObj) {
      return JS_FALSE;
    }

    // Set *rval to safeObj here to ensure it doesn't get collected in
    // any of the code below.
    *rval = OBJECT_TO_JSVAL(safeObj);

    if (JS_GetGlobalForObject(cx, obj) != JS_GetGlobalForObject(cx, safeObj)) {
      // Check to see if the new object we just wrapped is accessible
      // from the unsafe object we got the new object through. If not,
      // force the new wrapper to use the principal of the unsafe
      // object we got the new object from.
      nsCOMPtr<nsIPrincipal> srcObjPrincipal;
      nsCOMPtr<nsIPrincipal> subjPrincipal;
      nsCOMPtr<nsIPrincipal> valObjPrincipal;

      nsresult rv = FindPrincipals(cx, obj, getter_AddRefs(srcObjPrincipal),
                                   getter_AddRefs(subjPrincipal), nsnull);
      if (NS_FAILED(rv)) {
        return ThrowException(rv, cx);
      }

      rv = FindPrincipals(cx, JSVAL_TO_OBJECT(val),
                          getter_AddRefs(valObjPrincipal), nsnull, nsnull);
      if (NS_FAILED(rv)) {
        return ThrowException(rv, cx);
      }

      PRBool subsumes = PR_FALSE;
      rv = srcObjPrincipal->Subsumes(valObjPrincipal, &subsumes);
      if (NS_FAILED(rv)) {
        return ThrowException(rv, cx);
      }

      // If the subject can access both the source and object principals, then
      // don't bother forcing the principal below.
      if (!subsumes && subjPrincipal) {
        PRBool subjSubsumes = PR_FALSE;
        rv = subjPrincipal->Subsumes(srcObjPrincipal, &subjSubsumes);
        if (NS_SUCCEEDED(rv) && subjSubsumes) {
          rv = subjPrincipal->Subsumes(valObjPrincipal, &subjSubsumes);
          if (NS_SUCCEEDED(rv) && subjSubsumes) {
            subsumes = PR_TRUE;
          }
        }
      }

      if (!subsumes) {
        // The unsafe object we got the new object from can not access
        // the new object, force the wrapper we just created to use
        // the principal of the unsafe object to prevent users of the
        // new object wrapper from evaluating code through the new
        // wrapper with the principal of the new object.
        if (!::JS_SetReservedSlot(cx, safeObj, XPC_SJOW_SLOT_PRINCIPAL,
                                  PRIVATE_TO_JSVAL(srcObjPrincipal.get()))) {
          return JS_FALSE;
        }

        // Pass on ownership of the new object principal to the
        // wrapper.
        nsIPrincipal *tmp = nsnull;
        srcObjPrincipal.swap(tmp);
      }
    }
  }

  return ok;
}

static inline JSObject *
FindSafeObject(JSObject *obj)
{
  while (STOBJ_GET_CLASS(obj) != &sXPC_SJOW_JSClass.base) {
    obj = STOBJ_GET_PROTO(obj);

    if (!obj) {
      break;
    }
  }

  return obj;
}

PRBool
IsXPCSafeJSObjectWrapperClass(JSClass *clazz)
{
  return clazz == &sXPC_SJOW_JSClass.base;
}

static inline JSObject *
GetUnsafeObject(JSObject *obj)
{
  obj = FindSafeObject(obj);

  if (!obj) {
    return nsnull;
  }

  return STOBJ_GET_PARENT(obj);
}

JSObject *
XPC_SJOW_GetUnsafeObject(JSObject *obj)
{
  return GetUnsafeObject(obj);
}

static jsval
UnwrapJSValue(jsval val)
{
  if (JSVAL_IS_PRIMITIVE(val)) {
    return val;
  }

  JSObject *unsafeObj = GetUnsafeObject(JSVAL_TO_OBJECT(val));
  if (unsafeObj) {
    return OBJECT_TO_JSVAL(unsafeObj);
  }

  return val;
}

static JSBool
XPC_SJOW_AddProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
  // The constructor and toString properties needs to live on the safe
  // wrapper.
  if (id == GetRTStringByIndex(cx, XPCJSRuntime::IDX_CONSTRUCTOR) ||
      id == GetRTStringByIndex(cx, XPCJSRuntime::IDX_TO_STRING)) {
    return JS_TRUE;
  }

  obj = FindSafeObject(obj);
  NS_ASSERTION(obj != nsnull, "FindSafeObject() returned null in class hook!");

  // Do nothing here if we're in the middle of resolving a property on
  // this safe wrapper.
  jsval isResolving;
  JSBool ok = ::JS_GetReservedSlot(cx, obj, XPC_SJOW_SLOT_IS_RESOLVING,
                                   &isResolving);
  if (!ok || HAS_FLAGS(isResolving, FLAG_RESOLVING)) {
    return ok;
  }

  JSObject *unsafeObj = GetUnsafeObject(obj);
  if (!unsafeObj) {
    return ThrowException(NS_ERROR_UNEXPECTED, cx);
  }

  // Check that the caller can access the unsafe object.
  if (!CanCallerAccess(cx, unsafeObj)) {
    // CanCallerAccess() already threw for us.
    return JS_FALSE;
  }

  return XPCWrapper::AddProperty(cx, obj, JS_FALSE, unsafeObj, id, vp);
}

static JSBool
XPC_SJOW_DelProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
  JSObject *unsafeObj = GetUnsafeObject(obj);
  if (!unsafeObj) {
    return ThrowException(NS_ERROR_UNEXPECTED, cx);
  }

  // Check that the caller can access the unsafe object.
  if (!CanCallerAccess(cx, unsafeObj)) {
    // CanCallerAccess() already threw for us.
    return JS_FALSE;
  }

  return XPCWrapper::DelProperty(cx, unsafeObj, id, vp);
}

NS_STACK_CLASS class SafeCallGuard {
public:
  SafeCallGuard(JSContext *cx, nsIPrincipal *principal)
    : cx(cx) {
    nsIScriptSecurityManager *ssm = XPCWrapper::GetSecurityManager();
    if (ssm) {
      // Note: We pass null as the target frame pointer because we know that
      // we're about to set aside the frame chain.
      nsresult rv = ssm->PushContextPrincipal(cx, nsnull, principal);
      if (NS_FAILED(rv)) {
        NS_WARNING("Not allowing call because we're out of memory");
        JS_ReportOutOfMemory(cx);
        this->cx = nsnull;
        return;
      }
    }

    js_SaveAndClearRegExpStatics(cx, &statics, &tvr);
    fp = JS_SaveFrameChain(cx);
    options =
      JS_SetOptions(cx, JS_GetOptions(cx) | JSOPTION_DONT_REPORT_UNCAUGHT);
  }

  JSBool ready() {
    return cx != nsnull;
  }

  ~SafeCallGuard() {
    if (cx) {
      JS_SetOptions(cx, options);
      JS_RestoreFrameChain(cx, fp);
      js_RestoreRegExpStatics(cx, &statics, &tvr);
      nsIScriptSecurityManager *ssm = XPCWrapper::GetSecurityManager();
      if (ssm) {
        ssm->PopContextPrincipal(cx);
      }
    }
  }

private:
  JSContext *cx;
  JSRegExpStatics statics;
  JSTempValueRooter tvr;
  uint32 options;
  JSStackFrame *fp;
};

static JSBool
XPC_SJOW_GetOrSetProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp,
                          JSBool aIsSet)
{
  // We resolve toString to a function in our resolve hook.
  if (id == GetRTStringByIndex(cx, XPCJSRuntime::IDX_TO_STRING)) {
    return JS_TRUE;
  }

  obj = FindSafeObject(obj);
  NS_ASSERTION(obj != nsnull, "FindSafeObject() returned null in class hook!");

  JSObject *unsafeObj = GetUnsafeObject(obj);
  if (!unsafeObj) {
    return ThrowException(NS_ERROR_UNEXPECTED, cx);
  }

  // Check that the caller can access the unsafe object.
  if (!CanCallerAccess(cx, unsafeObj)) {
    // CanCallerAccess() already threw for us.
    return JS_FALSE;
  }

  {
    SafeCallGuard guard(cx, FindObjectPrincipals(cx, obj, unsafeObj));
    if (!guard.ready()) {
      return JS_FALSE;
    }

    jsid interned_id;
    if (!JS_ValueToId(cx, id, &interned_id)) {
      return JS_FALSE;
    }

    if (aIsSet) {
      *vp = UnwrapJSValue(*vp);
    }

    JSBool ok = aIsSet
                ? JS_SetPropertyById(cx, unsafeObj, interned_id, vp)
                : JS_GetPropertyById(cx, unsafeObj, interned_id, vp);
    if (!ok) {
      return JS_FALSE;
    }
  }

  return WrapJSValue(cx, obj, *vp, vp);
}

static JSBool
XPC_SJOW_GetProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
  return XPC_SJOW_GetOrSetProperty(cx, obj, id, vp, PR_FALSE);
}

static JSBool
XPC_SJOW_SetProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
  return XPC_SJOW_GetOrSetProperty(cx, obj, id, vp, PR_TRUE);
}

static JSBool
XPC_SJOW_Enumerate(JSContext *cx, JSObject *obj)
{
  obj = FindSafeObject(obj);
  NS_ASSERTION(obj != nsnull, "FindSafeObject() returned null in class hook!");

  // We are being notified of a for-in loop or similar operation on
  // this XPCSafeJSObjectWrapper. Forward to the correct high-level
  // object hook, OBJ_ENUMERATE on the unsafe object, called via the
  // JS_Enumerate API.  Then reflect properties named by the
  // enumerated identifiers from the unsafe object to the safe
  // wrapper.

  JSObject *unsafeObj = GetUnsafeObject(obj);
  if (!unsafeObj) {
    return JS_TRUE;
  }

  // Check that the caller can access the unsafe object.
  if (!CanCallerAccess(cx, unsafeObj)) {
    // CanCallerAccess() already threw for us.
    return JS_FALSE;
  }

  // Since we enumerate using JS_Enumerate() on the unsafe object here
  // we don't need to do a security check since JS_Enumerate() will
  // look up unsafeObj.__iterator__ and if we don't have permission to
  // access that, it'll throw and we'll be safe.

  return XPCWrapper::Enumerate(cx, obj, unsafeObj);
}

static JSBool
XPC_SJOW_NewResolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
                    JSObject **objp)
{
  obj = FindSafeObject(obj);
  NS_ASSERTION(obj != nsnull, "FindSafeObject() returned null in class hook!");

  JSObject *unsafeObj = GetUnsafeObject(obj);
  if (!unsafeObj) {
    // No unsafe object, nothing to resolve here.

    return JS_TRUE;
  }

  // Check that the caller can access the unsafe object.
  if (!CanCallerAccess(cx, unsafeObj)) {
    // CanCallerAccess() already threw for us.
    return JS_FALSE;
  }

  // Resolve toString specially.
  if (id == GetRTStringByIndex(cx, XPCJSRuntime::IDX_TO_STRING)) {
    *objp = obj;
    return JS_DefineFunction(cx, obj, "toString",
                             XPC_SJOW_toString, 0, 0) != nsnull;
  }

  return XPCWrapper::NewResolve(cx, obj, JS_FALSE, unsafeObj, id, flags, objp);
}

static JSBool
XPC_SJOW_Convert(JSContext *cx, JSObject *obj, JSType type, jsval *vp)
{
  NS_ASSERTION(type != JSTYPE_STRING, "toString failed us");
  return JS_TRUE;
}

static void
XPC_SJOW_Finalize(JSContext *cx, JSObject *obj)
{
  // Release the reference to the cached principal if we have one.
  jsval v;
  if (::JS_GetReservedSlot(cx, obj, XPC_SJOW_SLOT_PRINCIPAL, &v) &&
      !JSVAL_IS_VOID(v)) {
    nsIPrincipal *principal = (nsIPrincipal *)JSVAL_TO_PRIVATE(v);

    NS_RELEASE(principal);
  }
}

static JSBool
XPC_SJOW_CheckAccess(JSContext *cx, JSObject *obj, jsval id,
                     JSAccessMode mode, jsval *vp)
{
  // Prevent setting __proto__ on an XPCSafeJSObjectWrapper
  if ((mode & JSACC_WATCH) == JSACC_PROTO && (mode & JSACC_WRITE)) {
    return ThrowException(NS_ERROR_XPC_SECURITY_MANAGER_VETO, cx);
  }

  // Forward to the checkObjectAccess hook in the runtime, if any.
  JSSecurityCallbacks *callbacks = JS_GetSecurityCallbacks(cx);
  if (callbacks && callbacks->checkObjectAccess &&
      !callbacks->checkObjectAccess(cx, obj, id, mode, vp)) {
    return JS_FALSE;
  }

  JSObject *unsafeObj = GetUnsafeObject(obj);
  if (!unsafeObj) {
    return JS_TRUE;
  }

  // Forward the unsafe object to the checkObjectAccess hook in the
  // runtime too, if any.
  if (callbacks && callbacks->checkObjectAccess &&
      !callbacks->checkObjectAccess(cx, unsafeObj, id, mode, vp)) {
    return JS_FALSE;
  }

  JSClass *clazz = STOBJ_GET_CLASS(unsafeObj);
  return !clazz->checkAccess ||
    clazz->checkAccess(cx, unsafeObj, id, mode, vp);
}

static JSBool
XPC_SJOW_Call(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
              jsval *rval)
{
  JSObject *tmp = FindSafeObject(obj);
  JSObject *unsafeObj, *callThisObj = nsnull;

  if (tmp) {
    // A function wrapped in an XPCSafeJSObjectWrapper is being called
    // directly (i.e. safeObj.fun()), set obj to be the safe object
    // wrapper. In this case, the "this" object used when calling the
    // function will be the unsafe object gotten off of the safe
    // object.
    obj = tmp;
  } else {
    // A function wrapped in an XPCSafeJSObjectWrapper is being called
    // indirectly off of an object that's not a safe wrapper
    // (i.e. foo.bar = safeObj.fun; foo.bar()), set obj to be the safe
    // wrapper for the function, and use the object passed in as the
    // "this" object when calling the function.
    callThisObj = obj;

    // Check that the caller can access the object we're about to pass
    // in as "this" for the call we're about to make.
    if (!CanCallerAccess(cx, callThisObj)) {
      // CanCallerAccess() already threw for us.
      return JS_FALSE;
    }

    obj = FindSafeObject(JSVAL_TO_OBJECT(argv[-2]));

    if (!obj) {
      return ThrowException(NS_ERROR_INVALID_ARG, cx);
    }
  }

  unsafeObj = GetUnsafeObject(obj);
  if (!unsafeObj) {
    return ThrowException(NS_ERROR_UNEXPECTED, cx);
  }

  if (!callThisObj) {
    callThisObj = unsafeObj;
  }

  JSObject *safeObj = JSVAL_TO_OBJECT(argv[-2]);
  JSObject *funToCall = GetUnsafeObject(safeObj);

  if (!funToCall) {
    // Someone has called XPCSafeJSObjectWrapper.prototype() causing
    // us to find a safe object wrapper without an unsafeObject as
    // its parent. That call shouldn't do anything, so bail here.
    return JS_TRUE;
  }

  // Check that the caller can access the unsafe object on which the
  // call is being made, and the actual function we're about to call.
  if (!CanCallerAccess(cx, unsafeObj) || !CanCallerAccess(cx, funToCall)) {
    // CanCallerAccess() already threw for us.
    return JS_FALSE;
  }

  {
    SafeCallGuard guard(cx, FindObjectPrincipals(cx, safeObj, funToCall));

    for (uintN i = 0; i < argc; ++i) {
      argv[i] = UnwrapJSValue(argv[i]);
    }

    if (!JS_CallFunctionValue(cx, callThisObj, OBJECT_TO_JSVAL(funToCall),
                              argc, argv, rval)) {
      return JS_FALSE;
    }
  }

  return WrapJSValue(cx, obj, *rval, rval);
}

JSBool
XPC_SJOW_Construct(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                   jsval *rval)
{
  if (argc < 1) {
    return ThrowException(NS_ERROR_XPC_NOT_ENOUGH_ARGS, cx);
  }

  // |obj| almost always has the wrong proto and parent so we have to create
  // our own object anyway.  Set |obj| to null so we don't use it by accident.
  obj = nsnull;

  if (JSVAL_IS_PRIMITIVE(argv[0])) {
    JSStackFrame *fp = nsnull;
    if (JS_FrameIterator(cx, &fp) && JS_IsConstructorFrame(cx, fp)) {
      return ThrowException(NS_ERROR_ILLEGAL_VALUE, cx);
    }

    *rval = argv[0];
    return JS_TRUE;
  }

  JSObject *objToWrap = JSVAL_TO_OBJECT(argv[0]);

  // Prevent script created Script objects from ever being wrapped
  // with XPCSafeJSObjectWrapper, and never let the eval function
  // object be directly wrapped.

  if (STOBJ_GET_CLASS(objToWrap) == &js_ScriptClass ||
      (::JS_ObjectIsFunction(cx, objToWrap) &&
       ::JS_GetFunctionNative(cx, ::JS_ValueToFunction(cx, argv[0])) ==
       XPCWrapper::sEvalNative)) {
    return ThrowException(NS_ERROR_INVALID_ARG, cx);
  }

  SLIM_LOG_WILL_MORPH(cx, objToWrap);
  if(IS_SLIM_WRAPPER(objToWrap) && !MorphSlimWrapper(cx, objToWrap)) {
    return ThrowException(NS_ERROR_FAILURE, cx);
  }

  // Check that the caller can access the unsafe object.
  if (!CanCallerAccess(cx, objToWrap)) {
    // CanCallerAccess() already threw for us.
    return JS_FALSE;
  }

  JSObject *unsafeObj = GetUnsafeObject(objToWrap);

  if (unsafeObj) {
    // We're asked to wrap an already wrapped object. Re-wrap the
    // object wrapped by the given wrapper.

    objToWrap = unsafeObj;
  }

  // Don't use the object the JS engine created for us, it is in most
  // cases incorectly parented and has a proto from the wrong scope.
  JSObject *wrapperObj =
    ::JS_NewObjectWithGivenProto(cx, &sXPC_SJOW_JSClass.base, nsnull,
                                 objToWrap);

  if (!wrapperObj) {
    // JS_NewObjectWithGivenProto already threw.
    return JS_FALSE;
  }

  if (!::JS_SetReservedSlot(cx, wrapperObj, XPC_SJOW_SLOT_IS_RESOLVING,
                            JSVAL_ZERO)) {
    return JS_FALSE;
  }

  *rval = OBJECT_TO_JSVAL(wrapperObj);

  return JS_TRUE;
}

static JSBool
XPC_SJOW_Create(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                jsval *rval)
{
  JSObject *callee = JSVAL_TO_OBJECT(argv[-2]);
  NS_ASSERTION(GetUnsafeObject(callee), "How'd we get here?");
  JSObject *unsafeObj = GetUnsafeObject(callee);

  // Check that the caller can access the unsafe object.
  if (!CanCallerAccess(cx, unsafeObj)) {
    // CanCallerAccess() already threw for us.
    return JS_FALSE;
  }

  {
    SafeCallGuard guard(cx, FindObjectPrincipals(cx, callee, unsafeObj));
    if (!guard.ready()) {
      return JS_FALSE;
    }

    if (!JS_CallFunctionValue(cx, obj, OBJECT_TO_JSVAL(callee),
                              argc, argv, rval)) {
      return JS_FALSE;
    }
  }

  return WrapJSValue(cx, callee, *rval, rval);
}

static JSBool
XPC_SJOW_Equality(JSContext *cx, JSObject *obj, jsval v, JSBool *bp)
{
  if (JSVAL_IS_PRIMITIVE(v)) {
    *bp = JS_FALSE;
  } else {
    JSObject *unsafeObj = GetUnsafeObject(obj);

    JSObject *other = JSVAL_TO_OBJECT(v);
    JSObject *otherUnsafe = GetUnsafeObject(other);

    // An object is equal to a SJOW if:
    //   - The other object is the same SJOW.
    //   - The other object is the object that this SJOW is wrapping.
    //   - The other object is a SJOW wrapping the same object as this one.
    // or
    //   - Both objects somehow wrap the same native object.
    if (obj == other || unsafeObj == other ||
        (unsafeObj && unsafeObj == otherUnsafe)) {
      *bp = JS_TRUE;
    } else {
      nsISupports *objIdentity = XPC_GetIdentityObject(cx, obj);
      nsISupports *otherIdentity = XPC_GetIdentityObject(cx, other);

      *bp = objIdentity && objIdentity == otherIdentity;
    }
  }

  return JS_TRUE;
}

static JSObject *
XPC_SJOW_Iterator(JSContext *cx, JSObject *obj, JSBool keysonly)
{
  obj = FindSafeObject(obj);
  NS_ASSERTION(obj != nsnull, "FindSafeObject() returned null in class hook!");

  JSObject *unsafeObj = GetUnsafeObject(obj);
  if (!unsafeObj) {
    ThrowException(NS_ERROR_INVALID_ARG, cx);

    return nsnull;
  }

  // Check that the caller can access the unsafe object.
  if (!CanCallerAccess(cx, unsafeObj)) {
    // CanCallerAccess() already threw for us.
    return nsnull;
  }

  JSObject *tmp = XPCWrapper::UnwrapGeneric(cx, &sXPC_XOW_JSClass, unsafeObj);
  if (tmp) {
    unsafeObj = tmp;

    // Repeat the CanCallerAccess check because the XOW is parented to our
    // scope's global object which makes the above CanCallerAccess call lie.
    if (!CanCallerAccess(cx, unsafeObj)) {
      // CanCallerAccess() already threw for us.
      return nsnull;
    }
  }

  // Create our dummy SJOW.
  JSObject *wrapperIter =
    ::JS_NewObjectWithGivenProto(cx, &sXPC_SJOW_JSClass.base, nsnull,
                                 unsafeObj);
  if (!wrapperIter) {
    return nsnull;
  }

  if (!::JS_SetReservedSlot(cx, wrapperIter, XPC_SJOW_SLOT_IS_RESOLVING,
                            JSVAL_ZERO)) {
    return nsnull;
  }

  JSAutoTempValueRooter tvr(cx, OBJECT_TO_JSVAL(wrapperIter));

  // Initialize the wrapper.
  return XPCWrapper::CreateIteratorObj(cx, wrapperIter, obj, unsafeObj,
                                       keysonly);
}

static JSObject *
XPC_SJOW_WrappedObject(JSContext *cx, JSObject *obj)
{
  return GetUnsafeObject(obj);
}

static JSBool
XPC_SJOW_toString(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                  jsval *rval)
{
  obj = FindSafeObject(obj);
  if (!obj) {
    return ThrowException(NS_ERROR_INVALID_ARG, cx);
  }

  JSObject *unsafeObj = GetUnsafeObject(obj);

  if (!unsafeObj) {
    // No unsafe object, nothing to stringify here, return "[object
    // XPCSafeJSObjectWrapper]" so callers know what they're looking
    // at.

    JSString *str = JS_NewStringCopyZ(cx, "[object XPCSafeJSObjectWrapper]");
    if (!str) {
      return JS_FALSE;
    }

    *rval = STRING_TO_JSVAL(str);

    return JS_TRUE;
  }

  // Check that the caller can access the unsafe object.
  if (!CanCallerAccess(cx, unsafeObj)) {
    // CanCallerAccess() already threw for us.
    return JS_FALSE;
  }

  {
    SafeCallGuard guard(cx, FindObjectPrincipals(cx, obj, unsafeObj));
    if (!guard.ready()) {
      return JS_FALSE;
    }

    JSString *str = JS_ValueToString(cx, OBJECT_TO_JSVAL(unsafeObj));
    if (!str) {
      return JS_FALSE;
    }
    *rval = STRING_TO_JSVAL(str);
  }
  return JS_TRUE;
}

PRBool
XPC_SJOW_AttachNewConstructorObject(XPCCallContext &ccx,
                                    JSObject *aGlobalObject)
{
  // Initialize sEvalNative the first time we attach a constructor.
  // NB: This always happens before any cross origin wrappers are
  // created, so it's OK to do this here.
  if (!XPCWrapper::FindEval(ccx, aGlobalObject)) {
    return PR_FALSE;
  }

  JSObject *class_obj =
    ::JS_InitClass(ccx, aGlobalObject, nsnull, &sXPC_SJOW_JSClass.base,
                   XPC_SJOW_Construct, 0, nsnull, nsnull, nsnull, nsnull);
  if (!class_obj) {
    NS_WARNING("can't initialize the XPCSafeJSObjectWrapper class");
    return PR_FALSE;
  }

  if (!::JS_DefineFunction(ccx, class_obj, "toString", XPC_SJOW_toString,
                           0, 0)) {
    return PR_FALSE;
  }

  // Null out the class object's parent to prevent code in this class
  // from thinking the class object is a wrapper for the global
  // object.
  ::JS_SetParent(ccx, class_obj, nsnull);

  // Make sure our prototype chain is empty and that people can't mess
  // with XPCSafeJSObjectWrapper.prototype.
  ::JS_SetPrototype(ccx, class_obj, nsnull);
  if (!::JS_SealObject(ccx, class_obj, JS_FALSE)) {
    NS_WARNING("Failed to seal XPCSafeJSObjectWrapper.prototype");
    return PR_FALSE;
  }

  JSBool found;
  return ::JS_SetPropertyAttributes(ccx, aGlobalObject,
                                    sXPC_SJOW_JSClass.base.name,
                                    JSPROP_READONLY | JSPROP_PERMANENT,
                                    &found);
}
