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

/* sock.c */

#define _GNU_SOURCE 

#include "sock.h"
#include "handle_http.h"
#include "http.h"
#include "users.h"

#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

#include "utils.h"
#include "ticks.h"

static void setnonblocking(int fd);


static int newSockListen(unsigned int port) // BIND
{
	int sock;
	struct sockaddr_in addr;
	
	
	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	{
		printf("ERREUR: socket().. (%s line: %i)\n",__FILE__, __LINE__);
		return -1;
	}
	
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	memset(&(addr.sin_zero), '\0', 8);

	if (bind(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr)) == -1)
	{
		printf("ERREUR: bind(%i).. (%s line: %i)\n", port, __FILE__, __LINE__);
		return -3;
	}

	if (listen(sock, 50000) == -1)
	{
		printf("ERREUR: listen().. (%s line: %i)\n",__FILE__, __LINE__);
		return -4;
	}

	return sock;
}

static void growup(int *basemem, connection **conn_list, struct epoll_event **epoll)
{
	*basemem *= 2;
	
	*epoll = xrealloc(*epoll, 
			sizeof(struct epoll_event) * (*basemem));
	
	*conn_list = xrealloc(*conn_list, 
			sizeof(connection) * (*basemem));
}

static void setnonblocking(int fd)
{
	int old_flags;
	
	old_flags = fcntl(fd, F_GETFL, 0);
	
	if (!(old_flags & O_NONBLOCK)) {
		old_flags |= O_NONBLOCK;
	}
	fcntl(fd, F_SETFL, old_flags);	
}

static void clear_buffer(connection *co)
{
	free(co->buffer.data);
	co->buffer.size = 0;
	co->buffer.length = 0;
	co->ip_client[0] = '\0';
	
	co->http.step = 0;
	co->http.type = HTTP_NULL;
	co->http.contentlength = -1;
	co->http.pos = 0;
	co->http.error = 0;
	co->http.ready = 0;
	co->http.read = 0;
	co->user = NULL;
}

unsigned int sockroutine(size_t port, acetables *g_ape)
{
	int basemem = 16, epoll_fd;
	
	struct epoll_event ev, *events;

	int s_listen, new_fd, nfds, sin_size = sizeof(struct sockaddr_in), i;
	
	struct timeval t_start, t_end;
	
	unsigned int ticks = 0;

	struct sockaddr_in their_addr;
	
	connection *co = xmalloc(sizeof(*co) * basemem);

	epoll_fd = epoll_create(40000);
	
	events = xmalloc(sizeof(*events) * basemem);


	if ((s_listen = newSockListen(port)) < 0) {
		return 0;
	}

	setnonblocking(s_listen);
	
	
	ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLPRI;
	ev.data.fd = s_listen;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, s_listen, &ev);

	while (1) {
		
		gettimeofday(&t_start, NULL);
		
		nfds = epoll_wait(epoll_fd, events, basemem, (1000/TICKS_RATE)-ticks);
		
		if (nfds < 0) {
			continue;
		}
		if (nfds > 0) {
			for (i = 0; i < nfds; i++) {

				if (events[i].data.fd == s_listen) {
				
					while (1) {
						struct epoll_event cev;
						http_state http;
					
						new_fd = accept(s_listen, 
							(struct sockaddr *)&their_addr, 
							(unsigned int *)&sin_size);
					
						if (new_fd == -1) {
							break;
						}
						/*
							New connection
						*/
						if (new_fd + 4 == basemem) {
							/*
								Increase connection & events size
							*/
							growup(&basemem, &co, &events);
						}
					
					
						http.step = 0;
						http.type = HTTP_NULL;
						http.contentlength = -1;
						http.pos = 0;
						http.error = 0;
						http.ready = 0;
						http.read = 0;
					
						strncpy(co[new_fd].ip_client, inet_ntoa(their_addr.sin_addr), 16);
					
						co[new_fd].buffer.data = xmalloc(sizeof(char) * (DEFAULT_BUFFER_SIZE + 1));
						co[new_fd].buffer.size = DEFAULT_BUFFER_SIZE;
						co[new_fd].buffer.length = 0;
					
						co[new_fd].http = http;
						co[new_fd].user = NULL;
					
						setnonblocking(new_fd);	

						cev.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLPRI;
						cev.data.fd = new_fd;
					
						epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_fd, &cev);

					
					}
					continue;
				} else {
					int readb = 0;

					do {
						/*
							TODO : Check if maximum data read can improve perf
							Huge data may attempt to increase third parameter
						*/
						readb = read(events[i].data.fd, 
									co[events[i].data.fd].buffer.data + co[events[i].data.fd].buffer.length, 
									co[events[i].data.fd].buffer.size - co[events[i].data.fd].buffer.length);
					
						if (readb == -1 && errno == EAGAIN) {
							/*
								Nothing to read again
							*/
							co[events[i].data.fd].buffer.data[co[events[i].data.fd].buffer.length] = '\0';
	
							break;
						} else {
							if (readb < 1) {
								if (events[i].events & EPOLLRDHUP) {
									// Client half closed the connection
									// this can be useful to avoid "wait for free"
								}

								if (co[events[i].data.fd].user != NULL) {
									
									if (events[i].data.fd == co[events[i].data.fd].user->fd) {
										co[events[i].data.fd].user->state = ADIED;
									}
									if (co[events[i].data.fd].user->wait_for_free == 1) {
										free(co[events[i].data.fd].user);
										co[events[i].data.fd].user = NULL;						
									}
								}
								
								clear_buffer(&co[events[i].data.fd]);
								close(events[i].data.fd);

							
								break;
							} else if (co[events[i].data.fd].http.ready != -1) {
								co[events[i].data.fd].buffer.length += readb;

								if (co[events[i].data.fd].buffer.length == co[events[i].data.fd].buffer.size) {
									co[events[i].data.fd].buffer.size *= 2;
									co[events[i].data.fd].buffer.data = xrealloc(co[events[i].data.fd].buffer.data, 
															sizeof(char) * (co[events[i].data.fd].buffer.size + 1));
								}
														
								process_http(&co[events[i].data.fd]);
							
								if (co[events[i].data.fd].http.ready == 1) {
									co[events[i].data.fd].user = checkrecv(co[events[i].data.fd].buffer.data, events[i].data.fd, g_ape, co[events[i].data.fd].ip_client);
									
									co[events[i].data.fd].buffer.length = 0;
									co[events[i].data.fd].http.ready = -1;
			
								} else if (co[events[i].data.fd].http.error == 1) {
									shutdown(events[i].data.fd, 2);
								}

							}
						
						}
					
					} while(readb >= 0);
				}			
			}
		}
		
		gettimeofday(&t_end, NULL);
		
		// How many "ms" since last loop ?
		ticks += (1000*(t_end.tv_sec - t_start.tv_sec))+((t_end.tv_usec - t_start.tv_usec)/1000);
		
		// Tic tac, tic tac :-)
		if (ticks >= 1000/TICKS_RATE) {
			ticks = 0;
			process_tick(g_ape);
		}                

	}

	close(epoll_fd);
	return 0;
}

int sendf(int sock, char *buf, ...)
{
	char *buff;
	
	int len, t_bytes = 0, r_bytes, n = 0;
	
	va_list val;
	
	va_start(val, buf);
	vasprintf(&buff, buf, val);
	va_end(val);	


	len = strlen(buff);
	r_bytes = len;
	
	if (sock != 0) {
		while(t_bytes < len) {
			n = send(sock, buff + t_bytes, r_bytes, 0);
			if (n == -1) {
				break; 
			}
			t_bytes += n;
			r_bytes -= n;
		}
	} else { // It's a Bot !
		printf("Bot: %s\n", buff);
	}
	len = t_bytes;
	free(buff);

	return (n == -1 ? -1 : 0);
}

