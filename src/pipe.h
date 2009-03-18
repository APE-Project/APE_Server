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

/* proxy.h */

#ifndef _PIPE_H
#define _PIPE_H

#include "users.h"
#include "main.h"

enum {
	CHANNEL_PIPE = 0,
	USER_PIPE,
	PROXY_PIPE
};

typedef struct _pipe_link pipe_link;
struct _pipe_link {
	struct _transpipe *plink;
	struct _pipe_link *next;
};

typedef struct _transpipe transpipe;
struct _transpipe
{
	void *pipe;
	struct _pipe_link *link;
	
	int type;
	
	char pubid[33];
};

transpipe *init_pipe(void *pipe, int type, acetables *g_ape);

void post_raw_pipe(RAW *raw, char *pipe, acetables *g_ape);

void *get_pipe(char *pubid, acetables *g_ape);
void *get_pipe_strict(char *pubid, struct USERS *user, acetables *g_ape);
void gen_sessid_new(char *input, acetables *g_ape);

#endif
