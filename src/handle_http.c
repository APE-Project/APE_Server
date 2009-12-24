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

/* handle_http.c */

#include <string.h>

#include "handle_http.h"
#include "utils.h"
#include "config.h"
#include "cmd.h"
#include "main.h"
#include "sock.h"

static int gettransport(char *input)
{
	char *start = strchr(input, '/');

	if (start != NULL && start[1] >= 48 && start[1] <= 54 && start[2] == '/') {
		return start[1]-48;
	}
	
	return 0;
}

subuser *checkrecv(ape_socket *co, acetables *g_ape)
{
	unsigned int op;
	http_state *http = co->parser.data;
	subuser *user = NULL;
	clientget cget;
	
	if (http->host == NULL) {
		shutdown(co->fd, 2);
		return NULL;
	}
	
	if (http->data == NULL) {
		sendbin(co->fd, HEADER_DEFAULT, HEADER_DEFAULT_LEN, 0, g_ape);
		sendbin(co->fd, CONST_STR_LEN(CONTENT_NOTFOUND), 0, g_ape);
		
		safe_shutdown(co->fd, g_ape);
		return NULL;
	}
	
	cget.client = co;
	cget.ip_get = co->ip_client;
	cget.get = http->data;
	cget.host = http->host;
	
	op = checkcmd(&cget, gettransport(http->uri), &user, g_ape);

	switch (op) {
		case CONNECT_SHUTDOWN:
			safe_shutdown(co->fd, g_ape);			
			break;
		case CONNECT_KEEPALIVE:
			break;
	}
	
	return user;
}

