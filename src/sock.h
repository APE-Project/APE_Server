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
#include <netinet/tcp.h>
#include <sys/socket.h> 
#include <sys/wait.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "main.h"

#define TCP_TIMEOUT 20 // ~Timeout if the socket is not identified to APE


struct _socks_bufout
{
	char *buf;
	int fd;
	int buflen;
	int allocsize;
};

struct _socks_list
{
	struct _ape_socket *co;
	int *tfd;
};

struct _ape_sock_connect_async
{
	ape_socket *sock;
	int port;
};

ape_socket *ape_listen(unsigned int port, char *listen_ip, acetables *g_ape);
ape_socket *ape_connect(char *ip, int port, acetables *g_ape);
void ape_connect_name(char *name, int port, ape_socket *pattern, acetables *g_ape);
void setnonblocking(int fd);
int sendf(int sock, acetables *g_ape, char *buf, ...);
int sendbin(int sock, const char *bin, unsigned int len, unsigned int burn_after_writing, acetables *g_ape);
void safe_shutdown(int sock, acetables *g_ape);
unsigned int sockroutine(acetables *g_ape);


#define SENDH(x, y, g_ape) \
	sendbin(x, HEADER_DEFAULT, HEADER_DEFAULT_LEN, 0, g_ape);\
	sendbin(x, y, strlen(y), 0, g_ape)


#define QUIT(x, g_ape) \
	sendbin(x, HEADER_DEFAULT, HEADER_DEFAULT_LEN, 0, g_ape);\
	sendbin(x, "QUIT", 4, 0, g_ape)
	
#ifdef TCP_CORK
	#define PACK_TCP(fd) \
		do { \
			int __state = 1; \
			setsockopt(fd, IPPROTO_TCP, TCP_CORK, &__state, sizeof(__state)); \
		} while(0)

	#define FLUSH_TCP(fd) \
	do { \
		int __state = 0; \
		setsockopt(fd, IPPROTO_TCP, TCP_CORK, &__state, sizeof(__state)); \
	} while(0)
#else
	#define PACK_TCP(fd)
	#define FLUSH_TCP(fd)
#endif

#endif
