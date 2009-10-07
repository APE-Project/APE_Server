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
 * JavaScript Debugging support - Hook support
 */

#include "jsd.h"

JSTrapStatus
jsd_InterruptHandler(JSContext *cx, JSScript *script, jsbytecode *pc, jsval *rval,
                     void *closure)
{
    JSDScript*      jsdscript;
    JSDContext*     jsdc = (JSDContext*) closure;
    JSD_ExecutionHookProc hook;
    void*                 hookData;

    if( ! jsdc || ! jsdc->inited )
        return JSTRAP_CONTINUE;

    if( JSD_IS_DANGEROUS_THREAD(jsdc) )
        return JSTRAP_CONTINUE;

    /* local in case jsdc->interruptHook gets cleared on another thread */
    JSD_LOCK();
    hook     = jsdc->interruptHook;
    hookData = jsdc->interruptHookData;
    JSD_UNLOCK();

    if (!hook)
        return JSTRAP_CONTINUE;
    
    JSD_LOCK_SCRIPTS(jsdc);
    jsdscript = jsd_FindOrCreateJSDScript(jsdc, cx, script, NULL);
    JSD_UNLOCK_SCRIPTS(jsdc);
    if( ! jsdscript )
        return JSTRAP_CONTINUE;

#ifdef LIVEWIRE
    if( ! jsdlw_UserCodeAtPC(jsdc, jsdscript, (jsuword)pc) )
        return JSTRAP_CONTINUE;
#endif

    return jsd_CallExecutionHook(jsdc, cx, JSD_HOOK_INTERRUPTED,
                                 hook, hookData, rval);
}

JSTrapStatus
jsd_DebuggerHandler(JSContext *cx, JSScript *script, jsbytecode *pc,
                    jsval *rval, void *closure)
{
    JSDScript*      jsdscript;
    JSDContext*     jsdc = (JSDContext*) closure;
    JSD_ExecutionHookProc hook;
    void*                 hookData;

    if( ! jsdc || ! jsdc->inited )
        return JSTRAP_CONTINUE;

    if( JSD_IS_DANGEROUS_THREAD(jsdc) )
        return JSTRAP_CONTINUE;

    /* local in case jsdc->debuggerHook gets cleared on another thread */
    JSD_LOCK();
    hook     = jsdc->debuggerHook;
    hookData = jsdc->debuggerHookData;
    JSD_UNLOCK();
    if(!hook)
        return JSTRAP_CONTINUE;

    JSD_LOCK_SCRIPTS(jsdc);
    jsdscript = jsd_FindOrCreateJSDScript(jsdc, cx, script, NULL);
    JSD_UNLOCK_SCRIPTS(jsdc);
    if( ! jsdscript )
        return JSTRAP_CONTINUE;

    return jsd_CallExecutionHook(jsdc, cx, JSD_HOOK_DEBUGGER_KEYWORD,
                                 hook, hookData, rval);
}


JSTrapStatus
jsd_ThrowHandler(JSContext *cx, JSScript *script, jsbytecode *pc,
                 jsval *rval, void *closure)
{
    JSDScript*      jsdscript;
    JSDContext*     jsdc = (JSDContext*) closure;
    JSD_ExecutionHookProc hook;
    void*                 hookData;

    if( ! jsdc || ! jsdc->inited )
        return JSD_HOOK_RETURN_CONTINUE_THROW;

    if( JSD_IS_DANGEROUS_THREAD(jsdc) )
        return JSD_HOOK_RETURN_CONTINUE_THROW;

    /* local in case jsdc->throwHook gets cleared on another thread */
    JSD_LOCK();
    hook     = jsdc->throwHook;
    hookData = jsdc->throwHookData;
    JSD_UNLOCK();
    if (!hook)
        return JSD_HOOK_RETURN_CONTINUE_THROW;

    JSD_LOCK_SCRIPTS(jsdc);
    jsdscript = jsd_FindOrCreateJSDScript(jsdc, cx, script, NULL);
    JSD_UNLOCK_SCRIPTS(jsdc);
    if( ! jsdscript )
        return JSD_HOOK_RETURN_CONTINUE_THROW;

    JS_GetPendingException(cx, rval);

    return jsd_CallExecutionHook(jsdc, cx, JSD_HOOK_THROW,
                                 hook, hookData, rval);
}

JSTrapStatus
jsd_CallExecutionHook(JSDContext* jsdc,
                      JSContext *cx,
                      uintN type,
                      JSD_ExecutionHookProc hook,
                      void* hookData,
                      jsval* rval)
{
    uintN hookanswer = JSD_HOOK_THROW == type ? 
                            JSD_HOOK_RETURN_CONTINUE_THROW :
                            JSD_HOOK_RETURN_CONTINUE;
    JSDThreadState* jsdthreadstate;

    if(hook && NULL != (jsdthreadstate = jsd_NewThreadState(jsdc,cx)))
    {
        if ((type != JSD_HOOK_THROW && type != JSD_HOOK_INTERRUPTED) ||
            jsdc->flags & JSD_MASK_TOP_FRAME_ONLY ||
            !(jsdthreadstate->flags & TS_HAS_DISABLED_FRAME))
        {
            /*
             * if it's not a throw and it's not an interrupt,
             * or we're only masking the top frame,
             * or there are no disabled frames in this stack,
             * then call out.
             */
             hookanswer = hook(jsdc, jsdthreadstate, type, hookData, rval);
             jsd_DestroyThreadState(jsdc, jsdthreadstate);
        }
    }

    switch(hookanswer)
    {
        case JSD_HOOK_RETURN_ABORT:
        case JSD_HOOK_RETURN_HOOK_ERROR:
            return JSTRAP_ERROR;
        case JSD_HOOK_RETURN_RET_WITH_VAL:
            return JSTRAP_RETURN;
        case JSD_HOOK_RETURN_THROW_WITH_VAL:
            return JSTRAP_THROW;
        case JSD_HOOK_RETURN_CONTINUE:
            break;
        case JSD_HOOK_RETURN_CONTINUE_THROW:
            /* only makes sense for jsd_ThrowHandler (which init'd rval) */
            JS_ASSERT(JSD_HOOK_THROW == type);
            return JSTRAP_THROW;
        default:
            JS_ASSERT(0);
            break;
    }
    return JSTRAP_CONTINUE;
}

JSBool
jsd_CallCallHook (JSDContext* jsdc,
                  JSContext *cx,
                  uintN type,
                  JSD_CallHookProc hook,
                  void* hookData)
{
    JSBool hookanswer;
    JSDThreadState*  jsdthreadstate;
    
    hookanswer = JS_FALSE;
    if(hook && NULL != (jsdthreadstate = jsd_NewThreadState(jsdc, cx)))
    {
        hookanswer = hook(jsdc, jsdthreadstate, type, hookData);
        jsd_DestroyThreadState(jsdc, jsdthreadstate);
    }

    return hookanswer;
}

JSBool
jsd_SetInterruptHook(JSDContext*           jsdc,
                     JSD_ExecutionHookProc hook,
                     void*                 callerdata)
{
    JSD_LOCK();
    jsdc->interruptHookData  = callerdata;
    jsdc->interruptHook      = hook;
    JS_SetInterrupt(jsdc->jsrt, jsd_InterruptHandler, (void*) jsdc);
    JSD_UNLOCK();

    return JS_TRUE;
}

JSBool
jsd_ClearInterruptHook(JSDContext* jsdc)
{
    JSD_LOCK();
    JS_ClearInterrupt(jsdc->jsrt, NULL, NULL );
    jsdc->interruptHook      = NULL;
    JSD_UNLOCK();

    return JS_TRUE;
}

JSBool
jsd_SetDebugBreakHook(JSDContext*           jsdc,
                      JSD_ExecutionHookProc hook,
                      void*                 callerdata)
{
    JSD_LOCK();
    jsdc->debugBreakHookData  = callerdata;
    jsdc->debugBreakHook      = hook;
    JSD_UNLOCK();

    return JS_TRUE;
}

JSBool
jsd_ClearDebugBreakHook(JSDContext* jsdc)
{
    JSD_LOCK();
    jsdc->debugBreakHook      = NULL;
    JSD_UNLOCK();

    return JS_TRUE;
}

JSBool
jsd_SetDebuggerHook(JSDContext*           jsdc,
                      JSD_ExecutionHookProc hook,
                      void*                 callerdata)
{
    JSD_LOCK();
    jsdc->debuggerHookData  = callerdata;
    jsdc->debuggerHook      = hook;
    JSD_UNLOCK();

    return JS_TRUE;
}

JSBool
jsd_ClearDebuggerHook(JSDContext* jsdc)
{
    JSD_LOCK();
    jsdc->debuggerHook      = NULL;
    JSD_UNLOCK();

    return JS_TRUE;
}

JSBool
jsd_SetThrowHook(JSDContext*           jsdc,
                 JSD_ExecutionHookProc hook,
                 void*                 callerdata)
{
    JSD_LOCK();
    jsdc->throwHookData  = callerdata;
    jsdc->throwHook      = hook;
    JSD_UNLOCK();

    return JS_TRUE;
}

JSBool
jsd_ClearThrowHook(JSDContext* jsdc)
{
    JSD_LOCK();
    jsdc->throwHook      = NULL;
    JSD_UNLOCK();

    return JS_TRUE;
}

JSBool
jsd_SetFunctionHook(JSDContext*      jsdc,
                    JSD_CallHookProc hook,
                    void*            callerdata)
{
    JSD_LOCK();
    jsdc->functionHookData  = callerdata;
    jsdc->functionHook      = hook;
    JSD_UNLOCK();

    return JS_TRUE;
}

JSBool
jsd_ClearFunctionHook(JSDContext* jsdc)
{
    JSD_LOCK();
    jsdc->functionHook      = NULL;
    JSD_UNLOCK();

    return JS_TRUE;
}

JSBool
jsd_SetTopLevelHook(JSDContext*      jsdc,
                    JSD_CallHookProc hook,
                    void*            callerdata)
{
    JSD_LOCK();
    jsdc->toplevelHookData  = callerdata;
    jsdc->toplevelHook      = hook;
    JSD_UNLOCK();

    return JS_TRUE;
}

JSBool
jsd_ClearTopLevelHook(JSDContext* jsdc)
{
    JSD_LOCK();
    jsdc->toplevelHook      = NULL;
    JSD_UNLOCK();

    return JS_TRUE;
}

