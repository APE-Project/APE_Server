#include <stdio.h>

#include "plugins.h"
#include "global_plugins.h"

#define MODULE_NAME "helloworld" // Unique identifier

/* You must name this "infos_module" because of macro READ_CONF */
static ace_plugin_infos infos_module = {
	"HelloWorld", 		// Module Name
	"0.01",			// Module Version
	"Anthony Catel",	// Module Author
	"helloworld.conf"	// Config file (from ./bin/) (can be NULL)
};




static unsigned int raw_helloworld(callbackp *callbacki)
{
	/* Sending a simple raw "Monkey" with "Helloworld" value */
	send_msg(callbacki->call_user, "Helloworld", "Monkey");
	
	/* Most used return */
	return (FOR_NOTHING);
}

static void init_module(acetables *g_ape) // Called when module is loaded (passed to APE_INIT_PLUGIN)
{
	/* Print "foo" key from helloworld.conf */
	printf("Helloworld loaded ;-) [Conf foo : %s]\n", READ_CONF("foo"));
	
	/* Adding a new raw GET /?q&HELLOWORLD&[SESSID]&[ANTICACHE] */
	register_raw("HELLOWORLD", 1, raw_helloworld, NEED_SESSID, g_ape);
}

/* Passed to callbacks list */
static USERS *helloworld_adduser(unsigned int fdclient, char *host, acetables *ace_tables)
{
	/*								*/
	/* Everything put here will be executed BEFORE the user has bee added 	*/
	/*								*/
						
								
	/* Call parent function (can be another plugin callback) */
	USERS *n = adduser(fdclient, host, ace_tables);
	
	if (n == NULL) {
		printf("NULL1\n");
	}
	/*								*/
	/* Everything put here will be executed AFTER the user has been added 	*/
	/*								*/
	
	/*if (n == NULL) {
		return NULL;
	}*/
	printf("[Helloworld !] => user added\n");
	
	/* Parent result must be returned (or NULL) */
	return NULL;	
}




/* See plugins.h for prototype */
static ace_callbacks callbacks = {
	helloworld_adduser,		/* Called when new user is added */
	NULL,				/* Called when a user is disconnected */
	NULL,				/* Called when new chan is created */
	NULL,				/* Called when a user join a channel */
	NULL				/* Called when a user leave a channel */
};

/* Registering module (arg1 : unique identifier, arg2 : init function, arg3 : Callbacks list) */
APE_INIT_PLUGIN(MODULE_NAME, init_module, callbacks)
