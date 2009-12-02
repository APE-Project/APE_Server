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

/* channel.c */

#include "channel.h"
#include "hash.h"
#include "utils.h"
#include "extend.h"
#include "json.h"
#include "raw.h"
#include "plugins.h"

unsigned int isvalidchan(char *name) 
{
	char *pName;
	if (strlen(name) > MAX_CHAN_LEN) {
		return 0;
	}

	for (pName = (*name == '*' ? &name[1] : name ); *pName; pName++) {
		*pName = tolower(*pName);
		if (*pName == '_' || *pName == '|' || *pName == ':' || *pName == '.') {
			continue;
		}
		if (!isalnum(*pName) || ispunct(*pName)) {
			return 0;
		}
	}
	return 1;
}

CHANNEL *mkchan(char *chan, int flags, acetables *g_ape)
{
	CHANNEL *new_chan = NULL;

	FIRE_EVENT(mkchan, new_chan, chan, flags, g_ape);

	if (!isvalidchan(chan)) {
		return NULL;
	}
	
	new_chan = (CHANNEL *) xmalloc(sizeof(*new_chan));
		
	memcpy(new_chan->name, chan, strlen(chan)+1);
	
	new_chan->head = NULL;
	new_chan->banned = NULL;
	new_chan->properties = NULL;
	new_chan->flags = flags | (*new_chan->name == '*' ? CHANNEL_NONINTERACTIVE : 0);

	//memcpy(new_chan->topic, topic, strlen(topic)+1);

	new_chan->pipe = init_pipe(new_chan, CHANNEL_PIPE, g_ape);
	
	hashtbl_append(g_ape->hLusers, chan, (void *)new_chan);
	
	/* just to test */
	//proxy_attach(proxy_init("olol", "localhost", 1337, g_ape), new_chan->pipe->pubid, 0, g_ape);

	return new_chan;
	
}

CHANNEL *getchan(const char *chan, acetables *g_ape)
{
	if (strlen(chan) > MAX_CHAN_LEN) {
		return NULL;
	}
	return (CHANNEL *)hashtbl_seek(g_ape->hLusers, chan);	
}

CHANNEL *getchanbypubid(const char *pubid, acetables *g_ape)
{
	transpipe *gpipe;

	gpipe = get_pipe(pubid, g_ape);
	
	if (gpipe == NULL || gpipe->type != CHANNEL_PIPE) {
		return NULL;
	}
	
	return gpipe->pipe;
	
}

void rmchan(CHANNEL *chan, acetables *g_ape)
{
	if (chan->head != NULL) {
		struct userslist *head = chan->head;
		chan->flags |= CHANNEL_NONINTERACTIVE; /* Force to be non interactive (don't send LEFT raw) */
		chan->flags &= ~CHANNEL_AUTODESTROY; /* Don't destroy it */
		
		while(head != NULL) {
			struct userslist *thead = head->next;
			left(head->userinfo, chan, g_ape);
			head = thead;
		}
	}
	
	FIRE_EVENT_NULL(rmchan, chan, g_ape);
	
	rmallban(chan);
		
	hashtbl_erase(g_ape->hLusers, chan->name);
	
	clear_properties(&chan->properties);
	
	destroy_pipe(chan->pipe, g_ape);
	
	free(chan);
	chan = NULL;
}

void join(USERS *user, CHANNEL *chan, acetables *g_ape)
{
	userslist *list, *ulist;
	RAW *newraw;
	json_item *jlist;
	CHANLIST *chanl;
	
	FIRE_EVENT_NULL(join, user, chan, g_ape);
	
	if (isonchannel(user, chan)) {
		return;
	}
	
	jlist = json_new_object();
	
	list = xmalloc(sizeof(*list)); // TODO is it free ?
	list->userinfo = user;
	list->level = 1;
	list->next = chan->head;
	
	chan->head = list;
	
	chanl = xmalloc(sizeof(*chanl)); // TODO is it free ?
	chanl->chaninfo = chan;
	chanl->next = user->chan_foot;
	
	user->chan_foot = chanl;

	if (!(chan->flags & CHANNEL_NONINTERACTIVE)) {
		
		json_item *user_list;
		json_item *uinfo;
		
		user_list = json_new_array();
		
		if (list->next != NULL) {
			
			uinfo = json_new_object();
		
			json_set_property_objN(uinfo, "user", 4, get_json_object_user(user));
			json_set_property_objN(uinfo, "pipe", 4, get_json_object_channel(chan));

			newraw = forge_raw(RAW_JOIN, uinfo);
			post_raw_channel_restricted(newraw, chan, user, g_ape);
		}
		
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
	post_raw(newraw, user, g_ape);
	
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
	json_item *jlist;
	
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
			if (chan->head != NULL && !(chan->flags & CHANNEL_NONINTERACTIVE)) {
				jlist = json_new_object();
				
				json_set_property_objN(jlist, "user", 4, get_json_object_user(user));
				json_set_property_objN(jlist, "pipe", 4, get_json_object_channel(chan));
				
				newraw = forge_raw(RAW_LEFT, jlist);
				post_raw_channel(newraw, chan, g_ape);
			} else if (chan->head == NULL && chan->flags & CHANNEL_AUTODESTROY) {
				rmchan(chan, g_ape);
			}
			break;
		}
		prev = list;
		list = list->next;
	}
	
}

userslist *getlist(const char *chan, acetables *g_ape)
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
unsigned int setlevel(USERS *user_actif, USERS *user_passif, CHANNEL *chan, unsigned int lvl, acetables *g_ape)
{
	RAW *newraw;
	userslist *user_passif_chan, *user_actif_chan;
	json_item *jlist;

	user_passif_chan = getuchan(user_passif, chan);
	
	if (user_actif != NULL) {
		user_actif_chan = getuchan(user_actif, chan);
		
		if (user_passif_chan == NULL || user_actif_chan == NULL || ((user_actif_chan->level < lvl || user_actif_chan->level < user_passif_chan->level) && !(user_actif->flags & FLG_AUTOOP)) || lvl < 1 || lvl > 32) {
			send_error(user_actif, "SETLEVEL_ERROR", "110", g_ape);
		
			return 0;
		}
		
		user_passif_chan->level = lvl;
		
		if (!(chan->flags & CHANNEL_NONINTERACTIVE)) {
			jlist = json_new_object();
			
			json_set_property_objN(jlist, "ope", 3, get_json_object_user(user_passif));
			json_set_property_objN(jlist, "oper", 4, get_json_object_user(user_actif));
			json_set_property_objN(jlist, "pipe", 4, get_json_object_channel(chan));
			json_set_property_intN(jlist, "level", 5, lvl);
		
			newraw = forge_raw(RAW_SETLEVEL, jlist);
			post_raw_channel(newraw, chan, g_ape);
		}
		return 1;
	} else if (user_passif_chan != NULL && lvl > 0 && lvl < 32) {		
		user_passif_chan->level = lvl;
		
		if (!(chan->flags & CHANNEL_NONINTERACTIVE)) {
			jlist = json_new_object();

			json_set_property_objN(jlist, "ope", 3, get_json_object_user(user_passif));
			json_set_property_objN(jlist, "oper", 4, get_json_object_user(NULL));
			json_set_property_objN(jlist, "pipe", 4, get_json_object_channel(chan));
			json_set_property_intN(jlist, "level", 5, lvl);

			newraw = forge_raw(RAW_SETLEVEL, jlist);
			post_raw_channel(newraw, chan, g_ape);
			
		}
		return 1;
	}
	return 0;
}

/*unsigned int settopic(USERS *user, CHANNEL *chan, const char *topic, acetables *g_ape)
{
	RAW *newraw;
	userslist *list;
	json_item *jlist;
	
	list = getuchan(user, chan);
	
	if (list == NULL || list->level < 3 || strlen(topic)+1 > MAX_TOPIC_LEN) {
		
		send_error(user, "SETTOPIC_ERROR", "111", g_ape);
		
	} else {
		memcpy(chan->topic, topic, strlen(topic)+1);
		
		jlist = json_new_object();
		json_set_property_objN(jlist, "user", 4, get_json_object_user(user));
		json_set_property_objN(jlist, "pipe", 4, get_json_object_channel(chan));
		
		newraw = forge_raw(RAW_SETTOPIC, jlist);
		post_raw_channel(newraw, chan, g_ape);
		
		return 1;
	}
	return 0;
}*/

void ban(CHANNEL *chan, USERS *banner, const char *ip, char *reason, unsigned int expire, acetables *g_ape) // Ban IP
{
	userslist *uTmp, *tUtmp;
	RAW *newraw;
	json_item *jlist;
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
			jlist = json_new_object();
			
			json_set_property_strZ(jlist, "reason", reason);
			json_set_property_objN(jlist, "banner", 6, get_json_object_user(banner));
			json_set_property_objN(jlist, "pipe", 4, get_json_object_channel(chan));
			
			newraw = forge_raw(RAW_BAN, jlist);
			
			post_raw(newraw, uTmp->userinfo, g_ape);
			
			if (isban == 0) {
				blist = xmalloc(sizeof(*blist));
				
				memset(blist->reason, 0, 256);
				strncpy(blist->ip, ip, 16);
				strncpy(blist->reason, reason, 255);
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

BANNED *getban(CHANNEL *chan, const char *ip)
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

void rmban(CHANNEL *chan, const char *ip)
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

json_item *get_json_object_channel(CHANNEL *chan)
{
	json_item *jstr = json_new_object();
	json_set_property_strN(jstr, "casttype", 8, "multi", 5);
	json_set_property_strN(jstr, "pubid", 5, chan->pipe->pubid, 32);

	json_item *jprop = json_new_object();
	json_set_property_strZ(jprop, "name", chan->name);

	extend *eTmp = chan->properties;
	
	while (eTmp != NULL) {
		if (eTmp->visibility == EXTEND_ISPUBLIC) {
			if (eTmp->type == EXTEND_JSON) {
				json_item *jcopy = json_item_copy(eTmp->val, NULL);
				
				json_set_property_objZ(jprop, eTmp->key, jcopy);
			} else {
				json_set_property_strZ(jprop, eTmp->key, eTmp->val);
			}
		}
		
		eTmp = eTmp->next;
	}
	json_set_property_objN(jstr, "properties", 10, jprop);
	
	//}
	
	return jstr;
}

