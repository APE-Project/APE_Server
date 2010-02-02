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

/* users.c */


#include <sys/time.h>
#include <time.h>

#include "users.h"

#include "hash.h"
#include "handle_http.h"
#include "sock.h"
#include "extend.h"

#include "config.h"
#include "json.h"
#include "plugins.h"
#include "pipe.h"
#include "raw.h"

#include "utils.h"
#include "transports.h"
#include "log.h"

/* Checking whether the user is in a channel */
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
USERS *seek_user(const char *pubid, const char *linkid, acetables *g_ape)
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

USERS *seek_user_simple(const char *pubid, acetables *g_ape)
{
	transpipe *gpipe;

	gpipe = get_pipe(pubid, g_ape);
	
	if (gpipe == NULL || gpipe->type != USER_PIPE) {
		return NULL;
	}
	
	return gpipe->pipe;
	
}

USERS *seek_user_id(const char *sessid, acetables *g_ape)
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
	
	nuser->cmdqueue = NULL;
	
	nuser->lastping[0] = '\0';
	
	if (nuser->next != NULL) {
		nuser->next->prev = nuser;
	}
	g_ape->uHead = nuser;
	gen_sessid_new(nuser->sessid, g_ape);
	
	return nuser;
}

USERS *adduser(ape_socket *client, const char *host, const char *ip, USERS *allocated, acetables *g_ape)
{
	USERS *nuser = NULL;

	/* Calling module */
	if (allocated == NULL) {
		FIRE_EVENT(allocateuser, nuser, client, host, ip, g_ape);
		
		nuser = init_user(g_ape);
		strncpy(nuser->ip, ip, 16);
		
		nuser->pipe = init_pipe(nuser, USER_PIPE, g_ape);
		nuser->type = (client != NULL ? HUMAN : BOT);
		
		nuser->istmp = 1;
		
		hashtbl_append(g_ape->hSessid, nuser->sessid, (void *)nuser);

		addsubuser(client, host, nuser, g_ape);
	} else {
		FIRE_EVENT(adduser, nuser, allocated, g_ape);
		
		nuser = allocated;
		nuser->istmp = 0;
		
		g_ape->nConnected++;
		
		ape_log(APE_INFO, __FILE__, __LINE__, g_ape, 
			"New user - (ip : %s)", nuser->ip);
	}

	return nuser;
	
}

void deluser(USERS *user, acetables *g_ape)
{

	if (user == NULL) {
		return;
	}

	left_all(user, g_ape);

	FIRE_EVENT_NULL(deluser, user, user->istmp, g_ape);

	/* kill all users connections */
	
	clear_subusers(user, g_ape);

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
	
	/* TODO Add Event */
	free(user);

}

void do_died(subuser *sub)
{
	if (sub->state == ALIVE) {
		sub->state = ADIED;
		sub->headers.sent = 0;
		http_headers_free(sub->headers.content);
		sub->headers.content = NULL;
		
		shutdown(sub->client->fd, 2);
	}
}

void check_timeout(acetables *g_ape, int last)
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
			while (*n != NULL) {
				if ((ctime - (*n)->idle) >= TIMEOUT_SEC) {
					delsubuser(n, g_ape);
					continue;
				}
				if ((*n)->state == ALIVE && (*n)->raw_pools.nraw && !(*n)->need_update) {

					/* Data completetly sent => closed */
					if (send_raws(*n, g_ape)) {
						transport_data_completly_sent(*n, (*n)->user->transport); // todo : hook
					} else {

						(*n)->burn_after_writing = 1;
					}
				} else {
					FIRE_EVENT_NONSTOP(tickuser, *n, g_ape);
				}
				n = &(*n)->next;
			}
		}
		
		list = wait;
	}

}

void send_error(USERS *user, const char *msg, const char *code, acetables *g_ape)
{
	RAW *newraw;
	json_item *jlist = json_new_object();
	
	json_set_property_strZ(jlist, "code", code);
	json_set_property_strZ(jlist, "value", msg);
	
	newraw = forge_raw(RAW_ERR, jlist);
	
	post_raw(newraw, user, g_ape);	
}

void send_msg(USERS *user, const char *msg, const char *type, acetables *g_ape)
{
	RAW *newraw;
	json_item *jlist = json_new_object();
	
	json_set_property_strZ(jlist, "value", msg);
	
	newraw = forge_raw(type, jlist);
	
	post_raw(newraw, user, g_ape);	
}

void send_msg_channel(CHANNEL *chan, const char *msg, const char *type, acetables *g_ape)
{
	RAW *newraw;
	json_item *jlist = json_new_object();
	
	json_set_property_strZ(jlist, "value", msg);
	
	newraw = forge_raw(type, jlist);
	
	post_raw_channel(newraw, chan, g_ape);
}

void send_msg_sub(subuser *sub, const char *msg, const char *type, acetables *g_ape)
{
	RAW *newraw;
	json_item *jlist = json_new_object();
	
	json_set_property_strZ(jlist, "value", msg);
	
	newraw = forge_raw(type, jlist);
	
	post_raw_sub(newraw, sub, g_ape);		
}

session *get_session(USERS *user, const char *key)
{
	session *current = user->sessions.data;
	if (strlen(key) > 32) {
		return NULL;
	}
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

session *set_session(USERS *user, const char *key, const char *val, int update, acetables *g_ape)
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
		//strcpy(sTmp->key, key);
		memcpy(sTmp->val, val, vlen + 1);
		
		if (update) {
			sendback_session(user, sTmp, g_ape);
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
		sendback_session(user, new_session, g_ape);
	}	
	return new_session;
}

void sendback_session(USERS *user, session *sess, acetables *g_ape)
{
	subuser *current = user->subuser;
	
	while (current != NULL) {
		if (current->need_update) {
			json_item *jlist = json_new_object(), *jobj_item = json_new_object();
			RAW *newraw;
			
			current->need_update = 0;
			
			json_set_property_strZ(jobj_item, sess->key, sess->val);
			json_set_property_objN(jlist, "sessions", 8, jobj_item);
			
			newraw = forge_raw("SESSIONS", jlist);
			newraw->priority = RAW_PRI_HI;
			
			post_raw_sub(copy_raw_z(newraw), current, g_ape);
		}
		current = current->next;
	}	
	
}

subuser *addsubuser(ape_socket *client, const char *channel, USERS *user, acetables *g_ape)
{
	subuser *sub;
		
	if (getsubuser(user, channel) != NULL || strlen(channel) > MAX_HOST_LENGTH) {
		return NULL;
	}

	sub = xmalloc(sizeof(*sub));
	sub->client = client;
	sub->state = ADIED;
	sub->user = user;
	
	memcpy(sub->channel, channel, strlen(channel)+1);
	sub->next = user->subuser;
	
	sub->nraw = 0;
	sub->wait_for_free = 0;
	
	sub->properties = NULL;
	
	sub->headers.sent = 0;
	sub->headers.content = NULL;
	
	sub->burn_after_writing = 0;
	
	sub->idle = time(NULL);
	sub->need_update = 0;
	sub->current_chl = 0;

	sub->raw_pools.nraw = 0;
	
	/* Pre-allocate a pool of raw to reduce the number of malloc calls */
	
	/* Low priority raws */
	sub->raw_pools.low.nraw = 0;
	sub->raw_pools.low.size = 32;
	sub->raw_pools.low.rawhead = init_raw_pool(sub->raw_pools.low.size);
	sub->raw_pools.low.rawfoot = sub->raw_pools.low.rawhead;
	
	/* High priority raws */
	sub->raw_pools.high.nraw = 0;
	sub->raw_pools.high.size = 8;
	sub->raw_pools.high.rawhead = init_raw_pool(sub->raw_pools.high.size);
	sub->raw_pools.high.rawfoot = sub->raw_pools.high.rawhead;
	
	(user->nsub)++;
	
	user->subuser = sub;
	
	/* if the previous subuser have some messages in queue, copy them to the new subuser */
	if (sub->next != NULL && sub->next->raw_pools.low.nraw) {
		struct _raw_pool *rTmp;
		for (rTmp = sub->next->raw_pools.low.rawhead; rTmp->raw != NULL; rTmp = rTmp->next) {
			post_raw_sub(copy_raw_z(rTmp->raw), sub, g_ape);
		}

	}
	
	FIRE_EVENT_NONSTOP(addsubuser, sub, g_ape);
	
	return sub;
}

void subuser_restor(subuser *sub, acetables *g_ape)
{
	CHANLIST *chanl;
	CHANNEL *chan;
	
	json_item *jlist;
	RAW *newraw;
	USERS *user = sub->user;
	userslist *ulist;

	chanl = user->chan_foot;

	while (chanl != NULL) {
		jlist = json_new_object();
		
		chan = chanl->chaninfo;
		
		if (!(chan->flags & CHANNEL_NONINTERACTIVE) && chan->head != NULL) {
			json_item *user_list = json_new_array();
			
			ulist = chan->head;
			
			while (ulist != NULL) {
	
				json_item *juser = get_json_object_user(ulist->userinfo);
		
				if (ulist->userinfo != user) {
					//make_link(user, ulist->userinfo);
				}
				
				json_set_property_intN(juser, "level", 5, ulist->level);
				
				json_set_element_obj(user_list, juser);

				ulist = ulist->next;
			}
			
			json_set_property_objN(jlist, "users", 5, user_list);
		}
		json_set_property_objN(jlist, "pipe", 4, get_json_object_channel(chan));

		newraw = forge_raw(RAW_CHANNEL, jlist);
		newraw->priority = RAW_PRI_HI;
		post_raw_sub(newraw, sub, g_ape);
		chanl = chanl->next;
	}

	jlist = json_new_object();
	json_set_property_objN(jlist, "user", 4, get_json_object_user(user));	
	
	newraw = forge_raw("IDENT", jlist);
	newraw->priority = RAW_PRI_HI;
	post_raw_sub(newraw, sub, g_ape);
	
}

subuser *getsubuser(USERS *user, const char *channel)
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

void delsubuser(subuser **current, acetables *g_ape)
{
	subuser *del = *current;
	
	FIRE_EVENT_NONSTOP(delsubuser, del, g_ape);
	((*current)->user->nsub)--;
	
	*current = (*current)->next;	
	
	destroy_raw_pool(del->raw_pools.low.rawhead);
	destroy_raw_pool(del->raw_pools.high.rawhead);
	
	clear_properties(&del->properties);
	
	if (del->state == ALIVE) {
		del->wait_for_free = 1;
		do_died(del);
	} else {
		free(del);
	}
	
}

void clear_subusers(USERS *user, acetables *g_ape)
{
	while (user->subuser != NULL) {
		delsubuser(&(user->subuser), g_ape);
	}
}

#if 0
void ping_request(USERS *user, acetables *g_ape)
{

	struct timeval t;
	gettimeofday(&t, NULL);

	sprintf(user->lastping, "%li%d", t.tv_sec, t.tv_usec);
	
	send_msg(user, user->lastping, "KING", g_ape);	
}
#endif
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

json_item *get_json_object_user(USERS *user)
{
	json_item *jstr = NULL;
	
	if (user != NULL) {
		jstr = json_new_object();
		json_set_property_strN(jstr, "casttype", 8, "uni", 3);
		json_set_property_strN(jstr, "pubid", 5, user->pipe->pubid, 32);
		
		if (user->properties != NULL) {
			int has_prop = 0;
			
			json_item *jprop = NULL;
						
			extend *eTmp = user->properties;
			
			while (eTmp != NULL) {
				if (eTmp->visibility == EXTEND_ISPUBLIC) {
					if (!has_prop) {
						has_prop = 1;
						jprop = json_new_object();
					}
					if (eTmp->type == EXTEND_JSON) {
						json_item *jcopy = json_item_copy(eTmp->val, NULL);
						
						json_set_property_objZ(jprop, eTmp->key, jcopy);
					} else {
						json_set_property_strZ(jprop, eTmp->key, eTmp->val);

					}			
				}
				eTmp = eTmp->next;
			}
			if (has_prop) {
				json_set_property_objN(jstr, "properties", 10, jprop);
			}
		}

	} else {
		json_set_property_strZ(jstr, "pubid", SERVER_NAME);
	}
	return jstr;
}

