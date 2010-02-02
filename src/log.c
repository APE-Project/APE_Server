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
	}
}

void ape_log(ape_log_lvl_t lvl, const char *file, unsigned long int line, acetables *g_ape, char *buf, ...)
{
	if (lvl == APE_DEBUG && !g_ape->logs.lvl&APE_DEBUG) {
		return;
	} else {
		time_t log_ts;
		char *buff;
		char date[32];
		
		int len, datelen;
		va_list val;
		log_ts = time(NULL);
		
		va_start(val, buf);
		len = vasprintf(&buff, buf, val);
		va_end(val);
		
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
