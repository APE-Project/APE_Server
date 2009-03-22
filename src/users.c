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

/* users.c */


#include "hash.h"
#include "handle_http.h"
#include "sock.h"
#include "extend.h"

#include "users.h"
#include "config.h"
#include "cmd.h"
#include "json.h"
#include "plugins.h"
#include "pipe.h"

#include <sys/time.h>
#include <time.h>
#include "utils.h"

unsigned int isonchannel(USERS *user, CHANNEL *chan)
{
	CHANLIST *clist;
	
	if (user == NULL || chan == NULL) {
		return 0;
	}
	
	clist = user->chan_foot;
	
	while (clist != NULL) {
		if (clist->chaninfo == chan) {
			return 1;
		}
		clist = clist->next;
	}
	
	return 0;
}

void grant_aceop(USERS *user)
{
	user->flags = FLG_AUTOOP | FLG_NOKICK | FLG_BOTMANAGE;	
}

// return user with a channel pubid
USERS *seek_user(char *pubid, char *linkid, acetables *g_ape)
{
	USERS *suser;
	CHANLIST *clist;

	if ((suser = seek_user_simple(pubid, g_ape)) == NULL) {
		return NULL;
	}
	
	clist = suser->chan_foot;
	
	while (clist != NULL) {
		if (strcasecmp(clist->chaninfo->pipe->pubid, linkid) == 0) {
			return suser;
		}
		clist = clist->next;
	}
	
	return NULL;
}
USERS *seek_user_simple(char *pubid, acetables *g_ape)
{
	transpipe *gpipe;

	gpipe = get_pipe(pubid, g_ape);
	
	if (gpipe == NULL || gpipe->type != USER_PIPE) {
		return NULL;
	}
	
	return gpipe->pipe;
	
}

USERS *seek_user_id(char *sessid, acetables *g_ape)
{
	if (strlen(sessid) != 32) {
		return NULL;
	}
	return ((USERS *)hashtbl_seek(g_ape->hSessid, sessid));
}


USERS *init_user(acetables *g_ape)
{
	USERS *nuser;
	
	nuser = xmalloc(sizeof(*nuser));


	nuser->idle = time(NULL);
	nuser->next = g_ape->uHead;
	nuser->prev = NULL;
	nuser->nraw = 0;

	nuser->flags = FLG_NOFLAG;
	nuser->chan_foot = NULL;

	nuser->sessions.data = NULL;
	nuser->sessions.length = 0;
	
	nuser->properties = NULL;
	nuser->subuser = NULL;
	nuser->nsub = 0;
	nuser->type = HUMAN;
	
	nuser->links.ulink = NULL;
	nuser->links.nlink = 0;
	nuser->transport = TRANSPORT_LONGPOLLING;
	
	nuser->lastping[0] = '\0';
	
	if (nuser->next != NULL) {
		nuser->next->prev = nuser;
	}

	gen_sessid_new(nuser->sessid, g_ape);
	
	return nuser;
}

USERS *adduser(unsigned int fdclient, char *host, acetables *g_ape)
{
	USERS *nuser = NULL;

	/* Calling module */
	FIRE_EVENT(adduser, nuser, fdclient, host, g_ape);
	

	nuser = init_user(g_ape);
	
	nuser->type = (fdclient ? HUMAN : BOT);
		
	g_ape->uHead = nuser;
	
	nuser->pipe = init_pipe(nuser, USER_PIPE, g_ape);

	hashtbl_append(g_ape->hSessid, nuser->sessid, (void *)nuser);
	
	g_ape->nConnected++;
	
	addsubuser(fdclient, host, nuser);

	
	return nuser;
	
}

void deluser(USERS *user, acetables *g_ape)
{

	if (user == NULL) {
		return;
	}

	FIRE_EVENT_NULL(deluser, user, g_ape);
	

	left_all(user, g_ape);
	
	/* kill all users connections */
	
	clear_subusers(user);

	hashtbl_erase(g_ape->hSessid, user->sessid);

	
	g_ape->nConnected--;

	
	if (user->prev == NULL) {
		g_ape->uHead = user->next;
	} else {
		user->prev->next = user->next;
	}
	if (user->next != NULL) {
		user->next->prev = user->prev;
	}

	clear_sessions(user);
	clear_properties(&user->properties);
	destroy_pipe(user->pipe, g_ape);
	
	free(user);

	user = NULL;
}


RAW *forge_raw(char *raw, struct json *jlist)
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
	
	new_raw->len = strlen(string->jstring);	
	new_raw->data = xstrdup(string->jstring);

	new_raw->next = NULL;
	new_raw->priority = 0;
	
	free(string->jstring);
	free(string);
	
	return new_raw;
}

// CoW
RAW *copy_raw(RAW *input)
{
	RAW *new_raw;

	new_raw = xmalloc(sizeof(*new_raw));
	new_raw->data = xstrdup(input->data);
	new_raw->len = input->len;
	
	new_raw->next = input->next;
	new_raw->priority = input->priority;
	
	return new_raw;	
}

void post_raw_sub(RAW *raw, subuser *sub)
{

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
void post_raw(RAW *raw, USERS *user)
{
	subuser *sub = user->subuser;
	
	while (sub != NULL) {
		post_raw_sub(copy_raw(raw), sub);
		sub = sub->next;
	}
	free(raw->data);
	free(raw);
}

void post_raw_restricted(RAW *raw, USERS *user, subuser *sub)
{
	subuser *tSub = user->subuser;
	
	if (sub == NULL) {
		return;
	}
	while (tSub != NULL) {
		if (sub != tSub) {
			post_raw_sub(copy_raw(raw), tSub);
		}
		tSub = tSub->next;
	}
	free(raw->data);
	free(raw);	
}

void post_raw_channel(RAW *raw, struct CHANNEL *chan)
{
	userslist *list;
	
	if (chan == NULL || raw == NULL || chan->head == NULL) {
		return;
	}
	list = chan->head;
	while (list) {
		post_raw(copy_raw(raw), list->userinfo);
		list = list->next;
	}
	free(raw->data);
	free(raw);
}


void post_raw_channel_restricted(RAW *raw, struct CHANNEL *chan, USERS *ruser)
{
	userslist *list;
	
	if (chan == NULL || raw == NULL || chan->head == NULL) {
		return;
	}
	list = chan->head;
	while (list) {
		if (list->userinfo != ruser) {
			post_raw(copy_raw(raw), list->userinfo);
		}
		list = list->next;
	}
	free(raw->data);
	free(raw);
}

/*
	Send queue to socket
*/
void send_raws(subuser *user)
{
	RAW *raw, *older;

	if (user->nraw == 0 || user->rawhead == NULL) {
		return;
	}
	raw = user->rawhead;
	
	if (!(user->user->flags & FLG_PCONNECT) || !user->headers_sent) {
		user->headers_sent = 1;
		sendbin(user->fd, HEADER, HEADER_LEN);
	}
	if (raw != NULL) {
		sendbin(user->fd, "[\n", 2);
	}
	while(raw != NULL) {

		sendbin(user->fd, raw->data, raw->len);
		
		if (raw->next != NULL) {
			sendbin(user->fd, ",\n", 2);
		} else {
			sendbin(user->fd, "\n]\n", 3);	
		}
		older = raw;
		raw = raw->next;
		
		free(older->data);
		free(older);
	}
	
	user->rawhead = NULL;
	user->rawfoot = NULL;
	user->nraw = 0;
}
void do_died(subuser *user)
{

	if (user->state == ALIVE && user->user->type == HUMAN && !(user->user->flags & FLG_PCONNECT)) {
		user->state = ADIED;
		user->headers_sent = 0;
		
		shutdown(user->fd, 2);
	}
}


struct json *get_json_object_user(USERS *user)
{
	json *jstr = NULL;
	
	if (user != NULL) {
	
		set_json("pubid", user->pipe->pubid, &jstr);
		set_json("casttype", "uni", &jstr);
		
		if (user->properties != NULL) {
			json *jprop = NULL;
			set_json("properties", NULL, &jstr);
			
			extend *eTmp = user->properties;
			
			while (eTmp != NULL) {
				set_json(eTmp->key, eTmp->val, &jprop);
				eTmp = eTmp->next;
			}
			json_attach(jstr, jprop, JSON_OBJECT);
		}

	} else {
		set_json("pubid", SERVER_NAME, &jstr);
	}
	return jstr;
}

void check_timeout(acetables *g_ape)
{
	USERS *list, *wait;
	long int ctime = time(NULL);
	
	list = g_ape->uHead;
	
	while (list != NULL) {
		
		wait = list->next;
		if ((ctime - list->idle) >= TIMEOUT_SEC && list->type == HUMAN) {
			deluser(list, g_ape);
		} else if (list->type == HUMAN) {
			subuser **n = &(list->subuser);
			while (*n != NULL)
			{
				if ((ctime - (*n)->idle) >= TIMEOUT_SEC)
				{
					delsubuser(n);
					continue;
				}
				if ((*n)->state == ALIVE && (*n)->nraw && !(*n)->need_update) {
					send_raws(*n);
					do_died(*n);
				}
				n = &(*n)->next;
			}
		}
		
		list = wait;
	}

}

void send_error(USERS *user, char *msg, char *code)
{
	RAW *newraw;
	json *jlist = NULL;
	
	set_json("value", msg, &jlist);
	set_json("code", code, &jlist);
	
	newraw = forge_raw(RAW_ERR, jlist);
	
	post_raw(newraw, user);	
}


void send_msg(USERS *user, char *msg, char *type)
{
	RAW *newraw;
	json *jlist = NULL;
	
	set_json("value", msg, &jlist);
	
	newraw = forge_raw(type, jlist);
	
	post_raw(newraw, user);	
}

void send_msg_channel(CHANNEL *chan, char *msg, char *type)
{
	RAW *newraw;
	json *jlist = NULL;
	
	set_json("value", msg, &jlist);
	
	newraw = forge_raw(type, jlist);
	
	post_raw_channel(newraw, chan);
}

void send_msg_sub(subuser *sub, char *msg, char *type)
{
	RAW *newraw;
	json *jlist = NULL;
	
	set_json("value", msg, &jlist);
	
	newraw = forge_raw(type, jlist);
	
	post_raw_sub(newraw, sub);		
}

session *get_session(USERS *user, char *key)
{
	session *current = user->sessions.data;
	
	while (current != NULL) {
		if (strcmp(current->key, key) == 0) {
			return current;
		}
		current = current->next;
	}
	
	return NULL;
	
}

void clear_sessions(USERS *user)
{
	session *pSession, *pTmp;
	
	pSession = user->sessions.data;
	
	while (pSession != NULL) {
		pTmp = pSession->next;
		free(pSession->val);
		free(pSession);
		pSession = pTmp;
	}
	user->sessions.data = NULL;
	user->sessions.length = 0;
}

session *set_session(USERS *user, char *key, char *val, int update)
{
	session *new_session = NULL, *sTmp = NULL;
	int vlen = strlen(val);
		
	if (strlen(key) > 32 || user->sessions.length+vlen > MAX_SESSION_LENGTH) {
		return NULL;
	}
	
	if ((sTmp = get_session(user, key)) != NULL) {
		int tvlen = strlen(sTmp->val);
		
		if (vlen > tvlen) {
			sTmp->val = xrealloc(sTmp->val, sizeof(char) * (vlen+1)); // if new val is bigger than previous
		}
		user->sessions.length += (vlen - tvlen); // update size
		strcpy(sTmp->key, key);
		strcpy(sTmp->val, val);
		if (update) {
			sendback_session(user, sTmp);
		}
		return sTmp;
	}
	
	sTmp = user->sessions.data;
	
	new_session = xmalloc(sizeof(*new_session));
	new_session->val = xmalloc(sizeof(char) * (vlen+1));
	
	user->sessions.length += vlen;
	
	strcpy(new_session->key, key);
	strcpy(new_session->val, val);
	new_session->next = sTmp;
	
	user->sessions.data = new_session;
	if (update) {
		sendback_session(user, new_session);
	}	
	return new_session;
}

void sendback_session(USERS *user, session *sess)
{
	subuser *current = user->subuser;
	
	while (current != NULL) {
		if (current->need_update) {
			json *jlist = NULL, *jobj = NULL;
			RAW *newraw;
			
			current->need_update = 0;
			set_json("sessions", NULL, &jlist);
			set_json(sess->key, (sess != NULL ? sess->val : NULL), &jobj);
			json_attach(jlist, jobj, JSON_OBJECT);
			newraw = forge_raw("SESSIONS", jlist);
			newraw->priority = 1;
			post_raw_sub(newraw, current);
		}
		current = current->next;
	}	
	
}

subuser *addsubuser(int fd, char *channel, USERS *user)
{
	subuser *sub;
	
	if (getsubuser(user, channel) != NULL || strlen(channel) > MAX_HOST_LENGTH) {
		return NULL;
	}

	sub = xmalloc(sizeof(*sub));
	sub->fd = fd;
	sub->state = ADIED;
	sub->user = user;
	
	memcpy(sub->channel, channel, strlen(channel)+1);
	sub->next = user->subuser;
	
	sub->rawhead = NULL;
	sub->rawfoot = NULL;
	sub->nraw = 0;
	sub->wait_for_free = 0;
	sub->headers_sent = 0;
	
	sub->idle = time(NULL);
	sub->need_update = 0;
	
	
	
	if (user->nsub) {
		sub->need_update = 1;
		ping_request(user);
	}
	
	(user->nsub)++;
	
	user->subuser = sub;

	return sub;
}

subuser *getsubuser(USERS *user, char *channel)
{
	subuser *current = user->subuser;
	

	while (current != NULL) {
		if (strcmp(current->channel, channel) == 0) {
			
			return current;
		}
		current = current->next;
	}
	
	return NULL;
}

void delsubuser(subuser **current)
{
	subuser *del = *current;
	
	((*current)->user->nsub)--;
	
	*current = (*current)->next;
	clear_subuser_raws(del);

	
	if (del->state == ALIVE) {
		del->wait_for_free = 1;
		do_died(del);
	} else {
		do_died(del);
		free(del);
	}
	
}

void clear_subusers(USERS *user)
{
	while (user->subuser != NULL) {
		delsubuser(&(user->subuser));
	}
}

void clear_subuser_raws(subuser *sub)
{
	RAW *raw, *older;
	
	raw = sub->rawhead;
	while(raw != NULL) {
		older = raw;
		raw = raw->next;
		free(older->data);
		free(older);
	}
	sub->rawhead = NULL;
	sub->rawfoot = NULL;
	sub->nraw = 0;
	sub = sub->next;	
}
void ping_request(USERS *user)
{

	struct timeval t;
	gettimeofday(&t, NULL);

	sprintf(user->lastping, "%li%li", t.tv_sec, t.tv_usec);
	
	send_msg(user, user->lastping, "KING");	
}

struct _users_link *are_linked(USERS *a, USERS *b)
{
	USERS *aUser, *bUser;
	struct _link_list *ulink;
	
	if (!a->links.nlink || !b->links.nlink) {
		return NULL;
	}
	
	/* Walk on the smallest list */
	if (a->links.nlink <= b->links.nlink) {
		aUser = a;
		bUser = b;
	} else {
		aUser = b;
		bUser = a;
	}
	
	ulink = aUser->links.ulink;
	
	while (ulink != NULL) {
		if (ulink->link->b == bUser || ulink->link->a == bUser) {
			return ulink->link;
		}
		ulink = ulink->next;
	}
	
	return NULL;
	
}

void make_link(USERS *a, USERS *b)
{
	struct _users_link *link;
	struct _link_list *link_a, *link_b;
	
	if (are_linked(a, b) != NULL) {	
		link = xmalloc(sizeof(*link));
	
		link_a = xmalloc(sizeof(*link_a));
		link_b = xmalloc(sizeof(*link_b));
	
		link_a->link = link;
		link_b->link = link;
	
		link_a->next = a->links.ulink;
		link_b->next = b->links.ulink;
	
		link->a = a;
		link->b = b;
	
		a->links.ulink = link_a;
		(a->links.nlink)++;
	
		b->links.ulink = link_b;
		(b->links.nlink)++;
	
		link->link_type = 0;
		printf("Link etablished between %s and %s\n", a->pipe->pubid, b->pipe->pubid);
	} else {
		printf("%s and %s are already linked\n", a->pipe->pubid, b->pipe->pubid);
	}
}

void destroy_link(USERS *a, USERS *b)
{
	struct _users_link *link;

	if ((link = are_linked(a, b)) != NULL) {
		
	}	
}



int post_to_pipe(json *jlist, char *rawname, char *pipe, subuser *from, void *restrict, acetables *g_ape)
{
	USERS *sender = from->user;
	transpipe *recver = get_pipe_strict(pipe, sender, g_ape);
	json *jlist_copy = NULL;
	RAW *newraw;
	
	
	if (sender != NULL) {
		if (recver == NULL) {
			send_error(sender, "UNKNOWN_PIPE", "109");
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
		post_raw(newraw, recver->pipe);
	} else {
		post_raw_channel_restricted(newraw, recver->pipe, sender);
	}

	newraw = forge_raw(rawname, jlist_copy);
	
	post_raw_restricted(newraw, sender, from);
	
	return 1;
}

