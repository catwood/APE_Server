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

static void write_pid_file(int pidfile, int pid)
{
	if (pidfile > 0) {
		char pidstring[32];
		int len;
		len = sprintf(pidstring, "%i", pid);
		write(pidfile, pidstring, len);
		close(pidfile);
	}
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
	write_pid_file(pidfile, (int) getpid());
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
	int argi = 0;
	int overrule_daemon = -1; //nothing fancy, -1: no, just the configuration, 0: yes overrule config, but do no daemon, 1: yes overrule config, but daemonize
	if (argc > 1 ) {
		for (argi = 1; argi < argc; argi++ ) {
			if (strcmp(argv[argi], "--version") == 0) {
				printf("\n   AJAX Push Engine Server %s - (C) Anthony Catel <a.catel@weelya.com>\n   http://www.ape-project.org/\n\n", _VERSION);
				return 0;
			} else if (strcmp(argv[argi], "--help") == 0) {
				printf("\n   AJAX Push Engine Server %s - (C) Anthony Catel <a.catel@weelya.com>\n   http://www.ape-project.org/\n", _VERSION);
				printf("\n   usage: aped [options]\n\n");
				printf("   Options:\n     --help             : Display this help\n     --version          : Show version number\n     --daemon yes|no    : Overrule the daemon settings in Server section of the config file\n     --cfg    FILE      : Load a specific config file (default is %s)\n\n", cfgfile);
				return 0;
			} else if (argc > argi + 1 && strcmp(argv[argi], "--cfg") == 0) {
				memset(cfgfile, 0, 513);
				strncpy(cfgfile, argv[argi + 1], 512);
				confs_path = get_path(cfgfile);
				argi++;
			} else if (argc > argi + 1 && strcmp(argv[argi], "--daemon") == 0) {
				overrule_daemon = (strcmp(argv[argi + 1], "yes") == 0 );
				argi++;
			} else {
				printf("\n   AJAX Push Engine Server %s - (C) Anthony Catel <a.catel@weelya.com>\n   http://www.ape-project.org/\n\n", _VERSION);
				printf("   Unknown parameters - check \"aped --help\"\n\n");
				exit(1);
			}
		}
	}

	if (NULL == (srv = ape_config_load(cfgfile))) {
		printf("\nExiting...\n\n");
		exit(1);
	}
	if (getuid() == 0) {
		im_r00t = 1;
	}

	signal(SIGINT, &signal_handler);
	signal(SIGTERM, &signal_handler);

	g_ape = xmalloc(sizeof(*g_ape));
	g_ape->basemem = 1; // set 1 for testing if growup works
	g_ape->srv = srv;
	g_ape->confs_path = confs_path;
	if (overrule_daemon == 0) {
		ape_config_set_key(ape_config_get_section(g_ape->srv, "Server"), "daemon", "no");
	} else if (overrule_daemon == 1) {
		ape_config_set_key(ape_config_get_section(g_ape->srv, "Server"), "daemon", "yes");
	}
	g_ape->is_daemon = (strcmp(CONFIG_VAL(Server, daemon, srv), "yes") == 0 )? 1 :0;
	ape_log_init(g_ape);

	if (VTICKS_RATE < 1) {
		if (!g_ape->is_daemon) {
			printf("[ERR] TICKS_RATE cannot be less than 1... exiting\n");
		}
		ape_log(APE_ERR, __FILE__, __LINE__, g_ape, "[ERR] TICKS_RATE cannot be less than 1... exiting");
		exit(1);
	}

	random = open("/dev/urandom", O_RDONLY);
	if (!random) {
		if (!g_ape->is_daemon) {
			printf("[ERR] Cannot open /dev/urandom... exiting\n");
		}
		ape_log(APE_ERR, __FILE__, __LINE__, g_ape, "[ERR] Cannot open /dev/urandom... exiting");
		exit(1);
	}
	read(random, &getrandom, 3);
	srand(getrandom);
	close(random);

	fdev.handler = EVENT_UNKNOWN;
	#ifdef USE_EPOLL_HANDLER
	fdev.handler = EVENT_EPOLL;
	#endif
	#ifdef USE_KQUEUE_HANDLER
	fdev.handler = EVENT_KQUEUE;
	#endif
	#ifdef USE_SELECT_HANDLER
	fdev.handler = EVENT_SELECT;
	#endif

	g_ape->co = xmalloc(sizeof(*g_ape->co) * g_ape->basemem);
	memset(g_ape->co, 0, sizeof(*g_ape->co) * g_ape->basemem);

	g_ape->bad_cmd_callbacks = NULL;
	g_ape->bufout = xmalloc(sizeof(struct _socks_bufout) * g_ape->basemem);
	g_ape->timers.timers = NULL;
	g_ape->timers.ntimers = 0;
	g_ape->events = &fdev;
	if (events_init(g_ape, &g_ape->basemem) == -1) {
		if (!g_ape->is_daemon) {
			printf("[ERR] Fatal error: APE compiled without an event handler... exiting\n");
		}
		ape_log(APE_ERR, __FILE__, __LINE__, g_ape, "[ERR] Fatal error: APE compiled without an event handler... exiting");
		exit(1);
	}

	serverfd = servers_init(g_ape);
	//printf("APE starting up %s:%i\n", CONFIG_VAL(Server, ip_listen, g_ape->srv), atoi(CONFIG_VAL(Server, port, srv)));
	//ape_log(APE_INFO, __FILE__, __LINE__, g_ape, "APE starting up %s:%i\n", CONFIG_VAL(Server, ip_listen, g_ape->srv), atoi(CONFIG_VAL(Server, port, srv)));

	if ((pidfile = CONFIG_VAL(Server, pid_file, srv)) != NULL) {
		if ((pidfd = open(pidfile, O_TRUNC | O_WRONLY | O_CREAT, 0655)) == -1) {
			if (!g_ape->is_daemon) {
				printf("[WARN] Cannot open pid file : %s\n", CONFIG_VAL(Server, pid_file, srv));
			}
			ape_log(APE_WARN, __FILE__, __LINE__, g_ape, "[WARN] Cannot open pid file : %s", CONFIG_VAL(Server, pid_file, srv));
		}
	}

	if (im_r00t) {
		struct group *grp = NULL;
		struct passwd *pwd = NULL;

		if (inc_rlimit(atoi(CONFIG_VAL(Server, rlimit_nofile, srv))) == -1) {
			if (!g_ape->is_daemon) {
				printf("[WARN] Cannot set the max filedescriptor limit (setrlimit) %s\n", strerror(errno));
			}
			ape_log(APE_WARN, __FILE__, __LINE__, g_ape, "[WARN] Cannot set the max filedescriptor limit (setrlimit) %s", strerror(errno));
		}

		/* Set uid when uid section exists */
		if (ape_config_get_section(srv, "uid")) {

			/* Get the user information (uid section) */
			if ((pwd = getpwnam(CONFIG_VAL(uid, user, srv))) == NULL) {
				if (!g_ape->is_daemon) {
					printf("[ERR] Cannot find username %s\n", CONFIG_VAL(uid, user, srv));
				}
				ape_log(APE_ERR, __FILE__, __LINE__, g_ape, "[ERR] Cannot find username %s", CONFIG_VAL(uid, user, srv));
				return -1;
			}
			if (pwd->pw_uid == 0) {
				if (!g_ape->is_daemon) {
					printf("[ERR] %s uid can\'t be 0\n", CONFIG_VAL(uid, user, srv));
				}
				ape_log(APE_ERR, __FILE__, __LINE__, g_ape, "[ERR] %s uid can\'t be 0", CONFIG_VAL(uid, user, srv));
				return -1;
			}

			/* Get the group information (uid section) */
			if ((grp = getgrnam(CONFIG_VAL(uid, group, srv))) == NULL) {
				if (!g_ape->is_daemon) {
					printf("[ERR] Cannot find group %s\n", CONFIG_VAL(uid, group, srv));
				}
				ape_log(APE_ERR, __FILE__, __LINE__, g_ape, "[ERR] Cannot find group %s", CONFIG_VAL(uid, group, srv));
				return -1;
			}

			if (grp->gr_gid == 0) {
				if (!g_ape->is_daemon) {
					printf("[ERR] %s gid cannot be 0\n", CONFIG_VAL(uid, group, srv));
				}
				ape_log(APE_ERR, __FILE__, __LINE__, g_ape, "[ERR] %s gid cannot be 0", CONFIG_VAL(uid, group, srv));
			return -1;
			}

			setgid(grp->gr_gid);
			setgroups(0, NULL);
			initgroups(CONFIG_VAL(uid, user, srv), grp->gr_gid);
			setuid(pwd->pw_uid);
		}
	} else {
		if (!g_ape->is_daemon) {
			printf("[WARN] You have to run \'aped\' as root to increase r_limit\n");
		}
		ape_log(APE_WARN, __FILE__, __LINE__, g_ape, "[WARN] You have to run \'aped\' as root to increase r_limit");
	}

	if (g_ape->is_daemon) {
		ape_log(APE_INFO, __FILE__, __LINE__, g_ape, "Starting daemon on %s:%i, pid: %i", CONFIG_VAL(Server, ip_listen, g_ape->srv), atoi(CONFIG_VAL(Server, port, srv)), getpid());
		ape_daemon(pidfd, g_ape);
		printf("Started daemon on %s:%i, pid: %i\n", CONFIG_VAL(Server, ip_listen, g_ape->srv), atoi(CONFIG_VAL(Server, port, srv)), getpid());
		events_reload(g_ape->events);
		events_add(g_ape->events, serverfd, EVENT_READ);
	} else {
		printf("   _   ___ ___ \n");
		printf("  /_\\ | _ \\ __|\n");
		printf(" / _ \\|  _/ _| \n");
		printf("/_/ \\_\\_| |___|\nAJAX Push Engine\n\n");
		printf("Bind on : %s:%i\nPid     : %i\n", CONFIG_VAL(Server, ip_listen, g_ape->srv), atoi(CONFIG_VAL(Server, port, srv)), getpid());
		printf("Version : %s\n", _VERSION);
		printf("Build   : %s %s\n", __DATE__, __TIME__);
		printf("Author  : Weelya (contact@weelya.com)\n\n");
		ape_log(APE_INFO, __FILE__, __LINE__, g_ape, "Started on %s:%i, pid: %i\n\n", CONFIG_VAL(Server, ip_listen, g_ape->srv), atoi(CONFIG_VAL(Server, port, srv)), getpid());
		write_pid_file(pidfd, (int) getpid());
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
	//fixme: unregister commands, register_bad_cmd and register_hook_cmd

	free(confs_path);

	timers_free(g_ape);

	events_free(g_ape);

	transport_free(g_ape);

	hashtbl_free(g_ape->hLogin);
	hashtbl_free(g_ape->hSessid);
	hashtbl_free(g_ape->hLusers);
	hashtbl_free(g_ape->hPubid);

	hashtbl_free(g_ape->hCallback);

	free(g_ape->bufout);

	ape_config_free(srv);

	int i;
	for (i = 0; i < g_ape->basemem; i++) {
		if (g_ape->co[i] != NULL) {
			free(g_ape->co[i]);
		}
	}
	free(g_ape->co);

	free_all_plugins(g_ape);

	free(g_ape);

	return 0;
}

