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

/* http.h */

#ifndef _DNS_H
#define _DNS_H

#include "main.h"
#include "sock.h"
#include <udns.h>

struct query {
	char *name;		/* original query string */
	unsigned char *dn;		/* the DN being looked up */
	void (*callback)(char *ip, void *data, acetables *g_ape);
	void *data;
	acetables *g_ape;
	enum dns_type qtyp;		/* type of the query */
};

void ape_dns_init();
void ape_gethostbyname(char *name, void (*callback)(char *, void *, acetables *), void *data, acetables *g_ape);

#endif

