/*
  Copyright (C) 2006, 2007, 2008, 2009, 2010, 2011  Anthony Catel <a.catel@weelya.com>

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
#include "sha1.h"
#include "base64.h"

/* Websocket GUID as defined by -07 (since -06) */
/* http://tools.ietf.org/html/draft-ietf-hybi-thewebsocketprotocol-07 */
#define WS_IETF_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

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
	cget.get    = websocket->data;
	cget.host   = websocket->http->host;
	cget.hlines = websocket->http->hlines;

	op = checkcmd(&cget, (websocket->version == WS_IETF_06 || 
	                    websocket->version == WS_IETF_07 ? 
	                            TRANSPORT_WEBSOCKET_IETF : TRANSPORT_WEBSOCKET), 
	              &user, g_ape);

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
static unsigned long int ws_compute_key_r76(const char *value)
{
	const char *pValue;
	unsigned long int val = 0;
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

/*
    WebSockets protocol rev ietf-hybi-07 (since -06)
    http://tools.ietf.org/html/draft-ietf-hybi-thewebsocketprotocol-07
*/
static char *ws_compute_key(const char *key, unsigned int key_len)
{
    unsigned char digest[20];
    char out[128];
    char *b64;
    
    if (key_len > 32) {
        return NULL;
    }
    
    memcpy(out, key, key_len);
    memcpy(out+key_len, WS_IETF_GUID, sizeof(WS_IETF_GUID)-1);
    
    sha1_csum((unsigned char *)out, (sizeof(WS_IETF_GUID)-1)+key_len, digest);
    
    b64 = base64_encode(digest, 20);
    
    return b64; /* must be released */
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
		ws_version version = WS_OLD;

		websocket_state *websocket;
		unsigned char md5sum[16];
		char *wsaccept = NULL;
				
		char *origin = get_header_line(http->hlines, "Origin");
		char *key1 = get_header_line(http->hlines, "Sec-WebSocket-Key1");
		char *key2 = get_header_line(http->hlines, "Sec-WebSocket-Key2");
		char *keybase = get_header_line(http->hlines, "Sec-WebSocket-Key");
		char *ws_version = get_header_line(http->hlines, "Sec-WebSocket-Version");
		char *ws_protocol = get_header_line(http->hlines, "Sec-WebSocket-Protocol");

		if (origin == NULL && (origin = get_header_line(http->hlines, "Sec-WebSocket-Origin")) == NULL) {
			shutdown(co->fd, 2);
			return NULL;
		}

		if (key1 != NULL && key2 != NULL) {
			md5_context ctx;
			
			unsigned long int ckey1 = htonl(ws_compute_key_r76(key1));
			unsigned long int ckey2 = htonl(ws_compute_key_r76(key2));
			
			version = WS_76;
			
			md5_starts(&ctx);
			
			md5_update(&ctx, (uint8 *)&ckey1, 4);
			md5_update(&ctx, (uint8 *)&ckey2, 4);
			md5_update(&ctx, (uint8 *)http->data, 8);
			
			md5_finish(&ctx, md5sum);
		} else if (keybase != NULL) {
		    if (ws_version != NULL) {
		        switch(atoi(ws_version)) {
		            case 6:
		            default:
		                version = WS_IETF_06;
		                break;
		            case 7:
		                version = WS_IETF_07;
		                break;
		        }
		    }
		    if ((wsaccept = ws_compute_key(keybase, strlen(keybase))) == NULL) {
	        	shutdown(co->fd, 2);
	            return NULL;		        
		    }
		} else if (origin != NULL) {
		    version = WS_OLD;
		} else {
	    	shutdown(co->fd, 2);
	        return NULL;
		}

		PACK_TCP(co->fd);
		
		switch(version) {
		    case WS_OLD:
			    sendbin(co->fd, CONST_STR_LEN(WEBSOCKET_HARDCODED_HEADERS_OLD), 0, g_ape);
			    sendbin(co->fd, CONST_STR_LEN("WebSocket-Origin: "), 0, g_ape);
			    sendbin(co->fd, origin, strlen(origin), 0, g_ape);
			    sendbin(co->fd, CONST_STR_LEN("\r\nWebSocket-Location: ws://"), 0, g_ape);
			    sendbin(co->fd, http->host, strlen(http->host), 0, g_ape);
		        sendbin(co->fd, http->uri, strlen(http->uri), 0, g_ape);
			    break;
			case WS_76:
			    sendbin(co->fd, CONST_STR_LEN(WEBSOCKET_HARDCODED_HEADERS_NEW), 0, g_ape);
			    sendbin(co->fd, CONST_STR_LEN("Sec-WebSocket-Origin: "), 0, g_ape);
			    sendbin(co->fd, origin, strlen(origin), 0, g_ape);
			    sendbin(co->fd, CONST_STR_LEN("\r\nSec-WebSocket-Location: ws://"), 0, g_ape);
		        sendbin(co->fd, http->host, strlen(http->host), 0, g_ape);
		        sendbin(co->fd, http->uri, strlen(http->uri), 0, g_ape);
			    break;
		    case WS_IETF_06:
		    case WS_IETF_07:
			    sendbin(co->fd, CONST_STR_LEN(WEBSOCKET_HARDCODED_HEADERS_IETF), 0, g_ape);
                sendbin(co->fd, CONST_STR_LEN("Sec-WebSocket-Accept: "), 0, g_ape);
                sendbin(co->fd, wsaccept, strlen(wsaccept), 0, g_ape);
                if (ws_protocol != NULL) {
                    sendbin(co->fd, CONST_STR_LEN("\r\nSec-WebSocket-Protocol: "), 0, g_ape);
                    sendbin(co->fd, ws_protocol, strlen(ws_protocol), 0, g_ape);
                }
                free(wsaccept);
		        break;
		}

		sendbin(co->fd, CONST_STR_LEN("\r\n\r\n"), 0, g_ape);
		if (version == WS_76) {
			sendbin(co->fd, (char *)md5sum, 16, 0, g_ape);
		}
		FLUSH_TCP(co->fd);
		
		co->parser = parser_init_stream(co);
		websocket = co->parser.data;
		websocket->http = http; /* keep http data */
		websocket->version = version;
		switch(version) {
		    case WS_IETF_06:
		        websocket->step = WS_STEP_KEY;
		        break;
		    case WS_IETF_07:
		        websocket->step = WS_STEP_START;
		        break;
		    default:
		        break;		    
		}
		
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
	cget.get    = http->data;
	cget.host   = http->host;
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

