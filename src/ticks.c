/*
  Copyright (C) 2006, 2007, 2008, 2009  Anthony Catel <a.catel@weelya.com>

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

/* ticks.c */


#include "ticks.h"
#include "main.h"
#include "utils.h"

#include <sys/time.h>
#include <time.h>

/* This routine is called by epoll() loop (sock.c) */
/* TICKS_RATE define how many times this routine is called each seconde */

void process_tick(acetables *g_ape)
{
	struct _ticks_callback **timers = &(g_ape->timers);
	
	while (*timers != NULL) {
		(*timers)->ticks_left--;
		
		if ((*timers)->ticks_left <= 0) {
			
			void (*func_timer)(void *param) = (*timers)->func;
			func_timer((*timers)->params);
			
			if ((*timers)->times > 0 && --(*timers)->times == 0) {
				del_timer(timers);
				continue;
			}
			
			(*timers)->ticks_left = (*timers)->ticks_need;
		}
		timers = &(*timers)->next;
	}
}

struct _ticks_callback *add_timeout(unsigned int sec, void *callback, void *params, acetables *g_ape)
{
	struct _ticks_callback *new_timer;
	
	new_timer = xmalloc(sizeof(*new_timer));
	
	new_timer->ticks_need = TICKS_RATE*sec;
	new_timer->ticks_left = new_timer->ticks_need;
	
	new_timer->times = 1;
	
	new_timer->func = callback;
	new_timer->params = params;
	
	new_timer->next = g_ape->timers;
	
	g_ape->timers = new_timer;
	
	return new_timer;
}


struct _ticks_callback *add_periodical(unsigned int sec, int times, void *callback, void *params, acetables *g_ape)
{
	struct _ticks_callback *new_timer = add_timeout(sec, callback, params, g_ape);

	new_timer->times = times;
	
	return new_timer;
}


void del_timer(struct _ticks_callback **timer)
{
	struct _ticks_callback *del = *timer;

	*timer = (*timer)->next;

	free(del);
}

