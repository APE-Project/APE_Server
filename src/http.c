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

struct _http_header_line
{
	struct {
		char val[64];
		unsigned int len;
	} key;
	
	struct {
		char val[1024];
		unsigned int len;
	} value;
	
	struct _http_header_line *next;
};

static struct _http_header_line *parse_header_line(const char *line)
{
	unsigned int i;
	unsigned short int state = 0;
	struct _http_header_line *hline;
	for (i = 0; i < 1024 && line[i] != '\0' && line[i] != '\r' && line[i] != '\n'; i++) {
		if (i == 0) {
			hline = xmalloc(sizeof(*hline));
			hline->key.len = 0;
			hline->value.len = 0;
		}
		switch(state) {
			case 0:
				if ((i == 0 && (line[i] == ':' || line[i] == ' ')) || (line[i] == ':' && line[i+1] != ' ') || (i > 63)) {
					free(hline);
					return NULL;
				}
				if (line[i] == ':') {
					hline->key.val[hline->key.len] = '\0';
					state = 1;
					i++;
				} else {
					hline->key.val[hline->key.len++] = line[i];
				}
				break;
			case 1:
				hline->value.val[hline->value.len++] = line[i];
				break;
		}
	}
	if (!state) {
		free(hline);
		return NULL;
	}
	hline->value.val[hline->value.len] = '\0';

	return hline;
}

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
			} else {
				/* TODO */
				//parse_header_line(data);
			}
			http->pos += pos;
			process_http(buffer, http);
			break;
		case 2:
			read = buffer->length - http->pos; // data length

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

http_headers_response *http_headers_init(int code, char *detail, int detail_len)
{
	http_headers_response *headers;
	
	if (detail_len > 63 || (code < 100 && code >= 600)) {
		return NULL;
	}
	
	headers = xmalloc(sizeof(*headers));

	headers->code = code;
	headers->detail.len = detail_len;
	memcpy(headers->detail.val, detail, detail_len + 1);
	
	headers->fields = NULL;
	headers->last = NULL;
	
	return headers;
}

void http_headers_set_field(http_headers_response *headers, const char *key, int keylen, const char *value, int valuelen)
{
	struct _http_headers_fields *field = NULL, *look_field;
	int value_l, key_l;

	value_l = (valuelen ? valuelen : strlen(value));
	key_l = (keylen ? keylen : strlen(key));
	
	if (key_l >= 32) {
		return;
	}
	
	for(look_field = headers->fields; look_field != NULL; look_field = look_field->next) {
		if (strncasecmp(look_field->key.val, key, key_l) == 0) {
			field = look_field;
			break;
		}
	}
	
	if (field == NULL) {
		field = xmalloc(sizeof(*field));
		field->next = NULL;
	
		if (headers->fields == NULL) {
			headers->fields = field;
		} else {
			headers->last->next = field;
		}
		headers->last = field;
	} else {
		free(field->value.val);
	}
	
	field->value.val = xmalloc(sizeof(char) * (value_l + 1));
	
	memcpy(field->key.val, key, key_l + 1);
	memcpy(field->value.val, value, value_l + 1);
	
	field->value.len = value_l;
	field->key.len = key_l;

}

/*
http_headers_response *headers = http_headers_init(200, "OK", 2);
http_headers_set_field(headers, "Content-Length", 0, "100", 0);
http_send_headers(headers, cget->client, g_ape);
*/

int http_send_headers(http_headers_response *headers, const char *default_h, unsigned int default_len, ape_socket *client, acetables *g_ape)
{
	char code[4];
	int finish = 1;
	struct _http_headers_fields *fields;
	//HTTP/1.1 200 OK\r\n
	
	if (headers == NULL) {
		finish &= sendbin(client->fd, (char *)default_h, default_len, g_ape);
	} else {
		/* We have a lot of write syscall here. TODO : use of writev */
		itos(headers->code, code, 4);
		finish &= sendbin(client->fd, "HTTP/1.1 ", 9, g_ape);
		finish &= sendbin(client->fd, code, 3, g_ape);
		finish &= sendbin(client->fd, " ", 1, g_ape);
		finish &= sendbin(client->fd, headers->detail.val, headers->detail.len, g_ape);
		finish &= sendbin(client->fd, "\r\n", 2, g_ape);
	
		for (fields = headers->fields; fields != NULL; fields = fields->next) {
			finish &= sendbin(client->fd, fields->key.val, fields->key.len, g_ape);
			finish &= sendbin(client->fd, ": ", 2, g_ape);
			finish &= sendbin(client->fd, fields->value.val, fields->value.len, g_ape);
			finish &= sendbin(client->fd, "\r\n", 2, g_ape);
		
			fields = fields->next;
		}
	
		finish &= sendbin(client->fd, "\r\n", 2, g_ape);
	}
	
	return finish;
}

void http_headers_free(http_headers_response *headers)
{
	struct _http_headers_fields *fields;
	
	if (headers == NULL) {
		return;
	}
	
	fields = headers->fields;
	
	while(fields != NULL) {
		struct _http_headers_fields *tmpfields = fields->next;
		
		free(fields->value.val);
		
		free(fields);
		fields = tmpfields;
	}
	free(headers);
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
	ape_socket *pattern;
	
	struct _http_attach *ha = xmalloc(sizeof(*ha));
	
	if (parse_uri(url, ha->host, &ha->port, ha->file) == -1) {
		free(ha);
		return;
	}
	ha->post = post;
	
	pattern = xmalloc(sizeof(*pattern));
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