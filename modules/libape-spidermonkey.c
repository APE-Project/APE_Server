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
#include <jsapi.h>

#include <stdio.h>
#include <glob.h>
#include "plugins.h"
#include "global_plugins.h"


#define MODULE_NAME "spidermonkey"

/* Return the global SpiderMonkey Runtime instance e.g. ASMR->runtime */
#define ASMR ((ape_sm_runtime *)get_property(g_ape->properties, "sm_runtime")->val)

#define APE_JS_EVENT(cb) ape_fire_callback(cb, g_ape)
static void ape_fire_cmd(const char *name, JSObject *obj, JSObject *cb, acetables *g_ape);

/* JSNative macro prototype */
#define APE_JS_NATIVE(func_name) \
	static JSBool func_name(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval) \
	{ \
		ape_sm_compiled *asc; \
		acetables *g_ape; \
		asc = JS_GetContextPrivate(cx); \
		g_ape = asc->g_ape;
		
static JSBool ape_sm_stub(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
	return JS_FALSE;
}


typedef enum {
	APE_EVENT,
	APE_CMD
} ape_sm_callback_t;

typedef struct _ape_sm_callback ape_sm_callback;
struct _ape_sm_callback
{
	char *callbackname;
	jsval func;
	ape_sm_callback_t type;
	JSContext *cx;
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

struct _ape_sock_callbacks {
	jsval on_accept;
	jsval on_read;
	jsval on_read_lf;
	jsval on_disconnect;
	JSObject *server_obj;
	
	ape_sm_compiled  *asc;
	void *private;
};

struct _ape_sock_js_obj {
	ape_socket *client;
	JSObject *client_obj;
};

//static JSBool sockserver_addproperty(JSContext *cx, JSObject *obj, jsval idval, jsval *vp);

static ace_plugin_infos infos_module = {
	"Javascript embeded", 	// Module Name
	"0.01",			// Module Version
	"Anthony Catel",	// Module Author
	NULL			// Config file
};



static JSClass apesocket_class = {
	"apesocket", JSCLASS_HAS_PRIVATE,
	    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
	    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
	    JSCLASS_NO_OPTIONAL_MEMBERS
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

static JSClass socketserver_class = {
	"sockServer", JSCLASS_HAS_PRIVATE,
	    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
	    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
	    JSCLASS_NO_OPTIONAL_MEMBERS
};

static JSClass raw_class = {
	"raw", JSCLASS_HAS_PRIVATE,
	    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_PropertyStub,
	    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
	    JSCLASS_NO_OPTIONAL_MEMBERS
};

static JSBool apesocket_write(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
	ape_socket *client;
	char *string;

	struct _ape_sock_callbacks *cb = JS_GetPrivate(cx, obj);
	ape_sm_compiled *asc = cb->asc;
	acetables *g_ape = asc->g_ape;

	client = ((struct _ape_sock_js_obj *)cb->private)->client;

	if (!JS_ConvertArguments(cx, argc, argv, "s", &string)) {
		return JS_FALSE;
	}
	
	sendbin(client->fd, string, strlen(string), g_ape);
	
	return JS_TRUE;
	
}


static JSBool apesocket_close(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
	ape_socket *client;
	struct _ape_sock_callbacks *cb = JS_GetPrivate(cx, obj);
	
	client = ((struct _ape_sock_js_obj *)cb->private)->client;
	shutdown(client->fd, 2);

	return JS_TRUE;
}



static JSFunctionSpec apesocket_funcs[] = {
    JS_FS("write",   apesocket_write,	1, 0, 0),
	JS_FS("close",   apesocket_close,	0, 0, 0),
    JS_FS_END
};

static JSFunctionSpec apesocket_client_funcs[] = {
	JS_FS("onAccept", ape_sm_stub, 0, 0, 0),
	JS_FS("onRead", ape_sm_stub, 0, 0, 0),
	JS_FS("onDisconnect", ape_sm_stub, 0, 0, 0),
	JS_FS_END
};

static JSFunctionSpec apeuser_funcs[] = {
	//JS_FS("getProperty", apeuser_sm_get_property, 1, 0, 0),
	//JS_FS("setProperty", apeuser_sm_set_property, 1, 0, 0),
	JS_FS_END
};

static void sm_sock_onaccept(ape_socket *client, acetables *g_ape)
{
	jsval rval;
	if (client->attach != NULL) {
		struct _ape_sock_callbacks *cb = ((struct _ape_sock_callbacks *)client->attach);
		

		struct _ape_sock_callbacks *cbcopy;
		struct _ape_sock_js_obj *sock_obj = xmalloc(sizeof(*sock_obj));
		
		sock_obj->client = client;
		
		JSObject *obj;
		jsval params[1];
		
		cbcopy = xmalloc(sizeof(struct _ape_sock_callbacks));
		cbcopy->private = sock_obj;
		cbcopy->asc = cb->asc;
		cbcopy->server_obj = cb->server_obj;
		
		client->attach = cbcopy;
		
		
		JS_SetContextThread(cb->asc->cx);
		JS_BeginRequest(cb->asc->cx);
			
			obj = JS_NewObject(cb->asc->cx, &apesocket_class, NULL, NULL);
			sock_obj->client_obj = obj;
			JS_AddRoot(cb->asc->cx, &sock_obj->client_obj);
			
			JS_SetPrivate(cb->asc->cx, obj, cbcopy);
			JS_DefineFunctions(cb->asc->cx, obj, apesocket_funcs);
			
			params[0] = OBJECT_TO_JSVAL(sock_obj->client_obj);
			
			JS_AddRoot(cb->asc->cx, &params[0]);
			
			JS_CallFunctionName(cb->asc->cx, cb->server_obj, "onAccept", 1, params, &rval);
			
			JS_RemoveRoot(cb->asc->cx, &params[0]);
			
		JS_EndRequest(cb->asc->cx);
		JS_ClearContextThread(cb->asc->cx);			

	}
}

static void sm_sock_ondisconnect(ape_socket *client, acetables *g_ape)
{
	jsval rval;
	
	if (client->attach != NULL) {
		struct _ape_sock_callbacks *cb = ((struct _ape_sock_callbacks *)client->attach);
		JSObject *client_obj = ((struct _ape_sock_js_obj *)cb->private)->client_obj;

		jsval params[1];
		
		JS_SetContextThread(cb->asc->cx);
		JS_BeginRequest(cb->asc->cx);
			params[0] = OBJECT_TO_JSVAL(client_obj);
			JS_CallFunctionName(cb->asc->cx, cb->server_obj, "onDisconnect", 1, params, &rval);
			
			JS_RemoveRoot(cb->asc->cx, &((struct _ape_sock_js_obj *)cb->private)->client_obj);
			
			free(cb->private);
			free(cb);
			
		JS_EndRequest(cb->asc->cx);
		JS_ClearContextThread(cb->asc->cx);			
		
		
	}
}

static void sm_sock_onread_lf(ape_socket *client, char *data, acetables *g_ape)
{
	jsval rval;
	if (client->attach != NULL) {
		struct _ape_sock_callbacks *cb = ((struct _ape_sock_callbacks *)client->attach);
		JSObject *client_obj = ((struct _ape_sock_js_obj *)cb->private)->client_obj;
		
		JS_SetContextThread(cb->asc->cx);
		JS_BeginRequest(cb->asc->cx);
			jsval params[2];
			params[0] = OBJECT_TO_JSVAL(client_obj);
			params[1] = STRING_TO_JSVAL(JS_NewStringCopyZ(cb->asc->cx, data));
			
			JS_AddRoot(cb->asc->cx, &params[1]);
			JS_CallFunctionName(cb->asc->cx, cb->server_obj, "onRead", 2, params, &rval);
			JS_RemoveRoot(cb->asc->cx, &params[1]);
			
		JS_EndRequest(cb->asc->cx);
		JS_ClearContextThread(cb->asc->cx);						
		
	}
}

static void sm_sock_onread(ape_socket *client, ape_buffer *buf, size_t offset, acetables *g_ape)
{
	jsval rval;
	if (client->attach != NULL) {
		struct _ape_sock_callbacks *cb = ((struct _ape_sock_callbacks *)client->attach);
		JSObject *client_obj = ((struct _ape_sock_js_obj *)cb->private)->client_obj;

		JS_SetContextThread(cb->asc->cx);
		JS_BeginRequest(cb->asc->cx);
			
			jsval params[2];
			params[0] = OBJECT_TO_JSVAL(client_obj);
			params[1] = STRING_TO_JSVAL(JS_NewStringCopyN(cb->asc->cx, buf->data, buf->length));
			
			JS_AddRoot(cb->asc->cx, &params[1]);
			JS_CallFunctionName(cb->asc->cx, cb->server_obj, "onRead", 2, params, &rval);
			JS_RemoveRoot(cb->asc->cx, &params[1]);

		
		JS_EndRequest(cb->asc->cx);
		JS_ClearContextThread(cb->asc->cx);						
		buf->length = 0;
	}
}

/* Reporting error from JS compilation (parse error, etc...) */
static void reportError(JSContext *cx, const char *message, JSErrorReport *report)
{
    fprintf(stderr, "%s:%u:%s\n",
            report->filename ? report->filename : "<no filename>",
            (unsigned int) report->lineno,
            message);
}

static void ape_json_to_jsobj(JSContext *cx, json_item *head, JSObject *root)
{
	while (head != NULL) {

		if (head->jchild.child == NULL && head->key.val != NULL) {
			jsval jval;
			
			jval = STRING_TO_JSVAL(JS_NewStringCopyN(cx, head->jval.vu.str.value, head->jval.vu.str.length));
			JS_SetProperty(cx, root, head->key.val, &jval);

		} else if (head->key.val == NULL && head->jval.vu.str.value != NULL) {
			jsuint rval;
			jsval jval;
			
			jval = STRING_TO_JSVAL(JS_NewStringCopyN(cx, head->jval.vu.str.value, head->jval.vu.str.length));
			
			if (JS_GetArrayLength(cx, root, &rval)) {
				JS_SetElement(cx, root, rval, &jval);
			}		
		}
		
		if (head->jchild.child != NULL) {
			JSObject *cobj;
			
			switch(head->jchild.type) {
				case JSON_C_T_OBJ:
					cobj = JS_NewObject(cx, NULL, NULL, root);
					break;
				case JSON_C_T_ARR:
					cobj = JS_NewArrayObject(cx, 0, NULL);
					break;
				default:
					break;
			}
			ape_json_to_jsobj(cx, head->jchild.child, cobj);
			
			if (head->key.val != NULL) {
				jsval jval;
				jval = OBJECT_TO_JSVAL(cobj);
				JS_SetProperty(cx, root, head->key.val, &jval);
			} else {
				jsval jval;
				jsuint rval;
				jval = OBJECT_TO_JSVAL(cobj);
				
				if (JS_GetArrayLength(cx, root, &rval)) {
					JS_SetElement(cx, root, rval, &jval);
				}								
			}
		}
	//	printf("Next\n");
		head = head->next;
	}	
}

/* Dispatch CMD to the corresponding javascript callback */
static unsigned int ape_sm_cmd_wrapper(callbackp *callbacki)
{
	acetables *g_ape = callbacki->g_ape;
	ape_sm_compiled *asc = ASMR->scripts;
	json_item *head = callbacki->param;
	JSContext *cx = NULL;
	JSObject *obj; // param obj
	JSObject *cb;
	
	if (asc == NULL) {
		return (RETURN_NOTHING);
	}
	
	/* Retrieve the CX */
	/* TODO : Add a private data to the callbacki struct (pass it to register_cmd) */
	while (asc != NULL) {
		ape_sm_callback *cb;
		
		for (cb = asc->callbacks; cb != NULL; cb = cb->next) {
			
			if (cb->type == APE_CMD && strcasecmp(callbacki->cmd, cb->callbackname) == 0) {
				cx = cb->cx;
				break;
			}
		}
		asc = asc->next;
	}
	
	if (cx == NULL) {
		return (RETURN_NOTHING);
	}
	
	JS_SetContextThread(cx);
	JS_BeginRequest(cx);
		jsval jval;
		
		obj = JS_NewObject(cx, NULL, NULL, NULL);
		JS_AddRoot(cx, &obj);
		ape_json_to_jsobj(cx, head, obj);
		
		cb = JS_NewObject(cx, NULL, NULL, NULL);
		JS_AddRoot(cx, &cb);
		
		jval = STRING_TO_JSVAL(JS_NewStringCopyZ(cx, callbacki->host));		
		JS_SetProperty(cx, cb, "host", &jval);
		
		jval = INT_TO_JSVAL(callbacki->chl);
		JS_SetProperty(cx, cb, "chl", &jval);
		
	JS_EndRequest(cx);
	JS_ClearContextThread(cx);
				
	ape_fire_cmd(callbacki->cmd, obj, cb, callbacki->g_ape);
	

	
	return (RETURN_NOTHING);
}

APE_JS_NATIVE(ape_sm_register_cmd)
//{
	const char *cmd;
	JSBool needsessid;

	ape_sm_callback *ascb;

	*rval = JSVAL_NULL;
	
	if (argc != 3) {
		return JS_FALSE;
	}
	
	if (!JS_ConvertArguments(cx, argc-1, argv, "sb", &cmd, &needsessid)) {
		return JS_FALSE;
	}
	
	ascb = xmalloc(sizeof(*ascb));

	if (!JS_ConvertValue(cx, argv[2], JSTYPE_FUNCTION, &ascb->func)) {
		free(ascb);
		return JS_FALSE;
	}
	JS_AddRoot(cx, &ascb->func);
	
	/* TODO : Effacer si déjà existant (RemoveRoot & co) */
	ascb->next = asc->callbacks;
	ascb->type = APE_CMD;
	ascb->cx = cx;
	ascb->callbackname = xstrdup(cmd);
	
	asc->callbacks = ascb;	
	
	register_cmd(cmd, ape_sm_cmd_wrapper, (needsessid == JS_TRUE ? NEED_SESSID : NEED_NOTHING), g_ape);
	
	return JS_TRUE;
}

APE_JS_NATIVE(ape_sm_addEvent)
//{
	const char *event;

	ape_sm_callback *ascb;

	*rval = JSVAL_NULL;
	
	if (argc != 2) {
		return JS_FALSE;
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
	ascb->type = APE_EVENT;
	ascb->cx = cx;
	ascb->callbackname = xstrdup(event);
	
	asc->callbacks = ascb;
	
	return JS_TRUE;
}


APE_JS_NATIVE(ape_sm_http_request)
//{
	char *url, *post = NULL;
	//ape_http_request("http://www.rabol.fr/bordel/post.php", "var=kikoo&para=roxxor", g_ape);
	if (!JS_ConvertArguments(cx, argc, argv, "s/s", &url, &post)) {
		return JS_FALSE;
	}
	
	ape_http_request(url, post, g_ape);
	
	return JS_TRUE;
	
}

APE_JS_NATIVE(ape_sm_echo)
//{
	const char *string;
	*rval = JSVAL_NULL;

	if (!JS_ConvertArguments(cx, argc, argv, "s", &string)) {
		return JS_FALSE;
	}

	printf("%s\n", string);

	return JS_TRUE;
}

APE_JS_NATIVE(ape_sm_raw_constructor)
//{
	char *rawname;
	
	if (!JS_ConvertArguments(cx, argc, argv, "s", &rawname)) {
		return JS_FALSE;
	}
	
	JS_SetPrivate(cx, obj, rawname);
	
	return JS_TRUE;
}

APE_JS_NATIVE(ape_sm_sockserver_constructor)
//{
	int port;
	char *ip;
	JSObject *options = NULL;
	ape_socket *server;
	jsval vp;

	if (!JS_ConvertArguments(cx, argc, argv, "is/o", &port, &ip, &options)) {
		return JS_FALSE;
	}

	server = ape_listen(port, ip, g_ape);
	server->attach = xmalloc(sizeof(struct _ape_sock_callbacks));

	((struct _ape_sock_callbacks *)server->attach)->on_accept 		= JSVAL_NULL;
	((struct _ape_sock_callbacks *)server->attach)->on_read_lf 		= JSVAL_NULL;
	((struct _ape_sock_callbacks *)server->attach)->on_disconnect 	= JSVAL_NULL;
	((struct _ape_sock_callbacks *)server->attach)->asc 			= asc;
	((struct _ape_sock_callbacks *)server->attach)->private 		= NULL;
	// TODO : Other clalback
	((struct _ape_sock_callbacks *)server->attach)->server_obj 		= obj;

	JS_AddRoot(cx, &((struct _ape_sock_callbacks *)server->attach)->server_obj);

	/* check if flushlf is set to true in the optional object */
	if (options != NULL && JS_GetProperty(cx, options, "flushlf", &vp) && JSVAL_IS_BOOLEAN(vp) && vp == JSVAL_TRUE) {
		server->callbacks.on_read_lf = sm_sock_onread_lf;
	} else {
		/* use the classic read callback */
		server->callbacks.on_read = sm_sock_onread;
	}

	server->callbacks.on_accept = sm_sock_onaccept;
	server->callbacks.on_disconnect = sm_sock_ondisconnect;

	/* store the ape_socket server in the js object */
	JS_SetPrivate(cx, obj, server);

	JS_DefineFunctions(cx, obj, apesocket_client_funcs);
		
	return JS_TRUE;
}


static JSFunctionSpec ape_funcs[] = {
    JS_FS("addEvent",   ape_sm_addEvent,	2, 0, 0), /* Ape.addEvent('name', function() { }); */
	JS_FS("registerCmd", ape_sm_register_cmd, 2, 0, 0),
    JS_FS("log",  		ape_sm_echo,  		1, 0, 0),/* Ape.echo('stdout\n'); */
	JS_FS("HTTPRequest", ape_sm_http_request, 2, 0, 0),
    JS_FS_END
};


static void ape_sm_define_ape(ape_sm_compiled *asc)
{
	JSObject *obj;

	obj = JS_DefineObject(asc->cx, asc->global, "Ape", &ape_class, NULL, 0);
	JS_DefineFunctions(asc->cx, obj, ape_funcs);
	
	JS_InitClass(asc->cx, obj, NULL, &socketserver_class, ape_sm_sockserver_constructor, 2, NULL, NULL, NULL, NULL);
	JS_InitClass(asc->cx, obj, NULL, &raw_class, ape_sm_raw_constructor, 1, NULL, NULL, NULL, NULL);
	
	JS_SetContextPrivate(asc->cx, asc);
}

static void ape_fire_cmd(const char *name, JSObject *obj, JSObject *cb, acetables *g_ape)
{
	ape_sm_compiled *asc = ASMR->scripts;
	jsval params[2];
	params[0] = OBJECT_TO_JSVAL(obj);
	params[1] = OBJECT_TO_JSVAL(cb);
		
	if (asc == NULL) {
		return;
	}
	
	while (asc != NULL) {
		ape_sm_callback *cb;
		
		for (cb = asc->callbacks; cb != NULL; cb = cb->next) {
			
			if (cb->type == APE_CMD && strcasecmp(name, cb->callbackname) == 0) {
				jsval rval;
				
				JS_SetContextThread(asc->cx);
				JS_BeginRequest(asc->cx);
					JS_CallFunctionValue(asc->cx, asc->global, (cb->func), 2, params, &rval);
				JS_EndRequest(asc->cx);
				JS_ClearContextThread(asc->cx);
				break;
			}
		}
		
		asc = asc->next;
	}
	
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
			
			if (cb->type == APE_EVENT && strcasecmp(name, cb->callbackname) == 0) {
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
	
	add_property(&g_ape->properties, "sm_runtime", asr, EXTEND_POINTER, EXTEND_ISPRIVATE);

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
				printf("here\n");
				JS_DestroyScript(asc->cx, asc->bytecode);
						
			} else {			
				asc->scriptObj = JS_NewScriptObject(asc->cx, asc->bytecode);

				/* Adding to the root (prevent the script to be GC collected) */
				JS_AddNamedRoot(asc->cx, &asc->scriptObj, asc->filename);

				/* put the Ape table on the script structure */
				asc->g_ape = g_ape;

				asc->callbacks = NULL;
				
				/* Run the script */
				JS_ExecuteScript(asc->cx, asc->global, asc->bytecode, &rval);
				
				//JS_MaybeGC(asc->cx);
				
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

