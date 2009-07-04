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

/* json.h */

#ifndef _JSON_H
#define _JSON_H


#include "json_parser.h"

enum {
	JSON_ARRAY = 0,
	JSON_OBJECT
};

typedef struct json {
	char *name;
	char *value;
	
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
	int jsize;
};

typedef struct _json_item {
        char *key;
	
	int type;
	
        struct JSON_value_struct jval;
	
	
        struct _json_item *father;
        struct _json_item *child;

        struct _json_item *next;
	

} json_item;

enum {
	JSON_ITEM_OBJ,
	JSON_ITEM_VAL
};

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
struct jsontring *jsontr(struct json *jlist, struct jsontring *string);
json_item *init_json_parser(const char *json_string);

#endif

