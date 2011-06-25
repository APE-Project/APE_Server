/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
 */

#include "tests.h"
#include "jsxdrapi.h"

static JSBool
nativeGet(JSContext *cx, JSObject *obj, jsid id, jsval *vp)
{
    *vp = INT_TO_JSVAL(17);
    return JS_TRUE;
}

BEGIN_TEST(testSetProperty_NativeGetterStubSetter)
{
    jsvalRoot vobj(cx);
    JSObject *obj = JS_NewObject(cx, NULL, NULL, NULL);
    CHECK(obj);
    vobj = OBJECT_TO_JSVAL(obj);

    CHECK(JS_DefineProperty(cx, global, "globalProp", vobj,
                            JS_PropertyStub, JS_StrictPropertyStub,
                            JSPROP_ENUMERATE));

    CHECK(JS_DefineProperty(cx, obj, "prop", JSVAL_VOID,
                            nativeGet, JS_StrictPropertyStub,
                            JSPROP_SHARED));

    EXEC("'use strict';                                     \n"
         "var error, passed = false;                        \n"
         "try                                               \n"
         "{                                                 \n"
         "  this.globalProp.prop = 42;                      \n"
         "  throw new Error('setting property succeeded!'); \n"
         "}                                                 \n"
         "catch (e)                                         \n"
         "{                                                 \n"
         "  error = e;                                      \n"
         "  if (e instanceof TypeError)                     \n"
         "    passed = true;                                \n"
         "}                                                 \n"
         "                                                  \n"
         "if (!passed)                                      \n"
         "  throw error;                                    \n");

    EXEC("var error, passed = false;                        \n"
         "try                                               \n"
         "{                                                 \n"
         "  this.globalProp.prop = 42;                      \n"
         "  if (this.globalProp.prop === 17)                \n"
         "    passed = true;                                \n"
         "  else                                            \n"
         "    throw new Error('bad value after set!');      \n"
         "}                                                 \n"
         "catch (e)                                         \n"
         "{                                                 \n"
         "  error = e;                                      \n"
         "}                                                 \n"
         "                                                  \n"
         "if (!passed)                                      \n"
         "  throw error;                                    \n");

    return true;
}
END_TEST(testSetProperty_NativeGetterStubSetter)

BEGIN_TEST(testSetProperty_InheritedGlobalSetter)
{
    // This is a JSAPI test because jsapi-test globals do not have a resolve
    // hook and therefore can use the property cache in some cases where the
    // shell can't.
    JS_ASSERT(JS_GET_CLASS(cx, global)->resolve == &JS_ResolveStub);

    CHECK(JS_DefineProperty(cx, global, "HOTLOOP", INT_TO_JSVAL(8), NULL, NULL, 0));
    EXEC("var n = 0;\n"
         "var global = this;\n"
         "function f() { n++; }\n"
         "Object.defineProperty(Object.prototype, 'x', {set: f});\n"
         "for (var i = 0; i < HOTLOOP; i++)\n"
         "    global.x = i;\n");
    EXEC("if (n != HOTLOOP)\n"
         "    throw 'FAIL';\n");
    return true;
}
END_TEST(testSetProperty_InheritedGlobalSetter)

