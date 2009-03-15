/*
  Copyright (C) 2006, 2007, 2008  Anthony Catel <a.catel@weelya.com>

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

/* channel.h */

#ifndef _CHANNEL
#define _CHANNEL

#include "main.h"


#define MAX_TOPIC_LEN 128
#define DEFAULT_TOPIC "Chat%20powered%20by%20AJAX%20Chat%20Engine\0"

typedef struct CHANNEL
{
	char name[MAX_CHAN_LEN+1];
	char topic[MAX_TOPIC_LEN+1];

	transpipe *pipe;
	
	struct userslist *head;
	
	struct BANNED *banned;
	
	struct _extend *properties;
	
	int interactive;

} CHANNEL;

typedef struct BANNED
{
	char ip[16];
	char reason[257];
	
	long int expire;
	
	struct BANNED *next;
} BANNED;

CHANNEL *mkchan(char *chan, char *topic, acetables *g_ape);
CHANNEL *getchan(char *chan, acetables *g_ape);

BANNED *getban(CHANNEL *chan, char *ip);

int mkallchan(acetables *g_ape);

void rmchan(CHANNEL *chan, acetables *g_ape);

void join(struct USERS *user, CHANNEL *chan, acetables *g_ape);
void left(struct USERS *user, CHANNEL *chan, acetables *g_ape);
void left_all(struct USERS *user, acetables *g_ape);

void ban(CHANNEL *chan, struct USERS *banner, char *ip, char *reason, unsigned int expire, acetables *g_ape);
void rmban(CHANNEL *chan, char *ip);
void rmallban(CHANNEL *chan);

struct userslist *getlist(char *chan, acetables *g_ape);
struct userslist *getuchan(struct USERS *user, CHANNEL *chan);
	
unsigned int setlevel(struct USERS *user_actif, struct USERS *user_passif, CHANNEL *chan, unsigned int lvl);
unsigned int settopic(struct USERS *user, CHANNEL *chan, char *topic);
unsigned int isvalidchan(char *name);

struct json *get_json_object_channel(CHANNEL *chan);
#endif
