/*
	Adding support of nickname
	CONNECT raw is overrided with new param (nickname) GET /?IP&CONNECT&nickname&anticache HTTP/1.1
	USER object has now a property identidied by "nickname"
	
	TODO : Move topic system, level, ban, kick, etc..
*/

#include "plugins.h"
#include "global_plugins.h"


#define MODULE_NAME "chat"

#define ERR_NICK_USED 		"[\n{\"raw\":\"ERR\",\"time\":null,\"datas\":{\"code\":\"005\",\"value\":\"NICK_USED\"}}\n]\n"
#define ERR_BAD_NICK		"[\n{\"raw\":\"ERR\",\"time\":null,\"datas\":{\"code\":\"006\",\"value\":\"BAD_NICK\"}}\n]\n"

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
	json *jprop = NULL;
	struct json *jstr = NULL;

	
	if (!isvalidnick(callbacki->param[1])) {
		SENDH(callbacki->fdclient, ERR_BAD_NICK, callbacki->g_ape);
		
		return (FOR_NOTHING);		
	}
	if (get_user_by_nickname(callbacki->param[1], callbacki->g_ape)) {
		SENDH(callbacki->fdclient, ERR_NICK_USED, callbacki->g_ape);
		
		return (FOR_NOTHING);
	}
	
	nuser = adduser(callbacki->fdclient, callbacki->host, callbacki->g_ape);
	
	callbacki->call_user = nuser;
	
	if (nuser == NULL) {
		SENDH(callbacki->fdclient, ERR_CONNECT, callbacki->g_ape);
		
		return (FOR_NOTHING);
	}
	if (strcmp(callbacki->param[2], "2") == 0) {
		nuser->transport = TRANSPORT_IFRAME;
		nuser->flags |= FLG_PCONNECT;
	} else {
		nuser->transport = TRANSPORT_LONGPOLLING;
	}
	hash_user(nuser, callbacki->param[1], callbacki->g_ape);
	
	set_json("name", callbacki->param[1], &jprop);
	set_json("test", callbacki->param[2], &jprop);
	
	add_property(&nuser->properties, "name", jprop, EXTEND_JSON, EXTEND_ISPUBLIC);
	
		
	subuser_restor(getsubuser(callbacki->call_user, callbacki->host));
	
	set_json("sessid", nuser->sessid, &jstr);
	
	newraw = forge_raw(RAW_LOGIN, jstr);
	newraw->priority = 1;
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
	
	add_property(&user->properties, "name", nick, EXTEND_STR, EXTEND_ISPUBLIC);
	
	while (clist != NULL) {
		//RAW *new_raw;
		
		clist = clist->next;
	}
	//TODO
}

static void init_module(acetables *g_ape) // Called when module is loaded
{
	// Adding hashtable identified by "nicklist" to g_ape properties
	add_property(&g_ape->properties, "nicklist", hashtbl_init(), EXTEND_POINTER, EXTEND_ISPRIVATE);

	// Overriding connect raw
	register_cmd("CONNECT",	2, chat_connect, NEED_NOTHING, g_ape);
}


static ace_callbacks callbacks = {
	NULL,				/* Called when new user is added */
	chat_deluser,			/* Called when a user is disconnected */
	NULL,				/* Called when new chan is created */
	NULL,				/* Called when a user join a channel */
	NULL				/* Called when a user leave a channel */
};

APE_INIT_PLUGIN(MODULE_NAME, init_module, callbacks)

