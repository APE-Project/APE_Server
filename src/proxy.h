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

/* proxy.h */

#ifndef _PROXY
#define _PROXY

#include "http.h"

typedef struct _ape_proxy_pipe ape_proxy_pipe;

struct _ape_proxy_pipe
{
	int allow_write;
	char pipe[33];
	
	struct _ape_proxy_pipe *next;
};

typedef struct _ape_proxy ape_proxy;
struct _ape_proxy
{
	char identifier[33];
	
	int eol; // FLushing at CRLF
	
	/* proxy pipe */
	transpipe *pipe;
	
	struct {
		struct _ape_proxy_cache *host;
		int port;
		int fd;
	} sock;
	
	int state;
	
	/* List of allowed user/pipe */
	ape_proxy_pipe *to;
	
	struct _ape_proxy *next;
};

typedef struct _ape_proxy_cache ape_proxy_cache;
struct _ape_proxy_cache
{
	char *host;
	char ip[16];
	
	struct _ape_proxy_cache *next;
};

enum {
	PROXY_NOT_CONNECTED = 0,
	PROXY_IN_PROGRESS,
	PROXY_CONNECTED,
	PROXY_THROTTLED
};

ape_proxy *proxy_init(char *ident, char *host, int port, acetables *g_ape);
ape_proxy_cache *proxy_cache_gethostbyname(char *name, acetables *g_ape);
void proxy_cache_addip(char *name, char *ip, acetables *g_ape);
void proxy_attach(ape_proxy *proxy, char *pipe, int allow_write, acetables *g_ape);
int proxy_connect(ape_proxy *proxy, acetables *g_ape);
void proxy_connect_all(acetables *g_ape);
void proxy_onconnect(ape_proxy *proxy);
void proxy_process_eol(connection *co);
void proxy_init_from_conf(acetables *g_ape);
#endif

