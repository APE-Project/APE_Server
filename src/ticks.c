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

/* ticks.c */


#include "ticks.h"
#include "utils.h"

#include <sys/time.h>
#include <time.h>

/* This routine is called by epoll() loop (sock.c) */
/* TICKS_RATE define how many times this routine is called each seconde */

#if 0
void process_tick(acetables *g_ape)
{
	struct _ticks_callback **timers = &(g_ape->timers);
	
	while (*timers != NULL) {
		(*timers)->ticks_left--;
		
		if ((*timers)->ticks_left <= 0) {
			int lastcall = ((*timers)->times > 0 && --(*timers)->times == 0);
			
			void (*func_timer)(void *param, int) = (*timers)->func;
			func_timer((*timers)->params, lastcall);
			
			if (lastcall) {
				del_timer(timers);
				continue;
			}
			
			(*timers)->ticks_left = (*timers)->ticks_need;
		}
		timers = &(*timers)->next;
	}
}
#endif

inline void process_tick(acetables *g_ape)
{
	struct _ticks_callback *timers = g_ape->timers.timers;
	
	while (timers != NULL) {
		timers->ticks_left--;
		
		if (timers->ticks_left <= 0) {
			int lastcall = (timers->times > 0 && --timers->times == 0);
			void (*func_timer)(void *param, int *) = timers->func;
			func_timer(timers->params, &lastcall);
			
			if (lastcall) {
				struct _ticks_callback *tmpTimers = timers->next;

				del_timer(timers, g_ape);
				timers = tmpTimers;
				continue;
			}
			
			timers->ticks_left = timers->ticks_need;
		}
		timers = timers->next;
	}
}

struct _ticks_callback *add_timeout(unsigned int msec, void *callback, void *params, acetables *g_ape)
{
	struct _ticks_callback *new_timer;
	
	new_timer = xmalloc(sizeof(*new_timer));
	
	new_timer->ticks_need = msec;
	new_timer->ticks_left = new_timer->ticks_need;

	new_timer->prev = NULL;
	new_timer->times = 1;
	new_timer->protect = 1;
	
	new_timer->func = callback;
	new_timer->params = params;
	
	if (g_ape->timers.timers != NULL) {
		g_ape->timers.timers->prev = new_timer;
		g_ape->timers.ntimers++;
	} else {
		g_ape->timers.ntimers = 0;
	}
	new_timer->next = g_ape->timers.timers;
	new_timer->identifier = g_ape->timers.ntimers;

	g_ape->timers.timers = new_timer;
	
	return new_timer;
}

/* Exec callback "times"x each "sec" */
/* If "times" is 0, the function is executed indefinitifvly */

struct _ticks_callback *add_periodical(unsigned int msec, int times, void *callback, void *params, acetables *g_ape)
{
	struct _ticks_callback *new_timer = add_timeout(msec, callback, params, g_ape);

	new_timer->times = times;
	
	return new_timer;
}

void del_timer(struct _ticks_callback *timer, acetables *g_ape)
{
	if (timer->prev != NULL) {
		timer->prev->next = timer->next;
	} else {
		g_ape->timers.timers = timer->next;
	}
	if (timer->next != NULL) {
		timer->next->prev = timer->prev;
	}
	//g_ape->timers.ntimers--;
	
	free(timer);
}

struct _ticks_callback *get_timer_identifier(unsigned int identifier, acetables *g_ape)
{
	struct _ticks_callback *timers = g_ape->timers.timers;
	
	while (timers != NULL) {
		if (timers->identifier == identifier) {
			return timers;
		}
		timers = timers->next;
	}
	
	return NULL;
}

void del_timer_identifier(unsigned int identifier, acetables *g_ape)
{
	struct _ticks_callback *timers = g_ape->timers.timers;
	
	while (timers != NULL) {
		if (timers->identifier == identifier) {
			del_timer(timers, g_ape);
			break;
		}
		timers = timers->next;
	}
}

#if 0
void del_timer(struct _ticks_callback **timer)
{
	struct _ticks_callback *del = *timer;

	*timer = (*timer)->next;

	free(del);
}
#endif
