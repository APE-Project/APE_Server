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
	struct {
		unsigned short int c_adduser;
		unsigned short int c_deluser;
		unsigned short int c_mkchan;
		unsigned short int c_rmchan;
		unsigned short int c_join;
		unsigned short int c_left;
		unsigned short int c_tickuser;
		unsigned short int c_post_raw_sub;
		unsigned short int c_allocateuser;
		unsigned short int c_addsubuser;
		unsigned short int c_delsubuser;
	} fire;
	
	/* Module Handle */
	void *hPlug;
	void (*loader)(acetables *g_ape);
	
	const char *modulename;
	
	struct _ace_callbacks *cb;
	/* Module info */
	ace_plugin_infos *infos;
	ace_plugins *next;
};


enum {
	CALLBACK_PLUGIN = 0,
	CALLBACK_NULL
};

ace_plugins *loadplugin(char *file);
void findandloadplugin(acetables *g_ape);
struct _plug_config *plugin_parse_conf(const char *file);
void plugin_read_config(ace_plugins *plug);
char *plugin_get_conf(struct _plug_config *conf, char *key);

#endif

