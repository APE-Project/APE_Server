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

/* ticks.h */

#ifndef _TICKS_H
#define _TICKS_H

#include "main.h"

/* Ticks/secondes */
#define VTICKS_RATE 50 // 50 ms

struct _ticks_callback
{
	int ticks_need;
	int ticks_left;
	int times;
	unsigned int identifier;
	unsigned int protect;

	void *func;
	void *params;
	
	struct _ticks_callback *prev;
	struct _ticks_callback *next;
};

void process_tick(acetables *g_ape);
struct _ticks_callback *add_timeout(unsigned int msec, void *callback, void *params, acetables *g_ape);
struct _ticks_callback *add_periodical(unsigned int msec, int times, void *callback, void *params, acetables *g_ape);
void del_timer(struct _ticks_callback *timer, acetables *g_ape);
void del_timer_identifier(unsigned int identifier, acetables *g_ape);
struct _ticks_callback *get_timer_identifier(unsigned int identifier, acetables *g_ape);

#define add_ticked(x, y) add_periodical(VTICKS_RATE, 0, x, y, g_ape)

#endif

