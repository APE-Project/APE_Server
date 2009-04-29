#include <stdio.h>


#include "plugins.h"
#include "global_plugins.h"

#define MODULE_NAME "mouse"


/* You must name this "infos_module" because of macro READ_CONF */
static ace_plugin_infos infos_module = {
	"Mouse", 		// Module Name
	"0.01",			// Module Version
	"Anthony Catel",	// Module Author
	NULL			// Config file
};


static unsigned int cmd_setmouse(callbackp *callbacki)
{

	json *jlist;

	if (get_pipe_strict(callbacki->param[2], callbacki->call_user, callbacki->g_ape) == NULL) {

		send_error(callbacki->call_user, "UNKNOWN_PIPE", "109");

	} else {
		int i;
		jlist = NULL;
		
		set_json("pos", NULL, &jlist);
		
		for (i = callbacki->nParam; i >= 3; i--) {
			char *param[2];
			json *jeach = NULL;
			
			if (explode(',', callbacki->param[i], param, 2) != 1) {				
				continue;
			}
			
			set_json("y", param[1], &jeach);
			set_json("x", param[0], &jeach);
			
			json_attach(jlist, jeach, JSON_ARRAY);
		}

		
		//printf("Param : %i\n", callbacki->nParam);
		post_to_pipe(jlist, "MOUSEPOS", callbacki->param[2], getsubuser(callbacki->call_user, callbacki->host), NULL, callbacki->g_ape);
	}

	return (FOR_NOTHING);
}

static void init_module(acetables *g_ape) // Called when module is loaded
{
	register_cmd("SETMOUSE", -4, cmd_setmouse, NEED_SESSID, g_ape);
}

static ace_callbacks callbacks = {
	NULL,				/* Called when new user is added */
	NULL,				/* Called when a user is disconnected */
	NULL,				/* Called when new chan is created */
	NULL,				/* Called when a user join a channel */
	NULL				/* Called when a user leave a channel */
};

APE_INIT_PLUGIN(MODULE_NAME, init_module, callbacks)

