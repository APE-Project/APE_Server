/*
  Copyright (C) 2006, 2007, 2008, 2009  Anthony Catel <a.catel@weelya.com>

  This file is part of ACE Server.
  ACE is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  ACE is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ACE ; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

/* proxy.c */

#include "main.h"
#include "utils.h"
#include "proxy.h"

ape_proxy *proxy_init(char *ident, char *host, int port)
{
	ape_proxy *proxy;
	
	if (strlen(ident) > 32) {
		return NULL;
	}
	
	proxy = xmalloc(sizeof(*proxy));
	
	memcpy(proxy->identifier, ident, strlen(ident)+1);
	
	proxy->sock.host = xstrdup(host);
	proxy->sock.port = port;
	proxy->sock.fd = -1;
	
	proxy->state = 0;
	
	proxy->to = NULL;
	proxy->next = NULL;
	
	return proxy;
}

void proxy_attach(ape_proxy *proxy, char *pipe, int allow_write)
{
//	transpipe *tpite;
	if (proxy == NULL) {
		return;
	}

}

