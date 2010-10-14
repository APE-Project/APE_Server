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

/* log.c */

#include <syslog.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

#include "utils.h"
#include "log.h"
#include "main.h"
#include "config.h"


void ape_log_init(acetables *g_ape)
{
	int debug = atoi(CONFIG_VAL(Log, debug, g_ape->srv));
	
	g_ape->logs.fd = STDERR_FILENO;
	g_ape->logs.lvl = (debug ? APE_DEBUG : 0);
	g_ape->logs.lvl |= APE_ERR | APE_WARN;
	if (!(g_ape->logs.use_syslog = atoi(CONFIG_VAL(Log, use_syslog, g_ape->srv)))) {
		if ((g_ape->logs.fd = open(CONFIG_VAL(Log, logfile, g_ape->srv), O_APPEND | O_WRONLY | O_CREAT, 0644)) == -1) {
			g_ape->logs.fd = STDERR_FILENO;
		}
	} else {
		char *facility = CONFIG_VAL(Log, syslog_facility, g_ape->srv);
		int facility_num = LOG_LOCAL2;

		/* decode the facility requested in the config file */
		if (!strncasecmp(facility, "local", 5)) {
			int local = 0;
			facility += 5;
			local = atoi(facility);

			if (!local && !strncmp("0", facility, 1)) {
				facility_num = LOG_LOCAL0;
			} else if (local < 8 && local > 0) {
				facility_num = ((local + 16)<<3);
			}
		} else if (!strncasecmp("kern", facility, 4)) {
			facility_num = LOG_KERN;
		} else if (!strncasecmp("user", facility, 4)) {
			facility_num = LOG_USER;
		} else if (!strncasecmp("mail", facility, 4)) {
			facility_num = LOG_MAIL;
		} else if (!strncasecmp("daemon", facility, 6)) {
			facility_num = LOG_DAEMON;
		} else if (!strncasecmp("auth", facility, 4)) {
			facility_num = LOG_AUTH;
		} else if (!strncasecmp("syslog", facility, 6)) {
			facility_num = LOG_SYSLOG;
		} else if (!strncasecmp("lpr", facility, 3)) {
			facility_num = LOG_LPR;
		} else if (!strncasecmp("news", facility, 4)) {
			facility_num = LOG_NEWS;
		} else if (!strncasecmp("uucp", facility, 4)) {
			facility_num = LOG_UUCP;
		} else if (!strncasecmp("cron", facility, 4)) {
			facility_num = LOG_CRON;
		} else if (!strncasecmp("authpriv", facility, 8)) {
			facility_num = LOG_AUTHPRIV;
		} else if (!strncasecmp("ftp", facility, 3)) {
			facility_num = LOG_FTP;
		}

		openlog("APE", LOG_CONS | LOG_PID, facility_num);
	}
}

void ape_log(ape_log_lvl_t lvl, const char *file, unsigned long int line, acetables *g_ape, char *buf, ...)
{
	if (lvl == APE_DEBUG && !g_ape->logs.lvl&APE_DEBUG) {
		return;
	} else {
		char *buff;
		int len;
		va_list val;
		
		va_start(val, buf);
		len = vasprintf(&buff, buf, val);
		va_end(val);

		if (g_ape->logs.use_syslog) {
			int level = LOG_ERR;
			switch (lvl) {
				case APE_DEBUG:
					level = LOG_DEBUG;
					break;
				case APE_WARN:
					level = LOG_WARNING;
					break;
				case APE_ERR:
					level = LOG_ERR;
					break;
				case APE_INFO:
					level = LOG_INFO;
					break;
			}
			syslog(level, "%s:%li - %s", file, line, buff);
		} else {
			int datelen;
			char date[32];
			time_t log_ts;
			log_ts = time(NULL);

			datelen = strftime(date, 32, "%Y-%m-%d %H:%M:%S - ", localtime(&log_ts));

			write(g_ape->logs.fd, date, datelen);
			if (g_ape->logs.lvl&APE_DEBUG) {
				char *debug_file;
				int dlen;
				dlen = asprintf(&debug_file, "%s:%li - ", file, line);
				write(g_ape->logs.fd, debug_file, dlen);
				free(debug_file);
			}
			write(g_ape->logs.fd, buff, len);
			write(g_ape->logs.fd, "\n", 1);

			free(buff);
		}
	}
}
