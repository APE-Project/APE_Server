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

/* channel.c */

#include "main.h"
#include "sock.h"
#include "raw.h"
#include "channel.h"
#include "plugins.h"
#include "handle_http.h"
#include "utils.h"
#include "proxy.h"

unsigned int isvalidchan(char *name) 
{
	char *pName;
	if (strlen(name) > MAX_CHAN_LEN) {
		return 0;
	}

	for (pName = (*name == '*' ? &name[1] : name ); *pName; pName++) {
		*pName = tolower(*pName);
		if (!isalnum(*pName) || ispunct(*pName)) {
			return 0;
		}
	}
	return 1;
}

CHANNEL *mkchan(char *chan, char *topic, acetables *g_ape)
{
	CHANNEL *new_chan = NULL;


	FIRE_EVENT(mkchan, new_chan, chan, topic, g_ape);

	
	if (!isvalidchan(chan)) {
		return NULL;
	}
	
	new_chan = (CHANNEL *) xmalloc(sizeof(*new_chan));
		
	memcpy(new_chan->name, chan, strlen(chan)+1);
	
	new_chan->head = NULL;
	new_chan->banned = NULL;
	new_chan->properties = NULL;
	
	new_chan->interactive = (*new_chan->name == '*' ? 0 : 1);

	memcpy(new_chan->topic, topic, strlen(topic)+1);

	new_chan->pipe = init_pipe(new_chan, CHANNEL_PIPE, g_ape);
	
	hashtbl_append(g_ape->hLusers, chan, (void *)new_chan);
	
	/* just to test */
	//proxy_attach(proxy_init("olol", "localhost", 1337, g_ape), new_chan->pipe->pubid, 0, g_ape);
	
	return new_chan;
	
}

CHANNEL *getchan(char *chan, acetables *g_ape)
{
	if (strlen(chan) > MAX_CHAN_LEN) {
		return NULL;
	}
	return (CHANNEL *)hashtbl_seek(g_ape->hLusers, chan);	
}
void rmchan(CHANNEL *chan, acetables *g_ape)
{

	if (chan->head != NULL) {
		return;
	}
	rmallban(chan);
	
	
	hashtbl_erase(g_ape->hPubid, chan->pipe->pubid);
	free(chan->pipe);
		
	hashtbl_erase(g_ape->hLusers, chan->name);
	
	clear_properties(&chan->properties);
	
	free(chan);
	chan = NULL;
}
void join(USERS *user, CHANNEL *chan, acetables *g_ape)
{
	userslist *list, *ulist;
	
	CHANLIST *chanl;
	
	FIRE_EVENT_NULL(join, user, chan, g_ape);
	
	RAW *newraw;
	json *jlist = NULL;
	char level[8];
	
	if (isonchannel(user, chan)) {
		return;
	}
	
	list = (userslist *)xmalloc(sizeof(*list)); // is it free ?
	list->userinfo = user;
	list->level = 1;
	list->next = chan->head;
	
	chan->head = list;
	
	chanl = (CHANLIST *)xmalloc(sizeof(*chanl)); // is it free ?
	chanl->chaninfo = chan;
	chanl->next = user->chan_foot;
	
	user->chan_foot = chanl;


	if (chan->interactive) {
		jlist = NULL;
		
		set_json("user", NULL, &jlist);
		json_attach(jlist, get_json_object_user(user), JSON_OBJECT);
		
		set_json("pipe", NULL, &jlist);
		json_attach(jlist, get_json_object_channel(chan), JSON_OBJECT);
		
		newraw = forge_raw(RAW_JOIN, jlist);
		post_raw_channel_restricted(newraw, chan, user);
	
		jlist = NULL;
		set_json("users", NULL, &jlist);

		ulist = chan->head;
		while (ulist != NULL) {
		
			struct json *juser = NULL;
			
			if (ulist->userinfo != user) {
				//make_link(user, ulist->userinfo);
			}
			
			sprintf(level, "%i", ulist->level);
			set_json("level", level, &juser);
			
			json_concat(juser, get_json_object_user(ulist->userinfo));
		
			json_attach(jlist, juser, JSON_ARRAY);

			ulist = ulist->next;
		}
	}

	
	set_json("pipe", NULL, &jlist);
	json_attach(jlist, get_json_object_channel(chan), JSON_OBJECT);
	
	newraw = forge_raw(RAW_CHANNEL, jlist);
	post_raw(newraw, user);
	
	#if 0
	if (user->flags & FLG_AUTOOP) {
		setlevel(NULL, user, chan, 3);
	}
	#endif

}

void left_all(USERS *user, acetables *g_ape)
{
	CHANLIST *list, *tList;
	
	if (user == NULL) {
		return;
	}
	
	list = user->chan_foot;
	
	while (list != NULL) {
		tList = list->next;

		left(user, list->chaninfo, g_ape);

		list = tList;
	}
}

void left(USERS *user, CHANNEL *chan, acetables *g_ape) // Vider la liste chainée de l'user
{
	userslist *list, *prev;

	CHANLIST *clist, *ctmp;
	RAW *newraw;
	json *jlist;
	
	FIRE_EVENT_NULL(left, user, chan, g_ape);
	
	if (!isonchannel(user, chan)) {
		return;
	}
	list = chan->head;
	prev = NULL;
	
	clist = user->chan_foot;
	ctmp = NULL;
	
	while (clist != NULL) {
		if (clist->chaninfo == chan) {
			if (ctmp != NULL) {
				ctmp->next = clist->next;
			} else {
				user->chan_foot = clist->next;
			}
			free(clist);
			clist = NULL;
			break;
		}
		ctmp = clist;
		clist = clist->next;
	}
	
	
	while (list != NULL && list->userinfo != NULL) {
		if (list->userinfo == user) {
			if (prev != NULL) {
				prev->next = list->next;
			} else {
				chan->head = list->next;
			}
			free(list);
			list = NULL;
			if (chan->head != NULL && chan->interactive) {
				jlist = NULL;
				
				set_json("user",  NULL, &jlist);
				json_attach(jlist, get_json_object_user(user), JSON_OBJECT);

				
				set_json("pipe", NULL, &jlist);
				json_attach(jlist, get_json_object_channel(chan), JSON_OBJECT);
				
				newraw = forge_raw(RAW_LEFT, jlist);
				post_raw_channel(newraw, chan);
			} else if (chan->head == NULL) {
				rmchan(chan, g_ape); // A verifier
			}
			break;
		}
		prev = list;
		list = list->next;
	}
	
}
userslist *getlist(char *chan, acetables *g_ape)
{
	CHANNEL *lchan;
	
	if (strlen(chan) > MAX_CHAN_LEN) {
		return NULL;
	}
	if ((lchan = (CHANNEL *)hashtbl_seek(g_ape->hLusers, chan)) == NULL) {
		return NULL;
	}
	return lchan->head;
}

/* get user info to a specific channel (i.e. level) */
userslist *getuchan(USERS *user, CHANNEL *chan)
{
	userslist *list;
	
	if (user == NULL || chan == NULL) {
		return 0;
	}
	list = chan->head;
	
	while (list != NULL) {
		if (list->userinfo == user) {
			return list;
		}
		list = list->next;
	}
	return NULL;
}

// TODO : Rewrite this f***g ugly function
unsigned int setlevel(USERS *user_actif, USERS *user_passif, CHANNEL *chan, unsigned int lvl)
{
	RAW *newraw;
	userslist *user_passif_chan, *user_actif_chan;
	json *jlist;

	char level[8];
	
	user_passif_chan = getuchan(user_passif, chan);
	
	if (user_actif != NULL) {
		user_actif_chan = getuchan(user_actif, chan);
		
		if (user_passif_chan == NULL || user_actif_chan == NULL || ((user_actif_chan->level < lvl || user_actif_chan->level < user_passif_chan->level) && !(user_actif->flags & FLG_AUTOOP)) || lvl < 1 || lvl > 32) {
			send_error(user_actif, "SETLEVEL_ERROR");
		
			return 0;
		}
		
		user_passif_chan->level = lvl;
		
		if (chan->interactive) {
			jlist = NULL;

			set_json("ope", NULL, &jlist);
			json_attach(jlist, get_json_object_user(user_passif), JSON_OBJECT);
		
			set_json("opeur", NULL, &jlist);
			json_attach(jlist, get_json_object_user(user_actif), JSON_OBJECT);
		
			set_json("level", itos(lvl, level), &jlist);
			set_json("channel", NULL, &jlist);
			json_attach(jlist, get_json_object_channel(chan), JSON_OBJECT);
		
			newraw = forge_raw(RAW_SETLEVEL, jlist);
			post_raw_channel(newraw, chan);
		}
		return 1;
	} else if (user_passif_chan != NULL && lvl > 0 && lvl < 32) {		
		user_passif_chan->level = lvl;
		
		if (chan->interactive) {
			jlist = NULL;
		
			set_json("ope", NULL, &jlist);
			json_attach(jlist, get_json_object_user(user_passif), JSON_OBJECT);
		
			set_json("opeur", NULL, &jlist);
			json_attach(jlist, get_json_object_user(NULL), JSON_OBJECT);
		
			set_json("level", itos(lvl, level), &jlist);
			set_json("channel", NULL, &jlist);
			json_attach(jlist, get_json_object_channel(chan), JSON_OBJECT);
		
			newraw = forge_raw(RAW_SETLEVEL, jlist);
			post_raw_channel(newraw, chan);
			
		}
		return 1;
	}
	return 0;
}
unsigned int settopic(USERS *user, CHANNEL *chan, char *topic)
{
	RAW *newraw;
	userslist *list;
	json *jlist;
	
	list = getuchan(user, chan);
	
	if (list == NULL || list->level < 3 || strlen(topic)+1 > MAX_TOPIC_LEN) {
		
		send_error(user, "SETTOPIC_ERROR");
		
	} else {
		memcpy(chan->topic, topic, strlen(topic)+1);
		
		jlist = NULL;
		
		set_json("user", NULL, &jlist);
		json_attach(jlist, get_json_object_user(user), JSON_OBJECT);
		
		set_json("channel", NULL, &jlist);
		json_attach(jlist, get_json_object_channel(chan), JSON_OBJECT);
		
		newraw = forge_raw(RAW_SETTOPIC, jlist);
		post_raw_channel(newraw, chan);
		
		return 1;
	}
	return 0;
}

void ban(CHANNEL *chan, USERS *banner, char *ip, char *reason, unsigned int expire, acetables *g_ape) // Ban IP
{
	userslist *uTmp, *tUtmp;
	RAW *newraw;
	json *jlist;
	BANNED *blist, *bTmp;
	
	unsigned int isban = 0;
	
	long int nextime = (expire * 60)+time(NULL); // NOW !
	
	if (chan == NULL) {
		return;
	}
	
	uTmp = chan->head;
	bTmp = chan->banned;
	
	while (uTmp != NULL) {

		if (strcmp(ip, uTmp->userinfo->ip) == 0) { // We find somebody with the same IP
			jlist = NULL;
			
			set_json("reason", reason, &jlist);
			if (banner != NULL) {
				set_json("banner", NULL, &jlist);
				json_attach(jlist, get_json_object_user(banner), JSON_OBJECT);
			} else {
				set_json("banner", NULL, &jlist);
				json_attach(jlist, get_json_object_user(NULL), JSON_OBJECT);
			}
			set_json("channel", NULL, &jlist);
			json_attach(jlist, get_json_object_channel(chan), JSON_OBJECT);
			
			newraw = forge_raw(RAW_BAN, jlist);
			
			post_raw(newraw, uTmp->userinfo);
			
			if (isban == 0) {
				blist = xmalloc(sizeof(*blist));
				
				strncpy(blist->ip, ip, 16);
				strncpy(blist->reason, reason, 256);
				blist->expire = nextime;
				blist->next = bTmp;
				chan->banned = blist;
				isban = 1;
			}
			tUtmp = uTmp->next;
			left(uTmp->userinfo, chan, g_ape); // if the user is the last : "chan" is free (rmchan())
			uTmp = tUtmp;
			continue;
		}
		uTmp = uTmp->next;
	}

}

BANNED *getban(CHANNEL *chan, char *ip)
{
	BANNED *blist, *bTmp, *bWait;
	
	blist = chan->banned;
	bTmp = NULL;
	
	while (blist != NULL) {
		if (blist->expire < time(NULL)) {
			bWait = blist->next;
			free(blist);
			blist = bWait;
			
			if (bTmp == NULL) {
				chan->banned = blist;
			} else {
				bTmp->next = blist;
			}			
			continue;
		} else if (strcmp(blist->ip, ip) == 0) {
			return blist;
		}
		bTmp = blist;
		blist = blist->next;
	}

	return NULL;
}

void rmban(CHANNEL *chan, char *ip)
{
	BANNED *blist, *bTmp, *bWait;
	
	blist = chan->banned;
	bTmp = NULL;
	
	while (blist != NULL) {
		if (blist->expire < time(NULL) || strcmp(blist->ip, ip) == 0) {
			bWait = blist->next;
			free(blist);
			blist = bWait;
			
			if (bTmp == NULL) {
				chan->banned = blist;
			} else {
				bTmp->next = blist;
			}			
			continue;
		}
		
		bTmp = blist;
		blist = blist->next;
	}
}

void rmallban(CHANNEL *chan)
{
	BANNED *blist, *bTmp;
	
	blist = chan->banned;
	
	while (blist != NULL) {
		bTmp = blist->next;
		free(blist);
		blist = bTmp;
	}
	chan->banned = NULL;
}

struct json *get_json_object_channel(CHANNEL *chan)
{
	json *jstr = NULL;
	
	//set_json("topic", chan->topic, &jstr);
	//set_json("name", chan->name, &jstr); // See below
	set_json("pubid", chan->pipe->pubid, &jstr);
	
	
	//if (chan->properties != NULL) {
	json *jprop = NULL;
	set_json("properties", NULL, &jstr);
	
	extend *eTmp = chan->properties;
	
	while (eTmp != NULL) {
		set_json(eTmp->key, eTmp->val, &jprop);
		eTmp = eTmp->next;
	}
	/* a little hack to have the same behaviour than user */
	set_json("name", chan->name, &jprop);
	json_attach(jstr, jprop, JSON_OBJECT);
	//}
	
	return jstr;
}

