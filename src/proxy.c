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

/* proxy.c */

#include "main.h"
#include "utils.h"
#include "proxy.h"
#include "handle_http.h"
#include "sock.h"
#include "errno.h"

#include <sys/epoll.h>

ape_proxy *proxy_init(char *ident, char *host, int port, acetables *g_ape)
{
	ape_proxy *proxy;

	ape_proxy_cache *host_cache;

	if (strlen(ident) > 32 || ((host_cache = proxy_cache_gethostbyname(host, g_ape)) == NULL)) {
		return NULL;
	}
	
	proxy = xmalloc(sizeof(*proxy));
	
	memcpy(proxy->identifier, ident, strlen(ident)+1);
	
	proxy->sock.host = host_cache;
	proxy->sock.port = port;
	proxy->sock.fd = -1;
	
	proxy->state = PROXY_NOT_CONNECTED;
	
	proxy->to = NULL;
	proxy->next = NULL;
	
	proxy->pipe = init_pipe(proxy, PROXY_PIPE, g_ape);
	
	proxy->next = g_ape->proxy.list;
	g_ape->proxy.list = proxy;
	
	return proxy;
}


/* IP are resolved during the "boot period" */
ape_proxy_cache *proxy_cache_gethostbyname(char *name, acetables *g_ape)
{
	ape_proxy_cache *host_cache = g_ape->proxy.hosts;
	
	while (host_cache != NULL) {
		if (strcasecmp(host_cache->host, name) == 0 && strlen(host_cache->ip)) {
			return host_cache;
		}
		host_cache = host_cache->next;
	}
	return NULL;
}

void proxy_cache_addip(char *name, char *ip, acetables *g_ape)
{
	ape_proxy_cache *cache;
	
	if (strlen(name) > 512 || strlen(ip) > 15) {
		return;
	}
	cache = xmalloc(sizeof(*cache));
	cache->host = xstrdup(name);
	strncpy(cache->ip, ip, 16);
	
	cache->next = g_ape->proxy.hosts;
	g_ape->proxy.hosts = cache;
}

void proxy_attach(ape_proxy *proxy, char *pipe, int allow_write, acetables *g_ape)
{

}

/* Not used for now */
void proxy_connect_all(acetables *g_ape)
{
	ape_proxy *proxy = g_ape->proxy.list;
	
	while (proxy != NULL) {
		if (proxy->state == PROXY_NOT_CONNECTED) {
			proxy_connect(proxy, g_ape);
		}
		proxy = proxy->next;
	}
}

int proxy_connect(ape_proxy *proxy, acetables *g_ape)
{
	int sock;
	struct sockaddr_in addr;
	struct epoll_event cev;
	
	if (proxy == NULL || proxy->state != PROXY_NOT_CONNECTED || !strlen(proxy->sock.host->ip)) {
		return 0;
	}

	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	{
		printf("ERREUR: socket().. (%s line: %i)\n",__FILE__, __LINE__);
		return 0;
	}

        addr.sin_family = AF_INET;
        addr.sin_port = htons(proxy->sock.port);
        addr.sin_addr.s_addr = inet_addr(proxy->sock.host->ip);
        memset(&(addr.sin_zero), '\0', 8);
        
        setnonblocking(sock);
        
        if (connect(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr)) == 0 || errno != EINPROGRESS) {

        	return 0;
        }
	proxy->state = PROXY_IN_PROGRESS;
	
	cev.events = EPOLLIN | EPOLLET | EPOLLOUT | EPOLLRDHUP | EPOLLPRI;
	cev.data.fd = sock;

	epoll_ctl(*(g_ape->epoll_fd), EPOLL_CTL_ADD, sock, &cev);
	
	return sock;

}

