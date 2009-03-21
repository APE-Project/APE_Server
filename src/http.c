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

#include "http.h"
#include "sock.h"
#include "main.h"
#include "utils.h"

/* Just a lightweight http request processor */

void process_http(connection *co)
{
	char *data = data = co->buffer.data;
	int pos, read;
	
	if (co->buffer.length == 0 || co->http.ready == 1 || co->http.error == 1) {
		return;
	}
	
	/* 0 will be erased by the next read()'ing loop */
	data[co->buffer.length] = '\0';
	
	data = &data[co->http.pos];
	
	if (*data == '\0') {
		return;
	}
	
	switch(co->http.step) {
		case 0:
			pos = seof(data);
			if (pos == -1) {
				return;
			}
			
			if (strncasecmp(data, "POST ", 5) == 0) {
				co->http.type = HTTP_POST;
			} else if (strncasecmp(data, "GET ", 4) == 0) {
				co->http.type = HTTP_GET;
			} else {
				/* Other methods are not implemented yet */
				co->http.error = 1;
				
				return;
			}
			co->http.pos = pos;
			co->http.step = 1;
			
			process_http(co);
			break;
		case 1:
			pos = seof(data);
			if (pos == -1) {

				return;
			}
			if (pos == 1 || (pos == 2 && data[0] == '\r')) {
				

				if (co->http.type == HTTP_GET) {
					/* Ok, at this point we have a blank line. Ready for GET */
					co->http.ready = 1;
					co->buffer.data[co->http.pos] = '\0';

					return;
				} else {
					/* Content-Length is mandatory in case of POST */
					if (co->http.contentlength == 0) {
						co->http.error = 1;
						
						return;
					} else {
						co->http.step = 2;
					}
				}
			} else if (co->http.type == HTTP_POST) {
				/* looking for content-length instruction */
				if (pos <= 25 && strncasecmp("content-length: ", data, 16) == 0) {
					int cl = atoi(&data[16]);
					
					/* Content-length can't be negative... */
					if (cl < 1 || cl > MAX_CONTENT_LENGTH) {
						co->http.error = 1;
						return;
					}
					/* At this time we are ready to read "cl" bytes contents */
					co->http.contentlength = cl;
					
				}
			}
			co->http.pos += pos;
			process_http(co);
			break;
		case 2:
			read = strlen(data);
			co->http.pos += read;
			co->http.read += read;

			if (co->http.read >= co->http.contentlength) {
				co->http.ready = 1;
				
				/* no more than content-length */
				co->buffer.data[co->http.pos - (co->http.read - co->http.contentlength)] = '\0';
			}
			break;
		default:
			break;
	}
}

