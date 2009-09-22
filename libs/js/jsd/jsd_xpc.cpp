/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
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
 *   Robert Ginda, <rginda@netscape.com>
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

#include "jsd_xpc.h"
#include "jsdbgapi.h"
#include "jscntxt.h"
#include "jsfun.h"

#include "nsIXPConnect.h"
#include "nsIGenericFactory.h"
#include "nsIServiceManager.h"
#include "nsIScriptGlobalObject.h"
#include "nsIObserver.h"
#include "nsIObserverService.h"
#include "nsICategoryManager.h"
#include "nsIJSRuntimeService.h"
#include "nsIThreadInternal.h"
#include "nsThreadUtils.h"
#include "nsMemory.h"
#include "jsdebug.h"
#include "nsReadableUtils.h"
#include "nsCRT.h"

/* XXX DOM dependency */
#include "nsIScriptContext.h"
#include "nsIJSContextStack.h"

/*
 * defining CAUTIOUS_SCRIPTHOOK makes jsds disable GC while calling out to the
 * script hook.  This was a hack to avoid some js engine problems that should
 * be fixed now (see Mozilla bug 77636).
 */
#undef CAUTIOUS_SCRIPTHOOK

#ifdef DEBUG_verbose
#   define DEBUG_COUNT(name, count)                                             \
        { if ((count % 10) == 0) printf (name ": %i\n", count); }
#   define DEBUG_CREATE(name, count) {count++; DEBUG_COUNT ("+++++ "name,count)}
#   define DEBUG_DESTROY(name, count) {count--; DEBUG_COUNT ("----- "name,count)}
#else
#   define DEBUG_CREATE(name, count) 
#   define DEBUG_DESTROY(name, count)
#endif

#define ASSERT_VALID_CONTEXT   { if (!mCx) return NS_ERROR_NOT_AVAILABLE; }
#define ASSERT_VALID_EPHEMERAL { if (!mValid) return NS_ERROR_NOT_AVAILABLE; }

#define JSDSERVICE_CID                               \
{ /* f1299dc2-1dd1-11b2-a347-ee6b7660e048 */         \
     0xf1299dc2,                                     \
     0x1dd1,                                         \
     0x11b2,                                         \
    {0xa3, 0x47, 0xee, 0x6b, 0x76, 0x60, 0xe0, 0x48} \
}

#define JSDASO_CID                                   \
{ /* 2fd6b7f6-eb8c-4f32-ad26-113f2c02d0fe */         \
     0x2fd6b7f6,                                     \
     0xeb8c,                                         \
     0x4f32,                                         \
    {0xad, 0x26, 0x11, 0x3f, 0x2c, 0x02, 0xd0, 0xfe} \
}

#define JSDS_MAJOR_VERSION 1
#define JSDS_MINOR_VERSION 2

#define NS_CATMAN_CTRID   "@mozilla.org/categorymanager;1"
#define NS_JSRT_CTRID     "@mozilla.org/js/xpc/RuntimeService;1"

#define AUTOREG_CATEGORY  "xpcom-autoregistration"
#define APPSTART_CATEGORY "app-startup"
#define JSD_AUTOREG_ENTRY "JSDebugger Startup Observer"
#define JSD_STARTUP_ENTRY "JSDebugger Startup Observer"

JS_STATIC_DLL_CALLBACK (JSBool)
jsds_GCCallbackProc (JSContext *cx, JSGCStatus status);

/*******************************************************************************
 * global vars
 ******************************************************************************/

const char implementationString[] = "Mozilla JavaScript Debugger Service";

const char jsdServiceCtrID[] = "@mozilla.org/js/jsd/debugger-service;1";
const char jsdARObserverCtrID[] = "@mozilla.org/js/jsd/app-start-observer;2";
const char jsdASObserverCtrID[] = "service,@mozilla.org/js/jsd/app-start-observer;2";

#ifdef DEBUG_verbose
PRUint32 gScriptCount   = 0;
PRUint32 gValueCount    = 0;
PRUint32 gPropertyCount = 0;
PRUint32 gContextCount  = 0;
PRUint32 gFrameCount  = 0;
#endif

static jsdService   *gJsds       = 0;
static JSGCCallback  gLastGCProc = jsds_GCCallbackProc;
static JSGCStatus    gGCStatus   = JSGC_END;

static struct DeadScript {
    PRCList     links;
    JSDContext *jsdc;
    jsdIScript *script;
} *gDeadScripts = nsnull;

enum PatternType {
    ptIgnore     = 0U,
    ptStartsWith = 1U,
    ptEndsWith   = 2U,
    ptContains   = 3U,
    ptEquals     = 4U
};

static struct FilterRecord {
    PRCList      links;
    jsdIFilter  *filterObject;
    void        *glob;
    char        *urlPattern;    
    PRUint32     patternLength;
    PatternType  patternType;
    PRUint32     startLine;
    PRUint32     endLine;
} *gFilters = nsnull;

static struct LiveEphemeral *gLiveValues      = nsnull;
static struct LiveEphemeral *gLiveProperties  = nsnull;
static struct LiveEphemeral *gLiveContexts    = nsnull;
static struct LiveEphemeral *gLiveStackFrames = nsnull;

/*******************************************************************************
 * utility functions for ephemeral lists
 *******************************************************************************/
already_AddRefed<jsdIEphemeral>
jsds_FindEphemeral (LiveEphemeral **listHead, void *key)
{
    if (!*listHead)
        return nsnull;
    
    LiveEphemeral *lv_record = 
        reinterpret_cast<LiveEphemeral *>
                        (PR_NEXT_LINK(&(*listHead)->links));
    do
    {
        if (lv_record->key == key)
        {
            NS_IF_ADDREF(lv_record->value);
            return lv_record->value;
        }
        lv_record = reinterpret_cast<LiveEphemeral *>
                                    (PR_NEXT_LINK(&lv_record->links));
    }
    while (lv_record != *listHead);

    return nsnull;
}

void
jsds_InvalidateAllEphemerals (LiveEphemeral **listHead)
{
    LiveEphemeral *lv_record = 
        reinterpret_cast<LiveEphemeral *>
                        (PR_NEXT_LINK(&(*listHead)->links));
    while (*listHead)
    {
        LiveEphemeral *next =
            reinterpret_cast<LiveEphemeral *>
                            (PR_NEXT_LINK(&lv_record->links));
        lv_record->value->Invalidate();
        lv_record = next;
    }
}

void
jsds_InsertEphemeral (LiveEphemeral **listHead, LiveEphemeral *item)
{
    if (*listHead) {
        /* if the list exists, add to it */
        PR_APPEND_LINK(&item->links, &(*listHead)->links);
    } else {
        /* otherwise create the list */
        PR_INIT_CLIST(&item->links);
        *listHead = item;
    }
}

void
jsds_RemoveEphemeral (LiveEphemeral **listHead, LiveEphemeral *item)
{
    LiveEphemeral *next = reinterpret_cast<LiveEphemeral *>
                                          (PR_NEXT_LINK(&item->links));

    if (next == item)
    {
        /* if the current item is also the next item, we're the only element,
         * null out the list head */
        NS_ASSERTION (*listHead == item,
                      "How could we not be the head of a one item list?");
        *listHead = nsnull;
    }
    else if (item == *listHead)
    {
        /* otherwise, if we're currently the list head, change it */
        *listHead = next;
    }
    
    PR_REMOVE_AND_INIT_LINK(&item->links);
}

/*******************************************************************************
 * utility functions for filters
 *******************************************************************************/
void
jsds_FreeFilter (FilterRecord *filter)
{
    NS_IF_RELEASE (filter->filterObject);
    if (filter->urlPattern)
        nsMemory::Free(filter->urlPattern);
    PR_Free (filter);
}

/* copies appropriate |filter| attributes into |rec|.
 * False return indicates failure, the contents of |rec| will not be changed.
 */
PRBool
jsds_SyncFilter (FilterRecord *rec, jsdIFilter *filter)
{
    NS_ASSERTION (rec, "jsds_SyncFilter without rec");
    NS_ASSERTION (filter, "jsds_SyncFilter without filter");
    
    JSObject *glob_proper = nsnull;
    nsCOMPtr<nsISupports> glob;
    nsresult rv = filter->GetGlobalObject(getter_AddRefs(glob));
    if (NS_FAILED(rv))
        return PR_FALSE;
    if (glob) {
        nsCOMPtr<nsIScriptGlobalObject> nsiglob = do_QueryInterface(glob);
        if (nsiglob)
            glob_proper = nsiglob->GetGlobalJSObject();
    }
    
    PRUint32 startLine;
    rv = filter->GetStartLine(&startLine);
    if (NS_FAILED(rv))
        return PR_FALSE;

    PRUint32 endLine;
    rv = filter->GetStartLine(&endLine);
    if (NS_FAILED(rv))
        return PR_FALSE;    

    char *urlPattern;
    rv = filter->GetUrlPattern (&urlPattern);
    if (NS_FAILED(rv))
        return PR_FALSE;
    
    if (urlPattern) {
        PRUint32 len = PL_strlen(urlPattern);
        if (urlPattern[0] == '*') {
            /* pattern starts with a *, shift all chars once to the left,
             * including the trailing null. */
            memmove (&urlPattern[0], &urlPattern[1], len);

            if (urlPattern[len - 2] == '*') {
                /* pattern is in the format "*foo*", overwrite the final * with
                 * a null. */
                urlPattern[len - 2] = '\0';
                rec->patternType = ptContains;
                rec->patternLength = len - 2;
            } else {
                /* pattern is in the format "*foo", just make a note of the
                 * new length. */
                rec->patternType = ptEndsWith;
                rec->patternLength = len - 1;
            }
        } else if (urlPattern[len - 1] == '*') {
            /* pattern is in the format "foo*", overwrite the final * with a 
             * null. */
            urlPattern[len - 1] = '\0';
            rec->patternType = ptStartsWith;
            rec->patternLength = len - 1;
        } else {
            /* pattern is in the format "foo". */
            rec->patternType = ptEquals;
            rec->patternLength = len;
        }
    } else {
        rec->patternType = ptIgnore;
        rec->patternLength = 0;
    }

    /* we got everything we need without failing, now copy it into rec. */

    if (rec->filterObject != filter) {
        NS_IF_RELEASE(rec->filterObject);
        NS_ADDREF(filter);
        rec->filterObject = filter;
    }
    
    rec->glob = glob_proper;
    
    rec->startLine     = startLine;
    rec->endLine       = endLine;
    
    if (rec->urlPattern)
        nsMemory::Free (rec->urlPattern);
    rec->urlPattern = urlPattern;

    return PR_TRUE;
            
}

FilterRecord *
jsds_FindFilter (jsdIFilter *filter)
{
    if (!gFilters)
        return nsnull;
    
    FilterRecord *current = gFilters;
    
    do {
        if (current->filterObject == filter)
            return current;
        current = reinterpret_cast<FilterRecord *>
                                  (PR_NEXT_LINK(&current->links));
    } while (current != gFilters);
    
    return nsnull;
}

/* returns true if the hook should be executed. */
PRBool
jsds_FilterHook (JSDContext *jsdc, JSDThreadState *state)
{
    JSContext *cx = JSD_GetJSContext (jsdc, state);
    void *glob = static_cast<void *>(JS_GetGlobalObject (cx));

    if (!glob) {
        NS_WARNING("No global in threadstate");
        return PR_FALSE;
    }
    
    JSDStackFrameInfo *frame = JSD_GetStackFrame (jsdc, state);

    if (!frame) {
        NS_WARNING("No frame in threadstate");
        return PR_FALSE;
    }

    JSDScript *script = JSD_GetScriptForStackFrame (jsdc, state, frame);
    if (!script)
        return PR_TRUE;

    jsuint pc = JSD_GetPCForStackFrame (jsdc, state, frame);

    const char *url = JSD_GetScriptFilename (jsdc, script);
    if (!url) {
        NS_WARNING ("Script with no filename");
        return PR_FALSE;
    }

    if (!gFilters)
        return PR_TRUE;    

    PRUint32 currentLine = JSD_GetClosestLine (jsdc, script, pc);
    PRUint32 len = 0;
    FilterRecord *currentFilter = gFilters;
    do {
        PRUint32 flags = 0;
        nsresult rv = currentFilter->filterObject->GetFlags(&flags);
        NS_ASSERTION(NS_SUCCEEDED(rv), "Error getting flags for filter");
        if (flags & jsdIFilter::FLAG_ENABLED) {
            /* if there is no glob, or the globs match */
            if ((!currentFilter->glob || currentFilter->glob == glob) &&
                /* and there is no start line, or the start line is before 
                 * or equal to the current */
                (!currentFilter->startLine || 
                 currentFilter->startLine <= currentLine) &&
                /* and there is no end line, or the end line is after
                 * or equal to the current */
                (!currentFilter->endLine ||
                 currentFilter->endLine >= currentLine)) {
                /* then we're going to have to compare the url. */
                if (currentFilter->patternType == ptIgnore)
                    return flags & jsdIFilter::FLAG_PASS;

                if (!len)
                    len = PL_strlen(url);
                
                if (len >= currentFilter->patternLength) {
                    switch (currentFilter->patternType) {
                        case ptEquals:
                            if (!PL_strcmp(currentFilter->urlPattern, url))
                                return flags & jsdIFilter::FLAG_PASS;
                            break;
                        case ptStartsWith:
                            if (!PL_strncmp(currentFilter->urlPattern, url, 
                                           currentFilter->patternLength))
                                return flags & jsdIFilter::FLAG_PASS;
                            break;
                        case ptEndsWith:
                            if (!PL_strcmp(currentFilter->urlPattern,
                                           &url[len - 
                                               currentFilter->patternLength]))
                                return flags & jsdIFilter::FLAG_PASS;
                            break;
                        case ptContains:
                            if (PL_strstr(url, currentFilter->urlPattern))
                                return flags & jsdIFilter::FLAG_PASS;
                            break;
                        default:
                            NS_ASSERTION(0, "Invalid pattern type");
                    }
                }                
            }
        }
        currentFilter = reinterpret_cast<FilterRecord *>
                                        (PR_NEXT_LINK(&currentFilter->links));
    } while (currentFilter != gFilters);

    return PR_TRUE;
    
}

/*******************************************************************************
 * c callbacks
 *******************************************************************************/

JS_STATIC_DLL_CALLBACK (void)
jsds_NotifyPendingDeadScripts (JSContext *cx)
{
#ifdef CAUTIOUS_SCRIPTHOOK
    JSRuntime *rt = JS_GetRuntime(cx);
#endif
    jsdService *jsds = gJsds;

    nsCOMPtr<jsdIScriptHook> hook;
    if (jsds) {
        NS_ADDREF(jsds);
        jsds->GetScriptHook (getter_AddRefs(hook));
        jsds->Pause(nsnull);
    }

    DeadScript *deadScripts = gDeadScripts;
    gDeadScripts = nsnull;
    while (deadScripts) {
        DeadScript *ds = deadScripts;
        /* get next deleted script */
        deadScripts = reinterpret_cast<DeadScript *>
                                       (PR_NEXT_LINK(&ds->links));
        if (deadScripts == ds)
            deadScripts = nsnull;

        if (hook)
        {
            /* tell the user this script has been destroyed */
#ifdef CAUTIOUS_SCRIPTHOOK
            JS_UNKEEP_ATOMS(rt);
#endif
            hook->OnScriptDestroyed (ds->script);
#ifdef CAUTIOUS_SCRIPTHOOK
            JS_KEEP_ATOMS(rt);
#endif
        }

        /* take it out of the circular list */
        PR_REMOVE_LINK(&ds->links);

        /* addref came from the FromPtr call in jsds_ScriptHookProc */
        NS_RELEASE(ds->script);
        /* free the struct! */
        PR_Free(ds);
    }

    if (jsds) {
        jsds->UnPause(nsnull);
        NS_RELEASE(jsds);
    }
}

JS_STATIC_DLL_CALLBACK (JSBool)
jsds_GCCallbackProc (JSContext *cx, JSGCStatus status)
{
#ifdef DEBUG_verbose
    printf ("new gc status is %i\n", status);
#endif
    if (status == JSGC_END) {
        /* just to guard against reentering. */
        gGCStatus = JSGC_BEGIN;
        while (gDeadScripts)
            jsds_NotifyPendingDeadScripts (cx);
    }

    gGCStatus = status;
    if (gLastGCProc)
        return gLastGCProc (cx, status);
    
    return JS_TRUE;
}

JS_STATIC_DLL_CALLBACK (uintN)
jsds_ErrorHookProc (JSDContext *jsdc, JSContext *cx, const char *message,
                    JSErrorReport *report, void *callerdata)
{
    static PRBool running = PR_FALSE;

    nsCOMPtr<jsdIErrorHook> hook;
    gJsds->GetErrorHook(getter_AddRefs(hook));
    if (!hook)
        return JSD_ERROR_REPORTER_PASS_ALONG;

    if (running)
        return JSD_ERROR_REPORTER_PASS_ALONG;
    
    running = PR_TRUE;
    
    nsCOMPtr<jsdIValue> val;
    if (JS_IsExceptionPending(cx)) {
        jsval jv;
        JS_GetPendingException(cx, &jv);
        JSDValue *jsdv = JSD_NewValue (jsdc, jv);
        val = getter_AddRefs(jsdValue::FromPtr(jsdc, jsdv));
    }
    
    const char *fileName;
    PRUint32    line;
    PRUint32    pos;
    PRUint32    flags;
    PRUint32    errnum;
    PRBool      rval;
    if (report) {
        fileName = report->filename;
        line = report->lineno;
        pos = report->tokenptr - report->linebuf;
        flags = report->flags;
        errnum = report->errorNumber;
    }
    else
    {
        fileName = 0;
        line     = 0;
        pos      = 0;
        flags    = 0;
        errnum   = 0;
    }
    
    gJsds->Pause(nsnull);
    hook->OnError (message, fileName, line, pos, flags, errnum, val, &rval);
    gJsds->UnPause(nsnull);
    
    running = PR_FALSE;
    if (!rval)
        return JSD_ERROR_REPORTER_DEBUG;
    
    return JSD_ERROR_REPORTER_PASS_ALONG;
}

JS_STATIC_DLL_CALLBACK (JSBool)
jsds_CallHookProc (JSDContext* jsdc, JSDThreadState* jsdthreadstate,
                   uintN type, void* callerdata)
{
    nsCOMPtr<jsdICallHook> hook;

    switch (type)
    {
        case JSD_HOOK_TOPLEVEL_START:
        case JSD_HOOK_TOPLEVEL_END:
            gJsds->GetTopLevelHook(getter_AddRefs(hook));
            break;
            
        case JSD_HOOK_FUNCTION_CALL:
        case JSD_HOOK_FUNCTION_RETURN:
            gJsds->GetFunctionHook(getter_AddRefs(hook));
            break;

        default:
            NS_ASSERTION (0, "Unknown hook type.");
    }
    
    if (!hook)
        return JS_TRUE;

    if (!jsds_FilterHook (jsdc, jsdthreadstate))
        return JS_FALSE;

    JSDStackFrameInfo *native_frame = JSD_GetStackFrame (jsdc, jsdthreadstate);
    nsCOMPtr<jsdIStackFrame> frame =
        getter_AddRefs(jsdStackFrame::FromPtr(jsdc, jsdthreadstate,
                                              native_frame));
    gJsds->Pause(nsnull);
    hook->OnCall(frame, type);    
    gJsds->UnPause(nsnull);
    jsdStackFrame::InvalidateAll();

    return JS_TRUE;
}

JS_STATIC_DLL_CALLBACK (PRUint32)
jsds_ExecutionHookProc (JSDContext* jsdc, JSDThreadState* jsdthreadstate,
                        uintN type, void* callerdata, jsval* rval)
{
    nsCOMPtr<jsdIExecutionHook> hook(0);
    PRUint32 hook_rv = JSD_HOOK_RETURN_CONTINUE;
    nsCOMPtr<jsdIValue> js_rv;

    switch (type)
    {
        case JSD_HOOK_INTERRUPTED:
            gJsds->GetInterruptHook(getter_AddRefs(hook));
            break;
        case JSD_HOOK_DEBUG_REQUESTED:
            gJsds->GetDebugHook(getter_AddRefs(hook));
            break;
        case JSD_HOOK_DEBUGGER_KEYWORD:
            gJsds->GetDebuggerHook(getter_AddRefs(hook));
            break;
        case JSD_HOOK_BREAKPOINT:
            {
                /* we can't pause breakpoints the way we pause the other
                 * execution hooks (at least, not easily.)  Instead we bail
                 * here if the service is paused. */
                PRUint32 level;
                gJsds->GetPauseDepth(&level);
                if (!level)
                    gJsds->GetBreakpointHook(getter_AddRefs(hook));
            }
            break;
        case JSD_HOOK_THROW:
        {
            hook_rv = JSD_HOOK_RETURN_CONTINUE_THROW;
            gJsds->GetThrowHook(getter_AddRefs(hook));
            if (hook) {
                JSDValue *jsdv = JSD_GetException (jsdc, jsdthreadstate);
                js_rv = getter_AddRefs(jsdValue::FromPtr (jsdc, jsdv));
            }
            break;
        }
        default:
            NS_ASSERTION (0, "Unknown hook type.");
    }

    if (!hook)
        return hook_rv;
    
    if (!jsds_FilterHook (jsdc, jsdthreadstate))
        return JSD_HOOK_RETURN_CONTINUE;
    
    JSDStackFrameInfo *native_frame = JSD_GetStackFrame (jsdc, jsdthreadstate);
    nsCOMPtr<jsdIStackFrame> frame =
        getter_AddRefs(jsdStackFrame::FromPtr(jsdc, jsdthreadstate,
                                              native_frame));
    gJsds->Pause(nsnull);
    jsdIValue *inout_rv = js_rv;
    NS_IF_ADDREF(inout_rv);
    hook->OnExecute (frame, type, &inout_rv, &hook_rv);
    js_rv = inout_rv;
    NS_IF_RELEASE(inout_rv);
    gJsds->UnPause(nsnull);
    jsdStackFrame::InvalidateAll();
        
    if (hook_rv == JSD_HOOK_RETURN_RET_WITH_VAL ||
        hook_rv == JSD_HOOK_RETURN_THROW_WITH_VAL) {
        *rval = JSVAL_VOID;
        if (js_rv) {
            JSDValue *jsdv;
            if (NS_SUCCEEDED(js_rv->GetJSDValue (&jsdv)))
                *rval = JSD_GetValueWrappedJSVal(jsdc, jsdv);
        }
    }
    
    return hook_rv;
}

JS_STATIC_DLL_CALLBACK (void)
jsds_ScriptHookProc (JSDContext* jsdc, JSDScript* jsdscript, JSBool creating,
                     void* callerdata)
{
#ifdef CAUTIOUS_SCRIPTHOOK
    JSContext *cx = JSD_GetDefaultJSContext(jsdc);
    JSRuntime *rt = JS_GetRuntime(cx);
#endif

    nsCOMPtr<jsdIScriptHook> hook;
    gJsds->GetScriptHook (getter_AddRefs(hook));
    
    if (creating) {
        /* a script is being created */
        if (!hook) {
            /* nobody cares, just exit */
            return;
        }
            
        nsCOMPtr<jsdIScript> script = 
            getter_AddRefs(jsdScript::FromPtr(jsdc, jsdscript));
#ifdef CAUTIOUS_SCRIPTHOOK
        JS_UNKEEP_ATOMS(rt);
#endif
        gJsds->Pause(nsnull);
        hook->OnScriptCreated (script);
        gJsds->UnPause(nsnull);
#ifdef CAUTIOUS_SCRIPTHOOK
        JS_KEEP_ATOMS(rt);
#endif
    } else {
        /* a script is being destroyed.  even if there is no registered hook
         * we'll still need to invalidate the jsdIScript record, in order
         * to remove the reference held in the JSDScript private data. */
        nsCOMPtr<jsdIScript> jsdis = 
            static_cast<jsdIScript *>(JSD_GetScriptPrivate(jsdscript));
        if (!jsdis)
            return;
        
        jsdis->Invalidate();
        if (!hook)
            return;
        
        if (gGCStatus == JSGC_END) {
            /* if GC *isn't* running, we can tell the user about the script
             * delete now. */
#ifdef CAUTIOUS_SCRIPTHOOK
            JS_UNKEEP_ATOMS(rt);
#endif
                
            gJsds->Pause(nsnull);
            hook->OnScriptDestroyed (jsdis);
            gJsds->UnPause(nsnull);
#ifdef CAUTIOUS_SCRIPTHOOK
            JS_KEEP_ATOMS(rt);
#endif
        } else {
            /* if a GC *is* running, we've got to wait until it's done before
             * we can execute any JS, so we queue the notification in a PRCList
             * until GC tells us it's done. See jsds_GCCallbackProc(). */
            DeadScript *ds = PR_NEW(DeadScript);
            if (!ds)
                return; /* NS_ERROR_OUT_OF_MEMORY */
        
            ds->jsdc = jsdc;
            ds->script = jsdis;
            NS_ADDREF(ds->script);
            if (gDeadScripts)
                /* if the queue exists, add to it */
                PR_APPEND_LINK(&ds->links, &gDeadScripts->links);
            else {
                /* otherwise create the queue */
                PR_INIT_CLIST(&ds->links);
                gDeadScripts = ds;
            }
        }
    }            
}

/*******************************************************************************
 * reflected jsd data structures
 *******************************************************************************/

/* Contexts */
/*
NS_IMPL_THREADSAFE_ISUPPORTS2(jsdContext, jsdIContext, jsdIEphemeral);

NS_IMETHODIMP
jsdContext::GetJSDContext(JSDContext **_rval)
{
    *_rval = mCx;
    return NS_OK;
}
*/

/* Objects */
NS_IMPL_THREADSAFE_ISUPPORTS1(jsdObject, jsdIObject)

NS_IMETHODIMP
jsdObject::GetJSDContext(JSDContext **_rval)
{
    *_rval = mCx;
    return NS_OK;
}

NS_IMETHODIMP
jsdObject::GetJSDObject(JSDObject **_rval)
{
    *_rval = mObject;
    return NS_OK;
}

NS_IMETHODIMP
jsdObject::GetCreatorURL(char **_rval)
{
    const char *url = JSD_GetObjectNewURL(mCx, mObject);
    if (url) {
        *_rval = PL_strdup(url);
        if (!*_rval)
            return NS_ERROR_OUT_OF_MEMORY;
    } else {
        *_rval = nsnull;
    }
    return NS_OK;
}

NS_IMETHODIMP
jsdObject::GetCreatorLine(PRUint32 *_rval)
{
    *_rval = JSD_GetObjectNewLineNumber(mCx, mObject);
    return NS_OK;
}

NS_IMETHODIMP
jsdObject::GetConstructorURL(char **_rval)
{
    const char *url = JSD_GetObjectConstructorURL(mCx, mObject);
    if (url) {
        *_rval = PL_strdup(url);
        if (!*_rval)
            return NS_ERROR_OUT_OF_MEMORY;
    } else {
        *_rval = nsnull;
    }
    return NS_OK;
}

NS_IMETHODIMP
jsdObject::GetConstructorLine(PRUint32 *_rval)
{
    *_rval = JSD_GetObjectConstructorLineNumber(mCx, mObject);
    return NS_OK;
}

NS_IMETHODIMP
jsdObject::GetValue(jsdIValue **_rval)
{
    JSDValue *jsdv = JSD_GetValueForObject (mCx, mObject);
    
    *_rval = jsdValue::FromPtr (mCx, jsdv);
    return NS_OK;
}

/* Properties */
NS_IMPL_THREADSAFE_ISUPPORTS2(jsdProperty, jsdIProperty, jsdIEphemeral)

jsdProperty::jsdProperty (JSDContext *aCx, JSDProperty *aProperty) :
    mCx(aCx), mProperty(aProperty)
{
    DEBUG_CREATE ("jsdProperty", gPropertyCount);
    mValid = (aCx && aProperty);
    mLiveListEntry.value = this;
    jsds_InsertEphemeral (&gLiveProperties, &mLiveListEntry);
}

jsdProperty::~jsdProperty () 
{
    DEBUG_DESTROY ("jsdProperty", gPropertyCount);
    if (mValid)
        Invalidate();
}

NS_IMETHODIMP
jsdProperty::Invalidate()
{
    ASSERT_VALID_EPHEMERAL;
    mValid = PR_FALSE;
    jsds_RemoveEphemeral (&gLiveProperties, &mLiveListEntry);
    JSD_DropProperty (mCx, mProperty);
    return NS_OK;
}

void
jsdProperty::InvalidateAll()
{
    if (gLiveProperties)
        jsds_InvalidateAllEphemerals (&gLiveProperties);
}

NS_IMETHODIMP
jsdProperty::GetJSDContext(JSDContext **_rval)
{
    *_rval = mCx;
    return NS_OK;
}

NS_IMETHODIMP
jsdProperty::GetJSDProperty(JSDProperty **_rval)
{
    *_rval = mProperty;
    return NS_OK;
}

NS_IMETHODIMP
jsdProperty::GetIsValid(PRBool *_rval)
{
    *_rval = mValid;
    return NS_OK;
}

NS_IMETHODIMP
jsdProperty::GetAlias(jsdIValue **_rval)
{
    JSDValue *jsdv = JSD_GetPropertyValue (mCx, mProperty);
    
    *_rval = jsdValue::FromPtr (mCx, jsdv);
    return NS_OK;
}

NS_IMETHODIMP
jsdProperty::GetFlags(PRUint32 *_rval)
{
    *_rval = JSD_GetPropertyFlags (mCx, mProperty);
    return NS_OK;
}

NS_IMETHODIMP
jsdProperty::GetName(jsdIValue **_rval)
{
    JSDValue *jsdv = JSD_GetPropertyName (mCx, mProperty);
    
    *_rval = jsdValue::FromPtr (mCx, jsdv);
    return NS_OK;
}

NS_IMETHODIMP
jsdProperty::GetValue(jsdIValue **_rval)
{
    JSDValue *jsdv = JSD_GetPropertyValue (mCx, mProperty);
    
    *_rval = jsdValue::FromPtr (mCx, jsdv);
    return NS_OK;
}

NS_IMETHODIMP
jsdProperty::GetVarArgSlot(PRUint32 *_rval)
{
    *_rval = JSD_GetPropertyVarArgSlot (mCx, mProperty);
    return NS_OK;
}

/* Scripts */
NS_IMPL_THREADSAFE_ISUPPORTS2(jsdScript, jsdIScript, jsdIEphemeral)

jsdScript::jsdScript (JSDContext *aCx, JSDScript *aScript) : mValid(PR_FALSE),
                                                             mTag(0),
                                                             mCx(aCx),
                                                             mScript(aScript),
                                                             mFileName(0), 
                                                             mFunctionName(0),
                                                             mBaseLineNumber(0),
                                                             mLineExtent(0),
                                                             mPPLineMap(0),
                                                             mFirstPC(0)
{
    DEBUG_CREATE ("jsdScript", gScriptCount);

    if (mScript) {
        /* copy the script's information now, so we have it later, when it
         * gets destroyed. */
        JSD_LockScriptSubsystem(mCx);
        mFileName = new nsCString(JSD_GetScriptFilename(mCx, mScript));
        mFunctionName =
            new nsCString(JSD_GetScriptFunctionName(mCx, mScript));
        mBaseLineNumber = JSD_GetScriptBaseLineNumber(mCx, mScript);
        mLineExtent = JSD_GetScriptLineExtent(mCx, mScript);
        mFirstPC = JSD_GetClosestPC(mCx, mScript, 0);
        JSD_UnlockScriptSubsystem(mCx);
        
        mValid = PR_TRUE;
    }
}

jsdScript::~jsdScript () 
{
    DEBUG_DESTROY ("jsdScript", gScriptCount);
    if (mFileName)
        delete mFileName;
    if (mFunctionName)
        delete mFunctionName;

    if (mPPLineMap)
        PR_Free(mPPLineMap);

    /* Invalidate() needs to be called to release an owning reference to
     * ourselves, so if we got here without being invalidated, something
     * has gone wrong with our ref count. */
    NS_ASSERTION (!mValid, "Script destroyed without being invalidated.");
}

/*
 * This method populates a line <-> pc map for a pretty printed version of this
 * script.  It does this by decompiling, and then recompiling the script.  The
 * resulting script is scanned for the line map, and then left as GC fodder.
 */
PCMapEntry *
jsdScript::CreatePPLineMap()
{    
    JSContext  *cx  = JSD_GetDefaultJSContext (mCx);
    JSAutoRequest ar(cx);
    JSObject   *obj = JS_NewObject(cx, NULL, NULL, NULL);
    JSFunction *fun = JSD_GetJSFunction (mCx, mScript);
    JSScript   *script;
    PRUint32    baseLine;
    PRBool      scriptOwner = PR_FALSE;
    
    if (fun) {
        if (fun->nargs > 12)
            return nsnull;
        JSString *jsstr = JS_DecompileFunctionBody (cx, fun, 4);
        if (!jsstr)
            return nsnull;
    
        const char *argnames[] = {"arg1", "arg2", "arg3", "arg4", 
                                  "arg5", "arg6", "arg7", "arg8",
                                  "arg9", "arg10", "arg11", "arg12" };
        fun = JS_CompileUCFunction (cx, obj, "ppfun", fun->nargs, argnames,
                                    JS_GetStringChars(jsstr),
                                    JS_GetStringLength(jsstr),
                                    "x-jsd:ppbuffer?type=function", 3);
        if (!fun || !(script = JS_GetFunctionScript(cx, fun)))
            return nsnull;
        baseLine = 3;
    } else {
        JSString *jsstr = JS_DecompileScript (cx, JSD_GetJSScript(mCx, mScript),
                                              "ppscript", 4);
        if (!jsstr)
            return nsnull;

        script = JS_CompileUCScript (cx, obj,
                                     JS_GetStringChars(jsstr),
                                     JS_GetStringLength(jsstr),
                                     "x-jsd:ppbuffer?type=script", 1);
        if (!script)
            return nsnull;
        scriptOwner = PR_TRUE;
        baseLine = 1;
    }
        
    PRUint32 scriptExtent = JS_GetScriptLineExtent (cx, script);
    jsbytecode* firstPC = JS_LineNumberToPC (cx, script, 0);
    /* allocate worst case size of map (number of lines in script + 1
     * for our 0 record), we'll shrink it with a realloc later. */
    mPPLineMap = 
        static_cast<PCMapEntry *>
                   (PR_Malloc((scriptExtent + 1) * sizeof (PCMapEntry)));
    if (mPPLineMap) {             
        mPCMapSize = 0;
        for (PRUint32 line = baseLine; line < scriptExtent + baseLine; ++line) {
            jsbytecode* pc = JS_LineNumberToPC (cx, script, line);
            if (line == JS_PCToLineNumber (cx, script, pc)) {
                mPPLineMap[mPCMapSize].line = line;
                mPPLineMap[mPCMapSize].pc = pc - firstPC;
                ++mPCMapSize;
            }
        }
        if (scriptExtent != mPCMapSize) {
            mPPLineMap =
                static_cast<PCMapEntry *>
                           (PR_Realloc(mPPLineMap,
                                          mPCMapSize * sizeof(PCMapEntry)));
        }
    }

    if (scriptOwner)
        JS_DestroyScript (cx, script);

    return mPPLineMap;
}

PRUint32
jsdScript::PPPcToLine (PRUint32 aPC)
{
    if (!mPPLineMap && !CreatePPLineMap())
        return 0;
    PRUint32 i;
    for (i = 1; i < mPCMapSize; ++i) {
        if (mPPLineMap[i].pc > aPC)
            return mPPLineMap[i - 1].line;            
    }

    return mPPLineMap[mPCMapSize - 1].line;
}

PRUint32
jsdScript::PPLineToPc (PRUint32 aLine)
{
    if (!mPPLineMap && !CreatePPLineMap())
        return 0;
    PRUint32 i;
    for (i = 1; i < mPCMapSize; ++i) {
        if (mPPLineMap[i].line > aLine)
            return mPPLineMap[i - 1].pc;
    }

    return mPPLineMap[mPCMapSize - 1].pc;
}

NS_IMETHODIMP
jsdScript::GetJSDContext(JSDContext **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = mCx;
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetJSDScript(JSDScript **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = mScript;
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetVersion (PRInt32 *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSContext *cx = JSD_GetDefaultJSContext (mCx);
    JSScript *script = JSD_GetJSScript(mCx, mScript);
    *_rval = static_cast<PRInt32>(JS_GetScriptVersion(cx, script));
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetTag(PRUint32 *_rval)
{
    if (!mTag)
        mTag = ++jsdScript::LastTag;
    
    *_rval = mTag;
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::Invalidate()
{
    ASSERT_VALID_EPHEMERAL;
    mValid = PR_FALSE;
    
    /* release the addref we do in FromPtr */
    jsdIScript *script = static_cast<jsdIScript *>
                                    (JSD_GetScriptPrivate(mScript));
    NS_ASSERTION (script == this, "That's not my script!");
    NS_RELEASE(script);
    JSD_SetScriptPrivate(mScript, NULL);
    return NS_OK;
}

void
jsdScript::InvalidateAll ()
{
    JSDContext *cx;
    if (NS_FAILED(gJsds->GetJSDContext (&cx)))
        return;

    JSDScript *script;
    JSDScript *iter = NULL;
    
    JSD_LockScriptSubsystem(cx);
    while((script = JSD_IterateScripts(cx, &iter)) != NULL) {
        nsCOMPtr<jsdIScript> jsdis = 
            static_cast<jsdIScript *>(JSD_GetScriptPrivate(script));
        if (jsdis)
            jsdis->Invalidate();
    }
    JSD_UnlockScriptSubsystem(cx);
}

NS_IMETHODIMP
jsdScript::GetIsValid(PRBool *_rval)
{
    *_rval = mValid;
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::SetFlags(PRUint32 flags)
{
    ASSERT_VALID_EPHEMERAL;
    JSD_SetScriptFlags(mCx, mScript, flags);
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetFlags(PRUint32 *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_GetScriptFlags(mCx, mScript);
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetFileName(char **_rval)
{
    *_rval = ToNewCString(*mFileName);
    if (!*_rval)
        return NS_ERROR_OUT_OF_MEMORY;
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetFunctionName(char **_rval)
{
    *_rval = ToNewCString(*mFunctionName);
    if (!*_rval)
        return NS_ERROR_OUT_OF_MEMORY;
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetFunctionObject(jsdIValue **_rval)
{
    JSFunction *fun = JSD_GetJSFunction(mCx, mScript);
    if (!fun)
        return NS_ERROR_NOT_AVAILABLE;
    
    JSObject *obj = JS_GetFunctionObject(fun);
    if (!obj)
        return NS_ERROR_FAILURE;

    JSDContext *cx;
    if (NS_FAILED(gJsds->GetJSDContext (&cx)))
        return NS_ERROR_NOT_INITIALIZED;

    JSDValue *jsdv = JSD_NewValue(cx, OBJECT_TO_JSVAL(obj));
    if (!jsdv)
        return NS_ERROR_OUT_OF_MEMORY;

    *_rval = jsdValue::FromPtr(cx, jsdv);
    if (!*_rval) {
        JSD_DropValue(cx, jsdv);
        return NS_ERROR_OUT_OF_MEMORY;
    }

    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetFunctionSource(nsAString & aFunctionSource)
{
    ASSERT_VALID_EPHEMERAL;
    JSContext *cx = JSD_GetDefaultJSContext (mCx);
    if (!cx) {
        NS_WARNING("No default context !?");
        return NS_ERROR_FAILURE;
    }
    JSFunction *fun = JSD_GetJSFunction (mCx, mScript);

    JSAutoRequest ar(cx);

    JSString *jsstr;
    if (fun)
        jsstr = JS_DecompileFunction (cx, fun, 4);
    else {
        JSScript *script = JSD_GetJSScript (mCx, mScript);
        jsstr = JS_DecompileScript (cx, script, "ppscript", 4);
    }
    if (!jsstr)
        return NS_ERROR_FAILURE;

    aFunctionSource = reinterpret_cast<PRUnichar*>(JS_GetStringChars(jsstr));
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetBaseLineNumber(PRUint32 *_rval)
{
    *_rval = mBaseLineNumber;
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetLineExtent(PRUint32 *_rval)
{
    *_rval = mLineExtent;
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetCallCount(PRUint32 *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_GetScriptCallCount (mCx, mScript);
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetMaxRecurseDepth(PRUint32 *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_GetScriptMaxRecurseDepth (mCx, mScript);
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetMinExecutionTime(double *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_GetScriptMinExecutionTime (mCx, mScript);
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetMaxExecutionTime(double *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_GetScriptMaxExecutionTime (mCx, mScript);
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetTotalExecutionTime(double *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_GetScriptTotalExecutionTime (mCx, mScript);
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetMinOwnExecutionTime(double *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_GetScriptMinOwnExecutionTime (mCx, mScript);
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetMaxOwnExecutionTime(double *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_GetScriptMaxOwnExecutionTime (mCx, mScript);
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::GetTotalOwnExecutionTime(double *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_GetScriptTotalOwnExecutionTime (mCx, mScript);
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::ClearProfileData()
{
    ASSERT_VALID_EPHEMERAL;
    JSD_ClearScriptProfileData(mCx, mScript);
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::PcToLine(PRUint32 aPC, PRUint32 aPcmap, PRUint32 *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    if (aPcmap == PCMAP_SOURCETEXT) {
        *_rval = JSD_GetClosestLine (mCx, mScript, mFirstPC + aPC);
    } else if (aPcmap == PCMAP_PRETTYPRINT) {
        *_rval = PPPcToLine(aPC);
    } else {
        return NS_ERROR_INVALID_ARG;
    }
    
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::LineToPc(PRUint32 aLine, PRUint32 aPcmap, PRUint32 *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    if (aPcmap == PCMAP_SOURCETEXT) {
        jsuword pc = JSD_GetClosestPC (mCx, mScript, aLine);
        *_rval = pc - mFirstPC;
    } else if (aPcmap == PCMAP_PRETTYPRINT) {
        *_rval = PPLineToPc(aLine);
    } else {
        return NS_ERROR_INVALID_ARG;
    }

    return NS_OK;
}

NS_IMETHODIMP
jsdScript::IsLineExecutable(PRUint32 aLine, PRUint32 aPcmap, PRBool *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    if (aPcmap == PCMAP_SOURCETEXT) {    
        jsuword pc = JSD_GetClosestPC (mCx, mScript, aLine);
        *_rval = (aLine == JSD_GetClosestLine (mCx, mScript, pc));
    } else if (aPcmap == PCMAP_PRETTYPRINT) {
        if (!mPPLineMap && !CreatePPLineMap())
            return NS_ERROR_FAILURE;
        *_rval = PR_FALSE;
        for (PRUint32 i = 0; i < mPCMapSize; ++i) {
            if (mPPLineMap[i].line >= aLine) {
                *_rval = (mPPLineMap[i].line == aLine);
                break;
            }
        }
    } else {
        return NS_ERROR_INVALID_ARG;
    }
    
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::SetBreakpoint(PRUint32 aPC)
{
    ASSERT_VALID_EPHEMERAL;
    jsuword pc = mFirstPC + aPC;
    JSD_SetExecutionHook (mCx, mScript, pc, jsds_ExecutionHookProc,
                          reinterpret_cast<void *>(PRIVATE_TO_JSVAL(NULL)));
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::ClearBreakpoint(PRUint32 aPC)
{
    ASSERT_VALID_EPHEMERAL;    
    jsuword pc = mFirstPC + aPC;
    JSD_ClearExecutionHook (mCx, mScript, pc);
    return NS_OK;
}

NS_IMETHODIMP
jsdScript::ClearAllBreakpoints()
{
    ASSERT_VALID_EPHEMERAL;
    JSD_LockScriptSubsystem(mCx);
    JSD_ClearAllExecutionHooksForScript (mCx, mScript);
    JSD_UnlockScriptSubsystem(mCx);
    return NS_OK;
}

/* Contexts */
NS_IMPL_THREADSAFE_ISUPPORTS2(jsdContext, jsdIContext, jsdIEphemeral)

jsdIContext *
jsdContext::FromPtr (JSDContext *aJSDCx, JSContext *aJSCx)
{
    if (!aJSDCx || !aJSCx ||
        !(JS_GetOptions(aJSCx) & JSOPTION_PRIVATE_IS_NSISUPPORTS))
    {
        return nsnull;
    }
    
    nsCOMPtr<jsdIContext> jsdicx;
    nsCOMPtr<jsdIEphemeral> eph = 
        jsds_FindEphemeral (&gLiveContexts, static_cast<void *>(aJSCx));
    if (eph)
    {
        jsdicx = do_QueryInterface(eph);
    }
    else
    {
        nsCOMPtr<nsISupports> iscx = 
            static_cast<nsISupports *>(JS_GetContextPrivate(aJSCx));
        if (!iscx)
            return nsnull;
        
        jsdicx = new jsdContext (aJSDCx, aJSCx, iscx);
    }

    jsdIContext *rv = jsdicx;
    NS_IF_ADDREF(rv);
    return rv;
}

jsdContext::jsdContext (JSDContext *aJSDCx, JSContext *aJSCx,
                        nsISupports *aISCx) : mValid(PR_TRUE), mTag(0),
                                              mJSDCx(aJSDCx),
                                              mJSCx(aJSCx), mISCx(aISCx)
{
    DEBUG_CREATE ("jsdContext", gContextCount);
    mLiveListEntry.value = this;
    mLiveListEntry.key   = static_cast<void *>(aJSCx);
    jsds_InsertEphemeral (&gLiveContexts, &mLiveListEntry);
}

jsdContext::~jsdContext() 
{
    DEBUG_DESTROY ("jsdContext", gContextCount);
    if (mValid)
    {
        /* call Invalidate() to take ourselves out of the live list */
        Invalidate();
    }
}

NS_IMETHODIMP
jsdContext::GetIsValid(PRBool *_rval)
{
    *_rval = mValid;
    return NS_OK;
}

NS_IMETHODIMP
jsdContext::Invalidate()
{
    ASSERT_VALID_EPHEMERAL;
    mValid = PR_FALSE;
    jsds_RemoveEphemeral (&gLiveContexts, &mLiveListEntry);
    return NS_OK;
}

void
jsdContext::InvalidateAll()
{
    if (gLiveContexts)
        jsds_InvalidateAllEphemerals (&gLiveContexts);
}

NS_IMETHODIMP
jsdContext::GetJSContext(JSContext **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = mJSCx;
    return NS_OK;
}

NS_IMETHODIMP
jsdContext::GetOptions(PRUint32 *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JS_GetOptions(mJSCx);
    return NS_OK;
}

NS_IMETHODIMP
jsdContext::SetOptions(PRUint32 options)
{
    ASSERT_VALID_EPHEMERAL;
    PRUint32 lastOptions = JS_GetOptions(mJSCx);

    /* don't let users change this option, they'd just be shooting themselves
     * in the foot. */
    if ((options ^ lastOptions) & JSOPTION_PRIVATE_IS_NSISUPPORTS)
        return NS_ERROR_ILLEGAL_VALUE;

    JS_SetOptions(mJSCx, options);
    return NS_OK;
}

NS_IMETHODIMP
jsdContext::GetPrivateData(nsISupports **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    PRUint32 options = JS_GetOptions(mJSCx);
    if (options & JSOPTION_PRIVATE_IS_NSISUPPORTS)
    {
        *_rval = static_cast<nsISupports*>(JS_GetContextPrivate(mJSCx));
        NS_IF_ADDREF(*_rval);
    }
    else
    {
        *_rval = nsnull;
    }
    
    return NS_OK;
}
        
NS_IMETHODIMP
jsdContext::GetWrappedContext(nsISupports **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = mISCx;
    NS_IF_ADDREF(*_rval);
    return NS_OK;
}

NS_IMETHODIMP
jsdContext::GetTag(PRUint32 *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    if (!mTag)
        mTag = ++jsdContext::LastTag;
    
    *_rval = mTag;
    return NS_OK;
}

NS_IMETHODIMP
jsdContext::GetVersion (PRInt32 *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = static_cast<PRInt32>(JS_GetVersion(mJSCx));
    return NS_OK;
}

NS_IMETHODIMP
jsdContext::SetVersion (PRInt32 id)
{
    ASSERT_VALID_EPHEMERAL;
    JSVersion ver = static_cast<JSVersion>(id);
    JS_SetVersion(mJSCx, ver);
    return NS_OK;
}

NS_IMETHODIMP
jsdContext::GetGlobalObject (jsdIValue **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSObject *glob = JS_GetGlobalObject(mJSCx);
    JSDValue *jsdv = JSD_NewValue (mJSDCx, OBJECT_TO_JSVAL(glob));
    if (!jsdv)
        return NS_ERROR_FAILURE;
    *_rval = jsdValue::FromPtr (mJSDCx, jsdv);
    if (!*_rval)
        return NS_ERROR_FAILURE;
    return NS_OK;
}

NS_IMETHODIMP
jsdContext::GetScriptsEnabled (PRBool *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    nsCOMPtr<nsIScriptContext> context = do_QueryInterface(mISCx);
    if (!context)
        return NS_ERROR_NO_INTERFACE;

    *_rval = context->GetScriptsEnabled();

    return NS_OK;
}

NS_IMETHODIMP
jsdContext::SetScriptsEnabled (PRBool _rval)
{
    ASSERT_VALID_EPHEMERAL;
    nsCOMPtr<nsIScriptContext> context = do_QueryInterface(mISCx);
    if (!context)
        return NS_ERROR_NO_INTERFACE;

    context->SetScriptsEnabled(_rval, PR_TRUE);

    return NS_OK;
}

/* Stack Frames */
NS_IMPL_THREADSAFE_ISUPPORTS2(jsdStackFrame, jsdIStackFrame, jsdIEphemeral)

jsdStackFrame::jsdStackFrame (JSDContext *aCx, JSDThreadState *aThreadState,
                              JSDStackFrameInfo *aStackFrameInfo) :
    mCx(aCx), mThreadState(aThreadState), mStackFrameInfo(aStackFrameInfo)
{
    DEBUG_CREATE ("jsdStackFrame", gFrameCount);
    mValid = (aCx && aThreadState && aStackFrameInfo);
    if (mValid) {
        mLiveListEntry.key = aStackFrameInfo;
        mLiveListEntry.value = this;
        jsds_InsertEphemeral (&gLiveStackFrames, &mLiveListEntry);
    }
}

jsdStackFrame::~jsdStackFrame() 
{
    DEBUG_DESTROY ("jsdStackFrame", gFrameCount);
    if (mValid)
    {
        /* call Invalidate() to take ourselves out of the live list */
        Invalidate();
    }
}

jsdIStackFrame *
jsdStackFrame::FromPtr (JSDContext *aCx, JSDThreadState *aThreadState,
                        JSDStackFrameInfo *aStackFrameInfo)
{
    if (!aStackFrameInfo)
        return nsnull;

    jsdIStackFrame *rv;
    nsCOMPtr<jsdIStackFrame> frame;

    nsCOMPtr<jsdIEphemeral> eph =
        jsds_FindEphemeral (&gLiveStackFrames,
                            reinterpret_cast<void *>(aStackFrameInfo));

    if (eph)
    {
        frame = do_QueryInterface(eph);
        rv = frame;
    }
    else
    {
        rv = new jsdStackFrame (aCx, aThreadState, aStackFrameInfo);
    }

    NS_IF_ADDREF(rv);
    return rv;
}

NS_IMETHODIMP
jsdStackFrame::Invalidate()
{
    ASSERT_VALID_EPHEMERAL;
    mValid = PR_FALSE;
    jsds_RemoveEphemeral (&gLiveStackFrames, &mLiveListEntry);
    return NS_OK;
}

void
jsdStackFrame::InvalidateAll()
{
    if (gLiveStackFrames)
        jsds_InvalidateAllEphemerals (&gLiveStackFrames);
}

NS_IMETHODIMP
jsdStackFrame::GetJSDContext(JSDContext **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = mCx;
    return NS_OK;
}

NS_IMETHODIMP
jsdStackFrame::GetJSDThreadState(JSDThreadState **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = mThreadState;
    return NS_OK;
}

NS_IMETHODIMP
jsdStackFrame::GetJSDStackFrameInfo(JSDStackFrameInfo **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = mStackFrameInfo;
    return NS_OK;
}

NS_IMETHODIMP
jsdStackFrame::GetIsValid(PRBool *_rval)
{
    *_rval = mValid;
    return NS_OK;
}

NS_IMETHODIMP
jsdStackFrame::GetCallingFrame(jsdIStackFrame **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSDStackFrameInfo *sfi = JSD_GetCallingStackFrame (mCx, mThreadState,
                                                       mStackFrameInfo);
    *_rval = jsdStackFrame::FromPtr (mCx, mThreadState, sfi);
    return NS_OK;
}

NS_IMETHODIMP
jsdStackFrame::GetExecutionContext(jsdIContext **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSContext *cx = JSD_GetJSContext (mCx, mThreadState);
    *_rval = jsdContext::FromPtr (mCx, cx);
    return NS_OK;
}

NS_IMETHODIMP
jsdStackFrame::GetFunctionName(char **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    const char *name = JSD_GetNameForStackFrame(mCx, mThreadState,
                                                mStackFrameInfo);
    if (name) {
        *_rval = PL_strdup(name);
        if (!*_rval)
            return NS_ERROR_OUT_OF_MEMORY;
    } else {
        /* top level scripts have no function name */
        *_rval = nsnull;
        return NS_OK;
    }
    return NS_OK;
}

NS_IMETHODIMP
jsdStackFrame::GetIsNative(PRBool *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_IsStackFrameNative (mCx, mThreadState, mStackFrameInfo);
    return NS_OK;
}

NS_IMETHODIMP
jsdStackFrame::GetIsDebugger(PRBool *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_IsStackFrameDebugger (mCx, mThreadState, mStackFrameInfo);
    return NS_OK;
}

NS_IMETHODIMP
jsdStackFrame::GetIsConstructing(PRBool *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_IsStackFrameConstructing (mCx, mThreadState, mStackFrameInfo);
    return NS_OK;
}

NS_IMETHODIMP
jsdStackFrame::GetScript(jsdIScript **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSDScript *script = JSD_GetScriptForStackFrame (mCx, mThreadState,
                                                    mStackFrameInfo);
    *_rval = jsdScript::FromPtr (mCx, script);
    return NS_OK;
}

NS_IMETHODIMP
jsdStackFrame::GetPc(PRUint32 *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSDScript *script = JSD_GetScriptForStackFrame (mCx, mThreadState,
                                                    mStackFrameInfo);
    if (!script)
        return NS_ERROR_FAILURE;
    jsuword pcbase = JSD_GetClosestPC(mCx, script, 0);
    
    jsuword pc = JSD_GetPCForStackFrame (mCx, mThreadState, mStackFrameInfo);
    if (pc)
        *_rval = pc - pcbase;
    else
        *_rval = pcbase;
    return NS_OK;
}

NS_IMETHODIMP
jsdStackFrame::GetLine(PRUint32 *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSDScript *script = JSD_GetScriptForStackFrame (mCx, mThreadState,
                                                    mStackFrameInfo);
    if (script) {
        jsuword pc = JSD_GetPCForStackFrame (mCx, mThreadState, mStackFrameInfo);
        *_rval = JSD_GetClosestLine (mCx, script, pc);
    } else {
        if (!JSD_IsStackFrameNative(mCx, mThreadState, mStackFrameInfo))
            return NS_ERROR_FAILURE;
        *_rval = 1;
    }
    return NS_OK;
}

NS_IMETHODIMP
jsdStackFrame::GetCallee(jsdIValue **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSDValue *jsdv = JSD_GetCallObjectForStackFrame (mCx, mThreadState,
                                                     mStackFrameInfo);
    
    *_rval = jsdValue::FromPtr (mCx, jsdv);
    return NS_OK;
}

NS_IMETHODIMP
jsdStackFrame::GetScope(jsdIValue **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSDValue *jsdv = JSD_GetScopeChainForStackFrame (mCx, mThreadState,
                                                     mStackFrameInfo);
    
    *_rval = jsdValue::FromPtr (mCx, jsdv);
    return NS_OK;
}

NS_IMETHODIMP
jsdStackFrame::GetThisValue(jsdIValue **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSDValue *jsdv = JSD_GetThisForStackFrame (mCx, mThreadState,
                                               mStackFrameInfo);
    
    *_rval = jsdValue::FromPtr (mCx, jsdv);
    return NS_OK;
}


NS_IMETHODIMP
jsdStackFrame::Eval (const nsAString &bytes, const char *fileName,
                     PRUint32 line, jsdIValue **result, PRBool *_rval)
{
    ASSERT_VALID_EPHEMERAL;

    if (bytes.IsEmpty())
        return NS_ERROR_INVALID_ARG;

    // get pointer to buffer contained in |bytes|
    nsAString::const_iterator h;
    bytes.BeginReading(h);
    const jschar *char_bytes = reinterpret_cast<const jschar *>(h.get());

    JSExceptionState *estate = 0;
    jsval jv;

    JSContext *cx = JSD_GetJSContext (mCx, mThreadState);

    JSAutoRequest ar(cx);

    estate = JS_SaveExceptionState (cx);
    JS_ClearPendingException (cx);

    *_rval = JSD_AttemptUCScriptInStackFrame (mCx, mThreadState,
                                              mStackFrameInfo,
                                              char_bytes, bytes.Length(),
                                              fileName, line, &jv);
    if (!*_rval) {
        if (JS_IsExceptionPending(cx))
            JS_GetPendingException (cx, &jv);
        else
            jv = 0;
    }

    JS_RestoreExceptionState (cx, estate);

    JSDValue *jsdv = JSD_NewValue (mCx, jv);
    if (!jsdv)
        return NS_ERROR_FAILURE;
    *result = jsdValue::FromPtr (mCx, jsdv);
    if (!*result)
        return NS_ERROR_FAILURE;
    
    return NS_OK;
}        

/* Values */
NS_IMPL_THREADSAFE_ISUPPORTS2(jsdValue, jsdIValue, jsdIEphemeral)
jsdIValue *
jsdValue::FromPtr (JSDContext *aCx, JSDValue *aValue)
{
    /* value will be dropped by te jsdValue destructor. */

    if (!aValue)
        return nsnull;
    
    jsdIValue *rv = new jsdValue (aCx, aValue);
    NS_IF_ADDREF(rv);
    return rv;
}

jsdValue::jsdValue (JSDContext *aCx, JSDValue *aValue) : mValid(PR_TRUE),
                                                         mCx(aCx), 
                                                         mValue(aValue)
{
    DEBUG_CREATE ("jsdValue", gValueCount);
    mLiveListEntry.value = this;
    jsds_InsertEphemeral (&gLiveValues, &mLiveListEntry);
}

jsdValue::~jsdValue() 
{
    DEBUG_DESTROY ("jsdValue", gValueCount);
    if (mValid)
        /* call Invalidate() to take ourselves out of the live list */
        Invalidate();
}   

NS_IMETHODIMP
jsdValue::GetIsValid(PRBool *_rval)
{
    *_rval = mValid;
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::Invalidate()
{
    ASSERT_VALID_EPHEMERAL;
    mValid = PR_FALSE;
    jsds_RemoveEphemeral (&gLiveValues, &mLiveListEntry);
    JSD_DropValue (mCx, mValue);
    return NS_OK;
}

void
jsdValue::InvalidateAll()
{
    if (gLiveValues)
        jsds_InvalidateAllEphemerals (&gLiveValues);
}

NS_IMETHODIMP
jsdValue::GetJSDContext(JSDContext **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = mCx;
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetJSDValue (JSDValue **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = mValue;
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetIsNative (PRBool *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_IsValueNative (mCx, mValue);
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetIsNumber (PRBool *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_IsValueNumber (mCx, mValue);
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetIsPrimitive (PRBool *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_IsValuePrimitive (mCx, mValue);
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetJsType (PRUint32 *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    jsval val;

    val = JSD_GetValueWrappedJSVal (mCx, mValue);
    
    if (JSVAL_IS_NULL(val))
        *_rval = TYPE_NULL;
    else if (JSVAL_IS_BOOLEAN(val))
        *_rval = TYPE_BOOLEAN;
    else if (JSVAL_IS_DOUBLE(val))
        *_rval = TYPE_DOUBLE;
    else if (JSVAL_IS_INT(val))
        *_rval = TYPE_INT;
    else if (JSVAL_IS_STRING(val))
        *_rval = TYPE_STRING;
    else if (JSVAL_IS_VOID(val))
        *_rval = TYPE_VOID;
    else if (JSD_IsValueFunction (mCx, mValue))
        *_rval = TYPE_FUNCTION;
    else if (JSVAL_IS_OBJECT(val))
        *_rval = TYPE_OBJECT;
    else
        NS_ASSERTION (0, "Value has no discernible type.");

    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetJsPrototype (jsdIValue **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSDValue *jsdv = JSD_GetValuePrototype (mCx, mValue);
    *_rval = jsdValue::FromPtr (mCx, jsdv);
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetJsParent (jsdIValue **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSDValue *jsdv = JSD_GetValueParent (mCx, mValue);
    *_rval = jsdValue::FromPtr (mCx, jsdv);
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetJsClassName(char **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    const char *name = JSD_GetValueClassName(mCx, mValue);
    if (name) {
        *_rval = PL_strdup(name);
        if (!*_rval)
            return NS_ERROR_OUT_OF_MEMORY;
    } else {
        *_rval = nsnull;
    }
    
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetJsConstructor (jsdIValue **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSDValue *jsdv = JSD_GetValueConstructor (mCx, mValue);
    *_rval = jsdValue::FromPtr (mCx, jsdv);
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetJsFunctionName(char **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    const char *name = JSD_GetValueFunctionName(mCx, mValue);
    if (name) {
        *_rval = PL_strdup(name);
        if (!*_rval)
            return NS_ERROR_OUT_OF_MEMORY;
    } else {
        /* top level scripts have no function name */
        *_rval = nsnull;
        return NS_OK;
    }

    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetBooleanValue(PRBool *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_GetValueBoolean (mCx, mValue);
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetDoubleValue(double *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    double *dp = JSD_GetValueDouble (mCx, mValue);
    if (!dp)
        return NS_ERROR_FAILURE;
    *_rval = *dp;
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetIntValue(PRInt32 *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    *_rval = JSD_GetValueInt (mCx, mValue);
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetObjectValue(jsdIObject **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSDObject *obj;
    obj = JSD_GetObjectForValue (mCx, mValue);
    *_rval = jsdObject::FromPtr (mCx, obj);
    if (!*_rval)
        return NS_ERROR_FAILURE;
    return NS_OK;
}
    
NS_IMETHODIMP
jsdValue::GetStringValue(char **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSString *jstr_val = JSD_GetValueString(mCx, mValue);
    if (jstr_val) {
        char *bytes = JS_GetStringBytes(jstr_val);
        *_rval = PL_strdup(bytes);
        if (!*_rval)
            return NS_ERROR_OUT_OF_MEMORY;
    } else {
        *_rval = nsnull;
    }
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetPropertyCount (PRInt32 *_rval)
{
    ASSERT_VALID_EPHEMERAL;
    if (JSD_IsValueObject(mCx, mValue))
        *_rval = JSD_GetCountOfProperties (mCx, mValue);
    else
        *_rval = -1;
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetProperties (jsdIProperty ***propArray, PRUint32 *length)
{
    ASSERT_VALID_EPHEMERAL;
    *propArray = nsnull;
    if (length)
        *length = 0;

    PRUint32 prop_count = JSD_IsValueObject(mCx, mValue)
        ? JSD_GetCountOfProperties (mCx, mValue)
        : 0;
    NS_ENSURE_TRUE(prop_count, NS_OK);

    jsdIProperty **pa_temp =
        static_cast<jsdIProperty **>
                   (nsMemory::Alloc(sizeof (jsdIProperty *) * 
                                       prop_count));
    NS_ENSURE_TRUE(pa_temp, NS_ERROR_OUT_OF_MEMORY);

    PRUint32     i    = 0;
    JSDProperty *iter = NULL;
    JSDProperty *prop;
    while ((prop = JSD_IterateProperties (mCx, mValue, &iter))) {
        pa_temp[i] = jsdProperty::FromPtr (mCx, prop);
        ++i;
    }
    
    NS_ASSERTION (prop_count == i, "property count mismatch");    

    /* if caller doesn't care about length, don't bother telling them */
    *propArray = pa_temp;
    if (length)
        *length = prop_count;
    
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetProperty (const char *name, jsdIProperty **_rval)
{
    ASSERT_VALID_EPHEMERAL;
    JSContext *cx = JSD_GetDefaultJSContext (mCx);

    JSAutoRequest ar(cx);

    /* not rooting this */
    JSString *jstr_name = JS_NewStringCopyZ (cx, name);
    if (!jstr_name)
        return NS_ERROR_OUT_OF_MEMORY;

    JSDProperty *prop = JSD_GetValueProperty (mCx, mValue, jstr_name);
    
    *_rval = jsdProperty::FromPtr (mCx, prop);
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::Refresh()
{
    ASSERT_VALID_EPHEMERAL;
    JSD_RefreshValue (mCx, mValue);
    return NS_OK;
}

NS_IMETHODIMP
jsdValue::GetWrappedValue()
{
    ASSERT_VALID_EPHEMERAL;
    nsresult rv;
    nsCOMPtr<nsIXPConnect> xpc = do_GetService(nsIXPConnect::GetCID(), &rv);
    if (NS_FAILED(rv))
        return rv;

    nsAXPCNativeCallContext *cc = nsnull;
    rv = xpc->GetCurrentNativeCallContext(&cc);
    if (NS_FAILED(rv))
        return rv;

    jsval *result;
    rv = cc->GetRetValPtr(&result);
    if (NS_FAILED(rv))
        return rv;

    if (result)
    {
        *result = JSD_GetValueWrappedJSVal (mCx, mValue);
        cc->SetReturnValueWasSet(PR_TRUE);
    }

    return NS_OK;
}

/******************************************************************************
 * debugger service implementation
 ******************************************************************************/
NS_IMPL_THREADSAFE_ISUPPORTS1(jsdService, jsdIDebuggerService)

NS_IMETHODIMP
jsdService::GetJSDContext(JSDContext **_rval)
{
    *_rval = mCx;
    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetInitAtStartup (PRBool *_rval)
{
    nsresult rv;
    nsCOMPtr<nsICategoryManager>
        categoryManager(do_GetService(NS_CATMAN_CTRID, &rv));
    
    if (NS_FAILED(rv))
    {
        NS_WARNING("couldn't get category manager");
        return rv;
    }

    if (mInitAtStartup == triUnknown) {
        nsXPIDLCString notused;
        nsresult autoreg_rv, appstart_rv;
        
        autoreg_rv = categoryManager->GetCategoryEntry(AUTOREG_CATEGORY, 
                                                       JSD_AUTOREG_ENTRY,
                                                       getter_Copies(notused));
        appstart_rv = categoryManager->GetCategoryEntry(APPSTART_CATEGORY,
                                                        JSD_STARTUP_ENTRY,
                                                        getter_Copies(notused));
        if (autoreg_rv != appstart_rv) {
            /* we have an inconsistent state in the registry, attempt to fix.
             * we need to make mInitAtStartup disagree with the state passed
             * to SetInitAtStartup to make it actually do something.
             */
            mInitAtStartup = triYes;
            rv = SetInitAtStartup (PR_FALSE);
            if (NS_FAILED(rv))
            {
                NS_WARNING("SetInitAtStartup failed");
                return rv;
            }
        } else if (autoreg_rv == NS_ERROR_NOT_AVAILABLE) {
            mInitAtStartup = triNo;
        } else if (NS_SUCCEEDED(autoreg_rv)) {
            mInitAtStartup = triYes;
        } else {
            NS_WARN_IF_FALSE(NS_SUCCEEDED(autoreg_rv),
                             "couldn't get autoreg category");
            NS_WARN_IF_FALSE(NS_SUCCEEDED(appstart_rv),
                             "couldn't get appstart category");
            return rv;
        }
    }
    
    if (_rval)
        *_rval = (mInitAtStartup == triYes);

    return NS_OK;
}

/*
 * The initAtStartup property controls whether or not we register the
 * app start observer (jsdASObserver.)  We register for both 
 * "xpcom-autoregistration" and "app-startup" notifications if |state| is true.
 * the autoreg message is sent just before registration occurs (before
 * "app-startup".)  We care about autoreg because it may load javascript
 * components.  autoreg does *not* fire if components haven't changed since the
 * last autoreg, so we watch "app-startup" as a fallback.
 */
NS_IMETHODIMP
jsdService::SetInitAtStartup (PRBool state)
{ 
    nsresult rv;

    if (mInitAtStartup == triUnknown) {
        /* side effect sets mInitAtStartup */
        rv = GetInitAtStartup(nsnull);
        if (NS_FAILED(rv))
            return rv;
    }

    if (state && mInitAtStartup == triYes ||
        !state && mInitAtStartup == triNo) {
        /* already in the requested state */
        return NS_OK;
    }
    
    nsCOMPtr<nsICategoryManager>
        categoryManager(do_GetService(NS_CATMAN_CTRID, &rv));
    if (NS_FAILED(rv))
        return rv;

    if (state) {
        rv = categoryManager->AddCategoryEntry(AUTOREG_CATEGORY,
                                               JSD_AUTOREG_ENTRY,
                                               jsdARObserverCtrID,
                                               PR_TRUE, PR_TRUE, nsnull);
        if (NS_FAILED(rv))
            return rv;
        rv = categoryManager->AddCategoryEntry(APPSTART_CATEGORY,
                                               JSD_STARTUP_ENTRY,
                                               jsdASObserverCtrID,
                                               PR_TRUE, PR_TRUE, nsnull);
        if (NS_FAILED(rv))
            return rv;
        mInitAtStartup = triYes;
    } else {
        rv = categoryManager->DeleteCategoryEntry(AUTOREG_CATEGORY,
                                                  JSD_AUTOREG_ENTRY, PR_TRUE);
        if (NS_FAILED(rv))
            return rv;
        rv = categoryManager->DeleteCategoryEntry(APPSTART_CATEGORY,
                                                  JSD_STARTUP_ENTRY, PR_TRUE);
        if (NS_FAILED(rv))
            return rv;
        mInitAtStartup = triNo;
    }

    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetFlags (PRUint32 *_rval)
{
    ASSERT_VALID_CONTEXT;
    *_rval = JSD_GetContextFlags (mCx);
    return NS_OK;
}

NS_IMETHODIMP
jsdService::SetFlags (PRUint32 flags)
{
    ASSERT_VALID_CONTEXT;
    JSD_SetContextFlags (mCx, flags);
    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetImplementationString(char **_rval)
{
    *_rval = PL_strdup(implementationString);
    if (!*_rval)
        return NS_ERROR_OUT_OF_MEMORY;
    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetImplementationMajor(PRUint32 *_rval)
{
    *_rval = JSDS_MAJOR_VERSION;
    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetImplementationMinor(PRUint32 *_rval)
{
    *_rval = JSDS_MINOR_VERSION;
    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetIsOn (PRBool *_rval)
{
    *_rval = mOn;
    return NS_OK;
}

NS_IMETHODIMP
jsdService::On (void)
{
    nsresult  rv;

    /* get JS things from the CallContext */
    nsCOMPtr<nsIXPConnect> xpc = do_GetService(nsIXPConnect::GetCID(), &rv);
    if (NS_FAILED(rv)) return rv;

    nsAXPCNativeCallContext *cc = nsnull;
    rv = xpc->GetCurrentNativeCallContext(&cc);
    if (NS_FAILED(rv)) return rv;

    JSContext *cx;
    rv = cc->GetJSContext (&cx);
    if (NS_FAILED(rv)) return rv;
    
    return OnForRuntime(JS_GetRuntime (cx));
    
}

NS_IMETHODIMP
jsdService::OnForRuntime (JSRuntime *rt)
{
    if (mOn)
        return (rt == mRuntime) ? NS_OK : NS_ERROR_ALREADY_INITIALIZED;

    mRuntime = rt;

    if (gLastGCProc == jsds_GCCallbackProc)
        /* condition indicates that the callback proc has not been set yet */
        gLastGCProc = JS_SetGCCallbackRT (rt, jsds_GCCallbackProc);

    mCx = JSD_DebuggerOnForUser (rt, NULL, NULL);
    if (!mCx)
        return NS_ERROR_FAILURE;

    JSContext *cx   = JSD_GetDefaultJSContext (mCx);
    JSObject  *glob = JS_GetGlobalObject (cx);

    /* init xpconnect on the debugger's context in case xpconnect tries to
     * use it for stuff. */
    nsresult rv;
    nsCOMPtr<nsIXPConnect> xpc = do_GetService(nsIXPConnect::GetCID(), &rv);
    if (NS_FAILED(rv))
        return rv;
    
    xpc->InitClasses (cx, glob);
    
    /* If any of these mFooHook objects are installed, do the required JSD
     * hookup now.   See also, jsdService::SetFooHook().
     */
    if (mErrorHook)
        JSD_SetErrorReporter (mCx, jsds_ErrorHookProc, NULL);
    if (mThrowHook)
        JSD_SetThrowHook (mCx, jsds_ExecutionHookProc, NULL);
    /* can't ignore script callbacks, as we need to |Release| the wrapper 
     * stored in private data when a script is deleted. */
    if (mInterruptHook)
        JSD_SetInterruptHook (mCx, jsds_ExecutionHookProc, NULL);
    if (mDebuggerHook)
        JSD_SetDebuggerHook (mCx, jsds_ExecutionHookProc, NULL);
    if (mDebugHook)
        JSD_SetDebugBreakHook (mCx, jsds_ExecutionHookProc, NULL);
    if (mTopLevelHook)
        JSD_SetTopLevelHook (mCx, jsds_CallHookProc, NULL);
    else
        JSD_ClearTopLevelHook (mCx);
    if (mFunctionHook)
        JSD_SetFunctionHook (mCx, jsds_CallHookProc, NULL);
    else
        JSD_ClearFunctionHook (mCx);
    mOn = PR_TRUE;

#ifdef DEBUG
    printf ("+++ JavaScript debugging hooks installed.\n");
#endif
    return NS_OK;
}

NS_IMETHODIMP
jsdService::Off (void)
{
    if (!mOn)
        return NS_OK;
    
    if (!mCx || !mRuntime)
        return NS_ERROR_NOT_INITIALIZED;
    
    if (gDeadScripts) {
        if (gGCStatus != JSGC_END)
            return NS_ERROR_NOT_AVAILABLE;

        JSContext *cx = JSD_GetDefaultJSContext(mCx);
        jsds_NotifyPendingDeadScripts(cx);
    }

    /*
    if (gLastGCProc != jsds_GCCallbackProc)
        JS_SetGCCallbackRT (mRuntime, gLastGCProc);
    */

    jsdContext::InvalidateAll();
    jsdScript::InvalidateAll();
    jsdValue::InvalidateAll();
    jsdProperty::InvalidateAll();
    ClearAllBreakpoints();

    JSD_SetErrorReporter (mCx, NULL, NULL);
    JSD_SetScriptHook (mCx, NULL, NULL);
    JSD_ClearThrowHook (mCx);
    JSD_ClearInterruptHook (mCx);
    JSD_ClearDebuggerHook (mCx);
    JSD_ClearDebugBreakHook (mCx);
    JSD_ClearTopLevelHook (mCx);
    JSD_ClearFunctionHook (mCx);
    
    JSD_DebuggerOff (mCx);

    mCx = nsnull;
    mRuntime = nsnull;
    mOn = PR_FALSE;

#ifdef DEBUG
    printf ("+++ JavaScript debugging hooks removed.\n");
#endif

    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetPauseDepth(PRUint32 *_rval)
{
    NS_ENSURE_ARG_POINTER(_rval);
    *_rval = mPauseLevel;
    return NS_OK;
}
    
NS_IMETHODIMP
jsdService::Pause(PRUint32 *_rval)
{
    if (!mCx)
        return NS_ERROR_NOT_INITIALIZED;

    if (++mPauseLevel == 1) {
        JSD_SetErrorReporter (mCx, NULL, NULL);
        JSD_ClearThrowHook (mCx);
        JSD_ClearInterruptHook (mCx);
        JSD_ClearDebuggerHook (mCx);
        JSD_ClearDebugBreakHook (mCx);
        JSD_ClearTopLevelHook (mCx);
        JSD_ClearFunctionHook (mCx);
    }

    if (_rval)
        *_rval = mPauseLevel;

    return NS_OK;
}

NS_IMETHODIMP
jsdService::UnPause(PRUint32 *_rval)
{
    if (!mCx)
        return NS_ERROR_NOT_INITIALIZED;

    if (mPauseLevel == 0)
        return NS_ERROR_NOT_AVAILABLE;

    /* check mOn before we muck with this stuff, it's possible the debugger
     * was turned off while we were paused.
     */
    if (--mPauseLevel == 0 && mOn) {
        if (mErrorHook)
            JSD_SetErrorReporter (mCx, jsds_ErrorHookProc, NULL);
        if (mThrowHook)
            JSD_SetThrowHook (mCx, jsds_ExecutionHookProc, NULL);
        if (mInterruptHook)
            JSD_SetInterruptHook (mCx, jsds_ExecutionHookProc, NULL);
        if (mDebuggerHook)
            JSD_SetDebuggerHook (mCx, jsds_ExecutionHookProc, NULL);
        if (mDebugHook)
            JSD_SetDebugBreakHook (mCx, jsds_ExecutionHookProc, NULL);
        if (mTopLevelHook)
            JSD_SetTopLevelHook (mCx, jsds_CallHookProc, NULL);
        else
            JSD_ClearTopLevelHook (mCx);
        if (mFunctionHook)
            JSD_SetFunctionHook (mCx, jsds_CallHookProc, NULL);
        else
            JSD_ClearFunctionHook (mCx);
    }
    
    if (_rval)
        *_rval = mPauseLevel;

    return NS_OK;
}

NS_IMETHODIMP
jsdService::EnumerateContexts (jsdIContextEnumerator *enumerator)
{
    ASSERT_VALID_CONTEXT;
    
    if (!enumerator)
        return NS_OK;
    
    JSContext *iter = NULL;
    JSContext *cx;

    while ((cx = JS_ContextIterator (mRuntime, &iter)))
    {
        nsCOMPtr<jsdIContext> jsdicx = 
            getter_AddRefs(jsdContext::FromPtr(mCx, cx));
        if (jsdicx)
        {
            if (NS_FAILED(enumerator->EnumerateContext(jsdicx)))
                break;
        }
    }

    return NS_OK;
}

NS_IMETHODIMP
jsdService::EnumerateScripts (jsdIScriptEnumerator *enumerator)
{
    ASSERT_VALID_CONTEXT;
    
    JSDScript *script;
    JSDScript *iter = NULL;
    nsresult rv = NS_OK;
    
    JSD_LockScriptSubsystem(mCx);
    while((script = JSD_IterateScripts(mCx, &iter))) {
        nsCOMPtr<jsdIScript> jsdis =
            getter_AddRefs(jsdScript::FromPtr(mCx, script));
        rv = enumerator->EnumerateScript (jsdis);
        if (NS_FAILED(rv))
            break;
    }
    JSD_UnlockScriptSubsystem(mCx);

    return rv;
}

NS_IMETHODIMP
jsdService::GC (void)
{
    ASSERT_VALID_CONTEXT;
    JSContext *cx = JSD_GetDefaultJSContext (mCx);
    JS_GC(cx);
    return NS_OK;
}
    
NS_IMETHODIMP
jsdService::DumpHeap(const char* fileName)
{
    ASSERT_VALID_CONTEXT;
#ifndef DEBUG
    return NS_ERROR_NOT_IMPLEMENTED;
#else
    nsresult rv = NS_OK;
    FILE *file = fileName ? fopen(fileName, "w") : stdout;
    if (!file) {
        rv = NS_ERROR_FAILURE;
    } else {
        JSContext *cx = JSD_GetDefaultJSContext (mCx);
        if (!JS_DumpHeap(cx, file, NULL, 0, NULL, (size_t)-1, NULL))
            rv = NS_ERROR_FAILURE;
        if (file != stdout)
            fclose(file);
    }
    return rv;
#endif
}

NS_IMETHODIMP
jsdService::ClearProfileData ()
{
    ASSERT_VALID_CONTEXT;
    JSD_ClearAllProfileData (mCx);
    return NS_OK;
}

NS_IMETHODIMP
jsdService::InsertFilter (jsdIFilter *filter, jsdIFilter *after)
{
    NS_ENSURE_ARG_POINTER (filter);
    if (jsds_FindFilter (filter))
        return NS_ERROR_INVALID_ARG;

    FilterRecord *rec = PR_NEWZAP (FilterRecord);
    if (!rec)
        return NS_ERROR_OUT_OF_MEMORY;

    if (!jsds_SyncFilter (rec, filter)) {
        PR_Free (rec);
        return NS_ERROR_FAILURE;
    }
    
    if (gFilters) {
        if (!after) {
            /* insert at head of list */
            PR_INSERT_LINK(&rec->links, &gFilters->links);
            gFilters = rec;
        } else {
            /* insert somewhere in the list */
            FilterRecord *afterRecord = jsds_FindFilter (after);
            if (!afterRecord) {
                jsds_FreeFilter(rec);
                return NS_ERROR_INVALID_ARG;
            }
            PR_INSERT_AFTER(&rec->links, &afterRecord->links);
        }
    } else {
        if (after) {
            /* user asked to insert into the middle of an empty list, bail. */
            jsds_FreeFilter(rec);
            return NS_ERROR_NOT_INITIALIZED;
        }
        PR_INIT_CLIST(&rec->links);
        gFilters = rec;
    }
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::AppendFilter (jsdIFilter *filter)
{
    NS_ENSURE_ARG_POINTER (filter);
    if (jsds_FindFilter (filter))
        return NS_ERROR_INVALID_ARG;
    FilterRecord *rec = PR_NEWZAP (FilterRecord);

    if (!jsds_SyncFilter (rec, filter)) {
        PR_Free (rec);
        return NS_ERROR_FAILURE;
    }
    
    if (gFilters) {
        PR_INSERT_BEFORE(&rec->links, &gFilters->links);
    } else {
        PR_INIT_CLIST(&rec->links);
        gFilters = rec;
    }
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::RemoveFilter (jsdIFilter *filter)
{
    NS_ENSURE_ARG_POINTER(filter);
    FilterRecord *rec = jsds_FindFilter (filter);
    if (!rec)
        return NS_ERROR_INVALID_ARG;
    
    if (gFilters == rec) {
        gFilters = reinterpret_cast<FilterRecord *>
                                   (PR_NEXT_LINK(&rec->links));
        /* If we're the only filter left, null out the list head. */
        if (gFilters == rec)
            gFilters = nsnull;
    }

    
    PR_REMOVE_LINK(&rec->links);
    jsds_FreeFilter (rec);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::SwapFilters (jsdIFilter *filter_a, jsdIFilter *filter_b)
{
    NS_ENSURE_ARG_POINTER(filter_a);
    NS_ENSURE_ARG_POINTER(filter_b);
    
    FilterRecord *rec_a = jsds_FindFilter (filter_a);
    if (!rec_a)
        return NS_ERROR_INVALID_ARG;
    
    if (filter_a == filter_b) {
        /* just a refresh */
        if (!jsds_SyncFilter (rec_a, filter_a))
            return NS_ERROR_FAILURE;
        return NS_OK;
    }
    
    FilterRecord *rec_b = jsds_FindFilter (filter_b);
    if (!rec_b) {
        /* filter_b is not in the list, replace filter_a with filter_b. */
        if (!jsds_SyncFilter (rec_a, filter_b))
            return NS_ERROR_FAILURE;
    } else {
        /* both filters are in the list, swap. */
        if (!jsds_SyncFilter (rec_a, filter_b))
            return NS_ERROR_FAILURE;
        if (!jsds_SyncFilter (rec_b, filter_a))
            return NS_ERROR_FAILURE;
    }
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::EnumerateFilters (jsdIFilterEnumerator *enumerator) 
{
    if (!gFilters)
        return NS_OK;
    
    FilterRecord *current = gFilters;
    do {
        jsds_SyncFilter (current, current->filterObject);
        /* SyncFilter failure would be bad, but what would we do about it? */
        if (enumerator) {
            nsresult rv = enumerator->EnumerateFilter (current->filterObject);
            if (NS_FAILED(rv))
                return rv;
        }
        current = reinterpret_cast<FilterRecord *>
                                  (PR_NEXT_LINK (&current->links));
    } while (current != gFilters);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::RefreshFilters ()
{
    return EnumerateFilters(nsnull);
}

NS_IMETHODIMP
jsdService::ClearFilters ()
{
    if (!gFilters)
        return NS_OK;

    FilterRecord *current = reinterpret_cast<FilterRecord *>
                                            (PR_NEXT_LINK (&gFilters->links));
    do {
        FilterRecord *next = reinterpret_cast<FilterRecord *>
                                             (PR_NEXT_LINK (&current->links));
        PR_REMOVE_AND_INIT_LINK(&current->links);
        jsds_FreeFilter(current);
        current = next;
    } while (current != gFilters);
    
    jsds_FreeFilter(current);
    gFilters = nsnull;
    
    return NS_OK;
}
        
NS_IMETHODIMP
jsdService::ClearAllBreakpoints (void)
{
    ASSERT_VALID_CONTEXT;

    JSD_LockScriptSubsystem(mCx);
    JSD_ClearAllExecutionHooks (mCx);
    JSD_UnlockScriptSubsystem(mCx);
    return NS_OK;
}

NS_IMETHODIMP
jsdService::WrapValue(jsdIValue **_rval)
{
    ASSERT_VALID_CONTEXT;

    nsresult rv;
    nsCOMPtr<nsIXPConnect> xpc = do_GetService (nsIXPConnect::GetCID(), &rv);
    if (NS_FAILED(rv))
        return rv;

    nsAXPCNativeCallContext *cc = nsnull;
    rv = xpc->GetCurrentNativeCallContext (&cc);
    if (NS_FAILED(rv))
        return rv;

    PRUint32 argc;
    rv = cc->GetArgc (&argc);
    if (NS_FAILED(rv))
        return rv;
    if (argc < 1)
        return NS_ERROR_INVALID_ARG;
    
    jsval    *argv;
    rv = cc->GetArgvPtr (&argv);
    if (NS_FAILED(rv))
        return rv;

    JSDValue *jsdv = JSD_NewValue (mCx, argv[0]);
    if (!jsdv)
        return NS_ERROR_FAILURE;
    
    *_rval = jsdValue::FromPtr (mCx, jsdv);
    return NS_OK;
}


NS_IMETHODIMP
jsdService::EnterNestedEventLoop (jsdINestCallback *callback, PRUint32 *_rval)
{
    // Nesting event queues is a thing of the past.  Now, we just spin the
    // current event loop.
 
    nsresult rv;
    nsCOMPtr<nsIJSContextStack> 
        stack(do_GetService("@mozilla.org/js/xpc/ContextStack;1", &rv));
    if (NS_FAILED(rv))
        return rv;
    PRUint32 nestLevel = ++mNestedLoopLevel;
    
    nsCOMPtr<nsIThread> thread = do_GetCurrentThread();

    if (NS_SUCCEEDED(stack->Push(nsnull))) {
        if (callback) {
            Pause(nsnull);
            rv = callback->OnNest();
            UnPause(nsnull);
        }
        
        while (NS_SUCCEEDED(rv) && mNestedLoopLevel >= nestLevel) {
            if (!NS_ProcessNextEvent(thread))
                rv = NS_ERROR_UNEXPECTED;
        }

        JSContext* cx;
        stack->Pop(&cx);
        NS_ASSERTION(cx == nsnull, "JSContextStack mismatch");
    }
    else
        rv = NS_ERROR_FAILURE;
    
    NS_ASSERTION (mNestedLoopLevel <= nestLevel,
                  "nested event didn't unwind properly");
    if (mNestedLoopLevel == nestLevel)
        --mNestedLoopLevel;

    *_rval = mNestedLoopLevel;
    return rv;
}

NS_IMETHODIMP
jsdService::ExitNestedEventLoop (PRUint32 *_rval)
{
    if (mNestedLoopLevel > 0)
        --mNestedLoopLevel;
    else
        return NS_ERROR_FAILURE;

    *_rval = mNestedLoopLevel;    
    return NS_OK;
}    

/* hook attribute get/set functions */

NS_IMETHODIMP
jsdService::SetErrorHook (jsdIErrorHook *aHook)
{
    mErrorHook = aHook;

    /* if the debugger isn't initialized, that's all we can do for now.  The
     * OnForRuntime() method will do the rest when the coast is clear.
     */
    if (!mCx || mPauseLevel)
        return NS_OK;

    if (aHook)
        JSD_SetErrorReporter (mCx, jsds_ErrorHookProc, NULL);
    else
        JSD_SetErrorReporter (mCx, NULL, NULL);

    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetErrorHook (jsdIErrorHook **aHook)
{
    *aHook = mErrorHook;
    NS_IF_ADDREF(*aHook);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::SetBreakpointHook (jsdIExecutionHook *aHook)
{    
    mBreakpointHook = aHook;
    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetBreakpointHook (jsdIExecutionHook **aHook)
{   
    *aHook = mBreakpointHook;
    NS_IF_ADDREF(*aHook);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::SetDebugHook (jsdIExecutionHook *aHook)
{    
    mDebugHook = aHook;

    /* if the debugger isn't initialized, that's all we can do for now.  The
     * OnForRuntime() method will do the rest when the coast is clear.
     */
    if (!mCx || mPauseLevel)
        return NS_OK;

    if (aHook)
        JSD_SetDebugBreakHook (mCx, jsds_ExecutionHookProc, NULL);
    else
        JSD_ClearDebugBreakHook (mCx);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetDebugHook (jsdIExecutionHook **aHook)
{   
    *aHook = mDebugHook;
    NS_IF_ADDREF(*aHook);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::SetDebuggerHook (jsdIExecutionHook *aHook)
{    
    mDebuggerHook = aHook;

    /* if the debugger isn't initialized, that's all we can do for now.  The
     * OnForRuntime() method will do the rest when the coast is clear.
     */
    if (!mCx || mPauseLevel)
        return NS_OK;

    if (aHook)
        JSD_SetDebuggerHook (mCx, jsds_ExecutionHookProc, NULL);
    else
        JSD_ClearDebuggerHook (mCx);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetDebuggerHook (jsdIExecutionHook **aHook)
{   
    *aHook = mDebuggerHook;
    NS_IF_ADDREF(*aHook);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::SetInterruptHook (jsdIExecutionHook *aHook)
{    
    mInterruptHook = aHook;

    /* if the debugger isn't initialized, that's all we can do for now.  The
     * OnForRuntime() method will do the rest when the coast is clear.
     */
    if (!mCx || mPauseLevel)
        return NS_OK;

    if (aHook)
        JSD_SetInterruptHook (mCx, jsds_ExecutionHookProc, NULL);
    else
        JSD_ClearInterruptHook (mCx);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetInterruptHook (jsdIExecutionHook **aHook)
{   
    *aHook = mInterruptHook;
    NS_IF_ADDREF(*aHook);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::SetScriptHook (jsdIScriptHook *aHook)
{    
    mScriptHook = aHook;

    /* if the debugger isn't initialized, that's all we can do for now.  The
     * OnForRuntime() method will do the rest when the coast is clear.
     */
    if (!mCx || mPauseLevel)
        return NS_OK;
    
    if (aHook)
        JSD_SetScriptHook (mCx, jsds_ScriptHookProc, NULL);
    /* we can't unset it if !aHook, because we still need to see script
     * deletes in order to Release the jsdIScripts held in JSDScript
     * private data. */
    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetScriptHook (jsdIScriptHook **aHook)
{   
    *aHook = mScriptHook;
    NS_IF_ADDREF(*aHook);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::SetThrowHook (jsdIExecutionHook *aHook)
{    
    mThrowHook = aHook;

    /* if the debugger isn't initialized, that's all we can do for now.  The
     * OnForRuntime() method will do the rest when the coast is clear.
     */
    if (!mCx || mPauseLevel)
        return NS_OK;

    if (aHook)
        JSD_SetThrowHook (mCx, jsds_ExecutionHookProc, NULL);
    else
        JSD_ClearThrowHook (mCx);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetThrowHook (jsdIExecutionHook **aHook)
{   
    *aHook = mThrowHook;
    NS_IF_ADDREF(*aHook);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::SetTopLevelHook (jsdICallHook *aHook)
{    
    mTopLevelHook = aHook;

    /* if the debugger isn't initialized, that's all we can do for now.  The
     * OnForRuntime() method will do the rest when the coast is clear.
     */
    if (!mCx || mPauseLevel)
        return NS_OK;

    if (aHook)
        JSD_SetTopLevelHook (mCx, jsds_CallHookProc, NULL);
    else
        JSD_ClearTopLevelHook (mCx);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetTopLevelHook (jsdICallHook **aHook)
{   
    *aHook = mTopLevelHook;
    NS_IF_ADDREF(*aHook);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::SetFunctionHook (jsdICallHook *aHook)
{    
    mFunctionHook = aHook;

    /* if the debugger isn't initialized, that's all we can do for now.  The
     * OnForRuntime() method will do the rest when the coast is clear.
     */
    if (!mCx || mPauseLevel)
        return NS_OK;

    if (aHook)
        JSD_SetFunctionHook (mCx, jsds_CallHookProc, NULL);
    else
        JSD_ClearFunctionHook (mCx);
    
    return NS_OK;
}

NS_IMETHODIMP
jsdService::GetFunctionHook (jsdICallHook **aHook)
{   
    *aHook = mFunctionHook;
    NS_IF_ADDREF(*aHook);
    
    return NS_OK;
}

/* virtual */
jsdService::~jsdService()
{
    ClearFilters();
    mErrorHook = nsnull;
    mBreakpointHook = nsnull;
    mDebugHook = nsnull;
    mDebuggerHook = nsnull;
    mInterruptHook = nsnull;
    mScriptHook = nsnull;
    mThrowHook = nsnull;
    mTopLevelHook = nsnull;
    mFunctionHook = nsnull;
    gGCStatus = JSGC_END;
    Off();
    gJsds = nsnull;
}

jsdService *
jsdService::GetService ()
{
    if (!gJsds)
        gJsds = new jsdService();
        
    NS_IF_ADDREF(gJsds);
    return gJsds;
}

NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(jsdService, jsdService::GetService)

/* app-start observer.  turns on the debugger at app-start.  this is inserted
 * and/or removed from the app-start category by the jsdService::initAtStartup
 * property.
 */
class jsdASObserver : public nsIObserver 
{
  public:
    NS_DECL_ISUPPORTS
    NS_DECL_NSIOBSERVER

    jsdASObserver () {}    
};

NS_IMPL_THREADSAFE_ISUPPORTS1(jsdASObserver, nsIObserver)

NS_IMETHODIMP
jsdASObserver::Observe (nsISupports *aSubject, const char *aTopic,
                        const PRUnichar *aData)
{
    nsresult rv;

    // Hmm.  Why is the app-startup observer called multiple times?
    //NS_ASSERTION(!gJsds, "app startup observer called twice");
    nsCOMPtr<jsdIDebuggerService> jsds = do_GetService(jsdServiceCtrID, &rv);
    if (NS_FAILED(rv))
        return rv;

    PRBool on;
    rv = jsds->GetIsOn(&on);
    if (NS_FAILED(rv) || on)
        return rv;
    
    nsCOMPtr<nsIJSRuntimeService> rts = do_GetService(NS_JSRT_CTRID, &rv);
    if (NS_FAILED(rv))
        return rv;    

    JSRuntime *rt;
    rts->GetRuntime (&rt);
    if (NS_FAILED(rv))
        return rv;

    rv = jsds->OnForRuntime(rt);
    if (NS_FAILED(rv))
        return rv;
    
    return jsds->SetFlags(JSD_DISABLE_OBJECT_TRACE);
}

NS_GENERIC_FACTORY_CONSTRUCTOR(jsdASObserver)

static const nsModuleComponentInfo components[] = {
    {"JSDService", JSDSERVICE_CID,    jsdServiceCtrID, jsdServiceConstructor},
    {"JSDASObserver",  JSDASO_CID, jsdARObserverCtrID, jsdASObserverConstructor}
};

NS_IMPL_NSGETMODULE(JavaScript_Debugger, components)

/********************************************************************************
 ********************************************************************************
 * graveyard
 */

#if 0
/* Thread States */
NS_IMPL_THREADSAFE_ISUPPORTS1(jsdThreadState, jsdIThreadState); 

NS_IMETHODIMP
jsdThreadState::GetJSDContext(JSDContext **_rval)
{
    *_rval = mCx;
    return NS_OK;
}

NS_IMETHODIMP
jsdThreadState::GetJSDThreadState(JSDThreadState **_rval)
{
    *_rval = mThreadState;
    return NS_OK;
}

NS_IMETHODIMP
jsdThreadState::GetFrameCount (PRUint32 *_rval)
{
    *_rval = JSD_GetCountOfStackFrames (mCx, mThreadState);
    return NS_OK;
}

NS_IMETHODIMP
jsdThreadState::GetTopFrame (jsdIStackFrame **_rval)
{
    JSDStackFrameInfo *sfi = JSD_GetStackFrame (mCx, mThreadState);
    
    *_rval = jsdStackFrame::FromPtr (mCx, mThreadState, sfi);
    return NS_OK;
}

NS_IMETHODIMP
jsdThreadState::GetPendingException(jsdIValue **_rval)
{
    JSDValue *jsdv = JSD_GetException (mCx, mThreadState);
    
    *_rval = jsdValue::FromPtr (mCx, jsdv);
    return NS_OK;
}

NS_IMETHODIMP
jsdThreadState::SetPendingException(jsdIValue *aException)
{
    JSDValue *jsdv;
    
    nsresult rv = aException->GetJSDValue (&jsdv);
    if (NS_FAILED(rv))
        return NS_ERROR_FAILURE;
    
    if (!JSD_SetException (mCx, mThreadState, jsdv))
        return NS_ERROR_FAILURE;

    return NS_OK;
}

#endif
