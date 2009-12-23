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

/* parser.c */

#include "parser.h"
#include "http.h"
#include "utils.h"


static void parser_destroy_http(ape_parser *http_parser)
{
	free_header_line(((http_state *)http_parser->data)->hlines);
	free(http_parser->data);
	http_parser->data = NULL;
	http_parser->ready = 0;
	http_parser->parser_func = NULL;
	http_parser->destroy = NULL;
	http_parser->socket = NULL;
}

ape_parser parser_init_http(ape_socket *co)
{
	ape_parser http_parser;
	http_state *http;
	
	http_parser.ready = 0;
	http_parser.data = xmalloc(sizeof(struct _http_state));
	
	http = http_parser.data;
	
	http->hlines = NULL;
	http->pos = 0;
	http->contentlength = -1;
	http->read = 0;
	http->step = 0;
	http->type = HTTP_NULL;
	http->error = 0;
	http->uri = NULL;
	http->data = NULL;
	http->host = NULL;

	http_parser.parser_func = process_http;
	http_parser.destroy = parser_destroy_http;
	http_parser.socket = co;
	
	return http_parser;
}

static void parser_destroy_stream(ape_parser *stream_parser)
{
	stream_parser->data = NULL;
	stream_parser->ready = 0;
	stream_parser->parser_func = NULL;
	stream_parser->destroy = NULL;
	stream_parser->socket = NULL;	
}

ape_parser parser_init_stream(ape_socket *co)
{
	ape_parser stream_parser;
	
	stream_parser.ready = 0;
	stream_parser.data = NULL;
	
	stream_parser.parser_func = NULL;
	stream_parser.destroy = parser_destroy_stream;
	stream_parser.socket = co;
	
	return stream_parser;	
}

void parser_destroy(ape_parser *parser)
{
	if (parser != NULL && parser->destroy != NULL) {
		parser->destroy(parser);
	}
}

void parser_run(ape_parser *parser)
{
	parser->parser_func(parser->socket);
}
