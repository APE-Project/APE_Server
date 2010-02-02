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
	unsigned short int port;
};

static struct _http_header_line *parse_header_line(const char *line)
{
	unsigned int i;
	unsigned short int state = 0;
	struct _http_header_line *hline = NULL;
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

char *get_header_line(struct _http_header_line *lines, const char *key)
{
	while (lines != NULL) {
		if (strcasecmp(lines->key.val, key) == 0) {
			return lines->value.val;
		}
		
		lines = lines->next;
	}
	
	return NULL;
}

void process_websocket(ape_socket *co, acetables *g_ape)
{
	char *pData;
	ape_buffer *buffer = &co->buffer_in;
	websocket_state *websocket = co->parser.data;
	ape_parser *parser = &co->parser;
	
	char *data = pData = &buffer->data[websocket->offset];
	
	if (buffer->length == 0 || parser->ready == 1) {
		return;
	}
	
	if (buffer->length > 502400) {
		shutdown(co->fd, 2);
		return;
	}

	data[buffer->length - websocket->offset] = '\0';
	
	if (*data == '\0') {
		data = &data[1];
	}

	while(data++ != &buffer->data[buffer->length]) {
	
		if ((unsigned char)*data == 0xFF) {
			*data = '\0';
			
			websocket->data = &pData[1];
			
			parser->onready(parser, g_ape);

			websocket->offset += (data - pData)+1;
			
			if (websocket->offset == buffer->length) {
				parser->ready = -1;
				buffer->length = 0;
				websocket->offset = 0;
				
				return;
			}
			
			break;
		}
	}
	
	if (websocket->offset != buffer->length && data != &buffer->data[buffer->length+1]) {
		process_websocket(co, g_ape);
	}
}

/* Just a lightweight http request processor */
void process_http(ape_socket *co, acetables *g_ape)
{
	ape_buffer *buffer = &co->buffer_in;
	http_state *http = co->parser.data;
	ape_parser *parser = &co->parser;
	
	char *data = buffer->data;
	int pos, read, p = 0;
	
	if (buffer->length == 0 || parser->ready == 1 || http->error == 1) {
		return;
	}

	/* 0 will be erased by the next read()'ing loop */
	data[buffer->length] = '\0';
	
	data = &data[http->pos];
	
	if (*data == '\0') {
		return;
	}
	
	/* Update the address of http->data and http->uri if buffer->data has changed (realloc) */
	if (http->buffer_addr != NULL && buffer->data != http->buffer_addr) {
		http->data = &buffer->data[(void *)http->data - (void *)http->buffer_addr];
		http->uri = &buffer->data[(void *)http->uri - (void *)http->buffer_addr];
		http->buffer_addr = buffer->data;
	}
	
	switch(http->step) {
		case 0:
			pos = seof(data, '\n');
			if (pos == -1) {
				return;
			}
			
			/* TODO : endieness */
			switch(*(unsigned int *)data) {
				case 542393671: /* GET + space */
					http->type = HTTP_GET;
					p = 4;
					break;
				case 1414745936: /* POST */
					http->type = HTTP_POST;
					p = 5;
					break;
				default:
					http->error = 1;
					shutdown(co->fd, 2);
					return;
			}
			
			if (data[p] != '/') {
				http->error = 1;
				shutdown(co->fd, 2);
				return;
			} else {
				int i = p;
				while (p++) {
					switch(data[p]) {
						case ' ':
							http->pos = pos;
							http->step = 1;
							http->uri = &data[i];
							http->buffer_addr = buffer->data;
							data[p] = '\0';
							process_http(co, g_ape);
							return;
						case '?':
							if (data[p+1] != ' ' && data[p+1] != '\r' && data[p+1] != '\n') {
								http->buffer_addr = buffer->data;
								http->data = &data[p+1];
							}
							break;
						case '\r':
						case '\n':
						case '\0':
							http->error = 1;
							shutdown(co->fd, 2);
							return;
					}
				}
			}
			break;
		case 1:
			pos = seof(data, '\n');
			if (pos == -1) {

				return;
			}
			if (pos == 1 || (pos == 2 && *data == '\r')) {
				if (http->type == HTTP_GET) {
					/* Ok, at this point we have a blank line. Ready for GET */
					buffer->data[http->pos] = '\0';
					urldecode(http->uri);
					
					parser->onready(parser, g_ape);
					parser->ready = -1;
					buffer->length = 0;
					return;
				} else {
					/* Content-Length is mandatory in case of POST */
					if (http->contentlength == 0) {
						http->error = 1;
						shutdown(co->fd, 2);
						return;
					} else {
						http->buffer_addr = buffer->data; // save the addr
						http->data = &buffer->data[http->pos+(pos)];
						http->step = 2;
					}
				}
			} else {
				struct _http_header_line *hl;

				if ((hl = parse_header_line(data)) != NULL) {
					hl->next = http->hlines;
					http->hlines = hl;
					if (strcasecmp(hl->key.val, "host") == 0) {
						http->host = hl->value.val;
					}
				}
				if (http->type == HTTP_POST) {
					/* looking for content-length instruction */
					if (pos <= 25 && strncasecmp("content-length: ", data, 16) == 0) {
						int cl = atoi(&data[16]);

						/* Content-length can't be negative... */
						if (cl < 1 || cl > MAX_CONTENT_LENGTH) {
							http->error = 1;
							shutdown(co->fd, 2);
							return;
						}
						/* At this time we are ready to read "cl" bytes contents */
						http->contentlength = cl;

					}
				}
			}
			http->pos += pos;
			process_http(co, g_ape);
			break;
		case 2:
			read = buffer->length - http->pos; // data length
			http->pos += read;
			http->read += read;
			
			if (http->read >= http->contentlength) {

				parser->ready = 1;
				urldecode(http->uri);
				/* no more than content-length */
				buffer->data[http->pos - (http->read - http->contentlength)] = '\0';
				
				parser->onready(parser, g_ape);
				parser->ready = -1;
				buffer->length = 0;
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
		finish &= sendbin(client->fd, (char *)default_h, default_len, 0, g_ape);
	} else {
		/* We have a lot of write syscall here. TODO : use of writev */
		itos(headers->code, code, 4);
		finish &= sendbin(client->fd, "HTTP/1.1 ", 9, 0, g_ape);
		finish &= sendbin(client->fd, code, 3, 0, g_ape);
		finish &= sendbin(client->fd, " ", 1, 0, g_ape);
		finish &= sendbin(client->fd, headers->detail.val, headers->detail.len, 0, g_ape);
		finish &= sendbin(client->fd, "\r\n", 2, 0, g_ape);
	
		for (fields = headers->fields; fields != NULL; fields = fields->next) {
			finish &= sendbin(client->fd, fields->key.val, fields->key.len, 0, g_ape);
			finish &= sendbin(client->fd, ": ", 2, 0, g_ape);
			finish &= sendbin(client->fd, fields->value.val, fields->value.len, 0, g_ape);
			finish &= sendbin(client->fd, "\r\n", 2, 0, g_ape);
		
			fields = fields->next;
		}
	
		finish &= sendbin(client->fd, "\r\n", 2, 0, g_ape);
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

void free_header_line(struct _http_header_line *line)
{
	struct _http_header_line *tline;
	
	while (line != NULL) {
		tline = line->next;
		free(line);
		line = tline;
	}
}

