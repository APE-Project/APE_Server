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
};

typedef struct _http_state http_state;
struct _http_state
{
	int step;
	int type; /* HTTP_GET or HTTP_POST */
	int pos;
	int contentlength;
	int read;
	int error;
	int ready;
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
	unsigned int size;
	unsigned int length;
	
	void *slot;
	int islot;
};



typedef struct _acetables
{
	HTBL *hLogin;
	HTBL *hSessid;
	HTBL *hLusers;
	HTBL *hCallback;

	HTBL *hPubid;

	struct apeconfig *srv;
	
	struct _callback_hook *cmd_hook;
	
	struct _ape_transports transports;
	
	struct USERS *uHead;
	
	struct {
		struct _ticks_callback *timers;
		unsigned int ntimers;
	} timers;
	
	struct _socks_bufout *bufout;

	struct _ace_plugins *plugins;
	
	struct _fdevent *events;
	
	int basemem;
	unsigned int nConnected;
	
	struct _ape_socket *co;
	
	struct {
		struct _ape_proxy *list;
		struct _ape_proxy_cache *hosts;
	} proxy;
	
	struct _extend *properties;
} acetables;

typedef struct _ape_socket ape_socket;
struct _ape_socket {
	char ip_client[16];

	http_state http;

	int fd;
	ape_socket_state_t state;
	
	ape_socket_t stream_type;
	
	long int idle;

	ape_buffer buffer_in;
	ape_buffer buffer_out;

	struct {
		void (*on_accept)(struct _ape_socket *client, acetables *g_ape);
		void (*on_connect)(struct _ape_socket *client, acetables *g_ape); /* ajouter in/out ? ou faire un onaccept */
		void (*on_disconnect)(struct _ape_socket *client, acetables *g_ape);
		void (*on_read)(struct _ape_socket *client, struct _ape_buffer *buf, size_t offset, acetables *g_ape);
		void (*on_read_lf)(struct _ape_socket *client, char *data, acetables *g_ape);
		void (*on_data_completly_sent)(struct _ape_socket *client, acetables *g_ape);
		void (*on_write)(struct _ape_socket *client, acetables *g_ape);
	} callbacks;

	void *attach;
	
	void *data;
};

#define HEADER "HTTP/1.1 200 OK\r\nPragma: no-cache\r\nCache-Control: no-cache, must-revalidate\r\nExpires: Thu, 27 Dec 1986 07:30:00 GMT\r\nContent-Type: text/html\r\n\r\n"
#define HEADER_LEN 144

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


