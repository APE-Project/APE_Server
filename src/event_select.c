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

/**
 *  @file	event_select.c		APE event manager for POSIX select back-end
 *  @author	Wes Garland, wes@page.ca
 *  @date	Nov 2010
 */
#include "events.h"
#include <syslog.h>
#include <sys/time.h>
#include <time.h>
#include <sys/socket.h>
#include <errno.h>

#ifdef USE_SELECT_HANDLER
/**
 *  Mark a file descriptor as "in use" by setting the first bit in the corresponding 
 *  array entry.
 */
static int event_select_add(struct _fdevent *ev, int fd, int bitadd)
{
  if (fd < 0 || fd > MAX_SELECT_FDS)
  {
    syslog(LOG_NOTICE, "File descriptor %i is out of range", fd);
    return -1;
  }
  
  if (bitadd & EVENT_READ) 
    ev->fds[fd].read |= 1;

  if (bitadd & EVENT_WRITE) 
    ev->fds[fd].write |= 1;

  ev->fds[fd].fd = fd;

  return 1;
}

static int event_select_remove(struct _fdevent *ev, int fd)
{
  ev->fds[fd].read = 0;
  ev->fds[fd].write = 0;

  return 1;
}

/**
 *  Wait for one or more sockets to be ready, mark the ready 
 *  ones by setting the second bit, and clear the others' 2nd bits.
 */
static int event_select_poll(struct _fdevent *ev, int timeout_ms)
{
  struct timeval tv;
  int		 fd, i, maxfd, numfds;
  fd_set	 rfds, wfds, efds;

  memset(&tv, 0, sizeof(tv));
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  FD_ZERO(&rfds);
  FD_ZERO(&wfds);
  FD_ZERO(&efds);

  for (fd=0, maxfd=0; fd < MAX_SELECT_FDS; fd++)
  {
    if (ev->fds[fd].read)
      FD_SET(fd, &rfds);
    if (ev->fds[fd].write)
      FD_SET(fd, &wfds);
    if (ev->fds[fd].read || ev->fds[fd].write)
    {
      FD_SET(fd, &efds);
      if (fd > maxfd)
	maxfd = fd;
    }
  }
  
  errno = 0;
  numfds = select(maxfd + 1, &rfds, &wfds, &efds, &tv);

  if (numfds == -1)
    return -1;

  /* Mark pending data */
  for (fd=0; fd <= maxfd; fd++)
  {
    if (FD_ISSET(fd, &efds))
    {
      printf("XXX ignoring fd %i with OOB data\n", fd);
      continue;
    }

    if (FD_ISSET(fd, &rfds))
      ev->fds[fd].read |= 2;
    else
      ev->fds[fd].read &= 1;

    if (FD_ISSET(fd, &wfds))
      ev->fds[fd].write |= 2;
    else
      ev->fds[fd].write &= 1;
  }
  
  /* Create the events array for event_select_revent et al */
  for (fd=0, i=0; fd <= maxfd; fd++)
  {
    if (FD_ISSET(fd, &rfds) || FD_ISSET(fd, &wfds))
    {
      ev->events[i++] = &ev->fds[fd];
    }
  }

  return i; /* i != numfds; sometimes read/write each count */
}

static int event_select_get_fd(struct _fdevent *ev, int i)
{
  return ev->events[i]->fd;
}

static void event_select_growup(struct _fdevent *ev)
{
  /* growup() increments basemem so that it is always the same size or 
   * bigger than the biggest fd returned by accept()
   */
  ev->events = xrealloc(ev->events, sizeof(ev->events[0]) * (*ev->basemem));
}

static int event_select_revent(struct _fdevent *ev, int i)
{
  int bitret = 0;
  int fd = ev->events[i]->fd;

  if (ev->fds[fd].read & 0x02)
    bitret |= EVENT_READ;

  if (ev->fds[fd].write & 0x02)
    bitret |= EVENT_WRITE;

  /* reset pending data bits */
  ev->fds[fd].read &= 0x01;
  ev->fds[fd].write &= 0x01;

  return bitret;
}

int event_select_reload(struct _fdevent *ev)
{
  memset(ev->fds, 0, sizeof(ev->fds));
  return 1;
}

int event_select_init(struct _fdevent *ev)
{
  ev->events = xmalloc(sizeof(*ev->events) * (*ev->basemem));
  memset(ev->fds, 0, sizeof(ev->fds));

  ev->events 		= malloc(1); /* events.c frees */
  ev->add 		= event_select_add;
  ev->remove		= event_select_remove;
  ev->poll 		= event_select_poll;
  ev->get_current_fd 	= event_select_get_fd;
  ev->growup 		= event_select_growup;
  ev->revent 		= event_select_revent;
  ev->reload 		= event_select_reload;

  return 1;
}

#else
int event_select_init(struct _fdevent *ev)
{
  return 0;
}
#endif
