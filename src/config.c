/*
  Copyright (C) 2006, 2007, 2008  Anthony Catel <a.catel@weelya.com>

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

/* config.c */

#include "main.h"
#include "config.h"
#include "utils.h"

srvconfig *load_ace_config(const char *path_config) 
{
	FILE *cfile;
	
	char lines[MAX_IO+1], *tkn[32+1];
	
	srvconfig *srv;
	size_t nTok;

	srv = (srvconfig *) xmalloc(sizeof(*srv));
	printf("\nReading Config...");
	if (NULL == (cfile = fopen(path_config, "r"))) {
		printf("NO (unable to open %s)\n", path_config);
		return NULL;
	}
	printf("YES\n");
	srv->port = 0;
	srv->max_connected = 0;

	memset(srv->fConnected, '\0', sizeof(srv->fConnected));
	memset(srv->daemon, '\0', sizeof(char));
	memset(srv->domain, '\0', sizeof(char));
	
	
	while(fgets(lines, MAX_IO, cfile)) {
		if (*lines == '#' || *lines == '\n' || *lines == '\0') {
			continue;
		}
		if (lines[strlen(lines)-1]=='\n' && removelast(lines, 1) == NULL) {
			continue;
		}
		nTok = explode('=', lines, tkn, 16);
		if (nTok == 1) {
			if (strcmp(tkn[0], "port")==0) {
				if (atoi(tkn[1]) < 1 || atoi(tkn[1]) > 65535) {
					printf("Erreur: Port range <1-65535>\n");
					return NULL;
				}
				srv->port = atoi(tkn[1]);
				continue;
			} else if (strcmp(tkn[0], "max_connected")==0) {
				if (atoi(tkn[1]) < 0) {
					srv->max_connected = 0;
					continue;
				}
				srv->max_connected = atoi(tkn[1]);
				continue;
			} else if (strcmp(tkn[0], "connectedfile")==0) {
				memcpy(srv->fConnected, tkn[1], strlen(tkn[1])+1);
				continue;
			} else if (strcmp(tkn[0], "daemon")==0) {
				memcpy(srv->daemon, tkn[1], strlen(tkn[1])+1);
				continue;
			} else if (strcmp(tkn[0], "domain")==0) {
				if (strlen(tkn[1])+1 < 512) {
					memcpy(srv->domain, tkn[1], strlen(tkn[1])+1);
				}
			}
		} else {
			printf("Erreur: fichier de configuration non conforme\n");
			return NULL;
		}
	}
	if (	
		strlen(srv->daemon)==0 ||
		srv->port==0) {
			printf("Erreur: Fichier de configuration incomplet.\n");
			return NULL;
		}
	return srv;
}
