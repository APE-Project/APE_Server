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

/* channel.h */

#ifndef _CHANNEL_H
#define _CHANNEL_H

#include "main.h"
#include "pipe.h"
#include "users.h"
#include "extend.h"
#include "json.h"

#define MAX_TOPIC_LEN 128
#define DEFAULT_TOPIC "Chat%20powered%20by%20AJAX%20Push%20Engine\0"

#define CHANNEL_NONINTERACTIVE 		0x01
#define CHANNEL_AUTODESTROY 		0x02

typedef struct CHANNEL
{
	//char topic[MAX_TOPIC_LEN+1];

	struct _transpipe *pipe;
	struct userslist *head;
	
	struct BANNED *banned;
	
	extend *properties;

	int flags;
	char name[MAX_CHAN_LEN+1];

} CHANNEL;

typedef struct BANNED
{
	char ip[16];
	char reason[257];
	
	long int expire;
	
	struct BANNED *next;
} BANNED;

CHANNEL *mkchan(char *chan, int flags, acetables *g_ape);
CHANNEL *getchan(const char *chan, acetables *g_ape);

BANNED *getban(CHANNEL *chan, const char *ip);
CHANNEL *getchanbypubid(const char *pubid, acetables *g_ape);
int mkallchan(acetables *g_ape);

void rmchan(CHANNEL *chan, acetables *g_ape);

void join(struct USERS *user, CHANNEL *chan, acetables *g_ape);
void left(struct USERS *user, CHANNEL *chan, acetables *g_ape);
void left_all(struct USERS *user, acetables *g_ape);

void ban(CHANNEL *chan, struct USERS *banner, const char *ip, char *reason, unsigned int expire, acetables *g_ape);
void rmban(CHANNEL *chan, const char *ip);
void rmallban(CHANNEL *chan);

struct userslist *getlist(const char *chan, acetables *g_ape);
struct userslist *getuchan(struct USERS *user, CHANNEL *chan);
	
unsigned int setlevel(struct USERS *user_actif, struct USERS *user_passif, CHANNEL *chan, unsigned int lvl, acetables *g_ape);
//unsigned int settopic(struct USERS *user, CHANNEL *chan, const char *topic, acetables *g_ape);
unsigned int isvalidchan(char *name);

json_item *get_json_object_channel(CHANNEL *chan);

#endif

