/*
  Copyright (C) 2006, 2007, 2008, 2009 Anthony Catel <a.catel@weelya.com>

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

/* cmd.h */

#ifndef _CMD_H
#define _CMD_H

#include "users.h"
#include "handle_http.h"
#include "sock.h"
#include "main.h"

/*
	Non identifiable user's error.
	E.g. Bad sessid, bad param
	Note : time is not defined in this case
*/
#define ERR_BAD_PARAM 		"[\n{\"raw\":\"ERR\",\"time\":null,\"datas\":{\"code\":\"001\",\"value\":\"BAD_PARAM\"}}\n]\n"
#define ERR_BAD_CMD 		"[\n{\"raw\":\"ERR\",\"time\":null,\"datas\":{\"code\":\"002\",\"value\":\"BAD_CMD\"}}\n]\n"
#define ERR_BAD_SESSID 		"[\n{\"raw\":\"ERR\",\"time\":null,\"datas\":{\"code\":\"004\",\"value\":\"BAD_SESSID\"}}\n]\n"
#define ERR_CONNECT		"[\n{\"raw\":\"ERR\",\"time\":null,\"datas\":{\"code\":\"200\",\"value\":\"UNKNOWN_CONNECTION_ERROR\"}}\n]\n"



#define SUCCESS_LOGIN "200"


#define MOTD_FILE "MOTD"

unsigned int checkcmd(clientget *cget, subuser **iuser, acetables *g_ape);


void do_register(acetables *g_ape);

/* Flag returned by cmd callback */

#define FOR_NOTHING 	0x00
#define FOR_LOGIN 	0x01
#define FOR_SESSID 	0x02
#define FOR_NULL	0x04
#define FOR_UPDATE_IP	0x08

typedef struct _callbackp callbackp;

struct _callbackp
{
	int nParam;
	char **param;
	unsigned int fdclient;
	struct USERS *call_user;
	char *host;
	
	acetables *g_ape;
};


typedef struct callback
{
	int nParam;
	unsigned int need;
	unsigned int (*func)(struct _callbackp *);
} callback;




enum {
	NEED_NICK = 0,
	NEED_SESSID,
	NEED_NOTHING
};

///////////////////////////////////////////////////////////////////////////////////////////////
unsigned int cmd_connect(struct _callbackp *);
unsigned int cmd_check(struct _callbackp *);
unsigned int cmd_send(struct _callbackp *);
unsigned int cmd_quit(struct _callbackp *);
unsigned int cmd_setlevel(struct _callbackp *);
unsigned int cmd_settopic(struct _callbackp *);
unsigned int cmd_join(struct _callbackp *);
unsigned int cmd_left(struct _callbackp *);
unsigned int cmd_kick(struct _callbackp *);
unsigned int cmd_ban(struct _callbackp *);
unsigned int cmd_session(struct _callbackp *);
unsigned int cmd_pconnect(struct _callbackp *);
unsigned int cmd_script(struct _callbackp *);
unsigned int cmd_pong(struct _callbackp *);
unsigned int cmd_proxy_connect(struct _callbackp *);
unsigned int cmd_proxy_write(struct _callbackp *);
///////////////////////////////////////////////////////////////////////////////////////////////
void register_cmd(const char *cmd, int nParam, unsigned int (*func)(callbackp *), unsigned int need, acetables *g_ape);
void unregister_cmd(const char *cmd, acetables *g_ape);

#endif

