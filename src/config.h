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

/* config.h */

#ifndef _CONFIG_H
#define _CONFIG_H


#define APE_CONFIG_FILE "ape.conf"


struct _apeconfig_def {
	char *val;
	struct _apeconfig_def *next;
	char key[33];
};

typedef struct apeconfig {
	struct _apeconfig_def *def;
	struct apeconfig *next;
	char section[33];
} apeconfig;


apeconfig *ape_config_load(const char *filename);
char *ape_config_get_key(apeconfig *conf, const char *key);
apeconfig *ape_config_get_section(apeconfig *conf, const char *section);

#define CONFIG_VAL(section, key, srv) \
	(ape_config_get_key(ape_config_get_section(srv, #section), #key) == NULL ? "" : ape_config_get_key(ape_config_get_section(srv, #section), #key))

#endif

