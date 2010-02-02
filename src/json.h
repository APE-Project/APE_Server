/*
  Copyright (C) 2006, 2007, 2008, 2009, 2010  Anthony Catel <a.catel@weelya.com>

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

/* json.h */

#ifndef _JSON_H
#define _JSON_H


#include "json_parser.h"


typedef char* jpath;

enum {
	JSON_ARRAY = 0,
	JSON_OBJECT
};

typedef struct json {
	struct {
		char *buf;
		size_t len;
	} name;
	
	struct {
		char *buf;
		size_t len;
	} value;
	
	struct json_childs *jchilds;
	
	struct json *jfather;
	
	struct json *next;
	struct json *prev;
} json;

struct json_childs {
	struct json *child;
	struct json_childs *next;
	unsigned int type;
};

struct jsontring {
	char *jstring;
	size_t jsize;
	size_t len;
};

typedef enum {
	JSON_C_T_OBJ,
	JSON_C_T_ARR,
	JSON_C_T_VAL,
	JSON_C_T_NULL,
} json_child_t;

typedef struct _json_item {
	struct JSON_value_struct jval;
	struct {
		struct _json_item *child;
		struct _json_item *head;
		json_child_t type;
	} jchild;
	
	struct {
        	char *val;
        	size_t len;
	} key;
	
	struct _json_item *father;
	struct _json_item *next;
			
	int type;
	
} json_item;


typedef struct _json_context {
	int key_under;
	int start_depth;
		
	json_item *head;
	json_item *current_cx;
	
} json_context;


void set_json(const char *name, const char *value, struct json **jprev);
struct json *json_copy(struct json *jbase);
void json_attach(struct json *json_father, struct json *json_child, unsigned int type);
void json_concat(struct json *json_father, struct json *json_child);
void json_free(struct json *jbase);
json_item *init_json_parser(const char *json_string);
json_item *json_lookup(json_item *head, char *path);
void free_json_item(json_item *cx);

json_item *json_new_object();
json_item *json_new_array();

void json_set_property_objN(json_item *obj, const char *key, int keylen, json_item *value);
void json_set_property_objZ(json_item *obj, const char *key, json_item *value);

void json_set_property_strN(json_item *obj, const char *key, int keylen, const char *value, int valuelen);
void json_set_property_strZ(json_item *obj, const char *key, const char *value);

void json_set_element_strN(json_item *obj, const char *value, int valuelen);
void json_set_element_strZ(json_item *obj, const char *value);
void json_set_element_int(json_item *obj, long int value);
void json_set_element_obj(json_item *obj, json_item *value);

void json_set_property_intN(json_item *obj, const char *key, int keylen, long int value);
void json_set_property_intZ(json_item *obj, const char *key, long int value);
struct jsontring *json_to_string(json_item *head, struct jsontring *string, int free_tree);
json_item *json_item_copy(json_item *cx, json_item *father);

void json_aff(json_item *cx, int depth);

#define APE_PARAMS_INIT() \
	int json_iterator; \
	json_iterator = 0; \
	json_item *json_params = NULL


/* Iterate over a JSON array */
#define JFOREACH(fkey, outvar) \
	for (json_params = json_lookup(callbacki->param, #fkey), json_iterator = 0; json_params != NULL; json_params = json_params->next) \
		if ((outvar = (char *)json_params->jval.vu.str.value) != NULL && ++json_iterator)

/* Iterate over a JSON Object */
#define JFOREACH_K(fkey, outkey, outvar) \
		for (json_params = json_lookup(callbacki->param, #fkey), json_iterator = 0; json_params != NULL; json_params = json_params->next) \
			if ((outvar = (char *)json_params->jval.vu.str.value) != NULL && (outkey = (char *)json_params->key.val) != NULL && ++json_iterator)		

#define JFOREACH_ELSE \
	if (json_iterator == 0)
	
#define JSTR(key) \
	(char *)(callbacki->param != NULL && (json_params = json_lookup(callbacki->param, #key)) != NULL ? json_params->jval.vu.str.value : NULL)

#define JINT(key) \
	(int)(callbacki->param != NULL && (json_params = json_lookup(callbacki->param, #key)) != NULL ? json_params->jval.vu.integer_value : 0)
	
#define JFLOAT(key) \
	(callbacki->param != NULL && (json_params = json_lookup(callbacki->param, #key)) != NULL ? json_params->jval.vu.float_value : 0.)
	
#define JGET_STR(head, key) \
	json_lookup(head, #key)->jval.vu.str.value

#endif

