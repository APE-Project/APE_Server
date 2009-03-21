#include <stdio.h>


#include "plugins.h"
#include "global_plugins.h"

#define MODULE_NAME "setcord"


/* You must name this "infos_module" because of macro READ_CONF */
static ace_plugin_infos infos_module = {
	"Setcord", 		// Module Name
	"0.02",			// Module Version
	"Anthony Catel",	// Module Author
	NULL			// Config file
};


static unsigned int cmd_setpos(callbackp *callbacki)
{
	/* ->param[1] is not used (sessid) */
	int x = abs(atoi(callbacki->param[3])), y = abs(atoi(callbacki->param[4]));
	char cx[8], cy[8];
	
	if (x > 10000 || y > 10000) {
		send_error(callbacki->call_user, "BAD_POS", "302");
	} else {

		json *jlist;
		
		/* Get channel structure by name */
		if (get_pipe_strict(callbacki->param[2], callbacki->call_user, callbacki->g_ape) == NULL) {

			send_error(callbacki->call_user, "UNKNOWN_PIPE", "109");
		
		/* Check if calling user is on this channel */
		} else {
			jlist = NULL;
			
			/* Adding two persistant properties to calling user */
			add_property_str(&callbacki->call_user->properties, "x", itos(x, cx));
			add_property_str(&callbacki->call_user->properties, "y", itos(y, cy));
			
			post_to_pipe(jlist, "POSITIONS", callbacki->param[2], getsubuser(callbacki->call_user, callbacki->host), NULL, callbacki->g_ape);
		}
	}
	/* Nothing todo after */
	return (FOR_NOTHING);
}

static void init_module(acetables *g_ape) // Called when module is loaded
{
	register_cmd("SETPOS", 4, cmd_setpos, NEED_SESSID, g_ape);
}

static ace_callbacks callbacks = {
	NULL,				/* Called when new user is added */
	NULL,				/* Called when a user is disconnected */
	NULL,				/* Called when new chan is created */
	NULL,				/* Called when a user join a channel */
	NULL				/* Called when a user leave a channel */
};

APE_INIT_PLUGIN(MODULE_NAME, init_module, callbacks)

