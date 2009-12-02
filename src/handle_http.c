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

/* handle_http.c */

#include <string.h>

#include "handle_http.h"
#include "utils.h"
#include "config.h"
#include "cmd.h"

static unsigned int fixpacket(char *pSock, int type)
{
	size_t i, pLen;
	
	pLen = strlen(pSock);
	
	for (i = 0; i < pLen; i++) {
			if (type == 0 && (pSock[i] == '\n' || pSock[i] == '\r')) {
				pSock[i] = '\0';
			
				return 1;
			} else if (type == 1 && pSock[i] == ' ') {
				pSock[i] = '\0';
			
				return 1;				
			}
		
	}
	return 0;
}

/* Reading the host http header */
static int gethost(char *base, char *output) // Catch the host HTTP header
{
	char *pBase;
	int i;
	
	output[0] = '\0';
	
	for (pBase = base; *pBase && strncasecmp(pBase, "Host:", 5) != 0; pBase++);
	
	if (!*pBase || !pBase[6]) {
		return 0;
	}
	
	pBase = &pBase[6];
	
	for (i = 0; pBase[i] && pBase[i] != '\n' && i < (MAX_HOST_LENGTH-1); i++) {
		output[i] = pBase[i];
	}
	
	output[i] = '\0';
	
	return 1;
}

/* Reading post data from the HTTP streaming incoming */
static char *getpost(char *input)
{
	char *pInput;
	
	for (pInput = input; *pInput && strncmp(pInput, "\r\n\r\n", 4) != 0; pInput++);
	
	if (!*pInput || !pInput[4]) {
		return NULL;
	} else {
		return &pInput[4];
	}
}

static int getqueryip(char *base, char *output)
{
	int i, size = strlen(base), step, x = 0;
	
	if (size < 16) {
		return 0;
	}
	
	for (i = 0, step = 0; i < size; i++) {
		if (base[i] == '\n') {
			output[0] = '\0';
			return 0;
		}
		if (step == 1 && (base[i] == '&' || base[i] == ' ') && x < 16) {
			output[x] = '\0';
			return 1;
		} else if (step == 1 && x < 16) {
			output[x] = base[i];
			x++;
		} else if (base[i] == '?') {
			step = 1;
		}
	}
	output[0] = '\0';
	return 0;
	
}

static char *getfirstparam(char *input, char sep)
{

	char *pInput;
	/*
		Should be replaced by a simple strchr
	*/	
	for (pInput = input; *pInput && *pInput != sep; pInput++);
	
	if (!*pInput || !pInput[1]) {
		return NULL;
	} else {
		return &pInput[1];
	}	
}

static int gettransport(char *input)
{
	char *start = strchr(input, '/');

	if (start != NULL && start[1] >= 48 && start[1] <= 53 && start[2] == '/') {
		return start[1]-48;
	}
	
	return 0;
}

subuser *checkrecv(char *pSock, ape_socket *client, acetables *g_ape, char *ip_client)
{

	unsigned int op;
	unsigned int isget = 0;
	
	subuser *user = NULL;
	int local = (strcmp(ip_client, CONFIG_VAL(Server, ip_local, g_ape->srv)) == 0);
	
	clientget *cget = xmalloc(sizeof(*cget));

	if (strlen(pSock) < 3 || (local && getqueryip(pSock, cget->ip_get) == 0)) {  // get query IP (from htaccess)
		free(cget);
		shutdown(client->fd, 2);
		return NULL;		
	}
	if (!local) {
		strncpy(cget->ip_get, ip_client, 16); // get real IP (from socket)
	}
	
	cget->client = client;
	
	gethost(pSock, cget->host);
	
	if (strncasecmp(pSock, "GET", 3) == 0) {
		if (!fixpacket(pSock, 0) || (cget->get = getfirstparam(pSock, (local ? '&' : '?'))) == NULL) {
			free(cget);
			
			shutdown(client->fd, 2);
			return NULL;			
		} else {
			isget = 1;
		}
	} else if (strncasecmp(pSock, "POST", 4) == 0) {
		if ((cget->get = getpost(pSock)) == NULL) {
			free(cget);
			
			shutdown(client->fd, 2);
			return NULL;			
		}
	} else {
		free(cget);

		shutdown(client->fd, 2);
		return NULL;		
	}
	
	fixpacket(cget->get, 1);

	if (isget) {
		urldecode(cget->get);
	}
	
	op = checkcmd(cget, gettransport(pSock), &user, g_ape);

	switch (op) {
		case CONNECT_SHUTDOWN:
			shutdown(client->fd, 2);			
			break;
		case CONNECT_KEEPALIVE:
			break;
	}

	free(cget);
	
	return user;

}

