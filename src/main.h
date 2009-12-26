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

/* main.h */


#ifndef _MAIN_H
#define _MAIN_H


#include <stdarg.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

#include "hash.h"

#define MAX_IO 4096
#define DEFAULT_BUFFER_SIZE 2048

#define MAX_NICK_LEN 	16 // move to module
#define MAX_CHAN_LEN 	40
#define MAX_MSG_LEN 	1024
#define MAX_RAW_LEN 	1024

#define TIMEOUT_SEC 45

#define SERVER_NAME "APE.Server"
#define _VERSION "1.01dev"

int server_is_running;

struct _transport_properties {
	struct {
		struct {
			char *val;
			int len;
		} left;
		struct {
			char *val;
			int len;
		} right;
	} padding;
};

struct _ape_transports {
	struct {
		struct _transport_properties properties;
	} jsonp;
	
	struct {
		struct _transport_properties properties;
	} xhrstreaming;
	
	struct {
		struct _transport_properties properties;
	} sse;
	
	struct {
		struct _transport_properties properties;
	} websocket;
};

typedef struct _http_state http_state;
struct _http_state
{
	struct _http_header_line *hlines;
	
	char *uri;
	const char *data;
	const char *host;
	
	int pos;
	int contentlength;
	int read;
	
	unsigned short int step;
	unsigned short int type; /* HTTP_GET or HTTP_POST */
	unsigned short int error;
};

typedef struct _websocket_state websocket_state;
struct _websocket_state
{
	struct _http_state *http;
	const char *data;
	unsigned short int offset;
	unsigned short int error;
};

typedef enum {
	STREAM_IN,
	STREAM_OUT,
	STREAM_SERVER,
	STREAM_DELEGATE
} ape_socket_t;

typedef enum {
	STREAM_ONLINE,
	STREAM_PROGRESS
} ape_socket_state_t;

typedef struct _ape_buffer ape_buffer;
struct _ape_buffer {
	char *data;
	void *slot;
	
	unsigned int size;
	unsigned int length;

	int islot;
};

typedef struct _acetables
{
	struct {
		struct _callback_hook *head;
		struct _callback_hook *foot;
	} cmd_hook;
	
	struct {
		struct _ape_proxy *list;
		struct _ape_proxy_cache *hosts;
	} proxy;
		
	struct {
		struct _ticks_callback *timers;
		unsigned int ntimers;
	} timers;
	
	struct {
		unsigned int lvl;
		unsigned int use_syslog;
		int fd;
	} logs;
	
	struct _ape_transports transports;
	
	HTBL *hLogin;
	HTBL *hSessid;
	HTBL *hLusers;
	HTBL *hCallback;
	HTBL *hPubid;

	struct apeconfig *srv;
	struct _callback_hook *bad_cmd_callbacks;	
	struct USERS *uHead;
	struct _socks_bufout *bufout;
	struct _ace_plugins *plugins;
	struct _fdevent *events;
	struct _ape_socket *co;
	struct _extend *properties;
	
	const char *confs_path;
	
	int is_daemon;
	int basemem;
	unsigned int nConnected;
} acetables;


typedef struct _ape_parser ape_parser;
struct _ape_parser {
	void (*parser_func)(struct _ape_socket *, acetables *);
	void (*destroy)(struct _ape_parser *);
	void (*onready)(struct _ape_parser *, acetables *);
	void *data;
	struct _ape_socket *socket;
	short int ready;
};

typedef struct _ape_socket ape_socket;
struct _ape_socket {
	struct {
		void (*on_accept)(struct _ape_socket *client, acetables *g_ape);
		void (*on_connect)(struct _ape_socket *client, acetables *g_ape); /* ajouter in/out ? ou faire un onaccept */
		void (*on_disconnect)(struct _ape_socket *client, acetables *g_ape);
		void (*on_read)(struct _ape_socket *client, struct _ape_buffer *buf, size_t offset, acetables *g_ape);
		void (*on_read_lf)(struct _ape_socket *client, char *data, acetables *g_ape);
		void (*on_data_completly_sent)(struct _ape_socket *client, acetables *g_ape);
		void (*on_write)(struct _ape_socket *client, acetables *g_ape);
	} callbacks;

	ape_parser parser;

	ape_buffer buffer_in;
	ape_buffer buffer_out;

	char ip_client[16];
	long int idle;

	void *attach;
	void *data;
	
	int fd;
	int burn_after_writing;
	
	ape_socket_state_t state;
	ape_socket_t stream_type;
};

#define HEADER_DEFAULT "HTTP/1.1 200 OK\r\nPragma: no-cache\r\nCache-Control: no-cache, must-revalidate\r\nExpires: Thu, 27 Dec 1986 07:30:00 GMT\r\nContent-Type: text/html\r\n\r\n"
#define HEADER_DEFAULT_LEN 144

#define HEADER_SSE "HTTP/1.1 200 OK\r\nPragma: no-cache\r\nCache-Control: no-cache, must-revalidate\r\nExpires: Thu, 27 Dec 1986 07:30:00 GMT\r\nContent-Type: application/x-dom-event-stream\r\n\r\n"
#define HEADER_SSE_LEN 165

#define HEADER_XHR "HTTP/1.1 200 OK\r\nPragma: no-cache\r\nCache-Control: no-cache, must-revalidate\r\nExpires: Thu, 27 Dec 1986 07:30:00 GMT\r\nContent-Type: application/x-ape-event-stream\r\n\r\n                                                                                                                                                                                                                                                                "
#define HEADER_XHR_LEN 421

#define CONTENT_NOTFOUND "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\"><html><head><title>APE Server</title></head><body><h1>APE Server</h1><p>No command given.</p><hr><address>http://www.ape-project.org/ - Server "_VERSION" (Build "__DATE__" "__TIME__")</address></body></html>"

/* http://tools.ietf.org/html/draft-hixie-thewebsocketprotocol-55 : The first three lines in each case are hard-coded (the exact case and order matters); */
#define WEBSOCKET_HARDCODED_HEADERS "HTTP/1.1 101 Web Socket Protocol Handshake\r\nUpgrade: WebSocket\r\nConnection: Upgrade\r\n"

#define FIRE_EVENT(event, ret, arg...) \
	if (g_ape->plugins != NULL) { \
		ace_plugins *cplug = g_ape->plugins; \
		while (cplug != NULL) { \
			if (cplug->cb != NULL && cplug->cb->c_##event != NULL && cplug->fire.c_##event == 0) { \
				cplug->fire.c_##event = 1; \
				ret = cplug->cb->c_##event(arg); \
				cplug->fire.c_##event = 0; \
				\
				if (ret == NULL) { \
					return NULL; \
				} \
				\
				break; \
			} \
			cplug = cplug->next; \
		} \
	} \
	if (ret != NULL) { \
		return ret; \
	}
 
#define FIRE_EVENT_NULL(event, arg...) \
	if (g_ape->plugins != NULL) { \
		ace_plugins *cplug = g_ape->plugins; \
		while (cplug != NULL) { \
			if (cplug->cb != NULL && cplug->cb->c_##event != NULL && cplug->fire.c_##event == 0) { \
				cplug->fire.c_##event = 1; \
				cplug->cb->c_##event(arg); \
				cplug->fire.c_##event = 0; \
				return; \
				break; \
			} \
			cplug = cplug->next; \
		} \
	}

#define FIRE_EVENT_NONSTOP(event, arg...) \
	if (g_ape->plugins != NULL) { \
		ace_plugins *cplug = g_ape->plugins; \
		while (cplug != NULL) { \
			if (cplug->cb != NULL && cplug->cb->c_##event != NULL && cplug->fire.c_##event == 0) { \
				cplug->fire.c_##event = 1; \
				cplug->cb->c_##event(arg); \
				cplug->fire.c_##event = 0; \
			} \
			cplug = cplug->next; \
		} \
	}
#endif


