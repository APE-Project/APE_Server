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

#ifndef _LHTBL_H
#define _LHTBL_H

#define HACH_TABLE_MAX 5381

typedef struct HTBL
{
	struct _htbl_item *first;
	struct _htbl_item **table;
} HTBL;


typedef struct _htbl_item
{
	char *key;
	void *addrs;
	struct _htbl_item *next;
	
	struct _htbl_item *lnext;
	struct _htbl_item *lprev;
	
} HTBL_ITEM;

HTBL *hashtbl_init();

void hashtbl_free(HTBL *htbl);
void *hashtbl_seek(HTBL *htbl, const char *key);
void hashtbl_erase(HTBL *htbl, const char *key);
void hashtbl_append(HTBL *htbl, const char *key, void *structaddr);

#endif
