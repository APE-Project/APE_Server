/* (c) Anthony Catel <a.catel@weelya.com> - Weelya - 2009 */
/* Javascript plugins support using spidermonkey API */

#define XP_UNIX
#define JS_THREADSAFE
#include "../../js/src/jsapi.h"

#include <stdio.h>
#include <glob.h>
#include "plugins.h"
#include "global_plugins.h"

#define MODULE_NAME "spidermonkey"

/* Return the global SpiderMonkey Runtime instance e.g. ASMR->runtime */
#define ASMR ((ape_sm_runtime *)get_property(g_ape->properties, "sm_runtime")->val)

static ace_plugin_infos infos_module = {
	"Javascript embeded", 	// Module Name
	"0.01",			// Module Version
	"Anthony Catel",	// Module Author
	NULL			// Config file
};


typedef struct _ape_sm_compiled ape_sm_compiled;
struct _ape_sm_compiled {
	char *filename;
	
	JSScript *bytecode;
	JSContext *cx;
	JSObject *global;
	
	struct _ape_sm_compiled *next;
};

typedef struct _ape_sm_runtime ape_sm_runtime;
struct _ape_sm_runtime {
	JSRuntime *runtime;
	
	ape_sm_compiled *scripts;
};


/* Reporting error from JS compilation (parse error, etc...) */
static void reportError(JSContext *cx, const char *message, JSErrorReport *report)
{
    fprintf(stderr, "%s:%u:%s\n",
            report->filename ? report->filename : "<no filename>",
            (unsigned int) report->lineno,
            message);
}


static void init_module(acetables *g_ape) // Called when module is loaded
{
	JSRuntime *rt;
	ape_sm_runtime *asr;
	int i;
	
	glob_t globbuf;

	JSClass global_class = {
		"global", JSCLASS_GLOBAL_FLAGS,
		    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
		    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
		    JSCLASS_NO_OPTIONAL_MEMBERS

	};
	
	rt = JS_NewRuntime(8L * 1024L * 1024L); // 8 Mio allocated
	
	if (rt == NULL) {
		printf("[ERR] Not enougth memory\n");
		exit(0);
	}
	asr = xmalloc(sizeof(*asr));
	asr->runtime = rt;
	asr->scripts = NULL;
	
	add_property(&g_ape->properties, "sm_runtime", asr);

	glob("./scripts/*.ape.js", 0, NULL, &globbuf);
	
	for (i = 0; i < globbuf.gl_pathc; i++) {
		ape_sm_compiled *asc = xmalloc(sizeof(*asc));
		
		asc->filename = (void *)xstrdup(globbuf.gl_pathv[i]);
		asc->cx = JS_NewContext(rt, 8192);
		
		JS_BeginRequest(asc->cx);
			JS_SetOptions(asc->cx, JSOPTION_VAROBJFIX);
			JS_SetVersion(asc->cx, JSVERSION_LATEST);
			JS_SetErrorReporter(asc->cx, reportError);
			asc->global = JS_NewObject(asc->cx, &global_class, NULL, NULL);
			JS_InitStandardClasses(asc->cx, asc->global);
			asc->bytecode = JS_CompileFile(asc->cx, asc->global, asc->filename);
		JS_EndRequest(asc->cx);
		
		if (asc->bytecode == NULL) {
			/* cleaning memory */
		} else {
			asc->next = asr->scripts;
			asr->scripts = asc;
		}
	}
	globfree(&globbuf);
}

static ace_callbacks callbacks = {
	NULL,				/* Called when new user is added */
	NULL,				/* Called when a user is disconnected */
	NULL,				/* Called when new chan is created */
	NULL,				/* Called when a user join a channel */
	NULL				/* Called when a user leave a channel */
};

APE_INIT_PLUGIN(MODULE_NAME, init_module, callbacks)

