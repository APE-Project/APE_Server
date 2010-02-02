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

/* entry.c */

#include "plugins.h"
#include "main.h"
#include "sock.h"

#include "config.h"
#include "cmd.h"

#include "channel.h"

#include <signal.h>
#include <syslog.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include "utils.h"
#include "ticks.h"
#include "proxy.h"
#include "events.h"
#include "transports.h"
#include "servers.h"
#include "dns.h"
#include "log.h"

#include <grp.h>
#include <pwd.h>

#include <errno.h>


static void signal_handler(int sign)
{
	server_is_running = 0;

}

static int inc_rlimit(int nofile)
{
	struct rlimit rl;
	
	rl.rlim_cur = nofile;
	rl.rlim_max = nofile;
	
	return setrlimit(RLIMIT_NOFILE, &rl);
}

static void ape_daemon(int pidfile, acetables *g_ape)
{
	
	if (0 != fork()) { 
		exit(0);
	}
	if (-1 == setsid()) {
		exit(0);
	}
	signal(SIGHUP, SIG_IGN);
	
	if (0 != fork()) {
		exit(0);
	}
	
	g_ape->is_daemon = 1;
	
	if (pidfile > 0) {
		char pidstring[32];
		int len;
		len = sprintf(pidstring, "%i", getpid());
		write(pidfile, pidstring, len);
		close(pidfile);
	}
}


int main(int argc, char **argv) 
{
	apeconfig *srv;
	
	int random, im_r00t = 0, pidfd = 0, serverfd;
	unsigned int getrandom = 0;
	const char *pidfile = NULL;
	char *confs_path = NULL;
	
	struct _fdevent fdev;
	
	char cfgfile[513] = APE_CONFIG_FILE;
	
	acetables *g_ape;
	
	if (argc > 1 && strcmp(argv[1], "--version") == 0) {
		printf("\n   AJAX Push Engine Server %s - (C) Anthony Catel <a.catel@weelya.com>\n   http://www.ape-project.org/\n\n", _VERSION);
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "--help") == 0) {
		printf("\n   AJAX Push Engine Server %s - (C) Anthony Catel <a.catel@weelya.com>\n   http://www.ape-project.org/\n", _VERSION);
		printf("\n   usage: aped [options]\n\n");
		printf("   Options:\n     --help             : Display this help\n     --version          : Show version number\n     --cfg <config path>: Load a specific config file (default is %s)\n\n", cfgfile);
		return 0;
	} else if (argc > 2 && strcmp(argv[1], "--cfg") == 0) {
		memset(cfgfile, 0, 513);
		strncpy(cfgfile, argv[2], 512);
		confs_path = get_path(cfgfile);
	} else if (argc > 1) {
		printf("\n   AJAX Push Engine Server %s - (C) Anthony Catel <a.catel@weelya.com>\n   http://www.ape-project.org/\n\n", _VERSION);
		printf("   Unknown parameters - check \"aped --help\"\n\n");
		return 0;
	}
	if (NULL == (srv = ape_config_load(cfgfile))) {
		printf("\nExited...\n\n");
		exit(1);
	}
	
	if (getuid() == 0) {
		im_r00t = 1;
	}

	signal(SIGINT, &signal_handler);
	signal(SIGTERM, &signal_handler);
	signal(SIGKILL, &signal_handler);
	
	if (VTICKS_RATE < 1) {
		printf("[ERR] TICKS_RATE cant be less than 1\n");
		return 0;
	}
	
	random = open("/dev/urandom", O_RDONLY);
	if (!random) {
		printf("Cannot open /dev/urandom... exiting\n");
		return 0;
	}
	read(random, &getrandom, 3);
	srand(getrandom);
	close(random);

	g_ape = xmalloc(sizeof(*g_ape));
	g_ape->basemem = 256000;
	g_ape->srv = srv;
	g_ape->confs_path = confs_path;
	g_ape->is_daemon = 0;
	
	ape_log_init(g_ape);
	
	#ifdef USE_EPOLL_HANDLER
	fdev.handler = EVENT_EPOLL;
	#endif
	#ifdef USE_KQUEUE_HANDLER
	fdev.handler = EVENT_KQUEUE;
	#endif

	g_ape->co = xmalloc(sizeof(*g_ape->co) * g_ape->basemem);
	memset(g_ape->co, 0, sizeof(*g_ape->co) * g_ape->basemem);
	
	g_ape->bad_cmd_callbacks = NULL;
	g_ape->bufout = xmalloc(sizeof(struct _socks_bufout) * g_ape->basemem);
	
	g_ape->timers.timers = NULL;
	g_ape->timers.ntimers = 0;
	g_ape->events = &fdev;
	events_init(g_ape, &g_ape->basemem);
	
	serverfd = servers_init(g_ape);
	
	ape_log(APE_INFO, __FILE__, __LINE__, g_ape, 
		"APE starting up - pid : %i", getpid());
	
	if (strcmp(CONFIG_VAL(Server, daemon, srv), "yes") == 0 && (pidfile = CONFIG_VAL(Server, pid_file, srv)) != NULL) {
		if ((pidfd = open(pidfile, O_TRUNC | O_WRONLY | O_CREAT, 0655)) == -1) {
			ape_log(APE_WARN, __FILE__, __LINE__, g_ape, 
				"Cant open pid file : %s", CONFIG_VAL(Server, pid_file, srv));
		}
	}
	
	if (im_r00t) {
		struct group *grp = NULL;
		struct passwd *pwd = NULL;
		
		if (inc_rlimit(atoi(CONFIG_VAL(Server, rlimit_nofile, srv))) == -1) {
			ape_log(APE_WARN, __FILE__, __LINE__, g_ape, 
				"Cannot set the max filedescriptos limit (setrlimit) %s", strerror(errno));
		}
		
		/* Get the user information (uid section) */
		if ((pwd = getpwnam(CONFIG_VAL(uid, user, srv))) == NULL) {
			ape_log(APE_ERR, __FILE__, __LINE__, g_ape, 
				"Can\'t find username %s", CONFIG_VAL(uid, user, srv));
			return -1;
		}
		if (pwd->pw_uid == 0) {
			ape_log(APE_ERR, __FILE__, __LINE__, g_ape, 
				"%s uid can\'t be 0", CONFIG_VAL(uid, user, srv));
			return -1;			
		}
		
		/* Get the group information (uid section) */
		if ((grp = getgrnam(CONFIG_VAL(uid, group, srv))) == NULL) {
			printf("[ERR] Can\'t find group %s\n", CONFIG_VAL(uid, group, srv));
			ape_log(APE_ERR, __FILE__, __LINE__, g_ape, 
				"Can\'t find group %s", CONFIG_VAL(uid, group, srv));
			return -1;
		}
		
		if (grp->gr_gid == 0) {
			ape_log(APE_ERR, __FILE__, __LINE__, g_ape, 
				"%s gid can\'t be 0", CONFIG_VAL(uid, group, srv));
			return -1;
		}
		
		setgid(grp->gr_gid);
		setgroups(0, NULL);

		initgroups(CONFIG_VAL(uid, user, srv), grp->gr_gid);
		
		setuid(pwd->pw_uid);
		
	} else {
		printf("[WARN] You have to run \'aped\' as root to increase r_limit\n");
		ape_log(APE_WARN, __FILE__, __LINE__, g_ape, 
			"You have to run \'aped\' as root to increase r_limit");
	}
	
	if (strcmp(CONFIG_VAL(Server, daemon, srv), "yes") == 0) {
		ape_log(APE_INFO, __FILE__, __LINE__, g_ape, 
			"Starting daemon");
		ape_daemon(pidfd, g_ape);

		events_reload(g_ape->events);
		events_add(g_ape->events, serverfd, EVENT_READ);
	}
	
	if (!g_ape->is_daemon) {	
		printf("   _   ___ ___ \n");
		printf("  /_\\ | _ \\ __|\n");
		printf(" / _ \\|  _/ _| \n");
		printf("/_/ \\_\\_| |___|\nAJAX Push Engine\n\n");

		printf("Bind on port %i\n\n", atoi(CONFIG_VAL(Server, port, srv)));
		printf("Version : %s\n", _VERSION);
		printf("Build   : %s %s\n", __DATE__, __TIME__);
		printf("Author  : Weelya (contact@weelya.com)\n\n");		
	}
	signal(SIGPIPE, SIG_IGN);

	ape_dns_init(g_ape);
	
	g_ape->cmd_hook.head = NULL;
	g_ape->cmd_hook.foot = NULL;
	
	g_ape->hLogin = hashtbl_init();
	g_ape->hSessid = hashtbl_init();

	g_ape->hLusers = hashtbl_init();
	g_ape->hPubid = hashtbl_init();
	
	g_ape->proxy.list = NULL;
	g_ape->proxy.hosts = NULL;
	
	g_ape->hCallback = hashtbl_init();

	g_ape->uHead = NULL;
	
	g_ape->nConnected = 0;
	g_ape->plugins = NULL;
	
	g_ape->properties = NULL;

	add_ticked(check_timeout, g_ape);
	
	do_register(g_ape);
	
	transport_start(g_ape);	
	
	findandloadplugin(g_ape);

	server_is_running = 1;

	/* Starting Up */
	sockroutine(g_ape); /* loop */
	/* Shutdown */	
	
	if (pidfile != NULL) {
		unlink(pidfile);
	}
	
	hashtbl_free(g_ape->hLogin);
	hashtbl_free(g_ape->hSessid);
	hashtbl_free(g_ape->hLusers);
	
	hashtbl_free(g_ape->hCallback);
	
	free(g_ape->plugins);
	//free(srv);
	free(g_ape);
	
	return 0;
}

