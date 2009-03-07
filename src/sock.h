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

/* sock.h */

#ifndef _SOCK
#define _SOCK

#include <sys/types.h> 
#include <netinet/in.h> 
#include <sys/socket.h> 
#include <sys/wait.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "main.h"

#define ENVOI(x, y) sendf(x, "%s%s", HEADER, y)
#define CLOSE(x) sendf(x, "%sCLOSE\n", HEADER)
#define QUIT(x) sendf(x, "%sQUIT\n", HEADER)


void setnonblocking(int fd);
int sendf(int sock, char *buf, ...);
unsigned int sockroutine(size_t port, acetables *g_ape);



#endif
