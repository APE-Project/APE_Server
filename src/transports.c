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

/* transports.c */

#include "transports.h"
#include "config.h"
#include "utils.h"

struct _transport_open_same_host_p transport_open_same_host(subuser *sub, ape_socket *client, transport_t transport)
{
	struct _transport_open_same_host_p ret;
	
	switch(transport) {
		case TRANSPORT_LONGPOLLING:
		case TRANSPORT_JSONP:
		case TRANSPORT_WEBSOCKET:
		default:
			ret.client_close = sub->client;
			ret.client_listener = client;
			ret.substate = ADIED;
			ret.attach = 1;
			break;
		case TRANSPORT_PERSISTANT:
		case TRANSPORT_XHRSTREAMING:
		case TRANSPORT_SSE_LONGPOLLING:
			ret.client_close = client;
			ret.client_listener = sub->client;
			ret.substate = ALIVE;
			ret.attach = 0;
			break;
	}
	
	return ret;
}

void transport_data_completly_sent(subuser *sub, transport_t transport)
{
	switch(transport) {
		case TRANSPORT_LONGPOLLING:
		case TRANSPORT_JSONP:
		default:
			do_died(sub);
			break;
		case TRANSPORT_PERSISTANT:
		case TRANSPORT_XHRSTREAMING:
		case TRANSPORT_SSE_LONGPOLLING:
		case TRANSPORT_WEBSOCKET:
			break;
	}	
}

struct _transport_properties *transport_get_properties(transport_t transport, acetables *g_ape)
{
	switch(transport) {
		case TRANSPORT_LONGPOLLING:
		case TRANSPORT_PERSISTANT:
		default:
			break;
		case TRANSPORT_XHRSTREAMING:
			return &(g_ape->transports.xhrstreaming.properties);
			break;
		case TRANSPORT_JSONP:
			return &(g_ape->transports.jsonp.properties);
			break;
		case TRANSPORT_SSE_LONGPOLLING:
			return &(g_ape->transports.sse.properties);
			break;
		case TRANSPORT_WEBSOCKET:
			return &(g_ape->transports.websocket.properties);
			break;
	}	
	return NULL;
}

void transport_start(acetables *g_ape)
{
	char *eval_func = CONFIG_VAL(JSONP, eval_func, g_ape->srv);
	int len = strlen(eval_func);
	
	if (len) {
		g_ape->transports.jsonp.properties.padding.left.val = xmalloc(sizeof(char) * (len + 3));
		strcpy(g_ape->transports.jsonp.properties.padding.left.val, eval_func);
		strcat(g_ape->transports.jsonp.properties.padding.left.val, "('");
		g_ape->transports.jsonp.properties.padding.left.len = len+2;
		
		g_ape->transports.jsonp.properties.padding.right.val = xstrdup("')");
		g_ape->transports.jsonp.properties.padding.right.len = 2;
		
	} else {
		g_ape->transports.jsonp.properties.padding.left.val = NULL;
	}

	g_ape->transports.xhrstreaming.properties.padding.left.val = NULL;
	g_ape->transports.xhrstreaming.properties.padding.left.len = 0;

	g_ape->transports.xhrstreaming.properties.padding.right.val = xstrdup("\n\n");
	g_ape->transports.xhrstreaming.properties.padding.right.len = 2;
	
	g_ape->transports.sse.properties.padding.left.val = xstrdup("Event: ape-event\nData: ");
	g_ape->transports.sse.properties.padding.left.len = 24;

	g_ape->transports.sse.properties.padding.right.val = xstrdup("\n\n");
	g_ape->transports.sse.properties.padding.right.len = 2;
	
	g_ape->transports.websocket.properties.padding.left.val = xstrdup("\x00");
	g_ape->transports.websocket.properties.padding.left.len = 1;
	
	g_ape->transports.websocket.properties.padding.right.val = xstrdup("\xFF");
	g_ape->transports.websocket.properties.padding.right.len = 1;	
	
}

void transport_free(acetables *g_ape)
{
	free(g_ape->transports.websocket.properties.padding.right.val);
	free(g_ape->transports.websocket.properties.padding.left.val);
	free(g_ape->transports.sse.properties.padding.right.val);
	free(g_ape->transports.sse.properties.padding.left.val);
	free(g_ape->transports.xhrstreaming.properties.padding.right.val);

	if (g_ape->transports.jsonp.properties.padding.left.val != NULL) {
		free(g_ape->transports.jsonp.properties.padding.left.val);
		free(g_ape->transports.jsonp.properties.padding.right.val);
	}
}
