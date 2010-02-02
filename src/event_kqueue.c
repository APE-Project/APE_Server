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

#ifdef USE_KQUEUE_HANDLER
static int event_kqueue_add(struct _fdevent *ev, int fd, int bitadd)
{
	struct kevent kev;
	struct timespec ts;
	int filter = 0;
		
	memset(&kev, 0, sizeof(kev));
	
	if (bitadd & EVENT_READ) {
		filter = EVFILT_READ; 

		ts.tv_sec = 0;
		ts.tv_nsec = 0;
	
		EV_SET(&kev, fd, filter, EV_ADD|EV_CLEAR, 0, 0, NULL);
		if (kevent(ev->kq_fd, &kev, 1, NULL, 0, &ts) == -1) {
			return -1;
		}
	
	}

	if (bitadd & EVENT_WRITE) {
		filter = EVFILT_WRITE; 
	
		memset(&kev, 0, sizeof(kev));

		ts.tv_sec = 0;
		ts.tv_nsec = 0;
	
		EV_SET(&kev, fd, filter, EV_ADD|EV_CLEAR, 0, 0, NULL);
		if (kevent(ev->kq_fd, &kev, 1, NULL, 0, &ts) == -1) {
			return -1;
		}
	
	}
	
	return 1;
}

static int event_kqueue_poll(struct _fdevent *ev, int timeout_ms)
{
	int nfds;
	struct timespec ts;
	
	ts.tv_sec = timeout_ms / 1000;
	ts.tv_nsec = (timeout_ms % 1000) * 1000000;

	if ((nfds = kevent(ev->kq_fd, NULL, 0, ev->events, *ev->basemem * 2, &ts)) == -1) {
		return -1;
	}
	
	return nfds;
}

static int event_kqueue_get_fd(struct _fdevent *ev, int i)
{
	return ev->events[i].ident;
}

static void event_kqueue_growup(struct _fdevent *ev)
{
	ev->events = xrealloc(ev->events, sizeof(struct kevent) * (*ev->basemem * 2));
}

static int event_kqueue_revent(struct _fdevent *ev, int i)
{
	int bitret = 0;
	
	if (ev->events[i].filter == EVFILT_READ) {
		bitret = EVENT_READ;
	} else if (ev->events[i].filter == EVFILT_WRITE) {
		bitret = EVENT_WRITE;
	}
	
	return bitret;
}


int event_kqueue_reload(struct _fdevent *ev)
{
	int nfd;
	if ((nfd = dup(ev->kq_fd)) != -1) {
		close(nfd);
		close(ev->kq_fd);
	}

	if ((ev->kq_fd = kqueue()) == -1) {
		return 0;
	}
	
	return 1;	
}

int event_kqueue_init(struct _fdevent *ev)
{
	if ((ev->kq_fd = kqueue()) == -1) {
		return 0;
	}

	ev->events = xmalloc(sizeof(struct kevent) * (*ev->basemem * 2));
	
	memset(ev->events, 0, sizeof(struct kevent) * (*ev->basemem * 2));
	
	ev->add = event_kqueue_add;
	ev->poll = event_kqueue_poll;
	ev->get_current_fd = event_kqueue_get_fd;
	ev->growup = event_kqueue_growup;
	ev->revent = event_kqueue_revent;
	ev->reload = event_kqueue_reload;
	
	return 1;
}

#else
int event_kqueue_init(struct _fdevent *ev)
{
	return 0;
}
#endif

