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

/* sock.c */

#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

#include "sock.h"
#include "http.h"
#include "users.h"

#include "utils.h"
#include "ticks.h"
#include "proxy.h"
#include "config.h"
#include "raw.h"
#include "events.h"
#include "transports.h"
#include "handle_http.h"
#include "dns.h"
#include "log.h"
#include "parser.h"

static int sendqueue(int sock, acetables *g_ape);


static void growup(int *basemem, ape_socket ***conn_ptr, struct _fdevent *ev, struct _socks_bufout **bufout)
{
	int old_basemem = *basemem;
	*basemem *= 2;
	
	events_growup(ev);
	
	*conn_ptr = xrealloc(*conn_ptr,	sizeof(ape_socket) * (*basemem));
	memset(&((*conn_ptr)[*basemem - old_basemem]), 0, sizeof(**conn_ptr) * (*basemem - old_basemem));
	
	*bufout = xrealloc(*bufout, sizeof(struct _socks_bufout) * (*basemem));
	memset(&((*bufout)[*basemem - old_basemem]), 0, sizeof(**bufout) * (*basemem - old_basemem));
}

ape_socket *ape_listen(unsigned int port, char *listen_ip, acetables *g_ape)
{
	int sock;
	struct sockaddr_in addr;
	int reuse_addr = 1;
	
	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	{
		ape_log(APE_ERR, __FILE__, __LINE__, g_ape, 
			"ape_listen() - socket()");
		return NULL;
	}
	
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	//addr.sin_addr.s_addr = inet_addr(CONFIG_VAL(Server, ip_listen, g_ape->srv));
	
	addr.sin_addr.s_addr = inet_addr(listen_ip);
	
	memset(&(addr.sin_zero), '\0', 8);
	
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));

	if (bind(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr)) == -1)
	{
		ape_log(APE_ERR, __FILE__, __LINE__, g_ape, 
			"ape_listen() - bind()");
		printf("ERREUR: bind(%i) (non-root ?).. (%s line: %i)\n", port, __FILE__, __LINE__);
		return NULL;
	}

	if (listen(sock, 2048) == -1)
	{
		ape_log(APE_ERR, __FILE__, __LINE__, g_ape, 
			"ape_listen() - listen()");
		return NULL;
	}
	
	setnonblocking(sock);

	prepare_ape_socket(sock, g_ape);

	g_ape->co[sock]->fd = sock;
	g_ape->co[sock]->state = STREAM_ONLINE;
	g_ape->co[sock]->stream_type = STREAM_SERVER;
	
	events_add(g_ape->events, sock, EVENT_READ);
	
	return g_ape->co[sock];
}

ape_socket *ape_connect(char *ip, int port, acetables *g_ape)
{
	int sock, ret;
	struct sockaddr_in addr;
	
	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		ape_log(APE_ERR, __FILE__, __LINE__, g_ape, 
			"ape_connect() - socket() : %s", strerror(errno));
		return NULL;
	}

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = inet_addr(ip);
	memset(&(addr.sin_zero), '\0', 8);

	setnonblocking(sock);
	
	if (connect(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr)) == 0 || errno != EINPROGRESS) {
		return NULL;
	}

	prepare_ape_socket(sock, g_ape);
	
	g_ape->co[sock]->buffer_in.data = xmalloc(sizeof(char) * (DEFAULT_BUFFER_SIZE + 1));
	g_ape->co[sock]->buffer_in.size = DEFAULT_BUFFER_SIZE;

	g_ape->co[sock]->fd = sock;
	g_ape->co[sock]->state = STREAM_PROGRESS;
	g_ape->co[sock]->stream_type = STREAM_OUT;
	
	g_ape->bufout[sock].fd = sock;
	g_ape->bufout[sock].buf = NULL;
	g_ape->bufout[sock].buflen = 0;
	g_ape->bufout[sock].allocsize = 0;

	ret = events_add(g_ape->events, sock, EVENT_READ|EVENT_WRITE);
	
	return g_ape->co[sock];	
}

static void ape_connect_name_cb(char *ip, void *data, acetables *g_ape)
{
	struct _ape_sock_connect_async *asca = data;
	ape_socket *sock;

	if ((sock = ape_connect(ip, asca->port, g_ape)) != NULL) {
		
		sock->attach = asca->sock->attach;
		
		sock->callbacks.on_accept = asca->sock->callbacks.on_accept;         	
		sock->callbacks.on_connect = asca->sock->callbacks.on_connect;
		sock->callbacks.on_disconnect = asca->sock->callbacks.on_disconnect;
		sock->callbacks.on_read = asca->sock->callbacks.on_read;
		sock->callbacks.on_read_lf = asca->sock->callbacks.on_read_lf;
		sock->callbacks.on_data_completly_sent = asca->sock->callbacks.on_data_completly_sent;
		sock->callbacks.on_write = asca->sock->callbacks.on_write;
	}
	
	free(ip);
	free(asca->sock);
	free(asca);
	
}

void ape_connect_name(char *name, int port, ape_socket *pattern, acetables *g_ape)
{
	struct _ape_sock_connect_async *asca = xmalloc(sizeof(*asca));
	
	asca->sock = pattern;
	asca->port = port;

	ape_gethostbyname(name, ape_connect_name_cb, asca, g_ape);
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

/* Close socket but preserve ape_socket struct */
void close_socket(int fd, acetables *g_ape)
{
	ape_socket *co = g_ape->co[fd];

	if (g_ape->bufout[fd].buf != NULL) {
		free(g_ape->bufout[fd].buf);
		g_ape->bufout[fd].buflen = 0;
		g_ape->bufout[fd].buf = NULL;
		g_ape->bufout[fd].allocsize = 0;
	}

	if (co->buffer_in.data != NULL) {
		free(co->buffer_in.data);
	}

	if (co->parser.data != NULL) {
		parser_destroy(&co->parser);
	}

	close(fd);
}

/* Create socket struct if not exists */
void prepare_ape_socket(int fd, acetables *g_ape)
{
	while (fd >= g_ape->basemem) {
		/* Increase connection & events size */
		growup(&g_ape->basemem, &g_ape->co, g_ape->events, &g_ape->bufout);
	}
	
	if (g_ape->co[fd] == NULL) {
		g_ape->co[fd] = xmalloc(sizeof(*g_ape->co[fd]));
	}
	
	memset(g_ape->co[fd], 0, sizeof(*g_ape->co[fd]));
}

#if 0
static void check_idle(struct _socks_list *sl)
{
	int i = 0, x = 0;
	long int current_time = time(NULL);
	printf("Tick tac \n");
	for (i = 0; x < *sl->tfd; i++) {
		if (sl->co[i].buffer.size) {
			x++;
			if (sl->co[i].attach == NULL && sl->co[i].idle <= current_time-TCP_TIMEOUT) {
				shutdown(i, 2);
			}
		}
		
	}
}
#endif

unsigned int sockroutine(acetables *g_ape)
{
	struct _socks_list sl;
	
	int new_fd, nfds, sin_size = sizeof(struct sockaddr_in), i, tfd = 0;

	struct timeval t_start, t_end;	
	long int ticks = 0, uticks = 0, lticks = 0;
	struct sockaddr_in their_addr;
	
	//sl.co = co;
	sl.tfd = &tfd;

	#if 0
	add_periodical(5, 0, check_idle, &sl, g_ape);
	#endif
	gettimeofday(&t_start, NULL);
	while (server_is_running) {
		/* Linux 2.6.25 provides a fd-driven timer system. It could be usefull to implement */
		int timeout_to_hang = get_first_timer_ms(g_ape);
		nfds = events_poll(g_ape->events, timeout_to_hang);
		
		if (nfds < 0) {
			ape_log(APE_ERR, __FILE__, __LINE__, g_ape, 
				"events_poll() : %s", strerror(errno));
			continue;
		}
		
		if (nfds > 0) {
			for (i = 0; i < nfds; i++) {

				int active_fd = events_get_current_fd(g_ape->events, i);
				
				if (g_ape->co[active_fd]->stream_type == STREAM_SERVER) {
					int bitev = events_revent(g_ape->events, i);
					
					if (!(bitev & EVENT_READ)) {
						/* Close server socket */
						close_socket(active_fd, g_ape);
						continue;
					}
					
					while (1) {

						//http_state http = {NULL, 0, -1, 0, 0, HTTP_NULL, 0, 0};
					
						new_fd = accept(active_fd, 
							(struct sockaddr *)&their_addr,
							(unsigned int *)&sin_size);

						if (new_fd == -1) {
							break;
						}
						
						prepare_ape_socket(new_fd, g_ape);
	
						strncpy(g_ape->co[new_fd]->ip_client, inet_ntoa(their_addr.sin_addr), 16);
						
						g_ape->co[new_fd]->buffer_in.data = xmalloc(sizeof(char) * (DEFAULT_BUFFER_SIZE + 1));
						g_ape->co[new_fd]->buffer_in.size = DEFAULT_BUFFER_SIZE;

						g_ape->co[new_fd]->idle = time(NULL);
						g_ape->co[new_fd]->fd = new_fd;

						g_ape->co[new_fd]->state = STREAM_ONLINE;
						g_ape->co[new_fd]->stream_type = STREAM_IN;
					
						g_ape->bufout[new_fd].fd = new_fd;
						g_ape->bufout[new_fd].buf = NULL;
						g_ape->bufout[new_fd].buflen = 0;
						g_ape->bufout[new_fd].allocsize = 0;
						
						g_ape->co[new_fd]->callbacks.on_disconnect = g_ape->co[active_fd]->callbacks.on_disconnect;
						g_ape->co[new_fd]->callbacks.on_read = g_ape->co[active_fd]->callbacks.on_read;
						g_ape->co[new_fd]->callbacks.on_read_lf = g_ape->co[active_fd]->callbacks.on_read_lf;
						g_ape->co[new_fd]->callbacks.on_data_completly_sent = g_ape->co[active_fd]->callbacks.on_data_completly_sent;
						g_ape->co[new_fd]->callbacks.on_write = g_ape->co[active_fd]->callbacks.on_write;
						
						g_ape->co[new_fd]->attach = g_ape->co[active_fd]->attach;
						
						setnonblocking(new_fd);
						
						events_add(g_ape->events, new_fd, EVENT_READ|EVENT_WRITE);
						
						tfd++;
						
						if (g_ape->co[active_fd]->callbacks.on_accept != NULL) {
							g_ape->co[active_fd]->callbacks.on_accept(g_ape->co[new_fd], g_ape);
						}
					
					}
					continue;
				} else {
					int readb = 0;
					int bitev = events_revent(g_ape->events, i);
						
					if (bitev & EVENT_WRITE) {

						if (g_ape->co[active_fd]->stream_type == STREAM_OUT && g_ape->co[active_fd]->state == STREAM_PROGRESS) {
							
							int serror = 0, ret;
							socklen_t serror_len = sizeof(serror);
						
							ret = getsockopt(active_fd, SOL_SOCKET, SO_ERROR, &serror, &serror_len);
							
							if (ret == 0 && serror == 0) {

								g_ape->co[active_fd]->state = STREAM_ONLINE;
								if (g_ape->co[active_fd]->callbacks.on_connect != NULL) {

									g_ape->co[active_fd]->callbacks.on_connect(g_ape->co[active_fd], g_ape);
								}
							} else { /* This can happen ? epoll seems set EPOLLIN as if the host is disconnecting */

								if (g_ape->co[active_fd]->callbacks.on_disconnect != NULL) {
									g_ape->co[active_fd]->callbacks.on_disconnect(g_ape->co[active_fd], g_ape);
								}

								close_socket(active_fd, g_ape);
								tfd--;
							}							
						} else if (g_ape->bufout[active_fd].buf != NULL) {

							if (sendqueue(active_fd, g_ape) == 1) {
								
								if (g_ape->co[active_fd]->callbacks.on_data_completly_sent != NULL) {
									g_ape->co[active_fd]->callbacks.on_data_completly_sent(g_ape->co[active_fd], g_ape);
								}
								
								if (g_ape->co[active_fd]->burn_after_writing) {
									shutdown(active_fd, 2);
									g_ape->co[active_fd]->burn_after_writing = 0;
								}

							}
						} else if (g_ape->co[active_fd]->stream_type == STREAM_DELEGATE) {
							if (g_ape->co[active_fd]->callbacks.on_write != NULL) {
								g_ape->co[active_fd]->callbacks.on_write(g_ape->co[active_fd], g_ape);

							}							
						}
					}

					if (bitev & EVENT_READ) {
						if (g_ape->co[active_fd]->stream_type == STREAM_DELEGATE) {
							if (g_ape->co[active_fd]->callbacks.on_read != NULL) {
								g_ape->co[active_fd]->callbacks.on_read(g_ape->co[active_fd], NULL, 0, g_ape);
								continue;
							}							
						}
						do {
							/*
								TODO : Check if maximum data read can improve perf
								Huge data may attempt to increase third parameter
							*/
							readb = read(active_fd, 
										g_ape->co[active_fd]->buffer_in.data + g_ape->co[active_fd]->buffer_in.length, 
										g_ape->co[active_fd]->buffer_in.size - g_ape->co[active_fd]->buffer_in.length);
						
						
							if (readb == -1 && errno == EAGAIN) {

								if (g_ape->co[active_fd]->stream_type == STREAM_OUT) {
									
										//proxy_process_eol(&co[active_fd], g_ape);
										//co[active_fd].buffer_in.length = 0;
								} else {
								//	co[active_fd].buffer_in.data[co[active_fd].buffer_in.length] = '\0';
								}
								break;
							} else {
								if (readb < 1) {

									if (g_ape->co[active_fd]->callbacks.on_disconnect != NULL) {
										g_ape->co[active_fd]->callbacks.on_disconnect(g_ape->co[active_fd], g_ape);
									}
									
									close_socket(active_fd, g_ape);
									tfd--;
									
									break;
								} else {
									
									g_ape->co[active_fd]->buffer_in.length += readb;
									
									/* realloc the buffer for the next read (x2) */
									if (g_ape->co[active_fd]->buffer_in.length == g_ape->co[active_fd]->buffer_in.size) {
										g_ape->co[active_fd]->buffer_in.size *= 2;

										g_ape->co[active_fd]->buffer_in.data = xrealloc(g_ape->co[active_fd]->buffer_in.data, 
																sizeof(char) * (g_ape->co[active_fd]->buffer_in.size + 1));

									}
									if (g_ape->co[active_fd]->callbacks.on_read_lf != NULL) {
										unsigned int eol, *len = &g_ape->co[active_fd]->buffer_in.length;
										char *pBuf = g_ape->co[active_fd]->buffer_in.data;

										while ((eol = sneof(pBuf, *len, 4096)) != -1) {
											pBuf[eol-1] = '\0';
											g_ape->co[active_fd]->callbacks.on_read_lf(g_ape->co[active_fd], pBuf, g_ape);
											pBuf = &pBuf[eol];
											*len -= eol;
										}
										if (*len > 4096 || !*len) {
											g_ape->co[active_fd]->buffer_in.length = 0;
										} else if (*len && pBuf != g_ape->co[active_fd]->buffer_in.data) {
											
											memmove(g_ape->co[active_fd]->buffer_in.data, 
												pBuf, 
												*len);

										}

									}
									
									/* on_read can't get along with on_read_lf */
									if (g_ape->co[active_fd]->callbacks.on_read != NULL && g_ape->co[active_fd]->callbacks.on_read_lf == NULL) {
										g_ape->co[active_fd]->callbacks.on_read(g_ape->co[active_fd], &g_ape->co[active_fd]->buffer_in, g_ape->co[active_fd]->buffer_in.length - readb, g_ape);
									}
								} 
							}
						} while(readb >= 0);
					}
				}			
			}
		}
		
		gettimeofday(&t_end, NULL);
		
		ticks = 0;
		
		uticks = 1000000L * (t_end.tv_sec - t_start.tv_sec);
		uticks += (t_end.tv_usec - t_start.tv_usec);
		t_start = t_end;
		lticks += uticks;
		/* Tic tac, tic tac */

		while (lticks >= 1000) {
			lticks -= 1000;
			process_tick(g_ape);
		}		
	}

	return 0;
}

int sendf(int sock, acetables *g_ape, char *buf, ...)
{
	char *buff;
	int len;
	int finish;
	
	va_list val;
	
	va_start(val, buf);
	len = vasprintf(&buff, buf, val);
	va_end(val);

	finish = sendbin(sock, buff, len, 0, g_ape);

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
				/* Still not complete */
				memmove(bufout->buf, bufout->buf + t_bytes, r_bytes);
				/* TODO : avoid memmove */
				bufout->buflen = r_bytes;
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

/* TODO : add "nowrite" flag to avoid write syscall when calling several time sendbin() */
int sendbin(int sock, const char *bin, unsigned int len, unsigned int burn_after_writing, acetables *g_ape)
{
	int t_bytes = 0, r_bytes, n = 0;

	r_bytes = len;

	if (sock != 0) {
		while(t_bytes < len) {
			if (g_ape->bufout[sock].buf == NULL) {
				n = write(sock, bin + t_bytes, r_bytes);
			} else {
				n = -2;
			}
			if (n < 0) {
				if ((errno == EAGAIN && r_bytes > 0) || (n == -2)) {
					if (g_ape->bufout[sock].buf == NULL) {
						g_ape->bufout[sock].allocsize = r_bytes + 128; /* add padding to prevent extra data to be reallocated */
						g_ape->bufout[sock].buf = xmalloc(sizeof(char) * g_ape->bufout[sock].allocsize);
						g_ape->bufout[sock].buflen = r_bytes;
					} else {
						g_ape->bufout[sock].buflen += r_bytes;
						if (g_ape->bufout[sock].buflen > g_ape->bufout[sock].allocsize) {
							g_ape->bufout[sock].allocsize = g_ape->bufout[sock].buflen + 128;
							g_ape->bufout[sock].buf = xrealloc(g_ape->bufout[sock].buf, sizeof(char) * g_ape->bufout[sock].allocsize);
						}
					}
					
					memcpy(g_ape->bufout[sock].buf + (g_ape->bufout[sock].buflen - r_bytes), bin + t_bytes, r_bytes);
					
					if (burn_after_writing) {
						g_ape->co[sock]->burn_after_writing = 1;
					}
					
					return 0;
				}
				
				break;
			}
			t_bytes += n;
			r_bytes -= n;
		}
	}
	
	if (burn_after_writing) {
		shutdown(sock, 2);
	}
	
	return 1;
}

void safe_shutdown(int sock, acetables *g_ape)
{
	if (g_ape->bufout[sock].buf == NULL) {
		shutdown(sock, 2);
	} else {
		g_ape->co[sock]->burn_after_writing = 1;
	}
}
