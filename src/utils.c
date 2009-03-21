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

/* utils.c */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void *xmalloc(size_t size)
{
	void *r = malloc(size);
	if (r == NULL) {
		printf("[ERR] Not enougth memory\n");
		exit(0);
	}
	return r;
}
void *xrealloc(void *ptr, size_t size)
{
	void *r = realloc(ptr, size);
	if (r == NULL) {
		printf("[ERR] Not enougth memory\n");
		exit(0);
	}
	return r;
}

char *itos(int input, char *output)
{
	int i = 1;
	
	output[sizeof(output) - i] = '\0';
	
	for (i = 2; input != 0; i++) {	
		
		output[sizeof(output) - i] = '0' + (input % 10);
		
		input /= 10;
	}
	return &output[sizeof(output)-(i-1)];
}

/* Taken from a random source */
char *trim(char *s)
{
	int i = 0, j;
	
	while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') {
		i++;
	}
	if (i > 0) {
		for(j = 0; j < strlen(s); j++) {
			s[j] = s[j+i];
		}
		s[j] = '\0';
	}

	i = strlen(s) - 1;
	
	while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') {
		i--;
	}
	if (i < (strlen(s) - 1)) {
		s[i+1] = '\0';
	}
	return s;
}

char *removelast(char *input, unsigned int n)
{
	if ((strlen(input)-n) < 1) {
		return NULL;
	}
	input[strlen(input)-n] = '\0';
	return input;
}

int seof(char *buf)
{
	char *pBuf;
	int pos = 0;
	
	for (pBuf = buf; pBuf[pos] != '\0'; pos++) {
		if (pos == 4096) {
			return -1;
		}
		if (pBuf[pos] == '\n') {
			return pos+1;
		}
	}
	return -1;
}

size_t explode(const char split, char *input, char **tP, unsigned int limit) // Explode a string in an array.
{
	size_t i = 0;
	
	tP[0] = input;
	for (i = 0; *input; input++) {
		if (*input == split) {
			i++;
			*input = '\0';
			if(*(input + 1) != '\0' && *(input + 1) != split) {
				tP[i] = input + 1;
			} else {
				i--;
			}
		}
		if ((i+1) == limit) {
			return i;
		}
	}
	
	return i;
}


char *xstrdup(const char *s)
{
	char *x = strdup(s);
	if (x == NULL) {
		printf("[ERR] Not enougth memory\n");
		exit(0);	
	}
	return x;
}


