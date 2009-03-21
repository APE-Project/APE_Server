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

/* handle_http.h */

#ifndef _HANDLE_HTTP_H
#define _HANDLE_HTTP_H

#include "users.h"
#include "main.h"

#include "sock.h"


subuser *checkrecv(char *pSock, int fdclient, acetables *g_ape, char *ip_client);

char *getpost(char *input);
int getqueryip(char *base, char *output);
char *getfirstparam(char *input);


typedef struct clientget
{
	int fdclient;
	char *get;
	char ip_client[16];
	char ip_get[16];
	char host[MAX_HOST_LENGTH];
} clientget ;

enum {
	CONNECT_SHUTDOWN = 0,
	CONNECT_KEEPALIVE
};

#endif

