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

/* ace.h */


#ifndef _MODULES_APE_H
#define _MODULES_APE_H
#if __cplusplus
	extern "C" {
#endif

#include "../src/main.h"
#include "../src/channel.h"
#include "../src/cmd.h"
#include "../src/extend.h"
#include "../src/users.h"
#include "../src/utils.h"
#include "../src/plugins.h"
#include "../src/ticks.h"
#include "../src/pipe.h"
#include "../src/raw.h"
#include "../src/base64.h"
#include "../src/sha1.h"
#include "../src/events.h"
#include "../src/log.h"

#include <stdarg.h>

#define READ_CONF(key) plugin_get_conf(infos_module.conf, key)

typedef struct _ace_callbacks ace_callbacks;

struct _ace_callbacks
{		
	USERS *(*c_adduser)(USERS *, acetables *);
	void (*c_deluser)(USERS *, int istmp, acetables *);
	CHANNEL *(*c_mkchan)(char *, int, acetables *);
	void (*c_rmchan)(CHANNEL *, acetables *);
	void (*c_join)(USERS *, CHANNEL *, acetables *);
	void (*c_left)(USERS *, CHANNEL *, acetables *);
	void (*c_tickuser)(subuser *, acetables *);
	void (*c_post_raw_sub)(RAW *, subuser *, acetables *);
	USERS *(*c_allocateuser)(ape_socket *, char *, char *, acetables *);
	void (*c_addsubuser)(subuser *, acetables *);
	void (*c_delsubuser)(subuser *, acetables *);
};

typedef struct _plug_config plug_config;
struct _plug_config
{
	char *key;
	char *value;
	struct _plug_config *next;
};

#if __cplusplus
#define APE_PLUGIN_ENTRY_POINT extern "C"
#else
#define APE_PLUGIN_ENTRY_POINT
#endif

#define APE_INIT_PLUGIN(modname, initfunc, modcallbacks) \
	APE_PLUGIN_ENTRY_POINT void ape_module_init(ace_plugins *module) \
	{ \
		 infos_module.conf = NULL; \
		 module->cb = &modcallbacks; \
		 module->infos = &infos_module; \
		 module->loader = initfunc; \
		 module->modulename = modname; \
	}

#if __cplusplus
	}
#endif

#endif

