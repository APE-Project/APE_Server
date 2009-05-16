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

/* sock.h */

#ifndef _SOCK_H
#define _SOCK_H

#include <sys/types.h> 
#include <netinet/in.h> 
#include <sys/socket.h> 
#include <sys/wait.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "main.h"

#define SENDH(x, y, g_ape) \
	sendbin(x, HEADER, HEADER_LEN, g_ape);\
	sendbin(x, y, strlen(y), g_ape)
	
#define CLOSE(x, g_ape) \
	sendbin(x, HEADER, HEADER_LEN, g_ape);\
	sendbin(x, "CLOSE", 5, g_ape)

#define QUIT(x, g_ape) \
	sendbin(x, HEADER, HEADER_LEN, g_ape);\
	sendbin(x, "QUIT", 4, g_ape)

int newSockListen(unsigned int port, char *listen_ip);
void setnonblocking(int fd);
int sendf(int sock, acetables *g_ape, char *buf, ...);
int sendbin(int sock, char *bin, int len, acetables *g_ape);
unsigned int sockroutine(int s_listen, acetables *g_ape);

struct _socks_bufout
{
	int fd;
	char *buf;
	int buflen;
	int allocsize;
};

#endif

