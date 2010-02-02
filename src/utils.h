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

/* utils.h */


#ifndef _UTILS_H
#define _UTILS_H

#include <stdlib.h>
#include <math.h>

void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);

int seof(char *buf, unsigned short int stop);
int sneof(char *buf, size_t len, size_t max);
long int itos(long int input, char *output, long int len);
char *trim(char *s);
char *removelast(char *input, unsigned int n);
size_t explode(const char split, char *input, char **tP, unsigned int limit);
char hex2int(unsigned char hex);
int urldecode(char *string);
int rand_n(int n);
void s_tolower(char *upper, unsigned int len);
char *get_path(const char *full_path);

/* CONST_STR_LEN from lighttpd */
#define CONST_STR_LEN(x) x, x ? sizeof(x) - 1 : 0

#define LENGTH_N(num) ((num<10 && num >= 0)?1:(long int)log10(fabs(num))+1);
#ifndef MAX
  #define MAX(a,b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
  #define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif

#endif
