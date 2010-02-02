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

/* extend.c */

#include <string.h>
#include <stdio.h>
#include "extend.h"
#include "utils.h"
#include "json.h"

/*
	Add a property to an object (user, channel, proxy, acetables)
	
	
	EXTEND_STR : allocate memory for val and free it when the property is deleted
	EXTEND_JSON : put the given "json" object on the properties. This object is free'ed using json_free when the property is deleted
	EXTEND_POINTER : add a private pointer as property (must be private. see EXTEND_PUBLIC)
	
	EXTEND_ISPUBLIC : The property is added to the json tree sent with get_json_object_*
	EXTEND_ISPRIVATE : The property is not shown in get_json_object_*
*/
extend *add_property(extend **entry, const char *key, void *val, EXTEND_TYPE etype, EXTEND_PUBLIC visibility)
{
	extend *new_property = NULL, *eTmp;
	
	if (strlen(key) > EXTEND_KEY_LENGTH) {
		return NULL;
	}
	
	/* Delete older property with this key (if any) */
	del_property(entry, key);

	eTmp = *entry;
	
	new_property = xmalloc(sizeof(*new_property));
	
	/* The key cannot be longer than EXTEND_KEY_LENGTH */
	strcpy(new_property->key, key);
	
	switch(etype) {
		case EXTEND_STR:
			new_property->val = xstrdup(val);	
			break;
		case EXTEND_POINTER:
		default:
			/* a pointer must be a private property */
			visibility = EXTEND_ISPRIVATE;
		case EXTEND_JSON:
			/* /!\ the JSON tree (val) is free'ed when this property is removed */
			new_property->val = val;		
			break;		
	}

	new_property->next = eTmp;
	new_property->type = etype;
	new_property->visibility = visibility;
	
	*entry = new_property;
	
	return new_property;	

}


extend *get_property(extend *entry, const char *key)
{
	while (entry != NULL) {
		if (strcmp(entry->key, key) == 0) {
			return entry;
		}
		entry = entry->next;
	}
	
	return NULL;	
}

void *get_property_val(extend *entry, const char *key)
{
	extend *find;
	
	if ((find = get_property(entry, key)) != NULL) {
		return find->val;
	}
	return NULL;
	
}

void del_property(extend **entry, const char *key)
{

	while (*entry != NULL) {
		if (strcmp((*entry)->key, key) == 0) {
			extend *pEntry = *entry;
			*entry = (*entry)->next;
			
			switch(pEntry->type) {
				case EXTEND_STR:
					free(pEntry->val);
					break;
				case EXTEND_JSON:
					free_json_item(pEntry->val);
					break;
				default:
					break;
			}
			
			free(pEntry);
			
			return;
		}
		entry = &(*entry)->next;
	}

}

/* TODO : use del_property */
void clear_properties(extend **entry)
{
	extend *pEntry = *entry, *pTmp;

	while (pEntry != NULL) {
		pTmp = pEntry->next;
		switch(pEntry->type) {
			case EXTEND_STR:
				free(pEntry->val);
				break;
			case EXTEND_JSON:
				free_json_item(pEntry->val);
				break;
			default:
				break;
		}
		free(pEntry);
		pEntry = pTmp;
	}
	*entry = NULL;
}


#if 0
extend *add_property_str(extend **entry, char *key, char *val)
{
	extend *new_property = NULL, *eTmp;
	
	if (strlen(key) > EXTEND_KEY_LENGTH) {
		return NULL;
	}
	if ((eTmp = get_property(*entry, key)) != NULL) {
		if (strlen(val) > strlen(eTmp->val)) {
			eTmp->val = xrealloc(eTmp->val, sizeof(char) * (strlen(val)+1));
		}

		strcpy(eTmp->key, key);
		strcpy(eTmp->val, val);
		
		return eTmp;
	}
	
	eTmp = *entry;
	
	new_property = xmalloc(sizeof(*new_property));
	new_property->val = xmalloc(sizeof(char) * (strlen(val)+1));
	new_property->allocval = 1;
	strcpy(new_property->key, key);
	strcpy(new_property->val, val);
	new_property->next = eTmp;
	
	*entry = new_property;
	
	return new_property;
	
}

extend *add_property(extend **entry, const char *key, void *val)
{
	extend *new_property = NULL, *eTmp;
	
	if (strlen(key) > EXTEND_KEY_LENGTH || (eTmp = get_property(*entry, key)) != NULL) {
		return NULL;
	}

	eTmp = *entry;
	
	new_property = xmalloc(sizeof(*new_property));
	
	strcpy(new_property->key, key);
	new_property->val = val;
	new_property->allocval = 0;
	new_property->next = eTmp;
	
	*entry = new_property;
	
	return new_property;
	
}
#endif

