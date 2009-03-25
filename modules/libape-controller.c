/*

*/

#include "plugins.h"
#include "global_plugins.h"

#define MODULE_NAME "controller"

static ace_plugin_infos infos_module = {
	"Controller", // Module Name
	"0.01",		// Module Version
	"Anthony Catel",// Module Author
	"controller.conf" // config file (bin/)
};

static unsigned int cmd_control(callbackp *callbacki)
{
	CHANNEL *jchan;

	
	if (strcmp(callbacki->param[1], READ_CONF("password")) != 0) {
		SENDH(callbacki->fdclient, "ERR BAD_PASSWORD");
		
	} else if ((jchan = getchan(callbacki->param[2], callbacki->g_ape)) == NULL) {
		SENDH(callbacki->fdclient, "ERR NOT_A_CHANNEL");
		
	} else {
		if (strcasecmp(callbacki->param[3], "POSTMSG") == 0) {
			send_msg_channel(jchan, callbacki->param[5], callbacki->param[4]);
			SENDH(callbacki->fdclient, "OK POSTED");
		}
	}
	return (FOR_NOTHING);
}

static void init_module(acetables *g_ape) // Called when module is loaded
{

	register_cmd("CONTROL", 5, cmd_control, NEED_NOTHING, g_ape);
}


static ace_callbacks callbacks = {
	NULL,				/* Called when new user is added */
	NULL,			/* Called when a user is disconnected */
	NULL,				/* Called when new chan is created */
	NULL,				/* Called when a user join a channel */
	NULL				/* Called when a user leave a channel */
};

APE_INIT_PLUGIN(MODULE_NAME, init_module, callbacks)

