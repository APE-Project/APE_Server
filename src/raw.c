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

/* raw.c */

#include "raw.h"
#include "users.h"
#include "channel.h"
#include "proxy.h"
#include "utils.h"
#include "plugins.h"
#include "pipe.h"
#include "transports.h"

RAW *forge_raw(const char *raw, json_item *jlist)
{
	RAW *new_raw;
	char unixtime[16];
	struct jsontring *string;
	
	json_item *jstruct = NULL;
		
	sprintf(unixtime, "%li", time(NULL));
	
	jstruct = json_new_object();
	
	json_set_property_strN(jstruct, "time", 4, unixtime, strlen(unixtime));
	json_set_property_strN(jstruct, "raw", 3, raw, strlen(raw));
	json_set_property_objN(jstruct, "data", 4, jlist);

	string = json_to_string(jstruct, NULL, 1);

	new_raw = xmalloc(sizeof(*new_raw));
    new_raw->len = string->len;
	new_raw->next = NULL;
	new_raw->priority = RAW_PRI_LO;
	new_raw->refcount = 0;

	new_raw->data = string->jstring;

	free(string);

	return new_raw;
}

int free_raw(RAW *fraw)
{
	if (--(fraw->refcount) <= 0) {
		free(fraw->data);
		free(fraw);

		return 0;
	}
	return fraw->refcount;
}

RAW *copy_raw(RAW *input)
{
	RAW *new_raw;
	
	new_raw = xmalloc(sizeof(*new_raw));
	new_raw->len = input->len;
	new_raw->next = input->next;
	new_raw->priority = input->priority;
	new_raw->refcount = 0;
    new_raw->data = xmalloc(sizeof(char) * (new_raw->len + 1));

    memcpy(new_raw->data, input->data, new_raw->len + 1);	

	return new_raw;	
}

RAW *copy_raw_z(RAW *input)
{
	(input->refcount)++;
	
	return input;
}


/************* Users related functions ****************/

/* Post raw to a subuser */
void post_raw_sub(RAW *raw, subuser *sub, acetables *g_ape)
{
	FIRE_EVENT_NULL(post_raw_sub, raw, sub, g_ape);

	int add_size = 16;
	struct _raw_pool_user *pool = (raw->priority == RAW_PRI_LO ? &sub->raw_pools.low : &sub->raw_pools.high);

	if (++pool->nraw == pool->size) {
		pool->size += add_size;
		expend_raw_pool(pool->rawfoot, add_size);
	}
	
	pool->rawfoot->raw = raw;
	pool->rawfoot = pool->rawfoot->next;
	
	(sub->raw_pools.nraw)++;
	
}

/* Post raw to a user and propagate it to all of it's subuser */
void post_raw(RAW *raw, USERS *user, acetables *g_ape)
{
	subuser *sub = user->subuser;
	while (sub != NULL) {
		post_raw_sub(copy_raw_z(raw), sub, g_ape);
		sub = sub->next;
	}
}

/* Post raw to a user and propagate it to all of it's subuser with *sub exception */
void post_raw_restricted(RAW *raw, USERS *user, subuser *sub, acetables *g_ape)
{
	subuser *tSub = user->subuser;
	
	if (sub == NULL) {
		return;
	}
	while (tSub != NULL) {
		if (sub != tSub) {
			post_raw_sub(copy_raw_z(raw), tSub, g_ape);
		}
		tSub = tSub->next;
	}
}

/************* Channels related functions ****************/

/* Post raw to a channel and propagate it to all of it's users */
void post_raw_channel(RAW *raw, struct CHANNEL *chan, acetables *g_ape)
{
	userslist *list;
	
	if (chan == NULL || raw == NULL || chan->head == NULL) {
		return;
	}
	list = chan->head;
	while (list) {
		post_raw(raw, list->userinfo, g_ape);
		list = list->next;
	}

}

/* Post raw to a channel and propagate it to all of it's users with a *ruser exception */
void post_raw_channel_restricted(RAW *raw, struct CHANNEL *chan, USERS *ruser, acetables *g_ape)
{
	userslist *list;
	
	if (chan == NULL || raw == NULL || chan->head == NULL) {
		return;
	}
	list = chan->head;
	
	while (list) {
		if (list->userinfo != ruser) {
			post_raw(raw, list->userinfo, g_ape);
		}
		list = list->next;
	}
}


/************* Proxys related functions ****************/

/* Post raw to a proxy and propagate it to all of it's attached users */
void proxy_post_raw(RAW *raw, ape_proxy *proxy, acetables *g_ape)
{
	ape_proxy_pipe *to = proxy->to;
	transpipe *pipe;
	
	while (to != NULL) {
		pipe = get_pipe(to->pipe, g_ape);
		if (pipe != NULL && pipe->type == USER_PIPE) {
			post_raw(raw, pipe->pipe, g_ape);
		} else {
			;//
		}
		to = to->next;
	}

}

/* to manage subuser use post_to_pipe() instead */
int post_raw_pipe(RAW *raw, const char *pipe, acetables *g_ape)
{
	transpipe *spipe;
	
	if ((spipe = get_pipe(pipe, g_ape)) != NULL) {
		
		if (spipe->type == CHANNEL_PIPE) {
			post_raw_channel(raw, spipe->pipe, g_ape);
			return 1;
		} else {
			post_raw(raw, spipe->pipe, g_ape);
			return 1;
		}
	}
	return 0;
}

int post_to_pipe(json_item *jlist, const char *rawname, const char *pipe, subuser *from, acetables *g_ape)
{
	USERS *sender = from->user;
	transpipe *recver = get_pipe_strict(pipe, sender, g_ape);
	json_item *jlist_copy = NULL;
	RAW *newraw;
	
	if (sender != NULL) {
		if (recver == NULL) {
			send_error(sender, "UNKNOWN_PIPE", "109", g_ape);
			return 0;
		}
		json_set_property_objN(jlist, "from", 4, get_json_object_user(sender));

	}
	
	if (sender != NULL && sender->nsub > 1) {
		jlist_copy = json_item_copy(jlist, NULL);
	
		json_set_property_objN(jlist_copy, "pipe", 4, get_json_object_pipe(recver));
		newraw = forge_raw(rawname, jlist_copy);
		post_raw_restricted(newraw, sender, from, g_ape);
	}	
	switch(recver->type) {
		case USER_PIPE:
			json_set_property_objN(jlist, "pipe", 4, get_json_object_user(sender));
			newraw = forge_raw(rawname, jlist);
			post_raw(newraw, recver->pipe, g_ape);
			break;
		case CHANNEL_PIPE:
			if (((CHANNEL*)recver->pipe)->head != NULL && ((CHANNEL*)recver->pipe)->head->next != NULL) {
				json_set_property_objN(jlist, "pipe", 4, get_json_object_channel(recver->pipe));
				newraw = forge_raw(rawname, jlist);
				post_raw_channel_restricted(newraw, recver->pipe, sender, g_ape);
			}
			break;
		case CUSTOM_PIPE:
			json_set_property_objN(jlist, "pipe", 4, get_json_object_user(sender));
			post_json_custom(jlist, sender, recver, g_ape);
			break;
		default:
			break;
	}
		
	return 1;
}


int send_raw_inline(ape_socket *client, transport_t transport, RAW *raw, acetables *g_ape)
{
	struct _transport_properties *properties;
	int finish = 1;
	
	properties = transport_get_properties(transport, g_ape);
	
	switch(transport) {
		case TRANSPORT_XHRSTREAMING:
			finish &= http_send_headers(NULL, HEADER_XHR, HEADER_XHR_LEN, client, g_ape);
			break;
		case TRANSPORT_SSE_LONGPOLLING:
			finish &= http_send_headers(NULL, HEADER_SSE, HEADER_SSE_LEN, client, g_ape);
			break;	
		default:
			finish &= http_send_headers(NULL, HEADER_DEFAULT, HEADER_DEFAULT_LEN, client, g_ape);
			break;
	}
	
	if (properties != NULL && properties->padding.left.val != NULL) {
		finish &= sendbin(client->fd, properties->padding.left.val, properties->padding.left.len, g_ape);
	}	
	
	finish &= sendbin(client->fd, "[", 1, g_ape);
	
	finish &= sendbin(client->fd, raw->data, raw->len, g_ape);
	
	finish &= sendbin(client->fd, "]", 1, g_ape);
	
	if (properties != NULL && properties->padding.right.val != NULL) {
		finish &= sendbin(client->fd, properties->padding.right.val, properties->padding.right.len, g_ape);
	}
	
	free_raw(raw);
	
	return finish;
}

/*
	Send queue to socket
*/
int send_raws(subuser *user, acetables *g_ape)
{
	int finish = 1, state = 0;
	struct _raw_pool *pool;
	struct _transport_properties *properties;
	
	if (user->raw_pools.nraw == 0) {
		return 1;
	}
	
	PACK_TCP(user->client->fd); /* Activate TCP_CORK */
	
	properties = transport_get_properties(user->user->transport, g_ape);
	
	if (!user->headers.sent) {
		user->headers.sent = 1;
		
		switch(user->user->transport) {
			case TRANSPORT_XHRSTREAMING:
				finish &= http_send_headers(user->headers.content, HEADER_XHR, HEADER_XHR_LEN, user->client, g_ape);
				break;
			case TRANSPORT_SSE_LONGPOLLING:
				finish &= http_send_headers(user->headers.content, HEADER_SSE, HEADER_SSE_LEN, user->client, g_ape);
				break;	
			default:
				finish &= http_send_headers(user->headers.content, HEADER_DEFAULT, HEADER_DEFAULT_LEN, user->client, g_ape);
				break;
		}
		
	}
	
	if (properties != NULL && properties->padding.left.val != NULL) {
		finish &= sendbin(user->client->fd, properties->padding.left.val, properties->padding.left.len, g_ape);
	}
	
	finish &= sendbin(user->client->fd, "[", 1, g_ape);
	
	if (user->raw_pools.high.nraw) {
		pool = user->raw_pools.high.rawfoot->prev;
	} else {
		pool = user->raw_pools.low.rawhead;
		state = 1;
	}
	
	while (pool->raw != NULL) {
		struct _raw_pool *pool_next = (state ? pool->next : pool->prev);
		
		finish &= sendbin(user->client->fd, pool->raw->data, pool->raw->len, g_ape);
		
		if ((pool_next != NULL && pool_next->raw != NULL) || (!state && user->raw_pools.low.nraw)) {
			finish &= sendbin(user->client->fd, ",", 1, g_ape);
		} else {
			finish &= sendbin(user->client->fd, "]", 1, g_ape);
			
			if (properties != NULL && properties->padding.right.val != NULL) {
				finish &= sendbin(user->client->fd, properties->padding.right.val, properties->padding.right.len, g_ape);
			}
		}
		
		free_raw(pool->raw);
		pool->raw = NULL;
		
		pool = pool_next;
		
		if ((pool == NULL || pool->raw == NULL) && !state) {
			pool = user->raw_pools.low.rawhead;
			state = 1;
		}
	}
	
	user->raw_pools.high.nraw = 0;
	user->raw_pools.low.nraw = 0;
	user->raw_pools.nraw = 0;
	
	user->raw_pools.high.rawfoot = user->raw_pools.high.rawhead;
	user->raw_pools.low.rawfoot = user->raw_pools.low.rawhead;
	
	FLUSH_TCP(user->client->fd);
	
	return finish;
}

struct _raw_pool *init_raw_pool(int n)
{
	int i;
	struct _raw_pool *pool = xmalloc(sizeof(*pool) * n);
	
	for (i = 0; i < n; i++) {
		pool[i].raw = NULL;
		pool[i].next = (i == n-1 ? NULL : &pool[i+1]);
		pool[i].prev = (i == 0 ? NULL : &pool[i-1]);
		pool[i].start = (i == 0);
	}
	
	return pool;
}

struct _raw_pool *expend_raw_pool(struct _raw_pool *ptr, int n)
{
	struct _raw_pool *pool = init_raw_pool(n);
	
	ptr->next = pool;
	pool->prev = ptr;
	
	return pool;
}

void destroy_raw_pool(struct _raw_pool *ptr)
{
	struct _raw_pool *pool = ptr, *tpool = NULL;
	
	while (pool != NULL) {
		if (pool->raw != NULL) {
			free_raw(pool->raw);
		}
		if (pool->start) {
			if (tpool != NULL) {
				free(tpool);
			}
			tpool = pool;
		}
		pool = pool->next;
	}
	if (tpool != NULL) {
		free(tpool);
	}
}
