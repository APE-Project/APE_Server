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

/* http.h */

#ifndef _HTTP_H
#define _HTTP_H

#include "main.h"

#define MAX_CONTENT_LENGTH 51200 // 50kb

struct _http_headers_fields
{
	struct {
		char val[32];
		int len;
	} key;
	
	struct {
		char *val;
		int len;
	} value;
	
	struct _http_headers_fields *next;
};

typedef struct _http_headers_response http_headers_response;
struct _http_headers_response
{
	int code;
	struct {
		char val[64];
		int len;
	} detail;
	
	struct _http_headers_fields *fields;
	struct _http_headers_fields *last;
};

typedef enum {
	HTTP_NULL = 0,
	HTTP_GET,
	HTTP_POST,
	HTTP_OPTIONS
} http_method;


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

void process_websocket(ape_socket *co, acetables *g_ape);
void process_http(ape_socket *co, acetables *g_ape);
http_headers_response *http_headers_init(int code, char *detail, int detail_len);
void http_headers_set_field(http_headers_response *headers, const char *key, int keylen, const char *value, int valuelen);
int http_send_headers(http_headers_response *headers, const char *default_h, unsigned int default_len, ape_socket *client, acetables *g_ape);
void http_headers_free(http_headers_response *headers);
void free_header_line(struct _http_header_line *line);
char *get_header_line(struct _http_header_line *lines, const char *key);

#endif

