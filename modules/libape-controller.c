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
	
	char *password, *channel, *raw, *value;
	
	APE_PARAMS_INIT();
	
	if ((password = JSTR(password)) != NULL && (channel = JSTR(channel)) != NULL && (value = JSTR(value)) != NULL) {
	
		if (strcmp(password, READ_CONF("password")) != 0) {
			SENDH(callbacki->fdclient, "ERR BAD_PASSWORD", callbacki->g_ape);
		
		} else if ((jchan = getchan(channel, callbacki->g_ape)) == NULL) {
			SENDH(callbacki->fdclient, "ERR NOT_A_CHANNEL", callbacki->g_ape);
		
		} else {
			
			send_msg_channel(jchan, value, raw, callbacki->g_ape);
			SENDH(callbacki->fdclient, "OK POSTED", callbacki->g_ape);
			
		}
		
		return (RETURN_NOTHING);
	}
	return (RETURN_BAD_PARAMS);
}

static void init_module(acetables *g_ape) // Called when module is loaded
{

	register_cmd("CONTROL", cmd_control, NEED_NOTHING, g_ape);
}


static ace_callbacks callbacks = {
	NULL,				/* Called when new user is added */
	NULL,				/* Called when a user is disconnected */
	NULL,				/* Called when new chan is created */
	NULL,				/* Called when a user join a channel */
	NULL				/* Called when a user leave a channel */
};

APE_INIT_PLUGIN(MODULE_NAME, init_module, callbacks)

