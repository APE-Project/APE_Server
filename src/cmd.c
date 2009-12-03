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

void do_register(acetables *g_ape)
{
	register_cmd("CONNECT",		cmd_connect, 	NEED_NOTHING, g_ape);
	register_cmd("SCRIPT",      cmd_script,		NEED_NOTHING, g_ape);
	
	register_cmd("CHECK", 		cmd_check, 		NEED_SESSID, g_ape);
	register_cmd("SEND", 		cmd_send, 		NEED_SESSID, g_ape);

	register_cmd("QUIT", 		cmd_quit, 		NEED_SESSID, g_ape);
	register_cmd("JOIN", 		cmd_join, 		NEED_SESSID, g_ape);
	register_cmd("LEFT", 		cmd_left, 		NEED_SESSID, g_ape);
	register_cmd("SESSION",     cmd_session,	NEED_SESSID, g_ape);
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

void register_bad_cmd(unsigned int (*func)(callbackp *), void *data, acetables *g_ape)
{
	callback_hook *new_cmd;
	
	new_cmd = xmalloc(sizeof(*new_cmd));

	new_cmd->func = func;
	new_cmd->next = g_ape->bad_cmd_callbacks;
	new_cmd->data = data;
	
	g_ape->bad_cmd_callbacks = new_cmd;
	
}

int register_hook_cmd(const char *cmd, unsigned int (*func)(callbackp *), void *data, acetables *g_ape)
{
	callback_hook *hook;
	
	if (hashtbl_seek(g_ape->hCallback, cmd) == NULL) {
		return 0;
	}
	
	hook = xmalloc(sizeof(*hook));
	hook->cmd = xstrdup(cmd);
	hook->func = func;
	hook->data = data;
	hook->next = NULL;
	
	if (g_ape->cmd_hook.head == NULL) {
		g_ape->cmd_hook.head = hook;
		g_ape->cmd_hook.foot = hook;
	} else {
		g_ape->cmd_hook.foot->next = hook;
		g_ape->cmd_hook.foot = hook;
	}

	return 1;
}

int call_cmd_hook(const char *cmd, callbackp *cp, acetables *g_ape)
{
	callback_hook *hook;
	
	for (hook = g_ape->cmd_hook.head; hook != NULL; hook = hook->next) {
		unsigned int ret;
		
		cp->data = hook->data;
		if (strcasecmp(hook->cmd, cmd) == 0 && (ret = hook->func(cp)) != RETURN_CONTINUE) {
			return ret;
		}
	}
	cp->data = NULL;
	return RETURN_CONTINUE;
}

void unregister_cmd(const char *cmd, acetables *g_ape)
{
	hashtbl_erase(g_ape->hCallback, cmd);
}

static unsigned int handle_bad_cmd(callbackp *callbacki)
{
	callback_hook *hook_bad;
	int flagret;
	
	for (hook_bad = callbacki->g_ape->bad_cmd_callbacks; hook_bad != NULL; hook_bad = hook_bad->next) {
		callbacki->data = hook_bad->data;
		if ((flagret = hook_bad->func(callbacki)) != RETURN_BAD_CMD) {
			return flagret;
		} else {
			;
		}
	}
	callbacki->data = NULL;
	
	return RETURN_BAD_CMD;
}

int process_cmd(json_item *ijson, struct _cmd_process *pc, subuser **iuser, acetables *g_ape)
{
	callback *cmdback, tmpback = {handle_bad_cmd, NEED_NOTHING};
	json_item *rjson = json_lookup(ijson->jchild.child, "cmd"), *jchl;
	subuser *sub = pc->sub;
	unsigned int flag;
	unsigned short int attach = 1;

	if (rjson != NULL && rjson->jval.vu.str.value != NULL) {
		callbackp cp;
		cp.client = NULL;
		cp.cmd 	= rjson->jval.vu.str.value;
		cp.data = NULL;
		json_item *jsid;
		
		if ((cmdback = (callback *)hashtbl_seek(g_ape->hCallback, rjson->jval.vu.str.value)) == NULL) {
			cmdback = &tmpback;
		}
		
		if ((pc->guser == NULL && (jsid = json_lookup(ijson->jchild.child, "sessid")) != NULL && jsid->jval.vu.str.value != NULL)) {
			pc->guser = seek_user_id(jsid->jval.vu.str.value, g_ape);
		}

		if (cmdback->need != NEED_NOTHING || pc->guser != NULL) { // We process the connection like a "NEED_SESSID" if the user provide its key

			if (pc->guser == NULL) {
				
				RAW *newraw;
				json_item *jlist = json_new_object();

				json_set_property_strZ(jlist, "code", "004");
				json_set_property_strZ(jlist, "value", "BAD_SESSID");

				newraw = forge_raw(RAW_ERR, jlist);
				
				send_raw_inline(pc->client, pc->transport, newraw, g_ape);

				return (CONNECT_SHUTDOWN);
			} else if (sub == NULL) {
				
				sub = getsubuser(pc->guser, pc->host);
				if (sub != NULL && sub->client->fd != pc->client->fd && sub->state == ALIVE) {
					/* The user open a new connection while he already has one openned */
					struct _transport_open_same_host_p retval = transport_open_same_host(sub, pc->client, pc->guser->transport);				
			
					if (retval.client_close != NULL) {
						RAW *newraw;
						json_item *jlist = json_new_object();

						json_set_property_strZ(jlist, "value", "null");

						newraw = forge_raw("CLOSE", jlist);

						send_raw_inline((retval.client_close->fd == pc->client->fd ? pc->client : sub->client), pc->transport, newraw, g_ape);
						
						shutdown(retval.client_close->fd, 2);
					}
					sub->client = cp.client = retval.client_listener;
					sub->state = retval.substate;
					attach = retval.attach;
			
				} else if (sub == NULL) {
					sub = addsubuser(pc->client, pc->host, pc->guser, g_ape);
					if (sub != NULL) {
						subuser_restor(sub, g_ape);
					}
				} else if (sub != NULL) {
					sub->client = pc->client;
				}
				pc->guser->idle = (long int)time(NULL); // update user idle

				sub->idle = pc->guser->idle; // Update subuser idle
				
			}

		}
		
		if (pc->guser != NULL && sub != NULL && (jchl = json_lookup(ijson->jchild.child, "chl")) != NULL && jchl->jval.vu.integer_value > sub->current_chl) {
			sub->current_chl = jchl->jval.vu.integer_value;
		} else if (pc->guser != NULL && sub != NULL) {
			/* if a bad challenge is detected, we are stoping walking on cmds */
			send_error(pc->guser, "BAD_CHL", "250", g_ape);

			sub->state = ALIVE;
			
			return (CONNECT_KEEPALIVE);
		}
					
		cp.param = json_lookup(ijson->jchild.child, "params");
		cp.client = (cp.client != NULL ? cp.client : pc->client);
		cp.call_user = pc->guser;
		cp.call_subuser = sub;
		cp.g_ape = g_ape;
		cp.host = pc->host;
		cp.ip = pc->ip;
		cp.chl = (sub != NULL ? sub->current_chl : 0);
		cp.transport = pc->transport;
		
		/* Little hack to access user object on connect hook callback (preallocate an user) */
		if (strncasecmp(cp.cmd, "CONNECT", 7) == 0 && cp.cmd[7] == '\0') {
			pc->guser = cp.call_user = adduser(cp.client, cp.host, cp.ip, NULL, g_ape);
			pc->guser->transport = pc->transport;
			sub = cp.call_subuser = cp.call_user->subuser;
		}
		
		if ((flag = call_cmd_hook(cp.cmd, &cp, g_ape)) == RETURN_CONTINUE) {
			flag = cmdback->func(&cp);
		}
		
		if (flag & RETURN_NULL) {
			pc->guser = NULL;
		} else if (flag & RETURN_BAD_PARAMS) {
			RAW *newraw;
			json_item *jlist = json_new_object();
			
			if (cp.chl) {
				json_set_property_intN(jlist, "chl", 3, cp.chl);
			}
			json_set_property_strZ(jlist, "code", "001");
			json_set_property_strZ(jlist, "value", "BAD_PARAMS");

			newraw = forge_raw(RAW_ERR, jlist);
			
			if (cp.call_user != NULL) {
				//cp.call_user->istmp = 0;
				if (sub == NULL) {
					sub = getsubuser(pc->guser, pc->host);	
				}
				post_raw_sub(newraw, sub, g_ape);
			} else {
				send_raw_inline(pc->client, pc->transport, newraw, g_ape);
			}
			
			//guser = NULL;
		} else if (flag & RETURN_BAD_CMD) {
			RAW *newraw;
			json_item *jlist = json_new_object();

			if (cp.chl) {
				json_set_property_intN(jlist, "chl", 3, cp.chl);
			}
			json_set_property_strZ(jlist, "code", "003");
			json_set_property_strZ(jlist, "value", "BAD_CMD");

			newraw = forge_raw(RAW_ERR, jlist);
			
			if (cp.call_user != NULL) {	
				if (sub == NULL) {
					sub = getsubuser(pc->guser, pc->host);	
				}
				post_raw_sub(newraw, sub, g_ape);
			} else {
				send_raw_inline(pc->client, pc->transport, newraw, g_ape);
			}					
		}

		if (pc->guser != NULL) {
			if (sub == NULL) {
				sub = getsubuser(pc->guser, pc->host);	
			}
			if (iuser != NULL) {
				*iuser = (attach ? sub : NULL);
			}
			/* If tmpfd is set, we do not have any reasons to change its state */
			sub->state = ALIVE;
			
			if (flag & RETURN_HANG || flag & RETURN_BAD_PARAMS) {
				return (CONNECT_KEEPALIVE);
			}
			
		} else {
			/* Doesn't need sessid */

			return (CONNECT_SHUTDOWN);
		}
	} else {

		RAW *newraw;
		json_item *jlist = json_new_object();

		json_set_property_strZ(jlist, "code", "003");
		json_set_property_strZ(jlist, "value", "NO_CMD");

		newraw = forge_raw(RAW_ERR, jlist);

		send_raw_inline(pc->client, pc->transport, newraw, g_ape);
		//printf("Cant find %s\n", rjson->jval.vu.str.value);
		return (CONNECT_SHUTDOWN);
	}
	
	return -1;
}


unsigned int checkcmd(clientget *cget, transport_t transport, subuser **iuser, acetables *g_ape)
{	
	struct _cmd_process pc = {NULL, NULL, cget->client, cget->host, cget->ip_get, transport};
	
	json_item *ijson, *ojson;
	
	unsigned int ret;

	ijson = ojson = init_json_parser(cget->get);
	
	if (ijson == NULL || ijson->jchild.child == NULL) {
		RAW *newraw;
		json_item *jlist = json_new_object();

		json_set_property_strZ(jlist, "code", "005");
		json_set_property_strZ(jlist, "value", "BAD_JSON");

		newraw = forge_raw(RAW_ERR, jlist);
		
		send_raw_inline(cget->client, transport, newraw, g_ape);
	} else {
		for (ijson = ijson->jchild.child; ijson != NULL; ijson = ijson->next) {
			
			if (pc.guser != NULL && pc.guser->istmp) { /* if "CONNECT" was delayed, push other cmd to the queue and stop execution */
				pc.guser->cmdqueue = json_item_copy(ijson, NULL);
				break;
			}
			
			if ((ret = process_cmd(ijson, &pc, iuser, g_ape)) != -1) {
				free_json_item(ojson);
				return ret;
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

	nuser = adduser(NULL, NULL, NULL, callbacki->call_user, callbacki->g_ape);
	
	callbacki->call_user = nuser;

	subuser_restor(getsubuser(callbacki->call_user, callbacki->host), callbacki->g_ape);
	
	jstr = json_new_object();	
	json_set_property_strN(jstr, "sessid", 6, nuser->sessid, 32);
	
	newraw = forge_raw(RAW_LOGIN, jstr);
	newraw->priority = RAW_PRI_HI;
	
	post_raw(newraw, nuser, callbacki->g_ape);	
	
	return (RETURN_NOTHING);
}

unsigned int cmd_script(callbackp *callbacki)
{
	char *domain = CONFIG_VAL(Server, domain, callbacki->g_ape->srv);
	char *script = NULL;
	int alloc = 0;
	APE_PARAMS_INIT();
	
	if (domain == NULL) {
		send_error(callbacki->call_user, "NO_DOMAIN", "201", callbacki->g_ape);
	} else {
		char *autodom;
		if (strcmp(domain, "auto") == 0 && (autodom = JSTR(domain)) != NULL) {
			domain = autodom;
			#if 0
			/* http://geekandpoke.typepad.com/.a/6a00d8341d3df553ef0120a6d65b8a970b-pi */
			
			struct _http_header_line *hlines;

			for (hlines = callbacki->client->http.hlines; hlines != NULL; hlines = hlines->next) {
				if (strcasecmp(hlines->key.val, "host") == 0) {
					char *loc;
					char *newdom = xmalloc(sizeof(char) * (hlines->value.len + 1));
					memset(newdom, '\0', hlines->value.len + 1);
					if ((loc = strrchr(hlines->value.val, '.')) != NULL) {
						int i, pos = 0;

						for (i = 0; i < hlines->value.len; i++, pos++) {
							newdom[pos] = hlines->value.val[i];
							if (newdom[pos] == ':') {
								newdom[pos] = '\0';
								break;
							}
							if (hlines->value.val[i] == '.' && &hlines->value.val[i] < loc) {
								pos = -1;
							}
						}
						newdom[pos] = '\0';
						domain = newdom;
						alloc = 1;
					}
				}
			}
			#endif		
		}
		sendf(callbacki->client->fd, callbacki->g_ape, "%s<html>\n<head>\n\t<script>\n\t\tdocument.domain=\"%s\"\n\t</script>\n", HEADER_DEFAULT, domain);
		
		if (alloc) {
			free(domain);
		}
		
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
			jchan = mkchan(chan_name, CHANNEL_AUTODESTROY, callbacki->g_ape);
		
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
			json_set_property_intN(jlist, "chl", 3, callbacki->chl);

			newraw = forge_raw("SESSIONS", jlist);
			newraw->priority = RAW_PRI_LO;
			
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
