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

/* event_kqueue.c */

#include "events.h"
#include <sys/time.h>
#include <time.h>

#ifdef USE_EPOLL_HANDLER
static int event_epoll_add(struct _fdevent *ev, int fd, int bitadd)
{
	struct epoll_event kev;
	
	kev.events = EPOLLET | EPOLLPRI;
	
	if (bitadd & EVENT_READ) {
		kev.events |= EPOLLIN; 
	}
	
	if (bitadd & EVENT_WRITE) {
		kev.events |= EPOLLOUT;
	}
	
	kev.data.fd = fd;
		
	if (epoll_ctl(ev->epoll_fd, EPOLL_CTL_ADD, fd, &kev) == -1) {
		return -1;
	}
	
	return 1;
}

static int event_epoll_poll(struct _fdevent *ev, int timeout_ms)
{
	int nfds;

	if ((nfds = epoll_wait(ev->epoll_fd, ev->events, *ev->basemem, timeout_ms)) == -1) {
		return -1;
	}
	
	return nfds;
}

static int event_epoll_get_fd(struct _fdevent *ev, int i)
{
	return ev->events[i].data.fd;
}

static void event_epoll_growup(struct _fdevent *ev)
{
	ev->events = xrealloc(ev->events, sizeof(struct epoll_event) * (*ev->basemem));
}

static int event_epoll_revent(struct _fdevent *ev, int i)
{
	int bitret = 0;
	
	if (ev->events[i].events & EPOLLIN) {
		bitret = EVENT_READ;
	}
	if (ev->events[i].events & EPOLLOUT) {
		bitret |= EVENT_WRITE;
	}
	
	return bitret;
}


int event_epoll_reload(struct _fdevent *ev)
{
	int nfd;
	if ((nfd = dup(ev->epoll_fd)) != -1) {
		close(nfd);
		close(ev->epoll_fd);
	}
	if ((ev->epoll_fd = epoll_create(1)) == -1) {
		return 0;
	}
	
	return 1;	
}

int event_epoll_init(struct _fdevent *ev)
{
	if ((ev->epoll_fd = epoll_create(1)) == -1) {
		return 0;
	}

	ev->events = xmalloc(sizeof(struct epoll_event) * (*ev->basemem));
	
	ev->add = event_epoll_add;
	ev->poll = event_epoll_poll;
	ev->get_current_fd = event_epoll_get_fd;
	ev->growup = event_epoll_growup;
	ev->revent = event_epoll_revent;
	ev->reload = event_epoll_reload;
	
	return 1;
}

#else
int event_epoll_init(struct _fdevent *ev)
{
	return 0;
}
#endif

