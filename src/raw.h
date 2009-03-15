/*
  Copyright (C) 2006, 2007, 2008, 2009 Anthony Catel <a.catel@weelya.com>

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

/* raw.h */

#ifndef _RAW
#define _RAW

#include "users.h"
#include "handle_http.h"
#include "sock.h"
#include "main.h"

/*
	Non identifiable user's error.
	E.g. Bad sessid, bad param
	Note : time is not defined in this case
*/
#define ERR_BAD_PARAM 		"[\n{\"raw\":\"ERR\",\"time\":null,\"datas\":{\"value\":\"001\"}}\n]\n"
#define ERR_BAD_RAW 		"[\n{\"raw\":\"ERR\",\"time\":null,\"datas\":{\"value\":\"002\"}}\n]\n"
#define ERR_NICK_USED 		"[\n{\"raw\":\"ERR\",\"time\":null,\"datas\":{\"value\":\"003\"}}\n]\n"
#define ERR_BAD_SESSID 		"[\n{\"raw\":\"ERR\",\"time\":null,\"datas\":{\"value\":\"004\"}}\n]\n"
#define ERR_BAD_NICK		"[\n{\"raw\":\"ERR\",\"time\":null,\"datas\":{\"value\":\"005\"}}\n]\n"
#define ERR_CONNECT		"[\n{\"raw\":\"ERR\",\"time\":null,\"datas\":{\"value\":\"006\"}}\n]\n"



#define SUCCESS_LOGIN "200"


#define MOTD_FILE "MOTD"

unsigned int checkraw(clientget *cget, subuser **iuser, acetables *g_ape);


void do_register(acetables *g_ape);

/* Flag returned by raw callback */

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
unsigned int raw_connect(struct _callbackp *);
unsigned int raw_check(struct _callbackp *);
unsigned int raw_send(struct _callbackp *);
unsigned int raw_quit(struct _callbackp *);
unsigned int raw_setlevel(struct _callbackp *);
unsigned int raw_settopic(struct _callbackp *);
unsigned int raw_join(struct _callbackp *);
unsigned int raw_left(struct _callbackp *);
unsigned int raw_kick(struct _callbackp *);
unsigned int raw_ban(struct _callbackp *);
unsigned int raw_session(struct _callbackp *);
unsigned int raw_pconnect(struct _callbackp *);
unsigned int raw_script(struct _callbackp *);
unsigned int raw_pong(struct _callbackp *);
unsigned int raw_proxy_connect(struct _callbackp *);
///////////////////////////////////////////////////////////////////////////////////////////////
void register_raw(char *raw, int nParam, unsigned int (*func)(callbackp *), unsigned int need, acetables *g_ape);
void unregister_raw(char *raw, acetables *g_ape);

#endif

