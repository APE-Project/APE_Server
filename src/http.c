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

/* http.c */

#include <string.h>

#include "http.h"
#include "sock.h"
#include "main.h"
#include "utils.h"
#include "dns.h"

#define HTTP_PREFIX		"http://"

struct _http_attach {
	char host[1024];
	char file[1024];
	
	const char *post;
	u_short port;
};

/* Just a lightweight http request processor */

void process_http(ape_buffer *buffer, http_state *http)
{
	char *data = buffer->data;
	int pos, read;
	
	if (buffer->length == 0 || http->ready == 1 || http->error == 1) {
		return;
	}
	
	/* 0 will be erased by the next read()'ing loop */
	data[buffer->length] = '\0';
	
	data = &data[http->pos];
	
	if (*data == '\0') {
		return;
	}
	
	switch(http->step) {
		case 0:
			pos = seof(data);
			if (pos == -1) {
				return;
			}
			
			if (strncasecmp(data, "POST ", 5) == 0) {
				http->type = HTTP_POST;
			} else if (strncasecmp(data, "GET ", 4) == 0) {
				http->type = HTTP_GET;
			} else {
				/* Other methods are not implemented yet */
				http->error = 1;
				
				return;
			}
			http->pos = pos;
			http->step = 1;
			
			process_http(buffer, http);
			break;
		case 1:
			pos = seof(data);
			if (pos == -1) {

				return;
			}
			if (pos == 1 || (pos == 2 && *data == '\r')) {
				

				if (http->type == HTTP_GET) {
					/* Ok, at this point we have a blank line. Ready for GET */
					http->ready = 1;
					buffer->data[http->pos] = '\0';

					return;
				} else {
					/* Content-Length is mandatory in case of POST */
					if (http->contentlength == 0) {
						http->error = 1;
						
						return;
					} else {
						http->step = 2;
					}
				}
			} else if (http->type == HTTP_POST) {
				/* looking for content-length instruction */
				if (pos <= 25 && strncasecmp("content-length: ", data, 16) == 0) {
					int cl = atoi(&data[16]);
					
					/* Content-length can't be negative... */
					if (cl < 1 || cl > MAX_CONTENT_LENGTH) {
						http->error = 1;
						return;
					}
					/* At this time we are ready to read "cl" bytes contents */
					http->contentlength = cl;
					
				}
			}
			http->pos += pos;
			process_http(buffer, http);
			break;
		case 2:
			read = strlen(data);
			http->pos += read;
			http->read += read;

			if (http->read >= http->contentlength) {
				http->ready = 1;
				
				/* no more than content-length */
				buffer->data[http->pos - (http->read - http->contentlength)] = '\0';
			}
			break;
		default:
			break;
	}
}


/* taken from libevent */

int parse_uri(char *url, char *host, u_short *port, char *file)
{
	char *p;
	const char *p2;
	int len;

	len = strlen(HTTP_PREFIX);
	if (strncasecmp(url, HTTP_PREFIX, len)) {
		return -1;
	}

	url += len;

	/* We might overrun */
	strncpy(host, url, 1023);


	p = strchr(host, '/');
	if (p != NULL) {
		*p = '\0';
		p2 = p + 1;
	} else {
		p2 = NULL;
	}
	if (file != NULL) {
		/* Generate request file */
		if (p2 == NULL)
			p2 = "";
		sprintf(file, "/%s", p2);
	}

	p = strchr(host, ':');
	
	if (p != NULL) {
		*p = '\0';
		*port = atoi(p + 1);

		if (*port == 0)
			return -1;
	} else
		*port = 80;

	return 0;
}


static void ape_http_connect(ape_socket *client, acetables *g_ape)
{
	struct _http_attach *ha = client->attach;
	char *method = (ha->post != NULL ? "POST" : "GET");
	
	sendf(client->fd, g_ape, "%s %s HTTP/1.1\r\nHost: %s\r\n", method, ha->file, ha->host);
	
	if (ha->post != NULL) {
		int plen = strlen(ha->post);
		sendf(client->fd, g_ape, "Content-Type: application/x-www-form-urlencoded\r\n");
		sendf(client->fd, g_ape, "Content-Length: %i\r\n\r\n", plen);
		sendbin(client->fd, (char *)ha->post, plen, g_ape);
	} else {
		sendbin(client->fd, "\r\n", 2, g_ape);
	}
	printf("Data posted\n");
}

static void ape_http_disconnect(ape_socket *client, acetables *g_ape)
{
	free(client->attach);
}

/*static void ape_http_read()
{
	
}*/

void ape_http_request(char *url, const char *post, acetables *g_ape)
{
	ape_socket *pattern = xmalloc(sizeof(*pattern));
	
	struct _http_attach *ha = xmalloc(sizeof(*ha));
	
	if (parse_uri(url, ha->host, &ha->port, ha->file) == -1) {
		free(pattern);
		free(ha);
		return;
	}
	ha->post = post;
	
	pattern->callbacks.on_accept = NULL;
	pattern->callbacks.on_connect = ape_http_connect;
	pattern->callbacks.on_disconnect = ape_http_disconnect;
	pattern->callbacks.on_read = NULL;
	pattern->callbacks.on_read_lf = NULL;
	pattern->callbacks.on_data_completly_sent = NULL;
	pattern->callbacks.on_write = NULL;
	pattern->attach = (void *)ha;
	
	ape_connect_name(ha->host, ha->port, pattern, g_ape);
	
	
}