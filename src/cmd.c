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

/* cmd.c */

#include "cmd.h"
#include "json.h"
#include "config.h"
#include "utils.h"
#include "proxy.h"
#include "raw.h"
#include "transports.h"

void do_register(acetables *g_ape) // register_raw("CMD", Nparam (without IP and time, with sessid), callback_func, NEEDSOMETHING?, g_ape);
{
	register_cmd("CONNECT",		cmd_connect, 	NEED_NOTHING, g_ape);
	register_cmd("SCRIPT",        	cmd_script,		NEED_NOTHING, g_ape);
	
	register_cmd("CHECK", 		cmd_check, 		NEED_SESSID, g_ape);
	register_cmd("SEND", 		cmd_send, 		NEED_SESSID, g_ape);

	register_cmd("QUIT", 		cmd_quit, 		NEED_SESSID, g_ape);
	//register_cmd("SETLEVEL", 	cmd_setlevel, 	NEED_SESSID, g_ape); // Module
	//register_cmd("SETTOPIC", 	cmd_settopic, 	NEED_SESSID, g_ape); // Module
	register_cmd("JOIN", 		cmd_join, 		NEED_SESSID, g_ape);
	register_cmd("LEFT", 		cmd_left, 		NEED_SESSID, g_ape);
	//register_cmd("KICK", 		cmd_kick, 		NEED_SESSID, g_ape); // Module
	//register_cmd("BAN",		cmd_ban,		NEED_SESSID, g_ape); // Module
	register_cmd("SESSION",        	cmd_session,		NEED_SESSID, g_ape);
	
	//register_cmd("KONG", 		cmd_pong, 		NEED_SESSID, g_ape);
	
	//register_cmd("PROXY_CONNECT", 	cmd_proxy_connect, 	NEED_SESSID, g_ape);
	//register_cmd("PROXY_WRITE", 	cmd_proxy_write, 	NEED_SESSID, g_ape);
}

void register_cmd(const char *cmd, unsigned int (*func)(callbackp *), unsigned int need, acetables *g_ape)
{
	callback *new_cmd, *old_cmd;
	
	new_cmd = (callback *) xmalloc(sizeof(*new_cmd));

	new_cmd->func = func;
	new_cmd->need = need;
	
	/* Unregister old cmd if exists */
	if ((old_cmd = (callback *)hashtbl_seek(g_ape->hCallback, cmd)) != NULL) {
		hashtbl_erase(g_ape->hCallback, cmd);
	}
	
	hashtbl_append(g_ape->hCallback, cmd, (void *)new_cmd);
	
}


int register_hook_cmd(const char *cmd, unsigned int (*func)(callbackp *), void *data, acetables *g_ape)
{
	callback_hook *hook;
	if (hashtbl_seek(g_ape->hCallback, cmd) == NULL) {
		return 0;
	}
	
	hook = xmalloc(sizeof(*hook));
	hook->cmd = xstrdup(cmd);
	hook->next = g_ape->cmd_hook;
	hook->func = func;
	hook->data = data;
	g_ape->cmd_hook = hook;
	
	return 1;
}

int call_cmd_hook(const char *cmd, callbackp *cp, acetables *g_ape)
{
	callback_hook *hook;
	
	for (hook = g_ape->cmd_hook; hook != NULL; hook = hook->next) {
		cp->data = hook->data;
		unsigned int ret;
		if (strcasecmp(hook->cmd, cmd) == 0 && (ret = hook->func(cp)) != RETURN_NOTHING) {
			return ret;
		}
	}
	cp->data = NULL;
	return RETURN_NOTHING;
}

void unregister_cmd(const char *cmd, acetables *g_ape)
{
	hashtbl_erase(g_ape->hCallback, cmd);
}

unsigned int checkcmd(clientget *cget, subuser **iuser, acetables *g_ape)
{
	
	int attach = 1;
	callback *cmdback;
	json_item *ijson, *ojson, *rjson, *jchl;
	
	unsigned int flag;
	
	USERS *guser = NULL;
	subuser *sub = NULL;
	
	ijson = ojson = init_json_parser(cget->get);
	
	if (ijson == NULL || ijson->jchild.child == NULL) {
		SENDH(cget->client->fd, ERR_BAD_JSON, g_ape);
	} else {
		//else if ((rjson = json_lookup(ijson->child, "cmd")) != NULL && rjson->jval.vu.str.value != NULL && (cmdback = (callback *)hashtbl_seek(g_ape->hCallback, rjson->jval.vu.str.value)) != NULL) {
		for (ijson = ijson->jchild.child; ijson != NULL; ijson = ijson->next) {

			rjson = json_lookup(ijson->jchild.child, "cmd");
			//printf("valval %s\n", rjson->jval.vu.str.value);
			if (rjson != NULL && rjson->jval.vu.str.value != NULL && (cmdback = (callback *)hashtbl_seek(g_ape->hCallback, rjson->jval.vu.str.value)) != NULL) {
				callbackp cp;
				cp.client = NULL;
				cp.cmd = rjson->jval.vu.str.value;
				cp.data = NULL;

				switch(cmdback->need) {
					case NEED_SESSID:
						{
							json_item *jsid;
				
							if (guser == NULL && (jsid = json_lookup(ijson->jchild.child, "sessid")) != NULL && jsid->jval.vu.str.value != NULL) {
								guser = seek_user_id(jsid->jval.vu.str.value, g_ape);
							}
						}
						break;
					case NEED_NOTHING:
						guser = NULL;
						break;
				}
		
				if (cmdback->need != NEED_NOTHING) {
					//json_item *jchl;
					
					if (guser == NULL) {
						SENDH(cget->client->fd, ERR_BAD_SESSID, g_ape);
						free_json_item(ojson);
						
						return (CONNECT_SHUTDOWN);
					} else if (sub == NULL) {
						
						sub = getsubuser(guser, cget->host);
						if (sub != NULL && sub->client->fd != cget->client->fd && sub->state == ALIVE) {
							/* The user open a new connection while he already has one openned */
							struct _transport_open_same_host_p retval = transport_open_same_host(sub, cget->client, guser->transport);				
					
							if (retval.client_close != NULL) {
								//CLOSE(retval.client_close->fd, g_ape);
								shutdown(retval.client_close->fd, 2);
							}
							sub->client = cp.client = retval.client_listener;
							sub->state = retval.substate;
							attach = retval.attach;
					
						} else if (sub == NULL) {
							sub = addsubuser(cget->client, cget->host, guser, g_ape);
							if (sub != NULL) {
								subuser_restor(sub, g_ape);
							}
						} else if (sub != NULL) {
							sub->client = cget->client;
						}
						guser->idle = (long int)time(NULL); // update user idle

						sub->idle = guser->idle; // Update subuser idle
						
					}
					
					if (sub != NULL && (jchl = json_lookup(ijson->jchild.child, "chl")) != NULL && jchl->jval.vu.integer_value > sub->current_chl) {
						sub->current_chl = jchl->jval.vu.integer_value;
					} else if (sub != NULL) {
						/* if a bad challenge is detected, we are stoping walking on cmds */
						send_error(guser, "BAD_CHL", "250", g_ape);

						free_json_item(ojson);
						sub->state = ALIVE;
						
						return (CONNECT_KEEPALIVE);
					}
				}
		
				cp.param = json_lookup(ijson->jchild.child, "params");
				cp.client = (cp.client != NULL ? cp.client : cget->client);
				cp.call_user = guser;
				cp.call_subuser = sub;
				cp.g_ape = g_ape;
				cp.host = cget->host;
				cp.ip = cget->ip_get;
				cp.chl = (sub != NULL ? sub->current_chl : 0);
				// Ajouter un string de la commande
				
				if ((flag = call_cmd_hook(cp.cmd, &cp, g_ape)) == RETURN_NOTHING) {
					flag = cmdback->func(&cp);
				}
		
				if (flag & RETURN_NULL) {
					guser = NULL;
				} else if (flag & RETURN_LOGIN) {
					guser = cp.call_user;
				} else if (flag & RETURN_BAD_PARAMS) {
					if (guser != NULL) {
						send_error(guser, "BAD_PARAM", "001", g_ape);
						guser = NULL;
					} else {
						SENDH(cget->client->fd, ERR_BAD_PARAM, g_ape);
					}
				}
		
				if (guser != NULL) {
			
					if (sub == NULL) {
						sub = getsubuser(guser, cget->host);	
					}
			
					*iuser = (attach ? sub : NULL);
			
					if (flag & RETURN_UPDATE_IP) {
						strncpy(guser->ip, cget->ip_get, 16);
					}
			
					/* If tmpfd is set, we do not have any reasons to change its state */
					sub->state = ALIVE;
					
				} else {
					/* Doesn't need sessid */
					free_json_item(ojson);
					
					return (CONNECT_SHUTDOWN);
				}
			} else {
				//printf("Cant find %s\n", rjson->jval.vu.str.value);
				free_json_item(ojson);
				SENDH(cget->client->fd, ERR_BAD_CMD, g_ape);
				return (CONNECT_SHUTDOWN);
			}
		}
		free_json_item(ojson);

		return (CONNECT_KEEPALIVE);
	}
	
	return (CONNECT_SHUTDOWN);
}

unsigned int cmd_connect(callbackp *callbacki)
{
	USERS *nuser;
	RAW *newraw;
	json_item *jstr = NULL;
	
	APE_PARAMS_INIT();
	
	nuser = adduser(callbacki->client, callbacki->host, callbacki->g_ape);
	
	callbacki->call_user = nuser;
	
	if (nuser == NULL) {
		SENDH(callbacki->client->fd, ERR_CONNECT, callbacki->g_ape);
		
		return (RETURN_NOTHING);
	}
	
	switch(JINT(transport)) {
		case 1:
			nuser->transport = TRANSPORT_XHRSTREAMING;
			break;
		case 2:
			nuser->transport = TRANSPORT_JSONP;
			break;
		case 3:
			nuser->transport = TRANSPORT_PERSISTANT;
			break;
		default:
			nuser->transport = TRANSPORT_LONGPOLLING;
			break;
		
	}

	subuser_restor(getsubuser(callbacki->call_user, callbacki->host), callbacki->g_ape);
	
	jstr = json_new_object();	
	json_set_property_strN(jstr, "sessid", 6, nuser->sessid, 32);
	
	newraw = forge_raw(RAW_LOGIN, jstr);
	newraw->priority = RAW_PRI_HI;
	
	post_raw(newraw, nuser, callbacki->g_ape);
	
	
	return (RETURN_LOGIN | RETURN_UPDATE_IP);

}

unsigned int cmd_script(callbackp *callbacki)
{
	char *domain = CONFIG_VAL(Server, domain, callbacki->g_ape->srv);
	char *script = NULL;
	
	APE_PARAMS_INIT();
	
	if (domain == NULL) {
		send_error(callbacki->call_user, "NO_DOMAIN", "201", callbacki->g_ape);
	} else {
		sendf(callbacki->client->fd, callbacki->g_ape, "%s<html>\n<head>\n\t<script>\n\t\tdocument.domain=\"%s\"\n\t</script>\n", HEADER_DEFAULT, domain);

		JFOREACH(scripts, script) {
			sendf(callbacki->client->fd, callbacki->g_ape, "\t<script type=\"text/javascript\" src=\"%s\"></script>\n", script);
		}
		sendbin(callbacki->client->fd, "</head>\n<body>\n</body>\n</html>", 30, callbacki->g_ape);
	}
	
	return (RETURN_NOTHING);
}

unsigned int cmd_join(callbackp *callbacki)
{
	CHANNEL *jchan;
	RAW *newraw;
	json_item *jlist = NULL;
	BANNED *blist;
	char *chan_name = NULL;
	
	APE_PARAMS_INIT();
	
	JFOREACH(channels, chan_name) {
	
		if ((jchan = getchan(chan_name, callbacki->g_ape)) == NULL) {
			jchan = mkchan(chan_name, callbacki->g_ape);
		
			if (jchan == NULL) {
			
				send_error(callbacki->call_user, "CANT_JOIN_CHANNEL", "202", callbacki->g_ape);
			
			} else {
		
				join(callbacki->call_user, jchan, callbacki->g_ape);
			}
	
		} else if (isonchannel(callbacki->call_user, jchan)) {
		
			send_error(callbacki->call_user, "ALREADY_ON_CHANNEL", "100", callbacki->g_ape);

		} else {
			blist = getban(jchan, callbacki->call_user->ip);
			if (blist != NULL) {
				
				jlist = json_new_object();
				
				json_set_property_strZ(jlist, "reason", blist->reason);
				json_set_property_strZ(jlist, "error", "YOU_ARE_BANNED");

				/*
					TODO: Add Until
				*/
				newraw = forge_raw(RAW_ERR, jlist);
			
				post_raw(newraw, callbacki->call_user, callbacki->g_ape);
			} else {
				join(callbacki->call_user, jchan, callbacki->g_ape);
			}
		}
	} JFOREACH_ELSE {
		return (RETURN_BAD_PARAMS);
	}
	return (RETURN_NOTHING);
}

unsigned int cmd_check(callbackp *callbacki)
{
	return (RETURN_NOTHING);
}

unsigned int cmd_send(callbackp *callbacki)
{
	json_item *jlist = NULL;
	char *msg, *pipe;
	
	APE_PARAMS_INIT();
	
	if ((msg = JSTR(msg)) != NULL && (pipe = JSTR(pipe)) != NULL) {
		jlist = json_new_object();
		
		json_set_property_strZ(jlist, "msg", msg);

		post_to_pipe(jlist, RAW_DATA, pipe, callbacki->call_subuser, callbacki->g_ape);
		
		return (RETURN_NOTHING);
	}
	
	return (RETURN_BAD_PARAMS);
	
}
unsigned int cmd_quit(callbackp *callbacki)
{
	QUIT(callbacki->client->fd, callbacki->g_ape);
	deluser(callbacki->call_user, callbacki->g_ape); // After that callbacki->call_user is free'd
	
	return (RETURN_NULL);
}

#if 0
unsigned int cmd_setlevel(callbackp *callbacki)
{
	USERS *recver;
	
	if ((recver = seek_user(callbacki->param[3], callbacki->param[2], callbacki->g_ape)) == NULL) {
		send_error(callbacki->call_user, "UNKNOWN_USER", "102", callbacki->g_ape);
	} else {
		setlevel(callbacki->call_user, recver, getchan(callbacki->param[2], callbacki->g_ape), atoi(callbacki->param[4]), callbacki->g_ape);
	}
	return (RETURN_NOTHING);
}
#endif

unsigned int cmd_left(callbackp *callbacki)
{
	CHANNEL *chan;
	char *chan_name;
	
	APE_PARAMS_INIT();
	
	if ((chan_name = JSTR(channel)) != NULL) {
	
		if ((chan = getchan(chan_name, callbacki->g_ape)) == NULL) {
			send_error(callbacki->call_user, "UNKNOWN_CHANNEL", "103", callbacki->g_ape);
		
		} else if (!isonchannel(callbacki->call_user, chan)) {
			send_error(callbacki->call_user, "NOT_IN_CHANNEL", "104", callbacki->g_ape);
	
		} else {
	
			left(callbacki->call_user, chan, callbacki->g_ape);
		}
		
		return (RETURN_NOTHING);
	}
	
	return (RETURN_BAD_PARAMS);
}

unsigned int cmd_session(callbackp *callbacki)
{
	char *key, *val, *action;
	APE_PARAMS_INIT();
	
	if ((action = JSTR(action)) != NULL) {
		if (strcasecmp(action, "set") == 0) {
			JFOREACH_K(values, key, val) {
				if (set_session(callbacki->call_user, key, val, 0, callbacki->g_ape) == NULL) {
					send_error(callbacki->call_user, "SESSION_ERROR", "203", callbacki->g_ape);
				}
			} JFOREACH_ELSE {
				return (RETURN_BAD_PARAMS);
			}		
		} else if (strcasecmp(action, "get") == 0) {
			json_item *jlist = NULL, *jobj = json_new_object();
			RAW *newraw;
			
			//set_json("sessions", NULL, &jlist);
			
			JFOREACH(values, val) {
				session *sTmp = get_session(callbacki->call_user, val);
				json_set_property_strZ(jobj, val, (sTmp != NULL ? sTmp->val : "null"));
			} JFOREACH_ELSE {
				free_json_item(jlist);
				return (RETURN_BAD_PARAMS);
			}
			jlist = json_new_object();
			json_set_property_objN(jlist, "sessions", 8, jobj);

			newraw = forge_raw("SESSIONS", jlist);
			newraw->priority = RAW_PRI_HI;
			post_raw_sub(newraw, callbacki->call_subuser, callbacki->g_ape);
			
		} else {
			return (RETURN_BAD_PARAMS);
		}
	} else {
		return (RETURN_BAD_PARAMS);
	}
	
	return (RETURN_NOTHING);
}

#if 0
unsigned int cmd_proxy_connect(callbackp *callbacki)
{
	ape_proxy *proxy;
	RAW *newraw;
	json *jlist = NULL;
	
	proxy = proxy_init_by_host_port(callbacki->param[2], callbacki->param[3], callbacki->g_ape);
	
	if (proxy == NULL) {
		send_error(callbacki->call_user, "PROXY_INIT_ERROR", "204", callbacki->g_ape);
	} else {
		proxy_attach(proxy, callbacki->call_user->pipe->pubid, 1, callbacki->g_ape);
		
		set_json("pipe", NULL, &jlist);
		json_attach(jlist, get_json_object_proxy(proxy), JSON_OBJECT);
	
		newraw = forge_raw(RAW_PROXY, jlist);
		post_raw(newraw, callbacki->call_user, callbacki->g_ape);		
	}
	
	return (RETURN_NOTHING);
}

unsigned int cmd_proxy_write(callbackp *callbacki)
{
	ape_proxy *proxy;

	if ((proxy = proxy_are_linked(callbacki->call_user->pipe->pubid, callbacki->param[2], callbacki->g_ape)) == NULL) {
		send_error(callbacki->call_user, "UNKNOWN_PIPE", "109", callbacki->g_ape);
	} else if (proxy->state != PROXY_CONNECTED) {
		send_error(callbacki->call_user, "PROXY_NOT_CONNETED", "205", callbacki->g_ape);
	} else {
		proxy_write(proxy, callbacki->param[3], callbacki->g_ape);
	}
	
	return (RETURN_NOTHING);
}
#endif
#if 0
/* This is usefull to ask all subuser to update their sessions */
unsigned int cmd_pong(callbackp *callbacki)
{
	if (strcmp(callbacki->param[2], callbacki->call_user->lastping) == 0) {
		RAW *newraw;
				
		callbacki->call_user->lastping[0] = '\0';

		json *jlist = NULL;
	
		set_json("value", callbacki->param[2], &jlist);
	
		newraw = forge_raw("UPDATE", jlist);
	
		post_raw_sub(newraw, getsubuser(callbacki->call_user, callbacki->host), callbacki->g_ape);
	}
	return (RETURN_NOTHING);
}



#endif
