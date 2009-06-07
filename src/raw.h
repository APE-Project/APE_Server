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

/* raw.h */

#ifndef _RAW_H
#define _RAW_H


#include "proxy.h"


typedef struct RAW
{
	char *data;
	int len;
	struct RAW *next;
	int priority;
} RAW;

RAW *forge_raw(const char *raw, struct json *jlist);
RAW *copy_raw(RAW *input);

void post_raw(RAW *raw, USERS *user, acetables *g_ape);
void post_raw_sub(RAW *raw, subuser *sub, acetables *g_ape);
void post_raw_channel(RAW *raw, struct CHANNEL *chan, acetables *g_ape);
void post_raw_restricted(RAW *raw, USERS *user, subuser *sub, acetables *g_ape);
void post_raw_channel_restricted(RAW *raw, struct CHANNEL *chan, USERS *ruser, acetables *g_ape);
void proxy_post_raw(RAW *raw, ape_proxy *proxy, acetables *g_ape);

int send_raws(subuser *user, acetables *g_ape);

#endif

