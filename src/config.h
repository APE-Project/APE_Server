/*
  Copyright (C) 2006, 2007, 2008  Anthony Catel <a.catel@weelya.com>

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

/* config.h */

#ifndef _CONFIG
#define _CONFIG

#define ACE_CONFIG_FILE "ace.conf"

typedef struct srvconfig {
	unsigned int port;
	
	unsigned int max_connected;
	

	char daemon[32];
	
	char fConnected[256];
	char domain[512];
	
} srvconfig;
srvconfig *load_ace_config(const char *path_config);
#endif
