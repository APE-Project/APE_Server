/*
  Copyright (C) 2006, 2007, 2008  Anthony Catel <a.catel@weelya.com>

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

void set_json(char *name, char *value, struct json **jprev)
{
	struct json *new_json, *old_json = *jprev;
	
	new_json = xmalloc(sizeof(struct json));
	
	new_json->name = xstrdup(name);
	new_json->value = (value != NULL ? xstrdup(value) : NULL);
	
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
	
	new_json->name = xstrdup(jbase->name);
	new_json->value = (jbase->value != NULL ? xstrdup(jbase->value) : NULL);
	
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
		string->jsize = strlen(jlist->name)+3 + 
				(jlist->jchilds == NULL ? (jlist->value != NULL ? strlen(jlist->value)+2 : 4) : 0) + 
				(jlist->prev == NULL ? 2 : 1) + (jlist->next == NULL && jlist->jchilds == NULL ? 1 : 0) + 2; /* Estimate init size ~ 2 bytes */
				
		string->jstring = xmalloc(sizeof(char) * string->jsize);
		
	} else {

		string_osize = string->jsize;
		string->jsize += strlen(jlist->name)+3 + 
				(jlist->jchilds == NULL ? (jlist->value != NULL ? strlen(jlist->value)+2 : 4) : 0) + 
				(jlist->prev == NULL ? 2 : 1) + (jlist->next == NULL && jlist->jchilds == NULL ? 1 : 0) + 2; /* Estimate new size ~ 2 bytes */
				
		string->jstring = xrealloc(string->jstring, sizeof(char) * string->jsize);
	}
	memset(string->jstring + string_osize, '\0', string->jsize - string_osize);
	
	pchild = jlist->jchilds;
	pjson = jlist->next;
	
	if (jlist->prev == NULL) {
		sprintf(string->jstring, "%s{", string->jstring);
	}
	sprintf(string->jstring, "%s\"%s\":", string->jstring, jlist->name);
	free(jlist->name);
	if (pchild == NULL) {
		if (jlist->value != NULL) {
			sprintf(string->jstring, "%s\"%s\"", string->jstring, jlist->value);
			free(jlist->value);
		} else {
			sprintf(string->jstring, "%snull", string->jstring);
		}
	} else if (pchild->type == JSON_ARRAY) {
		sprintf(string->jstring, "%s[", string->jstring);
	}
	while (pchild != NULL) {
		struct json_childs *pPchild = pchild;

		jsontr(pchild->child, string);
		
		pchild = pchild->next;
		
		if (pchild == NULL && pPchild->type == JSON_ARRAY) {
			sprintf(string->jstring, "%s]", string->jstring);
		} else if (pchild != NULL) {
			sprintf(string->jstring, "%s,", string->jstring);
		}
		free(pPchild);
	}
	if (jlist->next == NULL) {
		sprintf(string->jstring, "%s}", string->jstring);
	} else {
		sprintf(string->jstring, "%s,", string->jstring);
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

