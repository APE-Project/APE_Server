/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
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

/*
 * JS shell.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <locale.h>
#include "jstypes.h"
#include "jsarena.h"
#include "jsutil.h"
#include "jsprf.h"
#include "jsapi.h"
#include "jsarray.h"
#include "jsatom.h"
#include "jsbuiltins.h"
#include "jscntxt.h"
#include "jsdate.h"
#include "jsdbgapi.h"
#include "jsemit.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsiter.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsparse.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jstracer.h"

#include "prmjtime.h"

#ifdef JSDEBUGGER
#include "jsdebug.h"
#ifdef JSDEBUGGER_JAVA_UI
#include "jsdjava.h"
#endif /* JSDEBUGGER_JAVA_UI */
#ifdef JSDEBUGGER_C_UI
#include "jsdb.h"
#endif /* JSDEBUGGER_C_UI */
#endif /* JSDEBUGGER */

#ifdef XP_UNIX
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

#if defined(XP_WIN) || defined(XP_OS2)
#include <io.h>     /* for isatty() */
#endif

#ifdef XP_WIN
#include <windows.h>
#endif

typedef enum JSShellExitCode {
    EXITCODE_RUNTIME_ERROR      = 3,
    EXITCODE_FILE_NOT_FOUND     = 4,
    EXITCODE_OUT_OF_MEMORY      = 5,
    EXITCODE_TIMEOUT            = 6
} JSShellExitCode;

size_t gStackChunkSize = 8192;

/* Assume that we can not use more than 5e5 bytes of C stack by default. */
static size_t gMaxStackSize = 500000;

#ifdef JS_THREADSAFE
static PRUintn gStackBaseThreadIndex;
#else
static jsuword gStackBase;
#endif

static size_t gScriptStackQuota = JS_DEFAULT_SCRIPT_STACK_QUOTA;

/*
 * Limit the timeout to 30 minutes to prevent an overflow on platfoms
 * that represent the time internally in microseconds using 32-bit int.
 */
static jsdouble MAX_TIMEOUT_INTERVAL = 1800.0;
static jsdouble gTimeoutInterval = -1.0;
static volatile bool gCanceled = false;

static bool enableJit = false;

static JSBool
SetTimeoutValue(JSContext *cx, jsdouble t);

static bool
InitWatchdog(JSRuntime *rt);

static void
KillWatchdog();

static bool
ScheduleWatchdog(JSRuntime *rt, jsdouble t);

static void
CancelExecution(JSRuntime *rt);

/*
 * Watchdog thread state.
 */
#ifdef JS_THREADSAFE

static PRLock *gWatchdogLock = NULL;
static PRCondVar *gWatchdogWakeup = NULL;
static PRThread *gWatchdogThread = NULL;
static bool gWatchdogHasTimeout = false;
static PRIntervalTime gWatchdogTimeout = 0;

static PRCondVar *gSleepWakeup = NULL;

/*
 * Holding the gcLock already guarantees that the context list is locked when
 * the watchdog thread walks it.
 */

#define WITH_LOCKED_CONTEXT_LIST(x)             \
    JS_BEGIN_MACRO                              \
        x;                                      \
    JS_END_MACRO

#else

static JSRuntime *gRuntime = NULL;

/* 
 * Since signal handlers can't block, we must disable them before manipulating
 * the context list.
 */
#define WITH_LOCKED_CONTEXT_LIST(x)                                           \
    JS_BEGIN_MACRO                                                            \
        ScheduleWatchdog(gRuntime, -1);                                       \
        x;                                                                    \
        ScheduleWatchdog(gRuntime, gTimeoutInterval);                         \
    JS_END_MACRO
#endif

int gExitCode = 0;
JSBool gQuitting = JS_FALSE;
FILE *gErrFile = NULL;
FILE *gOutFile = NULL;

static JSBool reportWarnings = JS_TRUE;
static JSBool compileOnly = JS_FALSE;

typedef enum JSShellErrNum {
#define MSG_DEF(name, number, count, exception, format) \
    name = number,
#include "jsshell.msg"
#undef MSG_DEF
    JSShellErr_Limit
#undef MSGDEF
} JSShellErrNum;

static const JSErrorFormatString *
my_GetErrorMessage(void *userRef, const char *locale, const uintN errorNumber);
static JSObject *
split_setup(JSContext *cx);

#ifdef EDITLINE
JS_BEGIN_EXTERN_C
JS_EXTERN_API(char)    *readline(const char *prompt);
JS_EXTERN_API(void)     add_history(char *line);
JS_END_EXTERN_C
#endif

class ToString {
public:
    ToString(JSContext *aCx, jsval v, JSBool aThrow = JS_FALSE)
    : cx(aCx)
    , mThrow(aThrow)
    {
        mStr = JS_ValueToString(cx, v);
        if (!aThrow && !mStr && JS_IsExceptionPending(cx)) {
            if (!JS_ReportPendingException(cx))
                JS_ClearPendingException(cx);
        }
        JS_AddNamedRoot(cx, &mStr, "Value ToString helper");
    }
    ~ToString() {
        JS_RemoveRoot(cx, &mStr);
    }
    JSBool threw() { return !mStr; }
    jsval getJSVal() { return STRING_TO_JSVAL(mStr); }
    const char *getBytes() {
        return mStr ? JS_GetStringBytes(mStr) : "(error converting value)";
    }
private:
    JSContext *cx;
    JSString *mStr;
    JSBool mThrow;
};

static char *
GetLine(FILE *file, const char * prompt)
{
    size_t size;
    char *buffer;
#ifdef EDITLINE
    /*
     * Use readline only if file is stdin, because there's no way to specify
     * another handle.  Are other filehandles interactive?
     */
    if (file == stdin) {
        char *linep = readline(prompt);
        /*
         * We set it to zero to avoid complaining about inappropriate ioctl
         * for device in the case of EOF. Looks like errno == 251 if line is
         * finished with EOF and errno == 25 (EINVAL on Mac) if there is
         * nothing left to read.
         */
        if (errno == 251 || errno == 25 || errno == EINVAL)
            errno = 0;
        if (!linep)
            return NULL;
        if (linep[0] != '\0')
            add_history(linep);
        return linep;
    }
#endif
    size_t len = 0;
    if (*prompt != '\0') {
        fprintf(gOutFile, "%s", prompt);
        fflush(gOutFile);
    }
    size = 80;
    buffer = (char *) malloc(size);
    if (!buffer)
        return NULL;
    char *current = buffer;
    while (fgets(current, size - len, file)) {
        len += strlen(current);
        char *t = buffer + len - 1;
        if (*t == '\n') {
            /* Line was read. We remove '\n' and exit. */
            *t = '\0';
            return buffer;
        }
        if (len + 1 == size) {
            size = size * 2;
            char *tmp = (char *) realloc(buffer, size);
            if (!tmp) {
                free(buffer);
                return NULL;
            }
            buffer = tmp;
        }
        current = buffer + len;
    }
    if (len && !ferror(file))
        return buffer;
    free(buffer);
    return NULL;
}

/*
 * State to store as JSContext private.
 *
 * We declare such timestamp as volatile as they are updated in the operation
 * callback without taking any locks. Any possible race can only lead to more
 * frequent callback calls. This is safe as the callback does everything based
 * on timing.
 */
struct JSShellContextData {
    volatile JSIntervalTime startTime;
};

static JSShellContextData *
NewContextData()
{
    /* Prevent creation of new contexts after we have been canceled. */
    if (gCanceled)
        return NULL;

    JSShellContextData *data = (JSShellContextData *)
                               calloc(sizeof(JSShellContextData), 1);
    if (!data)
        return NULL;
    data->startTime = js_IntervalNow();
    return data;
}

static inline JSShellContextData *
GetContextData(JSContext *cx)
{
    JSShellContextData *data = (JSShellContextData *) JS_GetContextPrivate(cx);

    JS_ASSERT(data);
    return data;
}

static JSBool
ShellOperationCallback(JSContext *cx)
{
    if (!gCanceled)
        return JS_TRUE;

    JS_ClearPendingException(cx);
    return JS_FALSE;
}

static void
SetThreadStackLimit(JSContext *cx)
{
    jsuword stackLimit;

    if (gMaxStackSize == 0) {
        /*
         * Disable checking for stack overflow if limit is zero.
         */
        stackLimit = 0;
    } else {
        jsuword stackBase;
#ifdef JS_THREADSAFE
        stackBase = (jsuword) PR_GetThreadPrivate(gStackBaseThreadIndex);
#else
        stackBase = gStackBase;
#endif
        JS_ASSERT(stackBase != 0);
#if JS_STACK_GROWTH_DIRECTION > 0
        stackLimit = stackBase + gMaxStackSize;
#else
        stackLimit = stackBase - gMaxStackSize;
#endif
    }
    JS_SetThreadStackLimit(cx, stackLimit);

}

static void
SetContextOptions(JSContext *cx)
{
    SetThreadStackLimit(cx);
    JS_SetScriptStackQuota(cx, gScriptStackQuota);
    JS_SetOperationCallback(cx, ShellOperationCallback);
}

static void
Process(JSContext *cx, JSObject *obj, char *filename, JSBool forceTTY)
{
    JSBool ok, hitEOF;
    JSScript *script;
    jsval result;
    JSString *str;
    char *buffer;
    size_t size;
    int lineno;
    int startline;
    FILE *file;
    uint32 oldopts;

    if (forceTTY || !filename || strcmp(filename, "-") == 0) {
        file = stdin;
    } else {
        file = fopen(filename, "r");
        if (!file) {
            JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL,
                                 JSSMSG_CANT_OPEN, filename, strerror(errno));
            gExitCode = EXITCODE_FILE_NOT_FOUND;
            return;
        }
    }

    SetContextOptions(cx);

#ifndef WINCE
    /* windows mobile (and possibly other os's) does not have a TTY */
    if (!forceTTY && !isatty(fileno(file))) {
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
                if (ch == '\n' || ch == '\r')
                    break;
            }
        }
        ungetc(ch, file);

        oldopts = JS_GetOptions(cx);
        JS_SetOptions(cx, oldopts | JSOPTION_COMPILE_N_GO | JSOPTION_NO_SCRIPT_RVAL);
        script = JS_CompileFileHandle(cx, obj, filename, file);
        JS_SetOptions(cx, oldopts);
        if (script) {
            if (!compileOnly)
                (void)JS_ExecuteScript(cx, obj, script, NULL);
            JS_DestroyScript(cx, script);
        }

        if (file != stdin)
            fclose(file);
        return;
    }
#endif /* WINCE */

    /* It's an interactive filehandle; drop into read-eval-print loop. */
    lineno = 1;
    hitEOF = JS_FALSE;
    buffer = NULL;
    size = 0;           /* assign here to avoid warnings */
    do {
        /*
         * Accumulate lines until we get a 'compilable unit' - one that either
         * generates an error (before running out of source) or that compiles
         * cleanly.  This should be whenever we get a complete statement that
         * coincides with the end of a line.
         */
        startline = lineno;
        size_t len = 0; /* initialize to avoid warnings */
        do {
            ScheduleWatchdog(cx->runtime, -1);
            jsrefcount rc = JS_SuspendRequest(cx);
            gCanceled = false;
            errno = 0;
            char *line = GetLine(file, startline == lineno ? "js> " : "");
            if (!line) {
                JS_ResumeRequest(cx, rc);
                if (errno) {
                    JS_ReportError(cx, strerror(errno));
                    free(buffer);
                    return;
                }
                hitEOF = JS_TRUE;
                break;
            }
            if (!buffer) {
                buffer = line;
                len = strlen(buffer);
                size = len + 1;
            } else {
                /*
                 * len + 1 is required to store '\n' in the end of line.
                 */
                size_t newlen = strlen(line) + (len ? len + 1 : 0);
                if (newlen + 1 > size) {
                    size = newlen + 1 > size * 2 ? newlen + 1 : size * 2;
                    char *newBuf = (char *) realloc(buffer, size);
                    if (!newBuf) {
                        free(buffer);
                        free(line);
                        JS_ResumeRequest(cx, rc);
                        JS_ReportOutOfMemory(cx);
                        return;
                    }
                    buffer = newBuf;
                }
                char *current = buffer + len;
                if (startline != lineno)
                    *current++ = '\n';
                strcpy(current, line);
                len = newlen;
                free(line);
            }
            lineno++;
            JS_ResumeRequest(cx, rc);
            if (!ScheduleWatchdog(cx->runtime, gTimeoutInterval)) {
                hitEOF = JS_TRUE;
                break;
            }
        } while (!JS_BufferIsCompilableUnit(cx, obj, buffer, len));

        if (hitEOF && !buffer)
            break;

        /* Clear any pending exception from previous failed compiles. */
        JS_ClearPendingException(cx);

        /* Even though we're interactive, we have a compile-n-go opportunity. */
        oldopts = JS_GetOptions(cx);
        if (!compileOnly)
            JS_SetOptions(cx, oldopts | JSOPTION_COMPILE_N_GO);
        script = JS_CompileScript(cx, obj, buffer, len, "typein",
                                  startline);
        if (!compileOnly)
            JS_SetOptions(cx, oldopts);

        if (script) {
            if (!compileOnly) {
                ok = JS_ExecuteScript(cx, obj, script, &result);
                if (ok && !JSVAL_IS_VOID(result)) {
                    str = JS_ValueToSource(cx, result);
                    if (str)
                        fprintf(gOutFile, "%s\n", JS_GetStringBytes(str));
                    else
                        ok = JS_FALSE;
                }
            }
            JS_DestroyScript(cx, script);
        }
        *buffer = '\0';
    } while (!hitEOF && !gQuitting);

    free(buffer);
    fprintf(gOutFile, "\n");
    if (file != stdin)
        fclose(file);
    return;
}

static int
usage(void)
{
    fprintf(gErrFile, "%s\n", JS_GetImplementationVersion());
    fprintf(gErrFile, "usage: js [-zKPswWxCij] [-t timeoutSeconds] [-c stackchunksize] [-o option] [-v version] [-f scriptfile] [-e script] [-S maxstacksize] "
#ifdef JS_GC_ZEAL
"[-Z gczeal] "
#endif
#ifdef MOZ_TRACEVIS
"[-T TraceVisFileName] "
#endif
"[scriptfile] [scriptarg...]\n");
    return 2;
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

extern JSClass global_class;

static int
ProcessArgs(JSContext *cx, JSObject *obj, char **argv, int argc)
{
    int i, j, length;
    JSObject *argsObj;
    char *filename = NULL;
    JSBool isInteractive = JS_TRUE;
    JSBool forceTTY = JS_FALSE;

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
          case 'c':
          case 'f':
          case 'e':
          case 'v':
          case 'S':
          case 't':
#ifdef JS_GC_ZEAL
          case 'Z':
#endif
#ifdef MOZ_TRACEVIS
          case 'T':
#endif
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
            if (++i == argc)
                return usage();

            JS_SetVersion(cx, (JSVersion) atoi(argv[i]));
            break;

#ifdef JS_GC_ZEAL
        case 'Z':
            if (++i == argc)
                return usage();
            JS_SetGCZeal(cx, atoi(argv[i]));
            break;
#endif

        case 'w':
            reportWarnings = JS_TRUE;
            break;

        case 'W':
            reportWarnings = JS_FALSE;
            break;

        case 's':
            JS_ToggleOptions(cx, JSOPTION_STRICT);
            break;

        case 'E':
            JS_ToggleOptions(cx, JSOPTION_RELIMIT);
            break;

        case 'x':
            JS_ToggleOptions(cx, JSOPTION_XML);
            break;

        case 'j':
            enableJit = !enableJit;
            JS_ToggleOptions(cx, JSOPTION_JIT);
#if defined(JS_TRACER) && defined(DEBUG)
extern struct JSClass jitstats_class;
extern void js_InitJITStatsClass(JSContext *cx, JSObject *glob);
            js_InitJITStatsClass(cx, JS_GetGlobalObject(cx));
            JS_DefineObject(cx, JS_GetGlobalObject(cx), "tracemonkey",
                            &jitstats_class, NULL, 0);
#endif
            break;

        case 'o':
          {
            if (++i == argc)
                return usage();

            uint32 flag = MapContextOptionNameToFlag(cx, argv[i]);
            if (flag == 0)
                return gExitCode;
            JS_ToggleOptions(cx, flag);
            break;
          }
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

        case 't':
            if (++i == argc)
                return usage();

            if (!SetTimeoutValue(cx, atof(argv[i])))
                return JS_FALSE;

            break;

        case 'c':
            /* set stack chunk size */
            gStackChunkSize = atoi(argv[++i]);
            break;

        case 'f':
            if (++i == argc)
                return usage();

            Process(cx, obj, argv[i], JS_FALSE);
            if (gExitCode != 0)
                return gExitCode;

            /*
             * XXX: js -f foo.js should interpret foo.js and then
             * drop into interactive mode, but that breaks the test
             * harness. Just execute foo.js for now.
             */
            isInteractive = JS_FALSE;
            break;

        case 'e':
        {
            jsval rval;

            if (++i == argc)
                return usage();

            /* Pass a filename of -e to imitate PERL */
            JS_EvaluateScript(cx, obj, argv[i], strlen(argv[i]),
                              "-e", 1, &rval);

            isInteractive = JS_FALSE;
            break;

        }
        case 'C':
            compileOnly = JS_TRUE;
            isInteractive = JS_FALSE;
            break;

        case 'i':
            isInteractive = forceTTY = JS_TRUE;
            break;

        case 'S':
            if (++i == argc)
                return usage();

            /* Set maximum stack size. */
            gMaxStackSize = atoi(argv[i]);
            break;

        case 'z':
            obj = split_setup(cx);
            if (!obj)
                return gExitCode;
            break;
#ifdef MOZ_SHARK
        case 'k':
            JS_ConnectShark();
            break;
#endif
#ifdef MOZ_TRACEVIS
        case 'T':
            if (++i == argc)
                return usage();

            JS_StartTraceVis(argv[i]);
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

static JSBool
Version(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    if (argc > 0 && JSVAL_IS_INT(argv[0]))
        *rval = INT_TO_JSVAL(JS_SetVersion(cx, (JSVersion) JSVAL_TO_INT(argv[0])));
    else
        *rval = INT_TO_JSVAL(JS_GetVersion(cx));
    return JS_TRUE;
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

static JSBool
Load(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    uintN i;
    JSString *str;
    const char *filename;
    JSScript *script;
    JSBool ok;
    uint32 oldopts;

    for (i = 0; i < argc; i++) {
        str = JS_ValueToString(cx, argv[i]);
        if (!str)
            return JS_FALSE;
        argv[i] = STRING_TO_JSVAL(str);
        filename = JS_GetStringBytes(str);
        errno = 0;
        oldopts = JS_GetOptions(cx);
        JS_SetOptions(cx, oldopts | JSOPTION_COMPILE_N_GO | JSOPTION_NO_SCRIPT_RVAL);
        script = JS_CompileFile(cx, obj, filename);
        JS_SetOptions(cx, oldopts);
        if (!script) {
            ok = JS_FALSE;
        } else {
            ok = !compileOnly
                 ? JS_ExecuteScript(cx, obj, script, NULL)
                 : JS_TRUE;
            JS_DestroyScript(cx, script);
        }
        if (!ok)
            return JS_FALSE;
    }

    return JS_TRUE;
}

/*
 * function readline()
 * Provides a hook for scripts to read a line from stdin.
 */
static JSBool
ReadLine(JSContext *cx, uintN argc, jsval *vp)
{
#define BUFSIZE 256
    FILE *from;
    char *buf, *tmp;
    size_t bufsize, buflength, gotlength;
    JSBool sawNewline;
    JSString *str;

    from = stdin;
    buflength = 0;
    bufsize = BUFSIZE;
    buf = (char *) JS_malloc(cx, bufsize);
    if (!buf)
        return JS_FALSE;

    sawNewline = JS_FALSE;
    while ((gotlength =
            js_fgets(buf + buflength, bufsize - buflength, from)) > 0) {
        buflength += gotlength;

        /* Are we done? */
        if (buf[buflength - 1] == '\n') {
            buf[buflength - 1] = '\0';
            sawNewline = JS_TRUE;
            break;
        } else if (buflength < bufsize - 1) {
            break;
        }

        /* Else, grow our buffer for another pass. */
        bufsize *= 2;
        if (bufsize > buflength) {
            tmp = (char *) JS_realloc(cx, buf, bufsize);
        } else {
            JS_ReportOutOfMemory(cx);
            tmp = NULL;
        }

        if (!tmp) {
            JS_free(cx, buf);
            return JS_FALSE;
        }

        buf = tmp;
    }

    /* Treat the empty string specially. */
    if (buflength == 0) {
        *vp = feof(from) ? JSVAL_NULL : JS_GetEmptyStringValue(cx);
        JS_free(cx, buf);
        return JS_TRUE;
    }

    /* Shrink the buffer to the real size. */
    tmp = (char *) JS_realloc(cx, buf, buflength);
    if (!tmp) {
        JS_free(cx, buf);
        return JS_FALSE;
    }

    buf = tmp;

    /*
     * Turn buf into a JSString. Note that buflength includes the trailing null
     * character.
     */
    str = JS_NewString(cx, buf, sawNewline ? buflength - 1 : buflength);
    if (!str) {
        JS_free(cx, buf);
        return JS_FALSE;
    }

    *vp = STRING_TO_JSVAL(str);
    return JS_TRUE;
}

static JSBool
Print(JSContext *cx, uintN argc, jsval *vp)
{
    jsval *argv;
    uintN i;
    JSString *str;
    char *bytes;

    argv = JS_ARGV(cx, vp);
    for (i = 0; i < argc; i++) {
        str = JS_ValueToString(cx, argv[i]);
        if (!str)
            return JS_FALSE;
        bytes = JS_EncodeString(cx, str);
        if (!bytes)
            return JS_FALSE;
        fprintf(gOutFile, "%s%s", i ? " " : "", bytes);
        JS_free(cx, bytes);
    }

    fputc('\n', gOutFile);
    fflush(gOutFile);

    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return JS_TRUE;
}

static JSBool
Help(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval);

static JSBool
Quit(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JS_ConvertArguments(cx, argc, argv,"/ i", &gExitCode);

    gQuitting = JS_TRUE;
    return JS_FALSE;
}

static const char *
ToSource(JSContext *cx, jsval *vp)
{
    JSString *str = JS_ValueToSource(cx, *vp);
    if (str) {
        *vp = STRING_TO_JSVAL(str);
        return JS_GetStringBytes(str);
    }
    JS_ClearPendingException(cx);
    return "<<error converting value to string>>";
}

static JSBool
AssertEq(JSContext *cx, uintN argc, jsval *vp)
{
    if (!(argc == 2 || (argc == 3 && JSVAL_IS_STRING(JS_ARGV(cx, vp)[2])))) {
        JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL,
                             (argc < 2)
                             ? JSSMSG_NOT_ENOUGH_ARGS
                             : (argc == 3)
                             ? JSSMSG_INVALID_ARGS
                             : JSSMSG_TOO_MANY_ARGS,
                             "assertEq");
        return JS_FALSE;
    }

    jsval *argv = JS_ARGV(cx, vp);
    if (!JS_SameValue(cx, argv[0], argv[1])) {
        const char *actual = ToSource(cx, &argv[0]);
        const char *expected = ToSource(cx, &argv[1]);
        if (argc == 2) {
            JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL, JSSMSG_ASSERT_EQ_FAILED,
                                 actual, expected);
        } else {
            JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL, JSSMSG_ASSERT_EQ_FAILED_MSG,
                                 actual, expected, JS_GetStringBytes(JSVAL_TO_STRING(argv[2])));
        }
        return JS_FALSE;
    }
    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return JS_TRUE;
}

static JSBool
GC(JSContext *cx, uintN argc, jsval *vp)
{
    JSRuntime *rt;
    uint32 preBytes;

    rt = cx->runtime;
    preBytes = rt->gcBytes;
    JS_GC(cx);

    char buf[256];
    JS_snprintf(buf, sizeof(buf), "before %lu, after %lu, break %08lx\n",
                (unsigned long)preBytes, (unsigned long)rt->gcBytes,
#ifdef HAVE_SBRK
                (unsigned long)sbrk(0)
#else
                0
#endif
                );
#ifdef JS_GCMETER
    js_DumpGCStats(rt, stdout);
#endif
    *vp = STRING_TO_JSVAL(JS_NewStringCopyZ(cx, buf));
    return JS_TRUE;
}

static JSBool
GCParameter(JSContext *cx, uintN argc, jsval *vp)
{
    JSString *str;
    const char *paramName;
    JSGCParamKey param;
    uint32 value;

    if (argc == 0) {
        str = JS_ValueToString(cx, JSVAL_VOID);
        JS_ASSERT(str);
    } else {
        str = JS_ValueToString(cx, vp[2]);
        if (!str)
            return JS_FALSE;
        vp[2] = STRING_TO_JSVAL(str);
    }
    paramName = JS_GetStringBytes(str);
    if (!paramName)
        return JS_FALSE;
    if (strcmp(paramName, "maxBytes") == 0) {
        param = JSGC_MAX_BYTES;
    } else if (strcmp(paramName, "maxMallocBytes") == 0) {
        param = JSGC_MAX_MALLOC_BYTES;
    } else if (strcmp(paramName, "gcStackpoolLifespan") == 0) {
        param = JSGC_STACKPOOL_LIFESPAN;
    } else if (strcmp(paramName, "gcBytes") == 0) {
        param = JSGC_BYTES;
    } else if (strcmp(paramName, "gcNumber") == 0) {
        param = JSGC_NUMBER;
    } else if (strcmp(paramName, "gcTriggerFactor") == 0) {
        param = JSGC_TRIGGER_FACTOR;
    } else {
        JS_ReportError(cx,
                       "the first argument argument must be maxBytes, "
                       "maxMallocBytes, gcStackpoolLifespan, gcBytes, "
                       "gcNumber or gcTriggerFactor");
        return JS_FALSE;
    }

    if (argc == 1) {
        value = JS_GetGCParameter(cx->runtime, param);
        return JS_NewNumberValue(cx, value, &vp[0]);
    }

    if (param == JSGC_NUMBER ||
        param == JSGC_BYTES) {
        JS_ReportError(cx, "Attempt to change read-only parameter %s",
                       paramName);
        return JS_FALSE;
    }

    if (!JS_ValueToECMAUint32(cx, vp[3], &value)) {
        JS_ReportError(cx,
                       "the second argument must be convertable to uint32 "
                       "with non-zero value");
        return JS_FALSE;
    }
    if (param == JSGC_TRIGGER_FACTOR && value < 100) {
        JS_ReportError(cx,
                       "the gcTriggerFactor value must be >= 100");
        return JS_FALSE;
    }
    JS_SetGCParameter(cx->runtime, param, value);
    *vp = JSVAL_VOID;
    return JS_TRUE;
}

#ifdef JS_GC_ZEAL
static JSBool
GCZeal(JSContext *cx, uintN argc, jsval *vp)
{
    uint32 zeal;

    if (!JS_ValueToECMAUint32(cx, argc == 0 ? JSVAL_VOID : vp[2], &zeal))
        return JS_FALSE;
    JS_SetGCZeal(cx, (uint8)zeal);
    *vp = JSVAL_VOID;
    return JS_TRUE;
}
#endif /* JS_GC_ZEAL */

typedef struct JSCountHeapNode JSCountHeapNode;

struct JSCountHeapNode {
    void                *thing;
    int32               kind;
    JSCountHeapNode     *next;
};

typedef struct JSCountHeapTracer {
    JSTracer            base;
    JSDHashTable        visited;
    JSBool              ok;
    JSCountHeapNode     *traceList;
    JSCountHeapNode     *recycleList;
} JSCountHeapTracer;

static void
CountHeapNotify(JSTracer *trc, void *thing, uint32 kind)
{
    JSCountHeapTracer *countTracer;
    JSDHashEntryStub *entry;
    JSCountHeapNode *node;

    JS_ASSERT(trc->callback == CountHeapNotify);
    countTracer = (JSCountHeapTracer *)trc;
    if (!countTracer->ok)
        return;

    entry = (JSDHashEntryStub *)
            JS_DHashTableOperate(&countTracer->visited, thing, JS_DHASH_ADD);
    if (!entry) {
        JS_ReportOutOfMemory(trc->context);
        countTracer->ok = JS_FALSE;
        return;
    }
    if (entry->key)
        return;
    entry->key = thing;

    node = countTracer->recycleList;
    if (node) {
        countTracer->recycleList = node->next;
    } else {
        node = (JSCountHeapNode *) JS_malloc(trc->context, sizeof *node);
        if (!node) {
            countTracer->ok = JS_FALSE;
            return;
        }
    }
    node->thing = thing;
    node->kind = kind;
    node->next = countTracer->traceList;
    countTracer->traceList = node;
}

static JSBool
CountHeap(JSContext *cx, uintN argc, jsval *vp)
{
    void* startThing;
    int32 startTraceKind;
    jsval v;
    int32 traceKind, i;
    JSString *str;
    char *bytes;
    JSCountHeapTracer countTracer;
    JSCountHeapNode *node;
    size_t counter;

    static const struct {
        const char       *name;
        int32             kind;
    } traceKindNames[] = {
        { "all",        -1                  },
        { "object",     JSTRACE_OBJECT      },
        { "double",     JSTRACE_DOUBLE      },
        { "string",     JSTRACE_STRING      },
#if JS_HAS_XML_SUPPORT
        { "xml",        JSTRACE_XML         },
#endif
    };

    startThing = NULL;
    startTraceKind = 0;
    if (argc > 0) {
        v = JS_ARGV(cx, vp)[0];
        if (JSVAL_IS_TRACEABLE(v)) {
            startThing = JSVAL_TO_TRACEABLE(v);
            startTraceKind = JSVAL_TRACE_KIND(v);
        } else if (v != JSVAL_NULL) {
            JS_ReportError(cx,
                           "the first argument is not null or a heap-allocated "
                           "thing");
            return JS_FALSE;
        }
    }

    traceKind = -1;
    if (argc > 1) {
        str = JS_ValueToString(cx, JS_ARGV(cx, vp)[1]);
        if (!str)
            return JS_FALSE;
        bytes = JS_GetStringBytes(str);
        if (!bytes)
            return JS_FALSE;
        for (i = 0; ;) {
            if (strcmp(bytes, traceKindNames[i].name) == 0) {
                traceKind = traceKindNames[i].kind;
                break;
            }
            if (++i == JS_ARRAY_LENGTH(traceKindNames)) {
                JS_ReportError(cx, "trace kind name '%s' is unknown", bytes);
                return JS_FALSE;
            }
        }
    }

    JS_TRACER_INIT(&countTracer.base, cx, CountHeapNotify);
    if (!JS_DHashTableInit(&countTracer.visited, JS_DHashGetStubOps(),
                           NULL, sizeof(JSDHashEntryStub),
                           JS_DHASH_DEFAULT_CAPACITY(100))) {
        JS_ReportOutOfMemory(cx);
        return JS_FALSE;
    }
    countTracer.ok = JS_TRUE;
    countTracer.traceList = NULL;
    countTracer.recycleList = NULL;

    if (!startThing) {
        JS_TraceRuntime(&countTracer.base);
    } else {
        JS_SET_TRACING_NAME(&countTracer.base, "root");
        JS_CallTracer(&countTracer.base, startThing, startTraceKind);
    }

    counter = 0;
    while ((node = countTracer.traceList) != NULL) {
        if (traceKind == -1 || node->kind == traceKind)
            counter++;
        countTracer.traceList = node->next;
        node->next = countTracer.recycleList;
        countTracer.recycleList = node;
        JS_TraceChildren(&countTracer.base, node->thing, node->kind);
    }
    while ((node = countTracer.recycleList) != NULL) {
        countTracer.recycleList = node->next;
        JS_free(cx, node);
    }
    JS_DHashTableFinish(&countTracer.visited);

    return countTracer.ok && JS_NewNumberValue(cx, (jsdouble) counter, vp);
}

static JSScript *
ValueToScript(JSContext *cx, jsval v)
{
    JSScript *script = NULL;
    JSFunction *fun;

    if (!JSVAL_IS_PRIMITIVE(v)) {
        JSObject *obj = JSVAL_TO_OBJECT(v);
        JSClass *clasp = JS_GET_CLASS(cx, obj);

        if (clasp == &js_ScriptClass) {
            script = (JSScript *) JS_GetPrivate(cx, obj);
        } else if (clasp == &js_GeneratorClass) {
            JSGenerator *gen = (JSGenerator *) JS_GetPrivate(cx, obj);
            fun = gen->frame.fun;
            script = FUN_SCRIPT(fun);
        }
    }

    if (!script) {
        fun = JS_ValueToFunction(cx, v);
        if (!fun)
            return NULL;
        script = FUN_SCRIPT(fun);
        if (!script) {
            JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL,
                                 JSSMSG_SCRIPTS_ONLY);
        }
    }

    return script;
}

static JSBool
GetTrapArgs(JSContext *cx, uintN argc, jsval *argv, JSScript **scriptp,
            int32 *ip)
{
    jsval v;
    uintN intarg;
    JSScript *script;

    *scriptp = JS_GetScriptedCaller(cx, NULL)->script;
    *ip = 0;
    if (argc != 0) {
        v = argv[0];
        intarg = 0;
        if (!JSVAL_IS_PRIMITIVE(v) &&
            (JS_GET_CLASS(cx, JSVAL_TO_OBJECT(v)) == &js_FunctionClass ||
             JS_GET_CLASS(cx, JSVAL_TO_OBJECT(v)) == &js_ScriptClass)) {
            script = ValueToScript(cx, v);
            if (!script)
                return JS_FALSE;
            *scriptp = script;
            intarg++;
        }
        if (argc > intarg) {
            if (!JS_ValueToInt32(cx, argv[intarg], ip))
                return JS_FALSE;
        }
    }
    return JS_TRUE;
}

static JSTrapStatus
TrapHandler(JSContext *cx, JSScript *script, jsbytecode *pc, jsval *rval,
            void *closure)
{
    JSString *str;
    JSStackFrame *caller;

    str = (JSString *) closure;
    caller = JS_GetScriptedCaller(cx, NULL);
    if (!JS_EvaluateScript(cx, caller->scopeChain,
                           JS_GetStringBytes(str), JS_GetStringLength(str),
                           caller->script->filename, caller->script->lineno,
                           rval)) {
        return JSTRAP_ERROR;
    }
    if (!JSVAL_IS_VOID(*rval))
        return JSTRAP_RETURN;
    return JSTRAP_CONTINUE;
}

static JSBool
Trap(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSString *str;
    JSScript *script;
    int32 i;

    if (argc == 0) {
        JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL, JSSMSG_TRAP_USAGE);
        return JS_FALSE;
    }
    argc--;
    str = JS_ValueToString(cx, argv[argc]);
    if (!str)
        return JS_FALSE;
    argv[argc] = STRING_TO_JSVAL(str);
    if (!GetTrapArgs(cx, argc, argv, &script, &i))
        return JS_FALSE;
    return JS_SetTrap(cx, script, script->code + i, TrapHandler, str);
}

static JSBool
Untrap(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSScript *script;
    int32 i;

    if (!GetTrapArgs(cx, argc, argv, &script, &i))
        return JS_FALSE;
    JS_ClearTrap(cx, script, script->code + i, NULL, NULL);
    return JS_TRUE;
}

static JSBool
LineToPC(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSScript *script;
    int32 i;
    uintN lineno;
    jsbytecode *pc;

    if (argc == 0) {
        JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL, JSSMSG_LINE2PC_USAGE);
        return JS_FALSE;
    }
    script = JS_GetScriptedCaller(cx, NULL)->script;
    if (!GetTrapArgs(cx, argc, argv, &script, &i))
        return JS_FALSE;
    lineno = (i == 0) ? script->lineno : (uintN)i;
    pc = JS_LineNumberToPC(cx, script, lineno);
    if (!pc)
        return JS_FALSE;
    *rval = INT_TO_JSVAL(pc - script->code);
    return JS_TRUE;
}

static JSBool
PCToLine(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSScript *script;
    int32 i;
    uintN lineno;

    if (!GetTrapArgs(cx, argc, argv, &script, &i))
        return JS_FALSE;
    lineno = JS_PCToLineNumber(cx, script, script->code + i);
    if (!lineno)
        return JS_FALSE;
    *rval = INT_TO_JSVAL(lineno);
    return JS_TRUE;
}

#ifdef DEBUG

static void
UpdateSwitchTableBounds(JSContext *cx, JSScript *script, uintN offset,
                        uintN *start, uintN *end)
{
    jsbytecode *pc;
    JSOp op;
    ptrdiff_t jmplen;
    jsint low, high, n;

    pc = script->code + offset;
    op = js_GetOpcode(cx, script, pc);
    switch (op) {
      case JSOP_TABLESWITCHX:
        jmplen = JUMPX_OFFSET_LEN;
        goto jump_table;
      case JSOP_TABLESWITCH:
        jmplen = JUMP_OFFSET_LEN;
      jump_table:
        pc += jmplen;
        low = GET_JUMP_OFFSET(pc);
        pc += JUMP_OFFSET_LEN;
        high = GET_JUMP_OFFSET(pc);
        pc += JUMP_OFFSET_LEN;
        n = high - low + 1;
        break;

      case JSOP_LOOKUPSWITCHX:
        jmplen = JUMPX_OFFSET_LEN;
        goto lookup_table;
      case JSOP_LOOKUPSWITCH:
        jmplen = JUMP_OFFSET_LEN;
      lookup_table:
        pc += jmplen;
        n = GET_INDEX(pc);
        pc += INDEX_LEN;
        jmplen += JUMP_OFFSET_LEN;
        break;

      default:
        /* [condswitch] switch does not have any jump or lookup tables. */
        JS_ASSERT(op == JSOP_CONDSWITCH);
        return;
    }

    *start = (uintN)(pc - script->code);
    *end = *start + (uintN)(n * jmplen);
}

static void
SrcNotes(JSContext *cx, JSScript *script)
{
    uintN offset, delta, caseOff, switchTableStart, switchTableEnd;
    jssrcnote *notes, *sn;
    JSSrcNoteType type;
    const char *name;
    uint32 index;
    JSAtom *atom;
    JSString *str;

    fprintf(gOutFile, "\nSource notes:\n");
    offset = 0;
    notes = script->notes();
    switchTableEnd = switchTableStart = 0;
    for (sn = notes; !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn)) {
        delta = SN_DELTA(sn);
        offset += delta;
        type = (JSSrcNoteType) SN_TYPE(sn);
        name = js_SrcNoteSpec[type].name;
        if (type == SRC_LABEL) {
            /* Check if the source note is for a switch case. */
            if (switchTableStart <= offset && offset < switchTableEnd) {
                name = "case";
            } else {
                JS_ASSERT(js_GetOpcode(cx, script, script->code + offset) == JSOP_NOP);
            }
        }
        fprintf(gOutFile, "%3u: %5u [%4u] %-8s",
                (uintN) (sn - notes), offset, delta, name);
        switch (type) {
          case SRC_SETLINE:
            fprintf(gOutFile, " lineno %u", (uintN) js_GetSrcNoteOffset(sn, 0));
            break;
          case SRC_FOR:
            fprintf(gOutFile, " cond %u update %u tail %u",
                   (uintN) js_GetSrcNoteOffset(sn, 0),
                   (uintN) js_GetSrcNoteOffset(sn, 1),
                   (uintN) js_GetSrcNoteOffset(sn, 2));
            break;
          case SRC_IF_ELSE:
            fprintf(gOutFile, " else %u elseif %u",
                   (uintN) js_GetSrcNoteOffset(sn, 0),
                   (uintN) js_GetSrcNoteOffset(sn, 1));
            break;
          case SRC_COND:
          case SRC_WHILE:
          case SRC_PCBASE:
          case SRC_PCDELTA:
          case SRC_DECL:
          case SRC_BRACE:
            fprintf(gOutFile, " offset %u", (uintN) js_GetSrcNoteOffset(sn, 0));
            break;
          case SRC_LABEL:
          case SRC_LABELBRACE:
          case SRC_BREAK2LABEL:
          case SRC_CONT2LABEL:
            index = js_GetSrcNoteOffset(sn, 0);
            JS_GET_SCRIPT_ATOM(script, NULL, index, atom);
            JS_ASSERT(ATOM_IS_STRING(atom));
            str = ATOM_TO_STRING(atom);
            fprintf(gOutFile, " atom %u (", index);
            js_FileEscapedString(gOutFile, str, 0);
            putc(')', gOutFile);
            break;
          case SRC_FUNCDEF: {
            const char *bytes;
            JSObject *obj;
            JSFunction *fun;

            index = js_GetSrcNoteOffset(sn, 0);
            obj = script->getObject(index);
            fun = (JSFunction *) JS_GetPrivate(cx, obj);
            str = JS_DecompileFunction(cx, fun, JS_DONT_PRETTY_PRINT);
            if (str) {
              bytes = JS_GetStringBytes(str);
            } else {
              if (JS_IsExceptionPending(cx)) {
                if (!JS_ReportPendingException(cx))
                  JS_ClearPendingException(cx);
              }
              bytes = "N/A";
            }
            fprintf(gOutFile, " function %u (%s)", index, bytes);
            break;
          }
          case SRC_SWITCH:
            fprintf(gOutFile, " length %u", (uintN) js_GetSrcNoteOffset(sn, 0));
            caseOff = (uintN) js_GetSrcNoteOffset(sn, 1);
            if (caseOff)
                fprintf(gOutFile, " first case offset %u", caseOff);
            UpdateSwitchTableBounds(cx, script, offset,
                                    &switchTableStart, &switchTableEnd);
            break;
          case SRC_CATCH:
            delta = (uintN) js_GetSrcNoteOffset(sn, 0);
            if (delta) {
                if (script->main[offset] == JSOP_LEAVEBLOCK)
                    fprintf(gOutFile, " stack depth %u", delta);
                else
                    fprintf(gOutFile, " guard delta %u", delta);
            }
            break;
          default:;
        }
        fputc('\n', gOutFile);
    }
}

static JSBool
Notes(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    uintN i;
    JSScript *script;

    for (i = 0; i < argc; i++) {
        script = ValueToScript(cx, argv[i]);
        if (!script)
            continue;

        SrcNotes(cx, script);
    }
    return JS_TRUE;
}

JS_STATIC_ASSERT(JSTRY_CATCH == 0);
JS_STATIC_ASSERT(JSTRY_FINALLY == 1);
JS_STATIC_ASSERT(JSTRY_ITER == 2);

static const char* const TryNoteNames[] = { "catch", "finally", "iter" };

static JSBool
TryNotes(JSContext *cx, JSScript *script)
{
    JSTryNote *tn, *tnlimit;

    if (script->trynotesOffset == 0)
        return JS_TRUE;

    tn = script->trynotes()->vector;
    tnlimit = tn + script->trynotes()->length;
    fprintf(gOutFile, "\nException table:\n"
            "kind      stack    start      end\n");
    do {
        JS_ASSERT(tn->kind < JS_ARRAY_LENGTH(TryNoteNames));
        fprintf(gOutFile, " %-7s %6u %8u %8u\n",
                TryNoteNames[tn->kind], tn->stackDepth,
                tn->start, tn->start + tn->length);
    } while (++tn != tnlimit);
    return JS_TRUE;
}

static bool
DisassembleValue(JSContext *cx, jsval v, bool lines, bool recursive)
{
    JSScript *script = ValueToScript(cx, v);
    if (!script)
        return false;
    if (VALUE_IS_FUNCTION(cx, v)) {
        JSFunction *fun = JS_ValueToFunction(cx, v);
        if (fun && (fun->flags & ~7U)) {
            uint16 flags = fun->flags;
            fputs("flags:", stdout);

#define SHOW_FLAG(flag) if (flags & JSFUN_##flag) fputs(" " #flag, stdout);

            SHOW_FLAG(LAMBDA);
            SHOW_FLAG(SETTER);
            SHOW_FLAG(GETTER);
            SHOW_FLAG(BOUND_METHOD);
            SHOW_FLAG(HEAVYWEIGHT);
            SHOW_FLAG(THISP_STRING);
            SHOW_FLAG(THISP_NUMBER);
            SHOW_FLAG(THISP_BOOLEAN);
            SHOW_FLAG(EXPR_CLOSURE);
            SHOW_FLAG(TRCINFO);

#undef SHOW_FLAG

            if (FUN_NULL_CLOSURE(fun))
                fputs(" NULL_CLOSURE", stdout);
            else if (FUN_FLAT_CLOSURE(fun))
                fputs(" FLAT_CLOSURE", stdout);
            putchar('\n');
        }
    }

    if (!js_Disassemble(cx, script, lines, stdout))
        return false;
    SrcNotes(cx, script);
    TryNotes(cx, script);

    if (recursive && script->objectsOffset != 0) {
        JSObjectArray *objects = script->objects();
        for (uintN i = 0; i != objects->length; ++i) {
            JSObject *obj = objects->vector[i];
            if (HAS_FUNCTION_CLASS(obj)) {
                putchar('\n');
                if (!DisassembleValue(cx, OBJECT_TO_JSVAL(obj),
                                      lines, recursive)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static JSBool
Disassemble(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    bool lines = false, recursive = false;

    while (argc > 0 && JSVAL_IS_STRING(argv[0])) {
        const char *bytes = JS_GetStringBytes(JSVAL_TO_STRING(argv[0]));
        lines = !strcmp(bytes, "-l");
        recursive = !strcmp(bytes, "-r");
        if (!lines && !recursive)
            break;
        argv++, argc--;
    }

    for (uintN i = 0; i < argc; i++) {
        if (!DisassembleValue(cx, argv[i], lines, recursive))
            return false;
    }
    return true;
}

static JSBool
DisassFile(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSString *str;
    const char *filename;
    JSScript *script;
    JSBool ok;
    uint32 oldopts;

    if (!argc)
        return JS_TRUE;

    str = JS_ValueToString(cx, argv[0]);
    if (!str)
        return JS_FALSE;
    argv[0] = STRING_TO_JSVAL(str);

    filename = JS_GetStringBytes(str);
    oldopts = JS_GetOptions(cx);
    JS_SetOptions(cx, oldopts | JSOPTION_COMPILE_N_GO | JSOPTION_NO_SCRIPT_RVAL);
    script = JS_CompileFile(cx, obj, filename);
    JS_SetOptions(cx, oldopts);
    if (!script)
        return JS_FALSE;

    obj = JS_NewScriptObject(cx, script);
    if (!obj)
        return JS_FALSE;

    *rval = OBJECT_TO_JSVAL(obj); /* I like to root it, root it. */
    ok = Disassemble(cx, obj, 1, rval, rval); /* gross, but works! */
    *rval = JSVAL_VOID;

    return ok;
}

static JSBool
DisassWithSrc(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
              jsval *rval)
{
#define LINE_BUF_LEN 512
    uintN i, len, line1, line2, bupline;
    JSScript *script;
    FILE *file;
    char linebuf[LINE_BUF_LEN];
    jsbytecode *pc, *end;
    JSBool ok;
    static char sep[] = ";-------------------------";

    ok = JS_TRUE;
    for (i = 0; ok && i < argc; i++) {
        script = ValueToScript(cx, argv[i]);
        if (!script)
           return JS_FALSE;

        if (!script->filename) {
            JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL,
                                 JSSMSG_FILE_SCRIPTS_ONLY);
            return JS_FALSE;
        }

        file = fopen(script->filename, "r");
        if (!file) {
            JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL,
                                 JSSMSG_CANT_OPEN, script->filename,
                                 strerror(errno));
            return JS_FALSE;
        }

        pc = script->code;
        end = pc + script->length;

        /* burn the leading lines */
        line2 = JS_PCToLineNumber(cx, script, pc);
        for (line1 = 0; line1 < line2 - 1; line1++)
            fgets(linebuf, LINE_BUF_LEN, file);

        bupline = 0;
        while (pc < end) {
            line2 = JS_PCToLineNumber(cx, script, pc);

            if (line2 < line1) {
                if (bupline != line2) {
                    bupline = line2;
                    fprintf(gOutFile, "%s %3u: BACKUP\n", sep, line2);
                }
            } else {
                if (bupline && line1 == line2)
                    fprintf(gOutFile, "%s %3u: RESTORE\n", sep, line2);
                bupline = 0;
                while (line1 < line2) {
                    if (!fgets(linebuf, LINE_BUF_LEN, file)) {
                        JS_ReportErrorNumber(cx, my_GetErrorMessage, NULL,
                                             JSSMSG_UNEXPECTED_EOF,
                                             script->filename);
                        ok = JS_FALSE;
                        goto bail;
                    }
                    line1++;
                    fprintf(gOutFile, "%s %3u: %s", sep, line1, linebuf);
                }
            }

            len = js_Disassemble1(cx, script, pc,
                                  pc - script->code,
                                  JS_TRUE, stdout);
            if (!len) {
                ok = JS_FALSE;
                goto bail;
            }
            pc += len;
        }

      bail:
        fclose(file);
    }
    return ok;
#undef LINE_BUF_LEN
}

static JSBool
Tracing(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    FILE *file;

    if (argc == 0) {
        *rval = BOOLEAN_TO_JSVAL(cx->tracefp != 0);
        return JS_TRUE;
    }

    switch (JS_TypeOfValue(cx, argv[0])) {
      case JSTYPE_NUMBER:
      case JSTYPE_BOOLEAN: {
        JSBool bval;
        JS_ValueToBoolean(cx, argv[0], &bval);
        file = bval ? stderr : NULL;
        break;
      }
      case JSTYPE_STRING: {
        char *name = JS_GetStringBytes(JSVAL_TO_STRING(argv[0]));
        file = fopen(name, "w");
        if (!file) {
            JS_ReportError(cx, "tracing: couldn't open output file %s: %s", 
                           name, strerror(errno));
            return JS_FALSE;
        }
        break;
      }
      default:
          goto bad_argument;
    }
    if (cx->tracefp && cx->tracefp != stderr)
      fclose((FILE *)cx->tracefp);
    cx->tracefp = file;
    cx->tracePrevPc = NULL;
    return JS_TRUE;

 bad_argument:
    JSString *str = JS_ValueToString(cx, argv[0]);
    if (!str)
        return JS_FALSE;
    JS_ReportError(cx, "tracing: illegal argument %s",
                   JS_GetStringBytes(str));
    return JS_FALSE;
}

static void
DumpScope(JSContext *cx, JSObject *obj, FILE *fp)
{
    uintN i;
    JSScopeProperty *sprop;
    jsval v;
    JSString *str;

    i = 0;
    sprop = NULL;
    while (JS_PropertyIterator(obj, &sprop)) {
        fprintf(fp, "%3u %p ", i, (void *)sprop);

        v = ID_TO_VALUE(sprop->id);
        if (JSID_IS_INT(sprop->id)) {
            fprintf(fp, "[%ld]", (long)JSVAL_TO_INT(v));
        } else {
            if (JSID_IS_ATOM(sprop->id)) {
                str = JSVAL_TO_STRING(v);
            } else {
                JS_ASSERT(JSID_IS_OBJECT(sprop->id));
                str = js_ValueToString(cx, v);
                fputs("object ", fp);
            }
            if (!str)
                fputs("<error>", fp);
            else
                js_FileEscapedString(fp, str, '"');
        }
#define DUMP_ATTR(name) if (sprop->attrs & JSPROP_##name) fputs(" " #name, fp)
        DUMP_ATTR(ENUMERATE);
        DUMP_ATTR(READONLY);
        DUMP_ATTR(PERMANENT);
        DUMP_ATTR(GETTER);
        DUMP_ATTR(SETTER);
#undef  DUMP_ATTR

        fprintf(fp, " slot %lu flags %x shortid %d\n",
                (unsigned long)sprop->slot, sprop->flags, sprop->shortid);
    }
}

static JSBool
DumpStats(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    uintN i;
    JSString *str;
    const char *bytes;
    jsid id;
    JSObject *obj2;
    JSProperty *prop;
    jsval value;

    for (i = 0; i < argc; i++) {
        str = JS_ValueToString(cx, argv[i]);
        if (!str)
            return JS_FALSE;
        argv[i] = STRING_TO_JSVAL(str);
        bytes = JS_GetStringBytes(str);
        if (strcmp(bytes, "arena") == 0) {
#ifdef JS_ARENAMETER
            JS_DumpArenaStats(stdout);
#endif
        } else if (strcmp(bytes, "atom") == 0) {
            js_DumpAtoms(cx, gOutFile);
        } else if (strcmp(bytes, "global") == 0) {
            DumpScope(cx, cx->globalObject, stdout);
        } else {
            if (!JS_ValueToId(cx, STRING_TO_JSVAL(str), &id))
                return JS_FALSE;
            if (!js_FindProperty(cx, id, &obj, &obj2, &prop))
                return JS_FALSE;
            if (prop) {
                obj2->dropProperty(cx, prop);
                if (!obj->getProperty(cx, id, &value))
                    return JS_FALSE;
            }
            if (!prop || !JSVAL_IS_OBJECT(value)) {
                fprintf(gErrFile, "js: invalid stats argument %s\n",
                        bytes);
                continue;
            }
            obj = JSVAL_TO_OBJECT(value);
            if (obj)
                DumpScope(cx, obj, stdout);
        }
    }
    return JS_TRUE;
}

static JSBool
DumpHeap(JSContext *cx, uintN argc, jsval *vp)
{
    char *fileName;
    jsval v;
    void* startThing;
    uint32 startTraceKind;
    const char *badTraceArg;
    void *thingToFind;
    size_t maxDepth;
    void *thingToIgnore;
    FILE *dumpFile;
    JSBool ok;

    fileName = NULL;
    if (argc > 0) {
        v = JS_ARGV(cx, vp)[0];
        if (v != JSVAL_NULL) {
            JSString *str;

            str = JS_ValueToString(cx, v);
            if (!str)
                return JS_FALSE;
            JS_ARGV(cx, vp)[0] = STRING_TO_JSVAL(str);
            fileName = JS_GetStringBytes(str);
        }
    }

    startThing = NULL;
    startTraceKind = 0;
    if (argc > 1) {
        v = JS_ARGV(cx, vp)[1];
        if (JSVAL_IS_TRACEABLE(v)) {
            startThing = JSVAL_TO_TRACEABLE(v);
            startTraceKind = JSVAL_TRACE_KIND(v);
        } else if (v != JSVAL_NULL) {
            badTraceArg = "start";
            goto not_traceable_arg;
        }
    }

    thingToFind = NULL;
    if (argc > 2) {
        v = JS_ARGV(cx, vp)[2];
        if (JSVAL_IS_TRACEABLE(v)) {
            thingToFind = JSVAL_TO_TRACEABLE(v);
        } else if (v != JSVAL_NULL) {
            badTraceArg = "toFind";
            goto not_traceable_arg;
        }
    }

    maxDepth = (size_t)-1;
    if (argc > 3) {
        v = JS_ARGV(cx, vp)[3];
        if (v != JSVAL_NULL) {
            uint32 depth;

            if (!JS_ValueToECMAUint32(cx, v, &depth))
                return JS_FALSE;
            maxDepth = depth;
        }
    }

    thingToIgnore = NULL;
    if (argc > 4) {
        v = JS_ARGV(cx, vp)[4];
        if (JSVAL_IS_TRACEABLE(v)) {
            thingToIgnore = JSVAL_TO_TRACEABLE(v);
        } else if (v != JSVAL_NULL) {
            badTraceArg = "toIgnore";
            goto not_traceable_arg;
        }
    }

    if (!fileName) {
        dumpFile = stdout;
    } else {
        dumpFile = fopen(fileName, "w");
        if (!dumpFile) {
            JS_ReportError(cx, "can't open %s: %s", fileName, strerror(errno));
            return JS_FALSE;
        }
    }

    ok = JS_DumpHeap(cx, dumpFile, startThing, startTraceKind, thingToFind,
                     maxDepth, thingToIgnore);
    if (dumpFile != stdout)
        fclose(dumpFile);
    return ok;

  not_traceable_arg:
    JS_ReportError(cx, "argument '%s' is not null or a heap-allocated thing",
                   badTraceArg);
    return JS_FALSE;
}

#endif /* DEBUG */

#ifdef TEST_CVTARGS
#include <ctype.h>

static const char *
EscapeWideString(jschar *w)
{
    static char enuf[80];
    static char hex[] = "0123456789abcdef";
    jschar u;
    unsigned char b, c;
    int i, j;

    if (!w)
        return "";
    for (i = j = 0; i < sizeof enuf - 1; i++, j++) {
        u = w[j];
        if (u == 0)
            break;
        b = (unsigned char)(u >> 8);
        c = (unsigned char)(u);
        if (b) {
            if (i >= sizeof enuf - 6)
                break;
            enuf[i++] = '\\';
            enuf[i++] = 'u';
            enuf[i++] = hex[b >> 4];
            enuf[i++] = hex[b & 15];
            enuf[i++] = hex[c >> 4];
            enuf[i] = hex[c & 15];
        } else if (!isprint(c)) {
            if (i >= sizeof enuf - 4)
                break;
            enuf[i++] = '\\';
            enuf[i++] = 'x';
            enuf[i++] = hex[c >> 4];
            enuf[i] = hex[c & 15];
        } else {
            enuf[i] = (char)c;
        }
    }
    enuf[i] = 0;
    return enuf;
}

#include <stdarg.h>

static JSBool
ZZ_formatter(JSContext *cx, const char *format, JSBool fromJS, jsval **vpp,
             va_list *app)
{
    jsval *vp;
    va_list ap;
    jsdouble re, im;

    printf("entering ZZ_formatter");
    vp = *vpp;
    ap = *app;
    if (fromJS) {
        if (!JS_ValueToNumber(cx, vp[0], &re))
            return JS_FALSE;
        if (!JS_ValueToNumber(cx, vp[1], &im))
            return JS_FALSE;
        *va_arg(ap, jsdouble *) = re;
        *va_arg(ap, jsdouble *) = im;
    } else {
        re = va_arg(ap, jsdouble);
        im = va_arg(ap, jsdouble);
        if (!JS_NewNumberValue(cx, re, &vp[0]))
            return JS_FALSE;
        if (!JS_NewNumberValue(cx, im, &vp[1]))
            return JS_FALSE;
    }
    *vpp = vp + 2;
    *app = ap;
    printf("leaving ZZ_formatter");
    return JS_TRUE;
}

static JSBool
ConvertArgs(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSBool b = JS_FALSE;
    jschar c = 0;
    int32 i = 0, j = 0;
    uint32 u = 0;
    jsdouble d = 0, I = 0, re = 0, im = 0;
    char *s = NULL;
    JSString *str = NULL;
    jschar *w = NULL;
    JSObject *obj2 = NULL;
    JSFunction *fun = NULL;
    jsval v = JSVAL_VOID;
    JSBool ok;

    if (!JS_AddArgumentFormatter(cx, "ZZ", ZZ_formatter))
        return JS_FALSE;
    ok = JS_ConvertArguments(cx, argc, argv, "b/ciujdIsSWofvZZ*",
                             &b, &c, &i, &u, &j, &d, &I, &s, &str, &w, &obj2,
                             &fun, &v, &re, &im);
    JS_RemoveArgumentFormatter(cx, "ZZ");
    if (!ok)
        return JS_FALSE;
    fprintf(gOutFile,
            "b %u, c %x (%c), i %ld, u %lu, j %ld\n",
            b, c, (char)c, i, u, j);
    ToString obj2string(cx, obj2);
    ToString valueString(cx, v);
    JSString *tmpstr = JS_DecompileFunction(cx, fun, 4);
    const char *func;
    if (tmpstr) {
        func = JS_GetStringBytes(tmpstr);
    } else {
        if (JS_IsExceptionPending(cx)) {
            if (!JS_ReportPendingException(cx))
                JS_ClearPendingException(cx);
        }
        func = "error decompiling fun";
    }
    fprintf(gOutFile,
            "d %g, I %g, s %s, S %s, W %s, obj %s, fun %s\n"
            "v %s, re %g, im %g\n",
            d, I, s, str ? JS_GetStringBytes(str) : "", EscapeWideString(w),
            obj2string.getBytes(),
            fun ? func : "",
            valueString.getBytes(), re, im);
    return JS_TRUE;
}
#endif

static JSBool
BuildDate(JSContext *cx, uintN argc, jsval *vp)
{
    char version[20] = "\n";
#if JS_VERSION < 150
    sprintf(version, " for version %d\n", JS_VERSION);
#endif
    fprintf(gOutFile, "built on %s at %s%s", __DATE__, __TIME__, version);
    *vp = JSVAL_VOID;
    return JS_TRUE;
}

static JSBool
Clear(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    if (argc != 0 && !JS_ValueToObject(cx, argv[0], &obj))
        return JS_FALSE;
    JS_ClearScope(cx, obj);
    return JS_TRUE;
}

static JSBool
Intern(JSContext *cx, uintN argc, jsval *vp)
{
    JSString *str;

    str = JS_ValueToString(cx, argc == 0 ? JSVAL_VOID : vp[2]);
    if (!str)
        return JS_FALSE;
    if (!JS_InternUCStringN(cx, JS_GetStringChars(str),
                                JS_GetStringLength(str))) {
        return JS_FALSE;
    }
    *vp = JSVAL_VOID;
    return JS_TRUE;
}

static JSBool
Clone(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSObject *funobj, *parent, *clone;

    if (VALUE_IS_FUNCTION(cx, argv[0])) {
        funobj = JSVAL_TO_OBJECT(argv[0]);
    } else {
        JSFunction *fun = JS_ValueToFunction(cx, argv[0]);
        if (!fun)
            return JS_FALSE;
        funobj = JS_GetFunctionObject(fun);
    }
    if (argc > 1) {
        if (!JS_ValueToObject(cx, argv[1], &parent))
            return JS_FALSE;
    } else {
        parent = JS_GetParent(cx, funobj);
    }
    clone = JS_CloneFunctionObject(cx, funobj, parent);
    if (!clone)
        return JS_FALSE;
    *rval = OBJECT_TO_JSVAL(clone);
    return JS_TRUE;
}

static JSBool
Seal(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSObject *target;
    JSBool deep = JS_FALSE;

    if (!JS_ConvertArguments(cx, argc, argv, "o/b", &target, &deep))
        return JS_FALSE;
    if (!target)
        return JS_TRUE;
    return JS_SealObject(cx, target, deep);
}

static JSBool
GetPDA(JSContext *cx, uintN argc, jsval *vp)
{
    JSObject *vobj, *aobj, *pdobj;
    JSBool ok;
    JSPropertyDescArray pda;
    JSPropertyDesc *pd;
    uint32 i;
    jsval v;

    if (!JS_ValueToObject(cx, argc == 0 ? JSVAL_VOID : vp[2], &vobj))
        return JS_FALSE;
    if (!vobj) {
        *vp = JSVAL_VOID;
        return JS_TRUE;
    }

    aobj = JS_NewArrayObject(cx, 0, NULL);
    if (!aobj)
        return JS_FALSE;
    *vp = OBJECT_TO_JSVAL(aobj);

    ok = JS_GetPropertyDescArray(cx, vobj, &pda);
    if (!ok)
        return JS_FALSE;
    pd = pda.array;
    for (i = 0; i < pda.length; i++, pd++) {
        pdobj = JS_NewObject(cx, NULL, NULL, NULL);
        if (!pdobj) {
            ok = JS_FALSE;
            break;
        }

        /* Protect pdobj from GC by setting it as an element of aobj now */
        v = OBJECT_TO_JSVAL(pdobj);
        ok = JS_SetElement(cx, aobj, i, &v);
        if (!ok)
            break;

        ok = JS_SetProperty(cx, pdobj, "id", &pd->id) &&
             JS_SetProperty(cx, pdobj, "value", &pd->value) &&
             (v = INT_TO_JSVAL(pd->flags),
              JS_SetProperty(cx, pdobj, "flags", &v)) &&
             (v = INT_TO_JSVAL(pd->slot),
              JS_SetProperty(cx, pdobj, "slot", &v)) &&
             JS_SetProperty(cx, pdobj, "alias", &pd->alias);
        if (!ok)
            break;
    }
    JS_PutPropertyDescArray(cx, &pda);
    return ok;
}

static JSBool
GetSLX(JSContext *cx, uintN argc, jsval *vp)
{
    JSScript *script;

    script = ValueToScript(cx, argc == 0 ? JSVAL_VOID : vp[2]);
    if (!script)
        return JS_FALSE;
    *vp = INT_TO_JSVAL(js_GetScriptLineExtent(script));
    return JS_TRUE;
}

static JSBool
ToInt32(JSContext *cx, uintN argc, jsval *vp)
{
    int32 i;

    if (!JS_ValueToInt32(cx, argc == 0 ? JSVAL_VOID : vp[2], &i))
        return JS_FALSE;
    return JS_NewNumberValue(cx, i, vp);
}

static JSBool
StringsAreUTF8(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
               jsval *rval)
{
    *rval = JS_CStringsAreUTF8() ? JSVAL_TRUE : JSVAL_FALSE;
    return JS_TRUE;
}

static JSBool
StackQuota(JSContext *cx, uintN argc, jsval *vp)
{
    uint32 n;

    if (argc == 0)
        return JS_NewNumberValue(cx, (double) gScriptStackQuota, vp);
    if (!JS_ValueToECMAUint32(cx, JS_ARGV(cx, vp)[0], &n))
        return JS_FALSE;
    gScriptStackQuota = n;
    JS_SetScriptStackQuota(cx, gScriptStackQuota);
    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return JS_TRUE;
}

static const char* badUTF8 = "...\xC0...";
static const char* bigUTF8 = "...\xFB\xBF\xBF\xBF\xBF...";
static const jschar badSurrogate[] = { 'A', 'B', 'C', 0xDEEE, 'D', 'E', 0 };

static JSBool
TestUTF8(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    int32 mode = 1;
    jschar chars[20];
    size_t charsLength = 5;
    char bytes[20];
    size_t bytesLength = 20;
    if (argc && !JS_ValueToInt32(cx, *argv, &mode))
        return JS_FALSE;

    /* The following throw errors if compiled with UTF-8. */
    switch (mode) {
      /* mode 1: malformed UTF-8 string. */
      case 1:
        JS_NewStringCopyZ(cx, badUTF8);
        break;
      /* mode 2: big UTF-8 character. */
      case 2:
        JS_NewStringCopyZ(cx, bigUTF8);
        break;
      /* mode 3: bad surrogate character. */
      case 3:
        JS_EncodeCharacters(cx, badSurrogate, 6, bytes, &bytesLength);
        break;
      /* mode 4: use a too small buffer. */
      case 4:
        JS_DecodeBytes(cx, "1234567890", 10, chars, &charsLength);
        break;
      default:
        JS_ReportError(cx, "invalid mode parameter");
        return JS_FALSE;
    }
    return !JS_IsExceptionPending (cx);
}

static JSBool
ThrowError(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JS_ReportError(cx, "This is an error");
    return JS_FALSE;
}

#define LAZY_STANDARD_CLASSES

/* A class for easily testing the inner/outer object callbacks. */
typedef struct ComplexObject {
    JSBool isInner;
    JSBool frozen;
    JSObject *inner;
    JSObject *outer;
} ComplexObject;

static JSObject *
split_create_outer(JSContext *cx);

static JSObject *
split_create_inner(JSContext *cx, JSObject *outer);

static ComplexObject *
split_get_private(JSContext *cx, JSObject *obj);

static JSBool
split_addProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    ComplexObject *cpx;
    jsid asId;

    cpx = split_get_private(cx, obj);
    if (!cpx)
        return JS_TRUE;
    if (!cpx->isInner && cpx->inner) {
        /* Make sure to define this property on the inner object. */
        if (!JS_ValueToId(cx, *vp, &asId))
            return JS_FALSE;
        return JS_DefinePropertyById(cx, cpx->inner, asId, *vp, NULL, NULL, JSPROP_ENUMERATE);
    }
    return JS_TRUE;
}

static JSBool
split_getProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    ComplexObject *cpx;

    cpx = split_get_private(cx, obj);
    if (!cpx)
        return JS_TRUE;

    if (JSVAL_IS_STRING(id) &&
        !strcmp(JS_GetStringBytes(JSVAL_TO_STRING(id)), "isInner")) {
        *vp = BOOLEAN_TO_JSVAL(cpx->isInner);
        return JS_TRUE;
    }

    if (!cpx->isInner && cpx->inner) {
        if (JSVAL_IS_STRING(id)) {
            JSString *str;

            str = JSVAL_TO_STRING(id);
            return JS_GetUCProperty(cx, cpx->inner, JS_GetStringChars(str),
                                    JS_GetStringLength(str), vp);
        }
        if (JSVAL_IS_INT(id))
            return JS_GetElement(cx, cpx->inner, JSVAL_TO_INT(id), vp);
        return JS_TRUE;
    }

    return JS_TRUE;
}

static JSBool
split_setProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    ComplexObject *cpx;

    cpx = split_get_private(cx, obj);
    if (!cpx)
        return JS_TRUE;
    if (!cpx->isInner && cpx->inner) {
        if (JSVAL_IS_STRING(id)) {
            JSString *str;

            str = JSVAL_TO_STRING(id);
            return JS_SetUCProperty(cx, cpx->inner, JS_GetStringChars(str),
                                    JS_GetStringLength(str), vp);
        }
        if (JSVAL_IS_INT(id))
            return JS_SetElement(cx, cpx->inner, JSVAL_TO_INT(id), vp);
        return JS_TRUE;
    }

    return JS_TRUE;
}

static JSBool
split_delProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    ComplexObject *cpx;
    jsid asId;

    cpx = split_get_private(cx, obj);
    if (!cpx)
        return JS_TRUE;
    if (!cpx->isInner && cpx->inner) {
        /* Make sure to define this property on the inner object. */
        if (!JS_ValueToId(cx, *vp, &asId))
            return JS_FALSE;
        return cpx->inner->deleteProperty(cx, asId, vp);
    }
    return JS_TRUE;
}

static JSBool
split_enumerate(JSContext *cx, JSObject *obj, JSIterateOp enum_op,
                  jsval *statep, jsid *idp)
{
    ComplexObject *cpx;
    JSObject *iterator;

    switch (enum_op) {
      case JSENUMERATE_INIT:
        cpx = (ComplexObject *) JS_GetPrivate(cx, obj);

        if (!cpx->isInner && cpx->inner)
            obj = cpx->inner;

        iterator = JS_NewPropertyIterator(cx, obj);
        if (!iterator)
            return JS_FALSE;

        *statep = OBJECT_TO_JSVAL(iterator);
        if (idp)
            *idp = JSVAL_ZERO;
        break;

      case JSENUMERATE_NEXT:
        iterator = (JSObject*)JSVAL_TO_OBJECT(*statep);
        if (!JS_NextProperty(cx, iterator, idp))
            return JS_FALSE;

        if (!JSVAL_IS_VOID(*idp))
            break;
        /* Fall through. */

      case JSENUMERATE_DESTROY:
        /* Let GC at our iterator object. */
        *statep = JSVAL_NULL;
        break;
    }

    return JS_TRUE;
}

static JSBool
split_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
                JSObject **objp)
{
    ComplexObject *cpx;

    if (JSVAL_IS_STRING(id) &&
        !strcmp(JS_GetStringBytes(JSVAL_TO_STRING(id)), "isInner")) {
        *objp = obj;
        return JS_DefineProperty(cx, obj, "isInner", JSVAL_VOID, NULL, NULL,
                                 JSPROP_SHARED);
    }

    cpx = split_get_private(cx, obj);
    if (!cpx)
        return JS_TRUE;
    if (!cpx->isInner && cpx->inner) {
        jsid asId;
        JSProperty *prop;

        if (!JS_ValueToId(cx, id, &asId))
            return JS_FALSE;

        if (!cpx->inner->lookupProperty(cx, asId, objp, &prop))
            return JS_FALSE;
        if (prop)
            cpx->inner->dropProperty(cx, prop);

        return JS_TRUE;
    }

#ifdef LAZY_STANDARD_CLASSES
    if (!(flags & JSRESOLVE_ASSIGNING)) {
        JSBool resolved;

        if (!JS_ResolveStandardClass(cx, obj, id, &resolved))
            return JS_FALSE;

        if (resolved) {
            *objp = obj;
            return JS_TRUE;
        }
    }
#endif

    /* XXX For additional realism, let's resolve some random property here. */
    return JS_TRUE;
}

static void
split_finalize(JSContext *cx, JSObject *obj)
{
    JS_free(cx, JS_GetPrivate(cx, obj));
}

static uint32
split_mark(JSContext *cx, JSObject *obj, void *arg)
{
    ComplexObject *cpx;

    cpx = (ComplexObject *) JS_GetPrivate(cx, obj);

    if (!cpx->isInner && cpx->inner) {
        /* Mark the inner object. */
        JS_MarkGCThing(cx, cpx->inner, "ComplexObject.inner", arg);
    }

    return 0;
}

static JSObject *
split_outerObject(JSContext *cx, JSObject *obj)
{
    ComplexObject *cpx;

    cpx = (ComplexObject *) JS_GetPrivate(cx, obj);
    return cpx->isInner ? cpx->outer : obj;
}

static JSObject *
split_thisObject(JSContext *cx, JSObject *obj)
{
    OBJ_TO_OUTER_OBJECT(cx, obj);
    if (!obj)
        return NULL;
    return obj;
}

static JSObjectOps split_objectops;

static JSObjectOps *
split_getObjectOps(JSContext *cx, JSClass *clasp)
{
    if (!split_objectops.thisObject) {
        memcpy(&split_objectops, &js_ObjectOps, sizeof split_objectops);
        split_objectops.thisObject = split_thisObject;
    }

    return &split_objectops;
}

static JSBool
split_equality(JSContext *cx, JSObject *obj, jsval v, JSBool *bp);

static JSObject *
split_innerObject(JSContext *cx, JSObject *obj)
{
    ComplexObject *cpx;

    cpx = (ComplexObject *) JS_GetPrivate(cx, obj);
    if (cpx->frozen) {
        JS_ASSERT(!cpx->isInner);
        return obj;
    }
    return !cpx->isInner ? cpx->inner : obj;
}

static JSExtendedClass split_global_class = {
    {"split_global",
    JSCLASS_NEW_RESOLVE | JSCLASS_NEW_ENUMERATE | JSCLASS_HAS_PRIVATE |
    JSCLASS_GLOBAL_FLAGS | JSCLASS_IS_EXTENDED,
    split_addProperty, split_delProperty,
    split_getProperty, split_setProperty,
    (JSEnumerateOp)split_enumerate,
    (JSResolveOp)split_resolve,
    JS_ConvertStub, split_finalize,
    split_getObjectOps, NULL, NULL, NULL, NULL, NULL,
    split_mark, NULL},
    split_equality, split_outerObject, split_innerObject,
    NULL, NULL, NULL, NULL, NULL
};

static JSBool
split_equality(JSContext *cx, JSObject *obj, jsval v, JSBool *bp)
{
    *bp = JS_FALSE;
    if (JSVAL_IS_PRIMITIVE(v))
        return JS_TRUE;

    JSObject *obj2 = JSVAL_TO_OBJECT(v);
    if (JS_GET_CLASS(cx, obj2) != &split_global_class.base)
        return JS_TRUE;

    ComplexObject *cpx = (ComplexObject *) JS_GetPrivate(cx, obj2);
    JS_ASSERT(!cpx->isInner);

    ComplexObject *ourCpx = (ComplexObject *) JS_GetPrivate(cx, obj);
    JS_ASSERT(!ourCpx->isInner);

    *bp = (cpx == ourCpx);
    return JS_TRUE;
}

JSObject *
split_create_outer(JSContext *cx)
{
    ComplexObject *cpx;
    JSObject *obj;

    cpx = (ComplexObject *) JS_malloc(cx, sizeof *obj);
    if (!cpx)
        return NULL;
    cpx->isInner = JS_FALSE;
    cpx->frozen = JS_TRUE;
    cpx->inner = NULL;
    cpx->outer = NULL;

    obj = JS_NewObject(cx, &split_global_class.base, NULL, NULL);
    if (!obj || !JS_SetParent(cx, obj, NULL)) {
        JS_free(cx, cpx);
        return NULL;
    }

    if (!JS_SetPrivate(cx, obj, cpx)) {
        JS_free(cx, cpx);
        return NULL;
    }

    return obj;
}

static JSObject *
split_create_inner(JSContext *cx, JSObject *outer)
{
    ComplexObject *cpx, *outercpx;
    JSObject *obj;

    JS_ASSERT(JS_GET_CLASS(cx, outer) == &split_global_class.base);

    cpx = (ComplexObject *) JS_malloc(cx, sizeof *cpx);
    if (!cpx)
        return NULL;
    cpx->isInner = JS_TRUE;
    cpx->frozen = JS_FALSE;
    cpx->inner = NULL;
    cpx->outer = outer;

    obj = JS_NewObject(cx, &split_global_class.base, NULL, NULL);
    if (!obj || !JS_SetParent(cx, obj, NULL) || !JS_SetPrivate(cx, obj, cpx)) {
        JS_free(cx, cpx);
        return NULL;
    }

    outercpx = (ComplexObject *) JS_GetPrivate(cx, outer);
    outercpx->inner = obj;
    outercpx->frozen = JS_FALSE;

    return obj;
}

static ComplexObject *
split_get_private(JSContext *cx, JSObject *obj)
{
    do {
        if (JS_GET_CLASS(cx, obj) == &split_global_class.base)
            return (ComplexObject *) JS_GetPrivate(cx, obj);
        obj = JS_GetParent(cx, obj);
    } while (obj);

    return NULL;
}

static JSBool
sandbox_enumerate(JSContext *cx, JSObject *obj)
{
    jsval v;
    JSBool b;

    if (!JS_GetProperty(cx, obj, "lazy", &v))
        return JS_FALSE;

    JS_ValueToBoolean(cx, v, &b);
    return !b || JS_EnumerateStandardClasses(cx, obj);
}

static JSBool
sandbox_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
                JSObject **objp)
{
    jsval v;
    JSBool b, resolved;

    if (!JS_GetProperty(cx, obj, "lazy", &v))
        return JS_FALSE;

    JS_ValueToBoolean(cx, v, &b);
    if (b && (flags & JSRESOLVE_ASSIGNING) == 0) {
        if (!JS_ResolveStandardClass(cx, obj, id, &resolved))
            return JS_FALSE;
        if (resolved) {
            *objp = obj;
            return JS_TRUE;
        }
    }
    *objp = NULL;
    return JS_TRUE;
}

static JSClass sandbox_class = {
    "sandbox",
    JSCLASS_NEW_RESOLVE,
    JS_PropertyStub,   JS_PropertyStub,
    JS_PropertyStub,   JS_PropertyStub,
    sandbox_enumerate, (JSResolveOp)sandbox_resolve,
    JS_ConvertStub,    NULL,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

static JSBool
EvalInContext(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
              jsval *rval)
{
    JSString *str;
    JSObject *sobj;
    JSContext *scx;
    const jschar *src;
    size_t srclen;
    JSBool lazy, ok;
    jsval v;
    JSStackFrame *fp;

    sobj = NULL;
    if (!JS_ConvertArguments(cx, argc, argv, "S / o", &str, &sobj))
        return JS_FALSE;

    WITH_LOCKED_CONTEXT_LIST(
        scx = JS_NewContext(JS_GetRuntime(cx), gStackChunkSize)
    );
    if (!scx) {
        JS_ReportOutOfMemory(cx);
        return JS_FALSE;
    }
    JS_SetOptions(scx, JS_GetOptions(cx));

    JS_BeginRequest(scx);
    src = JS_GetStringChars(str);
    srclen = JS_GetStringLength(str);
    lazy = JS_FALSE;
    if (srclen == 4 &&
        src[0] == 'l' && src[1] == 'a' && src[2] == 'z' && src[3] == 'y') {
        lazy = JS_TRUE;
        srclen = 0;
    }

    if (!sobj) {
        sobj = JS_NewObject(scx, &sandbox_class, NULL, NULL);
        if (!sobj || (!lazy && !JS_InitStandardClasses(scx, sobj))) {
            ok = JS_FALSE;
            goto out;
        }
        v = BOOLEAN_TO_JSVAL(lazy);
        ok = JS_SetProperty(cx, sobj, "lazy", &v);
        if (!ok)
            goto out;
    }

    if (srclen == 0) {
        *rval = OBJECT_TO_JSVAL(sobj);
        ok = JS_TRUE;
    } else {
        fp = JS_GetScriptedCaller(cx, NULL);
        JS_SetGlobalObject(scx, sobj);
        JS_ToggleOptions(scx, JSOPTION_DONT_REPORT_UNCAUGHT);
        ok = JS_EvaluateUCScript(scx, sobj, src, srclen,
                                 fp->script->filename,
                                 JS_PCToLineNumber(cx, fp->script,
                                                   fp->regs->pc),
                                 rval);
        if (!ok) {
            if (JS_GetPendingException(scx, &v))
                JS_SetPendingException(cx, v);
            else
                JS_ReportOutOfMemory(cx);
        }
    }

out:
    JS_EndRequest(scx);
    WITH_LOCKED_CONTEXT_LIST(
        JS_DestroyContextNoGC(scx)
    );
    return ok;
}

static JSBool
ShapeOf(JSContext *cx, uintN argc, jsval *vp)
{
    jsval v = JS_ARGV(cx, vp)[0];
    if (!JSVAL_IS_OBJECT(v)) {
        JS_ReportError(cx, "shapeOf: object expected");
        return JS_FALSE;
    }
    JSObject *obj = JSVAL_TO_OBJECT(v);
    if (!obj) {
        *vp = JSVAL_ZERO;
        return JS_TRUE;
    }
    if (!OBJ_IS_NATIVE(obj)) {
        *vp = INT_TO_JSVAL(-1);
        return JS_TRUE;
    }
    return JS_NewNumberValue(cx, OBJ_SHAPE(obj), vp);
}

#ifdef JS_THREADSAFE

/*
 * Check that t1 comes strictly before t2. The function correctly deals with
 * PRIntervalTime wrap-around between t2 and t1 assuming that t2 and t1 stays
 * within INT32_MAX from each other. We use MAX_TIMEOUT_INTERVAL to enforce
 * this restriction.
 */
static bool
IsBefore(PRIntervalTime t1, PRIntervalTime t2)
{
    return int32(t1 - t2) < 0;
}

static JSBool
Sleep_fn(JSContext *cx, uintN argc, jsval *vp)
{
    PRIntervalTime t_ticks;

    if (argc == 0) {
        t_ticks = 0;
    } else {
        jsdouble t_secs;

        if (!JS_ValueToNumber(cx, argc == 0 ? JSVAL_VOID : vp[2], &t_secs))
            return JS_FALSE;

        /* NB: The next condition also filter out NaNs. */
        if (!(t_secs <= MAX_TIMEOUT_INTERVAL)) {
            JS_ReportError(cx, "Excessive sleep interval");
            return JS_FALSE;
        }
        t_ticks = (t_secs <= 0.0)
                  ? 0
                  : PRIntervalTime(PR_TicksPerSecond() * t_secs);
    }
    if (t_ticks == 0) {
        JS_YieldRequest(cx);
    } else {
        jsrefcount rc = JS_SuspendRequest(cx);
        PR_Lock(gWatchdogLock);
        PRIntervalTime to_wakeup = PR_IntervalNow() + t_ticks;
        for (;;) {
            PR_WaitCondVar(gSleepWakeup, t_ticks);
            if (gCanceled)
                break;
            PRIntervalTime now = PR_IntervalNow();
            if (!IsBefore(now, to_wakeup))
                break;
            t_ticks = to_wakeup - now;
        }
        PR_Unlock(gWatchdogLock);
        JS_ResumeRequest(cx, rc);
    }
    return !gCanceled;
}

typedef struct ScatterThreadData ScatterThreadData;
typedef struct ScatterData ScatterData;

typedef enum ScatterStatus {
    SCATTER_WAIT,
    SCATTER_GO,
    SCATTER_CANCEL
} ScatterStatus;

struct ScatterData {
    ScatterThreadData   *threads;
    jsval               *results;
    PRLock              *lock;
    PRCondVar           *cvar;
    ScatterStatus       status;
};

struct ScatterThreadData {
    jsint               index;
    ScatterData         *shared;
    PRThread            *thr;
    JSContext           *cx;
    jsval               fn;
};

static void
DoScatteredWork(JSContext *cx, ScatterThreadData *td)
{
    jsval *rval = &td->shared->results[td->index];

    if (!JS_CallFunctionValue(cx, NULL, td->fn, 0, NULL, rval)) {
        *rval = JSVAL_VOID;
        JS_GetPendingException(cx, rval);
        JS_ClearPendingException(cx);
    }
}

static void
RunScatterThread(void *arg)
{
    int stackDummy;
    ScatterThreadData *td;
    ScatterStatus st;
    JSContext *cx;

    if (PR_FAILURE == PR_SetThreadPrivate(gStackBaseThreadIndex, &stackDummy))
        return;

    td = (ScatterThreadData *)arg;
    cx = td->cx;

    /* Wait for our signal. */
    PR_Lock(td->shared->lock);
    while ((st = td->shared->status) == SCATTER_WAIT)
        PR_WaitCondVar(td->shared->cvar, PR_INTERVAL_NO_TIMEOUT);
    PR_Unlock(td->shared->lock);

    if (st == SCATTER_CANCEL)
        return;

    /* We are good to go. */
    JS_SetContextThread(cx);
    SetThreadStackLimit(cx);
    JS_BeginRequest(cx);
    DoScatteredWork(cx, td);
    JS_EndRequest(cx);
    JS_ClearContextThread(cx);
}

/*
 * scatter(fnArray) - Call each function in `fnArray` without arguments, each
 * in a different thread. When all threads have finished, return an array: the
 * return values. Errors are not propagated; if any of the function calls
 * fails, the corresponding element in the results array gets the exception
 * object, if any, else (undefined).
 */
static JSBool
Scatter(JSContext *cx, uintN argc, jsval *vp)
{
    jsuint i;
    jsuint n;  /* number of threads */
    JSObject *inArr;
    JSObject *arr;
    ScatterData sd;
    JSBool ok;
    jsrefcount rc;

    sd.lock = NULL;
    sd.cvar = NULL;
    sd.results = NULL;
    sd.threads = NULL;
    sd.status = SCATTER_WAIT;

    if (argc == 0 || JSVAL_IS_PRIMITIVE(JS_ARGV(cx, vp)[0])) {
        JS_ReportError(cx, "the first argument must be an object");
        goto fail;
    }

    inArr = JSVAL_TO_OBJECT(JS_ARGV(cx, vp)[0]);
    ok = JS_GetArrayLength(cx, inArr, &n);
    if (!ok)
        goto out;
    if (n == 0)
        goto success;

    sd.lock = PR_NewLock();
    if (!sd.lock)
        goto fail;

    sd.cvar = PR_NewCondVar(sd.lock);
    if (!sd.cvar)
        goto fail;

    sd.results = (jsval *) malloc(n * sizeof(jsval));
    if (!sd.results)
        goto fail;
    for (i = 0; i < n; i++) {
        sd.results[i] = JSVAL_VOID;
        ok = JS_AddRoot(cx, &sd.results[i]);
        if (!ok) {
            while (i-- > 0)
                JS_RemoveRoot(cx, &sd.results[i]);
            free(sd.results);
            sd.results = NULL;
            goto fail;
        }
    }

    sd.threads = (ScatterThreadData *) malloc(n * sizeof(ScatterThreadData));
    if (!sd.threads)
        goto fail;
    for (i = 0; i < n; i++) {
        sd.threads[i].index = i;
        sd.threads[i].shared = &sd;
        sd.threads[i].thr = NULL;
        sd.threads[i].cx = NULL;
        sd.threads[i].fn = JSVAL_NULL;

        ok = JS_AddRoot(cx, &sd.threads[i].fn);
        if (ok && !JS_GetElement(cx, inArr, (jsint) i, &sd.threads[i].fn)) {
            JS_RemoveRoot(cx, &sd.threads[i].fn);
            ok = JS_FALSE;
        }
        if (!ok) {
            while (i-- > 0)
                JS_RemoveRoot(cx, &sd.threads[i].fn);
            free(sd.threads);
            sd.threads = NULL;
            goto fail;
        }
    }

    for (i = 1; i < n; i++) {
        JSContext *newcx;
        WITH_LOCKED_CONTEXT_LIST(
            newcx = JS_NewContext(JS_GetRuntime(cx), 8192)
        );
        if (!newcx)
            goto fail;
        JS_SetGlobalObject(newcx, JS_GetGlobalObject(cx));
        JS_ClearContextThread(newcx);
        sd.threads[i].cx = newcx;
    }

    for (i = 1; i < n; i++) {
        PRThread *t = PR_CreateThread(PR_USER_THREAD,
                                      RunScatterThread,
                                      &sd.threads[i],
                                      PR_PRIORITY_NORMAL,
                                      PR_GLOBAL_THREAD,
                                      PR_JOINABLE_THREAD,
                                      0);
        if (!t) {
            /* Failed to start thread. */
            PR_Lock(sd.lock);
            sd.status = SCATTER_CANCEL;
            PR_NotifyAllCondVar(sd.cvar);
            PR_Unlock(sd.lock);
            while (i-- > 1)
                PR_JoinThread(sd.threads[i].thr);
            goto fail;
        }

        sd.threads[i].thr = t;
    }
    PR_Lock(sd.lock);
    sd.status = SCATTER_GO;
    PR_NotifyAllCondVar(sd.cvar);
    PR_Unlock(sd.lock);

    DoScatteredWork(cx, &sd.threads[0]);

    rc = JS_SuspendRequest(cx);
    for (i = 1; i < n; i++) {
        PR_JoinThread(sd.threads[i].thr);
    }
    JS_ResumeRequest(cx, rc);

success:
    arr = JS_NewArrayObject(cx, n, sd.results);
    if (!arr)
        goto fail;
    *vp = OBJECT_TO_JSVAL(arr);
    ok = JS_TRUE;

out:
    if (sd.threads) {
        JSContext *acx;

        for (i = 0; i < n; i++) {
            JS_RemoveRoot(cx, &sd.threads[i].fn);
            acx = sd.threads[i].cx;
            if (acx) {
                JS_SetContextThread(acx);
                WITH_LOCKED_CONTEXT_LIST(
                    JS_DestroyContext(acx)
                );
            }
        }
        free(sd.threads);
    }
    if (sd.results) {
        for (i = 0; i < n; i++)
            JS_RemoveRoot(cx, &sd.results[i]);
        free(sd.results);
    }
    if (sd.cvar)
        PR_DestroyCondVar(sd.cvar);
    if (sd.lock)
        PR_DestroyLock(sd.lock);

    return ok;

fail:
    ok = JS_FALSE;
    goto out;
}

static bool
InitWatchdog(JSRuntime *rt)
{
    JS_ASSERT(!gWatchdogThread);
    gWatchdogLock = PR_NewLock();
    if (gWatchdogLock) {
        gWatchdogWakeup = PR_NewCondVar(gWatchdogLock);
        if (gWatchdogWakeup) {
            gSleepWakeup = PR_NewCondVar(gWatchdogLock);
            if (gSleepWakeup)
                return true;
            PR_DestroyCondVar(gWatchdogWakeup);
        }
        PR_DestroyLock(gWatchdogLock);
    }
    return false;
}

static void
KillWatchdog()
{
    PRThread *thread;

    PR_Lock(gWatchdogLock);
    thread = gWatchdogThread;
    if (thread) {
        /*
         * The watchdog thread is running, tell it to terminate waking it up
         * if necessary.
         */
        gWatchdogThread = NULL;
        PR_NotifyCondVar(gWatchdogWakeup);
    }
    PR_Unlock(gWatchdogLock);
    if (thread)
        PR_JoinThread(thread);
    PR_DestroyCondVar(gSleepWakeup);
    PR_DestroyCondVar(gWatchdogWakeup);
    PR_DestroyLock(gWatchdogLock);
}

static void
WatchdogMain(void *arg)
{
    JSRuntime *rt = (JSRuntime *) arg;

    PR_Lock(gWatchdogLock);
    while (gWatchdogThread) {
        PRIntervalTime now = PR_IntervalNow();
         if (gWatchdogHasTimeout && !IsBefore(now, gWatchdogTimeout)) {
            /*
             * The timeout has just expired. Trigger the operation callback
             * outside the lock.
             */
            gWatchdogHasTimeout = false;
            PR_Unlock(gWatchdogLock);
            CancelExecution(rt);
            PR_Lock(gWatchdogLock);

            /* Wake up any threads doing sleep. */
            PR_NotifyAllCondVar(gSleepWakeup);
        } else {
            PRIntervalTime sleepDuration = gWatchdogHasTimeout
                                           ? gWatchdogTimeout - now
                                           : PR_INTERVAL_NO_TIMEOUT;
#ifdef DEBUG
            PRStatus status =
#endif
                PR_WaitCondVar(gWatchdogWakeup, sleepDuration);
            JS_ASSERT(status == PR_SUCCESS);
        }
    }
    PR_Unlock(gWatchdogLock);
}

static bool
ScheduleWatchdog(JSRuntime *rt, jsdouble t)
{
    if (t <= 0) {
        PR_Lock(gWatchdogLock);
        gWatchdogHasTimeout = false;
        PR_Unlock(gWatchdogLock);
        return true;
    }

    PRIntervalTime interval = PRIntervalTime(ceil(t * PR_TicksPerSecond()));
    PRIntervalTime timeout = PR_IntervalNow() + interval;
    PR_Lock(gWatchdogLock);
    if (!gWatchdogThread) {
        JS_ASSERT(!gWatchdogHasTimeout);
        gWatchdogThread = PR_CreateThread(PR_USER_THREAD,
                                          WatchdogMain,
                                          rt,
                                          PR_PRIORITY_NORMAL,
                                          PR_LOCAL_THREAD,
                                          PR_JOINABLE_THREAD,
                                          0);
        if (!gWatchdogThread) {
            PR_Unlock(gWatchdogLock);
            return false;
        }
    } else if (!gWatchdogHasTimeout || IsBefore(timeout, gWatchdogTimeout)) {
         PR_NotifyCondVar(gWatchdogWakeup);
    }
    gWatchdogHasTimeout = true;
    gWatchdogTimeout = timeout;
    PR_Unlock(gWatchdogLock);
    return true;
}

#else /* !JS_THREADSAFE */

#ifdef XP_WIN
static HANDLE gTimerHandle = 0;

VOID CALLBACK
TimerCallback(PVOID lpParameter, BOOLEAN TimerOrWaitFired)
{
    CancelExecution((JSRuntime *) lpParameter);
}

#else

static void
AlarmHandler(int sig)
{
    CancelExecution(gRuntime);
}

#endif

static bool
InitWatchdog(JSRuntime *rt)
{
    gRuntime = rt;
    return true;
}

static void
KillWatchdog()
{
    ScheduleWatchdog(gRuntime, -1);
}

static bool
ScheduleWatchdog(JSRuntime *rt, jsdouble t)
{
#ifdef XP_WIN
    if (gTimerHandle) {
        DeleteTimerQueueTimer(NULL, gTimerHandle, NULL);
        gTimerHandle = 0;
    }
    if (t > 0 &&
        !CreateTimerQueueTimer(&gTimerHandle,
                               NULL,
                               (WAITORTIMERCALLBACK)TimerCallback,
                               rt,
                               DWORD(ceil(t * 1000.0)),
                               0,
                               WT_EXECUTEINTIMERTHREAD | WT_EXECUTEONLYONCE)) {
        gTimerHandle = 0;
        return false;
    }
#else
    /* FIXME: use setitimer when available for sub-second resolution. */
    if (t <= 0) {
        alarm(0);
        signal(SIGALRM, NULL);
    } else {
        signal(SIGALRM, AlarmHandler); /* set the Alarm signal capture */
        alarm(ceil(t));
    }
#endif
    return true;
}

#endif /* !JS_THREADSAFE */

static void
CancelExecution(JSRuntime *rt)
{
    gCanceled = true;
    if (gExitCode == 0)
        gExitCode = EXITCODE_TIMEOUT;
    JS_TriggerAllOperationCallbacks(rt);

    static const char msg[] = "Script runs for too long, terminating.\n";
#if defined(XP_UNIX) && !defined(JS_THREADSAFE)
    /* It is not safe to call fputs from signals. */
    write(2, msg, sizeof(msg) - 1);
#else
    fputs(msg, stderr);
#endif
}

static JSBool
SetTimeoutValue(JSContext *cx, jsdouble t)
{
    /* NB: The next condition also filter out NaNs. */
    if (!(t <= MAX_TIMEOUT_INTERVAL)) {
        JS_ReportError(cx, "Excessive timeout value");
        return JS_FALSE;
    }
    gTimeoutInterval = t;
    if (!ScheduleWatchdog(cx->runtime, t)) {
        JS_ReportError(cx, "Failed to create the watchdog");
        return JS_FALSE;
    }
    return JS_TRUE;
}

static JSBool
Timeout(JSContext *cx, uintN argc, jsval *vp)
{
    if (argc == 0)
        return JS_NewNumberValue(cx, gTimeoutInterval, vp);

    if (argc > 1) {
        JS_ReportError(cx, "Wrong number of arguments");
        return JS_FALSE;
    }

    jsdouble t;
    if (!JS_ValueToNumber(cx, JS_ARGV(cx, vp)[0], &t))
        return JS_FALSE;

    *vp = JSVAL_VOID;
    return SetTimeoutValue(cx, t);
}

static JSBool
Elapsed(JSContext *cx, uintN argc, jsval *vp)
{
    if (argc == 0) {
        double d = 0.0;
        JSShellContextData *data = GetContextData(cx);
        if (data)
            d = js_IntervalNow() - data->startTime;
        return JS_NewNumberValue(cx, d, vp);
    }
    JS_ReportError(cx, "Wrong number of arguments");
    return JS_FALSE;
}

#ifdef XP_UNIX

#include <fcntl.h>
#include <sys/stat.h>

/*
 * Returns a JS_malloc'd string (that the caller needs to JS_free)
 * containing the directory (non-leaf) part of |from| prepended to |leaf|.
 * If |from| is empty or a leaf, MakeAbsolutePathname returns a copy of leaf.
 * Returns NULL to indicate an error.
 */
static char *
MakeAbsolutePathname(JSContext *cx, const char *from, const char *leaf)
{
    size_t dirlen;
    char *dir;
    const char *slash = NULL, *cp;

    cp = from;
    while (*cp) {
        if (*cp == '/') {
            slash = cp;
        }

        ++cp;
    }

    if (!slash) {
        /* We were given a leaf or |from| was empty. */
        return JS_strdup(cx, leaf);
    }

    /* Else, we were given a real pathname, return that + the leaf. */
    dirlen = slash - from + 1;
    dir = (char*) JS_malloc(cx, dirlen + strlen(leaf) + 1);
    if (!dir)
        return NULL;

    strncpy(dir, from, dirlen);
    strcpy(dir + dirlen, leaf); /* Note: we can't use strcat here. */

    return dir;
}

#endif // XP_UNIX

static JSBool
Snarf(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSString *str;
    const char *filename;
    const char *pathname;
    JSStackFrame *fp;
    JSBool ok;
    size_t cc, len;
    char *buf;
    FILE *file;

    str = JS_ValueToString(cx, argv[0]);
    if (!str)
        return JS_FALSE;
    filename = JS_GetStringBytes(str);

    /* Get the currently executing script's name. */
    fp = JS_GetScriptedCaller(cx, NULL);
    JS_ASSERT(fp && fp->script->filename);
#ifdef XP_UNIX
    pathname = MakeAbsolutePathname(cx, fp->script->filename, filename);
    if (!pathname)
        return JS_FALSE;
#else
    pathname = filename;
#endif

    ok = JS_FALSE;
    len = 0;
    buf = NULL;
    file = fopen(pathname, "rb");
    if (!file) {
        JS_ReportError(cx, "can't open %s: %s", pathname, strerror(errno));
    } else {
        if (fseek(file, 0, SEEK_END) == EOF) {
            JS_ReportError(cx, "can't seek end of %s", pathname);
        } else {
            len = ftell(file);
            if (fseek(file, 0, SEEK_SET) == EOF) {
                JS_ReportError(cx, "can't seek start of %s", pathname);
            } else {
                buf = (char*) JS_malloc(cx, len + 1);
                if (buf) {
                    cc = fread(buf, 1, len, file);
                    if (cc != len) {
                        JS_ReportError(cx, "can't read %s: %s", pathname,
                                       (ptrdiff_t(cc) < 0) ? strerror(errno) : "short read");
                    } else {
                        len = (size_t)cc;
                        ok = JS_TRUE;
                    }
                }
            }
        }
        fclose(file);
    }
    JS_free(cx, (void*)pathname);
    if (!ok) {
        JS_free(cx, buf);
        return ok;
    }

    buf[len] = '\0';
    str = JS_NewString(cx, buf, len);
    if (!str) {
        JS_free(cx, buf);
        return JS_FALSE;
    }
    *rval = STRING_TO_JSVAL(str);
    return JS_TRUE;
}

/* We use a mix of JS_FS and JS_FN to test both kinds of natives. */
static JSFunctionSpec shell_functions[] = {
    JS_FS("version",        Version,        0,0,0),
    JS_FS("options",        Options,        0,0,0),
    JS_FS("load",           Load,           1,0,0),
    JS_FN("readline",       ReadLine,       0,0),
    JS_FN("print",          Print,          0,0),
    JS_FS("help",           Help,           0,0,0),
    JS_FS("quit",           Quit,           0,0,0),
    JS_FN("assertEq",       AssertEq,       2,0),
    JS_FN("gc",             ::GC,           0,0),
    JS_FN("gcparam",        GCParameter,    2,0),
    JS_FN("countHeap",      CountHeap,      0,0),
#ifdef JS_GC_ZEAL
    JS_FN("gczeal",         GCZeal,         1,0),
#endif
    JS_FS("trap",           Trap,           3,0,0),
    JS_FS("untrap",         Untrap,         2,0,0),
    JS_FS("line2pc",        LineToPC,       0,0,0),
    JS_FS("pc2line",        PCToLine,       0,0,0),
    JS_FN("stackQuota",     StackQuota,     0,0),
    JS_FS("stringsAreUTF8", StringsAreUTF8, 0,0,0),
    JS_FS("testUTF8",       TestUTF8,       1,0,0),
    JS_FS("throwError",     ThrowError,     0,0,0),
#ifdef DEBUG
    JS_FS("dis",            Disassemble,    1,0,0),
    JS_FS("disfile",        DisassFile,     1,0,0),
    JS_FS("dissrc",         DisassWithSrc,  1,0,0),
    JS_FN("dumpHeap",       DumpHeap,       0,0),
    JS_FS("notes",          Notes,          1,0,0),
    JS_FS("tracing",        Tracing,        0,0,0),
    JS_FS("stats",          DumpStats,      1,0,0),
#endif
#ifdef TEST_CVTARGS
    JS_FS("cvtargs",        ConvertArgs,    0,0,12),
#endif
    JS_FN("build",          BuildDate,      0,0),
    JS_FS("clear",          Clear,          0,0,0),
    JS_FN("intern",         Intern,         1,0),
    JS_FS("clone",          Clone,          1,0,0),
    JS_FS("seal",           Seal,           1,0,1),
    JS_FN("getpda",         GetPDA,         1,0),
    JS_FN("getslx",         GetSLX,         1,0),
    JS_FN("toint32",        ToInt32,        1,0),
    JS_FS("evalcx",         EvalInContext,  1,0,0),
    JS_FN("shapeOf",        ShapeOf,        1,0),
#ifdef MOZ_SHARK
    JS_FS("startShark",     js_StartShark,      0,0,0),
    JS_FS("stopShark",      js_StopShark,       0,0,0),
    JS_FS("connectShark",   js_ConnectShark,    0,0,0),
    JS_FS("disconnectShark",js_DisconnectShark, 0,0,0),
#endif
#ifdef MOZ_CALLGRIND
    JS_FS("startCallgrind", js_StartCallgrind,  0,0,0),
    JS_FS("stopCallgrind",  js_StopCallgrind,   0,0,0),
    JS_FS("dumpCallgrind",  js_DumpCallgrind,   1,0,0),
#endif
#ifdef MOZ_VTUNE
    JS_FS("startVtune",     js_StartVtune,    1,0,0),
    JS_FS("stopVtune",      js_StopVtune,     0,0,0),
    JS_FS("pauseVtune",     js_PauseVtune,    0,0,0),
    JS_FS("resumeVtune",    js_ResumeVtune,   0,0,0),
#endif
#ifdef MOZ_TRACEVIS
    JS_FS("startTraceVis",  js_StartTraceVis, 1,0,0),
    JS_FS("stopTraceVis",   js_StopTraceVis,  0,0,0),
#endif
#ifdef DEBUG_ARRAYS
    JS_FS("arrayInfo",      js_ArrayInfo,       1,0,0),
#endif
#ifdef JS_THREADSAFE
    JS_FN("sleep",          Sleep_fn,       1,0),
    JS_FN("scatter",        Scatter,        1,0),
#endif
    JS_FS("snarf",          Snarf,        0,0,0),
    JS_FN("timeout",        Timeout,        1,0),
    JS_FN("elapsed",        Elapsed,        0,0),
    JS_FS_END
};

static const char shell_help_header[] =
"Command                  Description\n"
"=======                  ===========\n";

static const char *const shell_help_messages[] = {
"version([number])        Get or set JavaScript version number",
"options([option ...])    Get or toggle JavaScript options",
"load(['foo.js' ...])     Load files named by string arguments",
"readline()               Read a single line from stdin",
"print([exp ...])         Evaluate and print expressions",
"help([name ...])         Display usage and help messages",
"quit()                   Quit the shell",
"assertEq(actual, expected[, msg])\n"
"  Throw if the first two arguments are not the same (both +0 or both -0,\n"
"  both NaN, or non-zero and ===)",
"gc()                     Run the garbage collector",
"gcparam(name, value)\n"
"  Wrapper for JS_SetGCParameter. The name must be either 'maxBytes' or\n"
"  'maxMallocBytes' and the value must be convertable to a positive uint32",
"countHeap([start[, kind]])\n"
"  Count the number of live GC things in the heap or things reachable from\n"
"  start when it is given and is not null. kind is either 'all' (default) to\n"
"  count all things or one of 'object', 'double', 'string', 'function',\n"
"  'qname', 'namespace', 'xml' to count only things of that kind",
#ifdef JS_GC_ZEAL
"gczeal(level)            How zealous the garbage collector should be",
#endif
"trap([fun, [pc,]] exp)   Trap bytecode execution",
"untrap(fun[, pc])        Remove a trap",
"line2pc([fun,] line)     Map line number to PC",
"pc2line(fun[, pc])       Map PC to line number",
"stackQuota([number])     Query/set script stack quota",
"stringsAreUTF8()         Check if strings are UTF-8 encoded",
"testUTF8(mode)           Perform UTF-8 tests (modes are 1 to 4)",
"throwError()             Throw an error from JS_ReportError",
#ifdef DEBUG
"dis([fun])               Disassemble functions into bytecodes",
"disfile('foo.js')        Disassemble script file into bytecodes",
"dissrc([fun])            Disassemble functions with source lines",
"dumpHeap([fileName[, start[, toFind[, maxDepth[, toIgnore]]]]])\n"
"  Interface to JS_DumpHeap with output sent to file",
"notes([fun])             Show source notes for functions",
"tracing([true|false|filename]) Turn bytecode execution tracing on/off.\n"
"                         With filename, send to file.\n",
"stats([string ...])      Dump 'arena', 'atom', 'global' stats",
#endif
#ifdef TEST_CVTARGS
"cvtargs(arg1..., arg12)  Test argument formatter",
#endif
"build()                  Show build date and time",
"clear([obj])             Clear properties of object",
"intern(str)              Internalize str in the atom table",
"clone(fun[, scope])      Clone function object",
"seal(obj[, deep])        Seal object, or object graph if deep",
"getpda(obj)              Get the property descriptors for obj",
"getslx(obj)              Get script line extent",
"toint32(n)               Testing hook for JS_ValueToInt32",
"evalcx(s[, o])\n"
"  Evaluate s in optional sandbox object o\n"
"  if (s == '' && !o) return new o with eager standard classes\n"
"  if (s == 'lazy' && !o) return new o with lazy standard classes",
"shapeOf(obj)             Get the shape of obj (an implementation detail)",
#ifdef MOZ_SHARK
"startShark()             Start a Shark session.\n"
"                         Shark must be running with programatic sampling",
"stopShark()              Stop a running Shark session",
"connectShark()           Connect to Shark.\n"
"                         The -k switch does this automatically",
"disconnectShark()        Disconnect from Shark",
#endif
#ifdef MOZ_CALLGRIND
"startCallgrind()         Start callgrind instrumentation",
"stopCallgrind()          Stop callgrind instrumentation",
"dumpCallgrind([name])    Dump callgrind counters",
#endif
#ifdef MOZ_VTUNE
"startVtune([filename])   Start vtune instrumentation",
"stopVtune()              Stop vtune instrumentation",
"pauseVtune()             Pause vtune collection",
"resumeVtune()            Resume vtune collection",
#endif
#ifdef MOZ_TRACEVIS
"startTraceVis(filename)  Start TraceVis recording (stops any current recording)",
"stopTraceVis()           Stop TraceVis recording",
#endif
#ifdef DEBUG_ARRAYS
"arrayInfo(a1, a2, ...)   Report statistics about arrays",
#endif
#ifdef JS_THREADSAFE
"sleep(dt)                Sleep for dt seconds",
"scatter(fns)             Call functions concurrently (ignoring errors)",
#endif
"snarf(filename)          Read filename into returned string",
"timeout([seconds])\n"
"  Get/Set the limit in seconds for the execution time for the current context.\n"
"  A negative value (default) means that the execution time is unlimited.",
"elapsed()                Execution time elapsed for the current context.\n",
};

/* Help messages must match shell functions. */
JS_STATIC_ASSERT(JS_ARRAY_LENGTH(shell_help_messages) + 1 ==
                 JS_ARRAY_LENGTH(shell_functions));

#ifdef DEBUG
static void
CheckHelpMessages()
{
    const char *const *m;
    const char *lp;

    /* Each message must begin with "function_name(" prefix. */
    for (m = shell_help_messages; m != JS_ARRAY_END(shell_help_messages); ++m) {
        lp = strchr(*m, '(');
        JS_ASSERT(lp);
        JS_ASSERT(memcmp(shell_functions[m - shell_help_messages].name,
                         *m, lp - *m) == 0);
    }
}
#else
# define CheckHelpMessages() ((void) 0)
#endif

static JSBool
Help(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    uintN i, j;
    int did_header, did_something;
    JSType type;
    JSFunction *fun;
    JSString *str;
    const char *bytes;

    fprintf(gOutFile, "%s\n", JS_GetImplementationVersion());
    if (argc == 0) {
        fputs(shell_help_header, gOutFile);
        for (i = 0; shell_functions[i].name; i++)
            fprintf(gOutFile, "%s\n", shell_help_messages[i]);
    } else {
        did_header = 0;
        for (i = 0; i < argc; i++) {
            did_something = 0;
            type = JS_TypeOfValue(cx, argv[i]);
            if (type == JSTYPE_FUNCTION) {
                fun = JS_ValueToFunction(cx, argv[i]);
                str = fun->atom ? ATOM_TO_STRING(fun->atom) : NULL;
            } else if (type == JSTYPE_STRING) {
                str = JSVAL_TO_STRING(argv[i]);
            } else {
                str = NULL;
            }
            if (str) {
                bytes = JS_GetStringBytes(str);
                for (j = 0; shell_functions[j].name; j++) {
                    if (!strcmp(bytes, shell_functions[j].name)) {
                        if (!did_header) {
                            did_header = 1;
                            fputs(shell_help_header, gOutFile);
                        }
                        did_something = 1;
                        fprintf(gOutFile, "%s\n", shell_help_messages[j]);
                        break;
                    }
                }
            }
            if (!did_something) {
                str = JS_ValueToString(cx, argv[i]);
                if (!str)
                    return JS_FALSE;
                fprintf(gErrFile, "Sorry, no help for %s\n",
                        JS_GetStringBytes(str));
            }
        }
    }
    return JS_TRUE;
}

static JSObject *
split_setup(JSContext *cx)
{
    JSObject *outer, *inner, *arguments;

    outer = split_create_outer(cx);
    if (!outer)
        return NULL;
    JS_SetGlobalObject(cx, outer);

    inner = split_create_inner(cx, outer);
    if (!inner)
        return NULL;

    if (!JS_DefineFunctions(cx, inner, shell_functions))
        return NULL;
    JS_ClearScope(cx, outer);

    /* Create a dummy arguments object. */
    arguments = JS_NewArrayObject(cx, 0, NULL);
    if (!arguments ||
        !JS_DefineProperty(cx, inner, "arguments", OBJECT_TO_JSVAL(arguments),
                           NULL, NULL, 0)) {
        return NULL;
    }

#ifndef LAZY_STANDARD_CLASSES
    if (!JS_InitStandardClasses(cx, inner))
        return NULL;
#endif

    return inner;
}

/*
 * Define a JS object called "it".  Give it class operations that printf why
 * they're being called for tutorial purposes.
 */
enum its_tinyid {
    ITS_COLOR, ITS_HEIGHT, ITS_WIDTH, ITS_FUNNY, ITS_ARRAY, ITS_RDONLY,
    ITS_CUSTOM, ITS_CUSTOMRDONLY
};

static JSBool
its_getter(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
  jsval *val = (jsval *) JS_GetPrivate(cx, obj);
  *vp = val ? *val : JSVAL_VOID;
  return JS_TRUE;
}

static JSBool
its_setter(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
  jsval *val = (jsval *) JS_GetPrivate(cx, obj);
  if (val) {
      *val = *vp;
      return JS_TRUE;
  }

  val = new jsval;
  if (!val) {
      JS_ReportOutOfMemory(cx);
      return JS_FALSE;
  }

  if (!JS_AddRoot(cx, val)) {
      delete val;
      return JS_FALSE;
  }

  if (!JS_SetPrivate(cx, obj, (void*)val)) {
      JS_RemoveRoot(cx, val);
      delete val;
      return JS_FALSE;
  }

  *val = *vp;
  return JS_TRUE;
}

static JSPropertySpec its_props[] = {
    {"color",           ITS_COLOR,      JSPROP_ENUMERATE,       NULL, NULL},
    {"height",          ITS_HEIGHT,     JSPROP_ENUMERATE,       NULL, NULL},
    {"width",           ITS_WIDTH,      JSPROP_ENUMERATE,       NULL, NULL},
    {"funny",           ITS_FUNNY,      JSPROP_ENUMERATE,       NULL, NULL},
    {"array",           ITS_ARRAY,      JSPROP_ENUMERATE,       NULL, NULL},
    {"rdonly",          ITS_RDONLY,     JSPROP_READONLY,        NULL, NULL},
    {"custom",          ITS_CUSTOM,     JSPROP_ENUMERATE,
                        its_getter,     its_setter},
    {"customRdOnly",    ITS_CUSTOMRDONLY, JSPROP_ENUMERATE | JSPROP_READONLY,
                        its_getter,     its_setter},
    {NULL,0,0,NULL,NULL}
};

static JSBool
its_bindMethod(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
               jsval *rval)
{
    char *name;
    JSObject *method;

    if (!JS_ConvertArguments(cx, argc, argv, "so", &name, &method))
        return JS_FALSE;

    *rval = OBJECT_TO_JSVAL(method);

    if (JS_TypeOfValue(cx, *rval) != JSTYPE_FUNCTION) {
        JSString *valstr = JS_ValueToString(cx, *rval);
        if (valstr) {
            JS_ReportError(cx, "can't bind method %s to non-callable object %s",
                           name, JS_GetStringBytes(valstr));
        }
        return JS_FALSE;
    }

    if (!JS_DefineProperty(cx, obj, name, *rval, NULL, NULL, JSPROP_ENUMERATE))
        return JS_FALSE;

    return JS_SetParent(cx, method, obj);
}

static JSFunctionSpec its_methods[] = {
    {"bindMethod",      its_bindMethod, 2,0,0},
    {NULL,NULL,0,0,0}
};

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

static JSBool its_noisy;    /* whether to be noisy when finalizing it */
static JSBool its_enum_fail;/* whether to fail when enumerating it */

static JSBool
its_addProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    if (!its_noisy)
        return JS_TRUE;

    ToString idString(cx, id);
    fprintf(gOutFile, "adding its property %s,", idString.getBytes());
    ToString valueString(cx, *vp);
    fprintf(gOutFile, " initial value %s\n", valueString.getBytes());
    return JS_TRUE;
}

static JSBool
its_delProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    if (!its_noisy)
        return JS_TRUE;

    ToString idString(cx, id);
    fprintf(gOutFile, "deleting its property %s,", idString.getBytes());
    ToString valueString(cx, *vp);
    fprintf(gOutFile, " initial value %s\n", valueString.getBytes());
    return JS_TRUE;
}

static JSBool
its_getProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    if (!its_noisy)
        return JS_TRUE;

    ToString idString(cx, id);
    fprintf(gOutFile, "getting its property %s,", idString.getBytes());
    ToString valueString(cx, *vp);
    fprintf(gOutFile, " initial value %s\n", valueString.getBytes());
    return JS_TRUE;
}

static JSBool
its_setProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    ToString idString(cx, id);
    if (its_noisy) {
        fprintf(gOutFile, "setting its property %s,", idString.getBytes());
        ToString valueString(cx, *vp);
        fprintf(gOutFile, " new value %s\n", valueString.getBytes());
    }

    if (!JSVAL_IS_STRING(id))
        return JS_TRUE;

    if (!strcmp(idString.getBytes(), "noisy"))
        JS_ValueToBoolean(cx, *vp, &its_noisy);
    else if (!strcmp(idString.getBytes(), "enum_fail"))
        JS_ValueToBoolean(cx, *vp, &its_enum_fail);

    return JS_TRUE;
}

/*
 * Its enumerator, implemented using the "new" enumerate API,
 * see class flags.
 */
static JSBool
its_enumerate(JSContext *cx, JSObject *obj, JSIterateOp enum_op,
              jsval *statep, jsid *idp)
{
    JSObject *iterator;

    switch (enum_op) {
      case JSENUMERATE_INIT:
        if (its_noisy)
            fprintf(gOutFile, "enumerate its properties\n");

        iterator = JS_NewPropertyIterator(cx, obj);
        if (!iterator)
            return JS_FALSE;

        *statep = OBJECT_TO_JSVAL(iterator);
        if (idp)
            *idp = JSVAL_ZERO;
        break;

      case JSENUMERATE_NEXT:
        if (its_enum_fail) {
            JS_ReportError(cx, "its enumeration failed");
            return JS_FALSE;
        }

        iterator = (JSObject *) JSVAL_TO_OBJECT(*statep);
        if (!JS_NextProperty(cx, iterator, idp))
            return JS_FALSE;

        if (!JSVAL_IS_VOID(*idp))
            break;
        /* Fall through. */

      case JSENUMERATE_DESTROY:
        /* Allow our iterator object to be GC'd. */
        *statep = JSVAL_NULL;
        break;
    }

    return JS_TRUE;
}

static JSBool
its_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
            JSObject **objp)
{
    if (its_noisy) {
        ToString idString(cx, id);
        fprintf(gOutFile, "resolving its property %s, flags {%s,%s,%s}\n",
               idString.getBytes(),
               (flags & JSRESOLVE_QUALIFIED) ? "qualified" : "",
               (flags & JSRESOLVE_ASSIGNING) ? "assigning" : "",
               (flags & JSRESOLVE_DETECTING) ? "detecting" : "");
    }
    return JS_TRUE;
}

static JSBool
its_convert(JSContext *cx, JSObject *obj, JSType type, jsval *vp)
{
    if (its_noisy)
        fprintf(gOutFile, "converting it to %s type\n", JS_GetTypeName(cx, type));
    return JS_TRUE;
}

static void
its_finalize(JSContext *cx, JSObject *obj)
{
    jsval *rootedVal;
    if (its_noisy)
        fprintf(gOutFile, "finalizing it\n");
    rootedVal = (jsval *) JS_GetPrivate(cx, obj);
    if (rootedVal) {
      JS_RemoveRoot(cx, rootedVal);
      JS_SetPrivate(cx, obj, NULL);
      delete rootedVal;
    }
}

static JSClass its_class = {
    "It", JSCLASS_NEW_RESOLVE | JSCLASS_NEW_ENUMERATE | JSCLASS_HAS_PRIVATE,
    its_addProperty,  its_delProperty,  its_getProperty,  its_setProperty,
    (JSEnumerateOp)its_enumerate, (JSResolveOp)its_resolve,
    its_convert,      its_finalize,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

JSErrorFormatString jsShell_ErrorFormatString[JSErr_Limit] = {
#define MSG_DEF(name, number, count, exception, format) \
    { format, count, JSEXN_ERR } ,
#include "jsshell.msg"
#undef MSG_DEF
};

static const JSErrorFormatString *
my_GetErrorMessage(void *userRef, const char *locale, const uintN errorNumber)
{
    if ((errorNumber > 0) && (errorNumber < JSShellErr_Limit))
        return &jsShell_ErrorFormatString[errorNumber];
    return NULL;
}

static void
my_ErrorReporter(JSContext *cx, const char *message, JSErrorReport *report)
{
    int i, j, k, n;
    char *prefix, *tmp;
    const char *ctmp;

    if (!report) {
        fprintf(gErrFile, "%s\n", message);
        return;
    }

    /* Conditionally ignore reported warnings. */
    if (JSREPORT_IS_WARNING(report->flags) && !reportWarnings)
        return;

    prefix = NULL;
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
        if (prefix)
            fputs(prefix, gErrFile);
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

    /* report->linebuf usually ends with a newline. */
    n = strlen(report->linebuf);
    fprintf(gErrFile, ":\n%s%s%s%s",
            prefix,
            report->linebuf,
            (n > 0 && report->linebuf[n-1] == '\n') ? "" : "\n",
            prefix);
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
    if (!JSREPORT_IS_WARNING(report->flags)) {
        if (report->errorNumber == JSMSG_OUT_OF_MEMORY) {
            gExitCode = EXITCODE_OUT_OF_MEMORY;
        } else {
            gExitCode = EXITCODE_RUNTIME_ERROR;
        }
    }
    JS_free(cx, prefix);
}

#if defined(SHELL_HACK) && defined(DEBUG) && defined(XP_UNIX)
static JSBool
Exec(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSFunction *fun;
    const char *name, **nargv;
    uintN i, nargc;
    JSString *str;
    pid_t pid;
    int status;

    fun = JS_ValueToFunction(cx, argv[-2]);
    if (!fun)
        return JS_FALSE;
    if (!fun->atom)
        return JS_TRUE;
    name = JS_GetStringBytes(ATOM_TO_STRING(fun->atom));
    nargc = 1 + argc;
    nargv = JS_malloc(cx, (nargc + 1) * sizeof(char *));
    if (!nargv)
        return JS_FALSE;
    nargv[0] = name;
    for (i = 1; i < nargc; i++) {
        str = JS_ValueToString(cx, argv[i-1]);
        if (!str) {
            JS_free(cx, nargv);
            return JS_FALSE;
        }
        nargv[i] = JS_GetStringBytes(str);
    }
    nargv[nargc] = 0;
    pid = fork();
    switch (pid) {
      case -1:
        perror("js");
        break;
      case 0:
        (void) execvp(name, (char **)nargv);
        perror("js");
        exit(127);
      default:
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            continue;
        break;
    }
    JS_free(cx, nargv);
    return JS_TRUE;
}
#endif

static JSBool
global_enumerate(JSContext *cx, JSObject *obj)
{
#ifdef LAZY_STANDARD_CLASSES
    return JS_EnumerateStandardClasses(cx, obj);
#else
    return JS_TRUE;
#endif
}

static JSBool
global_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
               JSObject **objp)
{
#ifdef LAZY_STANDARD_CLASSES
    if ((flags & JSRESOLVE_ASSIGNING) == 0) {
        JSBool resolved;

        if (!JS_ResolveStandardClass(cx, obj, id, &resolved))
            return JS_FALSE;
        if (resolved) {
            *objp = obj;
            return JS_TRUE;
        }
    }
#endif

#if defined(SHELL_HACK) && defined(DEBUG) && defined(XP_UNIX)
    if ((flags & (JSRESOLVE_QUALIFIED | JSRESOLVE_ASSIGNING)) == 0) {
        /*
         * Do this expensive hack only for unoptimized Unix builds, which are
         * not used for benchmarking.
         */
        char *path, *comp, *full;
        const char *name;
        JSBool ok, found;
        JSFunction *fun;

        if (!JSVAL_IS_STRING(id))
            return JS_TRUE;
        path = getenv("PATH");
        if (!path)
            return JS_TRUE;
        path = JS_strdup(cx, path);
        if (!path)
            return JS_FALSE;
        name = JS_GetStringBytes(JSVAL_TO_STRING(id));
        ok = JS_TRUE;
        for (comp = strtok(path, ":"); comp; comp = strtok(NULL, ":")) {
            if (*comp != '\0') {
                full = JS_smprintf("%s/%s", comp, name);
                if (!full) {
                    JS_ReportOutOfMemory(cx);
                    ok = JS_FALSE;
                    break;
                }
            } else {
                full = (char *)name;
            }
            found = (access(full, X_OK) == 0);
            if (*comp != '\0')
                free(full);
            if (found) {
                fun = JS_DefineFunction(cx, obj, name, Exec, 0,
                                        JSPROP_ENUMERATE);
                ok = (fun != NULL);
                if (ok)
                    *objp = obj;
                break;
            }
        }
        JS_free(cx, path);
        return ok;
    }
#else
    return JS_TRUE;
#endif
}

JSClass global_class = {
    "global", JSCLASS_NEW_RESOLVE | JSCLASS_GLOBAL_FLAGS | JSCLASS_HAS_PRIVATE,
    JS_PropertyStub,  JS_PropertyStub,
    JS_PropertyStub,  JS_PropertyStub,
    global_enumerate, (JSResolveOp) global_resolve,
    JS_ConvertStub,   its_finalize,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

static JSBool
env_setProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
/* XXX porting may be easy, but these don't seem to supply setenv by default */
#if !defined XP_BEOS && !defined XP_OS2 && !defined SOLARIS
    int rv;

    ToString idstr(cx, id, JS_TRUE);
    if (idstr.threw())
        return JS_FALSE;
    ToString valstr(cx, *vp, JS_TRUE);
    if (valstr.threw())
        return JS_FALSE;
#if defined XP_WIN || defined HPUX || defined OSF1 || defined IRIX
    {
        char *waste = JS_smprintf("%s=%s", idstr.getBytes(), valstr.getBytes());
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
        JS_smprintf_free(waste);
#endif
    }
#else
    rv = setenv(idstr.getBytes(), valstr.getBytes(), 1);
#endif
    if (rv < 0) {
        JS_ReportError(cx, "can't set env variable %s to %s", idstr.getBytes(), valstr.getBytes());
        return JS_FALSE;
    }
    *vp = valstr.getJSVal();
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
    JSString *valstr;
    const char *name, *value;

    if (flags & JSRESOLVE_ASSIGNING)
        return JS_TRUE;

    ToString idstr(cx, id, JS_TRUE);
    if (idstr.threw())
        return JS_FALSE;

    name = idstr.getBytes();
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
    JS_ConvertStub,   NULL,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

#ifdef NARCISSUS

static JSBool
defineProperty(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
               jsval *rval)
{
    JSString *str;
    jsval value;
    JSBool dontDelete, readOnly, dontEnum;
    const jschar *chars;
    size_t length;
    uintN attrs;

    dontDelete = readOnly = dontEnum = JS_FALSE;
    if (!JS_ConvertArguments(cx, argc, argv, "Sv/bbb",
                             &str, &value, &dontDelete, &readOnly, &dontEnum)) {
        return JS_FALSE;
    }
    chars = JS_GetStringChars(str);
    length = JS_GetStringLength(str);
    attrs = dontEnum ? 0 : JSPROP_ENUMERATE;
    if (dontDelete)
        attrs |= JSPROP_PERMANENT;
    if (readOnly)
        attrs |= JSPROP_READONLY;
    return JS_DefineUCProperty(cx, obj, chars, length, value, NULL, NULL,
                               attrs);
}

static JSBool
Evaluate(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    /* function evaluate(source, filename, lineno) { ... } */
    JSString *source;
    const char *filename = "";
    jsuint lineno = 0;
    uint32 oldopts;
    JSBool ok;

    if (argc == 0) {
        *rval = JSVAL_VOID;
        return JS_TRUE;
    }

    if (!JS_ConvertArguments(cx, argc, argv, "S/su",
                             &source, &filename, &lineno)) {
        return JS_FALSE;
    }

    oldopts = JS_GetOptions(cx);
    JS_SetOptions(cx, oldopts | JSOPTION_COMPILE_N_GO);
    ok = JS_EvaluateUCScript(cx, obj, JS_GetStringChars(source),
                             JS_GetStringLength(source), filename,
                             lineno, rval);
    JS_SetOptions(cx, oldopts);

    return ok;
}

#endif /* NARCISSUS */

static JSBool
ContextCallback(JSContext *cx, uintN contextOp)
{
    JSShellContextData *data;
    
    switch (contextOp) {
      case JSCONTEXT_NEW: {
        data = NewContextData();
        if (!data)
            return JS_FALSE;
        JS_SetContextPrivate(cx, data);
        JS_SetErrorReporter(cx, my_ErrorReporter);
        JS_SetVersion(cx, JSVERSION_LATEST);
        SetContextOptions(cx);
        if (enableJit)
            JS_ToggleOptions(cx, JSOPTION_JIT);
        break;
      case JSCONTEXT_DESTROY:
        data = GetContextData(cx);
        JS_SetContextPrivate(cx, NULL);
        free(data);
        break;
      }

      default:
        break;
    }
    return JS_TRUE;
}

int
main(int argc, char **argv, char **envp)
{
    int stackDummy;
    JSRuntime *rt;
    JSContext *cx;
    JSObject *glob, *it, *envobj;
    int result;
#ifdef JSDEBUGGER
    JSDContext *jsdc;
#ifdef JSDEBUGGER_JAVA_UI
    JNIEnv *java_env;
    JSDJContext *jsdjc;
#endif
#ifdef JSDEBUGGER_C_UI
    JSBool jsdbc;
#endif /* JSDEBUGGER_C_UI */
#endif /* JSDEBUGGER */

    CheckHelpMessages();
#ifdef HAVE_SETLOCALE
    setlocale(LC_ALL, "");
#endif

#ifdef JS_THREADSAFE
    if (PR_FAILURE == PR_NewThreadPrivateIndex(&gStackBaseThreadIndex, NULL) ||
        PR_FAILURE == PR_SetThreadPrivate(gStackBaseThreadIndex, &stackDummy)) {
        return 1;
    }
#else
    gStackBase = (jsuword) &stackDummy;
#endif

#ifdef XP_OS2
   /* these streams are normally line buffered on OS/2 and need a \n, *
    * so we need to unbuffer then to get a reasonable prompt          */
    setbuf(stdout,0);
    setbuf(stderr,0);
#endif

    gErrFile = stderr;
    gOutFile = stdout;

    argc--;
    argv++;

    rt = JS_NewRuntime(64L * 1024L * 1024L);
    if (!rt)
        return 1;

    if (!InitWatchdog(rt))
        return 1;

    JS_SetContextCallback(rt, ContextCallback);

    WITH_LOCKED_CONTEXT_LIST(
        cx = JS_NewContext(rt, gStackChunkSize)
    );
    if (!cx)
        return 1;

    JS_SetGCParameterForThread(cx, JSGC_MAX_CODE_CACHE_BYTES, 16 * 1024 * 1024);

    JS_BeginRequest(cx);

    glob = JS_NewObject(cx, &global_class, NULL, NULL);
    if (!glob)
        return 1;
#ifdef LAZY_STANDARD_CLASSES
    JS_SetGlobalObject(cx, glob);
#else
    if (!JS_InitStandardClasses(cx, glob))
        return 1;
#endif
    if (!JS_DefineFunctions(cx, glob, shell_functions))
        return 1;

    it = JS_DefineObject(cx, glob, "it", &its_class, NULL, 0);
    if (!it)
        return 1;
    if (!JS_DefineProperties(cx, it, its_props))
        return 1;
    if (!JS_DefineFunctions(cx, it, its_methods))
        return 1;

    if (!JS_DefineProperty(cx, glob, "custom", JSVAL_VOID, its_getter,
                           its_setter, 0))
        return 1;
    if (!JS_DefineProperty(cx, glob, "customRdOnly", JSVAL_VOID, its_getter,
                           its_setter, JSPROP_READONLY))
        return 1;

#ifdef JSDEBUGGER
    /*
    * XXX A command line option to enable debugging (or not) would be good
    */
    jsdc = JSD_DebuggerOnForUser(rt, NULL, NULL);
    if (!jsdc)
        return 1;
    JSD_JSContextInUse(jsdc, cx);
#ifdef JSD_LOWLEVEL_SOURCE
    JS_SetSourceHandler(rt, SendSourceToJSDebugger, jsdc);
#endif /* JSD_LOWLEVEL_SOURCE */
#ifdef JSDEBUGGER_JAVA_UI
    jsdjc = JSDJ_CreateContext();
    if (! jsdjc)
        return 1;
    JSDJ_SetJSDContext(jsdjc, jsdc);
    java_env = JSDJ_CreateJavaVMAndStartDebugger(jsdjc);
    /*
    * XXX This would be the place to wait for the debugger to start.
    * Waiting would be nice in general, but especially when a js file
    * is passed on the cmd line.
    */
#endif /* JSDEBUGGER_JAVA_UI */
#ifdef JSDEBUGGER_C_UI
    jsdbc = JSDB_InitDebugger(rt, jsdc, 0);
#endif /* JSDEBUGGER_C_UI */
#endif /* JSDEBUGGER */

    envobj = JS_DefineObject(cx, glob, "environment", &env_class, NULL, 0);
    if (!envobj || !JS_SetPrivate(cx, envobj, envp))
        return 1;

#ifdef NARCISSUS
    {
        jsval v;
        static const char Object_prototype[] = "Object.prototype";

        if (!JS_DefineFunction(cx, glob, "evaluate", Evaluate, 3, 0))
            return 1;

        if (!JS_EvaluateScript(cx, glob,
                               Object_prototype, sizeof Object_prototype - 1,
                               NULL, 0, &v)) {
            return 1;
        }
        if (!JS_DefineFunction(cx, JSVAL_TO_OBJECT(v), "__defineProperty__",
                               defineProperty, 5, 0)) {
            return 1;
        }
    }
#endif

    result = ProcessArgs(cx, glob, argv, argc);

#ifdef JSDEBUGGER
    if (jsdc) {
#ifdef JSDEBUGGER_C_UI
        if (jsdbc)
            JSDB_TermDebugger(jsdc);
#endif /* JSDEBUGGER_C_UI */
        JSD_DebuggerOff(jsdc);
    }
#endif  /* JSDEBUGGER */

    JS_EndRequest(cx);

    JS_CommenceRuntimeShutDown(rt);

    WITH_LOCKED_CONTEXT_LIST( 
        JS_DestroyContext(cx)
    );

    KillWatchdog();

    JS_DestroyRuntime(rt);
    JS_ShutDown();
    return result;
}
