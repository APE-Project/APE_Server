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

/* Determinate an heuristic final string size */
static int json_evaluate_string_size(json_item *head)
{
	int evalsize = 2;
	
	while (head != NULL) {
		if (head->key.val != NULL) {
			evalsize += head->key.len + 3;
		}
		
		if (head->jval.vu.str.value != NULL) {
			evalsize += head->jval.vu.str.length + 3;
		} else if (head->jchild.child == NULL) {
			evalsize += 16; /* uch ! TODO : int length */
		}
		if (head->jchild.child != NULL) {
			evalsize += json_evaluate_string_size(head->jchild.child);
		}
		head = head->next;
	}	
	
	return evalsize;
}

static int escape_json_string(char *in, char *out, int len)
{
	int i, e;
	
	for (i = 0, e = 0; i < len; i++, e++) {
		
		switch(in[i]) {
			case '"':
				out[e++] = '\\';
				out[e] = '"';
				break;
			case '\\':
				out[e++] = '\\';
				out[e] = '\\';
				break;
			case '\n':
				out[e++] = '\\';
				out[e] = 'n';
				break;
			case '\b':
				out[e++] = '\\';
				out[e] = 'b';
				break;
			case '\t':
				out[e++] = '\\';
				out[e] = 't';
				break;
			case '\f':
				out[e++] = '\\';
				out[e] = 'f';
				break;
			case '\r':
				out[e++] = '\\';
				out[e] = 'r';
				break;
			default: 
				out[e] = in[i];
				break;
		}

	}
	return e;
}

struct jsontring *json_to_string(json_item *head, struct jsontring *string, int free_tree)
{
	if (string == NULL) {
		string = xmalloc(sizeof(struct jsontring));
		
		/* Ok, this can cost a lot (traversing the tree), but avoid realloc at each iteration */
		string->jsize = json_evaluate_string_size(head) * 2; /* TODO : Remove * 2, add some padding, realloc when necessary (or at least just x2 str val) */
		
		string->jstring = xmalloc(sizeof(char) * (string->jsize + 1));
		string->len = 0;
	}
	 
	while (head != NULL) {
		
		if (head->key.val != NULL) {			
			string->jstring[string->len++] = '"';
			memcpy(string->jstring + string->len, head->key.val, head->key.len);
			string->len += head->key.len;
			string->jstring[string->len++] = '"';
			string->jstring[string->len++] = ':';
			
			if (free_tree) {
				free(head->key.val);
			}
		}
		
		if (head->jval.vu.str.value != NULL) {

			string->jstring[string->len++] = '"';
			string->len += escape_json_string(head->jval.vu.str.value, string->jstring + string->len, head->jval.vu.str.length); /* TODO : Add a "escape" argument to json_to_string */	
			string->jstring[string->len++] = '"';
			
			if (free_tree) {
				free(head->jval.vu.str.value);
			}
		} else if (head->jval.vu.integer_value) {
			
			long int l = LENGTH_N(head->jval.vu.integer_value);
			long int offset;
			char integer_str[l+2];

			offset = itos(head->jval.vu.integer_value, integer_str, l+2);
			
			memcpy(string->jstring + string->len, &integer_str[offset], ((l+2)-1)-offset);
			
			string->len += ((l+2)-1)-offset;
			
		} else if (head->jval.vu.float_value) {
			int length;

			/* TODO: check for -1 */
			/* TODO: fix max length 16 together with json_evaluate_string_size() */
			length = snprintf(string->jstring + string->len, 16 + 1, "%Lf", head->jval.vu.float_value);
			if(length > 16) /* cut-off number */
				length = 16;

			string->len += length;
		} else if (head->type == JSON_T_TRUE) {
			memcpy(string->jstring + string->len, "true", 4);
			string->len += 4;
		} else if (head->type == JSON_T_FALSE) {
			memcpy(string->jstring + string->len, "false", 5);
			string->len += 5;
		} else if (head->type == JSON_T_NULL) {
			memcpy(string->jstring + string->len, "null", 4);
			string->len += 4;
		} else if (head->jchild.child == NULL) {
			memcpy(string->jstring + string->len, "0", 1);
			string->len++;
		}
		
		if (head->jchild.child != NULL) {
			switch(head->jchild.type) {
				case JSON_C_T_OBJ:
					string->jstring[string->len++] = '{';
					break;
				case JSON_C_T_ARR:
					string->jstring[string->len++] = '[';
					break;
				default:
					break;
			}
			json_to_string(head->jchild.child, string, free_tree);

		}
		
		if (head->father != NULL) {
			if (head->next != NULL) {
				string->jstring[string->len++] = ',';
			} else {
				switch(head->father->jchild.type) {
					case JSON_C_T_OBJ:
						string->jstring[string->len++] = '}';
						break;
					case JSON_C_T_ARR:
						string->jstring[string->len++] = ']';
						break;
					default:
						break;
				}
			}
		}
		if (free_tree) {
			json_item *jtmp = head->next;
			free(head);
			head = jtmp;
		} else {
			head = head->next;
		}
	}
	string->jstring[string->len] = '\0';
	
	return string;
}

static json_item *init_json_item()
{
	
	json_item *jval = xmalloc(sizeof(*jval));

	jval->father = NULL;
	jval->jchild.child = NULL;
	jval->jchild.head = NULL;
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

json_item *json_item_copy(json_item *cx, json_item *father)
{
	json_item *new_item = NULL, *return_item = NULL;
	json_item *temp_item = NULL;

	while (cx != NULL) {	
		new_item = init_json_item();
		new_item->father = father;
		
		if (return_item == NULL) {
			return_item = new_item;
		}

		new_item->type = cx->type;
		new_item->jchild.type = cx->jchild.type;
		
		if (temp_item != NULL) {
			temp_item->next = new_item;
		}
		
		if (cx->key.val != NULL) {
			new_item->key.len = cx->key.len;
			new_item->key.val = xmalloc(sizeof(char) * (cx->key.len + 1));
			memcpy(new_item->key.val, cx->key.val, cx->key.len + 1);
		}
		
		if (cx->jval.vu.str.value != NULL) {
			new_item->jval.vu.str.length = cx->jval.vu.str.length;
			new_item->jval.vu.str.value = xmalloc(sizeof(char) * (cx->jval.vu.str.length + 1));
			memcpy(new_item->jval.vu.str.value, cx->jval.vu.str.value, cx->jval.vu.str.length + 1);
		} else if (cx->jval.vu.integer_value) {
			new_item->jval.vu.integer_value = cx->jval.vu.integer_value;
		} else if (cx->jval.vu.float_value) {
			new_item->jval.vu.float_value = cx->jval.vu.float_value;
		}
		if (cx->jchild.child != NULL) {
			new_item->jchild.child = json_item_copy(cx->jchild.child, new_item);
		}
		if (new_item->father != NULL) {
			new_item->father->jchild.head = new_item;
		}
		temp_item = new_item;
		
		cx = cx->next;
	}
	
	return return_item;
}

json_item *json_new_object()
{
	json_item *obj = init_json_item();
	obj->jchild.type = JSON_C_T_OBJ;
	
	return obj;
}

json_item *json_new_array()
{
	json_item *obj = init_json_item();
	obj->jchild.type = JSON_C_T_ARR;
	
	return obj;
}

void json_set_property_objN(json_item *obj, const char *key, int keylen, json_item *value)
{
	json_item *new_item = value;
	
	if (key != NULL) {
		new_item->key.val = xmalloc(sizeof(char) * (keylen + 1));
		memcpy(new_item->key.val, key, keylen + 1);
		new_item->key.len = keylen;
	}
	
	new_item->father = obj;

	if (obj->jchild.child == NULL) {
		obj->jchild.child = new_item;
	} else {
		obj->jchild.head->next = new_item;
	}
	
	obj->jchild.head = new_item;
}

void json_set_property_objZ(json_item *obj, const char *key, json_item *value)
{
	json_set_property_objN(obj, key, strlen(key), value);
}

void json_set_property_intN(json_item *obj, const char *key, int keylen, long int value)
{
	json_item *new_item = init_json_item();
	
	if (key != NULL) {
		new_item->key.val = xmalloc(sizeof(char) * (keylen + 1));
		memcpy(new_item->key.val, key, keylen + 1);
		new_item->key.len = keylen;
	}
	new_item->father = obj;
	new_item->jval.vu.integer_value = value;
	new_item->type = JSON_T_INTEGER;
	
	if (obj->jchild.child == NULL) {
		obj->jchild.child = new_item;
	} else {
		obj->jchild.head->next = new_item;
	}
	
	obj->jchild.head = new_item;	
}

void json_set_property_intZ(json_item *obj, const char *key, long int value)
{
	int len = (key != NULL ? strlen(key) : 0);
	
	json_set_property_intN(obj, key, len, value);
}

void json_set_property_floatN(json_item *obj, const char *key, int keylen, long double value)
{
	json_item *new_item = init_json_item();

	if (key != NULL) {
		new_item->key.val = xmalloc(sizeof(char) * (keylen + 1));
		memcpy(new_item->key.val, key, keylen + 1);
		new_item->key.len = keylen;
	}
	new_item->father = obj;
	new_item->jval.vu.float_value = value;
	new_item->type = JSON_T_FLOAT;
	
	if (obj->jchild.child == NULL) {
		obj->jchild.child = new_item;
	} else {
		obj->jchild.head->next = new_item;
	}
	
	obj->jchild.head = new_item;	
}

void json_set_property_boolean(json_item *obj, const char *key, int keylen, int value)
{
	json_item *new_item = init_json_item();

	if (key != NULL) {
		new_item->key.val = xmalloc(sizeof(char) * (keylen + 1));
		memcpy(new_item->key.val, key, keylen + 1);
		new_item->key.len = keylen;
	}
	new_item->father = obj;
	if(value)
		new_item->type = JSON_T_TRUE;
	else
		new_item->type = JSON_T_FALSE;
	
	if (obj->jchild.child == NULL) {
		obj->jchild.child = new_item;
	} else {
		obj->jchild.head->next = new_item;
	}
	
	obj->jchild.head = new_item;	
}

void json_set_property_null(json_item *obj, const char *key, int keylen)
{
	json_item *new_item = init_json_item();

	if (key != NULL) {
		new_item->key.val = xmalloc(sizeof(char) * (keylen + 1));
		memcpy(new_item->key.val, key, keylen + 1);
		new_item->key.len = keylen;
	}
	new_item->father = obj;
	new_item->type = JSON_T_NULL;
	
	if (obj->jchild.child == NULL) {
		obj->jchild.child = new_item;
	} else {
		obj->jchild.head->next = new_item;
	}
	
	obj->jchild.head = new_item;	
}


void json_set_property_strN(json_item *obj, const char *key, int keylen, const char *value, int valuelen)
{
	
	json_item *new_item = init_json_item();
	
	if (key != NULL) {
		new_item->key.val = xmalloc(sizeof(char) * (keylen + 1));
		memcpy(new_item->key.val, key, keylen + 1);
		new_item->key.len = keylen;
	}
	new_item->jval.vu.str.value = xmalloc(sizeof(char) * (valuelen + 1));
	memcpy(new_item->jval.vu.str.value, value, valuelen + 1);
	new_item->jval.vu.str.length = valuelen;
	new_item->type = JSON_T_STRING;
	
	new_item->father = obj;
	
	if (obj->jchild.child == NULL) {
		obj->jchild.child = new_item;
	} else {
		obj->jchild.head->next = new_item;
	}
	
	obj->jchild.head = new_item;
}

void json_set_property_strZ(json_item *obj, const char *key, const char *value)
{
	int len = (key != NULL ? strlen(key) : 0);
	
	json_set_property_strN(obj, key, len, value, strlen(value));
}

void json_set_element_strN(json_item *obj, const char *value, int valuelen)
{
	json_set_property_strN(obj, NULL, 0, value, valuelen);
}

void json_set_element_strZ(json_item *obj, const char *value)
{
	json_set_property_strZ(obj, NULL, value);
}

void json_set_element_obj(json_item *obj, json_item *value)
{
	json_set_property_objN(obj, NULL, 0, value);
}

void json_set_element_int(json_item *obj, long int value)
{
	json_set_property_intN(obj, NULL, 0, value);
}

void json_set_element_float(json_item *obj, long double value)
{
	json_set_property_floatN(obj, NULL, 0, value);
}

void json_set_element_boolean(json_item *obj, int value)
{
	json_set_property_boolean(obj, NULL, 0, value);
}

void json_set_element_null(json_item *obj)
{
	json_set_property_null(obj, NULL, 0);
}

void json_merge(json_item *obj_out, json_item *obj_in)
{
	
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
			if (cx->current_cx->father != NULL && !cx->start_depth) {
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

void json_aff(json_item *cx, int depth)
{
	while (cx != NULL) {
		if (cx->key.val != NULL) {
			printf("Key %s\n", cx->key.val);
		}
		if (cx->jval.vu.str.value != NULL) {
			printf("Value : %s\n", cx->jval.vu.str.value);
		}
		if (depth && cx->jchild.child != NULL) {
			json_aff(cx->jchild.child, depth - 1);
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

