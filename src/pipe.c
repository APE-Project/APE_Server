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

/* pipe.c */

#include "pipe.h"
#include "utils.h"


const char basic_chars[16] = { 	'a', 'b', 'c', 'd', 'e', 'f', '0', '1',
				'2', '3', '4', '5', '6', '7', '8', '9'
			};


/* Generate a string (32 chars) used for sessid and pubid */
void gen_sessid_new(char *input, acetables *g_ape)
{
	unsigned int i;
	
	do {
		for (i = 0; i < 32; i++) {
			input[i] = basic_chars[rand()%16];
		}
		input[32] = '\0';
	} while(seek_user_id(input, g_ape) != NULL || seek_user_simple(input, g_ape) != NULL); // Colision verification
}

/* Init a pipe (user, channel, proxy) */
transpipe *init_pipe(void *pipe, int type, acetables *g_ape)
{
	transpipe *npipe = NULL;
	
	npipe = xmalloc(sizeof(*npipe));
	npipe->pipe = pipe;
	npipe->type = type;
	npipe->link = NULL;
	
	gen_sessid_new(npipe->pubid, g_ape);
	hashtbl_append(g_ape->hPubid, npipe->pubid, (void *)npipe);
	return npipe;
}

void destroy_pipe(transpipe *pipe, acetables *g_ape)
{
	unlink_all_pipe(pipe, g_ape);
	hashtbl_erase(g_ape->hPubid, pipe->pubid);
	free(pipe);
}

/* Link a pipe to another (e.g. user <=> proxy) */
void link_pipe(transpipe *pipe_origin, transpipe *pipe_to, void (*on_unlink)(struct _transpipe *, struct _transpipe *, acetables *))
{
	struct _pipe_link *link;
	
	if (pipe_origin == NULL || pipe_to == NULL) {
		return;
	}
	link = xmalloc(sizeof(*link));
	
	/* on_unlink is called when the pipe is deleted */
	link->on_unlink = on_unlink;
	
	link->plink = pipe_to;
	link->next = pipe_origin->link;
	pipe_origin->link = link;

}

void unlink_all_pipe(transpipe *origin, acetables *g_ape)
{
	struct _pipe_link *link, *plink;
	
	if (origin == NULL) {
		return;
	}
	link = origin->link;

	while (link != NULL) {
		plink = link->next;
		if (link->on_unlink != NULL) {
		
			/* Calling callback if any */
			link->on_unlink(origin, link->plink, g_ape);
		}
		free(link);
		link = plink;
	}
	origin->link = NULL;
}

/* to manage subuser use post_to_pipe() instead */
void post_raw_pipe(RAW *raw, const char *pipe, acetables *g_ape)
{
	transpipe *spipe;
	
	if ((spipe = get_pipe(pipe, g_ape)) != NULL) {
		if (spipe->type == CHANNEL_PIPE) {
			post_raw_channel(raw, spipe->pipe, g_ape);
		} else {
			post_raw(raw, spipe->pipe, g_ape);
		}
	}
}

void *get_pipe(const char *pubid, acetables *g_ape)
{
	if (strlen(pubid) != 32) {
		return NULL;
	}
	return hashtbl_seek(g_ape->hPubid, pubid);
}

/* pubid : recver; user = sender */
void *get_pipe_strict(const char *pubid, USERS *user, acetables *g_ape)
{
	transpipe *pipe = get_pipe(pubid, g_ape);
	
	if (pipe != NULL && pipe->type == CHANNEL_PIPE && !isonchannel(user, pipe->pipe)) {
		return NULL;
	}
	
	return pipe;
	
}

