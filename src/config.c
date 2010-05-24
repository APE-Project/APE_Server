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

/* config.c */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "config.h"
#include "utils.h"

apeconfig *ape_config_get_section(apeconfig *conf, const char *section)
{
	apeconfig *pConf = conf;

	while (pConf != NULL) {
		
		if (strcasecmp(pConf->section, section) == 0) {
			
			return pConf;
		}
		pConf = pConf->next;
	}
	
	return NULL;
}

char *ape_config_get_key(apeconfig *conf, const char *key)
{
	struct _apeconfig_def *def;
	
	if (conf == NULL) {
		return NULL;
	}

	def = conf->def;
	
	while (def != NULL) {
		if (strcasecmp(def->key, key) == 0) {
			return def->val;
		}
		def = def->next;
	}
	
	return NULL;
	
}

apeconfig *ape_config_load(const char *filename)
{
	FILE *fp = fopen(filename, "r");
	apeconfig *conf = NULL;
	char lines[2048], *tkn[3];
	
	int open_brace = 0, curline = 0;
	
	if (fp == NULL) {
		printf("NO (unable to open %s)\n", filename);
		return NULL;
	}

	
	while(fgets(lines, 2048, fp)) {
		int len;
		int i;
		curline++;
		
		if (*lines == '#' || *lines == '\r' || *lines == '\n' || *lines == '\0') {
			continue;
		}
		len = strlen(lines);
		
		switch(open_brace) {
			case 0:
				for (i = 0; i < len; i++) {
					if (lines[i] == '{') {
						apeconfig *config;
						 
						open_brace = 1;
						lines[i] = '\0';
						
						config = xmalloc(sizeof(*conf));
						
						strncpy(config->section, trim(lines), 32);
						
						config->section[32] = '\0';
						config->def = NULL;
						config->next = conf;
						conf = config;
						
						break;
					}
				}
				if (!open_brace) {
					printf("[Error] Parse error in configuration file (out of brace) at line %i\n", curline);
					return NULL;
				}
				break;
			case 1:
				for (i = 0; i < len; i++) {
					if (lines[i] == '}') {
						open_brace = 0;
						break;
					}
				}
				if (open_brace) {
					int nTok = 0;
					struct _apeconfig_def *def;
					
					nTok = explode('=', lines, tkn, 3);
					
					if (nTok > 1) {
						printf("[Error] Parse error in configuration file (illegal equality \"=\") at line %i\n", curline);
						return NULL;						
					}
					def = xmalloc(sizeof(*def));
					if (nTok == 1) {
						strncpy(def->key, trim(tkn[0]), 33);
						def->key[32] = '\0';
						
						def->val = xstrdup(trim(tkn[1]));
					} else {
						def->key[0] = '\0';
						def->val = xstrdup(trim(lines));
					}
					
					def->next = conf->def;
					conf->def = def;					
					
				}
				break;
		}
	}
	
	return conf;
}

void ape_config_free(apeconfig *conf)
{
	apeconfig *tmp_s;
	apeconfig_def *tmp_d;

	while (conf != NULL) {
		while (conf->def != NULL) {
			tmp_d = conf->def->next;
			free(conf->def->val);
			free(conf->def);
			conf->def = tmp_d;
		}
		tmp_s = conf->next;
		free(conf);
		conf = tmp_s;
	}
}

