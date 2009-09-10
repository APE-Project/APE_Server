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

/* json.c */

#include <stdio.h>
#include <stdlib.h>

#include <string.h>

#include "json.h"
#include "utils.h"

void set_json(const char *name, const char *value, struct json **jprev)
{
	struct json *new_json, *old_json = *jprev;
	
	
	new_json = xmalloc(sizeof(struct json));
	
	new_json->name.len = strlen(name);
	new_json->name.buf = xmalloc(sizeof(char) * (new_json->name.len + 1));
	memcpy(new_json->name.buf, name, new_json->name.len + 1);
	
	
	new_json->value.len = (value != NULL ? strlen(value) : 0);
	
	if (new_json->value.len) {
		new_json->value.buf = xmalloc(sizeof(char) * (new_json->value.len + 1));
		memcpy(new_json->value.buf, value, new_json->value.len + 1);
	} else {
		new_json->value.buf = NULL;
	}
	
	new_json->jfather = NULL;
	new_json->jchilds = NULL;
	
	new_json->next = old_json;
	new_json->prev = NULL;
	
	if (old_json != NULL) {
		old_json->prev = new_json;
	}
	
	*jprev = new_json;
}

struct json *json_copy(struct json *jbase)
{
	struct json *new_json = xmalloc(sizeof(struct json));
	struct json_childs *jchilds = jbase->jchilds;
	
	new_json->name.len = jbase->name.len;
	new_json->name.buf = xmalloc(sizeof(char) * (new_json->name.len + 1));
	
	memcpy(new_json->name.buf, jbase->name.buf, new_json->name.len + 1);
	
	new_json->value.len = jbase->value.len;
	
	if (jbase->value.len) {
		
		new_json->value.buf = xmalloc(sizeof(char) * (new_json->value.len + 1));
		memcpy(new_json->value.buf, jbase->value.buf, new_json->value.len + 1);
	
	} else {
		new_json->value.buf = NULL;
	}
	new_json->prev = NULL;
	new_json->next = NULL;
	new_json->jfather = NULL;
	
	new_json->jchilds = NULL;
	
	if (jbase->next != NULL) {
		new_json->next = json_copy(jbase->next);
		new_json->next->prev = new_json;
	}
		
	
	while (jchilds != NULL) {
		struct json_childs *new_child = xmalloc(sizeof(struct json_childs));
		
		new_child->type = jchilds->type;
		new_child->child = json_copy(jchilds->child);
		new_child->child->jfather = new_json;
		new_child->next = new_json->jchilds;
		
		new_json->jchilds = new_child;
		
		jchilds = jchilds->next;
	}
	
	
	return new_json;
}

void json_free(struct json *jbase)
{

	struct json_childs *jchilds = jbase->jchilds;
	
	free(jbase->name.buf);
	
	if (jbase->value.buf != NULL) {
		free(jbase->value.buf);
	}

	
	if (jbase->next != NULL) {
		json_free(jbase->next);

	}
		
	while (jchilds != NULL) {

		json_free(jchilds->child);
		
		jchilds = jchilds->next;
	}
	
	free(jbase);

}

void json_attach(struct json *json_father, struct json *json_child, unsigned int type)
{
	struct json_childs *ochild = json_father->jchilds, *nchild;
	
	json_child->jfather = json_father;
	
	nchild = xmalloc(sizeof(struct json_childs));
	
	nchild->child = json_child;
	nchild->next = ochild;
	nchild->type = type;
	
	json_father->jchilds = nchild;
}

void json_concat(struct json *json_father, struct json *json_child)
{
	struct json *jTmp = json_father->next;
	
	if (jTmp == NULL) {
		json_father->next = json_child;
		json_child->prev = json_father;
		
		return;
	}
	
	while (jTmp != NULL) {
		if (jTmp->next == NULL) {
			jTmp->next = json_child;
			json_child->prev = jTmp;
			return;
		}
		jTmp = jTmp->next;
	}

}

/*
	Transforme a JSON tree to a allocated string and free memory
*/
struct jsontring *jsontr(struct json *jlist, struct jsontring *string)
{
	struct json_childs *pchild;
	struct json *pjson;
	size_t string_osize = 0;

	
	if (string == NULL) { // initial size
		string = xmalloc(sizeof(struct jsontring));
		string->jsize = jlist->name.len+3 + 
				(jlist->jchilds == NULL ? (jlist->value.buf != NULL ? jlist->value.len+2 : 4) : 0) + 
				(jlist->prev == NULL ? 2 : 1) + (jlist->next == NULL && jlist->jchilds == NULL ? 1 : 0) + 2; /* Estimate init size ~ 2 bytes */
				
		string->jstring = xmalloc(sizeof(char) * string->jsize);
		string->len = 0;
		
	} else {

		string_osize = string->jsize;
		string->jsize += jlist->name.len+3 + 
				(jlist->jchilds == NULL ? (jlist->value.buf != NULL ? jlist->value.len+2 : 4) : 0) + 
				(jlist->prev == NULL ? 2 : 1) + (jlist->next == NULL && jlist->jchilds == NULL ? 1 : 0) + 2; /* Estimate new size ~ 2 bytes */
				
		string->jstring = xrealloc(string->jstring, sizeof(char) * string->jsize);
	}
	memset(string->jstring + string_osize, '\0', string->jsize - string_osize);
	
	pchild = jlist->jchilds;
	pjson = jlist->next;
	
	if (jlist->prev == NULL) {
		string->jstring[string->len++] = '{';
	}
	
	string->jstring[string->len++] = '"';
	
	memcpy(&string->jstring[string->len], jlist->name.buf, jlist->name.len);
	
	string->len += jlist->name.len;
	
	string->jstring[string->len++] = '"';
	string->jstring[string->len++] = ':';
	
	free(jlist->name.buf);
	if (pchild == NULL) {
		if (jlist->value.buf != NULL) {
			string->jstring[string->len++] = '"';
			memcpy(&string->jstring[string->len], jlist->value.buf, jlist->value.len);
			string->len += jlist->value.len;
			string->jstring[string->len++] = '"';

			free(jlist->value.buf);
		} else {
			strncpy(&string->jstring[string->len], "null", 4);
			string->len += 4;

		}
	} else if (pchild->type == JSON_ARRAY) {
		string->jstring[string->len++] = '[';
	}
	while (pchild != NULL) {
		struct json_childs *pPchild = pchild;

		jsontr(pchild->child, string);
		
		pchild = pchild->next;
		
		if (pchild == NULL && pPchild->type == JSON_ARRAY) {
			string->jstring[string->len++] = ']';

		} else if (pchild != NULL) {
			string->jstring[string->len++] = ',';
		}
		free(pPchild);
	}
	if (jlist->next == NULL) {
		string->jstring[string->len++] = '}';
	} else {
		string->jstring[string->len++] = ',';
	}
	if (pjson != NULL) {

		jsontr(pjson, string);
	}

	if (jlist->jchilds == NULL) {
		free(jlist);
		
		return string;
	}
	free(jlist);
	return string;
}

static json_item *init_json_item()
{
	
	json_item *jval = xmalloc(sizeof(*jval));

	jval->father = NULL;
	jval->jchild.child = NULL;
	jval->jchild.type = JSON_C_T_NULL;
	jval->next = NULL;
	
	jval->key.val = NULL;
	jval->key.len = 0;
	
	jval->jval.vu.str.value = NULL;
	jval->jval.vu.integer_value = 0;
	jval->jval.vu.float_value = 0.;
	
	jval->type = -1;
	
	return jval;
}

void free_json_item(json_item *cx)
{
	while (cx != NULL) {
		json_item *tcx;

		if (cx->key.val != NULL) {
			free(cx->key.val);
		}
		if (cx->jval.vu.str.value != NULL) {
			free(cx->jval.vu.str.value);
		}
		if (cx->jchild.child != NULL) {
			free_json_item(cx->jchild.child);
		}
		tcx = cx->next;
		free(cx);
		cx = tcx;
	}
}

static int json_callback(void *ctx, int type, const JSON_value* value)
{
	json_context *cx = (json_context *)ctx;
	json_item *jval = NULL;
	
	switch(type) {
		case JSON_T_OBJECT_BEGIN:
		case JSON_T_ARRAY_BEGIN:
			
			if (!cx->key_under) {
				jval = init_json_item();
				
				if (cx->current_cx != NULL) {
					if (cx->start_depth) {						
						cx->current_cx->jchild.child = jval;
						jval->father = cx->current_cx;
					} else {
						
						jval->father = cx->current_cx->father;
						cx->current_cx->next = jval;
					}
				}
				cx->current_cx = jval;
			}
			
			cx->current_cx->jchild.type = (type == JSON_T_OBJECT_BEGIN ? JSON_C_T_OBJ : JSON_C_T_ARR);
			cx->start_depth = 1;
			cx->key_under = 0;
			break;
		case JSON_T_OBJECT_END:
		case JSON_T_ARRAY_END:
			
			/* If the father node exists, back to it */
			if (cx->current_cx->father != NULL) {
				cx->current_cx = cx->current_cx->father;
			}
			cx->start_depth = 0;
			cx->key_under = 0;
			break;

		case JSON_T_KEY:
			jval = init_json_item();

			if (cx->start_depth) {
				cx->current_cx->jchild.child = jval;
				jval->father = cx->current_cx;
				cx->start_depth = 0;
			} else {
				jval->father = cx->current_cx->father;
				cx->current_cx->next = jval;
			}				

			cx->current_cx = jval;
			cx->key_under = 1;
			
			cx->current_cx->key.val = xmalloc(sizeof(char) * (value->vu.str.length+1));
			memcpy(cx->current_cx->key.val, value->vu.str.value, value->vu.str.length+1);
			
			cx->current_cx->key.len = value->vu.str.length;
			
			break;  
			
		case JSON_T_INTEGER:
		case JSON_T_FLOAT:
		case JSON_T_NULL:
		case JSON_T_TRUE:
		case JSON_T_FALSE:
		case JSON_T_STRING:
						
			if (!cx->key_under) {
			
				jval = init_json_item();

				if (cx->start_depth) {
					cx->current_cx->jchild.child = jval;
					jval->father = cx->current_cx;
					cx->start_depth = 0;
				} else {
					jval->father = cx->current_cx->father;
					cx->current_cx->next = jval;
				}	
		
				cx->current_cx = jval;				
			}
			cx->key_under = 0;
			
			switch(type) {
				case JSON_T_INTEGER:
					cx->current_cx->jval.vu.integer_value = value->vu.integer_value;
					break;
				case JSON_T_FLOAT:
					cx->current_cx->jval.vu.float_value = value->vu.float_value;
					break;
				case JSON_T_NULL:
				case JSON_T_FALSE:
					cx->current_cx->jval.vu.integer_value = 0;
					break;
				case JSON_T_TRUE:
					cx->current_cx->jval.vu.integer_value = 1;
					break;
				case JSON_T_STRING:
					cx->current_cx->jval.vu.str.value = xmalloc(sizeof(char) * (value->vu.str.length+1));
					memcpy(cx->current_cx->jval.vu.str.value, value->vu.str.value, value->vu.str.length+1);
					cx->current_cx->jval.vu.str.length = value->vu.str.length;			
					break;
			}
			cx->current_cx->type = type;
			break;
		default:

			break;
	}
	
   	if (cx->head == NULL && cx->current_cx != NULL) {
   		cx->head = cx->current_cx;
   	}
   	
	return 1;
}

json_item *init_json_parser(const char *json_string)
{
	const char *pRaw;
	JSON_config config;

	struct JSON_parser_struct* jc = NULL;
	
	json_context jcx = {0, 0, NULL, NULL};

	init_JSON_config(&config);
	
	config.depth		= 15;
	config.callback		= &json_callback;
	config.callback_ctx	= &jcx;
	
	config.allow_comments	= 0;
	config.handle_floats_manually = 0;

	jc = new_JSON_parser(&config);

	for (pRaw = json_string; *pRaw; pRaw++) {
		if (!JSON_parser_char(jc, *pRaw)) {
			free_json_item(jcx.head);
		    delete_JSON_parser(jc);
		    return NULL;
		}
	}
	
	if (!JSON_parser_done(jc)) {
		free_json_item(jcx.head);
		delete_JSON_parser(jc);
		return NULL;
	}

	delete_JSON_parser(jc);
	
	return jcx.head;	
}

static void aff(json_item *cx, int depth)
{

	while (cx != NULL) {
		if (cx->key.val != NULL) {
			printf("Key %s\n", cx->key.val);
		}
		if (cx->jval.vu.str.value != NULL) {
			printf("Value : %s\n", cx->jval.vu.str.value);
		}
		if (depth && cx->jchild.child != NULL) {
			aff(cx->jchild.child, depth - 1);
		}
		cx = cx->next;
	}
}

/* "val[32]" return 32, "val" return -1 */
#if 0
static int key_is_array(char *key, int i)
{
	int ret = 0, f = 1;
	
	if (i < 4 || key[i--] != ']' || *key == '[') {
		return -1;
	}
	
	while (i != 0 && key[i] != '[') {
		if (f == 10000) {
			return -1;
		}
		if (key[i] < 48 || key[i] > 57) {
			return -1;
		}
		
		ret += (key[i] - 48) * f;
		f *= 10;
		i--;
	}
	
	return ret;
}
#endif

json_item *json_lookup(json_item *head, char *path)
{
	char *split[16];
	char *base;
	size_t nTok;
	int i = 0;
	
	if (head == NULL || path == NULL) {
		return NULL;
	}
	
	base = xstrdup(path);

	nTok = explode('.', base, split, 15);
	
	while (head != NULL && i <= nTok) {
		if (head->key.val != NULL && strcasecmp(split[i], head->key.val) == 0) {
			/*
			printf("Array %s : %i\n", split[i], key_is_array(split[i], strlen(split[i])-1));
			printf("Comparing : %s with %s\n", head->key.val, split[i]);
			printf("Find !\n");
			*/
			if (i == nTok) {
				free(base);
				return (head->jchild.child != NULL ? head->jchild.child : head);
			}
			i++;
			head = head->jchild.child;
			continue;
		}

		head = head->next;
	}
	free(base);
	return NULL;
}

