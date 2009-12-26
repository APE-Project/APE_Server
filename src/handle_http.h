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

/* handle_http.h */

#ifndef _HANDLE_HTTP_H
#define _HANDLE_HTTP_H

#include "main.h"
#include "users.h"

subuser *checkrecv(ape_socket *co, acetables *g_ape);
subuser *checkrecv_websocket(ape_socket *co, acetables *g_ape);

typedef struct clientget
{
	struct _http_header_line *hlines;
	ape_socket *client;
	const char *ip_get;
	const char *get;
	const char *host;
} clientget ;

enum {
	CONNECT_SHUTDOWN = 0,
	CONNECT_KEEPALIVE
};

#endif

