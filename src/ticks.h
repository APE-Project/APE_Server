/*
  Copyright (C) 2006, 2007, 2008, 2009  Anthony Catel <a.catel@weelya.com>

  This file is part of ACE Server.
  ACE is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  ACE is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ACE ; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

/* ticks.h */

#ifndef _TICKS
#define _TICKS

#include "main.h"

/* Ticks/secondes */
#define TICKS_RATE 20 // ~100ms

struct _ticks_callback
{
	int ticks_need;
	int ticks_left;
	
	int times;

	
	void *func;
	void *params;
	
	struct _ticks_callback *next;
};

void process_tick(acetables *g_ape);
struct _ticks_callback *add_timeout(unsigned int sec, void *callback, void *params, acetables *g_ape);
struct _ticks_callback *add_periodical(unsigned int sec, int times, void *callback, void *params, acetables *g_ape);
void del_timer(struct _ticks_callback **timer);

#define add_ticked(x, y) add_periodical(0, 0, x, y, g_ape)

#endif

