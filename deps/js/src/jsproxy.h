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
 * Portions created by the Initial Developer are Copyright (C) 2009
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Andreas Gal <gal@mozilla.com>
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

#ifndef jsproxy_h___
#define jsproxy_h___

#include "jsapi.h"
#include "jscntxt.h"
#include "jsobj.h"

namespace js {

/* Base class for all C++ proxy handlers. */
class JSProxyHandler {
    void *mFamily;
  public:
    explicit JSProxyHandler(void *family);
    virtual ~JSProxyHandler();

    /* ES5 Harmony fundamental proxy traps. */
    virtual bool getPropertyDescriptor(JSContext *cx, JSObject *proxy, jsid id,
                                       JSPropertyDescriptor *desc) = 0;
    virtual bool getOwnPropertyDescriptor(JSContext *cx, JSObject *proxy, jsid id,
                                          JSPropertyDescriptor *desc) = 0;
    virtual bool defineProperty(JSContext *cx, JSObject *proxy, jsid id,
                                JSPropertyDescriptor *desc) = 0;
    virtual bool getOwnPropertyNames(JSContext *cx, JSObject *proxy, js::AutoValueVector &props) = 0;
    virtual bool delete_(JSContext *cx, JSObject *proxy, jsid id, bool *bp) = 0;
    virtual bool enumerate(JSContext *cx, JSObject *proxy, js::AutoValueVector &props) = 0;
    virtual bool fix(JSContext *cx, JSObject *proxy, jsval *vp) = 0;

    /* ES5 Harmony derived proxy traps. */
    virtual JS_FRIEND_API(bool) has(JSContext *cx, JSObject *proxy, jsid id, bool *bp);
    virtual JS_FRIEND_API(bool) hasOwn(JSContext *cx, JSObject *proxy, jsid id, bool *bp);
    virtual JS_FRIEND_API(bool) get(JSContext *cx, JSObject *proxy, JSObject *receiver, jsid id, jsval *vp);
    virtual JS_FRIEND_API(bool) set(JSContext *cx, JSObject *proxy, JSObject *receiver, jsid id, jsval *vp);
    virtual JS_FRIEND_API(bool) enumerateOwn(JSContext *cx, JSObject *proxy, js::AutoValueVector &props);
    virtual JS_FRIEND_API(bool) iterate(JSContext *cx, JSObject *proxy, uintN flags, jsval *vp);

    /* Spidermonkey extensions. */
    virtual JS_FRIEND_API(bool) call(JSContext *cx, JSObject *proxy, uintN argc, jsval *vp);
    virtual JS_FRIEND_API(bool) construct(JSContext *cx, JSObject *proxy,
                                          uintN argc, jsval *argv, jsval *rval);
    virtual JS_FRIEND_API(JSString *) obj_toString(JSContext *cx, JSObject *proxy);
    virtual JS_FRIEND_API(JSString *) fun_toString(JSContext *cx, JSObject *proxy, uintN indent);
    virtual JS_FRIEND_API(void) finalize(JSContext *cx, JSObject *proxy);
    virtual JS_FRIEND_API(void) trace(JSTracer *trc, JSObject *proxy);

    inline void *family() {
        return mFamily;
    }
};

/* Dispatch point for handlers that executes the appropriate C++ or scripted traps. */
class JSProxy {
  public:
    /* ES5 Harmony fundamental proxy traps. */
    static bool getPropertyDescriptor(JSContext *cx, JSObject *proxy, jsid id,
                                      JSPropertyDescriptor *desc);
    static bool getPropertyDescriptor(JSContext *cx, JSObject *proxy, jsid id, jsval *vp);
    static bool getOwnPropertyDescriptor(JSContext *cx, JSObject *proxy, jsid id,
                                         JSPropertyDescriptor *desc);
    static bool getOwnPropertyDescriptor(JSContext *cx, JSObject *proxy, jsid id, jsval *vp);
    static bool defineProperty(JSContext *cx, JSObject *proxy, jsid id, JSPropertyDescriptor *desc);
    static bool defineProperty(JSContext *cx, JSObject *proxy, jsid id, jsval v);
    static bool getOwnPropertyNames(JSContext *cx, JSObject *proxy, js::AutoValueVector &props);
    static bool delete_(JSContext *cx, JSObject *proxy, jsid id, bool *bp);
    static bool enumerate(JSContext *cx, JSObject *proxy, js::AutoValueVector &props);
    static bool fix(JSContext *cx, JSObject *proxy, jsval *vp);

    /* ES5 Harmony derived proxy traps. */
    static bool has(JSContext *cx, JSObject *proxy, jsid id, bool *bp);
    static bool hasOwn(JSContext *cx, JSObject *proxy, jsid id, bool *bp);
    static bool get(JSContext *cx, JSObject *proxy, JSObject *receiver, jsid id, jsval *vp);
    static bool set(JSContext *cx, JSObject *proxy, JSObject *receiver, jsid id, jsval *vp);
    static bool enumerateOwn(JSContext *cx, JSObject *proxy, js::AutoValueVector &props);
    static bool iterate(JSContext *cx, JSObject *proxy, uintN flags, jsval *vp);

    /* Spidermonkey extensions. */
    static bool call(JSContext *cx, JSObject *proxy, uintN argc, jsval *vp);
    static bool construct(JSContext *cx, JSObject *proxy, uintN argc, jsval *argv, jsval *rval);
    static JSString *obj_toString(JSContext *cx, JSObject *proxy);
    static JSString *fun_toString(JSContext *cx, JSObject *proxy, uintN indent);
};

/* Shared between object and function proxies. */
const uint32 JSSLOT_PROXY_HANDLER = JSSLOT_PRIVATE + 0;
const uint32 JSSLOT_PROXY_PRIVATE = JSSLOT_PRIVATE + 1;
/* Function proxies only. */
const uint32 JSSLOT_PROXY_CALL = JSSLOT_PRIVATE + 2;
const uint32 JSSLOT_PROXY_CONSTRUCT = JSSLOT_PRIVATE + 3;

extern JS_FRIEND_API(JSClass) ObjectProxyClass;
extern JS_FRIEND_API(JSClass) FunctionProxyClass;
extern JSClass CallableObjectClass;

}

inline bool
JSObject::isObjectProxy() const
{
    return getClass() == &js::ObjectProxyClass;
}

inline bool
JSObject::isFunctionProxy() const
{
    return getClass() == &js::FunctionProxyClass;
}

inline bool
JSObject::isProxy() const
{
    return isObjectProxy() || isFunctionProxy();
}

inline js::JSProxyHandler *
JSObject::getProxyHandler() const
{
    JS_ASSERT(isProxy());
    jsval handler = getSlot(js::JSSLOT_PROXY_HANDLER);
    return (js::JSProxyHandler *) JSVAL_TO_PRIVATE(handler);
}

inline jsval
JSObject::getProxyPrivate() const
{
    JS_ASSERT(isProxy());
    return getSlot(js::JSSLOT_PROXY_PRIVATE);
}

inline void
JSObject::setProxyPrivate(jsval priv)
{
    JS_ASSERT(isProxy());
    setSlot(js::JSSLOT_PROXY_PRIVATE, priv);
}

namespace js {

JS_FRIEND_API(JSObject *)
NewProxyObject(JSContext *cx, JSProxyHandler *handler, jsval priv, JSObject *proto, JSObject *parent,
               JSObject *call = NULL, JSObject *construct = NULL);

JS_FRIEND_API(JSBool)
FixProxy(JSContext *cx, JSObject *proxy, JSBool *bp);

}

JS_BEGIN_EXTERN_C

extern JSClass js_ProxyClass;

extern JS_FRIEND_API(JSObject *)
js_InitProxyClass(JSContext *cx, JSObject *obj);

JS_END_EXTERN_C

#endif
