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

/* proxy.h */

#ifndef _PIPE_H
#define _PIPE_H

#include "main.h"
#include "users.h"
#include "json.h"

enum {
	CHANNEL_PIPE = 0,
	USER_PIPE,
	PROXY_PIPE,
	CUSTOM_PIPE
};

typedef struct _pipe_link pipe_link;
struct _pipe_link {
	struct _transpipe *plink;
	struct _pipe_link *next;
	
	void (*on_unlink)(struct _transpipe *, struct _transpipe *, acetables *);
};

typedef struct _transpipe transpipe;
struct _transpipe
{
	void *pipe;
	void *data;
	
	struct _pipe_link *link;
	struct _extend *properties;

	void (*on_send)(struct _transpipe *, struct USERS *, json_item *, acetables *);
	
	int type;
	
	char pubid[33];
};

transpipe *init_pipe(void *pipe, int type, acetables *g_ape);
void destroy_pipe(transpipe *pipe, acetables *g_ape);

void link_pipe(transpipe *pipe_origin, transpipe *pipe_to, void (*on_unlink)(struct _transpipe *, struct _transpipe *, acetables *));
transpipe *get_pipe(const char *pubid, acetables *g_ape);
transpipe *get_pipe_strict(const char *pubid, struct USERS *user, acetables *g_ape);
void post_json_custom(json_item *jstr, struct USERS *user, struct _transpipe *pipe, acetables *g_ape);
void gen_sessid_new(char *input, acetables *g_ape);
void unlink_all_pipe(transpipe *origin, acetables *g_ape);
json_item *get_json_object_pipe(transpipe *pipe);
json_item *get_json_object_pipe_custom(transpipe *pipe);
#endif

