/*
	Adding support of nickname
	CONNECT raw is overrided with new param (nickname) GET /?IP&CONNECT&nickname&anticache HTTP/1.1
	USER object has now a property identidied by "nickname"
	
	TODO : Move topic system, level, ban, kick, etc..
*/

#include "plugins.h"
#include "global_plugins.h"
#include "libape-mysql.h"

#define MODULE_NAME "chat"

static void hash_user(USERS *user, char *nick, acetables *g_ape);
static USERS *get_user_by_nickname(char *nick, acetables *g_ape);
/* This declaration is mandatory */

/* Donner un nom pour la resolution global */
static ace_plugin_infos infos_module = {
	"\"Chat\" system", // Module Name
	"0.01",		// Module Version
	"Anthony Catel",// Module Author
	NULL // config file (bin/)
};


static unsigned int isvalidnick(char *name) 
{
	char *pName;
	if (strlen(name) > MAX_NICK_LEN) {
		return 0;
	}
	for (pName = name; *pName != '\0'; pName++) {
		*pName = tolower(*pName);
		if (!isalnum(*pName) || ispunct(*pName)) {
			return 0;
		}
	}
	return 1;
}


static unsigned int chat_connect(callbackp *callbacki)
{
	USERS *nuser;
	RAW *newraw;
	
	struct json *jstr = NULL;
	
	
	if (!isvalidnick(callbacki->param[1])) {
		ENVOI(callbacki->fdclient, "BAD_NICKNAME");
		
		return (FOR_NOTHING);		
	}
	if (get_user_by_nickname(callbacki->param[1], callbacki->g_ape)) {
		ENVOI(callbacki->fdclient, "NICK_USED");
		
		return (FOR_NOTHING);
	}
	
	nuser = adduser(callbacki->fdclient, callbacki->host, callbacki->g_ape);
	
	callbacki->call_user = nuser;
	
	if (nuser == NULL) {
		ENVOI(callbacki->fdclient, ERR_CONNECT);
		
		return (FOR_NOTHING);
	}
	
	hash_user(nuser, callbacki->param[1], callbacki->g_ape);
	add_property_str(&nuser->properties, "name", callbacki->param[1]);


	set_json("sessid", nuser->sessid, &jstr);
	set_json("user", NULL, &jstr);
	
	json_attach(jstr, get_json_object_user(nuser), JSON_OBJECT);	
	
	newraw = forge_raw(RAW_LOGIN, jstr);

	post_raw(newraw, nuser);
	
	#if 0
	
	name = ape_mysql_get("SELECT name FROM user WHERE id = 1", callbacki->g_ape);
	
	printf("From char : %s\n", name);
	
	free(name);
	#endif
	
	return (FOR_LOGIN | FOR_UPDATE_IP);

}

static void chat_deluser(USERS *user, acetables *g_ape)
{
	hashtbl_erase(get_property(g_ape->properties, "nicklist")->val, get_property(user->properties, "name")->val);
	deluser(user, g_ape);
}

static void hash_user(USERS *user, char *nick, acetables *g_ape)
{
	// get_property(g_ape->properties, "nicklist")->val <= this return a hashtable identified by "nicklist"
	hashtbl_append(get_property(g_ape->properties, "nicklist")->val, nick, user);
}

static USERS *get_user_by_nickname(char *nick, acetables *g_ape)
{
	return hashtbl_seek(get_property(g_ape->properties, "nicklist")->val, nick);	

}

void change_nick(USERS *user, char *nick, acetables *g_ape)
{
	struct CHANLIST *clist = user->chan_foot;
	
	hashtbl_erase(get_property(g_ape->properties, "nicklist")->val, get_property(user->properties, "name")->val);
	hash_user(user, nick, g_ape);
	
	add_property_str(&user->properties, "name", nick);
	
	while (clist != NULL) {
		//RAW *new_raw;
		
		clist = clist->next;
	}
}

static void init_module(acetables *g_ape) // Called when module is loaded
{
	// Adding hashtable identified by "nicklist" to g_ape properties
	add_property(&g_ape->properties, "nicklist", hashtbl_init());

	// Overriding connect raw
	register_raw("CONNECT",	1, chat_connect, NEED_NOTHING, g_ape);
}


static ace_callbacks callbacks = {
	NULL,				/* Called when new user is added */
	chat_deluser,			/* Called when a user is disconnected */
	NULL,				/* Called when new chan is created */
	NULL,				/* Called when a user join a channel */
	NULL				/* Called when a user leave a channel */
};

APE_INIT_PLUGIN(MODULE_NAME, init_module, callbacks)

