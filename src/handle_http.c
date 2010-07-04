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

/* handle_http.c */

#include <string.h>

#include "handle_http.h"
#include "utils.h"
#include "config.h"
#include "cmd.h"
#include "main.h"
#include "sock.h"
#include "http.h"
#include "parser.h"
#include "md5.h"

static int gettransport(char *input)
{
	char *start = strchr(input, '/');

	if (start != NULL && start[1] >= 48 && start[1] <= 54 && start[2] == '/') {
		return start[1]-48;
	}
	
	return 0;
}

subuser *checkrecv_websocket(ape_socket *co, acetables *g_ape)
{
	unsigned int op;
	clientget cget;
	websocket_state *websocket = co->parser.data;
	subuser *user = NULL;
	
	cget.client = co;
	cget.ip_get = co->ip_client;
	cget.get = websocket->data;
	cget.host = websocket->http->host;
	cget.hlines = websocket->http->hlines;

	op = checkcmd(&cget, TRANSPORT_WEBSOCKET, &user, g_ape);

	switch (op) {
		case CONNECT_SHUTDOWN:
		case CONNECT_KEEPALIVE:
			break;
	}	
	
	return user;
}

/* 
	WebSockets protocol rev 76 (Opening handshake)
	http://tools.ietf.org/html/draft-hixie-thewebsocketprotocol-76 
*/
static unsigned int ws_compute_key(const char *value)
{
	const char *pValue;
	long int val = 0;
	int spaces = 0;
	
	for (pValue = value; *pValue != '\0'; pValue++) {
		if (*pValue >= 48 && *pValue <= 57) {
			val = (val * 10) + (*pValue-48);
		} else if (*pValue == ' ') {
			spaces++;
		}
	}
	if (spaces == 0) {
		return 0;
	}

	return val / spaces;
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
	
	if (gettransport(http->uri) == TRANSPORT_WEBSOCKET) {
		int is_rev_76 = 0;
		char *origin = get_header_line(http->hlines, "Origin");
		char *key1 = get_header_line(http->hlines, "Sec-WebSocket-Key1");
		char *key2 = get_header_line(http->hlines, "Sec-WebSocket-Key2");
		
		websocket_state *websocket;
		unsigned char md5sum[16];
		
		if (origin == NULL) {
			shutdown(co->fd, 2);
			return NULL;
		}

		if (key1 != NULL && key2 != NULL) {
			md5_context ctx;
			
			long int ckey1 = htonl(ws_compute_key(key1));
			long int ckey2 = htonl(ws_compute_key(key2));
			
			is_rev_76 = 1; /* draft rev 76 detected (used in Firefox 4.0 alpha2) */
			
			md5_starts(&ctx);
			
			md5_update(&ctx, (uint8 *)&ckey1, 4);
			md5_update(&ctx, (uint8 *)&ckey2, 4);
			md5_update(&ctx, (uint8 *)http->data, 8);
			
			md5_finish(&ctx, md5sum);
		}

		PACK_TCP(co->fd);
		
		if (is_rev_76) {
			sendbin(co->fd, CONST_STR_LEN(WEBSOCKET_HARDCODED_HEADERS_NEW), 0, g_ape);
			sendbin(co->fd, CONST_STR_LEN("Sec-WebSocket-Origin: "), 0, g_ape);
			sendbin(co->fd, origin, strlen(origin), 0, g_ape);
			sendbin(co->fd, CONST_STR_LEN("\r\nSec-WebSocket-Location: ws://"), 0, g_ape);			
		} else {
			sendbin(co->fd, CONST_STR_LEN(WEBSOCKET_HARDCODED_HEADERS_OLD), 0, g_ape);
			sendbin(co->fd, CONST_STR_LEN("WebSocket-Origin: "), 0, g_ape);
			sendbin(co->fd, origin, strlen(origin), 0, g_ape);
			sendbin(co->fd, CONST_STR_LEN("\r\nWebSocket-Location: ws://"), 0, g_ape);			
		}
		sendbin(co->fd, http->host, strlen(http->host), 0, g_ape);
		sendbin(co->fd, http->uri, strlen(http->uri), 0, g_ape);
		sendbin(co->fd, CONST_STR_LEN("\r\n\r\n"), 0, g_ape);
		if (is_rev_76) {
			sendbin(co->fd, (char *)md5sum, 16, 0, g_ape);
		}
		FLUSH_TCP(co->fd);
		
		
		co->parser = parser_init_stream(co);
		websocket = co->parser.data;
		websocket->http = http; /* keep http data */
		
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
	cget.hlines = http->hlines;
	
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

