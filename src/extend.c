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

/* extend.c */

#include "extend.h"
#include "main.h"
#include "utils.h"
#include "json.h"

extend *get_property(extend *current, char *key)
{
	while (current != NULL) {
		if (strcmp(current->key, key) == 0) {
			return current;
		}
		current = current->next;
	}
	
	return NULL;
	
}
void clear_properties(extend **entry)
{
	extend *pExtend = *entry, *pTmp;

	while (pExtend != NULL) {
		pTmp = pExtend->next;
		if (pExtend->allocval == 1) {
			free(pExtend->val);
		}
		free(pExtend);
		pExtend = pTmp;
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

extend *add_property(extend **entry, char *key, void *val)
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

extend *add_property(extend **entry, char *key, void *val, EXTEND_TYPE etype, EXTEND_PUBLIC visibility)
{
	extend *new_property = NULL, *eTmp;
	
	if (strlen(key) > EXTEND_KEY_LENGTH || (eTmp = get_property(*entry, key)) != NULL) {
		return NULL;
	}
	
	eTmp = *entry;
	
	new_property = xmalloc(sizeof(*new_property));
	
	strcpy(new_property->key, key);
	
	switch(etype) {
		case EXTEND_STR:
			new_property->val = xmalloc(sizeof(char) * (strlen(val)+1));
			strcpy(new_property->val, val);		
			break;
		case EXTEND_POINTER:
		default:
			visibility = EXTEND_ISPRIVATE;
		case EXTEND_JSON:
			new_property->val = val;
			new_property->allocval = 0;			
			break;		
	}

	new_property->next = eTmp;
	new_property->type = etype;
	new_property->visibility = visibility;
	
	*entry = new_property;
	
	return new_property;	

}

