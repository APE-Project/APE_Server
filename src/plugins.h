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

/* plugins.h */


#ifndef _PLUGINS_H
#define _PLUGINS_H

#include "main.h"
#include "../modules/plugins.h"


typedef struct _ace_plugin_infos ace_plugin_infos;
typedef struct _ace_plugins ace_plugins;


struct _ace_plugin_infos
{
	const char *name;
	const char *version;
	const char *author;
	
	const char *conf_file;
	
	struct _plug_config *conf;
};


struct _ace_plugins
{
	/* Module Handle */
	void *hPlug;
	
	const char *modulename;
	
	/* Module info */
	ace_plugin_infos *infos;
	
	/* Init function */
	void (*loader)(acetables *g_ape);

	/* Module callback entry point */
	struct _ace_callbacks *cb;
	
	struct {
		unsigned int c_adduser;
		unsigned int c_deluser;
		unsigned int c_mkchan;
		unsigned int c_rmchan;
		unsigned int c_join;
		unsigned int c_left;
		unsigned int c_tickuser;
		unsigned int c_post_raw_sub;

	} fire;

	ace_plugins *next;
};


enum {
	CALLBACK_PLUGIN = 0,
	CALLBACK_NULL
};

ace_plugins *loadplugin(char *file);
void unfire(ace_plugins *plug);
void findandloadplugin(acetables *g_ape);
struct _plug_config *plugin_parse_conf(const char *file);
void plugin_read_config(ace_plugins *plug);
char *plugin_get_conf(struct _plug_config *conf, char *key);

#endif

