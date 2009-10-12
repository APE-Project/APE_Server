/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=2 sw=4 et tw=80:
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
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   John Bandhauer <jband@netscape.com>
 *   Pierre Phaneuf <pp@ludusdesign.com>
 *   IBM Corp.
 *   Dan Mosedale <dan.mosedale@oracle.com>
 *   Serge Gautherie <sgautherie.bz@free.fr>
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

/* XPConnect JavaScript interactive shell. */

#include <stdio.h>
#include "nsXULAppAPI.h"
#include "nsServiceManagerUtils.h"
#include "nsComponentManagerUtils.h"
#include "nsStringAPI.h"
#include "nsIXPConnect.h"
#include "nsIXPCScriptable.h"
#include "nsIInterfaceInfo.h"
#include "nsIInterfaceInfoManager.h"
#include "nsIXPCScriptable.h"
#include "nsIServiceManager.h"
#include "nsIComponentManager.h"
#include "nsIComponentRegistrar.h"
#include "nsILocalFile.h"
#include "nsStringAPI.h"
#include "nsIDirectoryService.h"
#include "nsILocalFile.h"
#include "nsDirectoryServiceDefs.h"
#include "nsAppDirectoryServiceDefs.h"
#include "jsapi.h"
#include "jsdbgapi.h"
#include "jsprf.h"
#include "nscore.h"
#include "nsArrayEnumerator.h"
#include "nsCOMArray.h"
#include "nsDirectoryServiceUtils.h"
#include "nsMemory.h"
#include "nsIGenericFactory.h"
#include "nsISupportsImpl.h"
#include "nsIJSRuntimeService.h"
#include "nsCOMPtr.h"
#include "nsAutoPtr.h"
#include "nsIXPCSecurityManager.h"
#ifdef XP_MACOSX
#include "xpcshellMacUtils.h"
#endif
#ifdef XP_WIN
#include <windows.h>
#endif
#ifdef __SYMBIAN32__
#include <unistd.h>
#endif

#ifndef XPCONNECT_STANDALONE
#include "nsIScriptSecurityManager.h"
#include "nsIPrincipal.h"
#endif

// all this crap is needed to do the interactive shell stuff
#include <stdlib.h>
#include <errno.h>
#ifdef HAVE_IO_H
#include <io.h>     /* for isatty() */
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>     /* for isatty() */
#endif

#include "nsIJSContextStack.h"

#ifdef MOZ_CRASHREPORTER
#include "nsICrashReporter.h"
#endif

class XPCShellDirProvider : public nsIDirectoryServiceProvider2
{
public:
    NS_DECL_ISUPPORTS_INHERITED
    NS_DECL_NSIDIRECTORYSERVICEPROVIDER
    NS_DECL_NSIDIRECTORYSERVICEPROVIDER2

    XPCShellDirProvider() { }
    ~XPCShellDirProvider() { }

    PRBool SetGREDir(const char *dir);
    void ClearGREDir() { mGREDir = nsnull; }

private:
    nsCOMPtr<nsILocalFile> mGREDir;
};

/***************************************************************************/

#ifdef JS_THREADSAFE
#define DoBeginRequest(cx) JS_BeginRequest((cx))
#define DoEndRequest(cx)   JS_EndRequest((cx))
#else
#define DoBeginRequest(cx) ((void)0)
#define DoEndRequest(cx)   ((void)0)
#endif

/***************************************************************************/

static const char kXPConnectServiceContractID[] = "@mozilla.org/js/xpc/XPConnect;1";

#define EXITCODE_RUNTIME_ERROR 3
#define EXITCODE_FILE_NOT_FOUND 4

FILE *gOutFile = NULL;
FILE *gErrFile = NULL;
FILE *gInFile = NULL;

int gExitCode = 0;
JSBool gQuitting = JS_FALSE;
static JSBool reportWarnings = JS_TRUE;
static JSBool compileOnly = JS_FALSE;

JSPrincipals *gJSPrincipals = nsnull;
nsAutoString *gWorkingDirectory = nsnull;

static JSBool
GetLocationProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
#if (!defined(XP_WIN) && !defined(XP_UNIX)) || defined(WINCE)
    //XXX: your platform should really implement this
    return JS_FALSE;
#else
    JSStackFrame *fp = JS_GetScriptedCaller(cx, NULL);
    JSScript *script = JS_GetFrameScript(cx, fp);
    const char *filename = JS_GetScriptFilename(cx, script);

    if (filename) {
        nsresult rv;
        nsCOMPtr<nsIXPConnect> xpc =
            do_GetService(kXPConnectServiceContractID, &rv);

#if defined(XP_WIN)
        // convert from the system codepage to UTF-16
        int bufferSize = MultiByteToWideChar(CP_ACP, 0, filename,
                                             -1, NULL, 0);
        nsAutoString filenameString;
        filenameString.SetLength(bufferSize);
        MultiByteToWideChar(CP_ACP, 0, filename,
                            -1, (LPWSTR)filenameString.BeginWriting(),
                            filenameString.Length());
        // remove the null terminator
        filenameString.SetLength(bufferSize - 1);

        // replace forward slashes with backslashes,
        // since nsLocalFileWin chokes on them
        PRUnichar *start, *end;

        filenameString.BeginWriting(&start, &end);

        while (start != end) {
            if (*start == L'/')
                *start = L'\\';
            start++;
        }
#elif defined(XP_UNIX)
        NS_ConvertUTF8toUTF16 filenameString(filename);
#endif

        nsCOMPtr<nsILocalFile> location;
        if (NS_SUCCEEDED(rv)) {
            rv = NS_NewLocalFile(filenameString,
                                 PR_FALSE, getter_AddRefs(location));
        }

        if (!location && gWorkingDirectory) {
            // could be a relative path, try appending it to the cwd
            // and then normalize
            nsAutoString absolutePath(*gWorkingDirectory);
            absolutePath.Append(filenameString);

            rv = NS_NewLocalFile(absolutePath,
                                 PR_FALSE, getter_AddRefs(location));
        }

        if (location) {
            nsCOMPtr<nsIXPConnectJSObjectHolder> locationHolder;
            JSObject *locationObj = NULL;

            PRBool symlink;
            // don't normalize symlinks, because that's kind of confusing
            if (NS_SUCCEEDED(location->IsSymlink(&symlink)) &&
                !symlink)
                location->Normalize();
            rv = xpc->WrapNative(cx, obj, location,
                                 NS_GET_IID(nsILocalFile),
                                 getter_AddRefs(locationHolder));

            if (NS_SUCCEEDED(rv) &&
                NS_SUCCEEDED(locationHolder->GetJSObject(&locationObj))) {
                *vp = OBJECT_TO_JSVAL(locationObj);
            }
        }
    }

    return JS_TRUE;
#endif
}

#ifdef EDITLINE
extern "C" {
extern char     *readline(const char *prompt);
extern void     add_history(char *line);
}
#endif

static JSBool
GetLine(JSContext *cx, char *bufp, FILE *file, const char *prompt) {
#ifdef EDITLINE
    /*
     * Use readline only if file is stdin, because there's no way to specify
     * another handle.  Are other filehandles interactive?
     */
    if (file == stdin) {
        char *linep = readline(prompt);
        if (!linep)
            return JS_FALSE;
        if (*linep)
            add_history(linep);
        strcpy(bufp, linep);
        JS_free(cx, linep);
        bufp += strlen(bufp);
        *bufp++ = '\n';
        *bufp = '\0';
    } else
#endif
    {
        char line[256];
        fprintf(gOutFile, prompt);
        fflush(gOutFile);
        if (!fgets(line, sizeof line, file))
            return JS_FALSE;
        strcpy(bufp, line);
    }
    return JS_TRUE;
}

static void
my_ErrorReporter(JSContext *cx, const char *message, JSErrorReport *report)
{
    int i, j, k, n;
    char *prefix = NULL, *tmp;
    const char *ctmp;
    JSStackFrame * fp = nsnull;
    nsCOMPtr<nsIXPConnect> xpc;

    // Don't report an exception from inner JS frames as the callers may intend
    // to handle it.
    while ((fp = JS_FrameIterator(cx, &fp))) {
        if (!JS_IsNativeFrame(cx, fp)) {
            return;
        }
    }

    // In some cases cx->fp is null here so use XPConnect to tell us about inner
    // frames.
    if ((xpc = do_GetService(nsIXPConnect::GetCID()))) {
        nsAXPCNativeCallContext *cc = nsnull;
        xpc->GetCurrentNativeCallContext(&cc);
        if (cc) {
            nsAXPCNativeCallContext *prev = cc;
            while (NS_SUCCEEDED(prev->GetPreviousCallContext(&prev)) && prev) {
                PRUint16 lang;
                if (NS_SUCCEEDED(prev->GetLanguage(&lang)) &&
                    lang == nsAXPCNativeCallContext::LANG_JS) {
                    return;
                }
            }
        }
    }

    if (!report) {
        fprintf(gErrFile, "%s\n", message);
        return;
    }

    /* Conditionally ignore reported warnings. */
    if (JSREPORT_IS_WARNING(report->flags) && !reportWarnings)
        return;

    if (report->filename)
        prefix = JS_smprintf("%s:", report->filename);
    if (report->lineno) {
        tmp = prefix;
        prefix = JS_smprintf("%s%u: ", tmp ? tmp : "", report->lineno);
        JS_free(cx, tmp);
    }
    if (JSREPORT_IS_WARNING(report->flags)) {
        tmp = prefix;
        prefix = JS_smprintf("%s%swarning: ",
                             tmp ? tmp : "",
                             JSREPORT_IS_STRICT(report->flags) ? "strict " : "");
        JS_free(cx, tmp);
    }

    /* embedded newlines -- argh! */
    while ((ctmp = strchr(message, '\n')) != 0) {
        ctmp++;
        if (prefix) fputs(prefix, gErrFile);
        fwrite(message, 1, ctmp - message, gErrFile);
        message = ctmp;
    }
    /* If there were no filename or lineno, the prefix might be empty */
    if (prefix)
        fputs(prefix, gErrFile);
    fputs(message, gErrFile);

    if (!report->linebuf) {
        fputc('\n', gErrFile);
        goto out;
    }

    fprintf(gErrFile, ":\n%s%s\n%s", prefix, report->linebuf, prefix);
    n = report->tokenptr - report->linebuf;
    for (i = j = 0; i < n; i++) {
        if (report->linebuf[i] == '\t') {
            for (k = (j + 8) & ~7; j < k; j++) {
                fputc('.', gErrFile);
            }
            continue;
        }
        fputc('.', gErrFile);
        j++;
    }
    fputs("^\n", gErrFile);
 out:
    if (!JSREPORT_IS_WARNING(report->flags))
        gExitCode = EXITCODE_RUNTIME_ERROR;
    JS_free(cx, prefix);
}

static JSBool
ReadLine(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    // While 4096 might be quite arbitrary, this is something to be fixed in
    // bug 105707. It is also the same limit as in ProcessFile.
    char buf[4096];
    JSString *str;

    /* If a prompt was specified, construct the string */
    if (argc > 0) {
        str = JS_ValueToString(cx, argv[0]);
        if (!str)
            return JS_FALSE;
        argv[0] = STRING_TO_JSVAL(str);
    } else {
        str = JSVAL_TO_STRING(JS_GetEmptyStringValue(cx));
    }

    /* Get a line from the infile */
    if (!GetLine(cx, buf, gInFile, JS_GetStringBytes(str)))
        return JS_FALSE;

    /* Strip newline character added by GetLine() */
    unsigned int buflen = strlen(buf);
    if (buflen == 0) {
        if (feof(gInFile)) {
            *rval = JSVAL_NULL;
            return JS_TRUE;
        }
    } else if (buf[buflen - 1] == '\n') {
        --buflen;
    }

    /* Turn buf into a JSString */
    str = JS_NewStringCopyN(cx, buf, buflen);
    if (!str)
        return JS_FALSE;

    *rval = STRING_TO_JSVAL(str);
    return JS_TRUE;
}

static JSBool
Print(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    uintN i, n;
    JSString *str;

    for (i = n = 0; i < argc; i++) {
        str = JS_ValueToString(cx, argv[i]);
        if (!str)
            return JS_FALSE;
        fprintf(gOutFile, "%s%s", i ? " " : "", JS_GetStringBytes(str));
        fflush(gOutFile);
    }
    n++;
    if (n)
        fputc('\n', gOutFile);
    return JS_TRUE;
}

static JSBool
Dump(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSString *str;
    if (!argc)
        return JS_TRUE;

    str = JS_ValueToString(cx, argv[0]);
    if (!str)
        return JS_FALSE;

    fputs(JS_GetStringBytes(str), gOutFile);
    fflush(gOutFile);
    return JS_TRUE;
}

static JSBool
Load(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    uintN i;
    JSString *str;
    const char *filename;
    JSScript *script;
    JSBool ok;
    jsval result;
    FILE *file;

    for (i = 0; i < argc; i++) {
        str = JS_ValueToString(cx, argv[i]);
        if (!str)
            return JS_FALSE;
        argv[i] = STRING_TO_JSVAL(str);
        filename = JS_GetStringBytes(str);
        file = fopen(filename, "r");
        if (!file) {
            JS_ReportError(cx, "cannot open file '%s' for reading", filename);
            return JS_FALSE;
        }
        script = JS_CompileFileHandleForPrincipals(cx, obj, filename, file,
                                                   gJSPrincipals);
        fclose(file);
        if (!script)
            return JS_FALSE;

        ok = !compileOnly
             ? JS_ExecuteScript(cx, obj, script, &result)
             : JS_TRUE;
        JS_DestroyScript(cx, script);
        if (!ok)
            return JS_FALSE;
    }
    return JS_TRUE;
}

static JSBool
Version(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    if (argc > 0 && JSVAL_IS_INT(argv[0]))
        *rval = INT_TO_JSVAL(JS_SetVersion(cx, JSVersion(JSVAL_TO_INT(argv[0]))));
    else
        *rval = INT_TO_JSVAL(JS_GetVersion(cx));
    return JS_TRUE;
}

static JSBool
BuildDate(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    fprintf(gOutFile, "built on %s at %s\n", __DATE__, __TIME__);
    return JS_TRUE;
}

static JSBool
Quit(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    gExitCode = 0;
    JS_ConvertArguments(cx, argc, argv,"/ i", &gExitCode);

    gQuitting = JS_TRUE;
//    exit(0);
    return JS_FALSE;
}

static JSBool
DumpXPC(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    int32 depth = 2;

    if (argc > 0) {
        if (!JS_ValueToInt32(cx, argv[0], &depth))
            return JS_FALSE;
    }

    nsCOMPtr<nsIXPConnect> xpc = do_GetService(nsIXPConnect::GetCID());
    if(xpc)
        xpc->DebugDump((int16)depth);
    return JS_TRUE;
}

/* XXX needed only by GC() */
#include "jscntxt.h"

static JSBool
GC(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSRuntime *rt;
    uint32 preBytes;

    rt = cx->runtime;
    preBytes = rt->gcBytes;
    JS_GC(cx);
    fprintf(gOutFile, "before %lu, after %lu, break %08lx\n",
           (unsigned long)preBytes, (unsigned long)rt->gcBytes,
#if defined(XP_UNIX) && !defined(__SYMBIAN32__)
           (unsigned long)sbrk(0)
#else
           0
#endif
           );
#ifdef JS_GCMETER
    js_DumpGCStats(rt, stdout);
#endif
    return JS_TRUE;
}

#ifdef DEBUG

static JSBool
DumpHeap(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    char *fileName = NULL;
    void* startThing = NULL;
    uint32 startTraceKind = 0;
    void *thingToFind = NULL;
    size_t maxDepth = (size_t)-1;
    void *thingToIgnore = NULL;
    jsval *vp;
    FILE *dumpFile;
    JSBool ok;

    vp = &argv[0];
    if (*vp != JSVAL_NULL && *vp != JSVAL_VOID) {
        JSString *str;

        str = JS_ValueToString(cx, *vp);
        if (!str)
            return JS_FALSE;
        *vp = STRING_TO_JSVAL(str);
        fileName = JS_GetStringBytes(str);
    }

    vp = &argv[1];
    if (*vp != JSVAL_NULL && *vp != JSVAL_VOID) {
        if (!JSVAL_IS_TRACEABLE(*vp))
            goto not_traceable_arg;
        startThing = JSVAL_TO_TRACEABLE(*vp);
        startTraceKind = JSVAL_TRACE_KIND(*vp);
    }

    vp = &argv[2];
    if (*vp != JSVAL_NULL && *vp != JSVAL_VOID) {
        if (!JSVAL_IS_TRACEABLE(*vp))
            goto not_traceable_arg;
        thingToFind = JSVAL_TO_TRACEABLE(*vp);
    }

    vp = &argv[3];
    if (*vp != JSVAL_NULL && *vp != JSVAL_VOID) {
        uint32 depth;

        if (!JS_ValueToECMAUint32(cx, *vp, &depth))
            return JS_FALSE;
        maxDepth = depth;
    }

    vp = &argv[4];
    if (*vp != JSVAL_NULL && *vp != JSVAL_VOID) {
        if (!JSVAL_IS_TRACEABLE(*vp))
            goto not_traceable_arg;
        thingToIgnore = JSVAL_TO_TRACEABLE(*vp);
    }

    if (!fileName) {
        dumpFile = gOutFile;
    } else {
        dumpFile = fopen(fileName, "w");
        if (!dumpFile) {
            fprintf(gErrFile, "dumpHeap: can't open %s: %s\n",
                    fileName, strerror(errno));
            return JS_FALSE;
        }
    }

    ok = JS_DumpHeap(cx, dumpFile, startThing, startTraceKind, thingToFind,
                     maxDepth, thingToIgnore);
    if (dumpFile != gOutFile)
        fclose(dumpFile);
    return ok;

  not_traceable_arg:
    fprintf(gErrFile,
            "dumpHeap: argument %u is not null or a heap-allocated thing\n",
            (unsigned)(vp - argv));
    return JS_FALSE;
}

#endif /* DEBUG */

static JSBool
Clear(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    if (argc > 0 && !JSVAL_IS_PRIMITIVE(argv[0])) {
        JS_ClearScope(cx, JSVAL_TO_OBJECT(argv[0]));
    } else {
        JS_ReportError(cx, "'clear' requires an object");
        return JS_FALSE;
    }
    return JS_TRUE;
}

/*
 * JSContext option name to flag map. The option names are in alphabetical
 * order for better reporting.
 */
static const struct {
    const char  *name;
    uint32      flag;
} js_options[] = {
    {"anonfunfix",      JSOPTION_ANONFUNFIX},
    {"atline",          JSOPTION_ATLINE},
    {"jit",             JSOPTION_JIT},
    {"relimit",         JSOPTION_RELIMIT},
    {"strict",          JSOPTION_STRICT},
    {"werror",          JSOPTION_WERROR},
    {"xml",             JSOPTION_XML},
};

static uint32
MapContextOptionNameToFlag(JSContext* cx, const char* name)
{
    for (size_t i = 0; i != JS_ARRAY_LENGTH(js_options); ++i) {
        if (strcmp(name, js_options[i].name) == 0)
            return js_options[i].flag;
    }

    char* msg = JS_sprintf_append(NULL,
                                  "unknown option name '%s'."
                                  " The valid names are ", name);
    for (size_t i = 0; i != JS_ARRAY_LENGTH(js_options); ++i) {
        if (!msg)
            break;
        msg = JS_sprintf_append(msg, "%s%s", js_options[i].name,
                                (i + 2 < JS_ARRAY_LENGTH(js_options)
                                 ? ", "
                                 : i + 2 == JS_ARRAY_LENGTH(js_options)
                                 ? " and "
                                 : "."));
    }
    if (!msg) {
        JS_ReportOutOfMemory(cx);
    } else {
        JS_ReportError(cx, msg);
        free(msg);
    }
    return 0;
}

static JSBool
Options(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    uint32 optset, flag;
    JSString *str;
    const char *opt;
    char *names;
    JSBool found;

    optset = 0;
    for (uintN i = 0; i < argc; i++) {
        str = JS_ValueToString(cx, argv[i]);
        if (!str)
            return JS_FALSE;
        argv[i] = STRING_TO_JSVAL(str);
        opt = JS_GetStringBytes(str);
        if (!opt)
            return JS_FALSE;
        flag = MapContextOptionNameToFlag(cx,  opt);
        if (!flag)
            return JS_FALSE;
        optset |= flag;
    }
    optset = JS_ToggleOptions(cx, optset);

    names = NULL;
    found = JS_FALSE;
    for (size_t i = 0; i != JS_ARRAY_LENGTH(js_options); i++) {
        if (js_options[i].flag & optset) {
            found = JS_TRUE;
            names = JS_sprintf_append(names, "%s%s",
                                      names ? "," : "", js_options[i].name);
            if (!names)
                break;
        }
    }
    if (!found)
        names = strdup("");
    if (!names) {
        JS_ReportOutOfMemory(cx);
        return JS_FALSE;
    }
    str = JS_NewString(cx, names, strlen(names));
    if (!str) {
        free(names);
        return JS_FALSE;
    }
    *rval = STRING_TO_JSVAL(str);
    return JS_TRUE;
}

static JSFunctionSpec glob_functions[] = {
    {"print",           Print,          0,0,0},
    {"readline",        ReadLine,       1,0,0},
    {"load",            Load,           1,0,0},
    {"quit",            Quit,           0,0,0},
    {"version",         Version,        1,0,0},
    {"build",           BuildDate,      0,0,0},
    {"dumpXPC",         DumpXPC,        1,0,0},
    {"dump",            Dump,           1,0,0},
    {"gc",              GC,             0,0,0},
    {"clear",           Clear,          1,0,0},
    {"options",         Options,        0,0,0},
#ifdef DEBUG
    {"dumpHeap",        DumpHeap,       5,0,0},
#endif
#ifdef MOZ_SHARK
    {"startShark",      js_StartShark,      0,0,0},
    {"stopShark",       js_StopShark,       0,0,0},
    {"connectShark",    js_ConnectShark,    0,0,0},
    {"disconnectShark", js_DisconnectShark, 0,0,0},
#endif
#ifdef MOZ_CALLGRIND
    {"startCallgrind",  js_StartCallgrind,  0,0,0},
    {"stopCallgrind",   js_StopCallgrind,   0,0,0},
    {"dumpCallgrind",   js_DumpCallgrind,   1,0,0},
#endif
    {nsnull,nsnull,0,0,0}
};

JSClass global_class = {
    "global", 0,
    JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,  JS_PropertyStub,
    JS_EnumerateStub, JS_ResolveStub,   JS_ConvertStub,   nsnull
};

static JSBool
env_setProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
/* XXX porting may be easy, but these don't seem to supply setenv by default */
#if !defined XP_BEOS && !defined XP_OS2 && !defined SOLARIS
    JSString *idstr, *valstr;
    const char *name, *value;
    int rv;

    idstr = JS_ValueToString(cx, id);
    valstr = JS_ValueToString(cx, *vp);
    if (!idstr || !valstr)
        return JS_FALSE;
    name = JS_GetStringBytes(idstr);
    value = JS_GetStringBytes(valstr);
#if defined XP_WIN || defined HPUX || defined OSF1 || defined IRIX \
    || defined SCO
    {
        char *waste = JS_smprintf("%s=%s", name, value);
        if (!waste) {
            JS_ReportOutOfMemory(cx);
            return JS_FALSE;
        }
        rv = putenv(waste);
#ifdef XP_WIN
        /*
         * HPUX9 at least still has the bad old non-copying putenv.
         *
         * Per mail from <s.shanmuganathan@digital.com>, OSF1 also has a putenv
         * that will crash if you pass it an auto char array (so it must place
         * its argument directly in the char *environ[] array).
         */
        free(waste);
#endif
    }
#else
    rv = setenv(name, value, 1);
#endif
    if (rv < 0) {
        JS_ReportError(cx, "can't set envariable %s to %s", name, value);
        return JS_FALSE;
    }
    *vp = STRING_TO_JSVAL(valstr);
#endif /* !defined XP_BEOS && !defined XP_OS2 && !defined SOLARIS */
    return JS_TRUE;
}

static JSBool
env_enumerate(JSContext *cx, JSObject *obj)
{
    static JSBool reflected;
    char **evp, *name, *value;
    JSString *valstr;
    JSBool ok;

    if (reflected)
        return JS_TRUE;

    for (evp = (char **)JS_GetPrivate(cx, obj); (name = *evp) != NULL; evp++) {
        value = strchr(name, '=');
        if (!value)
            continue;
        *value++ = '\0';
        valstr = JS_NewStringCopyZ(cx, value);
        if (!valstr) {
            ok = JS_FALSE;
        } else {
            ok = JS_DefineProperty(cx, obj, name, STRING_TO_JSVAL(valstr),
                                   NULL, NULL, JSPROP_ENUMERATE);
        }
        value[-1] = '=';
        if (!ok)
            return JS_FALSE;
    }

    reflected = JS_TRUE;
    return JS_TRUE;
}

static JSBool
env_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
            JSObject **objp)
{
    JSString *idstr, *valstr;
    const char *name, *value;

    if (flags & JSRESOLVE_ASSIGNING)
        return JS_TRUE;

    idstr = JS_ValueToString(cx, id);
    if (!idstr)
        return JS_FALSE;
    name = JS_GetStringBytes(idstr);
    value = getenv(name);
    if (value) {
        valstr = JS_NewStringCopyZ(cx, value);
        if (!valstr)
            return JS_FALSE;
        if (!JS_DefineProperty(cx, obj, name, STRING_TO_JSVAL(valstr),
                               NULL, NULL, JSPROP_ENUMERATE)) {
            return JS_FALSE;
        }
        *objp = obj;
    }
    return JS_TRUE;
}

static JSClass env_class = {
    "environment", JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE,
    JS_PropertyStub,  JS_PropertyStub,
    JS_PropertyStub,  env_setProperty,
    env_enumerate, (JSResolveOp) env_resolve,
    JS_ConvertStub,   nsnull
};

/***************************************************************************/

typedef enum JSShellErrNum {
#define MSG_DEF(name, number, count, exception, format) \
    name = number,
#include "jsshell.msg"
#undef MSG_DEF
    JSShellErr_Limit
#undef MSGDEF
} JSShellErrNum;

JSErrorFormatString jsShell_ErrorFormatString[JSErr_Limit] = {
#define MSG_DEF(name, number, count, exception, format) \
    { format, count } ,
#include "jsshell.msg"
#undef MSG_DEF
};

static const JSErrorFormatString *
my_GetErrorMessage(void *userRef, const char *locale, const uintN errorNumber)
{
    if ((errorNumber > 0) && (errorNumber < JSShellErr_Limit))
            return &jsShell_ErrorFormatString[errorNumber];
        else
            return NULL;
}

static void
ProcessFile(JSContext *cx, JSObject *obj, const char *filename, FILE *file,
            JSBool forceTTY)
{
    JSScript *script;
    jsval result;
    int lineno, startline;
    JSBool ok, hitEOF;
    char *bufp, buffer[4096];
    JSString *str;

    if (forceTTY) {
        file = stdin;
    }
    else
#ifdef HAVE_ISATTY
    if (!isatty(fileno(file)))
#endif
    {
        /*
         * It's not interactive - just execute it.
         *
         * Support the UNIX #! shell hack; gobble the first line if it starts
         * with '#'.  TODO - this isn't quite compatible with sharp variables,
         * as a legal js program (using sharp variables) might start with '#'.
         * But that would require multi-character lookahead.
         */
        int ch = fgetc(file);
        if (ch == '#') {
            while((ch = fgetc(file)) != EOF) {
                if(ch == '\n' || ch == '\r')
                    break;
            }
        }
        ungetc(ch, file);
        DoBeginRequest(cx);

        script = JS_CompileFileHandleForPrincipals(cx, obj, filename, file,
                                                   gJSPrincipals);

        if (script) {
            if (!compileOnly)
                (void)JS_ExecuteScript(cx, obj, script, &result);
            JS_DestroyScript(cx, script);
        }
        DoEndRequest(cx);

        return;
    }

    /* It's an interactive filehandle; drop into read-eval-print loop. */
    lineno = 1;
    hitEOF = JS_FALSE;
    do {
        bufp = buffer;
        *bufp = '\0';

        /*
         * Accumulate lines until we get a 'compilable unit' - one that either
         * generates an error (before running out of source) or that compiles
         * cleanly.  This should be whenever we get a complete statement that
         * coincides with the end of a line.
         */
        startline = lineno;
        do {
            if (!GetLine(cx, bufp, file, startline == lineno ? "js> " : "")) {
                hitEOF = JS_TRUE;
                break;
            }
            bufp += strlen(bufp);
            lineno++;
        } while (!JS_BufferIsCompilableUnit(cx, obj, buffer, strlen(buffer)));

        DoBeginRequest(cx);
        /* Clear any pending exception from previous failed compiles.  */
        JS_ClearPendingException(cx);
        script = JS_CompileScriptForPrincipals(cx, obj, gJSPrincipals, buffer,
                                               strlen(buffer), "typein", startline);
        if (script) {
            JSErrorReporter older;

            if (!compileOnly) {
                ok = JS_ExecuteScript(cx, obj, script, &result);
                if (ok && result != JSVAL_VOID) {
                    /* Suppress error reports from JS_ValueToString(). */
                    older = JS_SetErrorReporter(cx, NULL);
                    str = JS_ValueToString(cx, result);
                    JS_SetErrorReporter(cx, older);

                    if (str)
                        fprintf(gOutFile, "%s\n", JS_GetStringBytes(str));
                    else
                        ok = JS_FALSE;
                }
            }
            JS_DestroyScript(cx, script);
        }
        DoEndRequest(cx);
    } while (!hitEOF && !gQuitting);

    fprintf(gOutFile, "\n");
}

static void
Process(JSContext *cx, JSObject *obj, const char *filename, JSBool forceTTY)
{
    FILE *file;

    if (forceTTY || !filename || strcmp(filename, "-") == 0) {
        file = stdin;
    } else {
        file = fopen(filename, "r");
        if (!file) {
            JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL,
                                 JSSMSG_CANT_OPEN,
                                 filename, strerror(errno));
            gExitCode = EXITCODE_FILE_NOT_FOUND;
            return;
        }
    }

    ProcessFile(cx, obj, filename, file, forceTTY);
    if (file != stdin)
        fclose(file);
}

static int
usage(void)
{
    fprintf(gErrFile, "%s\n", JS_GetImplementationVersion());
    fprintf(gErrFile, "usage: xpcshell [-g gredir] [-PsSwWxCij] [-v version] [-f scriptfile] [-e script] [scriptfile] [scriptarg...]\n");
    return 2;
}

extern JSClass global_class;

static int
ProcessArgs(JSContext *cx, JSObject *obj, char **argv, int argc)
{
    const char rcfilename[] = "xpcshell.js";
    FILE *rcfile;
    int i, j, length;
    JSObject *argsObj;
    char *filename = NULL;
    JSBool isInteractive = JS_TRUE;
    JSBool forceTTY = JS_FALSE;

    rcfile = fopen(rcfilename, "r");
    if (rcfile) {
        printf("[loading '%s'...]\n", rcfilename);
        ProcessFile(cx, obj, rcfilename, rcfile, JS_FALSE);
        fclose(rcfile);
    }

    /*
     * Scan past all optional arguments so we can create the arguments object
     * before processing any -f options, which must interleave properly with
     * -v and -w options.  This requires two passes, and without getopt, we'll
     * have to keep the option logic here and in the second for loop in sync.
     */
    for (i = 0; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            ++i;
            break;
        }
        switch (argv[i][1]) {
          case 'v':
          case 'f':
          case 'e':
            ++i;
            break;
          default:;
        }
    }

    /*
     * Create arguments early and define it to root it, so it's safe from any
     * GC calls nested below, and so it is available to -f <file> arguments.
     */
    argsObj = JS_NewArrayObject(cx, 0, NULL);
    if (!argsObj)
        return 1;
    if (!JS_DefineProperty(cx, obj, "arguments", OBJECT_TO_JSVAL(argsObj),
                           NULL, NULL, 0)) {
        return 1;
    }

    length = argc - i;
    for (j = 0; j < length; j++) {
        JSString *str = JS_NewStringCopyZ(cx, argv[i++]);
        if (!str)
            return 1;
        if (!JS_DefineElement(cx, argsObj, j, STRING_TO_JSVAL(str),
                              NULL, NULL, JSPROP_ENUMERATE)) {
            return 1;
        }
    }

    for (i = 0; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            filename = argv[i++];
            isInteractive = JS_FALSE;
            break;
        }
        switch (argv[i][1]) {
        case 'v':
            if (++i == argc) {
                return usage();
            }
            JS_SetVersion(cx, JSVersion(atoi(argv[i])));
            break;
        case 'W':
            reportWarnings = JS_FALSE;
            break;
        case 'w':
            reportWarnings = JS_TRUE;
            break;
        case 'S':
            JS_ToggleOptions(cx, JSOPTION_WERROR);
        case 's':
            JS_ToggleOptions(cx, JSOPTION_STRICT);
            break;
        case 'x':
            JS_ToggleOptions(cx, JSOPTION_XML);
            break;
        case 'P':
            if (JS_GET_CLASS(cx, JS_GetPrototype(cx, obj)) != &global_class) {
                JSObject *gobj;

                if (!JS_SealObject(cx, obj, JS_TRUE))
                    return JS_FALSE;
                gobj = JS_NewObject(cx, &global_class, NULL, NULL);
                if (!gobj)
                    return JS_FALSE;
                if (!JS_SetPrototype(cx, gobj, obj))
                    return JS_FALSE;
                JS_SetParent(cx, gobj, NULL);
                JS_SetGlobalObject(cx, gobj);
                obj = gobj;
            }
            break;
        case 'f':
            if (++i == argc) {
                return usage();
            }
            Process(cx, obj, argv[i], JS_FALSE);
            /*
             * XXX: js -f foo.js should interpret foo.js and then
             * drop into interactive mode, but that breaks test
             * harness. Just execute foo.js for now.
             */
            isInteractive = JS_FALSE;
            break;
        case 'i':
            isInteractive = forceTTY = JS_TRUE;
            break;
        case 'e':
        {
            jsval rval;

            if (++i == argc) {
                return usage();
            }

            JS_EvaluateScriptForPrincipals(cx, obj, gJSPrincipals, argv[i],
                                           strlen(argv[i]), "-e", 1, &rval);

            isInteractive = JS_FALSE;
            break;
        }
        case 'C':
            compileOnly = JS_TRUE;
            isInteractive = JS_FALSE;
            break;
        case 'j':
            JS_ToggleOptions(cx, JSOPTION_JIT);
            break;
#ifdef MOZ_SHARK
        case 'k':
            JS_ConnectShark();
            break;
#endif
        default:
            return usage();
        }
    }

    if (filename || isInteractive)
        Process(cx, obj, filename, forceTTY);
    return gExitCode;
}

/***************************************************************************/

class FullTrustSecMan
#ifndef XPCONNECT_STANDALONE
  : public nsIScriptSecurityManager
#else
  : public nsIXPCSecurityManager
#endif
{
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIXPCSECURITYMANAGER
#ifndef XPCONNECT_STANDALONE
  NS_DECL_NSISCRIPTSECURITYMANAGER
#endif

  FullTrustSecMan();
  virtual ~FullTrustSecMan();

#ifndef XPCONNECT_STANDALONE
  void SetSystemPrincipal(nsIPrincipal *aPrincipal) {
    mSystemPrincipal = aPrincipal;
  }

private:
  nsCOMPtr<nsIPrincipal> mSystemPrincipal;
#endif
};

NS_INTERFACE_MAP_BEGIN(FullTrustSecMan)
  NS_INTERFACE_MAP_ENTRY(nsIXPCSecurityManager)
#ifndef XPCONNECT_STANDALONE
  NS_INTERFACE_MAP_ENTRY(nsIScriptSecurityManager)
#endif
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIXPCSecurityManager)
NS_INTERFACE_MAP_END

NS_IMPL_ADDREF(FullTrustSecMan)
NS_IMPL_RELEASE(FullTrustSecMan)

FullTrustSecMan::FullTrustSecMan()
{
#ifndef XPCONNECT_STANDALONE
  mSystemPrincipal = nsnull;
#endif
}

FullTrustSecMan::~FullTrustSecMan()
{
}

NS_IMETHODIMP
FullTrustSecMan::CanCreateWrapper(JSContext * aJSContext, const nsIID & aIID,
                                  nsISupports *aObj, nsIClassInfo *aClassInfo,
                                  void * *aPolicy)
{
    return NS_OK;
}

NS_IMETHODIMP
FullTrustSecMan::CanCreateInstance(JSContext * aJSContext, const nsCID & aCID)
{
    return NS_OK;
}

NS_IMETHODIMP
FullTrustSecMan::CanGetService(JSContext * aJSContext, const nsCID & aCID)
{
    return NS_OK;
}

#ifndef XPCONNECT_STANDALONE
/* void CanAccess (in PRUint32 aAction, in nsIXPCNativeCallContext aCallContext, in JSContextPtr aJSContext, in JSObjectPtr aJSObject, in nsISupports aObj, in nsIClassInfo aClassInfo, in JSVal aName, inout voidPtr aPolicy); */
NS_IMETHODIMP
FullTrustSecMan::CanAccess(PRUint32 aAction,
                           nsAXPCNativeCallContext *aCallContext,
                           JSContext * aJSContext, JSObject * aJSObject,
                           nsISupports *aObj, nsIClassInfo *aClassInfo,
                           jsval aName, void * *aPolicy)
{
    return NS_OK;
}

/* [noscript] void checkPropertyAccess (in JSContextPtr aJSContext, in JSObjectPtr aJSObject, in string aClassName, in JSVal aProperty, in PRUint32 aAction); */
NS_IMETHODIMP
FullTrustSecMan::CheckPropertyAccess(JSContext * aJSContext,
                                     JSObject * aJSObject,
                                     const char *aClassName,
                                     jsval aProperty, PRUint32 aAction)
{
    return NS_OK;
}

/* [noscript] void checkConnect (in JSContextPtr aJSContext, in nsIURI aTargetURI, in string aClassName, in string aProperty); */
NS_IMETHODIMP
FullTrustSecMan::CheckConnect(JSContext * aJSContext, nsIURI *aTargetURI,
                              const char *aClassName, const char *aProperty)
{
    return NS_OK;
}

/* [noscript] void checkLoadURIFromScript (in JSContextPtr cx, in nsIURI uri); */
NS_IMETHODIMP
FullTrustSecMan::CheckLoadURIFromScript(JSContext * cx, nsIURI *uri)
{
    return NS_OK;
}

/* void checkLoadURIWithPrincipal (in nsIPrincipal aPrincipal, in nsIURI uri, in unsigned long flags); */
NS_IMETHODIMP
FullTrustSecMan::CheckLoadURIWithPrincipal(nsIPrincipal *aPrincipal,
                                           nsIURI *uri, PRUint32 flags)
{
    return NS_OK;
}

/* void checkLoadURI (in nsIURI from, in nsIURI uri, in unsigned long flags); */
NS_IMETHODIMP
FullTrustSecMan::CheckLoadURI(nsIURI *from, nsIURI *uri, PRUint32 flags)
{
    return NS_OK;
}

/* void checkLoadURIStrWithPrincipal (in nsIPrincipal aPrincipal, in AUTF8String uri, in unsigned long flags); */
NS_IMETHODIMP
FullTrustSecMan::CheckLoadURIStrWithPrincipal(nsIPrincipal *aPrincipal,
                                              const nsACString & uri,
                                              PRUint32 flags)
{
    return NS_OK;
}

/* void checkLoadURIStr (in AUTF8String from, in AUTF8String uri, in unsigned long flags); */
NS_IMETHODIMP
FullTrustSecMan::CheckLoadURIStr(const nsACString & from,
                                 const nsACString & uri, PRUint32 flags)
{
    return NS_OK;
}

/* [noscript] void checkFunctionAccess (in JSContextPtr cx, in voidPtr funObj, in voidPtr targetObj); */
NS_IMETHODIMP
FullTrustSecMan::CheckFunctionAccess(JSContext * cx, void * funObj,
                                     void * targetObj)
{
    return NS_OK;
}

/* [noscript] boolean canExecuteScripts (in JSContextPtr cx, in nsIPrincipal principal); */
NS_IMETHODIMP
FullTrustSecMan::CanExecuteScripts(JSContext * cx, nsIPrincipal *principal,
                                   PRBool *_retval)
{
    *_retval = PR_TRUE;
    return NS_OK;
}

/* [noscript] nsIPrincipal getSubjectPrincipal (); */
NS_IMETHODIMP
FullTrustSecMan::GetSubjectPrincipal(nsIPrincipal **_retval)
{
    NS_IF_ADDREF(*_retval = mSystemPrincipal);
    return *_retval ? NS_OK : NS_ERROR_FAILURE;
}

/* [noscript] void pushContextPrincipal (in JSContextPtr cx, in JSStackFramePtr fp, in nsIPrincipal principal); */
NS_IMETHODIMP
FullTrustSecMan::PushContextPrincipal(JSContext * cx, JSStackFrame * fp, nsIPrincipal *principal)
{
    return NS_OK;
}

/* [noscript] void popContextPrincipal (in JSContextPtr cx); */
NS_IMETHODIMP
FullTrustSecMan::PopContextPrincipal(JSContext * cx)
{
    return NS_OK;
}

/* [noscript] nsIPrincipal getSystemPrincipal (); */
NS_IMETHODIMP
FullTrustSecMan::GetSystemPrincipal(nsIPrincipal **_retval)
{
    NS_IF_ADDREF(*_retval = mSystemPrincipal);
    return *_retval ? NS_OK : NS_ERROR_FAILURE;
}

/* [noscript] nsIPrincipal getCertificatePrincipal (in AUTF8String aCertFingerprint, in AUTF8String aSubjectName, in AUTF8String aPrettyName, in nsISupports aCert, in nsIURI aURI); */
NS_IMETHODIMP
FullTrustSecMan::GetCertificatePrincipal(const nsACString & aCertFingerprint,
                                         const nsACString & aSubjectName,
                                         const nsACString & aPrettyName,
                                         nsISupports *aCert, nsIURI *aURI,
                                         nsIPrincipal **_retval)
{
    NS_IF_ADDREF(*_retval = mSystemPrincipal);
    return *_retval ? NS_OK : NS_ERROR_FAILURE;
}

/* [noscript] nsIPrincipal getCodebasePrincipal (in nsIURI aURI); */
NS_IMETHODIMP
FullTrustSecMan::GetCodebasePrincipal(nsIURI *aURI, nsIPrincipal **_retval)
{
    NS_IF_ADDREF(*_retval = mSystemPrincipal);
    return *_retval ? NS_OK : NS_ERROR_FAILURE;
}

/* [noscript] short requestCapability (in nsIPrincipal principal, in string capability); */
NS_IMETHODIMP
FullTrustSecMan::RequestCapability(nsIPrincipal *principal,
                                   const char *capability, PRInt16 *_retval)
{
    *_retval = nsIPrincipal::ENABLE_GRANTED;
    return NS_OK;
}

/* boolean isCapabilityEnabled (in string capability); */
NS_IMETHODIMP
FullTrustSecMan::IsCapabilityEnabled(const char *capability, PRBool *_retval)
{
    *_retval = PR_TRUE;
    return NS_OK;
}

/* void enableCapability (in string capability); */
NS_IMETHODIMP
FullTrustSecMan::EnableCapability(const char *capability)
{
    return NS_OK;;
}

/* void revertCapability (in string capability); */
NS_IMETHODIMP
FullTrustSecMan::RevertCapability(const char *capability)
{
    return NS_OK;
}

/* void disableCapability (in string capability); */
NS_IMETHODIMP
FullTrustSecMan::DisableCapability(const char *capability)
{
    return NS_OK;
}

/* void setCanEnableCapability (in AUTF8String certificateFingerprint, in string capability, in short canEnable); */
NS_IMETHODIMP
FullTrustSecMan::SetCanEnableCapability(const nsACString & certificateFingerprint,
                                        const char *capability,
                                        PRInt16 canEnable)
{
    return NS_OK;
}

/* [noscript] nsIPrincipal getObjectPrincipal (in JSContextPtr cx, in JSObjectPtr obj); */
NS_IMETHODIMP
FullTrustSecMan::GetObjectPrincipal(JSContext * cx, JSObject * obj,
                                    nsIPrincipal **_retval)
{
    NS_IF_ADDREF(*_retval = mSystemPrincipal);
    return *_retval ? NS_OK : NS_ERROR_FAILURE;
}

/* [noscript] boolean subjectPrincipalIsSystem (); */
NS_IMETHODIMP
FullTrustSecMan::SubjectPrincipalIsSystem(PRBool *_retval)
{
    *_retval = PR_TRUE;
    return NS_OK;
}

/* [noscript] void checkSameOrigin (in JSContextPtr aJSContext, in nsIURI aTargetURI); */
NS_IMETHODIMP
FullTrustSecMan::CheckSameOrigin(JSContext * aJSContext, nsIURI *aTargetURI)
{
    return NS_OK;
}

/* void checkSameOriginURI (in nsIURI aSourceURI, in nsIURI aTargetURI); */
NS_IMETHODIMP
FullTrustSecMan::CheckSameOriginURI(nsIURI *aSourceURI, nsIURI *aTargetURI,
                                    PRBool reportError)
{
    return NS_OK;
}

/* [noscript] nsIPrincipal getPrincipalFromContext (in JSContextPtr cx); */
NS_IMETHODIMP
FullTrustSecMan::GetPrincipalFromContext(JSContext * cx, nsIPrincipal **_retval)
{
    NS_IF_ADDREF(*_retval = mSystemPrincipal);
    return *_retval ? NS_OK : NS_ERROR_FAILURE;
}

/* [noscript] nsIPrincipal getChannelPrincipal (in nsIChannel aChannel); */
NS_IMETHODIMP
FullTrustSecMan::GetChannelPrincipal(nsIChannel *aChannel, nsIPrincipal **_retval)
{
    NS_IF_ADDREF(*_retval = mSystemPrincipal);
    return *_retval ? NS_OK : NS_ERROR_FAILURE;
}

/* boolean isSystemPrincipal (in nsIPrincipal aPrincipal); */
NS_IMETHODIMP
FullTrustSecMan::IsSystemPrincipal(nsIPrincipal *aPrincipal, PRBool *_retval)
{
    *_retval = aPrincipal == mSystemPrincipal;
    return NS_OK;
}

NS_IMETHODIMP_(nsIPrincipal *)
FullTrustSecMan::GetCxSubjectPrincipal(JSContext *cx)
{
    return mSystemPrincipal;
}

NS_IMETHODIMP_(nsIPrincipal *)
FullTrustSecMan::GetCxSubjectPrincipalAndFrame(JSContext *cx, JSStackFrame **fp)
{
    *fp = nsnull;
    return mSystemPrincipal;
}

#endif

/***************************************************************************/

// #define TEST_InitClassesWithNewWrappedGlobal

#ifdef TEST_InitClassesWithNewWrappedGlobal
// XXX hacky test code...
#include "xpctest.h"

class TestGlobal : public nsIXPCTestNoisy, public nsIXPCScriptable
{
public:
    NS_DECL_ISUPPORTS
    NS_DECL_NSIXPCTESTNOISY
    NS_DECL_NSIXPCSCRIPTABLE

    TestGlobal(){}
};

NS_IMPL_ISUPPORTS2(TestGlobal, nsIXPCTestNoisy, nsIXPCScriptable)

// The nsIXPCScriptable map declaration that will generate stubs for us...
#define XPC_MAP_CLASSNAME           TestGlobal
#define XPC_MAP_QUOTED_CLASSNAME   "TestGlobal"
#define XPC_MAP_FLAGS               nsIXPCScriptable::USE_JSSTUB_FOR_ADDPROPERTY |\
                                    nsIXPCScriptable::USE_JSSTUB_FOR_DELPROPERTY |\
                                    nsIXPCScriptable::USE_JSSTUB_FOR_SETPROPERTY
#include "xpc_map_end.h" /* This will #undef the above */

NS_IMETHODIMP TestGlobal::Squawk() {return NS_OK;}

#endif

// uncomment to install the test 'this' translator
// #define TEST_TranslateThis

#ifdef TEST_TranslateThis

#include "xpctest.h"

class nsXPCFunctionThisTranslator : public nsIXPCFunctionThisTranslator
{
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIXPCFUNCTIONTHISTRANSLATOR

  nsXPCFunctionThisTranslator();
  virtual ~nsXPCFunctionThisTranslator();
  /* additional members */
};

/* Implementation file */
NS_IMPL_ISUPPORTS1(nsXPCFunctionThisTranslator, nsIXPCFunctionThisTranslator)

nsXPCFunctionThisTranslator::nsXPCFunctionThisTranslator()
{
  /* member initializers and constructor code */
}

nsXPCFunctionThisTranslator::~nsXPCFunctionThisTranslator()
{
  /* destructor code */
#ifdef DEBUG_jband
    printf("destroying nsXPCFunctionThisTranslator\n");
#endif
}

/* nsISupports TranslateThis (in nsISupports aInitialThis, in nsIInterfaceInfo aInterfaceInfo, in PRUint16 aMethodIndex, out PRBool aHideFirstParamFromJS, out nsIIDPtr aIIDOfResult); */
NS_IMETHODIMP
nsXPCFunctionThisTranslator::TranslateThis(nsISupports *aInitialThis,
                                           nsIInterfaceInfo *aInterfaceInfo,
                                           PRUint16 aMethodIndex,
                                           PRBool *aHideFirstParamFromJS,
                                           nsIID * *aIIDOfResult,
                                           nsISupports **_retval)
{
    NS_IF_ADDREF(aInitialThis);
    *_retval = aInitialThis;
    *aHideFirstParamFromJS = JS_FALSE;
    *aIIDOfResult = nsnull;
    return NS_OK;
}

#endif

// ContextCallback calls are chained
static JSContextCallback gOldJSContextCallback;

static JSBool
ContextCallback(JSContext *cx, uintN contextOp)
{
    if (gOldJSContextCallback && !gOldJSContextCallback(cx, contextOp))
        return JS_FALSE;

    if (contextOp == JSCONTEXT_NEW) {
        JS_SetErrorReporter(cx, my_ErrorReporter);
        JS_SetVersion(cx, JSVERSION_LATEST);
    }
    return JS_TRUE;
}

static bool
GetCurrentWorkingDirectory(nsAString& workingDirectory)
{
#if (!defined(XP_WIN) && !defined(XP_UNIX)) || defined(WINCE)
    //XXX: your platform should really implement this
    return false;
#elif XP_WIN
    DWORD requiredLength = GetCurrentDirectoryW(0, NULL);
    workingDirectory.SetLength(requiredLength);
    GetCurrentDirectoryW(workingDirectory.Length(),
                         (LPWSTR)workingDirectory.BeginWriting());
    // we got a trailing null there
    workingDirectory.SetLength(requiredLength);
    workingDirectory.Replace(workingDirectory.Length() - 1, 1, L'\\');
#elif defined(XP_UNIX)
    nsCAutoString cwd;
    // 1024 is just a guess at a sane starting value
    size_t bufsize = 1024;
    char* result = nsnull;
    while (result == nsnull) {
        if (!cwd.SetLength(bufsize))
            return false;
        result = getcwd(cwd.BeginWriting(), cwd.Length());
        if (!result) {
            if (errno != ERANGE)
                return false;
            // need to make the buffer bigger
            bufsize *= 2;
        }
    }
    // size back down to the actual string length
    cwd.SetLength(strlen(result) + 1);
    cwd.Replace(cwd.Length() - 1, 1, '/');
    workingDirectory = NS_ConvertUTF8toUTF16(cwd);
#endif
    return true;
}

#ifdef WINCE
#include "nsWindowsWMain.cpp"
#endif

int
#ifndef WINCE
main(int argc, char **argv, char **envp)
{
#else
main(int argc, char **argv)
{
	char **envp = 0;
#endif
#ifdef XP_MACOSX
    InitAutoreleasePool();
#endif
    JSRuntime *rt;
    JSContext *cx;
    JSObject *glob, *envobj;
    int result;
    nsresult rv;

#ifdef HAVE_SETBUF
    // unbuffer stdout so that output is in the correct order; note that stderr
    // is unbuffered by default
    setbuf(stdout, 0);
#endif

    gErrFile = stderr;
    gOutFile = stdout;
    gInFile = stdin;

    NS_LogInit();

    nsCOMPtr<nsILocalFile> appFile;
    rv = XRE_GetBinaryPath(argv[0], getter_AddRefs(appFile));
    if (NS_FAILED(rv)) {
        printf("Couldn't find application file.\n");
        return 1;
    }
    nsCOMPtr<nsIFile> appDir;
    rv = appFile->GetParent(getter_AddRefs(appDir));
    if (NS_FAILED(rv)) {
        printf("Couldn't get application directory.\n");
        return 1;
    }

    XPCShellDirProvider dirprovider;

    if (argc > 1 && !strcmp(argv[1], "-g")) {
        if (argc < 3)
            return usage();

        if (!dirprovider.SetGREDir(argv[2])) {
            printf("SetGREDir failed.\n");
            return 1;
        }
        argc -= 2;
        argv += 2;
    }

    {
        nsCOMPtr<nsIServiceManager> servMan;
        rv = NS_InitXPCOM2(getter_AddRefs(servMan), appDir, &dirprovider);
        if (NS_FAILED(rv)) {
            printf("NS_InitXPCOM2 failed!\n");
            return 1;
        }
        {
            nsCOMPtr<nsIComponentRegistrar> registrar = do_QueryInterface(servMan);
            NS_ASSERTION(registrar, "Null nsIComponentRegistrar");
            if (registrar)
                registrar->AutoRegister(nsnull);
        }

        nsCOMPtr<nsIJSRuntimeService> rtsvc = do_GetService("@mozilla.org/js/xpc/RuntimeService;1");
        // get the JSRuntime from the runtime svc
        if (!rtsvc) {
            printf("failed to get nsJSRuntimeService!\n");
            return 1;
        }

        if (NS_FAILED(rtsvc->GetRuntime(&rt)) || !rt) {
            printf("failed to get JSRuntime from nsJSRuntimeService!\n");
            return 1;
        }

        gOldJSContextCallback = JS_SetContextCallback(rt, ContextCallback);

        cx = JS_NewContext(rt, 8192);
        if (!cx) {
            printf("JS_NewContext failed!\n");
            return 1;
        }

        nsCOMPtr<nsIXPConnect> xpc = do_GetService(nsIXPConnect::GetCID());
        if (!xpc) {
            printf("failed to get nsXPConnect service!\n");
            return 1;
        }

        // Since the caps security system might set a default security manager
        // we will be sure that the secman on this context gives full trust.
        nsRefPtr<FullTrustSecMan> secman = new FullTrustSecMan();
        xpc->SetSecurityManagerForJSContext(cx, secman, 0xFFFF);

#ifndef XPCONNECT_STANDALONE
        // Fetch the system principal and store it away in a global, to use for
        // script compilation in Load() and ProcessFile() (including interactive
        // eval loop)
        {
            nsCOMPtr<nsIPrincipal> princ;

            nsCOMPtr<nsIScriptSecurityManager> securityManager =
                do_GetService(NS_SCRIPTSECURITYMANAGER_CONTRACTID, &rv);
            if (NS_SUCCEEDED(rv) && securityManager) {
                rv = securityManager->GetSystemPrincipal(getter_AddRefs(princ));
                if (NS_FAILED(rv)) {
                    fprintf(gErrFile, "+++ Failed to obtain SystemPrincipal from ScriptSecurityManager service.\n");
                } else {
                    // fetch the JS principals and stick in a global
                    rv = princ->GetJSPrincipals(cx, &gJSPrincipals);
                    if (NS_FAILED(rv)) {
                        fprintf(gErrFile, "+++ Failed to obtain JS principals from SystemPrincipal.\n");
                    }
                    secman->SetSystemPrincipal(princ);
                }
            } else {
                fprintf(gErrFile, "+++ Failed to get ScriptSecurityManager service, running without principals");
            }
        }
#endif

#ifdef TEST_TranslateThis
        nsCOMPtr<nsIXPCFunctionThisTranslator>
            translator(new nsXPCFunctionThisTranslator);
        xpc->SetFunctionThisTranslator(NS_GET_IID(nsITestXPCFunctionCallback), translator, nsnull);
#endif

        nsCOMPtr<nsIJSContextStack> cxstack = do_GetService("@mozilla.org/js/xpc/ContextStack;1");
        if (!cxstack) {
            printf("failed to get the nsThreadJSContextStack service!\n");
            return 1;
        }

        if(NS_FAILED(cxstack->Push(cx))) {
            printf("failed to push the current JSContext on the nsThreadJSContextStack!\n");
            return 1;
        }

        nsCOMPtr<nsIXPCScriptable> backstagePass;
        nsresult rv = rtsvc->GetBackstagePass(getter_AddRefs(backstagePass));
        if (NS_FAILED(rv)) {
            fprintf(gErrFile, "+++ Failed to get backstage pass from rtsvc: %8x\n",
                    rv);
            return 1;
        }

        nsCOMPtr<nsIXPConnectJSObjectHolder> holder;
        rv = xpc->InitClassesWithNewWrappedGlobal(cx, backstagePass,
                                                  NS_GET_IID(nsISupports),
                                                  nsIXPConnect::
                                                      FLAG_SYSTEM_GLOBAL_OBJECT,
                                                  getter_AddRefs(holder));
        if (NS_FAILED(rv))
            return 1;

        rv = holder->GetJSObject(&glob);
        if (NS_FAILED(rv)) {
            NS_ASSERTION(glob == nsnull, "bad GetJSObject?");
            return 1;
        }

        JS_BeginRequest(cx);

        if (!JS_DefineFunctions(cx, glob, glob_functions)) {
            JS_EndRequest(cx);
            return 1;
        }

        envobj = JS_DefineObject(cx, glob, "environment", &env_class, NULL, 0);
        if (!envobj || !JS_SetPrivate(cx, envobj, envp)) {
            JS_EndRequest(cx);
            return 1;
        }

        nsAutoString workingDirectory;
        if (GetCurrentWorkingDirectory(workingDirectory))
            gWorkingDirectory = &workingDirectory;

        JS_DefineProperty(cx, glob, "__LOCATION__", JSVAL_VOID,
                          GetLocationProperty, NULL, 0);

        argc--;
        argv++;

        result = ProcessArgs(cx, glob, argv, argc);


//#define TEST_CALL_ON_WRAPPED_JS_AFTER_SHUTDOWN 1

#ifdef TEST_CALL_ON_WRAPPED_JS_AFTER_SHUTDOWN
        // test of late call and release (see below)
        nsCOMPtr<nsIJSContextStack> bogus;
        xpc->WrapJS(cx, glob, NS_GET_IID(nsIJSContextStack),
                    (void**) getter_AddRefs(bogus));
#endif

        JSPRINCIPALS_DROP(cx, gJSPrincipals);
        JS_ClearScope(cx, glob);
        JS_GC(cx);
        JSContext *oldcx;
        cxstack->Pop(&oldcx);
        NS_ASSERTION(oldcx == cx, "JS thread context push/pop mismatch");
        cxstack = nsnull;
        JS_GC(cx);
        JS_DestroyContext(cx);
    } // this scopes the nsCOMPtrs

#ifdef MOZ_CRASHREPORTER
    // Get the crashreporter service while XPCOM is still active.
    // This is a special exception: it will remain usable after NS_ShutdownXPCOM().
    nsCOMPtr<nsICrashReporter> crashReporter =
        do_GetService("@mozilla.org/toolkit/crash-reporter;1");
#endif

    // no nsCOMPtrs are allowed to be alive when you call NS_ShutdownXPCOM
    rv = NS_ShutdownXPCOM( NULL );
    NS_ASSERTION(NS_SUCCEEDED(rv), "NS_ShutdownXPCOM failed");

#ifdef TEST_CALL_ON_WRAPPED_JS_AFTER_SHUTDOWN
    // test of late call and release (see above)
    JSContext* bogusCX;
    bogus->Peek(&bogusCX);
    bogus = nsnull;
#endif

    appDir = nsnull;
    appFile = nsnull;
    dirprovider.ClearGREDir();

#ifdef MOZ_CRASHREPORTER
    // Shut down the crashreporter service to prevent leaking some strings it holds.
    if (crashReporter) {
        crashReporter->SetEnabled(PR_FALSE);
        crashReporter = nsnull;
    }
#endif

    NS_LogTerm();

#ifdef XP_MACOSX
    FinishAutoreleasePool();
#endif

    return result;
}

PRBool
XPCShellDirProvider::SetGREDir(const char *dir)
{
    nsresult rv = XRE_GetFileFromPath(dir, getter_AddRefs(mGREDir));
    return NS_SUCCEEDED(rv);
}

NS_IMETHODIMP_(nsrefcnt)
XPCShellDirProvider::AddRef()
{
    return 2;
}

NS_IMETHODIMP_(nsrefcnt)
XPCShellDirProvider::Release()
{
    return 1;
}

NS_IMPL_QUERY_INTERFACE2(XPCShellDirProvider,
                         nsIDirectoryServiceProvider,
                         nsIDirectoryServiceProvider2)

NS_IMETHODIMP
XPCShellDirProvider::GetFile(const char *prop, PRBool *persistent,
                             nsIFile* *result)
{
    if (mGREDir && !strcmp(prop, NS_GRE_DIR)) {
        *persistent = PR_TRUE;
        NS_ADDREF(*result = mGREDir);
        return NS_OK;
    }

    return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
XPCShellDirProvider::GetFiles(const char *prop, nsISimpleEnumerator* *result)
{
    if (mGREDir && !strcmp(prop, "ChromeML")) {
        nsCOMArray<nsIFile> dirs;

        nsCOMPtr<nsIFile> file;
        mGREDir->Clone(getter_AddRefs(file));
        file->AppendNative(NS_LITERAL_CSTRING("chrome"));
        dirs.AppendObject(file);

        nsresult rv = NS_GetSpecialDirectory(NS_APP_CHROME_DIR,
                                             getter_AddRefs(file));
        if (NS_SUCCEEDED(rv))
            dirs.AppendObject(file);

        return NS_NewArrayEnumerator(result, dirs);
    }
    return NS_ERROR_FAILURE;
}
