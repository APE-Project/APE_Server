/*
  Copyright (C) 2006, 2007, 2008, 2009  Anthony Catel <a.catel@weelya.com>

  This file is part of APE Server.
  APE is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  APE is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with APE ; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

/* Javascript plugins support using (awesome) spidermonkey API */

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

#define APE_JS_EVENT(cb) ape_fire_callback(cb, g_ape)

/* JSNative macro prototype */
#define APE_JS_NATIVE(func_name) \
	static JSBool func_name(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval) \
	{ \
		ape_sm_compiled *asc; \
		acetables *g_ape; \
		asc = JS_GetPrivate(cx, obj); \
		g_ape = asc->g_ape;

typedef struct _ape_sm_callback ape_sm_callback;

struct _ape_sm_callback
{
	char *callbackname;
	jsval func;
	
	struct _ape_sm_callback *next;
};

typedef struct _ape_sm_compiled ape_sm_compiled;
struct _ape_sm_compiled {
	char *filename;
	
	JSScript *bytecode;
	JSContext *cx;
	JSObject *global;
	JSObject *scriptObj;
	
	acetables *g_ape;
	ape_sm_callback *callbacks;
	
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
	"Ape", JSCLASS_HAS_PRIVATE,
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
//{
	const char *event;

	ape_sm_callback *ascb;

	*rval = JSVAL_NULL;
	
	if (argc != 2) {
		return JS_TRUE;
	}
	
	
	if (!JS_ConvertArguments(cx, argc-1, argv, "s", &event)) {
		return JS_FALSE;
	}
	
	ascb = xmalloc(sizeof(*ascb));
	
	if (!JS_ConvertValue(cx, argv[1], JSTYPE_FUNCTION, &ascb->func)) {
		free(ascb);
		return JS_FALSE;
	}
	JS_AddRoot(cx, &ascb->func);
	
	ascb->next = asc->callbacks;
	ascb->callbackname = xstrdup(event);
	
	asc->callbacks = ascb;
	
	return JS_TRUE;
}

APE_JS_NATIVE(ape_sm_echo)
//{
	const char *string;
	*rval = JSVAL_NULL;
	
	if (!JS_ConvertArguments(cx, argc, argv, "s", &string)) {
		return JS_FALSE;
	}
	
	printf("%s", string);

	return JS_TRUE;
}


static JSFunctionSpec ape_funcs[] = {
    JS_FS("addEvent",   ape_sm_addEvent,	2, 0, 0), /* Ape.addEvent('name', function() { }); */
    JS_FS("echo",  	ape_sm_echo,  		1, 0, 0), /* Ape.echo('stdout\n'); */
    JS_FS_END
};


static void ape_sm_define_ape(ape_sm_compiled *asc)
{
	JSObject *obj;

	obj = JS_DefineObject(asc->cx, asc->global, "Ape", &ape_class, NULL, 0);
	JS_SetPrivate(asc->cx, obj, asc);
	
	JS_DefineFunctions(asc->cx, obj, ape_funcs);

}

static void ape_fire_callback(const char *name, acetables *g_ape)
{
	ape_sm_compiled *asc = ASMR->scripts;
	
	if (asc == NULL) {
		return;
	}
	
	while (asc != NULL) {
		ape_sm_callback *cb;
		
		for (cb = asc->callbacks; cb != NULL; cb = cb->next) {
			
			if (strcasecmp(name, cb->callbackname) == 0) {
				jsval rval;
				
				JS_SetContextThread(asc->cx);
				JS_BeginRequest(asc->cx);
					JS_CallFunctionValue(asc->cx, asc->global, (cb->func), 0, NULL, &rval);
				JS_EndRequest(asc->cx);
				JS_ClearContextThread(asc->cx);
			}
		}
		
		asc = asc->next;
	}
	
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
		
		if (asc->cx == NULL) {
			free(asc->filename);
			free(asc);
			continue;
		}
		
		JS_SetContextThread(asc->cx);
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

				/* Adding to the root (prevent the script to be GC collected) */
				JS_AddNamedRoot(asc->cx, asc->scriptObj, asc->filename);

				/* put the Ape table on the script structure */
				asc->g_ape = g_ape;

				asc->callbacks = NULL;
				
				/* Run the script */
				JS_ExecuteScript(asc->cx, asc->global, asc->bytecode, &rval);
				
				
			}
		JS_EndRequest(asc->cx);
		JS_ClearContextThread(asc->cx);

		if (asc->bytecode == NULL) {
			/* cleaning memory */
		} else {
			asc->next = asr->scripts;
			asr->scripts = asc;
		}
	}
	globfree(&globbuf);
	
	APE_JS_EVENT("init");
	
}

static USERS *ape_cb_add_user(unsigned int fdclient, char *host, acetables *g_ape)
{

	USERS *n = adduser(fdclient, host, g_ape);
	
	if (n != NULL) {
		APE_JS_EVENT("adduser");
	}

	return n;	
}

static ace_callbacks callbacks = {
	ape_cb_add_user,				/* Called when new user is added */
	NULL,				/* Called when a user is disconnected */
	NULL,				/* Called when new chan is created */
	NULL,				/* Called when a user join a channel */
	NULL				/* Called when a user leave a channel */
};

APE_INIT_PLUGIN(MODULE_NAME, init_module, callbacks)

