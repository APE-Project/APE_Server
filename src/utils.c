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

/* utils.c */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "log.h"

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

void s_tolower(char *upper, unsigned int len)
{
	unsigned int i;
	
	for (i = 0; i < len; i++) {
		upper[i] = tolower(upper[i]);
	}
}

long int itos(long long int input, char *output, long int len)
{
	int sign = 0, i = 1;

	if (input < 0) {
		sign = 1;
		input = -input;
	}
	output[(len - i)] = '\0';
	
	for (i = 2; input != 0; i++) {	
		
		output[len - i] = '0' + (input % 10);
		
		input /= 10;
	}
	if (sign) {
		output[len - i++] = '-';
	}

	return len-(i-1);
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

int seof(char *buf, unsigned short int stop)
{
	char *pBuf;
	int pos = 0;
	
	for (pBuf = buf; pBuf[pos] != '\0'; pos++) {
		/*if (pos == 4096) {
			return -1;
		}*/
		if (pBuf[pos] == stop) {
			return pos+1;
		}
	}
	return -1;
}

int sneof(char *buf, size_t len, size_t max)
{
	char *pBuf;
	int pos = 0;
	
	for (pBuf = buf; pos < len && pos < max; pos++) {
		if (pBuf[pos] == '\n') {
			return pos+1;
		}
	}
	return -1;
}

int rand_n(int n)
{
    int partSize   = 1 + (n == RAND_MAX ? 0 : (RAND_MAX - n) / (n + 1));
    int maxUsefull = partSize * n + (partSize - 1);
    int draw;
    
    do {
        draw = rand();
    } while (draw > maxUsefull);
    
    return draw / partSize;
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

char *get_path(const char *full_path)
{
	char *new_path;
	char *last;
	new_path = xstrdup(full_path);
	
	last = strrchr(new_path, '/');
	if (last == NULL) {
		free(new_path);
		return NULL;
	}
	
	last[1] = '\0';
	
	return new_path;
}

char hex2int(unsigned char hex)
{
	hex = hex - '0';
	if (hex > 9) {
		hex = (hex + '0' - 1) | 0x20;
		hex = hex - 'a' + 11;
	}
	if (hex > 15) {
		hex = 0xFF;
	}

	return hex;
}

/* taken from lighttp */
int urldecode(char *string)
{
	unsigned char high, low;
	const char *src;
	char *dst;

	if (string == NULL || !string) return -1;

	src = (const char *) string;
	dst = (char *) string;

	while ((*src) != '\0') {
		if (*src == '%') {
			*dst = '%';

			high = hex2int(*(src + 1));
			if (high != 0xFF) {
				low = hex2int(*(src + 2));
				if (low != 0xFF) {
					high = (high << 4) | low;

					if (high < 32 || high == 127) high = '_';

					*dst = high;
					src += 2;
				}
			}
		} else {
			*dst = *src;
		}

		dst++;
		src++;
	}

	*dst = '\0';

	return 1;
}


