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
#include "proxy.h"
#include "pipe.h"

static int sendqueue(int sock, acetables *g_ape);

static int newSockListen(unsigned int port) // BIND
{
	int sock;
	struct sockaddr_in addr;
	int reuse_addr = 1;
	
	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	{
		printf("ERREUR: socket().. (%s line: %i)\n",__FILE__, __LINE__);
		return -1;
	}
	
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	memset(&(addr.sin_zero), '\0', 8);
	
	
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));

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

static void growup(int *basemem, connection **conn_list, struct epoll_event **epoll, struct _socks_bufout **bufout)
{
	*basemem *= 2;

	*epoll = xrealloc(*epoll, 
			sizeof(struct epoll_event) * (*basemem));
	
	*conn_list = xrealloc(*conn_list, 
			sizeof(connection) * (*basemem));
						
	*bufout = xrealloc(*bufout, 
			sizeof(struct _socks_bufout) * (*basemem));
}

void setnonblocking(int fd)
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
	co->attach = NULL;
}

unsigned int sockroutine(size_t port, acetables *g_ape)
{
	int basemem = 512, epoll_fd;
	struct epoll_event ev, *events;

	int s_listen, new_fd, nfds, sin_size = sizeof(struct sockaddr_in), i;
	
	struct timeval t_start, t_end;	
	unsigned int ticks = 0;
	struct sockaddr_in their_addr;
	
	
	connection *co = xmalloc(sizeof(*co) * basemem);

	epoll_fd = epoll_create(40000);
	
	g_ape->epoll_fd = &epoll_fd;
	
	events = xmalloc(sizeof(*events) * basemem);
	
	g_ape->bufout = xmalloc(sizeof(struct _socks_bufout) * basemem);

	if ((s_listen = newSockListen(port)) < 0) {
		return 0;
	}

	setnonblocking(s_listen);
	
	ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLPRI;
	ev.data.fd = s_listen;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, s_listen, &ev);
	
	while (1) {
		
		/* Linux 2.6.25 provide a fd-driven timer system. It could be usefull to implement */
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
						http_state http = {0, HTTP_NULL, 0, -1, 0, 0, 0};
					
						new_fd = accept(s_listen, 
							(struct sockaddr *)&their_addr, 
							(unsigned int *)&sin_size);
					
						if (new_fd == -1) {
							break;
						}
						
						if (new_fd + 4 == basemem) {
							/*
								Increase connection & events size
							*/
							growup(&basemem, &co, &events, &g_ape->bufout);
						}

						strncpy(co[new_fd].ip_client, inet_ntoa(their_addr.sin_addr), 16);
						
						co[new_fd].buffer.data = xmalloc(sizeof(char) * (DEFAULT_BUFFER_SIZE + 1));
						co[new_fd].buffer.size = DEFAULT_BUFFER_SIZE;
						co[new_fd].buffer.length = 0;
					
						co[new_fd].http = http;
						co[new_fd].attach = NULL;
						
						co[new_fd].stream_type = STREAM_IN;
					
						g_ape->bufout[new_fd].fd = new_fd;
						g_ape->bufout[new_fd].buf = NULL;
						g_ape->bufout[new_fd].buflen = 0;
					
						setnonblocking(new_fd);

						cev.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLPRI | EPOLLOUT;
						cev.data.fd = new_fd;
					
						epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_fd, &cev);
						
					
					}
					continue;
				} else {
					int readb = 0;
					
					if (events[i].events & EPOLLOUT) {
						if (co[events[i].data.fd].stream_type == STREAM_OUT && 
						((ape_proxy *)(co[events[i].data.fd].attach))->state == PROXY_IN_PROGRESS) {
							int serror = 0, ret;
							socklen_t serror_len = sizeof(serror);
						
							ret = getsockopt(events[i].data.fd, SOL_SOCKET, SO_ERROR, &serror, &serror_len);
							
							if (ret == 0 && serror == 0) {
								((ape_proxy *)(co[events[i].data.fd].attach))->state = PROXY_CONNECTED;
								((ape_proxy *)(co[events[i].data.fd].attach))->sock.fd = events[i].data.fd;
								proxy_onevent((ape_proxy *)(co[events[i].data.fd].attach), "CONNECT", g_ape);
							} else { /* This can be happen ? epoll seems set EPOLLIN as if the host is disconnecting */
								((ape_proxy *)(co[events[i].data.fd].attach))->state = PROXY_THROTTLED;
								//epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
								clear_buffer(&co[events[i].data.fd]);
								close(events[i].data.fd);
							}

						} else if (co[events[i].data.fd].stream_type == STREAM_IN && g_ape->bufout[events[i].data.fd].buf != NULL) {

							if (sendqueue(events[i].data.fd, g_ape) == 1) {
								if (co[events[i].data.fd].attach != NULL && ((subuser *)(co[events[i].data.fd].attach))->burn_after_writing) {
									do_died((subuser *)(co[events[i].data.fd].attach));
									((subuser *)(co[events[i].data.fd].attach))->burn_after_writing = 0;
								}
							}
						}
					}
					if (events[i].events & EPOLLIN) {
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
							
								if (co[events[i].data.fd].stream_type == STREAM_OUT) {
									
										proxy_process_eol(&co[events[i].data.fd], g_ape);
										co[events[i].data.fd].buffer.length = 0;
								} else {
									co[events[i].data.fd].buffer.data[co[events[i].data.fd].buffer.length] = '\0';
								}
								break;
							} else {
								if (readb < 1) {
									#if 0
									TODO :
									if (events[i].events & EPOLLRDHUP) {
										/* 
										   Client hangup the connection (half-closed)
										*/
									}
									#endif
									if (co[events[i].data.fd].stream_type == STREAM_IN && co[events[i].data.fd].attach != NULL) {
									
										if (events[i].data.fd == ((subuser *)(co[events[i].data.fd].attach))->fd) {
											((subuser *)(co[events[i].data.fd].attach))->headers_sent = 0;
											((subuser *)(co[events[i].data.fd].attach))->state = ADIED;
										}
										if (((subuser *)(co[events[i].data.fd].attach))->wait_for_free == 1) {
											free(co[events[i].data.fd].attach);
											co[events[i].data.fd].attach = NULL;						
										}
									} else if (co[events[i].data.fd].stream_type == STREAM_OUT) {
									
										if (((ape_proxy *)(co[events[i].data.fd].attach))->state == PROXY_TOFREE) {
											free(co[events[i].data.fd].attach);
											co[events[i].data.fd].attach = NULL;								
										} else {
									
											((ape_proxy *)(co[events[i].data.fd].attach))->state = PROXY_THROTTLED;
											proxy_onevent((ape_proxy *)(co[events[i].data.fd].attach), "DISCONNECT", g_ape);
										}
									}
								
									clear_buffer(&co[events[i].data.fd]);
								
									if (g_ape->bufout[events[i].data.fd].buf != NULL) {
										free(g_ape->bufout[events[i].data.fd].buf);
										g_ape->bufout[events[i].data.fd].buflen = 0;
										g_ape->bufout[events[i].data.fd].buf = NULL;
									}
									
									close(events[i].data.fd);
							
									break;
								} else if (co[events[i].data.fd].http.ready != -1) {
									co[events[i].data.fd].buffer.length += readb;

									if (co[events[i].data.fd].buffer.length == co[events[i].data.fd].buffer.size) {
										co[events[i].data.fd].buffer.size *= 2;
										co[events[i].data.fd].buffer.data = xrealloc(co[events[i].data.fd].buffer.data, 
																sizeof(char) * (co[events[i].data.fd].buffer.size + 1));
									
									}
									if (co[events[i].data.fd].stream_type == STREAM_IN) {
										process_http(&co[events[i].data.fd]);
								
										if (co[events[i].data.fd].http.ready == 1) {
											co[events[i].data.fd].attach = checkrecv(co[events[i].data.fd].buffer.data, 
																events[i].data.fd, g_ape, co[events[i].data.fd].ip_client);
									
											co[events[i].data.fd].buffer.length = 0;
											co[events[i].data.fd].http.ready = -1;
			
										} else if (co[events[i].data.fd].http.error == 1) {
											shutdown(events[i].data.fd, 2);
										}
									}
								}
						
							}
					
						} while(readb >= 0);
					}
				}			
			}
		}
		
		gettimeofday(&t_end, NULL);

		ticks += (1000*(t_end.tv_sec - t_start.tv_sec))+((t_end.tv_usec - t_start.tv_usec)/1000);
		
		/* Tic tac, tic tac :-) */
		if (ticks >= 1000/TICKS_RATE) {
			ape_proxy *proxy = g_ape->proxy.list;
			int psock;
			
			ticks = 0;
			
			while (proxy != NULL) {

				if (proxy->state == PROXY_NOT_CONNECTED && ((psock = proxy_connect(proxy, g_ape)) != 0)) {
					http_state http_s = {0, HTTP_NULL, 0, -1, 0, 0, 0};
					if (psock + 4 == basemem) {
						growup(&basemem, &co, &events, &g_ape->bufout);
					}
					co[psock].ip_client[0] = '\0';
					co[psock].buffer.data = xmalloc(sizeof(char) * (DEFAULT_BUFFER_SIZE + 1));
					co[psock].buffer.size = DEFAULT_BUFFER_SIZE;
					co[psock].buffer.length = 0;
				
					co[psock].http = http_s;
					co[psock].attach = proxy;
					co[psock].stream_type = STREAM_OUT;
				}


				proxy = proxy->next;
			}
			
			process_tick(g_ape);
		}                

	}

	close(epoll_fd);
	return 0;
}

int sendf(int sock, acetables *g_ape, char *buf, ...)
{
	char *buff;
	
	int finish;
	
	va_list val;
	
	va_start(val, buf);
	vasprintf(&buff, buf, val);
	va_end(val);

	finish = sendbin(sock, buff, strlen(buff), g_ape);

	free(buff);

	return finish;
}

static int sendqueue(int sock, acetables *g_ape)
{
	int t_bytes = 0, r_bytes, n = 0;
	struct _socks_bufout *bufout = &g_ape->bufout[sock];
	
	if (bufout->buf == NULL) {
		return 1;
	}
	
	r_bytes = bufout->buflen;
	
	while(t_bytes < bufout->buflen) {
		n = write(sock, bufout->buf + t_bytes, r_bytes);
		if (n == -1) {
			if (errno == EAGAIN && r_bytes > 0) {
				memmove(bufout->buf, bufout->buf + t_bytes, r_bytes);
				bufout->buflen = r_bytes;
				/* Decrease memory size ? */
				return 0;
			}
			break;
		}
		t_bytes += n;
		r_bytes -= n;		
	}
	
	bufout->buflen = 0;
	free(bufout->buf);

	bufout->buf = NULL;
	
	return 1;
}

int sendbin(int sock, char *bin, int len, acetables *g_ape)
{
	int t_bytes = 0, r_bytes, n = 0;

	r_bytes = len;
	
	if (sock != 0) {
		while(t_bytes < len) {
			n = write(sock, bin + t_bytes, r_bytes);
			if (n == -1) {
				if (errno == EAGAIN && r_bytes > 0) {
					if (g_ape->bufout[sock].buf == NULL) {
						/* TODO : Allocate more than r_bytes to decrease realloc calls */
						g_ape->bufout[sock].buf = xmalloc(sizeof(char) * r_bytes);
						g_ape->bufout[sock].buflen = r_bytes;
					} else {
						g_ape->bufout[sock].buflen += r_bytes;						
						g_ape->bufout[sock].buf = xrealloc(g_ape->bufout[sock].buf, sizeof(char) * g_ape->bufout[sock].buflen);
					}
					
					memcpy(g_ape->bufout[sock].buf + (g_ape->bufout[sock].buflen - r_bytes), bin + t_bytes, r_bytes);
					
					return 0;
				}
				break; 
			}
			t_bytes += n;
			r_bytes -= n;
		}
	}

	return 1;
}

