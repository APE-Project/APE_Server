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
 *   John Bandhauer <jband@netscape.com>
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

/* Implements nsXPCToolsCompiler. */

#include "xpctools_private.h"
#include "nsDirectoryServiceDefs.h"
#include "nsDirectoryServiceUtils.h"

NS_IMPL_ISUPPORTS1(nsXPCToolsCompiler, nsIXPCToolsCompiler)

nsXPCToolsCompiler::nsXPCToolsCompiler()
{
}

nsXPCToolsCompiler::~nsXPCToolsCompiler()
{
}

/* readonly attribute nsILocalFile binDir; */
NS_IMETHODIMP nsXPCToolsCompiler::GetBinDir(nsILocalFile * *aBinDir)
{
    *aBinDir = nsnull;
    
    nsCOMPtr<nsIFile> file;
    nsresult rv = NS_GetSpecialDirectory(NS_XPCOM_CURRENT_PROCESS_DIR, getter_AddRefs(file));
    if(NS_FAILED(rv))
        return rv;

    nsCOMPtr<nsILocalFile> lfile = do_QueryInterface(file);
    NS_ADDREF(*aBinDir = lfile);
    return NS_OK;
}

static void ErrorReporter(JSContext *cx, const char *message,
                          JSErrorReport *report)
{
    printf("compile error!\n");
}

static JSClass global_class = {
    "nsXPCToolsCompiler::global", 0,
    JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,
    JS_EnumerateStub, JS_ResolveStub,   JS_ConvertStub,   nsnull
};

/* void CompileFile (in nsILocalFile aFile, in PRBool strict); */
NS_IMETHODIMP nsXPCToolsCompiler::CompileFile(nsILocalFile *aFile, PRBool strict)
{
    // use the xpccallcontext stuff to get the current JSContext
    
    // get the xpconnect service
    nsresult rv;
    nsCOMPtr<nsIXPConnect> xpc(do_GetService(nsIXPConnect::GetCID(), &rv));
    if(NS_FAILED(rv))
        return NS_ERROR_FAILURE;

    // get the xpconnect native call context
    nsAXPCNativeCallContext *callContext = nsnull;
    xpc->GetCurrentNativeCallContext(&callContext);
    if(!callContext)
        return NS_ERROR_FAILURE;

    // verify that we are being called from JS (i.e. the current call is
    // to this object - though we don't verify that it is to this exact method)
    nsCOMPtr<nsISupports> callee;
    callContext->GetCallee(getter_AddRefs(callee));
    if(!callee || callee.get() != (nsISupports*)this)
        return NS_ERROR_FAILURE;

    // Get JSContext of current call
    JSContext* cx;
    rv = callContext->GetJSContext(&cx);
    if(NS_FAILED(rv) || !cx)
        return NS_ERROR_FAILURE;

    FILE* handle;
    if(NS_FAILED(aFile->OpenANSIFileDesc("r", &handle)))
        return NS_ERROR_FAILURE;

    JSObject* glob = JS_NewObject(cx, &global_class, NULL, NULL);
    if (!glob)
        return NS_ERROR_FAILURE;
    if (!JS_InitStandardClasses(cx, glob))
        return NS_ERROR_FAILURE;

    nsCAutoString path;
    if(NS_FAILED(aFile->GetNativePath(path)))
        return NS_ERROR_FAILURE;

    uint32 oldoptions = JS_GetOptions(cx);
    JS_SetOptions(cx, JSOPTION_WERROR | (strict ? JSOPTION_STRICT : 0));
    JSErrorReporter older = JS_SetErrorReporter(cx, ErrorReporter);
    JSExceptionState *es =JS_SaveExceptionState(cx);

    if(!JS_CompileFileHandle(cx, glob, path.get(), handle))
    {
        jsval v;
        JSErrorReport* report;
        if(JS_GetPendingException(cx, &v) &&
           nsnull != (report = JS_ErrorFromException(cx, v)))
        {
            JSString* str;
            const char* msg = "Error";
            str = JS_ValueToString(cx, v);
            if(str)
                msg = JS_GetStringBytes(str);
            printf("%s [%s,%d]\n\n",
                    msg,
                    report->filename, 
                    (int)report->lineno);            
        }
        else
        {
            printf("no script and no error report!\n");
        }
        
    }    
    JS_RestoreExceptionState(cx, es);
    JS_SetErrorReporter(cx, older);
    JS_SetOptions(cx, oldoptions);
        
    return NS_OK;
}
