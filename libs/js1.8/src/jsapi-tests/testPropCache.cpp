#include "tests.h"

static int g_counter;

static JSBool
CounterAdd(JSContext *cx, JSObject *obj, jsval idval, jsval *vp)
{
    g_counter++;
    return JS_TRUE;
}

static JSClass CounterClass = {
    "Counter",  /* name */
    0,  /* flags */
    CounterAdd, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

BEGIN_TEST(testPropCache_bug505798)
{
    g_counter = 0;
    EXEC("var x = {};");
    CHECK(JS_DefineObject(cx, global, "y", &CounterClass, NULL, JSPROP_ENUMERATE));
    EXEC("var arr = [x, y];\n"
         "for (var i = 0; i < arr.length; i++)\n"
         "    arr[i].p = 1;\n");
    knownFail = true;
    CHECK(g_counter == 1);
    return true;
}
END_TEST(testPropCache_bug505798)
