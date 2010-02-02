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
			input[i] = basic_chars[rand_n(15)];
		}
		input[32] = '\0';
	} while(seek_user_id(input, g_ape) != NULL || get_pipe(input, g_ape) != NULL); // Colision verification
}

/* Init a pipe (user, channel, proxy) */
transpipe *init_pipe(void *pipe, int type, acetables *g_ape)
{
	transpipe *npipe = NULL;
	
	npipe = xmalloc(sizeof(*npipe));
	npipe->pipe = pipe;
	npipe->type = type;
	npipe->link = NULL;
	npipe->data = NULL;
	npipe->on_send = NULL;
	npipe->properties = NULL;
	
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

transpipe *get_pipe(const char *pubid, acetables *g_ape)
{
	if (strlen(pubid) != 32) {
		return NULL;
	}
	return hashtbl_seek(g_ape->hPubid, pubid);
}

/* pubid : recver; user = sender */
transpipe *get_pipe_strict(const char *pubid, USERS *user, acetables *g_ape)
{
	transpipe *pipe = get_pipe(pubid, g_ape);
	
	if (pipe != NULL && pipe->type == CHANNEL_PIPE && !isonchannel(user, pipe->pipe)) {
		return NULL;
	}
	
	return pipe;
	
}

void post_json_custom(json_item *jstr, USERS *user, transpipe *pipe, acetables *g_ape)
{
	if (pipe->on_send == NULL) {
		free_json_item(jstr);
		return;
	}
	
	pipe->on_send(pipe, user, jstr, g_ape);
}

json_item *get_json_object_pipe_custom(transpipe *pipe)
{
	json_item *jstr = NULL;
	
	if (pipe != NULL) {
		jstr = json_new_object();
		json_set_property_strN(jstr, "casttype", 8, "custom", 6);
		json_set_property_strN(jstr, "pubid", 5, pipe->pubid, 32);
		
		if (pipe->properties != NULL) {
			int has_prop = 0;
			
			json_item *jprop = NULL;
						
			extend *eTmp = pipe->properties;
			
			while (eTmp != NULL) {
				if (eTmp->visibility == EXTEND_ISPUBLIC) {
					if (!has_prop) {
						has_prop = 1;
						jprop = json_new_object();
					}
					if (eTmp->type == EXTEND_JSON) {
						json_item *jcopy = json_item_copy(eTmp->val, NULL);
						
						json_set_property_objZ(jprop, eTmp->key, jcopy);
					} else {
						json_set_property_strZ(jprop, eTmp->key, eTmp->val);

					}			
				}
				eTmp = eTmp->next;
			}
			if (has_prop) {
				json_set_property_objN(jstr, "properties", 10, jprop);
			}
		}

	}
	return jstr;
}

json_item *get_json_object_pipe(transpipe *pipe)
{
	switch(pipe->type) {
		case USER_PIPE:
			return get_json_object_user(pipe->pipe);
		case CHANNEL_PIPE:
			return get_json_object_channel(pipe->pipe);
		case CUSTOM_PIPE:
			return get_json_object_pipe_custom(pipe);
		case PROXY_PIPE:
		default:
			return NULL;
	}
}

