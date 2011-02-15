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

/* events.c */

#include "events.h"
#include "main.h"

int events_init(acetables *g_ape, int *basemem)
{
	g_ape->events->basemem = basemem;

	switch(g_ape->events->handler) {
		case EVENT_EPOLL:
			return event_epoll_init(g_ape->events);
			break;
		case EVENT_KQUEUE:
			return event_kqueue_init(g_ape->events);
   	        case EVENT_SELECT:
		        return event_select_init(g_ape->events);
		default:
			break;
	}
	
	return -1;
}

void events_free(acetables *g_ape)
{
	if (g_ape->events->handler != EVENT_UNKNOWN) {
		free(g_ape->events->events);
	}
}

int events_add(struct _fdevent *ev, int fd, int bitadd)
{
	if (ev->add(ev, fd, bitadd) == -1) {
		return -1;
	}
	return 1;
}

int events_remove(struct _fdevent *ev, int fd)
{
  return ev->remove(ev, fd);
}

int events_poll(struct _fdevent *ev, int timeout_ms)
{
	int nfds;
	
	if ((nfds = ev->poll(ev, timeout_ms)) == -1) {
		return -1;
	}
	
	return nfds;
}

int events_get_current_fd(struct _fdevent *ev, int i)
{
	return ev->get_current_fd(ev, i);
}

void events_growup(struct _fdevent *ev)
{
	ev->growup(ev);
}

int events_revent(struct _fdevent *ev, int i)
{
	return ev->revent(ev, i);
}

int events_reload(struct _fdevent *ev)
{
	return ev->reload(ev);
}
