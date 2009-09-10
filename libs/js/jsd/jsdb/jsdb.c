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
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
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

/*
* JSDB public and callback functions
*/

#include "jsdbpriv.h"
#include <errno.h>

#ifndef JS_THREADSAFE
#define JS_BeginRequest(cx) (void)0
#define JS_EndRequest(cx) (void)0
#endif

/***************************************************************************/

JS_STATIC_DLL_CALLBACK(void)
_ErrorReporter(JSContext *cx, const char *message, JSErrorReport *report)
{
    int i, j, k, n;

    fputs("jsdb: ", stderr);
    if (!report) {
        fprintf(stderr, "%s\n", message);
        return;
    }

    if (report->filename)
        fprintf(stderr, "%s, ", report->filename);
    if (report->lineno)
        fprintf(stderr, "line %u: ", report->lineno);
    fputs(message, stderr);
    if (!report->linebuf) {
        putc('\n', stderr);
        return;
    }

    fprintf(stderr, ":\n%s\n", report->linebuf);
    n = report->tokenptr - report->linebuf;
    for (i = j = 0; i < n; i++) {
        if (report->linebuf[i] == '\t') {
            for (k = (j + 8) & ~7; j < k; j++)
                putc('.', stderr);
            continue;
        }
        putc('.', stderr);
        j++;
    }
    fputs("^\n", stderr);
}

JS_STATIC_DLL_CALLBACK(void)
jsdb_ScriptHookProc(JSDContext* jsdc,
                    JSDScript*  jsdscript,
                    JSBool      creating,
                    void*       callerdata)
{
    JSDB_Data* data = (JSDB_Data*) callerdata;
    JSFunction* fun;

    JS_BeginRequest(data->cxDebugger);
    if(data->jsScriptHook &&
       NULL != (fun = JS_ValueToFunction(data->cxDebugger, data->jsScriptHook)))
    {
        jsval result;
        jsval args[2];

        args[0] = P2H_SCRIPT(data->cxDebugger, jsdscript);
        args[1] = creating ? JSVAL_TRUE : JSVAL_FALSE;

        JS_CallFunction(data->cxDebugger, NULL, fun, 2, args, &result);
    }
    JS_EndRequest(data->cxDebugger);
}

uintN JS_DLL_CALLBACK
jsdb_ExecHookHandler(JSDContext*     jsdc,
                     JSDThreadState* jsdthreadstate,
                     uintN           type,
                     void*           callerdata,
                     jsval*          rval)
{
    uintN ourRetVal = JSD_HOOK_RETURN_CONTINUE;
    jsval result;
    JSFunction* fun;
    int answer;
    JSDB_Data* data = (JSDB_Data*) callerdata;
    JS_ASSERT(data);

    /* if we're already stopped, then don't stop */
    if(data->jsdthreadstate)
        return JSD_HOOK_RETURN_CONTINUE;

    JS_BeginRequest(data->cxDebugger);
    if(!jsdb_SetThreadState(data, jsdthreadstate))
        goto label_bail;

    if(data->jsExecutionHook &&
       NULL != (fun = JS_ValueToFunction(data->cxDebugger, data->jsExecutionHook)))
    {
        jsval arg = INT_TO_JSVAL((int)type);
        if(!JS_CallFunction(data->cxDebugger, NULL, fun, 1, &arg, &result))
            goto label_bail;
        if(!JSVAL_IS_INT(result))
            goto label_bail;
        answer = JSVAL_TO_INT(result);


        if((answer == JSD_HOOK_RETURN_RET_WITH_VAL ||
            answer == JSD_HOOK_RETURN_THROW_WITH_VAL) &&
           !jsdb_EvalReturnExpression(data, rval))          
        {
            goto label_bail;
        }

        if(answer >= JSD_HOOK_RETURN_HOOK_ERROR &&
           answer <= JSD_HOOK_RETURN_CONTINUE_THROW)
            ourRetVal = answer;
        else
            ourRetVal = JSD_HOOK_RETURN_CONTINUE;
    }

label_bail:
    jsdb_SetThreadState(data, NULL);
    JS_EndRequest(data->cxDebugger);
    return ourRetVal;
}

typedef enum
{
    ARG_MSG = 0,
    ARG_FILENAME,
    ARG_LINENO,
    ARG_LINEBUF,
    ARG_TOKEN_OFFSET,
    ARG_LIMIT
} ER_ARGS;

uintN JS_DLL_CALLBACK
jsdb_ErrorReporter(JSDContext*     jsdc,
                   JSContext*      cx,
                   const char*     message,
                   JSErrorReport*  report,
                   void*           callerdata)
{
    uintN ourRetVal = JSD_ERROR_REPORTER_PASS_ALONG;
    jsval result;
    JSFunction* fun;
    int32 answer;
    JSDB_Data* data = (JSDB_Data*) callerdata;
    JS_ASSERT(data);

    JS_BeginRequest(data->cxDebugger);
    if(data->jsErrorReporterHook &&
       NULL != (fun = JS_ValueToFunction(data->cxDebugger,
                                         data->jsErrorReporterHook)))
    {
        jsval args[ARG_LIMIT] = {JSVAL_NULL};

        if(message)
            args[ARG_MSG] =
                    STRING_TO_JSVAL(JS_NewStringCopyZ(cx, message));
        if(report && report->filename)
            args[ARG_FILENAME] =
                    STRING_TO_JSVAL(JS_NewStringCopyZ(cx, report->filename));
        if(report && report->linebuf)
            args[ARG_LINEBUF] =
                    STRING_TO_JSVAL(JS_NewStringCopyZ(cx, report->linebuf));
        if(report)
            args[ARG_LINENO] =
                    INT_TO_JSVAL(report->lineno);
        if(report && report->linebuf && report->tokenptr)
            args[ARG_TOKEN_OFFSET] =
                    INT_TO_JSVAL((int)(report->tokenptr - report->linebuf));

        if(!JS_CallFunction(data->cxDebugger, NULL, fun, ARG_LIMIT, args, &result))
            return ourRetVal;

        if(JS_ValueToInt32(data->cxDebugger, result, &answer))
            ourRetVal = (uintN) answer;
    }
    JS_EndRequest(data->cxDebugger);
    return ourRetVal;
}

JS_STATIC_DLL_CALLBACK(JSBool)
Load(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    uintN i;
    JSString *str;
    const char *filename;
    JSScript *script;
    JSBool ok;
    jsval result;
    uint32 oldopts;

    ok = JS_TRUE;
    for (i = 0; i < argc; i++) {
        str = JS_ValueToString(cx, argv[i]);
        if (!str) {
            ok = JS_FALSE;
            break;
        }
        argv[i] = STRING_TO_JSVAL(str);
        filename = JS_GetStringBytes(str);
        errno = 0;
        oldopts = JS_GetOptions(cx);
        JS_SetOptions(cx, oldopts | JSOPTION_COMPILE_N_GO);
        script = JS_CompileFile(cx, obj, filename);
        if (!script)
            continue;
        ok = JS_ExecuteScript(cx, obj, script, &result);
        JS_DestroyScript(cx, script);
        if (!ok)
            break;
    }
    JS_SetOptions(cx, oldopts);
    JS_GC(cx);
    *rval = STRING_TO_JSVAL(JS_NewStringCopyZ(cx,""));
    return ok;
}

JS_STATIC_DLL_CALLBACK(JSBool)
Write(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSString *str = JS_ValueToString(cx, argv[0]);
    if (!str)
        return JS_FALSE;
    printf(JS_GetStringBytes(str));
    *rval = STRING_TO_JSVAL(JS_NewStringCopyZ(cx,""));
    return JS_TRUE;
}

JS_STATIC_DLL_CALLBACK(JSBool)
Gets(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    char buf[1024];
    if(! fgets(buf, sizeof(buf), stdin))
        return JS_FALSE;
    *rval = STRING_TO_JSVAL(JS_NewStringCopyZ(cx, buf));
    return JS_TRUE;
}

JS_STATIC_DLL_CALLBACK(JSBool)
GC(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    argc = 0;
    obj = 0;
    argv = 0;
    JS_GC(cx);
    *rval = JSVAL_VOID;
    return JS_TRUE;
}

JS_STATIC_DLL_CALLBACK(JSBool)
Version(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    if (argc > 0 && JSVAL_IS_INT(argv[0]))
        *rval = INT_TO_JSVAL(JS_SetVersion(cx, JSVAL_TO_INT(argv[0])));
    else
        *rval = INT_TO_JSVAL(JS_GetVersion(cx));
    return JS_TRUE;
}


JS_STATIC_DLL_CALLBACK(JSBool)
SafeEval(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    static char default_filename[] = "jsdb_eval";
/*     JSContext *cx2; */
    JSString* textJSString;
    char* filename;
    int32 lineno;
    JSDB_Data* data = (JSDB_Data*) JS_GetContextPrivate(cx);
    JS_ASSERT(data);

    if(argc < 1 || !(textJSString = JS_ValueToString(cx, argv[0])))
    {
        JS_ReportError(cx, "safeEval requires source text as a first argument");
        return JS_FALSE;
    }

    if(argc < 2)
        filename = default_filename;
    else
    {
        JSString* filenameJSString;
        if(!(filenameJSString = JS_ValueToString(cx, argv[1])))
        {
            JS_ReportError(cx, "safeEval passed non-string filename as 2nd param");
            return JS_FALSE;
        }
        filename = JS_GetStringBytes(filenameJSString);
    }

    if(argc < 3)
        lineno = 1;
    else
    {
        if(!JS_ValueToInt32(cx, argv[2], &lineno))
        {
            JS_ReportError(cx, "safeEval passed non-int lineno as 3rd param");
            return JS_FALSE;
        }
    }

    if(! JS_EvaluateScript(cx, obj,
                           JS_GetStringBytes(textJSString),
                           JS_GetStringLength(textJSString),
                           filename, lineno, rval))
        *rval = STRING_TO_JSVAL(JS_NewStringCopyZ(cx,""));

    return JS_TRUE;
}

JS_STATIC_DLL_CALLBACK(JSBool)
NativeBreak(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
#ifdef _WINDOWS
    _asm int 3;
    *rval = STRING_TO_JSVAL(JS_NewStringCopyZ(cx,"did _asm int 3;"));
#else
    *rval = STRING_TO_JSVAL(JS_NewStringCopyZ(cx,"only supported for Windows"));
#endif
    return JS_TRUE;
}

static JSFunctionSpec debugger_functions[] = {
    {"gc",              GC,        0, 0, 0},
    {"version",         Version,        0, 0, 0},
    {"load",            Load,           1, 0, 0},
    {"write",           Write,          0, 0, 0},
    {"gets",            Gets,           0, 0, 0},
    {"safeEval",        SafeEval,       3, 0, 0},
    {"nativeBreak",     NativeBreak,    0, 0, 0},
    {0, 0, 0, 0, 0}
};

static JSClass debugger_global_class = {
    "debugger_global", JSCLASS_HAS_PRIVATE,
    JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,
    JS_EnumerateStub, JS_ResolveStub,   JS_ConvertStub,   JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

/***************************************************************************/

#ifdef JSD_LOWLEVEL_SOURCE
/*
 * This facilitates sending source to JSD (the debugger system) in the shell
 * where the source is loaded using the JSFILE hack in jsscan. The function
 * below is used as a callback for the jsdbgapi JS_SetSourceHandler hook.
 * A more normal embedding (e.g. mozilla) loads source itself and can send
 * source directly to JSD without using this hook scheme.
 */
static void
SendSourceToJSDebugger(const char *filename, uintN lineno,
                       jschar *str, size_t length,
                       void **listenerTSData, JSDContext* jsdc)
{
    JSDSourceText *jsdsrc = (JSDSourceText *) *listenerTSData;

    if (!jsdsrc) {
        if (!filename)
            filename = "typein";
        if (1 == lineno) {
            jsdsrc = JSD_NewSourceText(jsdc, filename);
        } else {
            jsdsrc = JSD_FindSourceForURL(jsdc, filename);
            if (jsdsrc && JSD_SOURCE_PARTIAL !=
                JSD_GetSourceStatus(jsdc, jsdsrc)) {
                jsdsrc = NULL;
            }
        }
    }
    if (jsdsrc) {
        jsdsrc = JSD_AppendUCSourceText(jsdc,jsdsrc, str, length,
                                        JSD_SOURCE_PARTIAL);
    }
    *listenerTSData = jsdsrc;
}
#endif /* JSD_LOWLEVEL_SOURCE */

static JSBool
_initReturn(const char* str, JSBool retval)
{
    if(str)
        printf("%s\n", str);
    if(retval)
        ; /* printf("debugger initialized\n"); */
    else
    {
        printf("debugger FAILED to initialize\n");
    }
    return retval;
}

static JSBool
_initReturn_cx(const char* str, JSBool retval, JSDB_Data* data)
{
    JSContext* cx = data->cxDebugger;
    JS_EndRequest(cx);
    if (!retval) {
        JS_DestroyContext(cx);
        JS_DestroyRuntime(data->rtDebugger);
        free(data);
    }
    return _initReturn(str, retval);
}

#define MAX_DEBUGGER_DEPTH 3

void
jsdb_TermDebugger(JSDB_Data* data, JSBool callDebuggerOff)
{
    JSObject* dbgObj;
    jsval rvalIgnore;
    JSContext* cx;
    if (!data)
        return;

    cx = data->cxDebugger;
    if (data->jsdcDebugger) {
        JSDB_Data* targetData = (JSDB_Data*) JSD_GetContextPrivate(data->jsdcDebugger);
        if (targetData)
            jsdb_TermDebugger(targetData, JS_TRUE);
    }
    JS_DestroyContext(data->cxDebugger);
    if (callDebuggerOff)
        JSD_DebuggerOff(data->jsdcTarget);
    JS_DestroyRuntime(data->rtDebugger);
    free(data);
}

JS_EXPORT_API(void)
JSDB_TermDebugger(JSDContext* jsdc)
{
    JSDB_Data* data = (JSDB_Data*) JSD_GetContextPrivate(jsdc);
    if (!data)
        return;

    jsdb_TermDebugger(data, JS_FALSE);
}

JS_EXPORT_API(JSBool)
JSDB_InitDebugger(JSRuntime* rt, JSDContext* jsdc, int depth)
{
    JSContext* cx;
    JSObject* dbgObj;
    jsval rvalIgnore;
    static char load_deb[] = "load('debugger.js')";

    JSDB_Data* data = (JSDB_Data*) calloc(1, sizeof(JSDB_Data));
    if(!data)
        return _initReturn("memory alloc error", JS_FALSE);

    data->rtTarget   = rt;
    data->jsdcTarget = jsdc;
    data->debuggerDepth = depth+1;

    if(!(data->rtDebugger = JS_NewRuntime(8L * 1024L * 1024L)))
        return _initReturn("debugger runtime creation error", JS_FALSE);
    printf("NewRuntime: %p\n", data->rtDebugger);
    if(!(data->cxDebugger = cx = JS_NewContext(data->rtDebugger, 8192))) {
        JS_DestroyRuntime(data->rtDebugger);
        free(data);
        return _initReturn("debugger creation error", JS_FALSE);
    }

    JS_SetContextPrivate(cx, data);

    JSD_SetContextPrivate(jsdc, data);

    JS_SetErrorReporter(cx, _ErrorReporter);

    JS_BeginRequest(cx);
    if(!(data->globDebugger = dbgObj =
            JS_NewObject(cx, &debugger_global_class, NULL, NULL)))
        return _initReturn_cx("debugger global object creation error", JS_FALSE, data);

    if(!JS_SetPrivate(cx, dbgObj, data))
        return _initReturn_cx("debugger global set private failure", JS_FALSE, data);

    if(!JS_InitStandardClasses(cx, dbgObj))
        return _initReturn_cx("debugger InitStandardClasses error", JS_FALSE, data);

    if(!JS_DefineFunctions(cx, dbgObj, debugger_functions))
        return _initReturn_cx("debugger DefineFunctions error", JS_FALSE, data);

    if(!jsdb_ReflectJSD(data))
        return _initReturn_cx("debugger reflection of JSD API error", JS_FALSE, data);

    if(data->debuggerDepth < MAX_DEBUGGER_DEPTH)
    {
        JSDContext* local_jsdc;
        if(!(local_jsdc = JSD_DebuggerOnForUser(data->rtDebugger, NULL, NULL)))
            return _initReturn_cx("failed to create jsdc for nested debugger",
                               JS_FALSE, data);
        JSD_JSContextInUse(local_jsdc, cx);
        if(!JSDB_InitDebugger(data->rtDebugger, local_jsdc, data->debuggerDepth)) {
            JSD_DebuggerOff(local_jsdc);
            return _initReturn_cx("failed to init nested debugger", JS_FALSE, data);
        }

        data->jsdcDebugger = local_jsdc;
    }

    JSD_SetScriptHook(jsdc, jsdb_ScriptHookProc, data);
    JSD_SetDebuggerHook(jsdc, jsdb_ExecHookHandler, data);
    JSD_SetThrowHook(jsdc, jsdb_ExecHookHandler, data);
    JSD_SetDebugBreakHook(jsdc, jsdb_ExecHookHandler, data);
    JSD_SetErrorReporter(jsdc, jsdb_ErrorReporter, data);

#ifdef JSD_LOWLEVEL_SOURCE
    JS_SetSourceHandler(data->rtDebugger, SendSourceToJSDebugger, jsdc);
#endif /* JSD_LOWLEVEL_SOURCE */

    if (!JS_EvaluateScript(cx, dbgObj,
                      load_deb, sizeof(load_deb)-1, "jsdb_autoload", 1,
                      &rvalIgnore))
    {
        return _initReturn_cx("failed to execute jsdb_autoload", JS_FALSE, data);
    }

    return _initReturn_cx(NULL, JS_TRUE, data);
}

