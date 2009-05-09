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
#define APE_JS_NATIVE(func_name) static JSBool func_name(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)


typedef struct _ape_sm_compiled ape_sm_compiled;
struct _ape_sm_compiled {
	char *filename;
	
	JSScript *bytecode;
	JSContext *cx;
	JSObject *global;
	JSObject *scriptObj;
	
	struct _ape_sm_compiled *next;
};

typedef struct _ape_sm_runtime ape_sm_runtime;
struct _ape_sm_runtime {
	JSRuntime *runtime;
	
	ape_sm_compiled *scripts;
};


static ace_plugin_infos infos_module = {
	"Javascript embeded", 	// Module Name
	"0.01",			// Module Version
	"Anthony Catel",	// Module Author
	NULL			// Config file
};


/* Standard javascript object */
static JSClass global_class = {
	"global", JSCLASS_GLOBAL_FLAGS,
	    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
	    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
	    JSCLASS_NO_OPTIONAL_MEMBERS

};

/* The main Ape Object (global) */
static JSClass ape_class = {
	"Ape", 0,
	    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
	    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
	    JSCLASS_NO_OPTIONAL_MEMBERS
};

/* Reporting error from JS compilation (parse error, etc...) */
static void reportError(JSContext *cx, const char *message, JSErrorReport *report)
{
    fprintf(stderr, "%s:%u:%s\n",
            report->filename ? report->filename : "<no filename>",
            (unsigned int) report->lineno,
            message);
}



APE_JS_NATIVE(ape_sm_addEvent)
{
	char event[32];
	
	*rval = JSVAL_NULL;	
	if (argc != 2) {
		return JS_TRUE;
	}
	
	if (JS_ConvertArguments(cx, argc-1, argv, "s", event) == JS_FALSE) {
		printf("Cannot convert...\n");
		return JS_TRUE;
	}
	
	printf("Event : %s\n", event);
	
	return JS_TRUE;
}


static void ape_sm_define_ape(ape_sm_compiled *asc)
{
	JSObject *obj;

	obj = JS_DefineObject(asc->cx, asc->global, "Ape", &ape_class, NULL, 0);

	
	JS_DefineFunction(asc->cx, obj, "addEvent", ape_sm_addEvent, 2, 0);
}


static void init_module(acetables *g_ape) // Called when module is loaded
{
	JSRuntime *rt;

	ape_sm_runtime *asr;
	jsval rval;
	int i;
	
	glob_t globbuf;

	
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
			
			/* define the Ape Object */
			ape_sm_define_ape(asc);

			asc->bytecode = JS_CompileFile(asc->cx, asc->global, asc->filename);
			
			
			if (asc->bytecode == NULL) {
				JS_DestroyScript(asc->cx, asc->bytecode);
			
			} else {
			
				asc->scriptObj = JS_NewScriptObject(asc->cx, asc->bytecode);
				
				/* Adding to the root (avoiding the script to be GC collected) */
				JS_AddNamedRoot(asc->cx, asc->scriptObj, asc->filename);

				/* Run the script */
				JS_ExecuteScript(asc->cx, asc->global, asc->bytecode, &rval);
			}
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

