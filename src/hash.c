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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "hash.h"
#include "users.h"
#include "utils.h"

static unsigned int hach_string(const char *str)
{
        int hash = 5381; // DJB Hash
        const char *s;
	
	
        for (s = str; *s != '\0'; s++) {
                hash = ((hash << 5) + hash) + tolower(*s);
        }
	
        return (hash & 0x7FFFFFFF)%(HACH_TABLE_MAX-1);
}

HTBL **hashtbl_init()
{
	HTBL **htbl;
	
	htbl = (HTBL **) xmalloc( sizeof(*htbl) * (HACH_TABLE_MAX + 1) );

	
	memset(htbl, 0, sizeof(*htbl) * (HACH_TABLE_MAX + 1));
	
	return htbl;
}
void hashtbl_free(HTBL **htbl)
{
	size_t i;
	HTBL *hTmp;
	HTBL *hNext;
	
	for (i = 0; i < (HACH_TABLE_MAX + 1); i++) {
		hTmp = htbl[i];
		while (hTmp != 0) {
			hNext = hTmp->next;
			free(hTmp->key);
			hTmp->key = NULL;
			free(hTmp);
			hTmp = hNext;
		}
	}
	free(htbl);	
}

void hashtbl_append(HTBL **htbl, char *key, void *structaddr)
{
	unsigned int key_hash;
	HTBL *hTmp, *hDbl;
	
	if (key == NULL) {
		return;
	}
	
	key_hash = hach_string(key);
	
	hTmp = (HTBL *)xmalloc(sizeof(*hTmp));

	
	hTmp->next = NULL;
	hTmp->key = xmalloc(sizeof(char) * (strlen(key)+1));

	
	hTmp->addrs = (void *)structaddr;
	
	memcpy(hTmp->key, key, strlen(key)+1);
	
	if (htbl[key_hash] != NULL) {
		hDbl = htbl[key_hash];
		
		while (hDbl != NULL) {
			if (strcasecmp(hDbl->key, key) == 0) {
				free(hTmp->key);
				free(hTmp);
				hDbl->addrs = (void *)structaddr; // Si c'est la même clef on remplace juste l'adresse de la structure pointé.
				return;
			} else {
				hDbl = hDbl->next;
			}
		}
		hTmp->next = htbl[key_hash];
	}
	
	htbl[key_hash] = hTmp;
}


void hashtbl_erase(HTBL **htbl, char *key)
{
	unsigned int key_hash;
	HTBL *hTmp, *hPrev;
	
	if (key == NULL) {
		return;
	}
	
	key_hash = hach_string(key);
	
	hTmp = htbl[key_hash];
	hPrev = NULL;
	
	while (hTmp != NULL) {
		if (strcasecmp(hTmp->key, key) == 0) {
			if (hPrev != NULL) {
				hPrev->next = hTmp->next;
			} else {
				htbl[key_hash] = hTmp->next;
			}
			free(hTmp->key);
			free(hTmp);
			return;
		}
		hPrev = hTmp;
		hTmp = hTmp->next;
	}
}

void * hashtbl_seek(HTBL **htbl, char *key)
{
	unsigned int key_hash;
	HTBL *hTmp;
	
	if (key == NULL) {
		return NULL;
	}
	
	key_hash = hach_string(key);
	
	hTmp = htbl[key_hash];
	
	while (hTmp != NULL) {
		if (strcasecmp(hTmp->key, key) == 0) {
			return (void *)(hTmp->addrs);
		}
		hTmp = hTmp->next;
	}
	
	return NULL;
}

