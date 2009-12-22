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

/* events.h */

#ifndef _EVENTS_H
#define _EVENTS_H

#include "utils.h"
#include "main.h"

#include "configure.h"

#ifdef USE_KQUEUE_HANDLER
#include <sys/event.h>
#endif
#ifdef USE_EPOLL_HANDLER
#include <sys/epoll.h>
#endif

/* Generics flags */
#define EVENT_READ 0x01
#define EVENT_WRITE 0x02

/* Events handler */
typedef enum {
	EVENT_EPOLL, 	/* Linux */
	EVENT_KQUEUE, 	/* BSD */
	EVENT_DEVPOLL,	/* Solaris */
	EVENT_POLL,		/* POSIX */
	EVENT_SELECT	/* Generic (Windows) */
} fdevent_handler_t;

struct _fdevent {
	/* Common values */
	int *basemem;
	/* Interface */
	int (*add)(struct _fdevent *, int, int);
	int (*poll)(struct _fdevent *, int);
	int (*get_current_fd)(struct _fdevent *, int);
	void (*growup)(struct _fdevent *);
	int (*revent)(struct _fdevent *, int);
	int (*reload)(struct _fdevent *);
	
	/* Specifics values */
	#ifdef USE_KQUEUE_HANDLER
	struct kevent *events;
	int kq_fd;
	#endif
	#ifdef USE_EPOLL_HANDLER
	struct epoll_event *events;
	int epoll_fd;
	#endif
	
	fdevent_handler_t handler;
};

int events_init(acetables *g_ape, int *basemem);
int events_add(struct _fdevent *ev, int fd, int bitadd);
int events_poll(struct _fdevent *ev, int timeout_ms);
int events_get_current_fd(struct _fdevent *ev, int i);
void events_growup(struct _fdevent *ev);
int events_revent(struct _fdevent *ev, int i);
int events_reload(struct _fdevent *ev);

int event_kqueue_init(struct _fdevent *ev);
int event_epoll_init(struct _fdevent *ev);

#endif
