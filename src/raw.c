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

/* raw.c */

#include "raw.h"
#include "hash.h"
#include "json.h"
#include "plugins.h"
#include "config.h"
#include "utils.h"

void do_register(acetables *g_ape) // register_raw("RAW", Nparam (without IP and time, with sessid), callback_func, NEEDSOMETHING?, g_ape);
{
	register_raw("CONNECT",		0, raw_connect, 	NEED_NOTHING, g_ape);
	register_raw("PCONNECT",	1, raw_pconnect, 	NEED_NOTHING, g_ape);
	register_raw("SCRIPT",    	-1, raw_script,		NEED_NOTHING, g_ape);
	
	register_raw("CHECK", 		1, raw_check, 		NEED_SESSID, g_ape);
	register_raw("SEND", 		3, raw_send, 		NEED_SESSID, g_ape);

	register_raw("QUIT", 		1, raw_quit, 		NEED_SESSID, g_ape);
	register_raw("SETLEVEL", 	4, raw_setlevel, 	NEED_SESSID, g_ape); // Module
	register_raw("SETTOPIC", 	3, raw_settopic, 	NEED_SESSID, g_ape); // Module
	register_raw("JOIN", 		2, raw_join, 		NEED_SESSID, g_ape);
	register_raw("LEFT", 		2, raw_left, 		NEED_SESSID, g_ape);
	register_raw("KICK", 		3, raw_kick, 		NEED_SESSID, g_ape); // Module
	register_raw("BAN",		5, raw_ban,		NEED_SESSID, g_ape); // Module
	register_raw("SESSION",   	-3, raw_session,	NEED_SESSID, g_ape);
	
	register_raw("KONG", 		2, raw_pong, 		NEED_SESSID, g_ape);
}

void register_raw(char *raw, int nParam, unsigned int (*func)(callbackp *), unsigned int need, acetables *g_ape)
{
	callback *new_raw, *old_raw;
	
	new_raw = (callback *) xmalloc(sizeof(*new_raw));
	
	new_raw->nParam = nParam;
	new_raw->func = func;
	new_raw->need = need;
	
	/* Unregister old raw if exists */
	if ((old_raw = (callback *)hashtbl_seek(g_ape->hCallback, raw)) != NULL) {
		hashtbl_erase(g_ape->hCallback, raw);
	}
	
	hashtbl_append(g_ape->hCallback, raw, (void *)new_raw);
	
}

void unregister_raw(char *raw, acetables *g_ape)
{
	hashtbl_erase(g_ape->hCallback, raw);
}

unsigned int checkraw(clientget *cget, subuser **iuser, acetables *g_ape)
{
	char *param[64+1], *raw;
	callback *rawback;
	
	size_t nTok;
	
	unsigned int flag;
	
	USERS *guser = NULL;
	subuser *sub = NULL;
	

	nTok = explode('&', cget->get, param, 64);
	
	if (nTok < 1) {
		raw = NULL;
	} else {
		raw = param[0];

	}

	rawback = (callback *)hashtbl_seek(g_ape->hCallback, raw);
	if (rawback != NULL) { // Cool 27/12/2006 00:00 j'ai 20 ans xD
		if ((nTok-1) == rawback->nParam || (rawback->nParam < 0 && (nTok-1) >= (rawback->nParam*-1) && (rawback->nParam*-1) <= 16)) {
			callbackp cp;
			switch(rawback->need) {
				case NEED_SESSID:
					guser = seek_user_id(param[1], g_ape);
					break;
				case NEED_NOTHING:
					guser = NULL;
					break;
			}
			
			if (rawback->need != NEED_NOTHING) {
				if (guser == NULL) {
					ENVOI(cget->fdclient, ERR_BAD_SESSID);
					
					return (CONNECT_SHUTDOWN);
				} else {
					sub = getsubuser(guser, cget->host);
					if (sub != NULL && sub->fd != cget->fdclient && sub->state == ALIVE) {
						CLOSE(sub->fd);
						shutdown(sub->fd, 2);
						sub->state = ADIED;
					} else if (sub == NULL) {
						sub = addsubuser(cget->fdclient, cget->host, guser);
					}
					guser->idle = (long int)time(NULL); // update user idle
					sub->idle = guser->idle; // Update subuser idle
					sub->fd = cget->fdclient;
					
				}
			}
			cp.param = param;
			cp.fdclient = cget->fdclient;
			cp.call_user = guser,
			cp.g_ape = g_ape;
			cp.nParam = nTok-1;
			cp.host = cget->host;
			
			flag = rawback->func(&cp);
			
			if (flag & FOR_NULL) {
				guser = NULL;
			} else if (flag & FOR_LOGIN) {
				guser = cp.call_user;
			} 
			
			if (guser != NULL) {
				
				if (sub == NULL && (sub = getsubuser(guser, cget->host)) == NULL) {
					
					sub = addsubuser(cget->fdclient, cget->host, guser);
					
				}
				if (sub == NULL) {
					
					return (CONNECT_SHUTDOWN);
				}
				*iuser = sub;
				if (flag & FOR_UPDATE_IP) {
					/*
						TODO : Fix IP management
					*/
					if (strcmp(cget->ip_client, "127.0.0.1") == 0) { // Becareful, a local user can spoof his ip address
						strncpy(guser->ip, cget->ip_get, 16);
					} else {
						strncpy(guser->ip, cget->ip_client, 16); // never trust foreign data
					}
				}
				if ((guser->flags & FLG_PCONNECT)) {
					sendf(sub->fd, "%s", HEADER);
				}

				sub->state = ALIVE;
				return (CONNECT_KEEPALIVE);
				
			}
			return (CONNECT_SHUTDOWN);
		} else {

			ENVOI(cget->fdclient, ERR_BAD_PARAM);
		}
	} else { // unregistered Raw
		ENVOI(cget->fdclient, ERR_BAD_RAW);
	}
	return (CONNECT_SHUTDOWN);
}

unsigned int raw_connect(callbackp *callbacki)
{
	USERS *nuser;
	RAW *newraw;
	struct json *jstr = NULL;

	nuser = adduser(callbacki->fdclient, callbacki->host, callbacki->g_ape);
	
	callbacki->call_user = nuser;
	
	if (nuser == NULL) {
		ENVOI(callbacki->fdclient, ERR_CONNECT);
		
		return (FOR_NOTHING);
	}
	set_json("sessid", nuser->sessid, &jstr);
	set_json("user", NULL, &jstr);
	
	json_attach(jstr, get_json_object_user(nuser), JSON_OBJECT);	
	
	newraw = forge_raw(RAW_LOGIN, jstr);

	post_raw(newraw, nuser);

	
	return (FOR_LOGIN | FOR_UPDATE_IP);

}
/* Deprecated */
unsigned int raw_pconnect(callbackp *callbacki)
{
	USERS *nuser;

	nuser = adduser(callbacki->fdclient, callbacki->host, callbacki->g_ape);
	if (nuser == NULL) {
		ENVOI(callbacki->fdclient, ERR_CONNECT);
		
		return (FOR_NOTHING);
	}
	nuser->flags |= FLG_PCONNECT;
	
	return (FOR_LOGIN | FOR_UPDATE_IP);

}

unsigned int raw_script(callbackp *callbacki)
{
	char *domain = CONFIG_VAL(Server, domain, callbacki->g_ape->srv);
	if (domain == NULL) {
		send_error(callbacki->call_user, "NO_DOMAIN");
	} else {
		int i;
		sendf(callbacki->fdclient, "%s<html>\n<head>\n\t<script>\n\t\tdocument.domain=\"%s\"\n\t</script>\n", HEADER, domain);
		for (i = 1; i <= callbacki->nParam; i++) {
			sendf(callbacki->fdclient, "\t<script type=\"text/javascript\" src=\"%s\"></script>\n", callbacki->param[i]);
		}
		sendf(callbacki->fdclient, "</head>\n<body>\n</body>\n</html>");
	}
	return (FOR_NOTHING);
}

unsigned int raw_join(callbackp *callbacki)
{
	CHANNEL *jchan;
	RAW *newraw;
	json *jlist;
	BANNED *blist;
	
	if ((jchan = getchan(callbacki->param[2], callbacki->g_ape)) == NULL) {
		jchan = mkchan(callbacki->param[2], "Default%20Topic", callbacki->g_ape);
		
		if (jchan == NULL) {
			
			send_error(callbacki->call_user, "CANT_JOIN_CHANNEL");
			
		} else {
		
			join(callbacki->call_user, jchan, callbacki->g_ape);
		}
	
	} else if (isonchannel(callbacki->call_user, jchan)) {
		
		send_error(callbacki->call_user, "ALREADY_ON_CHANNEL");

	} else {
		blist = getban(jchan, callbacki->call_user->ip);
		if (blist != NULL) {
			jlist = NULL;
			
			set_json("reason", blist->reason, &jlist);
			set_json("error", "YOU_ARE_BANNED", &jlist);
			/*
				TODO: Add Until
			*/
			newraw = forge_raw(RAW_ERR, jlist);
			
			post_raw(newraw, callbacki->call_user);
		} else {
			join(callbacki->call_user, jchan, callbacki->g_ape);
		}
	}
	return (FOR_NOTHING);
}

unsigned int raw_check(callbackp *callbacki)
{
	return (FOR_NOTHING);
}

unsigned int raw_send(callbackp *callbacki)
{
	json *jlist = NULL;

	set_json("msg", callbacki->param[3], &jlist);

	post_to_pipe(jlist, RAW_DATA, callbacki->param[2], getsubuser(callbacki->call_user, callbacki->host), NULL, callbacki->g_ape);
	
	return (FOR_NOTHING);
}
unsigned int raw_quit(callbackp *callbacki)
{
	QUIT(callbacki->fdclient);
	deluser(callbacki->call_user, callbacki->g_ape); // After that callbacki->call_user is free'd
	
	return (FOR_NULL);
}

unsigned int raw_setlevel(callbackp *callbacki)
{
	USERS *recver;
	
	if ((recver = seek_user(callbacki->param[3], callbacki->param[2], callbacki->g_ape)) == NULL) {
		send_error(callbacki->call_user, "UNKNOWN_USER");
	} else {
		setlevel(callbacki->call_user, recver, getchan(callbacki->param[2], callbacki->g_ape), atoi(callbacki->param[4]));
	}
	return (FOR_NOTHING);
}


unsigned int raw_left(callbackp *callbacki)
{
	CHANNEL *chan;

		
	if ((chan = getchan(callbacki->param[2], callbacki->g_ape)) == NULL) {
		send_error(callbacki->call_user, "UNKNOWN_CHANNEL");
		
	} else if (!isonchannel(callbacki->call_user, chan)) {
		send_error(callbacki->call_user, "NOT_IN_CHANNEL");
	
	} else {
	
		left(callbacki->call_user, chan, callbacki->g_ape);
	}
	
	return (FOR_NOTHING);
}

unsigned int raw_settopic(callbackp *callbacki)
{
	settopic(callbacki->call_user, getchan(callbacki->param[2], callbacki->g_ape), callbacki->param[3]);
	
	return (FOR_NOTHING);
}

unsigned int raw_kick(callbackp *callbacki)
{
	CHANNEL *chan;
	RAW *newraw;
	json *jlist;
	
	USERS *victim;

	if ((chan = getchan(callbacki->param[2], callbacki->g_ape)) == NULL) {
		send_error(callbacki->call_user, "UNKNOWN_CHANNEL");
		
	} else if (!isonchannel(callbacki->call_user, chan)) {
		send_error(callbacki->call_user, "NOT_IN_CHANNEL");
		
	} else if (getuchan(callbacki->call_user, chan)->level < 3) {
		send_error(callbacki->call_user, "CANT_KICK");
		
	} else {
		victim = seek_user(callbacki->param[3], chan->name, callbacki->g_ape);
		
		if (victim == NULL) {

			send_error(callbacki->call_user, "UNKNOWN_USER");
		} else if (victim->flags & FLG_NOKICK) {
			
			send_error(callbacki->call_user, "USER_PROTECTED");
			// haha ;-)
			
			jlist = NULL;
			set_json("kicker", NULL, &jlist);
			json_attach(jlist, get_json_object_user(callbacki->call_user), JSON_OBJECT);
			
			set_json("channel", NULL, &jlist);
			json_attach(jlist, get_json_object_channel(chan), JSON_OBJECT);
						
			newraw = forge_raw("TRY_KICK", jlist);
			
			post_raw(newraw, victim);
			
		} else {
			jlist = NULL;
			
			set_json("kicker", NULL, &jlist);
			json_attach(jlist, get_json_object_user(callbacki->call_user), JSON_OBJECT);
			
			set_json("channel", NULL, &jlist);
			json_attach(jlist, get_json_object_channel(chan), JSON_OBJECT);
						
			newraw = forge_raw(RAW_KICK, jlist);
			
			post_raw(newraw, victim);
			
			left(victim, chan, callbacki->g_ape); // chan may be removed
			
		}
		
	}
	
	return (FOR_NOTHING);
	
}

unsigned int raw_ban(callbackp *callbacki)
{
	CHANNEL *chan;
	RAW *newraw;
	json *jlist;
	
	USERS *victim;

	if ((chan = getchan(callbacki->param[2], callbacki->g_ape)) == NULL) {
		send_error(callbacki->call_user, "UNKNOWN_CHANNEL");
		
	
	} else if (!isonchannel(callbacki->call_user, chan)) {

		send_error(callbacki->call_user, "NOT_IN_CHANNEL");
	
	} else if (getuchan(callbacki->call_user, chan)->level < 3) {

		
		send_error(callbacki->call_user, "CANT_BAN");
		
	} else {
		victim = seek_user(callbacki->param[3], chan->name, callbacki->g_ape);
		
		if (victim == NULL) {

			send_error(callbacki->call_user, "UNKNOWN_USER");
			
		} else if (victim->flags & FLG_NOKICK) {
			send_error(callbacki->call_user, "USER_PROTECTED");
			
			// Bad boy :-)
			jlist = NULL;
			set_json("banner", NULL, &jlist);
			json_attach(jlist, get_json_object_user(callbacki->call_user), JSON_OBJECT);
			
			set_json("channel", NULL, &jlist);
			json_attach(jlist, get_json_object_channel(chan), JSON_OBJECT);
						
			newraw = forge_raw("TRY_BAN", jlist);
			
			post_raw(newraw, victim);
			
		} else if (strlen(callbacki->param[4]) > 255 || atoi(callbacki->param[5]) > 44640) { // 31 days max

			send_error(callbacki->call_user, "REASON_OR_TIME_TOO_LONG");
		} else {
			ban(chan, callbacki->call_user, victim->ip, callbacki->param[4], atoi(callbacki->param[5]), callbacki->g_ape);
		}
		
	}
	
	return (FOR_NOTHING);
}

unsigned int raw_session(callbackp *callbacki)
{
	if (strcmp(callbacki->param[2], "set") == 0 && (callbacki->nParam == 4 || callbacki->nParam == 5)) {
		int shutdown = 1;
		if (callbacki->nParam == 5) {
			shutdown = 0;
			subuser *tmpSub = getsubuser(callbacki->call_user, callbacki->host);
		
			if (tmpSub != NULL) {
				tmpSub->need_update = 0;
			}
		}
		if (set_session(callbacki->call_user, callbacki->param[3], callbacki->param[4], (callbacki->nParam == 4 ? 0 : 1)) == NULL) {
			send_error(callbacki->call_user, "SESSION_ERROR");
		} else if (shutdown) {
			/* little hack to closing the connection (webkit bug) */
			send_msg_sub(getsubuser(callbacki->call_user, callbacki->host), "close", "close");
		}
	} else if (strcmp(callbacki->param[2], "get") == 0 && callbacki->nParam >= 3) {
		int i;
		json *jlist = NULL, *jobj = NULL;
		RAW *newraw;
		
		set_json("sessions", NULL, &jlist);
		
		for (i = 3; i <= callbacki->nParam; i++) {
			if (strlen(callbacki->param[i]) > 32) {
				continue;
			}
			session *sTmp = get_session(callbacki->call_user, callbacki->param[i]);

			set_json(callbacki->param[i], (sTmp != NULL ? sTmp->val : NULL), &jobj);

		}
		json_attach(jlist, jobj, JSON_OBJECT);
		newraw = forge_raw("SESSIONS", jlist);
		newraw->priority = 1;
		/* Only sending to current subuser */
		post_raw_sub(newraw, getsubuser(callbacki->call_user, callbacki->host));

	} else {
		send_error(callbacki->call_user, "SESSION_ERROR_PARAMS");
	}
	return (FOR_NOTHING);
}

/* This is usefull to ask all subuser to update their sessions */
unsigned int raw_pong(callbackp *callbacki)
{
	if (strcmp(callbacki->param[2], callbacki->call_user->lastping) == 0) {
		RAW *newraw;
				
		callbacki->call_user->lastping[0] = '\0';

		json *jlist = NULL;
	
		set_json("value", callbacki->param[2], &jlist);
	
		newraw = forge_raw("UPDATE", jlist);
	
		post_raw_sub(newraw, getsubuser(callbacki->call_user, callbacki->host));
	}
	return (FOR_NOTHING);
}

