#include "plugins.h"
#include "global_plugins.h"

#define MODULE_NAME "control"

#define MAX_CONNECT 32

static ace_plugin_infos infos_module = {
	"Controller", // Module Name
	"0.90",		// Module Version
	"Anthony Catel",// Module Author
	"control.conf" // config file (bin/)
};


static int nconnected;

static struct _control_users {
	int fd;
	struct {
		int adduser;
		int deluser;
		int mkchan;
	} callbacks;
} *ucontrol;

static void control_cmd_listen(struct _control_users *pcontrol, char **params, int nparam, acetables *g_ape);
static void control_cmd_push(struct _control_users *pcontrol, char **params, int nparam, acetables *g_ape);

static struct _control_cmds {
	char *cmd;
	void (*func)(struct _control_users *pcontrol, char **params, int nparam, acetables *g_ape);
} ccontrol[] = {
	{"LISTEN", control_cmd_listen},
	{"PUSH", control_cmd_push},
	{NULL, NULL}
};

/* LISTEN event [...] */
static void control_cmd_listen(struct _control_users *pcontrol, char **params, int nparam, acetables *g_ape)
{
	int i;

	if (!nparam) {
		sendf(pcontrol->fd, g_ape, "300 Missing params\n");
		return;
	}
	
	for (i = 0; i < nparam; i++) {
		if (strcasecmp(params[i], "adduser") == 0) {
			pcontrol->callbacks.adduser ^= 1;
			sendf(pcontrol->fd, g_ape, "200 adduser %i\n", pcontrol->callbacks.adduser);
		} else if (strcasecmp(params[i], "deluser") == 0) {
			pcontrol->callbacks.deluser ^= 1;
			sendf(pcontrol->fd, g_ape, "200 deluser %i\n", pcontrol->callbacks.deluser);			
		} else if (strcasecmp(params[i], "mkchan") == 0) {
			pcontrol->callbacks.mkchan ^= 1;
			sendf(pcontrol->fd, g_ape, "200 mkchan %i\n", pcontrol->callbacks.mkchan);			
		} else {
			sendf(pcontrol->fd, g_ape, "301 Bad params : %s\n", params[i]);
		}
	}
}

/* PUSH pubid RAW Message(url encoded) */
static void control_cmd_push(struct _control_users *pcontrol, char **params, int nparam, acetables *g_ape)
{
	RAW *newraw;
	json *jlist = NULL;

	if (nparam != 3) {
		sendf(pcontrol->fd, g_ape, "300 Missing params\n");
		return;		
	}
	
	set_json("value", params[2], &jlist);
	
	newraw = forge_raw(params[1], jlist);	
	
	if (!post_raw_pipe(newraw, params[0], g_ape)) {
		sendf(pcontrol->fd, g_ape, "303 Unknown pipe\n");
	} else {
		sendf(pcontrol->fd, g_ape, "201 Raw sent\n");
	}
	
}

static void control_accept(ape_socket *client, acetables *g_ape)
{
	nconnected++;
	
	client->attach = NULL;
	
	if (nconnected > MAX_CONNECT) {
		sendf(client->fd, g_ape, "400 Too many connections\n");
		shutdown(client->fd, 2);
		return;
	}
	sendf(client->fd, g_ape, "001 Welcome to APE Controller\n");
	
	ucontrol[nconnected-1].fd = client->fd;
	ucontrol[nconnected-1].callbacks.adduser = 0;
	ucontrol[nconnected-1].callbacks.deluser = 0;
	ucontrol[nconnected-1].callbacks.mkchan = 0;
	
	client->attach = &ucontrol[nconnected-1];
}


static void control_disconnect(ape_socket *client, acetables *g_ape)
{
	struct _control_users *cu;
	nconnected--;
	
	if (client->attach != NULL) {
		cu = (struct _control_users *)client->attach;
		cu->fd = 0;
		cu->callbacks.adduser = 0;
		cu->callbacks.deluser = 0;
		cu->callbacks.mkchan = 0;
	}
}

static void control_read_lf(ape_socket *client, char *data, acetables *g_ape)
{
	char *tkn[16];
	int nTok, i;
	
	nTok = explode(' ', data, tkn, 16);

	for (i = 0; ccontrol[i].cmd != NULL; i++) {
		if (strcasecmp(ccontrol[i].cmd, tkn[0]) == 0) {
			ccontrol[i].func(client->attach, (nTok ? &tkn[1] : NULL), nTok, g_ape);
			return;
		}
	}
	sendf(client->fd, g_ape, "302 Unknown command\n");
}

static void init_module(acetables *g_ape)
{
	ape_socket *control_server;
	if ((control_server = ape_listen(atoi(READ_CONF("port")), READ_CONF("ip_listen"), g_ape)) == NULL) {
		return;
	}
	
	nconnected = 0;
	ucontrol = xmalloc(sizeof(*ucontrol) * MAX_CONNECT);
	memset(ucontrol, 0, sizeof(*ucontrol) * MAX_CONNECT);
	
	control_server->callbacks.on_accept = control_accept;
	control_server->callbacks.on_disconnect = control_disconnect;
	control_server->callbacks.on_read_lf = control_read_lf;
		
	printf("[Module] Controller module is now listening on %s\n", READ_CONF("port"));
}

static USERS *control_adduser(unsigned int fdclient, char *host, acetables *g_ape)
{
	RAW *newraw;
	json *jlist = NULL;
	int i;
	USERS *user = adduser(fdclient, host, g_ape);
	
	if (user != NULL) {
	
		set_json("user", NULL, &jlist);
		json_attach(jlist, get_json_object_user(user), JSON_OBJECT);
	
		newraw = forge_raw("ADDUSER", jlist);
	
		for (i = 0; i < MAX_CONNECT; i++) {
			if (ucontrol[i].fd && ucontrol[i].callbacks.adduser) {
				sendbin(ucontrol[i].fd, newraw->data, newraw->len, g_ape);
				sendbin(ucontrol[i].fd, "\n", 1, g_ape);
			}
		}
	
		free_raw(newraw);
	}
	
	return user;
}

static void control_deluser(USERS *user, acetables *g_ape)
{
	RAW *newraw;
	json *jlist = NULL;
	int i;

	set_json("user", NULL, &jlist);
	json_attach(jlist, get_json_object_user(user), JSON_OBJECT);
	
	newraw = forge_raw("DELUSER", jlist);
	
	for (i = 0; i < MAX_CONNECT; i++) {
		if (ucontrol[i].fd && ucontrol[i].callbacks.adduser) {
			sendbin(ucontrol[i].fd, newraw->data, newraw->len, g_ape);
			sendbin(ucontrol[i].fd, "\n", 1, g_ape);
		}
	}
	
	free_raw(newraw);
	
	deluser(user, g_ape);
}

static CHANNEL *control_mkchan(char *channel, char *topic, acetables *g_ape)
{
	RAW *newraw;
	json *jlist = NULL;
	int i;
	CHANNEL *chan = mkchan(channel, topic, g_ape);
	
	if (chan != NULL) {
		set_json("channel", NULL, &jlist);
		json_attach(jlist, get_json_object_channel(chan), JSON_OBJECT);
	
		newraw = forge_raw("MKCHAN", jlist);
	
		for (i = 0; i < MAX_CONNECT; i++) {
			if (ucontrol[i].fd && ucontrol[i].callbacks.mkchan) {
				sendbin(ucontrol[i].fd, newraw->data, newraw->len, g_ape);
				sendbin(ucontrol[i].fd, "\n", 1, g_ape);
			}
		}
	
		free_raw(newraw);		
	}
	
	return chan;
}

static ace_callbacks callbacks = {
	control_adduser,				/* Called when new user is added */
	control_deluser,				/* Called when a user is disconnected */
	control_mkchan,				/* Called when new chan is created */
	NULL,				/* Called when a user join a channel */
	NULL				/* Called when a user leave a channel */
};

APE_INIT_PLUGIN(MODULE_NAME, init_module, callbacks)

