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

/* extend.h */

#ifndef _EXTEND_H
#define _EXTEND_H

#define EXTEND_KEY_LENGTH 32

typedef enum {
	EXTEND_STR,
	EXTEND_JSON,
	EXTEND_POINTER
} EXTEND_TYPE;

typedef enum {
	EXTEND_ISPUBLIC,
	EXTEND_ISPRIVATE
} EXTEND_PUBLIC;

typedef struct _extend extend;

struct _extend
{	
	void *val;
	
	EXTEND_TYPE type;
	EXTEND_PUBLIC visibility;
	
	struct _extend *next;
	char key[EXTEND_KEY_LENGTH+1];
};

extend *get_property(extend *entry, const char *key);
void clear_properties(extend **entry);
void del_property(extend **entry, const char *key);
//extend *add_property_str(extend **entry, char *key, char *val);
extend *add_property(extend **entry, const char *key, void *val, EXTEND_TYPE etype, EXTEND_PUBLIC visibility);
#endif
