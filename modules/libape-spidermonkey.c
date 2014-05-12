/*
  Copyright (C) 2009  Anthony Catel <a.catel@weelya.com>

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

/* Javascript plugins support using spidermonkey API */
/* HOWTO : http://www.ape-project.org/wiki/index.php/How_to_build_a_serverside_JS_module */

#define XP_UNIX
#include "../src/configure.h"
#ifdef _USE_MYSQL
#include <mysac.h>
#endif
#include <jsapi.h>
#include <stdio.h>
#include <glob.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "plugins.h"
#include "global_plugins.h"
#include <sys/stat.h>

#define MODULE_NAME "spidermonkey"

/* Return the global SpiderMonkey Runtime instance e.g. ASMR->runtime */
#define ASMR ((ape_sm_runtime *)get_property(g_ape->properties, "sm_runtime")->val)
#define ASMC ((JSContext *)get_property(g_ape->properties, "sm_context")->val)

#define APEUSER_TO_JSOBJ(apeuser) \
		 (JSObject *)get_property(apeuser->properties, "jsobj")->val
#define APESUBUSER_TO_JSOBJ(apeuser) \
		 (JSObject *)get_property(apeuser->properties, "jsobj")->val
#define APECHAN_TO_JSOBJ(apechan) \
		 (JSObject *)get_property(apechan->properties, "jsobj")->val

#define APE_JS_EVENT(cb, argc, argv) ape_fire_callback(cb, argc, argv, g_ape)
static int ape_fire_cmd(const char *name, JSObject *obj, JSObject *cb, callbackp *callbacki, acetables *g_ape);

static JSObject *ape_json_to_jsobj(JSContext *cx, json_item *head, JSObject *root);
/* JSNative macro prototype */
#define APE_JS_NATIVE(func_name) \
	static JSBool func_name(JSContext *cx, uintN argc, jsval *vpn) \
	{\
		ape_sm_compiled *asc; \
		acetables *g_ape; \
		asc = JS_GetContextPrivate(cx); \
		g_ape = asc->g_ape;\
		
#define APE_JS_NATIVE_END(func_name) }

#ifdef _USE_MYSQL
static void apemysql_finalize(JSContext *cx, JSObject *jsmysql);
#endif

static JSBool ape_sm_stub(JSContext *cx, uintN argc, jsval *vpn)
{
	return JS_TRUE;
}

typedef enum {
	APE_EVENT,
	APE_CMD,
	APE_HOOK,
	APE_BADCMD
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
static int ape_fire_hook(ape_sm_callback *cbk, JSObject *obj, JSObject *cb, callbackp *callbacki, acetables *g_ape);

typedef struct _ape_sm_compiled ape_sm_compiled;
struct _ape_sm_compiled {
	char *filename;
	
	JSObject *bytecode;
	JSContext *cx;
	JSObject *global;
	
	acetables *g_ape;
	
	struct {
		ape_sm_callback *head;
		ape_sm_callback *foot;
	} callbacks;
	
	struct _ape_sm_compiled *next;
};

typedef struct _ape_sm_runtime ape_sm_runtime;
struct _ape_sm_runtime {
	JSRuntime *runtime;
	
	ape_sm_compiled *scripts;
};

struct _ape_sock_callbacks {
	JSObject *server_obj;
	ape_sm_compiled  *asc;
	short int state;
	void *private;
};

struct _ape_sock_js_obj {
	ape_socket *client;
	JSObject *client_obj;
};

struct _ape_sm_udns{
	JSContext *cx;
	JSObject *global;
	jsval callback;
	char *cname;
};

#ifdef _USE_MYSQL
struct _ape_mysql_queue {
	struct _ape_mysql_queue	*next;
	MYSAC_RES *res;
	char *query;
	unsigned int query_len;
	jsval callback;
};
#endif

typedef enum {
	SQL_READY_FOR_QUERY,
	SQL_NEED_QUEUE
} ape_mysql_state_t;

#ifdef _USE_MYSQL
struct _ape_mysql_data {
	MYSAC *my;
	void (*on_success)(struct _ape_mysql_data *, int);
	JSObject *jsmysql;
	JSContext *cx;
	char *db;
	void *data;
	jsval callback;
	ape_mysql_state_t state;
	
	struct {
		struct _ape_mysql_queue *head;
		struct _ape_mysql_queue *foot;
	} queue;
};

static void mysac_query_success(struct _ape_mysql_data *myhandle, int code);
static struct _ape_mysql_queue *apemysql_push_queue(struct _ape_mysql_data *myhandle, char *query, unsigned int query_len, jsval callback);
static void apemysql_shift_queue(struct _ape_mysql_data *myhandle);
#endif
//static JSBool sockserver_addproperty(JSContext *cx, JSObject *obj, jsval idval, jsval *vp);

/**
 * @fileOverview libape-spidermonkey API's
 * <p><a href="http://www.ape-project.org/">http://www.ape-project.org/</a></p>
 * Weelya <contact _at_ weelya _dot_ com>
 *
 * @author Anthony Catel (paraboul)
 * @author Nicolas Trani (efyx)
 * @author Florian Gasquez (Fy-)
 * @author John Chavarria (psi)
 * @extends plugins.h
 *
 */
static ace_plugin_infos infos_module = {
	"Javascript embedded", 	// Module Name
	"0.01",					// Module Version
	"Anthony Catel",		// Module Author
	"javascript.conf"		// Config file
};

 /**
 * Standard apesocket object.
 *
 * @name Ape.apesocket
 * @abstract
 */
static JSClass apesocket_class = {
	"apesocket", JSCLASS_HAS_PRIVATE,
		JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
		JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
		JSCLASS_NO_OPTIONAL_MEMBERS
};

 /**
 * Standard javascript object.
 *
 * @name global
 * @private
 */
static JSClass global_class = {
	"global", JSCLASS_GLOBAL_FLAGS | JSCLASS_IS_GLOBAL,
		JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
		JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
		JSCLASS_NO_OPTIONAL_MEMBERS
};

/**
 * The great APE Object in the global namespace.
 *
 * @name Ape
 * @namespace
 */
static JSClass ape_class = {
	"Ape", JSCLASS_HAS_PRIVATE,
		JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
		JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
		JSCLASS_NO_OPTIONAL_MEMBERS
};

/**
 * @brief Collection of os related functions
 * @name Ape.os
 * @namespace
 */
static JSClass os_class = {
	"os", JSCLASS_HAS_PRIVATE,
		JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
		JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
		JSCLASS_NO_OPTIONAL_MEMBERS
};

/**
 * Namespace for b64 encoding and dedoding.
 *
 * @name Ape.b64
 * @namespace
 */
static JSClass b64_class = {
	"base64", JSCLASS_HAS_PRIVATE,
		JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
		JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
		JSCLASS_NO_OPTIONAL_MEMBERS
};

/**
 * Namespace for SHA1 encoding and dedoding.
 *
 * @name Ape.sha1
 * @namespace
 */
static JSClass sha1_class = {
	"sha1", JSCLASS_HAS_PRIVATE,
		JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
		JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
		JSCLASS_NO_OPTIONAL_MEMBERS
};

static JSClass socketserver_class = {
	"sockServer", JSCLASS_HAS_PRIVATE,
		JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
		JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
		JSCLASS_NO_OPTIONAL_MEMBERS
};

static JSClass socketclient_class = {
	"sockClient", JSCLASS_HAS_PRIVATE,
		JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
		JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
		JSCLASS_NO_OPTIONAL_MEMBERS
};

#if 0
static JSClass raw_class = {
	"raw", JSCLASS_HAS_PRIVATE,
		JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
		JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
		JSCLASS_NO_OPTIONAL_MEMBERS
};
#endif

static JSClass user_class = {
	"user", JSCLASS_HAS_PRIVATE,
		JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
		JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
		JSCLASS_NO_OPTIONAL_MEMBERS
};

static JSClass subuser_class = {
	"subuser", JSCLASS_HAS_PRIVATE,
		JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
		JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
		JSCLASS_NO_OPTIONAL_MEMBERS
};

static JSClass channel_class = {
	"channel", JSCLASS_HAS_PRIVATE,
		JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
		JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
		JSCLASS_NO_OPTIONAL_MEMBERS
};

static JSClass pipe_class = {
	"pipe", JSCLASS_HAS_PRIVATE,
		JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
		JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
		JSCLASS_NO_OPTIONAL_MEMBERS
};

#ifdef _USE_MYSQL
static JSClass mysql_class = {
	"MySQL", JSCLASS_HAS_PRIVATE,
		JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
		JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, apemysql_finalize,
		JSCLASS_NO_OPTIONAL_MEMBERS
};
#endif

/**
 * Creates an commandresponce object.
 *
 * @name Ape.cmdresponse
 * @class Internal object
 * @ignore
 */
static JSClass cmdresponse_class = {
	"cmdresponse", JSCLASS_HAS_PRIVATE,
		JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
		JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
		JSCLASS_NO_OPTIONAL_MEMBERS
};


APE_JS_NATIVE(apesocket_write)
//{
	JSString *string;
	char *cstring;
	size_t lstring;
	JSBool burn = JS_FALSE;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	
	struct _ape_sock_callbacks *cb = JS_GetPrivate(cx, obj);
	ape_socket *client;
	
	if (cb == NULL) {
		return JS_TRUE;
	}
	
	client = ((struct _ape_sock_js_obj *)cb->private)->client;

	if (client == NULL || !JS_ConvertArguments(cx, argc, JS_ARGV(cx, vpn), "S/b", &string, &burn)) {
		return JS_TRUE;
	}
	
	lstring = JS_GetStringEncodingLength(cx, string);
	cstring = JS_malloc(cx, lstring);
	lstring = JS_EncodeStringToBuffer(string, cstring, lstring);
	
	sendbin(client->fd, cstring, lstring, (burn == JS_TRUE ? 1 : 0), g_ape);
	
	JS_free(cx, cstring);
	
	return JS_TRUE;
}

/**
 * Writes the data on a clientSocket.
 *
 * @name Ape.sockClient.write
 * @function
 * @public
 *
 * @param {string} data The data to write.
 *
 * @example
 * var socket = new Ape.sockClient('21', 'example.com', {flushlf: true} );
 * socket.onConnect = function() {
 * 	Ape.log('Connected to example.com');
 * 	this.write('Hello\n');
 * });
 *
 * @see Ape.sockClient
 * @see Ape.sockServer
 *
 */
APE_JS_NATIVE(apesocketclient_write)
//{
	JSString *string;
	char *cstring;
	size_t lstring;
	JSBool burn = JS_FALSE;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	
	ape_socket *client = JS_GetPrivate(cx, obj);
	
	if (client == NULL) {
		return JS_TRUE;
	}

	if (!JS_ConvertArguments(cx, argc, JS_ARGV(cx, vpn), "S/b", &string)) {
		return JS_TRUE;
	}
	
	lstring = JS_GetStringEncodingLength(cx, string);
	cstring = JS_malloc(cx, lstring);
	lstring = JS_EncodeStringToBuffer(string, cstring, lstring);
	
	sendbin(client->fd, cstring, lstring, (burn == JS_TRUE ? 1 : 0), g_ape);
	
	JS_free(cx, cstring);
	
	return JS_TRUE;
}

/**
 * Closes a clientSocket.
 *
 * @name Ape.sockClient.close
 * @public
 * @function
 *
 * @returns {void}
 *
 * @example
 * var socket = new Ape.sockClient('21', 'example.com', {flushlf: true} );
 * socket.onConnect = function() {
 * 	Ape.log('Connected to example.com');
 * 	this.write('Bye!\n');
 * 	this.close();
 * });
 *
 * @see Ape.sockClient
 */
APE_JS_NATIVE(apesocketclient_close)
//{
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	
	ape_socket *client = JS_GetPrivate(cx, obj);
	
	if (client == NULL) {
		return JS_TRUE;
	}

	shutdown(client->fd, 2);

	return JS_TRUE;
}

APE_JS_NATIVE(apesocket_close)
//{
	ape_socket *client;
	JSBool safe = JS_FALSE;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	
	struct _ape_sock_callbacks *cb = JS_GetPrivate(cx, obj);
	
	if (cb == NULL || !cb->state) {
		return JS_TRUE;
	}
	
	client = ((struct _ape_sock_js_obj *)cb->private)->client;
	
	if (client == NULL) {
		return JS_TRUE;
	}

	if (!JS_ConvertArguments(cx, argc, JS_ARGV(cx, vpn), "/b", &safe)) {
		return JS_TRUE;
	}
	
	cb->state = 0;
	
	if (safe == JS_TRUE) {
		shutdown(client->fd, 2);
	} else {
		safe_shutdown(client->fd, g_ape);
	}
	return JS_TRUE;
}

/**
 * Closes a serverSocket.
 *
 * @name Ape.sockServer.close
 * @function
 * @public
 *
 * @returns {void}
 *
 * @example
 * var socket = new Ape.sockServer('80', '0.0.0.0', {flushlf: true});
 * socket.onAccept = function(client) {
 * 	Ape.log('New client !');
 * 	client.write('Bye!\n');
 * 	client.close
 * }
 *
 * @see Ape.sockServer
 */
APE_JS_NATIVE(apesocketserver_close)
//{
	ape_socket *server;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	
	server = JS_GetPrivate(cx, obj);
	
	if (server == NULL) {
		return JS_TRUE;
	}

	shutdown(server->fd, 2);
	
	return JS_TRUE;
}

static json_item *jsobj_to_ape_json(JSContext *cx, JSObject *json_obj)
{
	unsigned int i, length = 0, isarray = 0;
	JSIdArray *enumjson = NULL;
	json_item *ape_json = NULL;
	jsval propname;

	/* TODO Fixme : If array has no contigious values, they cannot be retrived, use JS_NewPropertyIterator, JS_NextProperty */

	if (JS_IsArrayObject(cx, json_obj) == JS_TRUE) {
		isarray = 1;
		JS_GetArrayLength(cx, json_obj, &length);
		if (length) {
			//ape_json = json_new_array();
		}
		ape_json = json_new_array();
	} else {
		enumjson = JS_Enumerate(cx, json_obj);
		if ((length = enumjson->length)) {
			//ape_json = json_new_object();
		}
		ape_json = json_new_object();
	}
	
	for (i = 0; i < length; i++) {
		jsval vp;
		JSString *key = NULL;
		JSString *value = NULL;
		json_item *val_obj = NULL;
		
		if (!isarray) {
			JS_IdToValue(cx, enumjson->vector[i], &propname);
			key = JS_ValueToString(cx, propname);
			JS_GetPropertyById(cx, json_obj, enumjson->vector[i], &vp);
		} else {
			JS_GetElement(cx, json_obj, i, &vp);
		}

		switch(JS_TypeOfValue(cx, vp)) {
			case JSTYPE_VOID:
				
				break;
			case JSTYPE_OBJECT:
				
				/* hmm "null" is an empty object */
				if (JSVAL_TO_OBJECT(vp) == NULL) {
					if (!isarray) {
						json_item *jitem;
						size_t jlength = JS_GetStringEncodingLength(cx, key);
						
						jitem = json_set_property_null(ape_json, "\0", jlength);
						jitem->key.len = JS_EncodeStringToBuffer(key, jitem->key.val, jlength);
					} else {
						json_set_element_null(ape_json);
					}
					break;
				}
				if ((val_obj = jsobj_to_ape_json(cx, JSVAL_TO_OBJECT(vp))) != NULL) {
					if (!isarray) {
						json_item *jitem;
						size_t jlength = JS_GetStringEncodingLength(cx, key);
						
						jitem = json_set_property_objN(ape_json, "\0", jlength, val_obj);
						jitem->key.len = JS_EncodeStringToBuffer(key, jitem->key.val, jlength);
					} else {
						json_set_element_obj(ape_json, val_obj);
					}
				}
				break;
			case JSTYPE_FUNCTION:

				break;
			case JSTYPE_STRING:
			{
				json_item *jitem;
				size_t jvlength;
				
				value = JSVAL_TO_STRING(vp);
				jvlength = JS_GetStringEncodingLength(cx, value);
				
				if (!isarray) {
					size_t jklength = JS_GetStringEncodingLength(cx, key);
					
					jitem = json_set_property_strN(ape_json, "\0", jklength, "\0", jvlength);
					jitem->key.len = JS_EncodeStringToBuffer(key, jitem->key.val, jklength);
					jitem->jval.vu.str.length = JS_EncodeStringToBuffer(value, jitem->jval.vu.str.value, jvlength);
					
				} else {
					jitem = json_set_element_strN(ape_json, "\0", jvlength);
					jitem->jval.vu.str.length = JS_EncodeStringToBuffer(value, jitem->jval.vu.str.value, jvlength);
				}
				
				break;
			}
			case JSTYPE_NUMBER:
				{
					if (JSVAL_IS_INT(vp) ) {
						jsint di = JSVAL_TO_INT(vp);

						if (!isarray) {
							json_item *jitem;
							size_t jklength = JS_GetStringEncodingLength(cx, key);
							
							jitem = json_set_property_intN(ape_json, "\0", jklength, di);
							jitem->key.len = JS_EncodeStringToBuffer(key, jitem->key.val, jklength);
							
						} else {
							json_set_element_int(ape_json, di);
						}
					} else {
						jsdouble dp;

						JS_ValueToNumber(cx, vp, &dp);
						long long ll = (long long)trunc(dp);

						if (ll != dp) { //
							if (!isarray) {
								json_item *jitem;
								size_t jklength = JS_GetStringEncodingLength(cx, key);
								
								jitem = json_set_property_floatN(ape_json, "\0", jklength, dp);
								jitem->key.len = JS_EncodeStringToBuffer(key, jitem->key.val, jklength);
							} else {
								json_set_element_float(ape_json, dp);
							}
						} else {
							
							if (!isarray) {
								json_item *jitem;
								size_t jklength = JS_GetStringEncodingLength(cx, key);
								
								jitem = json_set_property_intN(ape_json, "\0", jklength, dp);
								jitem->key.len = JS_EncodeStringToBuffer(key, jitem->key.val, jklength);
								
							} else {
								json_set_element_int(ape_json, dp);
							}
						}
					}
				}
				break;
			case JSTYPE_BOOLEAN:
				if (!isarray) {
					json_item *jitem;
					size_t jklength = JS_GetStringEncodingLength(cx, key);
								
					jitem = json_set_property_boolean(ape_json, "\0", jklength, JSVAL_TO_BOOLEAN(vp));
					jitem->key.len = JS_EncodeStringToBuffer(key, jitem->key.val, jklength);
					
				} else {
					json_set_element_boolean(ape_json, JSVAL_TO_BOOLEAN(vp));
				}
				break;
			default:

				break;
		}
	}
	if (!isarray) {
		JS_DestroyIdArray(cx, enumjson);
	}
	
	return ape_json;
}

/**
 * Get a public property of a pipe.
 * <p>By default each pipe has as pubid property.</p>
 *
 * @name Ape.pipe.getProperty
 * @function
 * @public
 *
 * @param {string} name The name of the property to get.
 * @returns {mixed} the property value or undefined if the property is not present.
 *
 * @example
 * vap pub = pipe.getProperty('pubid');
 *
 * @see Ape.pipe
 * @see Ape.pipe.setProperty
 * @see Ape.pipe.delProperty
 * @see Ape.getPipe
 * @see Ape.pipe
 */
APE_JS_NATIVE(apepipe_sm_get_property)
//{
	char *cproperty;
	JSString *property;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	transpipe *pipe = JS_GetPrivate(cx, obj);
	
	if (pipe == NULL) {
		return JS_TRUE;
	}
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &property)) {
		return JS_TRUE;
	}
	
	cproperty = JS_EncodeString(cx, property);
	
	if (strcmp(cproperty, "pubid") == 0) {
		JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyN(cx, pipe->pubid, 32)));
	} else {
		extend *getprop = get_property(pipe->properties, cproperty);
		if (getprop != NULL) {
			if (getprop->type == EXTEND_STR) {
				JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyZ(cx, getprop->val)));
			} else if (getprop->type == EXTEND_JSON) {
				JSObject *propobj = ape_json_to_jsobj(cx, ((json_item *)getprop->val)->jchild.child, NULL);
				JS_SET_RVAL(cx, vpn, OBJECT_TO_JSVAL(propobj));
			}
		}
	}

	JS_free(cx, cproperty);

	return JS_TRUE;
}

/**
 * Delete a public property from a pipe.
 * <p>The property 'pubid' cannot be removed.</p>
 *
 * @name Ape.pipe.delProperty
 * @function
 * @public
 *
 * @param {string} name The name of the property to unset
 * @returns {void}
 *
 * @example
 * pipe.delProperty('user_id');
 *
 * @see Ape.pipe
 * @see Ape.pipe.destroy
 * @see Ape.pipe.setProperty
 * @see Ape.pipe.getProperty
 * @see Ape.pipe.toObject
 * @see Ape.pipe.getParent
 * @see Ape.getPipe
 * @see Ape.pipe
 */
APE_JS_NATIVE(apepipe_sm_del_property)
//{

	char *cproperty;
	JSString *property;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	transpipe *pipe = JS_GetPrivate(cx, obj);

	if (pipe == NULL) {
		return JS_TRUE;
	}

	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "s", &property)) {
		return JS_TRUE;
	}
	cproperty = JS_EncodeString(cx, property);
	if (strcmp(cproperty, "pubid") != 0) {
		del_property(&pipe->properties, cproperty);
	}
	JS_free(cx, cproperty);
	return JS_TRUE;
}

/**
 * Get the channel object of a pipe.
 *
 * @name Ape.pipe.getParent
 * @function
 * @public
 *
 * @returns {Ape.channel}
 *
 * @example
 * var channel = Ape.getChannelByPubid(pubid);
 * var pipe = channel.getParent();
 *
 * @see Ape.pipe
 * @see Ape.pipe.toObject
 * @see Ape.subuser.getParent
 * @see Ape.getPipe
 */

/**
 * Get the user object of a subuser.
 *
 * @name Ape.subuser.getParent
 * @function
 * @public
 *
 * @returns {Ape.user}
 *
 * @example
 * Ape.registerCmd('foo', true, function(params, info) {
 * 	var user = info.subuser.getParent();
 * });
 *
 * @see Ape.pipe
 * @see Ape.pipe.getParent
 * @see Ape.getPipe
 */
APE_JS_NATIVE(apepipe_sm_get_parent)
//{
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	transpipe *pipe = JS_GetPrivate(cx, obj);
	
	if (pipe == NULL) {
		return JS_TRUE;
	}
	
	switch(pipe->type) {
		case USER_PIPE:
			JS_SET_RVAL(cx, vpn, OBJECT_TO_JSVAL(APEUSER_TO_JSOBJ(((USERS*)pipe->pipe))));
			break;
		case CHANNEL_PIPE:
			JS_SET_RVAL(cx, vpn, OBJECT_TO_JSVAL(APECHAN_TO_JSOBJ(((CHANNEL*)pipe->pipe))));
			break;
		default:
			break;
	}
	
	return JS_TRUE;
}

/**
 * Set a public property on a pipe.
 *
 * @name Ape.pipe.setProperty
 * @function
 * @public
 *
 * @param {string} key The name of the property to set
 * @param {mixed} value The value to set
 * @returns {void}
 *
 * @example
 * var userObj = {'name': 'john', 'age': 30};
 * pipe.setProperty('user', userObj);
 * @example
 * pipe.setProperty('foo', 'bar');
 *
 * @see Ape.pipe
 * @see Ape.getPipe
 * @see Ape.pipe
 */
APE_JS_NATIVE(apepipe_sm_set_property)
//{
	JSString *key;
	char *ckey;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	transpipe *pipe = JS_GetPrivate(cx, obj);
	int typextend = EXTEND_STR;
	void *valuextend = NULL;
	
	if (pipe == NULL) {
		return JS_TRUE;
	}
	
	if (argc != 2) {
		return JS_TRUE;
	}
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &key)) {
		return JS_TRUE;
	}
	
	ckey = JS_EncodeString(cx, key);

	if (JSVAL_IS_NULL(JS_ARGV(cx, vpn)[1]) ){
		typextend = EXTEND_STR;
		jsval *val= xmalloc(sizeof(jsval) * 1);
		*val= JSVAL_NULL;
		valuextend = val;

	}else if (JSVAL_IS_OBJECT(JS_ARGV(cx, vpn)[1])) { /* Convert to APE JSON Object */
		json_item *ji;

		if ((ji = jsobj_to_ape_json(cx, JSVAL_TO_OBJECT(JS_ARGV(cx, vpn)[1]))) != NULL) {
			typextend = EXTEND_JSON;
			valuextend = ji;
		}
	} else { /* Convert to string */
		typextend = EXTEND_STR;
		valuextend = JS_EncodeString(cx, JS_ValueToString(cx, JS_ARGV(cx, vpn)[1])); /* No needs to be gc-rooted while there is no JSAPI Call after that */
	}

	switch(pipe->type) {
		case USER_PIPE:
			/* Set property on directly on the user (not on the pipe) */
			add_property(&((USERS *)(pipe->pipe))->properties, ckey, valuextend, typextend, EXTEND_ISPUBLIC);
			break;
		case CHANNEL_PIPE:
			/* Set property on directly on the channel (not on the pipe) */
			add_property(&((CHANNEL *)(pipe->pipe))->properties, ckey, valuextend, typextend, EXTEND_ISPUBLIC);
			break;
		case CUSTOM_PIPE:
			add_property(&pipe->properties, ckey, valuextend, typextend, EXTEND_ISPUBLIC);
			break;
		default:
			break;
	}
	
	if (typextend == EXTEND_STR && valuextend != NULL) {
		JS_free(cx, valuextend);
	}
	
	JS_free(cx, ckey);
	
	return JS_TRUE;
}

/**
 * Get a pipe object.
 *
 * @name Ape.pipe.toObject
 * @function
 * @public
 *
 * @returns {object}
 *
 * @example
 * Ape.registerCmd('foocmd', true, function(params, info) {
 * 	var obj = Ape.getPipe(params.pubid).toObject();
 * });
 *
 * @see Ape.pipe
 * @see Ape.pipe
 * @see Ape.getPipe
 */
APE_JS_NATIVE(apepipe_sm_to_object)
//{
	transpipe *spipe;
	json_item *pipe_object;
	JSObject *js_pipe_object;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	
	if ((spipe = JS_GetPrivate(cx, obj)) == NULL) {
		return JS_TRUE;
	}
	
	pipe_object = get_json_object_pipe(spipe);
	
	js_pipe_object = ape_json_to_jsobj(cx, pipe_object->jchild.child, NULL);
	
	JS_SET_RVAL(cx, vpn, OBJECT_TO_JSVAL(js_pipe_object));
	
	return JS_TRUE;
	
}

/**
 * Destroy a custom pipe.
 *
 * @name Ape.pipe.destroy
 * @function
 * @public
 *
 * @returns {void}
 *
 * @example
 * pipe.destroy();
 *
 * @see Ape.pipe
 * @see Ape.getPipe
 * @see Ape.getPipe
 * @see Ape.pipe
 */
APE_JS_NATIVE(apepipe_sm_destroy)
//{
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	transpipe *pipe = JS_GetPrivate(cx, obj);
	
	if (pipe == NULL) {
		return JS_TRUE;
	}
	
	JS_SetPrivate(cx, obj, (void *)NULL);
	destroy_pipe(pipe, g_ape);
	
	return JS_TRUE;
}

static JSBool sm_send_raw(JSContext *cx, transpipe *to_pipe, int chl, uintN argc, jsval *argv, acetables *g_ape)
{
	RAW *newraw;
	JSString *raw;
	char *craw;
	JSObject *json_obj = NULL, *options = NULL;
	json_item *jstr;
	jsval vp;
	
	if (to_pipe == NULL) {
		return JS_TRUE;
	}
	
	if (!JS_ConvertArguments(cx, 3, argv, "So/o", &raw, &json_obj, &options) || json_obj == NULL) {
		return JS_TRUE;
	}
	
	craw = JS_EncodeString(cx, raw);
	
	jstr = jsobj_to_ape_json(cx, json_obj);

	if (options != NULL && JS_GetProperty(cx, options, "from", &vp) && JSVAL_IS_OBJECT(vp) && JS_InstanceOf(cx, JSVAL_TO_OBJECT(vp), &pipe_class, 0) == JS_TRUE) {
		JSObject *js_pipe = JSVAL_TO_OBJECT(vp);
		transpipe *from_pipe = JS_GetPrivate(cx, js_pipe);
		
		if (from_pipe != NULL && from_pipe->type == USER_PIPE) {
			json_set_property_objN(jstr, "from", 4, get_json_object_pipe(from_pipe));
			
			if (to_pipe->type == USER_PIPE) {
				json_set_property_objN(jstr, "pipe", 4, get_json_object_pipe(from_pipe));
			} else if (to_pipe->type == CHANNEL_PIPE) {
				json_item *jcopy = json_item_copy(jstr, NULL);
				if (((CHANNEL*)to_pipe->pipe)->head != NULL && ((CHANNEL*)to_pipe->pipe)->head->next != NULL) {
					
					json_set_property_objN(jstr, "pipe", 4, get_json_object_pipe(to_pipe));
				
					newraw = forge_raw(craw, jstr);
					post_raw_channel_restricted(newraw, to_pipe->pipe, from_pipe->pipe, g_ape);
				}
				if (options != NULL && JS_GetProperty(cx, options, "restrict", &vp) && JSVAL_IS_OBJECT(vp) && JS_InstanceOf(cx, JSVAL_TO_OBJECT(vp), &subuser_class, 0) == JS_TRUE) {
					JSObject *subjs = JSVAL_TO_OBJECT(vp);
					subuser *sub = JS_GetPrivate(cx, subjs);
					if (sub != NULL && ((USERS *)from_pipe->pipe)->nsub > 1) {
						json_set_property_objN(jcopy, "pipe", 4, get_json_object_pipe(to_pipe));
						newraw = forge_raw(craw, jcopy);
						post_raw_restricted(newraw, from_pipe->pipe, sub, g_ape);
					} else {
						free_json_item(jcopy);
					}
					
				} else {
					free_json_item(jcopy);
				}
				
				JS_free(cx, craw);
				return JS_TRUE;
			}
		} else if (from_pipe != NULL && from_pipe->type == CUSTOM_PIPE) {
			json_set_property_objN(jstr, "pipe", 4, get_json_object_pipe(from_pipe));
		}
	}

	/* in the case of sendResponse */
	/* TODO : May be broken if to_pipe->type == CHANNNEL and from == USER */
	if (chl) {
		json_set_property_intN(jstr, "chl", 3, chl);
	}
	
	if (to_pipe->type == CHANNEL_PIPE && ((struct CHANNEL *)to_pipe->pipe)->head != NULL) {
		if (options != NULL && JS_GetProperty(cx, options, "restrict", &vp) && JSVAL_IS_OBJECT(vp) && JS_InstanceOf(cx, JSVAL_TO_OBJECT(vp), &user_class, 0) == JS_TRUE) {
			JSObject *userjs = JSVAL_TO_OBJECT(vp);
			USERS *user = JS_GetPrivate(cx, userjs);
			
			if (user == NULL) {
				free_json_item(jstr);
				
				JS_free(cx, craw);
				return JS_TRUE;
			}

			post_raw_channel_restricted(forge_raw(craw, jstr), to_pipe->pipe, user, g_ape);
			
			JS_free(cx, craw);
			return JS_TRUE;
		}
		post_raw_channel(forge_raw(craw, jstr), to_pipe->pipe, g_ape);
	} else if (to_pipe->type != CHANNEL_PIPE) {
		if (options != NULL && JS_GetProperty(cx, options, "restrict", &vp) && JSVAL_IS_OBJECT(vp) && JS_InstanceOf(cx, JSVAL_TO_OBJECT(vp), &subuser_class, 0) == JS_TRUE) {
			JSObject *subjs = JSVAL_TO_OBJECT(vp);
			subuser *sub = JS_GetPrivate(cx, subjs);
			
			if (sub == NULL || ((USERS *)to_pipe->pipe)->nsub < 2 || to_pipe->pipe != sub->user) {
				free_json_item(jstr);
				
				JS_free(cx, craw);
				return JS_TRUE;
			}

			post_raw_restricted(forge_raw(craw, jstr), to_pipe->pipe, sub, g_ape);
			
			JS_free(cx, craw);
			return JS_TRUE;

		}
		post_raw(forge_raw(craw, jstr), to_pipe->pipe, g_ape);
	} else {
		free_json_item(jstr);
	}
	
	JS_free(cx, craw);
	return JS_TRUE;
}

/**
 * Send a custom RAW on a pipe.
 *
 * <p>The Ape server has some pre-registered CMD where automatically RAWs are returned upon. See Ape.registerCmd for details.</p>
 *
 * @name Ape.pipe.sendRaw
 * @function
 * @public
 *
 * @param {string} name The RAW name
 * @param {object} data An object that will be JSON encoded and send outh with the RAW
 * @param {object} [options]
 * @param {pipe} [options.from] An user pipe or a custom pipe that will be added in the from field, if an user pipe, the raw will not be sent to this user.
 * @param {user|subuser} [options.restrict] A user (if sending to a channel), or a subuser (if sending to n user) which will not receive the raw.
 * @returns {void}
 *
 * @example
 * //Basic
 * pipe.sendRaw('CUSTOM_RAW', {'foo': 'bar'});
 * //This will send this raw: {'time': '1255281320', 'raw': 'CUSTOM_RAW', 'data': {'foo': 'bar'}}
 * @example
 * //Send a raw to an user
 * Ape.registerCmd('foocmd', true, function(params, info) {
 * 	Ape.log('The user ip : (' + infos.ip + '), foo : ' + params.foo);
 * 	info.user.pipe.sendRaw('CUSTOM_RAW', {'foo': 'bar'});
 * });
 * @example
 * //Send a raw to a pipe
 * Ape.registerCmd('foocmd', true, function(params, info) {
 * 	Ape.log('The user ip : (' + info.ip + '), foo : ' + params.foo);
 * 	Ape.getPipe(params.pubid).sendRaw('CUSTOM_RAW', {'foo': 'bar'});
 * });
 *
 * @see Ape.pipe
 * @see Ape.pipe.sendRaw
 * @see Ape.getPipe
 * @see Ape.pipe
 * @see Ape.registerCmd
 * @see Ape.pipe.sendResponse
 * @see Ape.subuser.sendRaw
 * @see Ape.subuser
 */

/**
 * Send a custom RAW to a subuser.
 *
 * @name Ape.subuser.sendRaw
 * @function
 * @public
 *
 * @param {string} name The RAW name
 * @param {object} data An object that will be JSON encoded and send outh with the RAW
 * @param {object} [options]
 * @param {pipe} [options.from] An user pipe or a custom pipe that will be added in the from field, if an user pipe, the raw will not be sent to this user.
 * @param {user|subuser} [options.restrict] A user (if sending to a channel), or a subuser (if sending to n user) which will not receive the raw.
 * @returns {void}
 *
 * @example
 * Ape.registerCmd('foo', true, function(params, info) {
 * 	info.subuser.sendRaw('bar', {'ok':'true'});
 * });
 *
 * @see Ape.pipe.sendRaw
 * @see Ape.pipe.sendResponse
 * @see Ape.subuser
 */
APE_JS_NATIVE(apepipe_sm_send_raw)
//{
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	transpipe *to_pipe = JS_GetPrivate(cx, obj);
	
	if (to_pipe == NULL) {
		return JS_TRUE;
	}
	
	return sm_send_raw(cx, to_pipe, 0, argc, JS_ARGV(cx, vpn), g_ape);

}

/**
 * Send a response to a command.
 * <p>You can send a response in registerHookCmd and registerCmd. This is useful if you want to assign a callback to a command in client-side.</p>
 * <p>When calling sendResponse method, the RAW sent to the user has the challenge 'chl' set to the value that came in the request to facilitate a callback. Other methods of sending a response includes pipe.sendRaw however server doesn't set the 'chl' automatically for them.</p>
 *
 * @name Ape.pipe.sendResponse
 * @function
 * @public
 *
 * @param {string} name The RAW name
 * @param {object} data An object that will be JSON encoded and send out with the RAW
 * @returns {void}
 *
 * @example
 * Ape.registerCmd('foocmd', true, function(params, info) {
 * 	info.sendResponse('custom_raw', {'foo':'bar'});
 * });
 *
 * @see Ape.pipe.sendRaw
 * @see Ape.subuser.sendRaw
 * @see Ape.registerCmd
 * @see Ape.registerHookCmd
 * @see Ape.registerHookBadCmd
 */
APE_JS_NATIVE(apepipe_sm_send_response)
//{
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	jsval user, chl, pipe;
	JS_GetProperty(cx, obj, "user", &user);
	
	if (JSVAL_IS_VOID(user) || JS_InstanceOf(cx, JSVAL_TO_OBJECT(user), &user_class, 0) == JS_FALSE) {
		return JS_TRUE;
	}
	
	JS_GetProperty(cx, JSVAL_TO_OBJECT(user), "pipe", &pipe);
	
	if (JSVAL_IS_VOID(pipe) || JS_InstanceOf(cx, JSVAL_TO_OBJECT(pipe), &pipe_class, 0) == JS_FALSE) {
		return JS_TRUE;
	}
	
	JS_GetProperty(cx, obj, "chl", &chl);
	
	if (JSVAL_IS_VOID(chl) || !JSVAL_IS_NUMBER(chl)) {
		return JS_TRUE;
	}
	
	/* TODO : Fixme JSVAL_TO_INT => double */
	return sm_send_raw(cx, JS_GetPrivate(cx, JSVAL_TO_OBJECT(pipe)), JSVAL_TO_INT(chl), argc, JS_ARGV(cx, vpn), g_ape);
}

/**
 * Get a public property of a channel.
 * <p>By default each channel has a name and pubid property.</p>
 *
 * @name Ape.channel.getProperty
 * @function
 * @public
 *
 * @param {string} name The name of the property to get
 * @returns {mixed} the property value or undefined
 *
 * @example
 * var pub = channel.getProperty('pubid');
 *
 * @see Ape.channel.setProperty
 * @see Ape.channel.delProperty
 * @see Ape.mkChan
 * @see Ape.rmChan
 * @see Ape.getChannelByName
 * @see Ape.getChannelByPubid
 * @see Ape.channel
 */
APE_JS_NATIVE(apechannel_sm_get_property)
//{
	JSString *property;
	char *cproperty;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	CHANNEL *chan = JS_GetPrivate(cx, obj);
	
	if (chan == NULL) {
		return JS_TRUE;
	}
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &property)) {
		return JS_TRUE;
	}
	
	cproperty = JS_EncodeString(cx, property);
	
	if (strcmp(cproperty, "pubid") == 0) {
		JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyN(cx, chan->pipe->pubid, 32)));
	} else if (strcmp(cproperty, "name") == 0) {
		JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyZ(cx, chan->name)));
	} else {
		extend *getprop = get_property(chan->properties, cproperty);
		if (getprop != NULL) {
			if (getprop->type == EXTEND_STR) {
				JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyZ(cx, getprop->val)));
			} else if (getprop->type == EXTEND_JSON) {
				JSObject *propobj = ape_json_to_jsobj(cx, ((json_item *)getprop->val)->jchild.child, NULL);
				JS_SET_RVAL(cx, vpn, OBJECT_TO_JSVAL(propobj));
			}
		}
	}

	JS_free(cx, cproperty);

	return JS_TRUE;
}

/**
 * Delete a public property from a channel.
 * <p>The property 'pubid' and 'name' cannot be removed.</p>
 *
 * @name Ape.channel.delProperty
 * @function
 * @public
 *
 * @param {string} name The name of the property to unset
 * @returns {void}
 *
 * @example
 * channel.delProperty('foo');
 *
 * @see Ape.channel.setProperty
 * @see Ape.channel.getProperty
 * @see Ape.mkChan
 * @see Ape.rmChan
 * @see Ape.getChannelByName
 * @see Ape.getChannelByPubid
 * @see Ape.channel
 */
APE_JS_NATIVE(apechannel_sm_del_property)
//{
	JSString *property;
	char *cproperty;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	CHANNEL *chan = JS_GetPrivate(cx, obj);

	if (chan == NULL) {
		return JS_TRUE;
	}

	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "s", &property)) {
		return JS_TRUE;
	}
	cproperty = JS_EncodeString(cx, property);
	if (strcmp(cproperty, "pubid") != 0 && strcmp(cproperty, "name") != 0) {
		del_property(&chan->properties, cproperty);
	}
	JS_free(cx, cproperty);
	return JS_TRUE;
}

/**
 * Set a public property from a channel.
 *
 * @name Ape.channel.setProperty
 * @function
 * @public
 *
 * @param {string} key The name of the property to set
 * @param {mixed} value The value to set
 * @returns {void}
 *
 * @example
 * channel.setProperty('foo', 'bar');
 * @example
 * channel.setProperty('foo', {'bar':' 1});
 *
 * @see Ape.channel.delProperty
 * @see Ape.channel.getProperty
 * @see Ape.mkChan
 * @see Ape.rmChan
 * @see Ape.getChannelByName
 * @see Ape.getChannelByPubid
 * @see Ape.channel
 */
APE_JS_NATIVE(apechannel_sm_set_property)
//{
	JSString *key;
	char *ckey, *property;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);

	CHANNEL *chan = JS_GetPrivate(cx, obj);
	
	if (chan == NULL) {
		return JS_TRUE;
	}

	if (!JS_ConvertArguments(cx, 2, JS_ARGV(cx, vpn), "S", &key)) {
		return JS_TRUE;
	}
	
	ckey = JS_EncodeString(cx, key);
	if (JSVAL_IS_NULL(JS_ARGV(cx, vpn)[1]) ){
		jsval val = JSVAL_NULL;
		add_property(&chan->properties, ckey, &val, EXTEND_STR, EXTEND_ISPUBLIC);
	}else if (JSVAL_IS_OBJECT(JS_ARGV(cx, vpn)[1])) { /* Convert to APE JSON Object */
		json_item *ji;
		
		if ((ji = jsobj_to_ape_json(cx, JSVAL_TO_OBJECT(JS_ARGV(cx, vpn)[1]))) != NULL) {
			add_property(&chan->properties, ckey, ji, EXTEND_JSON, EXTEND_ISPUBLIC);
		}
	} else { /* Convert to string */
		property = JS_EncodeString(cx, JS_ValueToString(cx, JS_ARGV(cx, vpn)[1])); /* No needs to be gc-rooted while there is no JSAPI Call after that */
		add_property(&chan->properties, ckey, property, EXTEND_STR, EXTEND_ISPUBLIC);
		JS_free(cx, property);
	}
	
	JS_free(cx, ckey);
	
	return JS_TRUE;
}

/**
 * Check if a channel is interactive.
 * <p>A channel can be set to readonly if the channelname starts with a '*'</p>
 *
 * @name Ape.channel.isInteractive
 * @function
 * @public
 *
 * @returns {boolean} interactive
 *
 * @example
 * var channel = Ape.mkChan('*foo');
 * Ape.log('is interactive: ' + channel.isInteractve());
 *
 * @see Ape.mkChan
 * @see Ape.rmChan
 * @see Ape.getChannelByName
 * @see Ape.getChannelByPubid
 * @see Ape.channel
 */
APE_JS_NATIVE(apechannel_sm_isinteractive)
//{
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	CHANNEL *chan = JS_GetPrivate(cx, obj);
	
	if (chan == NULL) {
		return JS_TRUE;
	}
	
	JS_SET_RVAL(cx, vpn, (!(chan->flags & CHANNEL_NONINTERACTIVE) ? JSVAL_TRUE : JSVAL_FALSE));
		
	return JS_TRUE;
}

/**
 * Get a public property of a user.
 * <p>By default each user has as sessid, ip and pubid propert.</p>
 *
 * @name Ape.user.getProperty
 * @function
 * @public
 *
 * @param {string} name The name of the property to get
 * @returns {mixed} the property value or undefined
 *
 * @example
 * var prop = user.getProperty('pubid');
 *
 * @see Ape.user.setProperty
 * @see Ape.user.delProperty
 * @see Ape.getUserByPubid
 * @see Ape.user
 */
APE_JS_NATIVE(apeuser_sm_get_property)
//{
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	JSString *property;
	USERS *user = JS_GetPrivate(cx, obj);
	char *cproperty;

	if (user == NULL) {
		return JS_TRUE;
	}
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &property)) {
		return JS_TRUE;
	}
	
	cproperty = JS_EncodeString(cx, property);
	
	if (strcmp(cproperty, "sessid") == 0) {
		JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyN(cx, user->sessid, 32)));
	} else if (strcmp(cproperty, "pubid") == 0) {
		JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyN(cx, user->pipe->pubid, 32)));
	} else if (strcmp(cproperty, "ip") == 0) {
		JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyZ(cx, user->ip)));
	} else {
		extend *getprop = get_property(user->properties, cproperty);
		if (getprop != NULL) {
			if (getprop->type == EXTEND_STR) {
				JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyZ(cx, getprop->val)));
			} else if (getprop->type == EXTEND_JSON) {
				JSObject *propobj = ape_json_to_jsobj(cx, ((json_item *)getprop->val)->jchild.child, NULL);
				JS_SET_RVAL(cx, vpn, OBJECT_TO_JSVAL(propobj));
			}
		}
	}
	
	JS_free(cx, cproperty);
	
	return JS_TRUE;
}

/**
 * Delete a public property from a user.
 * </p>The property 'pubid', ip and 'sessid' cannot be removed.</p>
 *
 * @name Ape.user.delProperty
 * @function
 * @public
 *
 * @param {string} name The name of the property to unset
 * @returns {void}
 *
 * @example
 * user.delProperty('foo');
 *
 * @see Ape.user.setProperty
 * @see Ape.user.getProperty
 * @see Ape.getUserByPubid
 * @see Ape.user
 */
APE_JS_NATIVE(apeuser_sm_del_property)
//{
	JSString *property;
	char *cproperty;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	USERS *user = JS_GetPrivate(cx, obj);

	if (user == NULL) {
		return JS_TRUE;
	}

	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "s", &property)) {
		return JS_TRUE;
	}
	cproperty = JS_EncodeString(cx, property);
	if (strcmp(cproperty, "sessid") != 0 && strcmp(cproperty, "pubid") != 0 && strcmp(cproperty, "ip") != 0) {
		del_property(&user->properties, cproperty);
	}
	JS_free(cx, cproperty);
	return JS_TRUE;
}

/**
 * User has quit the APE.
 *
 * @name Ape.user.quit
 * @function
 * @public
 *
 * @returns {void}
 *
 * @example
 * Ape.registerCmd('signoff', true, function(user, infos) {
 * 	user.quit();
 * });
 *
 * @see Ape.getUserByPubid
 * @see Ape.user
 * @see Ape.user.left
 * @see Ape.user.join
 */
APE_JS_NATIVE(apeuser_sm_quit)
//{
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	USERS *user = JS_GetPrivate(cx, obj);

	if (user == NULL) {
		return JS_TRUE;
	}
	//TODO: call deluser event? (see ape_cb_del_user)
	deluser(user, g_ape);
	
	return JS_TRUE;
}

/**
 * Forces the user to join a specific channel.
 *
 * @name Ape.user.join
 * @function
 * @public
 *
 * @param {string|channel} channel The channel to join
 * @returns {boolean} true if succesfull
 *
 * @example
 * Ape.registerCmd('entry', true, function(user, infos) {
 * 	user.join('*level1');
 * });
 * @example
 * Ape.registerHookCmd('connect', function(params, cmd) {
 * 	cmd.user.join('testChannel');
 * 	return 1;
 * });
 *
 * @see Ape.getUserByPubid
 * @see Ape.user
 * @see Ape.user.left
 * @see Ape.user.quit
 *
 * @fires beforeJoin
 * @fires join
 * @fires afterJoin
 */
APE_JS_NATIVE(apeuser_sm_join)
//{
	CHANNEL *chan;
	JSString *chan_name;
	JSObject *chan_obj;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	char *cchan_name;
	
	USERS *user = JS_GetPrivate(cx, obj);
	
	JS_SET_RVAL(cx, vpn, JSVAL_FALSE);
		
	if (user == NULL) {
		return JS_TRUE;
	}
	
	if (JSVAL_IS_STRING(JS_ARGV(cx, vpn)[0])) {
		JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &chan_name);
		cchan_name = JS_EncodeString(cx, chan_name);
		
		if ((chan = getchan(cchan_name, g_ape)) == NULL) {
			JS_free(cx, cchan_name);
			return JS_TRUE;
		}
		JS_free(cx, cchan_name);
	} else if (JSVAL_IS_OBJECT(JS_ARGV(cx, vpn)[0])) {
		JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "o", &chan_obj);
		if (!JS_InstanceOf(cx, chan_obj, &channel_class, 0) || (chan = JS_GetPrivate(cx, chan_obj)) == NULL) {
			return JS_TRUE;
		}
	} else {
		return JS_TRUE;
	}
	
	join(user, chan, g_ape);
	
	JS_SET_RVAL(cx, vpn, JSVAL_TRUE);
	
	return JS_TRUE;
}

/**
 * Forces the user to leave a specific channel.
 *
 * @name Ape.user.left
 * @function
 * @public
 *
 * @param {string|channel} channel The channel to leave
 * @returns {void}
 *
 * @example
 * Ape.registerCmd('leaveMain', true, function(user, infos) {
 * 	user.left('main_channel');
 * });
 * @see Ape.getUserByPubid
 * @see Ape.user
 * @see Ape.user.join
 * @see Ape.user.quit
 *
 * @fires left
 */
APE_JS_NATIVE(apeuser_sm_left)
//{
	CHANNEL *chan;
	JSString *chan_name;
	JSObject *chan_obj;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	char *cchan_name;
	USERS *user = JS_GetPrivate(cx, obj);
	
	JS_SET_RVAL(cx, vpn, JSVAL_FALSE);
		
	if (user == NULL) {
		return JS_TRUE;
	}
	
	if (JSVAL_IS_STRING(JS_ARGV(cx, vpn)[0])) {
		JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &chan_name);
		cchan_name = JS_EncodeString(cx, chan_name);
		if ((chan = getchan(cchan_name, g_ape)) == NULL) {
			JS_free(cx, cchan_name);
			return JS_TRUE;
		}
		JS_free(cx, cchan_name);
	} else if (JSVAL_IS_OBJECT(JS_ARGV(cx, vpn)[0])) {
		JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "o", &chan_obj);
		if (!JS_InstanceOf(cx, chan_obj, &channel_class, 0) || (chan = JS_GetPrivate(cx, chan_obj)) == NULL) {
			return JS_TRUE;
		}
	} else {
		return JS_TRUE;
	}
	
	left(user, chan, g_ape);
	
	JS_SET_RVAL(cx, vpn, JSVAL_TRUE);
	
	return JS_TRUE;
}

/**
 * Set a public property on a user object.
 *
 * @name Ape.user.setProperty
 * @function
 * @public
 *
 * @param {string} key The name of the property to set
 * @param {mixed} value The value to set
 * @returns {void}
 *
 * @example
 * user.setProperty('foo', 'bar');
 * @example
 * user.setProperty('foo', {'bar':' 1});
 *
 * @see Ape.user.delProperty
 * @see Ape.user.getProperty
 * @see Ape.getUserByPubid
 * @see Ape.user
 */
APE_JS_NATIVE(apeuser_sm_set_property)
//{
	char *ckey, *property;
	JSString *key;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	USERS *user = JS_GetPrivate(cx, obj);
	
	if (user == NULL) {
		return JS_TRUE;
	}
	
	if (argc != 2) {
		return JS_TRUE;
	}
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &key)) {
		return JS_TRUE;
	}
	
	ckey = JS_EncodeString(cx, key);
	if ( JSVAL_IS_NULL(JS_ARGV(cx, vpn)[1]) ){
		jsval val = JSVAL_NULL;
		add_property(&user->properties, ckey, &val, EXTEND_STR, EXTEND_ISPUBLIC);
	} else if (JSVAL_IS_OBJECT(JS_ARGV(cx, vpn)[1])) { /* Convert to APE JSON Object */
		json_item *ji;
		
		if ((ji = jsobj_to_ape_json(cx, JSVAL_TO_OBJECT(JS_ARGV(cx, vpn)[1]))) != NULL) {
			add_property(&user->properties, ckey, ji, EXTEND_JSON, EXTEND_ISPUBLIC);
		}
	} else { /* Convert to string */
		property = JS_EncodeString(cx, JS_ValueToString(cx, JS_ARGV(cx, vpn)[1]));
		add_property(&user->properties, ckey, property, EXTEND_STR, EXTEND_ISPUBLIC);
		
		JS_free(cx, property);
	}
	
	JS_free(cx, ckey);
	
	return JS_TRUE;
}

#ifdef _USE_MYSQL
/**
 * Get the last mysql error message.
 * <p> This returns the errorstring from the Database.</p>
 *
 * @name Ape.MySQL.errorString
 * @function
 * @public
 *
 * @returns {string} the last mysql error message for the connection
 *
 * @example
 * sql.onError = function(errorNo) {
 * 	Ape.log('Connection Error : ' + errorNo + ' : ' + this.errorString());
 * }
 *
 * @see Ape.MySQL.onError
 */
APE_JS_NATIVE(apemysql_sm_errorstring)
//{
	struct _ape_mysql_data *myhandle;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	
	if ((myhandle = JS_GetPrivate(cx, obj)) == NULL) {
		return JS_TRUE;
	}

	JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyZ(cx, mysac_advance_error(myhandle->my))));
	
	return JS_TRUE;
}

/**
 * Get the last id insert with auto-increment.
 * <p>Beware: This is the last id insert for the current connection (object instance).</p>
 *
 * @name Ape.MySQL.getInsertId
 * @function
 * @public
 *
 * @returns {integer} Used to get the last insert id with auto-increment.for the connection
 *
 * @example
 * sql.query('INSERT INTO table VALUES("a", "b", "c")', function(res, errorNo) {
 * 	if (errorNo) Ape.log('Request error : ' + errorNo + ' : ' + this.errorString());
 * 	else Ape.log('Inserted: ' + MySQL.getInsertId ());
 * });
 */
APE_JS_NATIVE(apemysql_sm_insert_id)
//{
	struct _ape_mysql_data *myhandle;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	
	if ((myhandle = JS_GetPrivate(cx, obj)) == NULL) {
		return JS_TRUE;
	}

	JS_SET_RVAL(cx, vpn, INT_TO_JSVAL(mysac_insert_id(myhandle->my)));
	
	return JS_TRUE;
}

/**
 * Escape a string for mysql.
 *
 * @name Ape.MySQL.escape
 * @function
 * @public
 * @static
 *
 * @param {string} text The string to be escaped with mysql_escape_string to prepare it for a query
 * @returns {string} The escaped string
 *
 * @example
 * sql.query('SELECT nick FROM user WHERE login = \'' + Ape.MySQL.escape(mylogin) + '\'');
 */
APE_JS_NATIVE(apemysql_escape)
//{
	JSString *input_c;
	char *cinput_c, *escaped;
	unsigned long int len;
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &input_c)) {
		return JS_TRUE;
	}
	
	cinput_c = JS_EncodeString(cx, input_c);
	
	len = strlen(cinput_c);
	escaped = xmalloc(sizeof(char) * (len*2+1));
	
	len = mysql_escape_string(escaped, cinput_c, len);
	
	JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyN(cx, escaped, len)));
	
	free(escaped);
	JS_free(cx, cinput_c);
	
	return JS_TRUE;
}

/**
 * Execute a mysql query.
 *
 * @name Ape.MySQL.query
 * @function
 * @public
 *
 * @param {string} sql The SQL statement
 * @param {function} fn callback function
 * @param {Array} fn.res An array of objects if the query was a 'SELECT'
 * @param {integer} fn.errorNo if an error occured the errorNo != 0
 * @returns {void}
 *
 * @example
 * sql.query('SELECT * FROM table', function(res, errorNo) {
 * 	if (errorNo) Ape.log('Request error : ' + errorNo + ' : ' + this.errorString());
 * 	else {
 * 		Ape.log('Fetching ' + res.length);
 * 		for(var i = 0; i < res.length; i++) {
 * 			Ape.log(res[i].title);//res[i].<column name>
 * 		});
 * 	}
 * });
 * @example
 * sql.query('INSERT INTO table VALUES("a", "b", "c")', function(res, errorNo) {
 * 	if (errorNo) Ape.log('Request error : ' + errorNo + ' : ' + this.errorString());
 * 	else Ape.log('Inserted');
 * });
 */
APE_JS_NATIVE(apemysql_sm_query)
//{
	JSString *query;
	struct _ape_mysql_data *myhandle;
	jsval callback;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	
	if ((myhandle = JS_GetPrivate(cx, obj)) == NULL) {
		return JS_TRUE;
	}
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &query)) {
		return JS_TRUE;
	}
	if (!JS_ConvertValue(cx, JS_ARGV(cx, vpn)[1], JSTYPE_FUNCTION, &callback)) {
		return JS_TRUE;
	}

	apemysql_push_queue(myhandle, JS_EncodeString(cx, query), JS_GetStringEncodingLength(cx, query), callback);
	
	return JS_TRUE;
}
#endif

static JSFunctionSpec apesocket_funcs[] = {
	JS_FS("write",   apesocket_write,	1, 0),
	JS_FS("close",   apesocket_close,	0, 0),
	JS_FS_END
};

static JSFunctionSpec apesocketserver_funcs[] = {
	JS_FS("close",   apesocketserver_close,	0, 0),
	JS_FS_END
};

static JSFunctionSpec apesocketclient_funcs[] = {
	JS_FS("write",   apesocketclient_write,	1, 0),
	JS_FS("close",   apesocketclient_close,	0, 0),
	JS_FS_END
};

static JSFunctionSpec apesocket_client_funcs[] = {
	JS_FS("onAccept", ape_sm_stub, 0, 0),
	JS_FS("onRead", ape_sm_stub, 0, 0),
	JS_FS("onDisconnect", ape_sm_stub, 0, 0),
	JS_FS("onConnect", ape_sm_stub, 0, 0),
	JS_FS_END
};

static JSFunctionSpec apeuser_funcs[] = {
	JS_FS("getProperty", apeuser_sm_get_property, 1, 0),
	JS_FS("delProperty", apeuser_sm_del_property, 1, 0),
	JS_FS("setProperty", apeuser_sm_set_property, 2, 0),
	JS_FS("join", apeuser_sm_join, 1, 0),
	JS_FS("left", apeuser_sm_left, 1, 0),
	JS_FS("quit", apeuser_sm_quit, 0, 0),
	JS_FS_END
};

static JSFunctionSpec apechannel_funcs[] = {
	JS_FS("getProperty", apechannel_sm_get_property, 1, 0),
	JS_FS("delProperty", apechannel_sm_del_property, 1, 0),
	JS_FS("setProperty", apechannel_sm_set_property, 2, 0),
	JS_FS("isInteractive", apechannel_sm_isinteractive, 1, 0),
	JS_FS_END
};

/**
 * Trigger a event if something is send on a pipe.
 *
 * @name Ape.pipe.onSend
 * @event
 * @public
 *
 * @see Ape.pipe
 * @see Ape.pipe.sendRaw
 * @see Ape.pipe
 * @see Ape.getPipe
 *
 */
static JSFunctionSpec apepipe_funcs[] = {
	JS_FS("sendRaw", apepipe_sm_send_raw, 3, 0),
	JS_FS("toObject", apepipe_sm_to_object, 0, 0),
	JS_FS("getProperty", apepipe_sm_get_property, 1, 0),
	JS_FS("delProperty", apepipe_sm_del_property, 1, 0),
	JS_FS("setProperty", apepipe_sm_set_property, 2, 0),
	JS_FS("getParent", apepipe_sm_get_parent, 0, 0),
	JS_FS("onSend", ape_sm_stub, 0, 0),
	JS_FS_END
};

#ifdef _USE_MYSQL
/**
 * Mysql connection established callback.
 * <p>This callback is called when mysql connection (and database selection) has been established.</p>
 *
 * @name Ape.MySQL.onConnect
 * @event
 * @public
 *
 * @example
 * sql.onConnect = function() {
 * 	Ape.log('Connected to mysql server');
 * }
 * @example
 * var sql = new Ape.MySQL('ip:port', 'user', 'password', 'database');
 * sql.onConnect = function() {
 * 	Ape.log('Connected to mysql server');
 * }
 * sql.onError = function(errorNo) {
 * 	Ape.log('Connection Error : ' + errorNo + ' : ' + this.errorString());
 * }
 *
 * @see Ape.MySQL
 * @see Ape.MySQL.onError
 */


/**
 * Mysql error occurance callback.
 * <p>This callback is called when a mysql error occurs.</p>
 * <p>The error codes from MySac.</p>
 * <ol start="0">
 * <li>MYSAC_START</li>
 * <li>MYSAC_CONN_CHECK</li>
 * <li>MYSAC_READ_GREATINGS</li>
 * <li>MYSAC_SEND_AUTH_1</li>
 * <li>MYSAC_RECV_AUTH_1</li>
 * <li>MYSAC_SEND_AUTH_2</li>
 * <li>MYSAC_SEND_QUERY</li>
 * <li>MYSAC_RECV_QUERY_COLNUM</li>
 * <li>MYSAC_RECV_QUERY_COLDESC1</li>
 * <li>MYSAC_RECV_QUERY_COLDESC2</li>
 * <li>YSAC_RECV_QUERY_EOF1</li>
 * <li>MYSAC_RECV_QUERY_EOF2</li>
 * <li>MYSAC_RECV_QUERY_DATA</li>
 * <li>MYSAC_SEND_INIT_DB</li>
 * <li>MYSAC_RECV_INIT_DB</li>
 * <li>MYSAC_SEND_STMT_QUERY</li>
 * <li>MYSAC_RECV_STMT_QUERY</li>
 * <li>MYSAC_SEND_STMT_EXECUTE</li>
 * <li>MYSAC_RECV_STMT_EXECUTE</li>
 * <li>MYSAC_READ_NUM</li>
 * <li>MYSAC_READ_HEADER</li>
 * <li>MYSAC_READ_LINE</li>
 * </ol>
 *
 * @name Ape.MySQL.onError
 * @event
 * @public
 *
 * @param {integer} errorNo MySql MySac errorcode
 *
 * @example
 * sql.onError = function(errorNo) {
 * 	Ape.log('Connection Error : ' + errorNo + ' : ' + this.errorString());
 * }
 * @example
 * var sql = new Ape.MySQL('10.0.0.240:3306', 'user', 'password', 'database');
 * sql.onConnect = function() {
 * 	Ape.log('Connected to mysql server');
 * }
 * sql.onError = function(errorNo) {
 * 	Ape.log('Connection Error : ' + errorNo + ' : ' + this.errorString());
 * }
 *
 * @see Ape.MySQL
 * @see Ape.MySQL.onConnect
 * @see Ape.MySQL.errorString
 *
 * @fires error
 */
static JSFunctionSpec apemysql_funcs[] = {
	JS_FS("onConnect", ape_sm_stub, 0, 0),
	JS_FS("onError", ape_sm_stub, 0, 0),
	JS_FS("errorString", apemysql_sm_errorstring, 0, 0),
	JS_FS("query", apemysql_sm_query, 2, 0),
	JS_FS("getInsertId", apemysql_sm_insert_id, 0, 0),
	JS_FS_END
};

static JSFunctionSpec apemysql_funcs_static[] = {
	JS_FS("escape", apemysql_escape, 1, 0),
	JS_FS_END
};
#endif


static JSFunctionSpec cmdresponse_funcs[] = {
	JS_FS("sendResponse", apepipe_sm_send_response, 3, 0),
	JS_FS_END
};

static JSFunctionSpec apepipecustom_funcs[] = {
	JS_FS("destroy", apepipe_sm_destroy, 0, 0),
	JS_FS_END
};

static JSObject *sm_ape_socket_to_jsobj(JSContext *cx, ape_socket *client)
{
	/*if (client->data != NULL) {
		return (JSObject *)client->data;
	} else {*/
		
		JSObject *obj = JS_NewObject(cx, &apesocket_class, NULL, NULL);
		
		JS_AddObjectRoot(cx, &obj);

		JS_DefineFunctions(cx, obj, apesocketclient_funcs);
		JS_SetPrivate(cx, obj, client);
		
		JS_RemoveObjectRoot(cx, &obj);
		
		client->data = obj;
		return obj;
//	}
}

/**
 * Trigger a event if a clientSocket is connected to a serverSocket.
 *
 * @name Ape.sockClient.onAccept
 * @event
 * @public
 *
 * @param {sockServer} [socketServer]
 * @returns {void}
 *
 * @example
 * socket.onAccept = function(server) {
 * 	Ape.log('New server !');
 * 	server.write('Hello world\n');
 * }
 *
 * @see Ape.sockClient
 * @see Ape.sockServer
 *
 */
/**
 * Trigger a event if a client is connected to this socketServer.
 *
 * @name Ape.sockServer.onAccept
 * @event
 * @public
 *
 * @param {sockClient} [socketClient]
 * @returns {void}
 *
 * @example
 * socket.onAccept = function(client) {
 * 	Ape.log('New client !');
 * 	client.write('Hello world\n');
 * }
 *
 * @see Ape.sockClient
 * @see Ape.sockServer
 * @see Ape.sockClient.connect
 * @see Ape.sockServer.connect
 *
 */
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
		cbcopy->state = 1;
		
		client->attach = cbcopy;

		//JS_SetContextThread(cb->asc->cx);
		//JS_BeginRequest(cb->asc->cx);

			obj = JS_NewObject(cb->asc->cx, &apesocket_class, NULL, NULL);
			sock_obj->client_obj = obj;
			JS_AddObjectRoot(cb->asc->cx, &sock_obj->client_obj);
			
			JS_SetPrivate(cb->asc->cx, obj, cbcopy);
			JS_DefineFunctions(cb->asc->cx, obj, apesocket_funcs);
			params[0] = OBJECT_TO_JSVAL(sock_obj->client_obj);
			
			//JS_AddRoot(cb->asc->cx, &params[0]);
			
			JS_CallFunctionName(cb->asc->cx, cb->server_obj, "onAccept", 1, params, &rval);
			
			//JS_RemoveRoot(cb->asc->cx, &params[0]);
			
		//JS_EndRequest(cb->asc->cx);
		//JS_ClearContextThread(cb->asc->cx);

	}
}
/**
 * Trigger a event if the server closes a connection to this socketClient.
 *
 * @name Ape.sockClient.onDisconnect
 * @event
 * @public
 *
 * @returns {void}
 *
 * @example
 * socket.onDisconnect = function() {
 * 	Ape.log('Gone !');
 * }
 *
 * @see Ape.sockClient
 * @see Ape.sockServer
 *
 */

/**
 * Trigger a event if the client is disconnected.
 *
 * @name Ape.sockServer.onDisconnect
 * @event
 * @public
 *
 * @param {sockClient} [client] The client that has disconnected
 * @returns {void}
 *
 * @example
 * client.onDisconnect = function() {
 * 	Ape.log('Gone !');
 * }
 *
 * @see Ape.sockClient
 * @see Ape.sockServer
 *
 */
static void sm_sock_ondisconnect(ape_socket *client, acetables *g_ape)
{
	jsval rval;
	
	if (client->attach != NULL) {
		struct _ape_sock_callbacks *cb = ((struct _ape_sock_callbacks *)client->attach);
		JSObject *client_obj = ((struct _ape_sock_js_obj *)cb->private)->client_obj;
		
		//JS_SetContextThread(cb->asc->cx);
		//JS_BeginRequest(cb->asc->cx);

			if (client_obj != NULL) {
				jsval params[1];
				params[0] = OBJECT_TO_JSVAL(client_obj);
				
				JS_CallFunctionName(cb->asc->cx, cb->server_obj, "onDisconnect", 1, params, &rval);
				JS_SetPrivate(cb->asc->cx, client_obj, (void *)NULL);
				JS_RemoveObjectRoot(cb->asc->cx, &((struct _ape_sock_js_obj *)cb->private)->client_obj);
			} else {
				JS_CallFunctionName(cb->asc->cx, cb->server_obj, "onDisconnect", 0, NULL, &rval);
				JS_SetPrivate(cb->asc->cx, cb->server_obj, (void *)NULL);
				JS_RemoveObjectRoot(cb->asc->cx, &cb->server_obj);
			}
			
			free(cb->private);
			free(cb);
			
		//JS_EndRequest(cb->asc->cx);
		//JS_ClearContextThread(cb->asc->cx);
		
	}
}

/**
 * Trigger a event if something is read on a clientSocket.
 * This function is called when a server sends data on his socket.
 *
 * @name Ape.sockClient.onRead
 * @event
 * @public
 *
 * @param {string} data The data string that has been read
 * @returns {void}
 *
 * @example
 * socket.onDisconnect = function() {
 * 	Ape.log('Gone !');
 * }
 *
 * @see Ape.sockClient
 * @see Ape.sockServer
 *
 */

/**
 * Trigger a event if something is read on a serverSocket.
 * This function is called when a client sends data on his socket.
 *
 * @name Ape.sockServer.onRead
 * @event
 * @public
 *
 * @param {sockClient} [client] The client that send the data
 * @param {string} [data] The data string that has been read
 * @returns {void}
 *
 * @example
 * socket.onRead = function(client, data) {
 * 	Ape.log('Received: ' + data);
 * }
 *
 * @see Ape.sockClient
 * @see Ape.sockServer
 *
 */
static void sm_sock_onread_lf(ape_socket *client, char *data, acetables *g_ape)
{
	jsval rval;
	if (client->attach != NULL) {
		struct _ape_sock_callbacks *cb = ((struct _ape_sock_callbacks *)client->attach);
		JSObject *client_obj = ((struct _ape_sock_js_obj *)cb->private)->client_obj;
		
		if (!cb->state) {
			return;
		}
		//JS_SetContextThread(cb->asc->cx);
		//JS_BeginRequest(cb->asc->cx);
			
			if (client_obj != NULL) {
				jsval params[2];
				params[0] = OBJECT_TO_JSVAL(client_obj);
				params[1] = STRING_TO_JSVAL(JS_NewStringCopyZ(cb->asc->cx, data));

				JS_CallFunctionName(cb->asc->cx, cb->server_obj, "onRead", 2, params, &rval);
			} else {
				jsval params[1];
				params[0] = STRING_TO_JSVAL(JS_NewStringCopyZ(cb->asc->cx, data));

				JS_CallFunctionName(cb->asc->cx, cb->server_obj, "onRead", 1, params, &rval);
		
			}
		JS_MaybeGC(cb->asc->cx);
		//JS_EndRequest(cb->asc->cx);
		//JS_ClearContextThread(cb->asc->cx);
		
	}
}

/**
 * Trigger a event if a connection has been established.
 *
 * @name Ape.sockClient.onConnect
 * @event
 * @public
 *
 * @returns {void}
 *
 * @example
 * socket.onConnect = function() {
 * 	Ape.log('We are connected !');
 * 	this.write('Hello\n');
 * }
 *
 * @see Ape.sockClient
 * @see Ape.sockServer
 * @see Ape.sockClient.accept
 * @see Ape.sockServer.accept
 *
 */

/**
 * Trigger a event if a connection has been established.
 *
 * @name Ape.sockServer.onConnect
 * @event
 * @public
 *
 * @returns {void}
 *
 * @example
 * socket.onConnect = function() {
 * 	Ape.log('We are connected !');
 * 	this.write('Hello\n');
 * }
 *
 * @see Ape.sockClient
 * @see Ape.sockServer
 */
static void sm_sock_onconnect(ape_socket *client, acetables *g_ape)
{
	jsval rval;
	
	if (client->attach != NULL) {
		struct _ape_sock_callbacks *cb = ((struct _ape_sock_callbacks *)client->attach);
		((struct _ape_sock_js_obj *)cb->private)->client = client;
		//JS_SetContextThread(cb->asc->cx);
		//JS_BeginRequest(cb->asc->cx);
			JS_CallFunctionName(cb->asc->cx, cb->server_obj, "onConnect", 0, NULL, &rval);
		//JS_EndRequest(cb->asc->cx);
		//JS_ClearContextThread(cb->asc->cx);

	}
}

static void sm_sock_onread(ape_socket *client, ape_buffer *buf, size_t offset, acetables *g_ape)
{
	jsval rval;
	if (client->attach != NULL) {
		struct _ape_sock_callbacks *cb = ((struct _ape_sock_callbacks *)client->attach);
		JSObject *client_obj = ((struct _ape_sock_js_obj *)cb->private)->client_obj;

		if (!cb->state) {
			return;
		}
		//JS_SetContextThread(cb->asc->cx);
		//JS_BeginRequest(cb->asc->cx);
			
			if (client_obj != NULL) {
				jsval params[2];
				params[0] = OBJECT_TO_JSVAL(client_obj);
				params[1] = STRING_TO_JSVAL(JS_NewStringCopyN(cb->asc->cx, buf->data, buf->length));
				JS_CallFunctionName(cb->asc->cx, cb->server_obj, "onRead", 2, params, &rval);
			} else {
				jsval params[1];
				
				params[0] = STRING_TO_JSVAL(JS_NewStringCopyN(cb->asc->cx, buf->data, buf->length));

				JS_CallFunctionName(cb->asc->cx, cb->server_obj, "onRead", 1, params, &rval);

			}
		JS_MaybeGC(cb->asc->cx);
		//JS_EndRequest(cb->asc->cx);
		//JS_ClearContextThread(cb->asc->cx);
		buf->length = 0;
	}
}

/* Reporting error from JS compilation (parse error, etc...) */
static void reportError(JSContext *cx, const char *message, JSErrorReport *report)
{
	fprintf(stderr, "%s:%u:%s at token: %s\n",
		report->filename ? report->filename : "<no filename>",
		(unsigned int) report->lineno,
		message,
		report->tokenptr);
}

static JSObject *ape_json_to_jsobj(JSContext *cx, json_item *head, JSObject *root)
{
	while (head != NULL) {
		if (head->jchild.child == NULL && head->key.val != NULL) {
			jsval jval;

			if (root == NULL) {
				root = JS_NewObject(cx, NULL, NULL, NULL);
			}
			
			if (head->jval.vu.str.value != NULL) {
				jval = STRING_TO_JSVAL(JS_NewStringCopyN(cx, head->jval.vu.str.value, head->jval.vu.str.length));
			} else {
				jsdouble dp = (head->jval.vu.integer_value ? head->jval.vu.integer_value : head->jval.vu.float_value);
				JS_NewNumberValue(cx, dp, &jval);
			}
			JS_SetProperty(cx, root, head->key.val, &jval);
		} else if (head->key.val == NULL && head->jchild.child == NULL) {
			jsuint rval;
			jsval jval;

			if (root == NULL) {
				root = JS_NewArrayObject(cx, 0, NULL);
			}
			
			if (head->jval.vu.str.value != NULL) {
				jval = STRING_TO_JSVAL(JS_NewStringCopyN(cx, head->jval.vu.str.value, head->jval.vu.str.length));
			} else {
				jsdouble dp = (head->jval.vu.integer_value ? head->jval.vu.integer_value : head->jval.vu.float_value);
				JS_NewNumberValue(cx, dp, &jval);
			}

			if (JS_GetArrayLength(cx, root, &rval)) {
				JS_SetElement(cx, root, rval, &jval);
			}
		} else if (head->jchild.child != NULL) {
			JSObject *cobj = NULL;

			switch(head->jchild.type) {
				case JSON_C_T_OBJ:
					cobj = JS_NewObject(cx, NULL, NULL, NULL);
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

				if (root == NULL) {
					root = JS_NewObject(cx, NULL, NULL, NULL);
				}
				
				jval = OBJECT_TO_JSVAL(cobj);
				JS_SetProperty(cx, root, head->key.val, &jval);
			} else {
				jsval jval;
				jsuint rval;

				if (root == NULL) {
					root = JS_NewArrayObject(cx, 0, NULL);
				}
				
				jval = OBJECT_TO_JSVAL(cobj);
				
				if (JS_GetArrayLength(cx, root, &rval)) {
					JS_SetElement(cx, root, rval, &jval);
				}
			}

			
		}
		head = head->next;
	}
	return root;
}

static void ape_sm_pipe_on_send_wrapper(transpipe *pipe, USERS *user, json_item *jstr, acetables *g_ape)
{
	JSObject *obj;
	jsval params[2], rval;
	JSContext *cx = get_property(pipe->properties, "cx")->val;

	obj = ape_json_to_jsobj(cx, jstr->jchild.child, NULL);
	
	params[0] = OBJECT_TO_JSVAL(APEUSER_TO_JSOBJ(user));
	params[1] = OBJECT_TO_JSVAL(obj);
	
	JS_CallFunctionName(cx, pipe->data, "onSend", 2, params, &rval);
	
	JS_RemoveObjectRoot(cx, &obj);
}

/* Dispatch CMD to the corresponding javascript callback */
static unsigned int ape_sm_cmd_wrapper(callbackp *callbacki)
{
	acetables *g_ape = callbacki->g_ape;
	ape_sm_compiled *asc = ASMR->scripts;
	struct _http_header_line *hlines;
	
	json_item *head = callbacki->param;
	JSContext *cx = ASMC;
	
	JSObject *obj; // param object
	JSObject *cb; // cmd object
	JSObject *hl; // http object
	
	if (asc == NULL) {
		return (RETURN_NOTHING);
	}

	//JS_SetContextThread(cx);
	//JS_BeginRequest(cx);
		jsval jval;

		obj = ape_json_to_jsobj(cx, head, NULL);
		JS_AddObjectRoot(cx, &obj);
		
		cb = JS_NewObject(cx, &cmdresponse_class, NULL, NULL);
		JS_AddObjectRoot(cx, &cb);
		JS_DefineFunctions(cx, cb, cmdresponse_funcs);
		
		jval = STRING_TO_JSVAL(JS_NewStringCopyZ(cx, callbacki->host));
		/* infos.host */
		JS_SetProperty(cx, cb, "host", &jval);
		
		hl = JS_DefineObject(cx, cb, "http", NULL, NULL, 0);
		
		for (hlines = callbacki->hlines; hlines != NULL; hlines = hlines->next) {
			s_tolower(hlines->key.val, hlines->key.len);
			jval = STRING_TO_JSVAL(JS_NewStringCopyN(cx, hlines->value.val, hlines->value.len));
			JS_SetProperty(cx, hl, hlines->key.val, &jval);
		}

		jval = OBJECT_TO_JSVAL(sm_ape_socket_to_jsobj(cx, callbacki->client));
		JS_SetProperty(cx, cb, "client", &jval);
		
		jval = INT_TO_JSVAL(callbacki->chl);
		/* infos.chl */
		JS_SetProperty(cx, cb, "chl", &jval);

        /* Attempts to get an IP from proxy request first */
        JS_GetProperty(cx, hl, "x-forwarded-for", &jval);
		if ( !JSVAL_TO_BOOLEAN(jval) ) {
		    /* Get the IP from the socket */
    		jval = STRING_TO_JSVAL(JS_NewStringCopyZ(cx, callbacki->ip));
        }

		JS_SetProperty(cx, cb, "ip", &jval);
		
		if (callbacki->call_user != NULL) {
			jval = OBJECT_TO_JSVAL(APEUSER_TO_JSOBJ(callbacki->call_user));
			/* infos.user */
			JS_SetProperty(cx, cb, "user", &jval);
			
			if (callbacki->call_subuser != NULL) {
				jval = OBJECT_TO_JSVAL(APESUBUSER_TO_JSOBJ(callbacki->call_subuser));
				/* infos.subuser */
				JS_SetProperty(cx, cb, "subuser", &jval);
			}
		}
		
		JS_RemoveObjectRoot(cx, &obj);
		JS_RemoveObjectRoot(cx, &cb);
		
	//JS_EndRequest(cx);
	//JS_ClearContextThread(cx);
	
	if (callbacki->data == NULL) {
		return ape_fire_cmd(callbacki->cmd, obj, cb, callbacki, callbacki->g_ape);
	} else {
		return ape_fire_hook(callbacki->data, obj, cb, callbacki, callbacki->g_ape);
	}
	
	return (RETURN_NOTHING);
}

/**
 * Catch all commands that are not registered.
 *
 * @name Ape.registerHookBadCmd
 * @function
 * @public
 *
 * @param {function} fn This function will be called when an user uses the CMD command.
 * @param {object} params The list of parameters sent by the client.
 * @param {object} info Contains information about the client:
 * @param {string} info.host Host sent on HTTP headers.
 * @param {sockClient} info.client The client socket's object.
 * @param {integer} info.chl The challenge number.
 * @param {string} info.ip The client's IP.
 * @param {user} info.user User object (if logged in).
 * @param {subuser} info.subuser User object (if logged in).
 * @param {Array} info.http HTTP headers.
 * @param {string} raw the raw received
 * @returns {void}
 *
 * @example
 * //log
 * Ape.registerHookBadCmd(function(params, info, raw){
 * 		Ape.log('Bad RAW received (' + raw + ').');
 * 		//We return nothing so client will receive a BAD_CMD error
 * });
 * @example
 * //Forward all
 * Ape.registerHookBadCmd(function(params, info, raw) {
 * 	if(!params.pipe) {
 * 		return 0;//We return 0 so client will receive BAD_PARAMS error
 * 	}
 * 	var pipe = Ape.getPipe(params.pipe);
 * 	if (!pipe) {
 * 		return [109, 'UNKNOWN_PIPE']; //Client will receive UNKNOWN_PIPE error
 * 	}
 * 	pipe.sendRaw(raw, params, {from: info.user});
 * });
 *
 * @see Ape.registerHookCmd
 * @see Ape.registerCmd
 * @see Ape.subuser
 * @see Ape.user
 * @see Ape.sockClient
 */
APE_JS_NATIVE(ape_sm_register_bad_cmd)
//{
	ape_sm_callback *ascb;

	ascb = JS_malloc(cx, sizeof(*ascb));

	if (!JS_ConvertValue(cx, JS_ARGV(cx, vpn)[0], JSTYPE_FUNCTION, &ascb->func)) {
		JS_free(cx, ascb);
		return JS_TRUE;
	}
	JS_AddValueRoot(cx, &ascb->func);

	/* TODO : Effacer si déjà existant (RemoveRoot & co) */
	ascb->next = NULL;
	ascb->type = APE_BADCMD;
	ascb->cx = cx;
	ascb->callbackname = NULL;
	
	if (asc->callbacks.head == NULL) {
		asc->callbacks.head = ascb;
		asc->callbacks.foot = ascb;
	} else {
		asc->callbacks.foot->next = ascb;
		asc->callbacks.foot = ascb;
	}
	
	register_bad_cmd(ape_sm_cmd_wrapper, ascb, g_ape);
	
	return JS_TRUE;
	
}

/**
 * Register a new server command.
 *
 * @param {string} name The CMD name to listen to
 * @name Ape.registerCmd
 * @function
 * @public
 *
 * @param {boolean} require_sess_id Does this command requires a connected user (is sessid required) ?
 * @param {function} fn This function will be called when an user uses the CMD command.
 * @param {object} params The list of parameters sent by the client.
 * @param {object} info Contains information about the client:
 * @param {string} info.host Host sent on HTTP headers.
 * @param {sockClient} info.client The client socket's object.
 * @param {integer} info.chl The challenge number.
 * @param {string} info.ip The client's IP.
 * @param {user} info.user User object (if logged in).
 * @param {subuser} info.subuser User object (if logged in).
 * @param {Array} info.http HTTP headers.
 * @returns {void}
 *
 * @example
 * Ape.registerCmd('foocmd', true, function(params, info) {
 * 	Ape.log("The user ip : (' + info.ip + '), foo : ' + params.foo);
 * });
 * @example
 * // You can return error in two ways:
 * // By returning 0 to return a "BAD_PARAMS"
 * // By returning an array [code, error] for custom errors.
 * Ape.registerCmd('foocmd', true, function(params, info) {
 * 	if (!$defined(params.john)) return 0; // send a "BAD_PARAMS" RAW to the user
 * 	if (params.john != 'doe') return ['209', "NOT_A_JOHN_DOE"];
 * 	return 1;
 * });
 * @example
 * //Return an object to send a raw as response.
 * Ape.registerCmd( 'gettime', true, function(params, info) ) {
 * 	return {
 * 		name: 'TIME',//The raw's name
 * 		data: { time: new Date().getTime() }//The raw's data (must be an object)
 * 	}
 * }
 *
 * @see Ape.registerHookCmd
 * @see Ape.registerHookBadCmd
 * @see Ape.subuser
 * @see Ape.user
 * @see Ape.sockClient
 */
APE_JS_NATIVE(ape_sm_register_cmd)
//{
	JSString *cmd;
	JSBool needsessid;
	char *ccmd;

	ape_sm_callback *ascb;

	JS_SET_RVAL(cx, vpn, JSVAL_NULL);
	
	if (argc != 3) {
		return JS_TRUE;
	}
	
	if (!JS_ConvertArguments(cx, 2, JS_ARGV(cx, vpn), "Sb", &cmd, &needsessid)) {
		return JS_TRUE;
	}
	
	ascb = JS_malloc(cx, sizeof(*ascb));

	if (!JS_ConvertValue(cx, JS_ARGV(cx, vpn)[2], JSTYPE_FUNCTION, &ascb->func)) {
		JS_free(cx, ascb);
		return JS_TRUE;
	}
	JS_AddValueRoot(cx, &ascb->func);
	
	ccmd = JS_EncodeString(cx, cmd);

	/* TODO : Effacer si déjà existant (RemoveRoot & co) */
	ascb->next = NULL;
	ascb->type = APE_CMD;
	ascb->cx = cx;
	ascb->callbackname = ccmd;
	
	if (asc->callbacks.head == NULL) {
		asc->callbacks.head = ascb;
		asc->callbacks.foot = ascb;
	} else {
		asc->callbacks.foot->next = ascb;
		asc->callbacks.foot = ascb;
	}
	
	register_cmd(ccmd, ape_sm_cmd_wrapper, (needsessid == JS_TRUE ? NEED_SESSID : NEED_NOTHING), g_ape);
	
	return JS_TRUE;
}

/**
 * Register a hook on a server command.
 * <p>APE server commes with the following pre-registered CMD's</p>
 * <ul>
 * <li>CONNECT Returns a RAW IDENT  set a sessid on the user an sends a RAW LOGIN</li>
 * <li> SCRIPT Sends the files thaw were requested in the json array</li>
 * <li> CHECK  (sessid needed) does nothing</li>
 * <li> SEND (sessid needed) send a Raw Data to the requested pipe</li>
 * <li> QUIT (sessid needed) sends a Raw quit and makes the user quit</li>
 * <li> JOIN (sessid needed) Joins the user on the channel if possible. If the channel does not exist, it is created</li>
 * <li> LEFT (sessid needed) makes a user leave a channel or sends error {103 UNKNOWN, 104 NOT_IN_CHANNEL}</li>
 * <li> SESSION (sessid needed) sets or gets session propertyies and returns with Raw SESSIONS or sends a error (203 SESSION_ERROR BAD_PARAMS)</li>
 *</ul>
 * @name Ape.registerHookCmd
 * @function
 * @public
 *
 * @param {string} name The CMD name to listen to
 * @param {function} fn This function will be called when an user uses the CMD command.
 * @param {object} params The list of parameters sent by the client.
 * @param {object} info Contains information about the client:
 * @param {string} info.host Host sent on HTTP headers.
 * @param {sockClient} info.client The client socket's object.
 * @param {integer} info.chl The challenge number.
 * @param {string} info.ip The client's IP.
 * @param {user} info.user User object (if logged in).
 * @param {subuser} info.subuser User object (if logged in).
 * @param {Array} info.http HTTP headers.
 * @returns {void}
 *
 * @example
 * Ape.registerHookCmd('foocmd', function(params, info) {
 * 	if (!$defined(params.john)) return 0;
 * 	return 1;
 * });
 *
 * @see Ape.registerCmd
 * @see Ape.registerHookBadCmd
 * @see Ape.subuser
 * @see Ape.user
 * @see Ape.sockClient
 */
APE_JS_NATIVE(ape_sm_hook_cmd)
//{
	JSString *cmd;
	char *ccmd;

	ape_sm_callback *ascb;

	JS_SET_RVAL(cx, vpn, JSVAL_NULL);
	
	if (argc != 2) {
		return JS_TRUE;
	}
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &cmd)) {
		return JS_TRUE;
	}

	ascb = JS_malloc(cx, sizeof(*ascb));

	if (!JS_ConvertValue(cx, JS_ARGV(cx, vpn)[1], JSTYPE_FUNCTION, &ascb->func)) {
		JS_free(cx, ascb);
		return JS_TRUE;
	}
	
	ccmd = JS_EncodeString(cx, cmd);

	if (!register_hook_cmd(ccmd, ape_sm_cmd_wrapper, ascb, g_ape)) {
		/* CMD doesn't exist */
		JS_free(cx, ccmd);
		JS_free(cx, ascb);
		return JS_TRUE;
	}
	JS_AddValueRoot(cx, &ascb->func);

	/* TODO : Effacer si déjà existant (RemoveRoot & co) */
	ascb->next = NULL;
	ascb->type = APE_HOOK;
	ascb->cx = cx;
	ascb->callbackname = ccmd; /* TODO: ccmd leak */
	
	if (asc->callbacks.head == NULL) {
		asc->callbacks.head = ascb;
		asc->callbacks.foot = ascb;
	} else {
		asc->callbacks.foot->next = ascb;
		asc->callbacks.foot = ascb;
	}
		
	return JS_TRUE;
}

/**
 * Include another javascript file in the global context.
 *
 * @name include
 * @function
 * @public
 *
 * @param {string} filename The filename relative to the 'scripts' folde
 * @returns {void}
 *
 * @example
 * var inc = './scripts/foo.js';
 * include(inc);
 * Ape.log('some variable set in ' + inc + ' : ' + myfoovar);
 */
APE_JS_NATIVE(ape_sm_include)
//{
	JSString *file;
	char *cfile;
	JSObject *bytecode;
	jsval frval;
	char rpath[512];
	
	if (argc != 1) {
		return JS_TRUE;
	}
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &file)) {
		return JS_TRUE;
	}
	
	cfile = JS_EncodeString(cx, file);
	
	memset(rpath, '\0', sizeof(rpath));
	strncpy(rpath, READ_CONF("scripts_path"), 255);
	strncat(rpath, cfile, 255);
	
	JS_free(cx, cfile);
	
	if (!g_ape->is_daemon) {
		printf("[%s] : Loading script %s\n", MODULE_NAME , rpath);
	}
	ape_log(APE_INFO, __FILE__, __LINE__, g_ape, "[%s] : Loading script %s", MODULE_NAME , rpath);
	bytecode = JS_CompileFile(cx, JS_GetGlobalObject(cx), rpath);
	
	if (bytecode == NULL) {
		JSBool pending;
		jsval exception = JSVAL_VOID;
		pending = JS_IsExceptionPending(cx);
		if (pending) {
			if (JS_GetPendingException(cx, &exception)){
				JS_ReportPendingException(cx);
				JS_ClearPendingException(cx);
			}
		}
		if (!g_ape->is_daemon) {
			printf("[%s] Failed loading script %s\n", MODULE_NAME, rpath);
		}
		ape_log(APE_ERR, __FILE__, __LINE__, g_ape, "[%s] Failed loading script %s", MODULE_NAME, rpath);
		return JS_TRUE;
	}

	JS_ExecuteScript(cx, JS_GetGlobalObject(cx), bytecode, &frval);
	
	return JS_TRUE;
}

/**
 * Write a string to a file.
 *
 * @name Ape.os.writefile
 * @public
 * @static
 * @function
 *
 * @param {string} 	filename 	Filename to write to. If the filename is '', then a temporary file will be created (/tmp/apeXXXXXX)
 * @param {string} 	content 	The content that will be written to the file
 * @param {boolean} 	append 		Append to the file: False: create a new file. True: appends if the file exists, else it creates a new one:
 * @returns {boolean} 	True on success, false on failure, null on incorrect parameters
 *
 * @example
 * var content = os.readwrite('/tmp/dummy.txt', 'blabla');
 */
APE_JS_NATIVE(ape_sm_writefile)
//{
	JSString *content;
	JSString *filename;
	JSBool append;
	jsval ret;
	char *ccontent;
	char *cfilename;
	char mode[3] = {'w', 'b', '+'};
	struct stat sb;
	FILE *fOut;
	char tfilename[16] = {'\0'};
	int rc = 0;
	int temp = 0 ;
	JS_SET_RVAL(cx, vpn, JSVAL_NULL);
	if (argc != 3) {
		return JS_TRUE;
	}
	if (!JS_ConvertArguments(cx, 3, JS_ARGV(cx, vpn), "SSb", &filename, &content, &append)) {
		return JS_TRUE;
	}
	cfilename = JS_EncodeString(cx, filename);
	ccontent = JS_EncodeString(cx, content);
	if (*cfilename == '\0' ) {
		temp = 1;
		strcpy(tfilename, "/tmp/apeXXXXXX");
		strcpy(tfilename , mktemp(tfilename));
		fOut = fopen (tfilename, mode);
	}else{
		if (append == JS_TRUE && (stat(cfilename, &sb) == 0 && S_ISREG(sb.st_mode))) {
			mode[0] ='a';
		}
		fOut = fopen (cfilename, mode);
	}
	if (fOut != NULL) {
		if (fputs (ccontent, fOut) != EOF) {
			rc = 1;
		}
		if (fclose (fOut) == EOF) {
			rc = 0;
		}
	}
	if (rc > 0 || (rc == 0 && *ccontent == '\0')) {
		if (temp == 1) {
			ret = STRING_TO_JSVAL(JS_NewStringCopyN(cx, tfilename, strlen(tfilename)));
		} else {
			ret = JSVAL_TRUE;
		}
	} else {
		ret = JSVAL_FALSE;
	}
	JS_SET_RVAL(cx, vpn, ret);
	JS_free(cx, cfilename);
	JS_free(cx, ccontent);
	return JS_TRUE;
}

/**
 * Get the content of a file.
 *
 * @name Ape.readfile
 * @function
 * @public
 * @static
 * @ignore
 * @deprecated Please use Ape.os.readfile instead
 * 
 * @param {string} filename The filename to read
 * @returns {string} The content of the file or NULL
 *
 * @example
 * var content = Ape.readfile('/etc/hosts');
 *
 * @see Ape.os.readfile
 */
/**
 * Get the content of a file.
 *
 * @name Ape.os.readfile
 * @function
 * @public
 * @static
 * @ignore
 * 
 * @param {string} filename The filename to read
 * @returns {string} The content of the file or NULL
 *
 * @example
 * var content = os.readfile('/etc/hosts');
 */
APE_JS_NATIVE(ape_sm_readfile)
//{
	JSString *string;
	char *cstring;
	char *content;
	FILE *fp;
	const int bufferSize = 2048;
	int size = bufferSize;
	int fsize = 0;
	JS_SET_RVAL(cx, vpn, JSVAL_NULL);
	if (argc != 1) {
		return JS_TRUE;
	}
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &string)) {
		return JS_TRUE;
	}
	cstring = JS_EncodeString(cx, string);
	fp = fopen(cstring, "r");
	if (fp == NULL) {
		return JS_TRUE;
	}
	content = xmalloc(sizeof(char) * size);
	while (!feof(fp)) {
		int tmp;
		if ((tmp = fread(content + fsize, sizeof(char), bufferSize, fp)) > 0) {
			fsize += tmp * sizeof(char);
		}
		size += bufferSize;
		content = xrealloc(content, sizeof(char) * size);
	}
	JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyN(cx, content, fsize)));
	fclose(fp);
	free(content);
	JS_free(cx, cstring);
	return JS_TRUE;
}

/**
 * Encode a string in base64.
 *
 * @name Ape.b64.encode
 * @function
 * @public
 * @static
 *
 * @param {string} text A string.
 * @returns {string} The encoded string.
 *
 * @example
 * var foo = Ape.b64.encode('http://www.ape-project.org');
 *
 * @see Ape.b64.decode
 */
APE_JS_NATIVE(ape_sm_b64_encode)
//{
	JSString *string;
	char *b64, *cstring;
	
	if (argc != 1) {
		return JS_TRUE;
	}
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &string)) {
		return JS_TRUE;
	}
	
	cstring = JS_EncodeString(cx, string);
	
	b64 = base64_encode((unsigned char *)cstring, JS_GetStringEncodingLength(cx, string));
	
	JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyZ(cx, b64)));
	
	JS_free(cx, cstring);
	free(b64);
	
	return JS_TRUE;
	
}

/**
 * Decode a base64 string.
 *
 * @name Ape.b64.decode
 * @function
 * @public
 * @static
 *
 * @param {string} text A base64 encoded string.
 * @returns {string} The decoded string.
 *
 * @example
 * var foo = Ape.b64.decode('aHR0cDovL3d3dy5hcGUtcHJvamVjdC5vcmc=');
 * @see Ape.b64.encode
 */
APE_JS_NATIVE(ape_sm_b64_decode)
//{
	JSString *string;
	char *b64, *cstring;
	int length, len;
	
	if (argc != 1) {
		return JS_TRUE;
	}
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &string)) {
		return JS_TRUE;
	}
	
	length = JS_GetStringEncodingLength(cx, string);
	cstring = JS_EncodeString(cx, string);
	
	b64 = xmalloc(length+1);
	len = base64_decode((unsigned char *)b64, cstring, length+1);
	
	if (len != -1) {
		JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyN(cx, b64, len)));
	}
	
	JS_free(cx, cstring);
	free(b64);
	
	return JS_TRUE;
}

/**
 * Encode a string using sha1 algorithm as binary.
 * <p>You can specify the secret key has second argument. The result is returned has raw binary.</p>
 *
 * @name Ape.sha1.bin
 * @function
 * @public
 * @static
 *
 * @param {String} text A string.
 * @param {String} [secret] The secret key to use.
 * @returns {string} The sha1 digest string in raw binary format
 *
 * @example
 * var result = Ape.sha1.bin('hello world');
 *
 * @see Ape.sha.str
 */
APE_JS_NATIVE(ape_sm_sha1_bin)
//{
	JSString *string, *hmac = NULL;
	char *cstring, *chmac;
	unsigned char digest[20];

	if (!JS_ConvertArguments(cx, argc, JS_ARGV(cx, vpn), "S/S", &string, &hmac)) {
		return JS_TRUE;
	}
	
	cstring = JS_EncodeString(cx, string);
	
	if (hmac == NULL) {
		sha1_csum((unsigned char *)cstring, JS_GetStringEncodingLength(cx, string), digest);
	} else {
		chmac = JS_EncodeString(cx, hmac);
		sha1_hmac((unsigned char *)chmac, JS_GetStringEncodingLength(cx, hmac), (unsigned char *)cstring, JS_GetStringEncodingLength(cx, string), digest);
		JS_free(cx, chmac);
	}
	
	JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyN(cx, (char *)digest, 20)));
	
	JS_free(cx, cstring);
	
	return JS_TRUE;
}

/**
 * Encode a string using sha1 algorithm.
 * <p>You can specify the secret key has second argument.</p>
 *
 * @name Ape.sha1.str
 * @function
 * @public
 * @static
 *
 * @param {String} text A string.
 * @param {String} [secret] The secret key to use.
 * @returns {string} The sha1 digest
 *
 * @example
 * var result = Ape.sha1.str('hello world');
 * //result = 2aae6c35c94fcfb415dbe95f408b9ce91ee846ed
 * var result = Ape.sha1.str('hello world', 'mysecretkey');
 * //result = 529a42c50c09857dbe9a5891d6b453d543aaf434
 * @see Ape.sha.bin
 */
APE_JS_NATIVE(ape_sm_sha1_str)
//{
	JSString *string, *hmac = NULL;
	unsigned char digest[20];
	char *cstring, *chmac;
	char output[40];
	unsigned int i;
	
	if (!JS_ConvertArguments(cx, argc, JS_ARGV(cx, vpn), "S/S", &string, &hmac)) {
		return JS_TRUE;
	}

	cstring = JS_EncodeString(cx, string);
	
	if (hmac == NULL) {
		sha1_csum((unsigned char *)cstring, JS_GetStringEncodingLength(cx, string), digest);
	} else {
		chmac = JS_EncodeString(cx, hmac);
		sha1_hmac((unsigned char *)chmac, JS_GetStringEncodingLength(cx, hmac), (unsigned char *)cstring, JS_GetStringEncodingLength(cx, string), digest);
		JS_free(cx, chmac);
	}
	
	for (i = 0; i < 20; i++) {
		sprintf(output + (i*2), "%02x", digest[i]);
	}
	
	JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyN(cx, output, 40)));
	
	JS_free(cx, cstring);
	
	return JS_TRUE;
}

/**
 * Instantiate a channel object.
 * <p>Used to create a empty channel, can be useful to add user this channel before users join it.</p>
 * <p>Character that are allowed in channel names are a-Z and 0-9, 40 Characters long.</p>
 *
 * @name Ape.mkChan
 * @constructs
 * @lends channel.prototype
 * @public
 *
 * param {string} name The channel name (channel will not be interactive if it stats with an "*")
 * returns {Ape.channel} The created channel object
 *
 * @example
 * var channel = Ape.mkChan('my_channel');
 *
 * @see Ape.getChannelByName
 * @see Ape.getChannelByPubid
 * @see Ape.rmChan
 * @see Ape.channel
 */

/**
 * This event is fired when a channel is created
 *
 * @name Ape.mkChan
 * @event
 * @public
 *
 * @param {channnel} channel Channel that is created
 * @returns {void}
 *
 * @example
 * Ape.addEvent('mkChan', function(channel) {
 * 	if(channel.getProperty('name') == 'room_101'){
 * 		oceania.enemy= eurasia ;
 * 		eastasia.enemy = null;
 * 		eurasia.enemy = oceania;
 * 	}
 * });
 *
 * @see Ape.addEvent
 * @see Ape.rmChan
 */
APE_JS_NATIVE(ape_sm_mkchan)
//{
	JSString *chan_name;
	char *cchan_name;
	CHANNEL *new_chan;
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &chan_name)) {
		return JS_TRUE;
	}
	
	cchan_name = JS_EncodeString(cx, chan_name);
	
	if (getchan(cchan_name, g_ape) != NULL) {
		JS_free(cx, cchan_name);
		return JS_TRUE;
	}
	
	if ((new_chan = mkchan(cchan_name, 0, g_ape)) != NULL) {
		JS_SET_RVAL(cx, vpn, OBJECT_TO_JSVAL(APECHAN_TO_JSOBJ(new_chan)));
	}
	
	JS_free(cx, cchan_name);
	
	return JS_TRUE;
}

/**
 * Remove a channel object.
 * <p>Remove a channel and send a left event to all user on the channel.</p>
 *
 * @name Ape.rmChan
 * @function
 * @public
 *
 * param {string|channel} channel The channel
 * returns {void}
 *
 * @example
 * Ape.rmChan('my_channel');
 *
 * @see Ape.getChannelByName
 * @see Ape.getChannelByPubid
 * @see Ape.mkChan
 * @see Ape.channel
 */

/**
 * This event is fired when a channel is destroyed
 *
 * @name Ape.rmChan
 * @event
 * @public
 *
 * @param {channnel} channel Channel that is destroyed
 * @returns {void}
 *
 * @example
 * Ape.addEvent('rmChan', function(channel) {
 * 	if(channel.getProperty('name') == 'room_101'){
 * 		oceania.enemy= eastasia ;
 * 		eastasia.enemy = oceania;
 * 		eurasia.enemy = null;
 * 	}
 * });
 *
 * @see Ape.addEvent
 * @see Ape.mkChan
 */


APE_JS_NATIVE(ape_sm_rmchan)
//{
	JSString *chan_name;
	char *cchan_name;
	CHANNEL *chan;
	JSObject *chan_obj;

	if (JSVAL_IS_STRING(JS_ARGV(cx, vpn)[0])) {
		JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &chan_name);
		cchan_name = JS_EncodeString(cx, chan_name);
		
		if ((chan = getchan(cchan_name, g_ape)) == NULL) {
			JS_free(cx, cchan_name);
			return JS_TRUE;
		}
		JS_free(cx, cchan_name);
	} else if (JSVAL_IS_OBJECT(JS_ARGV(cx, vpn)[0])) {
		JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "o", &chan_obj);
		if (!JS_InstanceOf(cx, chan_obj, &channel_class, 0) || (chan = JS_GetPrivate(cx, chan_obj)) == NULL) {
			return JS_TRUE;
		}
	} else {
		return JS_TRUE;
	}
	
	rmchan(chan, g_ape);
	
	return JS_TRUE;
}

/**
 * Creates an user object.
 * <p>The user object is created by ape when an CONNECT command is successful, or via Ape.addUser.</p>
 *
 * @name Ape.addUser
 * @public
 * @constructs
 * @lends user.prototype
 *
 * param {user} user Userobject
 *
 * @see Ape.user.pipe
 * @see Ape.user.getProperty
 * @see Ape.user.delProperty
 * @see Ape.user.setProperty
 *
 * @fires connect
 * @fires adduser
 */

/**
 * Creates an user object.
 * <p>The user object is created by ape when an CONNECT command is successful, or via an Ape.addUser event</p>
 * <p>For a nuser object you can:</p>
 * <ul>
 * <li>Set/Get public or privates properties</li>
 * <li>Send a raw to the user's pipe</li>
 * <li>Add function to the prototype (Ape.user)</li>
 * </ul>
 *
 * @name Ape.user
 * @namespace
 *
 * @example
 * //You can set private properties on each user object,
 * // simply set a javascript property of the user.
 * Ape.addEvent('adduser', function(user) {
 * 	var res = {'foo': bar};
 * 	user.safe = res;							//private
 * 	for (var key in res) {
 * 		if (res.hasOwnproperty(key)) {
 * 			user.setProperty(key, res[key]);	//public
 * 		}
 * 	}
 * });
 * Ape.registerCmd('helloworld', function(params, info) {
 * 	Ape.log(info.user.safe.foo);
 * 	Ape.log(info.user.getProperty('foo');
 * });
 *
 * @see Ape.user.pipe
 * @see Ape.addUser
 * @see Ape.user.getProperty
 * @see Ape.user.delProperty
 * @see Ape.user.setProperty
 * @see Ape.onConnect
 */

/**
 * Each user has a pipe object where you can send raw's etc...
 *
 * @name Ape.user.pipe
 * @property {Ape.pipe} pipe Connected pipe for this user.
 * @field
 *
 * @example
 * user.pipe.sendRaw('RAW': {'foo': 'bar'});
 * @example
 * var pubid = user.pipe.getProperty('pubid');
 */


APE_JS_NATIVE(ape_sm_adduser)
//{
	JSObject *user;
	USERS *u;
	RAW *newraw;
	json_item *jstr = NULL;
		
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "o", &user)) {
		return JS_TRUE;
	}
	if (JS_InstanceOf(cx, user, &user_class, 0) == JS_FALSE) {
		return JS_TRUE;
	}
	
	if ((u = JS_GetPrivate(cx, user)) == NULL) {
		return JS_TRUE;
	}
	
	adduser(NULL, NULL, NULL, u, g_ape);
	
	subuser_restor(u->subuser, g_ape);
	
	jstr = json_new_object();
	json_set_property_strN(jstr, "sessid", 6, u->sessid, 32);
	
	newraw = forge_raw(RAW_LOGIN, jstr);
	newraw->priority = RAW_PRI_HI;
	
	post_raw(newraw, u, g_ape);
		
	if (u->cmdqueue != NULL) {
		unsigned int ret;
		json_item *queue;
		struct _cmd_process pc = {NULL, u, u->subuser, u->subuser->client, NULL, NULL, 0};
		
		for (queue = u->cmdqueue; queue != NULL; queue = queue->next) {
			if ((ret = process_cmd(queue, &pc, NULL, g_ape)) != -1) {
				if (ret == CONNECT_SHUTDOWN) {
					shutdown(u->subuser->client->fd, 2);
				}
				break;
			}
		}
		u->cmdqueue = NULL;
		free_json_item(u->cmdqueue);
	}
	
	return JS_TRUE;
	
}

/**
 * Listen to an ape event an start a callback function
 *
 * @name Ape.addEvent
 * @function
 * @public
 *
 * @param {string} name The event name to listen to
 * @param {function} fn This function will be called when the event is triggered
 * @returns {void}
 *
 * @example
 * Ape.addEvent('init', function(){
 * 	Ape.log('Ape is ready !');
 * });
 * @example
 * Ape.addEvent('adduser', function(user) {
 * 	Ape.log('New user :)');
 * });
 * Ape.addEvent('join', function(user, channel) {
 * 	Ape.log('New user has joined the channel (' + channel.getProperty('name') + ') :)');
 * });
 * @example
 * //Note that all objects passed to Events are persistent.
 * //This means that you can store private data inside user, channel, ...
 * Ape.addEvent('adduser', function(user) {
 * 	Ape.log('New user.');
 * 	user.foo = 'bar';
 * });
 * Ape.addEvent('join', function(user, channel) {
 * 	Ape.log(user.foo + ' joined the channel (' + channel.getProperty('name') + ').');
 * });
 *
 * @see Ape.init
 * @see Ape.addUser
 * @see Ape.delUser
 * @see Ape.beforeJoin
 * @see Ape.join
 * @see Ape.afterJoin
 * @see Ape.left
 * @see Ape.mkChan
 * @see Ape.rmChan
 * @see Ape.stop
 */
APE_JS_NATIVE(ape_sm_addEvent)
//{
	JSString *event;

	ape_sm_callback *ascb;

	JS_SET_RVAL(cx, vpn, JSVAL_NULL);
	
	if (argc != 2) {
		return JS_TRUE;
	}
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &event)) {
		return JS_TRUE;
	}

	ascb = JS_malloc(cx, sizeof(*ascb));

	if (!JS_ConvertValue(cx, JS_ARGV(cx, vpn)[1], JSTYPE_FUNCTION, &ascb->func)) {
		JS_free(cx, ascb);
		return JS_TRUE;
	}
	JS_AddValueRoot(cx, &ascb->func);

	ascb->next = NULL;
	ascb->type = APE_EVENT;
	ascb->cx = cx;
	ascb->callbackname = JS_EncodeString(cx, event);
	
	if (asc->callbacks.head == NULL) {
		asc->callbacks.head = ascb;
		asc->callbacks.foot = ascb;
	} else {
		asc->callbacks.foot->next = ascb;
		asc->callbacks.foot = ascb;
	}
	
	return JS_TRUE;
}

static JSObject *get_pipe_object(const char *pubid, transpipe *pipe, JSContext *cx, acetables *g_ape)
{
	JSObject *jspipe;
		
	if ((pipe != NULL) || ((pipe = get_pipe(pubid, g_ape)) != NULL)) {
		if ((jspipe = pipe->data) == NULL) {
			jspipe = JS_NewObject(cx, &pipe_class, get_property(g_ape->properties, "pipe_proto")->val, NULL);
			
			if (jspipe == NULL) {
				return NULL;
			}
			
			JS_AddObjectRoot(cx, &jspipe);
			//JS_DefineFunctions(cx, jspipe, apepipe_funcs);
			JS_SetPrivate(cx, jspipe, pipe);
			JS_RemoveObjectRoot(cx, &jspipe);
			
			pipe->data = jspipe;
		}
		
		return jspipe;
	}
	
	return NULL;
}

APE_JS_NATIVE(ape_sm_get_user_by_pubid)
//{
	JSString *pubid;
	char *cpubid;
	USERS *user;
	
	JS_SET_RVAL(cx, vpn, JSVAL_NULL);
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &pubid)) {
		return JS_TRUE;
	}
	
	cpubid = JS_EncodeString(cx, pubid);
	
	if ((user = seek_user_simple(cpubid, g_ape)) != NULL) {
		JS_SET_RVAL(cx, vpn, OBJECT_TO_JSVAL(APEUSER_TO_JSOBJ(user)));
	}
	
	JS_free(cx, cpubid);
	
	return JS_TRUE;
}

/**
 * Get the channel by its pubid.
 *
 * @name Ape.getChannelByPubid
 * @function
 * @public
 *
 * param {string} pubid The channel's pubid
 * returns {Ape.channel} The channel object if the channel is not present
 *
 * @example
 * var channel = Ape.getChannelByPubid(pubid);
 *
 * @see Ape.getChannelByName
 * @see Ape.mkChan
 * @see Ape.rmChan
 * @see Ape.channel
 */
APE_JS_NATIVE(ape_sm_get_channel_by_pubid)
//{
	JSString *pubid;
	char *cpubid;
	CHANNEL *chan;
	
	JS_SET_RVAL(cx, vpn, JSVAL_NULL);
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &pubid)) {
		return JS_TRUE;
	}
	
	cpubid = JS_EncodeString(cx, pubid);
	
	if ((chan = getchanbypubid(cpubid, g_ape)) != NULL) {
		JS_SET_RVAL(cx, vpn, OBJECT_TO_JSVAL(APECHAN_TO_JSOBJ(chan)));
	}
	
	JS_free(cx, cpubid);
	
	return JS_TRUE;
}

/**
 * Get a pipe object.
 *
 * @name Ape.getPipe
 * @function
 * @public
 *
 * @param {string} pubid The pubid of the pipe
 * @returns {Ape.pipe}
 *
 * @example
 * Ape.registerCmd('foocmd', true, function(params, info) {
 * 	Ape.log("The user ip : (' + info.ip + '), foo : " + params.foo);
 * 	Ape.getPipe(params.pubid).sendRaw('CUSTOM_RAW', {'foo': 'bar'});
 * });
 *
 * @see Ape.pipe
 * @see Ape.pipe.getParent
 * @see Ape.pipe
 * @see Ape.toObject
 */
APE_JS_NATIVE(ape_sm_get_pipe)
//{
	JSString *pubid;
	char *cpubid;
	JSObject *jspipe;
	
	JS_SET_RVAL(cx, vpn, JSVAL_NULL);
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &pubid)) {
		return JS_TRUE;
	}
	
	cpubid = JS_EncodeString(cx, pubid);
	
	if ((jspipe = get_pipe_object(cpubid, NULL, cx, g_ape)) != NULL) {
		JS_SET_RVAL(cx, vpn, OBJECT_TO_JSVAL(jspipe));
	}
	
	JS_free(cx, cpubid);
	
	return JS_TRUE;
}

/**
 * Get the channel by its name.
 *
 * @name Ape.getChannelByName
 * @function
 * @public
 *
 * param {string} channel The channel name
 * returns {Ape.channel} The channel object if the channel is not present
 *
 * @example
 * var channel = Ape.getChannelByName('foochannel');
 * channel.setProperty('foo', 'bar');
 * channel.myprivate = {'my':'private'}; // Can be a string or whatever you want
 * channel.pipe.sendRaw('FOORAW', {'John':'Doe'});
 * Ape.addEvent('beforeJoin', function(user, channel) {
 * 	Ape.log('My private : ' + channel.myprivate);
 * });
 *
 * @see Ape.getChannelByPubid
 * @see Ape.mkChan
 * @see Ape.rmChan
 * @see Ape.channel
 */
APE_JS_NATIVE(ape_sm_get_channel_by_name)
//{
	JSString *name;
	char *cname;
	CHANNEL *chan;
	
	JS_SET_RVAL(cx, vpn, JSVAL_NULL);
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &name)) {
		return JS_TRUE;
	}
	
	cname = JS_EncodeString(cx, name);
	
	if ((chan = getchan(cname, g_ape)) != NULL) {
		JS_SET_RVAL(cx, vpn, OBJECT_TO_JSVAL(APECHAN_TO_JSOBJ(chan)));
		
		JS_free(cx, cname);
		
		return JS_TRUE;
	}
	
	JS_free(cx, cname);
	
	return JS_TRUE;
}

/**
 * Get a configuration value.
 *
 * @name Ape.config
 * @function
 * @public
 * @static
 *
 * @param {string} configFilename Configuration filename
 * @param {string} key Configuration key
 * @returns {string} The string as defined in the configuration file for the configuration key, or an emtpty string.
 *
 * @example
 * var bar = Ape.config('settings.conf', 'foo');
 *
 * @see Ape.mainConfig
 */
APE_JS_NATIVE(ape_sm_config)
//{
	JSString *file, *key;
	char *value, *cfile, *ckey;
	JS_SET_RVAL(cx, vpn, JSVAL_NULL);
	plug_config *config;
	char conf_file[1024];
	
	if (!JS_ConvertArguments(cx, 2, JS_ARGV(cx, vpn), "SS", &file, &key)) {
		return JS_TRUE;
	}
	
	cfile = JS_EncodeString(cx, file);
	ckey = JS_EncodeString(cx, key);
	
	sprintf(conf_file, "%s%s", CONFIG_VAL(Config, modules_conf, g_ape->srv), cfile);
	
	config = plugin_parse_conf(conf_file);
	value = plugin_get_conf(config, ckey);
	
	JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyZ(cx, value)));
	
	JS_free(cx, cfile);
	JS_free(cx, ckey);

	return JS_TRUE;
}
/**
 * Get the ip address of a host asynchronously.
 *
 * @name Ape.os.resolveHostByName
 * @function
 * @public
 * @static
 *
 * @param {string} hostname The Hostname
 * @param {function} callback The function to be excecuted on success. The function argument is the  ip-address.
 *
 * @example
 * var sql;
 * os.getHostByName('www.ape-project', function(ip) {
 *	if (! ip) {
 *		Ape.log('Could not resolve host' );
 *	} else {
 *		Ape.log('Resolved: ' + ip);
 *		sql = new Ape.MySQL( ip + ':3306', 'user', 'password', 'database');
 *		sql.onConnect = function() {
 *			Ape.log('Connected to mysql server');
 *		}
 *	}
 * });
 */
static void ape_udns_resolve_cb(char *cip, void *data, acetables *g_ape)
{
	jsval rval;
	jsval params;
	JSString *ip;

	struct _ape_sm_udns *udns = data;
	ip = JS_NewStringCopyZ(udns->cx, cip);
	params = STRING_TO_JSVAL(ip);
	JS_CallFunctionValue(udns->cx, udns->global, udns->callback, 1, &params, &rval);

	free(udns);
}

APE_JS_NATIVE(ape_sm_resolvehostbyname)
//{
	JSString *name;
	char *cname;
	jsval callback_js;
	struct _ape_sm_udns *udns;

	JS_SET_RVAL(cx, vpn, JSVAL_NULL);
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &name)) {
		return JS_TRUE;
	}
	if (!JS_ConvertValue(cx, JS_ARGV(cx, vpn)[1], JSTYPE_FUNCTION, &callback_js)) {
		return JS_TRUE;
	}
	cname = JS_EncodeString(cx, name);
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	udns = JS_malloc(cx, sizeof(*udns));
	if (udns == NULL) {
		return JS_FALSE;
	}
	udns->cx = cx;
	udns->callback = callback_js;
	udns->global = obj;
	udns->cname = xstrdup(cname);
	ape_gethostbyname(cname, ape_udns_resolve_cb, udns, g_ape);

	JS_free(cx, cname);
	return JS_TRUE;
}


/**
 * Get the ip address of a host.
 * <p>Using getHostByName is very dangerous. The unix API gethostbyname is blocking. That is, if a resolution takes 5sec, the entire server is going to hang for 5 sec.</p>
 *
 * @name Ape.getHostByName
 * @function
 * @public
 * @static
 * @deprecated Pleas use Ape.os.resolveHostByName instead
 * @ignore
 *
 * @param {string} hostname The Hostname
 * @returns {string} ip		The ip address af the hostname or NULL
 *
 * @example
 * var content = Ape.getHostByName('www.ape-project');
 *
 * @see Ape.os.getHostByName
 * @see Ape.os.resolveHostByName
 */

/**
 * Get the ip address of a host.
 * <p>Using getHostByName is very dangerous. The unix API gethostbyname is blocking. That is, if a resolution takes 5sec, the entire server is going to hang for 5 sec.</p>
 *
 * @name Ape.os.getHostByName
 * @function
 * @public
 * @static
 * @deprecated Use Ape.os.resolveHostByName instead
 * @ignore
 *
 * @param {string} hostname The hostname
 * @returns {string} ip		The ip address of the hostname or NULL
 *
 * @example
 * var content = os.getHostByName('www.ape-project');
 *
 * @see Ape.os.resolveHostByName
 */
APE_JS_NATIVE(ape_sm_gethostbyname)
//{
	JSString *name;
	char *cname;
	JS_SET_RVAL(cx, vpn, JSVAL_NULL);
	//from beej getip.c; TODO: refractor; use ape_gethostbyname with a callback
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &name)) {
		return JS_TRUE;
	}
	cname = JS_EncodeString(cx, name);
	struct hostent *h;

	if ((h=gethostbyname(cname)) == NULL) {  // get the host info
		return JS_TRUE;
	}
	JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyZ(cx, inet_ntoa(*((struct in_addr *)h->h_addr)))));
	JS_free(cx, cname);
	return JS_TRUE;
}

/**
 * Get a configuration value from the main configfile (bin/ape.conf).
 *
 * @name Ape.mainConfig
 *
 * @function
 * @public
 * @static
 *
 * @param {string} section Configuration section
 * @param {string} key Configuration key
 * @returns {string} The string as defined in the configuration file for the configuration key, or an emtpty string.
 *
 * @example
 * var bar = Ape.mainConfig('Server', 'daemon');
 *
 * @see Ape.config
 */
APE_JS_NATIVE(ape_sm_mainconfig)
//{
	JSString *section, *key;
	char *ckey, *csection, *value;
	JS_SET_RVAL(cx, vpn, JSVAL_NULL);
	
	if (!JS_ConvertArguments(cx, 2, JS_ARGV(cx, vpn), "SS", &section, &key)) {
		return JS_TRUE;
	}
	
	csection = JS_EncodeString(cx, section);
	ckey = JS_EncodeString(cx, key);

	value = ape_config_get_key(ape_config_get_section(g_ape->srv, csection), ckey);

	JS_SET_RVAL(cx, vpn, (value != NULL ? STRING_TO_JSVAL(JS_NewStringCopyZ(cx, value)) : JSVAL_FALSE));

	JS_free(cx, csection);
	JS_free(cx, ckey);

	return JS_TRUE;
}

struct _ape_sm_timer
{
	JSContext *cx;
	JSObject *global;
	jsval func;
	
	uintN argc;
	jsval *argv;
	
	int cleared;
	struct _ticks_callback *timer;
};

static void ape_sm_timer_wrapper(struct _ape_sm_timer *params, int *last)
{
	jsval rval;
	
	//JS_SetContextThread(params->cx);
	//JS_BeginRequest(params->cx);
		if (!params->cleared) {
			JS_CallFunctionValue(params->cx, params->global, params->func, params->argc, params->argv, &rval);
		}
		if (params->cleared) { /* JS_CallFunctionValue can set params->Cleared to true */
			ape_sm_compiled *asc;
			asc = JS_GetContextPrivate(params->cx);

			if (!*last) {
				*last = 1;
			}
		}
		if (*last) {
			JS_RemoveValueRoot(params->cx, &params->func);
			
			if (params->argv != NULL) {
				free(params->argv);
			}
			free(params);
		}
	//JS_EndRequest(params->cx);
	//JS_ClearContextThread(params->cx);
}

/**
 * Execute a function after specified delay.
 *
 * @name Ape.setTimeout
 * @function
 * @public
 * @static
 *
 * @param {function} fn
 * @param {integer} delay
 * @param {mixed} [...] param1, param2, ..
 * @returns {integer} The interval id that can be used in clearTimeout
 *
 * @example
 * var timeoutID = Ape.setTimeout(function(a, b) {
 * 	Ape.log('Foo : ' + a + ' Bar : ' + b);
 * }, 3000, 'foo', 'bar');
 * Ape.clearInterval(timeoutID);
 *
 * @see Ape.setInterval
 * @see Ape.clearInterval
 * @see Ape.clearTimeout
 */
APE_JS_NATIVE(ape_sm_set_timeout)
//{
	struct _ape_sm_timer *params;
	struct _ticks_callback *timer;
	int ms, i;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	
	params = JS_malloc(cx, sizeof(*params));
	
	if (params == NULL) {
		return JS_FALSE;
	}
	
	params->cx = cx;
	params->global = obj;
	params->argc = argc-2;
	params->cleared = 0;
	params->timer = NULL;
	
	params->argv = (argc-2 ? JS_malloc(cx, sizeof(*params->argv) * argc-2) : NULL);
	
	if (!JS_ConvertValue(cx, JS_ARGV(cx, vpn)[0], JSTYPE_FUNCTION, &params->func)) {
		return JS_TRUE;
	}

	if (!JS_ConvertArguments(cx, 1, &JS_ARGV(cx, vpn)[1], "i", &ms)) {
		return JS_TRUE;
	}
	
	JS_AddValueRoot(cx, &params->func);
	
	for (i = 0; i < argc-2; i++) {
		params->argv[i] = JS_ARGV(cx, vpn)[i+2];
	}
	
	timer = add_timeout(ms, ape_sm_timer_wrapper, params, g_ape);
	timer->protect = 0;
	params->timer = timer;
	
	JS_SET_RVAL(cx, vpn, INT_TO_JSVAL(timer->identifier));
	
	return JS_TRUE;
	
}

/**
 * Calls a function repeatedly, with a fixed time delay between each call to that function.
 *
 * @name Ape.setInterval
 * @function
 * @public
 * @static
 *
 * @param {function} fn
 * @param {integer} delay
 * @param {mixed} [...] param1, param2, ..
 * @returns {integer} The interval id that can be used in clearInterval
 *
 * @example
 * var timeoutID = Ape.setInterval(function(a, b) {
 * 	Ape.log('Foo : ' + a + ' Bar : ' + b);
 * }, 3000, 'foo', 'bar');
 * Ape.clearInterval(timeoutID);
 *
 * @see Ape.setTimeout
 * @see Ape.clearInterval
 * @see Ape.clearTimeout
 */
APE_JS_NATIVE(ape_sm_set_interval)
//{
	struct _ape_sm_timer *params;
	struct _ticks_callback *timer;
	int ms, i;
	
	params = JS_malloc(cx, sizeof(*params));
	
	if (params == NULL) {
		return JS_FALSE;
	}
	
	params->cx = cx;
	params->global = asc->global;
	params->argc = argc-2;
	params->cleared = 0;
	params->timer = NULL;
	
	params->argv = (argc-2 ? JS_malloc(cx, sizeof(*params->argv) * argc-2) : NULL);
	
	if (!JS_ConvertValue(cx, JS_ARGV(cx, vpn)[0], JSTYPE_FUNCTION, &params->func)) {
		return JS_TRUE;
	}

	if (!JS_ConvertArguments(cx, 1, &JS_ARGV(cx, vpn)[1], "i", &ms)) {
		return JS_TRUE;
	}
	
	JS_AddValueRoot(cx, &params->func);
	
	for (i = 0; i < argc-2; i++) {
		params->argv[i] = JS_ARGV(cx, vpn)[i+2];
	}
	
	timer = add_periodical(ms, 0, ape_sm_timer_wrapper, params, g_ape);
	timer->protect = 0;
	params->timer = timer;
	
	JS_SET_RVAL(cx, vpn, INT_TO_JSVAL(timer->identifier));
	
	return JS_TRUE;
}

/**
 * Cancel a timeout created by setTimeout.
 *
 * @name Ape.clearTimeout
 * @function
 * @public
 * @static
 *
 * @param {integer} timeoutId
 * @returns {void}
 *
 * @example
 * var timeoutID = Ape.setTimeout(function(a, b) {
 * 	Ape.log('Foo : ' + a + ' Bar : ' + b);
 * }, 3000, 'foo', 'bar');
 * Ape.clearTimeout(timeoutID);
 *
 * @see Ape.setTimeout
 * @see Ape.setInterval
 * @see Ape.clearInterval
 */

/**
 * Cancel a timeout created by setInterval.
 *
 * @name Ape.clearInterval
 * @function
 * @public
 * @static
 *
 * @param {integer} timeoutId
 * @returns {void}
 *
 * @example
 * var timeoutID = Ape.setInterval(function(a, b) {
 * 	Ape.log('Foo : ' + a + ' Bar : ' + b);
 * }, 3000, 'foo', 'bar');
 * Ape.clearInterval(timeoutID);
 *
 * @see Ape.setTimeout
 * @see Ape.setInterval
 * @see Ape.clearTimeout
 */
APE_JS_NATIVE(ape_sm_clear_timeout)
//{
	unsigned int identifier;
	struct _ape_sm_timer *params;
	struct _ticks_callback *timer;
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "i", &identifier)) {
		return JS_TRUE;
	}
	
	if ((timer = get_timer_identifier(identifier, g_ape)) != NULL && !timer->protect) {
		params = timer->params;
		params->cleared = 1;
	}

	return JS_TRUE;
}

/**
 * Log a message in the console (daemon = false).
 *
 * @name Ape.log
 * @public
 * @function
 * @static
 *
 * @param {string} message The message that should be logged
 * @returns {void}
 *
 * @example
 * Ape.log('foo');// foo
 * @example
 * Ape.log(pipe); // [object Object]
 * @example
 * Ape.log(JSON.stringify(var)); // '{}';
 */
APE_JS_NATIVE(ape_sm_echo)
//{
	char *cstring;
	JSString *string;
	JS_SET_RVAL(cx, vpn, JSVAL_NULL);

	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &string)) {
		return JS_TRUE;
	}

	cstring = JS_EncodeString(cx, string);
	
	if (!g_ape->is_daemon) {
		fwrite(cstring, sizeof(char), JS_GetStringEncodingLength(cx, string), stdout);
		fwrite("\n", sizeof(char), 1, stdout);
	}
	ape_log(APE_INFO, __FILE__, __LINE__, g_ape, "[%s] : %s", MODULE_NAME, cstring);
	
	JS_free(cx, cstring);

	return JS_TRUE;
}

/**
 * @name: 	Ape.status
 * @function
 * @public
 * @static
 * @ignore
 *
 * @returns {object} status Object with status information,
 * @returns {integer} status.connected nr of connected users
 * @returns {boolean} status.daemon Running in deamon mode
 *
 * @example:var status = Ape.status();
 * 			Ape.log(JSON.stringify(status));
 */
APE_JS_NATIVE(ape_sm_status)
//{
	JSObject *elem = JS_NewObject(cx, NULL, NULL, NULL);
	JS_AddObjectRoot(cx, &elem);
	jsval connected = INT_TO_JSVAL(g_ape->nConnected);
	jsval isDaemon = g_ape->is_daemon == 0 ? JSVAL_TRUE : JSVAL_FALSE;
	JS_SetProperty(cx, elem, "connected", &connected);
	JS_SetProperty(cx, elem, "isDaemon", &isDaemon);
	jsval currentval = OBJECT_TO_JSVAL(elem);
	JS_SET_RVAL(cx, vpn, currentval);
	return JS_TRUE;
}

/**
 * @name: 	Ape.eval
 * @function
 * @public
 * @static
 *
 * @param {string} scriptstring The javascript code that should be executed in the Ape context
 * @returns {undefined|integer} if the scriptstring was empty or the could not compiled
 * 			else the return value of the scriptstring
 *
 * @example:var r = Ape.eval('var sum = function(a, b){return a + b;}; return sum(4,4);');
 * Ape.log('returned: ' + r);
 */

APE_JS_NATIVE(ape_sm_eval)
//{
	char *cscript;
	JSString *script;
	jsval ret;

	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "S", &script)) {
		return JS_TRUE;
	}
	cscript = JS_EncodeString(cx, script);

	ret = JSVAL_VOID;
	if ((JS_EvaluateScript(cx, obj, cscript, strlen (cscript), "eval()", 0, &ret)) != JS_FALSE) {
		JS_SET_RVAL(cx, vpn, ret);
	}
	JS_SET_RVAL(cx, vpn, ret);
	JS_free(cx, cscript);
	return JS_TRUE;
}

/**
 * @name: 	Ape.os.system
 * @function
 * @static
 * @public
 *
 * @param {string} exec The full path to the executable. This must exist and executable
 * @param {string} paramstring Parameters
 * @returns {null|undefined|integer} null: if the execution did not take place.
 *          undefined: if the execute could not start (-1)
 *          or the return code of the command
 *
 * @example:var r = os.system('/usr/bin/wget', 'http://www.verpeteren.nl -o /tmp/www.verpeteren.nl.html');
 * Ape.log('returned: ' + r);
 */

APE_JS_NATIVE(ape_sm_system)
//{
	char *cparams;
	char *cexec;
	JSString *params;
	JSString *exec;
	jsval ret;
	if (!JS_ConvertArguments(cx, 2, JS_ARGV(cx, vpn), "SS", &exec, &params)) {
		return JS_TRUE;
	}
	cexec = JS_EncodeString(cx, exec);
	cparams = JS_EncodeString(cx, params);
	ret = JSVAL_NULL;
	if (	(g_ape->is_daemon && getuid() !=0 )     //Should have change user to non root in ape.conf
		||
			( ! g_ape->is_daemon && getuid() != 0 )  //Do not do this as root
		) {
		char *cmd = NULL;
		struct stat sb;
		int execl = strlen (cexec);
		int paramsl = strlen(cparams);
		int len = 3 + execl + paramsl;
		if (stat(cexec, &sb) >= 0 && (sb.st_mode & S_IXUSR)) {
			cmd = xmalloc( sizeof(char) * (len) );
			memset(cmd, '\0', len);
			if (cmd) {
				strcpy (cmd, cexec);
		 		cmd[execl] = ' ';
				strcat(cmd, cparams);
				if (!g_ape->is_daemon) {
					printf("[%s] : executing '%s'\n", MODULE_NAME, cmd);
				}
				ape_log(APE_INFO, __FILE__, __LINE__, g_ape, "[%s] : executing '%s'", MODULE_NAME, cmd);
				int r = system(cmd);
				ret = (r == -1)? JSVAL_VOID : INT_TO_JSVAL(r);
			}
			free(cmd);
		}
	}
	JS_SET_RVAL(cx, vpn, ret);
	JS_free(cx, cparams);
	JS_free(cx, cexec);
	return JS_TRUE;
}

#if 0
/**
 * Ape.raw is a class constructor.
 *
 * @name Ape.raw
 * @class Internal object (Under development)
 * @constructor
 * @public
 * @ignore
 *
 * @param {string} raw Raw name
 * @returns {void}
 *
 * @see Ape.user.pipe.sendRaw
 */
APE_JS_NATIVE(ape_sm_raw_constructor)
//{
	char *rawname;
	JSObject *obj = JS_THIS_OBJECT(cx, vpn);
	
	if (!JS_ConvertArguments(cx, 1, JS_ARGV(cx, vpn), "s", &rawname)) {
		return JS_TRUE;
	}
	
	JS_SetPrivate(cx, obj, rawname);
	
	return JS_TRUE;
}
#endif

/**
 * Ape.sockClient is a class constructor.
 * <p>SockClient is used to connect to a socket server.</p>
 *
 * @name Ape.sockClient
 * @class SocketClient object (TCP client)
 * @augments Ape.apesocket
 * @constructor
 * @public
 *
 * @param {integer|string} port
 * @param {string} host hostname or ip-address
 * @param {object} [options]
 * @param {boolean} [options.flushlf=false]  If true onRead is called only when a "\n" is received (data is split around it)
 * @returns {Ape.sockClient}
 *
 * @example
 * //Instantiating a socket client is simple
 * var socket = new Ape.sockClient('21', 'example.com', {flushlf: true} );
 * @example
 * var socket = new Ape.sockClient('21', 'example.com', {flushlf: true});
 * socket.onConnect = function() {
 * 	Ape.log('Connected to example.com');
 * 	this.write('Hello\n');
 * }
 * //'\n' are removed.
 * socket.onRead = function(data) {
 * 	Ape.log('Data : ' + data);
 * }
 * socket.onDisconnect = function() {
 * 	Ape.log('Gone !');
 * }
 *
 * @see Ape.sockServer
 * @see Ape.sockClient
 * @see Ape.sockServer
 */
APE_JS_NATIVE(ape_sm_sockclient_constructor)
//{
	int port;
	char *cip;
	JSString *ip;
	JSObject *options = NULL;
	JSObject *obj = JS_NewObjectForConstructor(cx, vpn);
	ape_socket *pattern;
	jsval vp;
	struct _ape_sock_callbacks *cbcopy;
	struct _ape_sock_js_obj *sock_obj;

	JS_SET_RVAL(cx, vpn, OBJECT_TO_JSVAL(obj));

	if (!JS_ConvertArguments(cx, argc, JS_ARGV(cx, vpn), "iS/o", &port, &ip, &options)) {
		return JS_TRUE;
	}
	
	cip = JS_EncodeString(cx, ip);
	
	sock_obj = xmalloc(sizeof(*sock_obj));
	sock_obj->client_obj = NULL;
	sock_obj->client = NULL;
	
	cbcopy = xmalloc(sizeof(struct _ape_sock_callbacks));
	
	cbcopy->private = sock_obj;
	cbcopy->asc = asc;
	cbcopy->server_obj = obj;
	cbcopy->state = 1;
	
	JS_AddObjectRoot(cx, &cbcopy->server_obj);
	
	pattern = xmalloc(sizeof(*pattern));
	pattern->callbacks.on_connect = sm_sock_onconnect;
	pattern->callbacks.on_disconnect = sm_sock_ondisconnect;
	pattern->callbacks.on_data_completly_sent = NULL;

	if (options != NULL && JS_GetProperty(cx, options, "flushlf", &vp) && JSVAL_IS_BOOLEAN(vp) && JSVAL_TO_BOOLEAN(vp)) {
		pattern->callbacks.on_read_lf = sm_sock_onread_lf;
		pattern->callbacks.on_read = NULL;
	} else {
		/* use the classic read callback */
		pattern->callbacks.on_read = sm_sock_onread;
		pattern->callbacks.on_read_lf = NULL;
	}
	pattern->attach = cbcopy;
	JS_SetPrivate(cx, obj, cbcopy);
	
	ape_connect_name(cip, port, pattern, g_ape);
	
	JS_free(cx, cip);
	
	return JS_TRUE;
}

/**
 * Pipe constructor.
 * <p>A pipe is an object that is kind of a connector through wich RAWs are sent.</p>
 * <p>Each user an channel has got an associated pipe.
 * <p>Pipes created manually (custom pipes) are a little different from user and channel pipes. A manually created pipe is just usefull to receive "SEND" commands from clients (Proxy pipes are working this way). See example for more details.</p>
 *
 * @name Ape.pipe
 * @class pipe object (Communication line)
 * @constructor
 * @public
 *
 * @returns {Ape.pipe}
 *
 * @example
 * var mypipe = new Ape.pipe();
 * @example
 * //Custom pipe
 * //We create a new custom pipe
 * var pipe = new Ape.pipe();
 * //Custom pipe is created with an unique pubid
 * Ape.log('Custom pipe pubid: ' + pipe.getProperty('pubid'));
 * //We listen "SEND" commands received on this pipe
 * pipe.onSend = function(user, params) {
 * Ape.log('Received data from pipe: ' + params.msg);
 * 	if(params.destroy) {
 * 		pipe.destroy();
 * 	}
 * }
 *
 * @see Ape.pipe
 * @see Ape.getPipe
 * @see Ape.pipe.destroy
 * @see Ape.pipe.toObject
 * @see Ape.pipe.getParent
 * @see Ape.pipe.onSend
 * @see Ape.pipe.sendRaw
 */

/**
 * The channel object is created when an user connects to an nonexistent channel or if it is created on the server with Ape.mkChan.
 *
 * @name Ape.channel
 * @class Channel object (For many-to-many communcation)
 *
 * @example
 * var channel = Ape.mkChan('my_channel');
 * @example
 * Ape.addEvent('join', function(user, channel) {
 * 	Ape.log('New user has joined the channel (' + channel.getProperty('name') + ') :)');
 * });
 *
 * @see Ape.mkChan
 * @see Ape.rmChan
 * @see Ape.getChannelByName
 * @see Ape.getChannelByPubid
 */

/**
 * <p>
 * <p>This variable is mootools Hash that contain all the user on the channel.</p>
 * <p>This is provided by the script framework/userslist.js</p>
 *
 * @name Ape.channel.userslist
 * @property {Hash} userslist Hash with a list of all the users per channel
 * @requires framework/userslist.js
 *
 * @example
 * var chan = Ape.getChannelByName('test');
 * chan.userslist.each(function(user) {
 * 	Ape.log(user.getProperty('pubid'));
 * });
 * @example
 * //a little bit off-topic but nevertheless very interesting
 * function sendToMany(users, cmd, data) {
 * 	var chan = Ape.mkChan('*' + new Date());
 * 		for (var i = 0; i < users.length; i++) {
 * 			users[i].join(chan);
 * 		}
 * 		chan.pipe.sendRaw(cmd, data);
 * 		Ape.delChan(chan);
 * }
 */
APE_JS_NATIVE(ape_sm_pipe_constructor)
//{
	JSObject *obj = JS_NewObjectForConstructor(cx, vpn);
	transpipe *pipe;
	//JSObject *link;

	JS_SET_RVAL(cx, vpn, OBJECT_TO_JSVAL(obj));

	/* Add link to a Root ? */
	pipe = init_pipe(NULL, CUSTOM_PIPE, g_ape);
	pipe->on_send = ape_sm_pipe_on_send_wrapper;
	pipe->data = obj;
	
	add_property(&pipe->properties, "cx", cx, EXTEND_POINTER, EXTEND_ISPRIVATE);
	
	JS_SetPrivate(cx, obj, pipe);
	
	/* TODO : This private data must be removed is the pipe is destroyed */

	
	return JS_TRUE;
}

#ifdef _USE_MYSQL
static void ape_mysql_handle_io(struct _ape_mysql_data *myhandle, acetables *g_ape)
{
	int ret;
	
	if (myhandle->my->call_it == NULL) {
		return;
	}
	ret = mysac_io(myhandle->my);

	switch(ret) {
		case MYERR_WANT_WRITE:
		case MYERR_WANT_READ:
			break;
		default:
			myhandle->my->call_it = NULL; /* prevent any extra IO call */
			
			if (myhandle->on_success != NULL) {
				myhandle->on_success(myhandle, ret);
			}
			
			break;
	}
}

static void ape_mysql_io_read(ape_socket *client, ape_buffer *buf, size_t offset, acetables *g_ape)
{
	struct _ape_mysql_data *myhandle = client->data;

	ape_mysql_handle_io(myhandle, g_ape);
}

static void ape_mysql_io_write(ape_socket *client, acetables *g_ape)
{
	struct _ape_mysql_data *myhandle = client->data;
	
	ape_mysql_handle_io(myhandle, g_ape);
}

static void mysac_setdb_success(struct _ape_mysql_data *myhandle, int code)
{
	jsval rval;
	if (myhandle->jsmysql == NULL) {
		return;
	}
	
	if (!code) {
		myhandle->state = SQL_READY_FOR_QUERY;
		apemysql_shift_queue(myhandle);
		
		JS_CallFunctionName(myhandle->cx, myhandle->jsmysql, "onConnect", 0, NULL, &rval);
	} else {
		jsval params[1];
		params[0] = INT_TO_JSVAL(code);
		
		JS_CallFunctionName(myhandle->cx, myhandle->jsmysql, "onError", 1, params, &rval);
		
		myhandle->jsmysql = NULL;
		myhandle->on_success = NULL;
		
		/* TODO : Supress queue */
		
		free(myhandle->db);
	}
}

static void mysac_connect_success(struct _ape_mysql_data *myhandle, int code)
{
	jsval rval;
	if (myhandle->jsmysql == NULL) {
		return;
	}

	if (!code) {
		myhandle->on_success = mysac_setdb_success;
		
		mysac_set_database(myhandle->my, myhandle->db);
		mysac_send_database(myhandle->my);
		
	} else {
		jsval params[1];
		params[0] = INT_TO_JSVAL(code);
		
		JS_CallFunctionName(myhandle->cx, myhandle->jsmysql, "onError", 1, params, &rval);
		
		/* TODO : Supress queue */
		
		myhandle->jsmysql = NULL;
		myhandle->on_success = NULL;
		
		free(myhandle->db);
	}
}

static void mysac_query_success(struct _ape_mysql_data *myhandle, int code)
{
	struct _ape_mysql_queue *queue = myhandle->data;
	jsval params[2], rval;
	myhandle->state = SQL_READY_FOR_QUERY;
	myhandle->on_success = NULL;
	
	if (!code) {
		MYSAC_ROW *row;
		MYSAC_RES *myres = queue->res;
		
		unsigned int nfield = mysac_field_count(myres), nrow = mysac_num_rows(myres), pos = 0;
		JSObject *res = JS_NewArrayObject(myhandle->cx, nrow, NULL); /* First param [{},{},{},] */
		
		JS_AddObjectRoot(myhandle->cx, &res);
		
		while (nrow && (row = mysac_fetch_row(myres)) != NULL) {
			unsigned int i;
			jsval currentval;
			JSObject *elem = JS_NewObject(myhandle->cx, NULL, NULL, NULL);
			JS_AddObjectRoot(myhandle->cx, &elem);
			currentval = OBJECT_TO_JSVAL(elem);
			JS_SetElement(myhandle->cx, res, pos, &currentval);
			JS_RemoveObjectRoot(myhandle->cx, &elem);
			for (i = 0; i < nfield; i++) {
				int fieldlen, valuelen;
				char *field, *val;
				jsval jval;
				
				valuelen = ((MYSAC_RES *)myres)->cr->lengths[i];
				fieldlen = ((MYSAC_RES *)myres)->cols[i].name_length;
				
				field = ((MYSAC_RES *)myres)->cols[i].name;
				val = row[i].blob;
				
				jval = (val == NULL ? JSVAL_NULL : STRING_TO_JSVAL(JS_NewStringCopyN(myhandle->cx, val, valuelen)));
				
				JS_SetProperty(myhandle->cx, elem, field, &jval);
			}
			pos++;
		}
		params[0] = OBJECT_TO_JSVAL(res);
		params[1] = JSVAL_FALSE;

		JS_RemoveObjectRoot(myhandle->cx, &res);
		JS_CallFunctionValue(myhandle->cx, myhandle->jsmysql, queue->callback, 2, params, &rval);
	} else {
		params[0] = JSVAL_FALSE;
		params[1] = INT_TO_JSVAL(code);

		JS_CallFunctionValue(myhandle->cx, myhandle->jsmysql, queue->callback, 2, params, &rval);
	}
	JS_RemoveValueRoot(myhandle->cx, &queue->callback);
	
	JS_free(myhandle->cx, queue->query);
	free(queue->res);
	free(queue);
		
	apemysql_shift_queue(myhandle);
}

/**
 * Close my mysql connection
 *
 * @name Ape.MySQL.finalize
 * @function
 * @private
 * @static
 *
 * @returns {void}
 *
 * @example
 * sql.finalize()
 */
static void apemysql_finalize(JSContext *cx, JSObject *jsmysql)
{
	struct _ape_mysql_data *myhandle;

	if ((myhandle = JS_GetPrivate(cx, jsmysql)) != NULL) {
		myhandle->jsmysql = NULL;
		/* Todo: shutdown mysql */
	}
}

static void apemysql_shift_queue(struct _ape_mysql_data *myhandle)
{
	struct _ape_mysql_queue *queue;
	int ret;
	int basemem = (1024*1024);
	MYSAC_RES *res;
	char *res_buf;
	

	if (myhandle->queue.head == NULL || myhandle->state != SQL_READY_FOR_QUERY) {
		return;
	}
	res_buf = xmalloc(sizeof(char) * basemem);
	res = mysac_init_res(res_buf, basemem);

	queue = myhandle->queue.head;
	queue->res = res;
	
	myhandle->state = SQL_NEED_QUEUE;
	myhandle->data = queue;
	
	if ((myhandle->queue.head = queue->next) == NULL) {
		myhandle->queue.foot = NULL;
	}
	mysac_b_set_query(myhandle->my, queue->res, queue->query, queue->query_len);
	
	switch((ret = mysac_send_query(myhandle->my))) {
		case MYERR_WANT_WRITE:
		case MYERR_WANT_READ:
			break;
		default:
			myhandle->on_success = NULL;
			myhandle->my->call_it = NULL;
			mysac_query_success(myhandle, ret);
			return;
	}
	
	myhandle->on_success = mysac_query_success;
	
}

static struct _ape_mysql_queue *apemysql_push_queue(struct _ape_mysql_data *myhandle, char *query, unsigned int query_len, jsval callback)
{
	struct _ape_mysql_queue *nqueue;

	nqueue = xmalloc(sizeof(*nqueue));
	
	nqueue->next = NULL;
	nqueue->query = query;
	nqueue->query_len = query_len;
	nqueue->callback = callback;
	nqueue->res = NULL;
	
	if (myhandle->queue.foot == NULL) {
		myhandle->queue.head = nqueue;
	} else {
		myhandle->queue.foot->next = nqueue;
	}
	myhandle->queue.foot = nqueue;
	
	JS_AddValueRoot(myhandle->cx, &nqueue->callback);

	if (myhandle->queue.head->next == NULL && myhandle->state == SQL_READY_FOR_QUERY) {
		
		apemysql_shift_queue(myhandle);
		return NULL;
	}
	return nqueue;
}

/**
 * Ape.MySQL is a class constructor. You can connect and use a MySQL database.
 *
 * @name Ape.MySQL
 * @class MySql Object (Database connection)
 * @constructor
 * @public
 * @author Special thanks to  Louis Charette for repairing this!
 *
 * @param {string} host IP-address:port or unix socket
 * @param {string} username MySql username
 * @param {string} password MySql username
 * @param {string} database MySql database
 * @returns {Ape.MySQL}
 *
 * @example
 * //Database connection
 * var sql = new Ape.MySQL('10.0.0.240:3306', 'user', 'password', 'database');
 * 	sql.onConnect = function() {
 * 	Ape.log('Connected to mysql server');
 * }
 * @example
 * // Select
 * sql.query('SELECT * FROM table', function(res, errorNo) {
 * 	if (errorNo) Ape.log('Request error : ' + errorNo + ' : ' + this.errorString());
 * 	else {
 * 	Ape.log('Fetching ' + res.length);
 * 		res.each(function(data) {
 * 			Ape.log(data.content);//data.<column name>
 * 		});
 * 	}
 * });
 * @example
 * //Insert
 * sql.query('INSERT INTO table VALUES("a", "b", "c")', function(res, errorNo) {
 * 	if (errorNo) Ape.log('Request error : ' + errorNo + ' : ' + this.errorString());
 * 	else Ape.log('Inserted: ' + MySQL.getInsertId ());
 * });
 *
 * @fires Ape.MySQL.onError
 * @fires Ape.MySQL.onConnect
 */
APE_JS_NATIVE(ape_sm_mysql_constructor)
//{
	JSString *host, *login, *pass, *db;
	char *chost, *clogin, *cpass, *cdb;
	JSObject *obj = JS_NewObjectForConstructor(cx, vpn);

	JS_SET_RVAL(cx, vpn, OBJECT_TO_JSVAL(obj));

	MYSAC *my;
	int fd;
	struct _ape_mysql_data *myhandle;
	
	if (!JS_ConvertArguments(cx, argc, JS_ARGV(cx, vpn), "SSSS", &host, &login, &pass, &db)) {
		return JS_TRUE;
	}
	
	myhandle = xmalloc(sizeof(*myhandle));
	
	chost = JS_EncodeString(cx, host);
	clogin = JS_EncodeString(cx, login);
	cpass = JS_EncodeString(cx, pass);
	cdb = JS_EncodeString(cx, db);

	my = mysac_new(1024*1024);
	mysac_setup(my, xstrdup(chost), xstrdup(clogin), xstrdup(cpass), xstrdup(cdb), 0);
	mysac_connect(my);

	myhandle->my = my;
	myhandle->jsmysql = obj;
	myhandle->cx = cx;
	myhandle->db = xstrdup(cdb);
	myhandle->data = NULL;
	myhandle->callback = JSVAL_NULL;
	myhandle->state = SQL_NEED_QUEUE;
	myhandle->queue.head = NULL;
	myhandle->queue.foot = NULL;
	
	JS_SetPrivate(cx, obj, myhandle);
	
	fd = mysac_get_fd(my);

	prepare_ape_socket (fd, g_ape);

	g_ape->co[fd]->fd = fd;
	g_ape->co[fd]->stream_type = STREAM_DELEGATE;

	g_ape->co[fd]->callbacks.on_read = ape_mysql_io_read;
	g_ape->co[fd]->callbacks.on_write = ape_mysql_io_write;
	g_ape->co[fd]->data = myhandle;

	events_add(g_ape->events, fd, EVENT_READ|EVENT_WRITE);
	
	//myhandle->to_call = mysac_connect;
	myhandle->on_success = mysac_connect_success;
	
	JS_free(cx, chost);
	JS_free(cx, clogin);
	JS_free(cx, cpass);
	JS_free(cx, cdb);

	return JS_TRUE;
}
#endif

/**
 * Ape.sockServer is a class constructor.
 * <p>SockServer is used to create a socket server that can accept client sockets,</p>
 *
 * @name Ape.sockServer
 * @class sockServer Object (TCP connection server)
 * @augments Ape.apesocket
 * @constructor
 * @public
 *
 * @param {integer|string} port
 * @param {string} ip The ip address to bind the server
 * @param {object} [options]
 * @param {boolean} [options.flushlf=false]  If true onRead is called only when a "\n" is received (data is split around it)
 * @returns {Ape.sockServer}
 *
 * @example
 * //Instantiating a socket server that listens to port 80
 * var socket = new Ape.sockServer('80', '0.0.0.0', {flushlf: true} );
 * @example
 * var socket = new Ape.sockServer('80', '0.0.0.0', {flushlf: true});
 * socket.onAccept = function(client) {
 * 	Ape.log('New client !');
 * 	client.write('Hello world\n');
 * }
 * socket.onRead = function(client, data) {
 * 	Ape.log('Received data:' + data);
 * 	if(data == 'bye') {
 * 		client.close();
 * 	}
 * }
 *
 * @see Ape.sockClient
 * @see Ape.sockServer
 * @see Ape.sockClient
 */
APE_JS_NATIVE(ape_sm_sockserver_constructor)
//{
	int port;
	char *cip;
	JSString *ip;
	JSObject *options = NULL;
	ape_socket *server;
	jsval vp;
	JSObject *obj = JS_NewObjectForConstructor(cx, vpn);

	JS_SET_RVAL(cx, vpn, OBJECT_TO_JSVAL(obj));

	if (!JS_ConvertArguments(cx, argc, JS_ARGV(cx, vpn), "iS/o", &port, &ip, &options)) {
		return JS_TRUE;
	}
	
	cip = JS_EncodeString(cx, ip);
	
	server = ape_listen(port, cip, g_ape);

	if (server == NULL) {
		JS_SET_RVAL(cx, vpn, JSVAL_FALSE);
		JS_free(cx, cip);
		return JS_TRUE;
	}
	
	server->attach = xmalloc(sizeof(struct _ape_sock_callbacks));

	((struct _ape_sock_callbacks *)server->attach)->asc 			= asc;
	((struct _ape_sock_callbacks *)server->attach)->private 		= NULL;
	((struct _ape_sock_callbacks *)server->attach)->server_obj 		= obj;
	((struct _ape_sock_callbacks *)server->attach)->state 			= 1;
	
	JS_AddObjectRoot(cx, &((struct _ape_sock_callbacks *)server->attach)->server_obj);

	/* check if flushlf is set to true in the optional object */
	if (options != NULL && JS_GetProperty(cx, options, "flushlf", &vp) && JSVAL_IS_BOOLEAN(vp) && JSVAL_TO_BOOLEAN(vp)) {
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
	
	JS_free(cx, cip);
		
	return JS_TRUE;
}

/**
 * Apply a 'XOR' between two string (or binary)
 *
 * @name Ape.xorize
 * @function
 * @public
 * @static
 *
 * @param {string} string1
 * @param {string} string2
 * @returns {string} The xor-ed string
 *
 * @example
 * var result = Ape.xorize('key1', 'key2');
 * @example
 * for (i = 0; i < key1_len; i++) {
 * 	returned[i] = key1[i] ^ key2[i];
 * }
 */
APE_JS_NATIVE(ape_sm_xorize)
//{
	JSString *s1, *s2;
	char *ps1, *ps2, *final;
	int i, len;
	
	if (!JS_ConvertArguments(cx, 2, JS_ARGV(cx, vpn), "SS", &s1, &s2)) {
		return JS_TRUE;
	}
	
	ps1 = JS_EncodeString(cx, s1);
	ps2 = JS_EncodeString(cx, s2);
	len = JS_GetStringEncodingLength(cx, s1);
	
	if (JS_GetStringLength(s2) < len) {
		return JS_TRUE;
	}
	
	final = xmalloc(sizeof(char) * len);
	
	for (i = 0; i < len; i++) {
		final[i] = ps1[i] ^ ps2[i];
	}
	
	JS_SET_RVAL(cx, vpn, STRING_TO_JSVAL(JS_NewStringCopyN(cx, final, len)));
	
	free(final);
	
	JS_free(cx, ps1);
	JS_free(cx, ps2);
	
	return JS_TRUE;
}

static JSFunctionSpec ape_funcs[] = {
	JS_FS("addEvent",   ape_sm_addEvent,	2, 0), /* Ape.addEvent('name', function() { }); */
	JS_FS("registerCmd", ape_sm_register_cmd, 3, 0),
	JS_FS("registerHookBadCmd", ape_sm_register_bad_cmd, 1, 0),
	JS_FS("registerHookCmd", ape_sm_hook_cmd, 2, 0),
	JS_FS("log",  		ape_sm_echo,  		1, 0),/* Ape.echo('stdout\n'); */
	JS_FS("getPipe", ape_sm_get_pipe, 1, 0),
	JS_FS("getChannelByName", ape_sm_get_channel_by_name, 1, 0),
	JS_FS("getUserByPubid", ape_sm_get_user_by_pubid, 1, 0),
	JS_FS("getChannelByPubid", ape_sm_get_channel_by_pubid, 1, 0),
	JS_FS("config", ape_sm_config, 2, 0),
	JS_FS("mainConfig", ape_sm_mainconfig, 2, 0),
	JS_FS("setTimeout", ape_sm_set_timeout, 2, 0),
	JS_FS("setInterval", ape_sm_set_interval, 2, 0),
	JS_FS("clearTimeout", ape_sm_clear_timeout, 1, 0),
	JS_FS("clearInterval", ape_sm_clear_timeout, 1, 0),
	JS_FS("xorize", ape_sm_xorize, 2, 0),
	JS_FS("addUser", ape_sm_adduser, 1, 0),
	JS_FS("mkChan", ape_sm_mkchan, 1, 0),
	JS_FS("rmChan", ape_sm_rmchan, 1, 0),
	JS_FS("eval", ape_sm_eval, 1, 0),
	JS_FS("getHostByName", ape_sm_gethostbyname, 1, 0), 		//deprecated: is now also in os_funcs
	JS_FS("readfile", ape_sm_readfile, 1, 0),           		//deprecated: is now also in os_funct
	JS_FS("status", ape_sm_status, 1, 0),
	JS_FS_END
};

static JSFunctionSpec os_funcs[] = {
	JS_FS("system", ape_sm_system, 2, 0),
	JS_FS("getHostByName", ape_sm_gethostbyname, 1, 0), 	//deprecated: use Ape.os.resolveHostByName  instead
	JS_FS("resolveHostByName", ape_sm_resolvehostbyname, 2, 0),
	JS_FS("readfile", ape_sm_readfile, 1, 0),
	JS_FS("writefile", ape_sm_writefile, 2, 0),
	JS_FS_END
};

static JSFunctionSpec global_funcs[] = {
	JS_FS("include",   ape_sm_include,	1, 0),
	JS_FS_END
};

static JSFunctionSpec b64_funcs[] = {
	JS_FS("encode",   ape_sm_b64_encode,	1, 0),
	JS_FS("decode",   ape_sm_b64_decode,	1, 0),
	JS_FS_END
};

static JSFunctionSpec sha1_funcs[] = {
	JS_FS("str",   ape_sm_sha1_str,	1, 0),
	JS_FS("bin",   ape_sm_sha1_bin,	1, 0),
	JS_FS_END
};


static void ape_sm_define_ape(ape_sm_compiled *asc, JSContext *gcx, acetables *g_ape)
{
	JSObject *obj, *os, *b64, *sha1, *sockclient, *sockserver, *custompipe, *user, *channel, *subuser;
	#ifdef _USE_MYSQL
	JSObject *jsmysql;
	#endif

	obj = JS_DefineObject(asc->cx, asc->global, "Ape", &ape_class, NULL, 0);
	os = JS_DefineObject(asc->cx, asc->global, "os", &os_class, NULL, 0);
	b64 = JS_DefineObject(asc->cx, obj, "base64", &b64_class, NULL, 0);
	sha1 = JS_DefineObject(asc->cx, obj, "sha1", &sha1_class, NULL, 0);
	user = JS_DefineObject(gcx, obj, "user", &user_class, NULL, 0);
	subuser = JS_DefineObject(gcx, obj, "subuser", &subuser_class, NULL, 0);
	channel = JS_DefineObject(gcx, obj, "channel", &channel_class, NULL, 0);
	
	JS_DefineFunctions(gcx, user, apeuser_funcs);
	JS_DefineFunctions(gcx, channel, apechannel_funcs);
	
	add_property(&g_ape->properties, "user_proto", user, EXTEND_POINTER, EXTEND_ISPRIVATE);
	add_property(&g_ape->properties, "subuser_proto", subuser, EXTEND_POINTER, EXTEND_ISPRIVATE);
	add_property(&g_ape->properties, "channel_proto", channel, EXTEND_POINTER, EXTEND_ISPRIVATE);
	
	JS_DefineFunctions(asc->cx, obj, ape_funcs);
	JS_DefineFunctions(asc->cx, os, os_funcs);
	JS_DefineFunctions(asc->cx, asc->global, global_funcs);
	JS_DefineFunctions(asc->cx, b64, b64_funcs);
	JS_DefineFunctions(asc->cx, sha1, sha1_funcs);
	
	custompipe = JS_InitClass(asc->cx, obj, NULL, &pipe_class, ape_sm_pipe_constructor, 0, NULL, NULL, NULL, NULL);
	add_property(&g_ape->properties, "pipe_proto", custompipe, EXTEND_POINTER, EXTEND_ISPRIVATE);
	
	sockserver = JS_InitClass(asc->cx, obj, NULL, &socketserver_class, ape_sm_sockserver_constructor, 2, NULL, NULL, NULL, NULL);
	sockclient = JS_InitClass(asc->cx, obj, NULL, &socketclient_class, ape_sm_sockclient_constructor, 2, NULL, NULL, NULL, NULL);
	#ifdef _USE_MYSQL
	jsmysql = JS_InitClass(asc->cx, obj, NULL, &mysql_class, ape_sm_mysql_constructor, 2, NULL, NULL, NULL, apemysql_funcs_static);
	#endif
	#if 0
	JS_InitClass(asc->cx, obj, NULL, &raw_class, ape_sm_raw_constructor, 1, NULL, NULL, NULL, NULL); /* Not used */
	#endif

	JS_DefineFunctions(asc->cx, sockclient, apesocket_client_funcs);
	JS_DefineFunctions(asc->cx, sockclient, apesocket_funcs);

	JS_DefineFunctions(asc->cx, sockserver, apesocket_client_funcs);
	JS_DefineFunctions(asc->cx, sockserver, apesocketserver_funcs);

	JS_DefineFunctions(asc->cx, custompipe, apepipe_funcs);
	JS_DefineFunctions(asc->cx, custompipe, apepipecustom_funcs);
	
	#ifdef _USE_MYSQL
	JS_DefineFunctions(asc->cx, jsmysql, apemysql_funcs);
	#endif
	
	JS_SetContextPrivate(asc->cx, asc);
}

static int process_cmd_return(JSContext *cx, jsval rval, callbackp *callbacki, acetables *g_ape)
{
	JSObject *ret_opt = NULL;
	
	if (JSVAL_IS_INT(rval) && JSVAL_TO_INT(rval) == 0) {
		return RETURN_BAD_PARAMS;
	} else if (JSVAL_IS_INT(rval) && JSVAL_TO_INT(rval) == -1) {
		return RETURN_NOTHING;
	} else if (JSVAL_IS_INT(rval) && JSVAL_TO_INT(rval) == -2) {
		return RETURN_HANG;
	} else if (JSVAL_IS_OBJECT(rval)) {
		jsval vp[2];
		ret_opt = JSVAL_TO_OBJECT(rval);
		if (JS_IsArrayObject(cx, ret_opt) == JS_FALSE) {
			jsval rawname, data;

			JS_GetProperty(cx, ret_opt, "name", &rawname);
			JS_GetProperty(cx, ret_opt, "data", &data);
		
			if (!JSVAL_IS_VOID(rawname) && JSVAL_IS_STRING(rawname) && !JSVAL_IS_VOID(data) && JSVAL_IS_OBJECT(data)) {
				json_item *rawdata = NULL;
				
				if ((rawdata = jsobj_to_ape_json(cx, JSVAL_TO_OBJECT(data))) != NULL) {
					char *crawname;
					RAW *newraw;
					
					crawname = JS_EncodeString(cx, JSVAL_TO_STRING(rawname));
					
					newraw = forge_raw(crawname, rawdata);
					send_raw_inline(callbacki->client, callbacki->transport, newraw, g_ape);
					
					JS_free(cx, crawname);
					return RETURN_NULL;
				}
			}
		} else {
			unsigned int length = 0;
			JS_GetArrayLength(cx, ret_opt, &length);
			if (length == 2 && JS_GetElement(cx, ret_opt, 0, &vp[0]) && JS_GetElement(cx, ret_opt, 1, &vp[1]) && !JSVAL_IS_VOID(vp[0]) && !JSVAL_IS_VOID(vp[1])) {
				if (JSVAL_IS_STRING(vp[1])) {
					RAW *newraw;
					JSString *code = JS_ValueToString(cx, vp[0]);
					json_item *jlist = json_new_object();
					char *ccode, *cvalue;
					
					if (callbacki->chl) {
						json_set_property_intN(jlist, "chl", 3, callbacki->chl);
					}
					
					ccode = JS_EncodeString(cx, code);
					cvalue = JS_EncodeString(cx, JSVAL_TO_STRING(vp[1]));

					json_set_property_strZ(jlist, "code", ccode);
					json_set_property_strZ(jlist, "value", cvalue);

					newraw = forge_raw(RAW_ERR, jlist);
					
					JS_free(cx, ccode);
					JS_free(cx, cvalue);
					
					if (callbacki->call_user != NULL) {
						post_raw_sub(newraw, callbacki->call_subuser, g_ape);
					} else {
						send_raw_inline(callbacki->client, callbacki->transport, newraw, g_ape);
						return RETURN_CONTINUE;
					}
					return RETURN_HANG;
				}
			}
		}
	}
	return RETURN_CONTINUE;
}

static int ape_fire_cmd(const char *name, JSObject *obj, JSObject *cb, callbackp *callbacki, acetables *g_ape)
{
	ape_sm_compiled *asc = ASMR->scripts;
	jsval params[2];
	
	if (asc == NULL) {
		return RETURN_CONTINUE;
	}
	params[0] = OBJECT_TO_JSVAL(obj);
	params[1] = OBJECT_TO_JSVAL(cb);

	while (asc != NULL) {
		ape_sm_callback *cbk;
		
		for (cbk = asc->callbacks.head; cbk != NULL; cbk = cbk->next) {
			if ((cbk->type == APE_CMD && strcasecmp(name, cbk->callbackname) == 0)) {
				jsval rval;
				
				if (JS_CallFunctionValue(cbk->cx, JS_GetGlobalObject(cbk->cx), cbk->func, 2, params, &rval) == JS_FALSE) {
					return (cbk->type == APE_CMD ? RETURN_BAD_PARAMS : RETURN_BAD_CMD);
				}
				
				return process_cmd_return(cbk->cx, rval, callbacki, g_ape);
			}
		}
		asc = asc->next;
	}
	return RETURN_CONTINUE;
}

static int ape_fire_hook(ape_sm_callback *cbk, JSObject *obj, JSObject *cb, callbackp *callbacki, acetables *g_ape)
{
	ape_sm_compiled *asc = ASMR->scripts;
	jsval params[3];
	jsval rval;
	int flagret;

	if (asc == NULL) {
		return RETURN_CONTINUE;
	}
	
	params[0] = OBJECT_TO_JSVAL(obj);
	params[1] = OBJECT_TO_JSVAL(cb);
	if (cbk->type == APE_BADCMD) {
		params[2] = STRING_TO_JSVAL(JS_NewStringCopyZ(cbk->cx, callbacki->cmd));
	}
	
	if (JS_CallFunctionValue(cbk->cx, cb, (cbk->func), (cbk->type == APE_BADCMD ? 3 : 2), params, &rval) == JS_FALSE) {
		return (cbk->type != APE_BADCMD ? RETURN_BAD_PARAMS : RETURN_BAD_CMD);
	}
	
	flagret = process_cmd_return(cbk->cx, rval, callbacki, g_ape);

	return (cbk->type == APE_BADCMD && flagret == RETURN_BAD_PARAMS ? RETURN_BAD_CMD : flagret);
}

static void ape_fire_callback(const char *name, uintN argc, jsval *argv, acetables *g_ape)
{
	ape_sm_compiled *asc = ASMR->scripts;
	
	if (asc == NULL) {
		return;
	}
	
	while (asc != NULL) {
		ape_sm_callback *cb;
		
		for (cb = asc->callbacks.head; cb != NULL; cb = cb->next) {
			
			if (cb->type == APE_EVENT && strcasecmp(name, cb->callbackname) == 0) {
				jsval rval;
				
				//JS_SetContextThread(asc->cx);
				//JS_BeginRequest(asc->cx);
				
				JS_CallFunctionValue(asc->cx, asc->global, (cb->func), argc, argv, &rval);
					
				//JS_EndRequest(asc->cx);
				//JS_ClearContextThread(asc->cx);
			}
		}
		
		asc = asc->next;
	}
	
}

/**
 * This event is fired when the server is ready
 *
 * @name Ape.init
 * @event
 * @public
 *
 * @returns {void}
 *
 * @example
 * Ape.addEvent('init', function() {
 * 	var eurasia = {};
 * 	var eastasia = {};
 * 	var oceania = {'habitants': 1984} ;
 * 	var room_101 = APE.mkChan('room_101');
 * 	});
 *
 * @see Ape.addEvent
 * @see Ape.sto
 */
static void init_module(acetables *g_ape) // Called when module is loaded
{
	JSRuntime *rt;
	JSContext *gcx;
	char rpath[512];
	
	char *alt_ape_js, *start_ape_js, main_ape_js[255] = "main.ape.js";
	ape_sm_runtime *asr;
	jsval rval;

	rt = JS_NewRuntime(8L * 1024L * 1024L);
	
	if (rt == NULL) {
		if (!g_ape->is_daemon) {
			printf("[ERR] Not enough memory\n");
		}
		ape_log(APE_ERR, __FILE__, __LINE__, g_ape, "[ERR] Not enough memory");
		exit(0);
	}
	asr = xmalloc(sizeof(*asr));
	asr->runtime = rt;
	asr->scripts = NULL;
	
	/* Setup a global context to store shared object */
	gcx = JS_NewContext(rt, 8192);
	
	add_property(&g_ape->properties, "sm_context", gcx, EXTEND_POINTER, EXTEND_ISPRIVATE);
	add_property(&g_ape->properties, "sm_runtime", asr, EXTEND_POINTER, EXTEND_ISPRIVATE);

	ape_sm_compiled *asc = xmalloc(sizeof(*asc));
	
	memset(rpath, '\0', sizeof(rpath));
	if (infos_module.conf == NULL) {
		if (!g_ape->is_daemon) {
			printf("[ERR] Cannot find %s\n", infos_module.conf_file);
		}
		ape_log(APE_ERR, __FILE__, __LINE__, g_ape, "[ERR] Cannot find %s\n", infos_module.conf_file);
		exit(0);
	}
	strncpy(rpath, READ_CONF("scripts_path"), 255);
	alt_ape_js = xmalloc(sizeof(char) * 255);
	memset(alt_ape_js, '\0', sizeof(main_ape_js));
	alt_ape_js = READ_CONF("autoexec");
	start_ape_js = (alt_ape_js == NULL) ? main_ape_js : alt_ape_js;
	strcat(rpath, start_ape_js);
	
	asc->filename = (void *)xstrdup(rpath);

	asc->cx = gcx;
	
	#if 0
	JS_SetGCZeal(asc->cx, 2);
	#endif
	
	if (asc->cx == NULL) {
		free(asc->filename);
		free(asc);
		return;
	}

	JS_SetOptions(asc->cx, JSOPTION_VAROBJFIX | JSOPTION_JIT | JSOPTION_METHODJIT);
	JS_SetVersion(asc->cx, JSVERSION_LATEST);
	JS_SetErrorReporter(asc->cx, reportError);

	asc->global = JS_NewCompartmentAndGlobalObject(asc->cx, &global_class, NULL);

	JS_InitStandardClasses(asc->cx, asc->global);

	/* define the Ape Object */
	ape_sm_define_ape(asc, gcx, g_ape);

	asc->bytecode = JS_CompileFile(asc->cx, asc->global, asc->filename);

	if (asc->bytecode != NULL) {
		/* put the Ape table on the script structure */
		asc->g_ape = g_ape;

		asc->callbacks.head = NULL;
		asc->callbacks.foot = NULL;
	
		/* Run the script */
		JS_ExecuteScript(asc->cx, asc->global, asc->bytecode, &rval);
	
	}

	if (asc->bytecode == NULL) {
		if (!g_ape->is_daemon) {
			printf("[%s] : Cannot open %s\n", MODULE_NAME, start_ape_js);
		}
		ape_log(APE_INFO, __FILE__, __LINE__, g_ape, "[%s] : Cannot open %s", MODULE_NAME, start_ape_js);
		return;
	} else {
		asc->next = asr->scripts;
		asr->scripts = asc;
	}

	APE_JS_EVENT("init", 0, NULL);
	
}

/**
 * This event is fired when the server is stopped (plugin unload)
 *
 * @name Ape.stop
 * @event
 *
 * @returns {void}
 *
 * @example
 * @public
 * Ape.addEvent('stop', function() {
 * 	Ape.rmChan(room_101);
 * 	delete(eurasia);
 * 	delete(oceania);
 * 	delete(eastasia);
 * 	});
 *
 * @see Ape.addEvent
 * @see Ape.init
 */
static void free_module(acetables *g_ape) // Called when module is unloaded
{

	ape_sm_compiled *asc = ASMR->scripts;
	ape_sm_compiled *prev_asc;
	ape_sm_callback *cb;

	APE_JS_EVENT("stop", 0, NULL);
	while (asc != NULL) {
		free(asc->filename);
		for (cb = asc->callbacks.head; cb; cb = cb->next) {
			JS_RemoveValueRoot(asc->cx, &cb->func);
		}
		JS_DestroyContext(asc->cx);
		prev_asc = asc;
		asc = asc->next;
		free(prev_asc);
	}
	//JS_DestroyContext(ASMC);
	JS_DestroyRuntime(ASMR->runtime);
	free(ASMR);
		if (!g_ape->is_daemon) {
			printf("[%s] Unloaded module\n", MODULE_NAME);
	}
	JS_ShutDown();
	ape_log(APE_ERR, __FILE__, __LINE__, g_ape, "[%s] Unloaded module", MODULE_NAME);
}

/**
 * This is event is fired wen a user successfully connects to the server
 *
 * @name Ape.addUser
 * @event
 * @public
 *
 * @param {user} user The user who joined the channel
 * @returns {void}
 *
 * @example
 * Ape.addEvent('init', function() {});
 * Ape.registerHookCmd('connect', function(params, cmd) {
 * 	if (!$defined(params) || !$defined(params.name)) return 0;
 * 	cmd.user.setProperty('name', params.name);
 * 	return 1;
 * 	});
 * Ape.addEvent('addUser', function(user) {
 * 	oceania.habitants++;
 * 	var bigBro = Ape.getUserByPubid('1984');
 * 	if ( bigBro && user.getProperty('name') == 'Smith, Winston') {
 * 		user.join(room_101);
 * 		bigBro.pipe.sendRaw('ACCEPTANCE', {'fear': 'rats'});
 * 		}
 * 	});
 *
 * @see Ape.addEvent
 * @see Ape.delUser
 * @see Ape.connect
 */
static USERS *ape_cb_add_user(USERS *allocated, acetables *g_ape)
{
	jsval params[1];

	USERS *u = adduser(NULL, NULL, NULL, allocated, g_ape);
	
	if (u != NULL) {
		
		params[0] = OBJECT_TO_JSVAL(APEUSER_TO_JSOBJ(u));
		APE_JS_EVENT("adduser", 1, params);
	}

	return u;
}

static USERS *ape_cb_allocateuser(ape_socket *client, const char *host, const char *ip, acetables *g_ape)
{
	JSObject *user;
	extend *jsobj;
	JSContext *gcx = ASMC;
	jsval pipe;
	
	USERS *u = adduser(client, host, ip, NULL, g_ape);
	
	if (u != NULL) {
		user = JS_NewObject(gcx, &user_class, get_property(g_ape->properties, "user_proto")->val, NULL);

		/* Store the JSObject into a private properties of the user */
		jsobj = add_property(&u->properties, "jsobj", user, EXTEND_POINTER, EXTEND_ISPRIVATE);
		JS_AddObjectRoot(gcx, (JSObject **)&jsobj->val); /* add user object to the gc root */

		JS_SetPrivate(gcx, user, u);
		
		pipe = OBJECT_TO_JSVAL(get_pipe_object(NULL, u->pipe, gcx, g_ape));
		JS_SetProperty(gcx, user, "pipe", &pipe);
	}

	return u;
}

/**
 * This event is fired when an user is deleted from the server (timeout, disconnect etc...)
 *
 * @name Ape.delUser
 * @event
 * @public
 *
 * @param {user} user The user who left the ape
 * @returns {void}
 *
 * @example
 * Ape.addEvent('delUser', function(user) {
 * 	oceania.habitants--;
 * 	});
 *
 * @see Ape.addEvent
 * @see Ape.addUser
 */
static void ape_cb_del_user(USERS *user, int istmp, acetables *g_ape)
{
	jsval params[1];
	extend *jsobj;
	JSObject *pipe;
	JSContext *gcx = ASMC;
	
	if (!istmp) {
		params[0] = OBJECT_TO_JSVAL(APEUSER_TO_JSOBJ(user));
		APE_JS_EVENT("deluser", 1, params);
	}
	
	pipe = user->pipe->data;
	JS_SetPrivate(gcx, pipe, (void *)NULL);
	
	jsobj = get_property(user->properties, "jsobj");
	
	JS_SetPrivate(gcx, jsobj->val, (void *)NULL);
	JS_RemoveObjectRoot(gcx, (JSObject **)&jsobj->val);

	deluser(user, g_ape);
}

static CHANNEL *ape_cb_mkchan(char *name, int flags, acetables *g_ape)
{
	JSObject *js_channel;
	extend *jsobj;
	jsval params[1], pipe;
	JSContext *gcx = ASMC;
	CHANNEL *chan;

	if ((chan = mkchan(name, flags, g_ape)) == NULL) {
		return NULL;
	}

	//JS_SetContextThread(gcx);
	//JS_BeginRequest(gcx);

		js_channel = JS_NewObject(gcx, &channel_class, get_property(g_ape->properties, "channel_proto")->val, NULL);
		
		if (js_channel == NULL) {
			return NULL;
		}
		
		jsobj = add_property(&chan->properties, "jsobj", js_channel, EXTEND_POINTER, EXTEND_ISPRIVATE);
		JS_AddObjectRoot(gcx, (JSObject **)&jsobj->val);

		pipe = OBJECT_TO_JSVAL(get_pipe_object(NULL, chan->pipe, gcx, g_ape));
		JS_SetProperty(gcx, js_channel, "pipe", &pipe);
		JS_SetPrivate(gcx, js_channel, chan);

	//JS_EndRequest(gcx);
	//JS_ClearContextThread(gcx);
	
	params[0] = OBJECT_TO_JSVAL(js_channel);
	APE_JS_EVENT("mkchan", 1, params);
	
	return chan;
}

static void ape_cb_rmchan(CHANNEL *chan, acetables *g_ape)
{
	extend *jsobj;
	jsval params[1];
	JSObject *pipe;
	JSContext *gcx = ASMC;
	
	params[0] = OBJECT_TO_JSVAL(APECHAN_TO_JSOBJ(chan));
	
	APE_JS_EVENT("rmchan", 1, params);
	
	jsobj = get_property(chan->properties, "jsobj");
	
	JS_SetPrivate(gcx, jsobj->val, (void *)NULL);
	JS_RemoveObjectRoot(gcx, (JSObject **)&jsobj->val);
	
	pipe = chan->pipe->data;
	JS_SetPrivate(gcx, pipe, (void *)NULL);
	
	rmchan(chan, g_ape);
}

/**
 * This is event is fired just before an user joins a channel, can be useful to add a public property to the user that will be sent to all users on the channel.
 *
 * @name Ape.beforeJoin
 * @event
 * @public
 *
 * @param {user} user The user who joined the channel
 * @param {channel} channel The joined channel
 * @returns {void}
 *
 * @example
 * Ape.addEvent('beforeJoin', function(user, channel) {
 * 	if ( user.safe.role == 'captain' &&  channel.getProperty('name') == 'bridge') {
 * 		user.setProperty('captianOnDeck',true);
 * 	}
 * });
 *
 * @see Ape.addEvent
 * @see Ape.join
 * @see Ape.left
 * @see Ape.afterJoin
 */

 /**
 * This is event is fired when an user joins a channel.
 *
 * @name Ape.join
 * @event
 * @public
 *
 * @param {user} user The user who joined the channel
 * @param {channel} channel The joined channel
 * @returns {void}
 *
 * @example
 * Ape.addEvent('join', function(user, channel) {
 * 	user.pipe.sendRaw('WELCOME', {});
 * });
 * @see Ape.addEvent
 * @see Ape.join
 * @see Ape.left
 * @see Ape.beforeJoin
 *
 * @fires Ape.afterJoin
 * @fires Ape.beforeJoin
 */

/**
 * This is event is fired just after an user joins a channel.
 *
 * @name Ape.afterJoin
 * @event
 * @public
 *
 * @param {user} user The user who joined the channel
 * @param {channel} channel The joined channel
 * @returns {void}
 *
 * @example
 * Ape.addEvent('afterJoin', function(user, channel) {
 * 	if( user.getProperty('name') == 'Smith, Winston') {
 * 		user.save.inLoveWithJulia = true;
 * 	}
 * });
 *
 * @see Ape.addEvent
 * @see Ape.join
 * @see Ape.left
 * @see Ape.beforeJoin
 */
static void ape_cb_join(USERS *user, CHANNEL *chan, acetables *g_ape)
{
	jsval params[2];
	
	params[0] = OBJECT_TO_JSVAL(APEUSER_TO_JSOBJ(user));
	params[1] = OBJECT_TO_JSVAL(APECHAN_TO_JSOBJ(chan));
	
	APE_JS_EVENT("beforeJoin", 2, params);
	
	join(user, chan, g_ape);
	
	APE_JS_EVENT("afterJoin", 2, params);
	APE_JS_EVENT("join", 2, params);
}

/**
 * This is event is fired when a user leaves a channel
 *
 * @name Ape.left
 * @event
 * @public
 *
 * @param {user} user The user who leaves the channel
 * @param {channel} channel The abandoned channel
 * @returns {void}
 *
 * @example
 * Ape.addEvent('left', function(user, channel) {
 * 	if( channel.getProperty('name')== 'room_101' && user.getProperty('name') == 'Smith, Winston') {
 * 		user.save.inLoveWithJulia = false;
 * 	}
 * });
 *
 * @see Ape.addEvent
 * @see Ape.join
 * @see Ape.left
 * @see Ape.beforeJoin
 * @see Ape.afterJoin
 */
static void ape_cb_left(USERS *user, CHANNEL *chan, acetables *g_ape)
{
	jsval params[2];
	
	params[0] = OBJECT_TO_JSVAL(APEUSER_TO_JSOBJ(user));
	params[1] = OBJECT_TO_JSVAL(APECHAN_TO_JSOBJ(chan));
	
	APE_JS_EVENT("left", 2, params);
	
	left(user, chan, g_ape);

}

/**
 * An subuser object (tabbing in browser sessions).
 * <p>The subuser object is available on registerCmd's callback.</p>
 * <p>Each time a client opens a new tab on the same application (or refreshes the page) a new subuser is created to represent this client's instance.</p>
 * <p>You can send a message directly to this subuser, or send a message to all others subuser.</p>
 *
 * @name Ape.subuser
 * @class
 *
 * @example
 * Ape.registerCmd('foo', true, function(params, info) {
 * 	info.subuser.sendRaw('bar', {'ok':'true'});
 * });
 *
 * @see Ape.subuser.SendRaw
 * @see Ape.registerCmd
 */
static void ape_cb_addsubuser(subuser *sub, acetables *g_ape)
{
	JSObject *subjs;
	extend *jsobj;
	JSContext *gcx = ASMC;
	
	subjs = JS_NewObject(gcx, &subuser_class, get_property(g_ape->properties, "subuser_proto")->val, NULL);
	
	if (subjs == NULL) {
		return;
	}
	
	jsobj = add_property(&sub->properties, "jsobj", subjs, EXTEND_POINTER, EXTEND_ISPRIVATE);
	JS_AddObjectRoot(gcx, (JSObject **)&jsobj->val);
	
	JS_SetPrivate(gcx, subjs, sub);
	
}

static void ape_cb_delsubuser(subuser *sub, acetables *g_ape)
{
	extend *jsobj;
	JSContext *gcx = ASMC;
	
	jsobj = get_property(sub->properties, "jsobj");
	
	JS_SetPrivate(gcx, jsobj->val, (void *)NULL);
	JS_RemoveObjectRoot(gcx, (JSObject **)&jsobj->val);
	
}

static ace_callbacks callbacks = {
	ape_cb_add_user,	/* Called when new user is added */
	ape_cb_del_user,	/* Called when a user is disconnected */
	ape_cb_mkchan,		/* Called when new chan is created */
	ape_cb_rmchan,		/* Called when a chan is deleted */
	ape_cb_join,		/* Called when a user join a channel */
	ape_cb_left,		/* Called when a user leave a channel */
	NULL,
	NULL,
	ape_cb_allocateuser,
	ape_cb_addsubuser,
	ape_cb_delsubuser
};

APE_INIT_PLUGIN(MODULE_NAME, init_module, free_module, callbacks)

