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
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

#include "hash.h"


#define MAX_IO 4096
#define DEFAULT_BUFFER_SIZE 2048

#define MAX_NICK_LEN 	16 // move to module
#define MAX_CHAN_LEN 	16
#define MAX_MSG_LEN 	1024
#define MAX_RAW_LEN 	1024

#define TIMEOUT_SEC 45

#define SERVER_NAME "APE.Server"


typedef struct _acetables
{
	HTBL **hLogin;
	HTBL **hSessid;
	HTBL **hLusers;
	HTBL **hCallback;

	HTBL **hPubid;

	struct apeconfig *srv;
	struct USERS *uHead;
	
	struct _ticks_callback *timers;
	
	unsigned int nConnected;

	struct _ace_plugins *plugins;
	
	int *epoll_fd;
	
	struct {
		struct _ape_proxy *list;
		struct _ape_proxy_cache *hosts;
	} proxy;
	struct _extend *properties;
} acetables;

#define HEADER "HTTP/1.1 200 OK\r\nPragma: no-cache\r\nCache-Control: no-cache, must-revalidate\r\nContent-Type: text/html\r\n\r\n"


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
		printf("NULLED\n"); \
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
#endif

