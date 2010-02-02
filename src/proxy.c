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

/* proxy.c */

/* /!\ Not used anymore : see proxy.js */

#include "utils.h"
#include "proxy.h"
#include "handle_http.h"
#include "sock.h"
#include "errno.h"
#include "http.h"
#include "config.h"
#include "base64.h"
#include "pipe.h"
#include "raw.h"
#include "events.h"

#include <netdb.h> 
#include <sys/types.h>
#include <netinet/in.h> 



void proxy_init_from_conf(acetables *g_ape)
{
	apeconfig *conf = g_ape->srv;
	
	while (conf != NULL) {
		if (strcasecmp(conf->section, "Proxy") == 0) {
			char *host, *port, *readonly, *id;
			int iPort;

			host = ape_config_get_key(conf, "host");
			port = ape_config_get_key(conf, "port");
			readonly = ape_config_get_key(conf, "readonly");
			id = ape_config_get_key(conf, "id");
			
			if (host != NULL && port != NULL && readonly != NULL && id != NULL) {
				struct hostent *h;
				
				iPort = atoi(port);
				if ((h = gethostbyname(host)) != NULL) {
					printf("Cache : (%s) : %s\n", host, inet_ntoa(*((struct in_addr *)h->h_addr)));
					proxy_cache_addip(host, inet_ntoa(*((struct in_addr *)h->h_addr)), g_ape);
					
					if (strcasecmp(readonly, "true") == 0) {
						proxy_init(id, host, iPort, g_ape);
					} else {
						;//
					}
				} else {
					printf("[Warn] Unable to resolve : %s\n", host);
				}
			} else {
				printf("[Warn] Proxy : Configuration error\n");
			}
			
		}
		conf = conf->next;
	}
}


ape_proxy *proxy_init_by_host_port(char *host, char *port, acetables *g_ape)
{
	ape_proxy *proxy;
	apeconfig *conf = g_ape->srv;
	
	while (conf != NULL) {
		if (strcasecmp(conf->section, "Proxy") == 0) {
			char *h, *p, *id;
			
			h = ape_config_get_key(conf, "host");
			p = ape_config_get_key(conf, "port");
			id = ape_config_get_key(conf, "id");

			
			if (h != NULL && p != NULL && id != NULL && strcasecmp(host, h) == 0 && strcasecmp(port, p) == 0) {
				if ((proxy = proxy_init(id, host, atoi(port), g_ape)) != NULL) {

					return proxy;
				}
			}
					
		}
		conf = conf->next;
	}

	return NULL;
}


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
	proxy->eol = 0;
	
	proxy->state = PROXY_NOT_CONNECTED;
	proxy->nlink = 0;
	
	proxy->to = NULL;
	proxy->next = NULL;
	proxy->properties = NULL;
	
	proxy->pipe = init_pipe(proxy, PROXY_PIPE, g_ape);
	
	
	proxy->prev = NULL;
	
	proxy->next = g_ape->proxy.list;
	if (proxy->next != NULL) {
		proxy->next->prev = proxy;
	}
	
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
	ape_proxy_pipe *to;
	transpipe *gpipe;
	
	if (proxy == NULL || ((gpipe = get_pipe(pipe, g_ape)) == NULL)) {
		return;
	}
	to = xmalloc(sizeof(*to));
	memcpy(to->pipe, gpipe->pubid, strlen(gpipe->pubid)+1);

	to->allow_write = allow_write;
	
	to->next = proxy->to;
	proxy->to = to;
	
	proxy->nlink++;
	
	link_pipe(gpipe, proxy->pipe, proxy_detach);
}


void proxy_detach(transpipe *unlinker, transpipe *tproxy, acetables *g_ape)
{
	ape_proxy_pipe **to;
	ape_proxy *proxy = tproxy->pipe;

	to = &(proxy->to);

	while (*to != NULL) {

		if (strcmp((*to)->pipe, unlinker->pubid) == 0) {

			ape_proxy_pipe *pTo = *to;
			*to = (*to)->next;
			free(pTo);
			proxy->nlink--;
			break;
		}
		to = &(*to)->next;
	}
	if (!proxy->nlink) {

		proxy_shutdown(proxy, g_ape);
	}
}

// proxy->to must be clean
void proxy_shutdown(ape_proxy *proxy, acetables *g_ape)
{

	if (proxy->state == PROXY_CONNECTED) {
		shutdown(proxy->sock.fd, 2);
		proxy->state = PROXY_TOFREE;
	}
	if (proxy->prev != NULL) {
		proxy->prev->next = proxy->next;
	} else {
		g_ape->proxy.list = proxy->next;
	}
	if (proxy->next != NULL) {
		proxy->next->prev = proxy->prev;
	}
	
	destroy_pipe(proxy->pipe, g_ape);
	
	if (proxy->state != PROXY_TOFREE) {
		free(proxy);
	}
}


ape_proxy *proxy_are_linked(char *pubid, char *pubid_proxy, acetables *g_ape)
{
	transpipe *pipe = get_pipe(pubid_proxy, g_ape);
	struct _ape_proxy_pipe *ppipe;
	
	if (pipe == NULL || pipe->type != PROXY_PIPE || ((ppipe = ((ape_proxy *)(pipe->pipe))->to) == NULL)) {
		return NULL;
	}
	
	while (ppipe != NULL) {
		if (strcmp(pubid, ppipe->pipe) == 0) {
			return ((ape_proxy *)(pipe->pipe));
		}
		ppipe = ppipe->next;
	}
	
	return NULL;
}

void proxy_process_eol(ape_socket *co, acetables *g_ape)
{
	char *b64;
	ape_proxy *proxy = co->attach;
	char *data = co->buffer_in.data;

	RAW *newraw;
	json_item *jlist = json_new_object();
	
	data[co->buffer_in.length] = '\0';
	b64 = base64_encode(data, strlen(data));
	
	json_set_property_strZ(jlist, "data", b64);
	json_set_property_strN(jlist, "event", 5, "READ", 4);
	json_set_property_objN(jlist, "pipe", 4, get_json_object_proxy(proxy));
	
	newraw = forge_raw("PROXY_EVENT", jlist);
	
	proxy_post_raw(newraw, proxy, g_ape);
	
	free(b64);	
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
	
	events_add(g_ape->events, sock, EVENT_READ|EVENT_WRITE);
	
	return sock;

}

void proxy_onevent(ape_proxy *proxy, char *event, acetables *g_ape)
{
	RAW *newraw;
	json_item *jlist = json_new_object();
	
	json_set_property_strZ(jlist, "event", event);
	json_set_property_objN(jlist, "pipe", 4, get_json_object_proxy(proxy));
	
	newraw = forge_raw("PROXY_EVENT", jlist);
	
	proxy_post_raw(newraw, proxy, g_ape);
}

void proxy_write(ape_proxy *proxy, char *data, acetables *g_ape)
{
	char *b64;
	int len;
	if (proxy->state != PROXY_CONNECTED) {
		return;
	}
	
	b64 = xmalloc(strlen(data)+1);
	len = base64_decode(b64, data, strlen(data)+1);
	
	sendbin(proxy->sock.fd, b64, len, 0, g_ape);
	free(b64);
}


json_item *get_json_object_proxy(ape_proxy *proxy)
{
	json_item *jstr = json_new_object();
	json_item *jprop = json_new_object();
	extend *eTmp = proxy->properties;

	json_set_property_strN(jstr, "pubid", 5, proxy->pipe->pubid, 32);
	json_set_property_strN(jstr, "casttype", 8, "proxy", 5);
	
	json_set_property_strZ(jprop, "host", proxy->sock.host->host);
	json_set_property_strZ(jprop, "ip", proxy->sock.host->ip);
	json_set_property_intZ(jprop, "port", proxy->sock.port);

	while (eTmp != NULL) {
		json_set_property_strZ(jprop, eTmp->key, eTmp->val);
		eTmp = eTmp->next;
	}
	
	json_set_property_objN(jstr, "properties", 10, jprop);

	return jstr;
}

