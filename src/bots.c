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

/* bots.c */

#include "main.h"
#include "channel.h"
#include "users.h"
#include "raw.h"
#include "json.h"
#include "plugins.h"
// Please complete ;)

#if 0
USERS *bot_connect(char *nick, char *channel, acetables *ace_tables) // Verifier si deja connecté, que join
{
	USERS *nuser;
	CHANNEL *jchan;
	
	if ((nuser = seek_user_simple(nick, ace_tables))) { // No Ghost !!!
		deluser(nuser, ace_tables);
	}
	
	nuser = adduser(nick, 0, "", ace_tables);
	printf("Bot %s connected\n", nick);
	if (channel != NULL) {
	
		if ((jchan = getchan(channel, ace_tables)) == NULL) {

			jchan = mkchan(channel, "Default%20Topic", ace_tables);
		
			if (jchan == NULL) {
			
				send_error(nuser, "CANT_JOIN_CHANNEL");
			
			} else {
		
				join(nuser, jchan, ace_tables);
			}
	
		} else {
			join(nuser, jchan, ace_tables);
		}
	}
	
	return nuser;
}
#endif

