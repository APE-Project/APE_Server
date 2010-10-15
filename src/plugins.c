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

/* plugins.c */

#include "plugins.h"
#include "../modules/plugins.h"
#include <dlfcn.h>
#include <glob.h>
#include "utils.h"
#include "config.h"


ace_plugins *loadplugin(char *file)
{
	void *hwnd;
	//ace_plugin_infos *infos;
	ace_plugins *plug;
	
	hwnd = dlopen(file, RTLD_LAZY);
	
	if (!hwnd) {
		printf("[Module] Failed to load %s [Invalid library] (%s)\n", file, dlerror());
		return NULL;
	}

	plug = xmalloc(sizeof(*plug));
	
	plug->hPlug = hwnd;
	
	/*
	plug->conf.file = infos->conf;
	plug->conf.entry = dlsym(hwnd, "this_config");*/

	
	plug->next = NULL;
	plug->cb = NULL;
	
	return plug;
}

void findandloadplugin(acetables *g_ape)
{
	int i;
	char modules_path[1024];
	
	sprintf(modules_path, "%s*.so", CONFIG_VAL(Config, modules, g_ape->srv));
	
	void (*load)(ace_plugins *module);
	
	glob_t globbuf;
	glob(modules_path, 0, NULL, &globbuf);

	
	for (i = 0; i < globbuf.gl_pathc; i++) {
		ace_plugins *pcurrent;
		pcurrent = loadplugin(globbuf.gl_pathv[i]);
		
		if (pcurrent != NULL) {
			ace_plugins *plist = g_ape->plugins;
			
			load = dlsym(pcurrent->hPlug, "ape_module_init");
			if (load == NULL) {
				printf("[Module] Failed to load %s [No load entry point]\n", globbuf.gl_pathv[i]);
				free(pcurrent);
				continue;
			}
			
			memset(&pcurrent->fire, 0, sizeof(pcurrent->fire)); /* unfire all events */
			
			/* Calling entry point load function */
			load(pcurrent);
				
			plugin_read_config(pcurrent, CONFIG_VAL(Config, modules_conf, g_ape->srv));
			
			if (!g_ape->is_daemon) {
				printf("[Module] [%s] Loading module : %s (%s) - %s\n", pcurrent->modulename, pcurrent->infos->name, pcurrent->infos->version, pcurrent->infos->author);
			}
			pcurrent->next = plist;
			g_ape->plugins = pcurrent;
			
			/* Calling init module */		
			pcurrent->loader(g_ape);

		}
		
	}
	globfree(&globbuf);

}

void free_all_plugins(acetables *g_ape)
{
	ace_plugins *next;

	while (g_ape->plugins != NULL) {
		g_ape->plugins->unloader(g_ape);
//		dlclose(g_ape->plugins->hPlug);

		next = g_ape->plugins->next;
		free(g_ape->plugins);
		g_ape->plugins = next;
	}
}

plug_config *plugin_parse_conf(const char *file)
{
	char lines[2048], *tkn[2];
	FILE *fp = fopen(file, "r");
	if (fp == NULL) {
		return NULL;
	}
	plug_config *tmpconf = NULL, *new_conf = NULL;
	
	while(fgets(lines, 2048, fp)) {
		int nTok = 0;
	
		if (*lines == '#' || *lines == '\r' || *lines == '\n' || *lines == '\0') {
			continue;
		}
		
		nTok = explode('=', lines, tkn, 2);
		
		if (nTok == 1) {
			new_conf = xmalloc(sizeof(struct _plug_config));
			new_conf->key = xstrdup(trim(tkn[0]));
			new_conf->value = xstrdup(trim(tkn[1]));
			new_conf->next = tmpconf;
			tmpconf = new_conf;
		}
	}
	fclose(fp);
	return new_conf;
}

void plugin_read_config(ace_plugins *plug, const char *path)
{
	char conf_file[1024];
	if (plug->infos->conf_file != NULL) {
		sprintf(conf_file, "%s%s", path, plug->infos->conf_file);
		if ((plug->infos->conf = plugin_parse_conf(conf_file)) == NULL) {
			printf("[Module] [%s] [WARN] Cannot open configuration (%s)\n", plug->modulename, plug->infos->conf_file);
			return;			
		}
	}
}

char *plugin_get_conf(struct _plug_config *conf, char *key)
{
	plug_config *current = conf;
	
	while(current != NULL) {
		if (strcmp(current->key, key) == 0) {
			return current->value;
		}
		current = current->next;
	}

	return NULL;
}

