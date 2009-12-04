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

/* users.h */

#ifndef _USERS_H
#define _USERS_H

#include "main.h"

#include "channel.h"
#include "json.h"
#include "extend.h"


#define FLG_NOFLAG	0x00
#define FLG_AUTOOP 	0x01
#define FLG_NOKICK 	0x02
#define FLG_BOTMANAGE 	0x04
#define FLG_PCONNECT 	0x08


#define MAX_SESSION_LENGTH 102400 // 100 ko
#define MAX_HOST_LENGTH 256


// Le 25/12/2006 à 02:15:19 Joyeux Noël


struct _raw_pool {
	struct RAW *raw;
	struct _raw_pool *next;
	struct _raw_pool *prev;
	int start;
};

typedef struct USERS
{
	struct {
		struct _link_list *ulink;
		int nlink;
	} links;

	struct {
		struct _session *data;
		int length;
	} sessions;

	struct USERS *next;
	struct USERS *prev;
	struct CHANLIST *chan_foot;
	struct _transpipe *pipe;
	struct _extend *properties;
	struct _subuser *subuser;
	
	json_item *cmdqueue;
	
	time_t idle;
	int transport;
	int nsub;
	unsigned int nraw;
	unsigned int flags;
	
	unsigned short int type;
	unsigned short int istmp;

	char ip[16]; // ipv4
	char lastping[24];
	char sessid[33];

} USERS;


struct _raw_pool_user {
	int nraw;
	int size;
	struct _raw_pool *rawhead;
	struct _raw_pool *rawfoot;
};

typedef struct _subuser subuser;
struct _subuser
{
	
	struct {
		struct _raw_pool_user low;
		struct _raw_pool_user high;
		int nraw;
	} raw_pools;
		
	struct {
		struct _http_headers_response *content;
		int sent;
	} headers;
	
	struct _extend *properties;
	struct _subuser *next;
	ape_socket *client;
	USERS *user;
	time_t idle;
	
	int state;		
	int need_update;	
	int wait_for_free;
	int nraw;
	int burn_after_writing;
	int current_chl;
	char channel[MAX_HOST_LENGTH+1];
};


typedef struct CHANLIST
{
	struct CHANNEL *chaninfo;
	struct CHANLIST *next;
	
} CHANLIST;


struct _users_link
{
	USERS *a;
	USERS *b;
	
	int link_type;
};

struct _link_list
{
	struct _users_link *link;
	struct _link_list *next;
};


typedef struct userslist
{
	struct USERS *userinfo;
	struct userslist *next;
		
	unsigned int level;
	/* TODO: it can be intersting to extend this */
} userslist;


typedef struct _session session;

struct _session
{
	char *val;
	struct _session *next;
	char key[33];
};


enum {
	ALIVE = 0,
	ADIED
};

enum {
	HUMAN = 0,
	BOT
};

enum {
	PRIVMSG = 0,
	CHANMSG,
	INFOMSG
};
enum {
	RAW_MSG = 0,
	RAW_LOGIN,
	RAW_JOIN,
	RAW_LEFT,
	RAW_SETLEVEL
};

#define RAW_DATA 		"DATA"
#define RAW_CHANMSG 		"CHANMSG"
#define RAW_TOPIC 		"TOPIC"
#define RAW_LOGIN 		"LOGIN"
#define RAW_JOIN 		"JOIN"
#define RAW_LEFT 		"LEFT"
#define RAW_SETLEVEL 		"SETLVL"
#define RAW_SETTOPIC 		"SETTOPIC"
#define RAW_USER 		"USER"
#define RAW_ERR 		"ERR"
#define RAW_CHANNEL		"CHANNEL"
#define RAW_KICK		"KICKED"
#define RAW_BAN			"BANNED"
#define RAW_PROXY		"PROXY"

USERS *seek_user(const char *nick, const char *linkid, acetables *g_ape);
USERS *init_user(acetables *g_ape);
USERS *adduser(ape_socket *client, char *host, char *ip, USERS *allocated, acetables *g_ape);
USERS *seek_user_id(const char *sessid, acetables *g_ape);
USERS *seek_user_simple(const char *nick, acetables *g_ape);



void deluser(USERS *user, acetables *g_ape);

void do_died(subuser *user);

void check_timeout(acetables *g_ape, int last);
void grant_aceop(USERS *user);

void send_error(USERS *user, const char *msg, const char *code, acetables *g_ape);
void send_msg(USERS *user, const char *msg, const char *type, acetables *g_ape);
void send_msg_sub(subuser *sub, const char *msg, const char *type, acetables *g_ape);
void send_msg_channel(struct CHANNEL *chan, const char *msg, const char *type, acetables *g_ape);

unsigned int isonchannel(USERS *user, struct CHANNEL *chan);

json_item *get_json_object_user(USERS *user);

session *get_session(USERS *user, const char *key);
session *set_session(USERS *user, const char *key, const char *val, int update, acetables *g_ape);
void clear_sessions(USERS *user);
void sendback_session(USERS *user, session *sess, acetables *g_ape);

subuser *addsubuser(ape_socket *client, const char *channel, USERS *user, acetables *g_ape);
subuser *getsubuser(USERS *user, const char *channel);
void delsubuser(subuser **current, acetables *g_ape);
void subuser_restor(subuser *sub, acetables *g_ape);

void clear_subusers(USERS *user, acetables *g_ape);
void ping_request(USERS *user, acetables *g_ape);

void make_link(USERS *a, USERS *b);
struct _users_link *are_linked(USERS *a, USERS *b);
void destroy_link(USERS *a, USERS *b);


#endif

