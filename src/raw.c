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

RAW *forge_raw(const char *raw, struct json *jlist)
{
	RAW *new_raw;
	char unixtime[16];
	
	struct jsontring *string;
	struct json *jstruct = NULL;
	
	sprintf(unixtime, "%li", time(NULL));

	set_json("datas", NULL, &jstruct);
	
	json_attach(jstruct, jlist, JSON_OBJECT);
	
	set_json("time", unixtime, &jstruct);
	set_json("raw", raw, &jstruct);

	string = jsontr(jstruct, NULL);

	new_raw = xmalloc(sizeof(*new_raw));
	
        new_raw->len = string->len;

        new_raw->data = xmalloc(sizeof(char) * (new_raw->len + 1));
        memcpy(new_raw->data, string->jstring, new_raw->len + 1);

	new_raw->next = NULL;
	new_raw->priority = 0;
	
	free(string->jstring);
	free(string);
	
	return new_raw;
}


RAW *copy_raw(RAW *input)
{
	RAW *new_raw;
	
	new_raw = xmalloc(sizeof(*new_raw));
	
        new_raw->data = xmalloc(sizeof(char) * (input->len + 1));
        memcpy(new_raw->data, input->data, input->len + 1);

	new_raw->len = input->len;
	
	new_raw->next = input->next;
	new_raw->priority = input->priority;
	
	return new_raw;	
}


/************* Users related functions ****************/

/* Post raw to a subuser */
void post_raw_sub(RAW *raw, subuser *sub, acetables *g_ape)
{

	FIRE_EVENT_NULL(post_raw_sub, raw, sub, g_ape);
	
	if (raw->priority == 0) {
		if (sub->rawhead == NULL) {
			sub->rawhead = raw;
		}
		if (sub->rawfoot != NULL) {
			sub->rawfoot->next = raw;
		}
		sub->rawfoot = raw;
	} else {
		
		if (sub->rawfoot == NULL) {
			sub->rawfoot = raw;
		}		
		raw->next = sub->rawhead;
		sub->rawhead = raw;
	}
	(sub->nraw)++;
	
}

/* Post raw to a user and propagate it to all of it's subuser */
void post_raw(RAW *raw, USERS *user, acetables *g_ape)
{
	subuser *sub = user->subuser;
	
	while (sub != NULL) {
		post_raw_sub(copy_raw(raw), sub, g_ape);
		sub = sub->next;
	}
	free(raw->data);
	free(raw);
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
			post_raw_sub(copy_raw(raw), tSub, g_ape);
		}
		tSub = tSub->next;
	}
	free(raw->data);
	free(raw);	
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
		post_raw(copy_raw(raw), list->userinfo, g_ape);
		list = list->next;
	}
	free(raw->data);
	free(raw);
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
			post_raw(copy_raw(raw), list->userinfo, g_ape);
		}
		list = list->next;
	}
	
	free(raw->data);
	free(raw);
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
			post_raw(copy_raw(raw), pipe->pipe, g_ape);
		} else {
			;//
		}
		to = to->next;
	}
	free(raw->data);
	free(raw);
}


/* to manage subuser use post_to_pipe() instead */
void post_raw_pipe(RAW *raw, const char *pipe, acetables *g_ape)
{
	transpipe *spipe;
	
	if ((spipe = get_pipe(pipe, g_ape)) != NULL) {
		if (spipe->type == CHANNEL_PIPE) {
			post_raw_channel(raw, spipe->pipe, g_ape);
		} else {
			post_raw(raw, spipe->pipe, g_ape);
		}
	}
}


int post_to_pipe(json *jlist, const char *rawname, const char *pipe, subuser *from, void *restrict, acetables *g_ape)
{
	USERS *sender = from->user;
	transpipe *recver = get_pipe_strict(pipe, sender, g_ape);
	json *jlist_copy = NULL;
	RAW *newraw;
	
	
	if (sender != NULL) {
		if (recver == NULL) {
			send_error(sender, "UNKNOWN_PIPE", "109", g_ape);
			return 0;
		}
	
		set_json("sender", NULL, &jlist);
		json_attach(jlist, get_json_object_user(sender), JSON_OBJECT);
	}

	set_json("pipe", NULL, &jlist);

	
	jlist_copy = json_copy(jlist);

	
	json_attach(jlist, (recver->type == USER_PIPE ? get_json_object_user(sender) : get_json_object_channel(recver->pipe)), JSON_OBJECT);
	json_attach(jlist_copy, (recver->type == USER_PIPE ? get_json_object_user(recver->pipe) : get_json_object_channel(recver->pipe)), JSON_OBJECT);
	
	newraw = forge_raw(rawname, jlist);
	
	if (recver->type == USER_PIPE) {
		post_raw(newraw, recver->pipe, g_ape);
	} else {
		post_raw_channel_restricted(newraw, recver->pipe, sender, g_ape);
	}

	newraw = forge_raw(rawname, jlist_copy);
	
	post_raw_restricted(newraw, sender, from, g_ape);
	
	return 1;
}


/*
	Send queue to socket
*/
int send_raws(subuser *user, acetables *g_ape)
{
	RAW *raw, *older;
	int finish = 1;
	
	if (user->nraw == 0 || user->rawhead == NULL) {
		return 1;
	}
	raw = user->rawhead;
	
	if (!(user->user->flags & FLG_PCONNECT) || !user->headers_sent) {
		user->headers_sent = 1;
		sendbin(user->fd, HEADER, HEADER_LEN, g_ape);
	}
	if (raw != NULL) {
		finish &= sendbin(user->fd, "[\n", 2, g_ape);
	}
	while(raw != NULL) {

		finish &= sendbin(user->fd, raw->data, raw->len, g_ape);
		
		if (raw->next != NULL) {
			finish &= sendbin(user->fd, ",\n", 2, g_ape);
		} else {
			finish &= sendbin(user->fd, "\n]\n\n", 3, g_ape);	
		}
		older = raw;
		raw = raw->next;
		
		free(older->data);
		free(older);
	}
	
	user->rawhead = NULL;
	user->rawfoot = NULL;
	user->nraw = 0;
	
	return finish;
}

