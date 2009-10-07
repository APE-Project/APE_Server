/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
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
 * JavaScript Debugging support - Stepping support
 */

#include "jsd.h"

/*
* #define JSD_TRACE 1
*/

#ifdef JSD_TRACE

static char*
_indentSpaces(int i)
{
#define MAX_INDENT 63
    static char* p = NULL;
    if(!p)
    {
        p = calloc(1, MAX_INDENT+1);
        if(!p) return "";
        memset(p, ' ', MAX_INDENT);
    }
    if(i > MAX_INDENT) return p;
    return p + MAX_INDENT-i;
}

static void
_interpreterTrace(JSDContext* jsdc, JSContext *cx, JSStackFrame *fp,
                  JSBool before)
{
    JSDScript* jsdscript = NULL;
    JSScript * script;
    static indent = 0;
    char* buf;
    const char* funName = NULL;

    script = JS_GetFrameScript(cx, fp);
    if(script)
    {
        JSD_LOCK_SCRIPTS(jsdc);
        jsdscript = jsd_FindOrCreateJSDScript(jsdc, cx, script, fp);
        JSD_UNLOCK_SCRIPTS(jsdc);
        if(jsdscript)
            funName = JSD_GetScriptFunctionName(jsdc, jsdscript);
    }
    if(!funName)
        funName = "TOP_LEVEL";

    if(before)
    {
        buf = JS_smprintf("%sentering %s %s this: %0x\n",
                _indentSpaces(indent++),
                funName,
                JS_IsConstructorFrame(cx, fp) ? "constructing":"",
                (int)JS_GetFrameThis(cx, fp));
    }
    else
    {
        buf = JS_smprintf("%sleaving %s\n",
                _indentSpaces(--indent),
                funName);
    }
    JS_ASSERT(indent >= 0);

    if(!buf)
        return;

    printf(buf);
    free(buf);
}
#endif

JSBool
_callHook(JSDContext *jsdc, JSContext *cx, JSStackFrame *fp, JSBool before,
          uintN type, JSD_CallHookProc hook, void *hookData)
{
    JSDScript*        jsdscript;
    JSScript*         jsscript;
    JSBool            hookresult = JS_TRUE;
    
    if (!jsdc || !jsdc->inited)
        return JS_FALSE;

    if (!hook && !(jsdc->flags & JSD_COLLECT_PROFILE_DATA) &&
        jsdc->flags & JSD_DISABLE_OBJECT_TRACE)
    {
        /* no hook to call, no profile data needs to be collected, and
         * the client has object tracing disabled, so there is nothing
         * to do here.
         */
        return hookresult;
    }
    
    if (before && JS_IsConstructorFrame(cx, fp))
        jsd_Constructing(jsdc, cx, JS_GetFrameThis(cx, fp), fp);

    jsscript = JS_GetFrameScript(cx, fp);
    if (jsscript)
    {
        JSD_LOCK_SCRIPTS(jsdc);
        jsdscript = jsd_FindOrCreateJSDScript(jsdc, cx, jsscript, fp);
        JSD_UNLOCK_SCRIPTS(jsdc);
    
        if (jsdscript)
        {
            if (JSD_IS_PROFILE_ENABLED(jsdc, jsdscript))
            {
                JSDProfileData *pdata;
                pdata = jsd_GetScriptProfileData (jsdc, jsdscript);
                if (pdata)
                {
                    if (before)
                    {
                        if (JSLL_IS_ZERO(pdata->lastCallStart))
                        {
                            int64 now;
                            JSDProfileData *callerpdata;
                            
                            /* Get the time just the once, for consistency. */
                            now = JS_Now();
                            /* This contains a pointer to the profile data for
                             * the caller of this function. */
                            callerpdata = jsdc->callingFunctionPData;
                            if (callerpdata)
                            {
                                int64 ll_delta;
                                pdata->caller = callerpdata;
                                /* We need to 'stop' the timer for the caller.
                                 * Use time since last return if appropriate. */
                                if (JSLL_IS_ZERO(jsdc->lastReturnTime))
                                {
                                    JSLL_SUB(ll_delta, now, callerpdata->lastCallStart);
                                } else {
                                    JSLL_SUB(ll_delta, now, jsdc->lastReturnTime);
                                }
                                JSLL_ADD(callerpdata->runningTime, callerpdata->runningTime, ll_delta);
                            }
                            /* We're the new current function, and no return
                             * has happened yet. */
                            jsdc->callingFunctionPData = pdata;
                            jsdc->lastReturnTime = 0;
                            /* This function has no running time (just been
                             * called!), and we'll need the call start time. */
                            pdata->runningTime = 0;
                            pdata->lastCallStart = now;
                        } else {
                            if (++pdata->recurseDepth > pdata->maxRecurseDepth)
                                pdata->maxRecurseDepth = pdata->recurseDepth;
                        }
                        /* make sure we're called for the return too. */
                        hookresult = JS_TRUE;
                    } else if (!pdata->recurseDepth &&
                               !JSLL_IS_ZERO(pdata->lastCallStart)) {
                        int64 now, ll_delta;
                        jsdouble delta;
                        now = JS_Now();
                        JSLL_SUB(ll_delta, now, pdata->lastCallStart);
                        JSLL_L2D(delta, ll_delta);
                        delta /= 1000.0;
                        pdata->totalExecutionTime += delta;
                        /* minExecutionTime starts as 0, so we need to overwrite
                         * it on the first call always. */
                        if ((0 == pdata->callCount) ||
                            delta < pdata->minExecutionTime)
                        {
                            pdata->minExecutionTime = delta;
                        }
                        if (delta > pdata->maxExecutionTime)
                            pdata->maxExecutionTime = delta;
                        
                        /* If we last returned from a function (as opposed to
                         * having last entered this function), we need to inc.
                         * the running total by the time delta since the last
                         * return, and use the running total instead of the
                         * delta calculated above. */
                        if (!JSLL_IS_ZERO(jsdc->lastReturnTime))
                        {
                            /* Add last chunk to running time, and use total
                             * running time as 'delta'. */
                            JSLL_SUB(ll_delta, now, jsdc->lastReturnTime);
                            JSLL_ADD(pdata->runningTime, pdata->runningTime, ll_delta);
                            JSLL_L2D(delta, pdata->runningTime);
                            delta /= 1000.0;
                        }
                        
                        pdata->totalOwnExecutionTime += delta;
                        /* See minExecutionTime comment above. */
                        if ((0 == pdata->callCount) ||
                            delta < pdata->minOwnExecutionTime)
                        {
                            pdata->minOwnExecutionTime = delta;
                        }
                        if (delta > pdata->maxOwnExecutionTime)
                            pdata->maxOwnExecutionTime = delta;
                        
                        /* Current function is now our caller. */
                        jsdc->callingFunctionPData = pdata->caller;
                        /* No hanging pointers, please. */
                        pdata->caller = NULL;
                        /* Mark the time we returned, and indicate this
                         * function is no longer running. */
                        jsdc->lastReturnTime = now;
                        pdata->lastCallStart = 0;
                        ++pdata->callCount;
                    } else if (pdata->recurseDepth) {
                        --pdata->recurseDepth;
                        ++pdata->callCount;
                    }
                }
                if (hook)
                    jsd_CallCallHook (jsdc, cx, type, hook, hookData);
            } else {
                if (hook)
                    hookresult = 
                        jsd_CallCallHook (jsdc, cx, type, hook, hookData);
                else
                    hookresult = JS_TRUE;
            }
        }
    }

#ifdef JSD_TRACE
    _interpreterTrace(jsdc, cx, fp, before);
    return JS_TRUE;
#else
    return hookresult;
#endif

}

void *
jsd_FunctionCallHook(JSContext *cx, JSStackFrame *fp, JSBool before,
                     JSBool *ok, void *closure)
{
    JSDContext*       jsdc;
    JSD_CallHookProc  hook;
    void*             hookData;

    jsdc = (JSDContext*) closure;
    
    /* local in case jsdc->functionHook gets cleared on another thread */
    JSD_LOCK();
    hook     = jsdc->functionHook;
    hookData = jsdc->functionHookData;
    JSD_UNLOCK();
    
    if (_callHook (jsdc, cx, fp, before,
                   (before) ? JSD_HOOK_FUNCTION_CALL : JSD_HOOK_FUNCTION_RETURN,
                   hook, hookData))
    {
        return closure;
    }
    
    return NULL;
}

void *
jsd_TopLevelCallHook(JSContext *cx, JSStackFrame *fp, JSBool before,
                     JSBool *ok, void *closure)
{
    JSDContext*       jsdc;
    JSD_CallHookProc  hook;
    void*             hookData;

    jsdc = (JSDContext*) closure;

    /* local in case jsdc->toplevelHook gets cleared on another thread */
    JSD_LOCK();
    hook     = jsdc->toplevelHook;
    hookData = jsdc->toplevelHookData;
    JSD_UNLOCK();
    
    if (_callHook (jsdc, cx, fp, before,
                   (before) ? JSD_HOOK_TOPLEVEL_START : JSD_HOOK_TOPLEVEL_END,
                   hook, hookData))
    {
        return closure;
    }
    
    return NULL;
    
}
