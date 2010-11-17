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
		int facility_num = -1;
		struct _syslog_facilities {
			int facility;
			char str[12];
		} syslog_facilities[] = {
			{LOG_KERN,     "kern"},
			{LOG_USER,     "user"},
			{LOG_MAIL,     "mail"},
			{LOG_DAEMON,   "daemon"},
			{LOG_AUTH,     "auth"},
			{LOG_SYSLOG,   "syslog"},
			{LOG_LPR,      "lpr"},
			{LOG_NEWS,     "news"},
			{LOG_UUCP,     "uucp"},
			{LOG_CRON,     "cron"},
			/*{LOG_AUTHPRIV, "authpriv"}, private facility */
			{LOG_FTP,      "ftp"}
		};
		size_t syslog_max_facilities = sizeof(syslog_facilities) / sizeof(struct _syslog_facilities);

		/* decode the facility requested in the config file */
		if (!strncasecmp(facility, "local", 5) && strlen(facility) == 6) {
			int local = 0;
			char *localnum;
			// increment the facility pointer by 5 to move past "local"
			localnum = &facility[5];
			local = atoi(localnum);

			if (!local && !strncmp("0", localnum, 2)) {
				facility_num = LOG_LOCAL0;
			} else if (local < 8 && local > 0) {
				facility_num = ((local + 16)<<3);
			}
		} else {
			int i;
			for(i = 0; i < syslog_max_facilities; i++) {
				if (!strncasecmp(syslog_facilities[i].str, facility, strlen(syslog_facilities[i].str) + 1)) {
					facility_num = syslog_facilities[i].facility;
					break;
				}
			}
		}

		if (facility_num == -1) {
			printf("[WARN] Invalid facility '%s' requested, defaulting to LOCAL2\n", facility);
			facility_num = LOG_LOCAL2;
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
