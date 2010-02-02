/*
  Copyright (C) 2006, 2007, 2008, 2009, 2010 Anthony Catel <a.catel@weelya.com>

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

/* transports.h */

#ifndef _TRANSPORTS_H
#define _TRANSPORTS_H

#include "main.h"
#include "users.h"

struct _transport_open_same_host_p
{
	ape_socket *client_close;
	ape_socket *client_listener;
	int attach;
	int substate;
};

typedef enum {
	TRANSPORT_LONGPOLLING,
	TRANSPORT_XHRSTREAMING,
	TRANSPORT_JSONP,
	TRANSPORT_PERSISTANT,
	TRANSPORT_SSE_LONGPOLLING,
	TRANSPORT_SSE_JSONP,
	TRANSPORT_WEBSOCKET
} transport_t;


struct _transport_open_same_host_p transport_open_same_host(subuser *sub, ape_socket *client, transport_t transport);
void transport_data_completly_sent(subuser *sub, transport_t transport);
void transport_start(acetables *g_ape);
struct _transport_properties *transport_get_properties(transport_t transport, acetables *g_ape);

#endif
