/*
  Copyright (C) 2006, 2007, 2008, 2009  Anthony Catel <a.catel@weelya.com>

  This file is part of ACE Server.
  ACE is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  ACE is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ACE ; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

/* entry.c */

#include "plugins.h"
#include "main.h"
#include "sock.h"

#include "config.h"
#include "raw.h"

#include "channel.h"


#include <signal.h>
#include <syslog.h>
#include <sys/resource.h>
#include "utils.h"
#include "ticks.h"
#include "proxy.h"

#define _VERSION "0.8.0"

#define _RLIMIT_ 50000

static void signal_handler(int sign)
{
	printf("\nShutdown...!\n\n");
	exit(1);
}
static void signal_pipe(int sign)
{
	return;
}

static void inc_rlimit()
{
	struct rlimit rl;
	
	rl.rlim_cur = _RLIMIT_;
	rl.rlim_max = _RLIMIT_;
	
	setrlimit(RLIMIT_NOFILE, &rl);
}

int main(int argc, char **argv) 
{
	srvconfig *srv;
	
	int random;
	unsigned int getrandom;

	
	char cfgfile[512] = ACE_CONFIG_FILE;
	
	register acetables *g_ape;
	
	if (argc > 1 && strcmp(argv[1], "--version") == 0) {
		printf("\n   AJAX Push Engine Serveur %s - (C) Anthony Catel <a.catel@weelya.com>\n   http://www.ape-project.org/\n\n", _VERSION);
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "--help") == 0) {
		printf("\n   AJAX Push Engine Serveur %s - (C) Anthony Catel <a.catel@weelya.com>\n   http://www.ape-project.org/\n", _VERSION);
		printf("\n   usage: aced [options]\n\n");
		printf("   Options:\n     --help             : Display this help\n     --version          : Show version number\n     --cfg <config path>: Load a specific config file (default is ace.conf)\n\n");
		return 0;
	} else if (argc > 2 && strcmp(argv[1], "--cfg") == 0) {
		strncpy(cfgfile, argv[2], 512);
		cfgfile[strlen(argv[2])] = '\0';
		
	} else if (argc > 1) {
		printf("\n   AJAX Push Engine Serveur %s - (C) Anthony Catel <a.catel@weelya.com>\n   http://www.ape-project.org/\n\n", _VERSION);
		printf("   Unknown parameters - check \"aced --help\"\n\n");
		return 0;		
	}
	if (NULL == (srv = load_ace_config(cfgfile))) {
		printf("\nExited...\n\n");
		exit(1);
	}
	if (strcmp(srv->daemon, "yes") == 0) {
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
	}
	printf("   _   ___ ___ \n");
	printf("  /_\\ | _ \\ __|\n");
	printf(" / _ \\|  _/ _| \n");
	printf("/_/ \\_\\_| |___|\nAJAX Push Engine\n\n");

	printf("Bind on port %i\n\n", srv->port);
	printf("Version : %s\n", _VERSION);
	printf("Build   : %s %s\n", __DATE__, __TIME__);
	printf("Author  : Weelya (contact@weelya.com)\n\n");
	if (strcmp(srv->daemon, "yes")==0) {
		printf("Starting daemon.... pid : %i\n\n", getpid());
	}
	signal(SIGINT, &signal_handler);
	signal(SIGPIPE, &signal_pipe);
	
	if (getuid() == 0) {
		inc_rlimit();
	} else {
		printf("[ERR] You must run APE as root\n");
		return 0;
	}
	
	if (TICKS_RATE < 1) {
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
	
	g_ape->hLogin = hashtbl_init();
	g_ape->hSessid = hashtbl_init();

	g_ape->hLusers = hashtbl_init();
	g_ape->hPubid = hashtbl_init();
	

	g_ape->srv = srv;
	g_ape->proxy.list = NULL;
	g_ape->proxy.hosts = NULL;
	g_ape->epoll_fd = NULL;
	
	g_ape->hCallback = hashtbl_init();
	


	
	g_ape->uHead = NULL;
	
	g_ape->nConnected = 0;
	g_ape->plugins = NULL;
	
	g_ape->properties = NULL;
	
	g_ape->timers = NULL;

	add_ticked(check_timeout, g_ape);
	
	
	do_register(g_ape);
	
	
	findandloadplugin(g_ape);
	
	proxy_cache_addip("localhost", "91.121.79.141", g_ape);
	
	/*if (proxy_init("olol", "localhost", 80, g_ape) == NULL) {
		printf("Failed to connect to data stream\n");
	}*/
	
	//proxy_init("olol", "localhost", 1337, g_ape);
	
	sockroutine(g_ape->srv->port, g_ape);
	
	hashtbl_free(g_ape->hLogin);
	hashtbl_free(g_ape->hSessid);
	hashtbl_free(g_ape->hLusers);
	
	hashtbl_free(g_ape->hCallback);
	
	free(g_ape->plugins);
	free(srv);
	free(g_ape);
	
	return 0;
}

