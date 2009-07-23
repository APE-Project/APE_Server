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

#include "sock.h"
#include "users.h"

#define MAX_CONTENT_LENGTH 51200 // 50kb

typedef struct _http_state http_state;

struct _http_state
{
	int step;
	int type; /* HTTP_GET or HTTP_POST */
	int pos;
	int contentlength;
	int read;
	int error;
	int ready;
};

typedef struct _connection connection;

enum {
	STREAM_IN,
	STREAM_OUT	
};

struct _connection {
	char ip_client[16];
	
	http_state http;
	
	int stream_type;
	long int idle;
	
	struct {
		char *data;	
		unsigned int size;
		unsigned int length;
		
	} buffer;
	
	void *attach;
};

enum {
	HTTP_NULL = 0,
	HTTP_GET,
	HTTP_POST,
	HTTP_OPTIONS
};
void process_http(struct _connection *co);

#endif

