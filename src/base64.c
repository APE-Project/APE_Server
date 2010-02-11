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

/* base64.c */
/* taken from ffmpeg */

#include <stdint.h>
#include "utils.h"

static uint8_t map2[] =
{
     0x3e, 0xff, 0xff, 0xff, 0x3f, 0x34, 0x35, 0x36,
     0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x01,
     0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
     0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
     0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x1a, 0x1b,
     0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23,
     0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b,
     0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33
 };
 
int base64_decode(unsigned char* out, const char *in, int out_length)
{
     int i, v;
     unsigned char *dst = out;
 
     v = 0;
     for (i = 0; in[i] && in[i] != '='; i++) {
         unsigned int index= in[i]-43;
         if (index>=(sizeof(map2)/sizeof(map2[0])) || map2[index] == 0xff)
             return -1;
         v = (v << 6) + map2[index];
         if (i & 3) {
             if (dst - out < out_length) {
                 *dst++ = v >> (6 - 2 * (i & 3));
             }
         }
     }

     return (dst - out);
}

char *base64_encode(unsigned char * src, int len)
{
	static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char *ret, *dst;
	unsigned i_bits = 0;
	int i_shift = 0;
	int bytes_remaining = len;
	
	ret = dst = xmalloc(len * 4 / 3 + 12);

	if (len) {
		while (bytes_remaining) {
			i_bits = (i_bits << 8) + *src++;
			bytes_remaining--;
			i_shift += 8;
			
			do {
				*dst++ = b64[(i_bits << 6 >> i_shift) & 0x3f];
				i_shift -= 6;
			} while (i_shift > 6 || (bytes_remaining == 0 && i_shift > 0));
	}
	while ((dst - ret) & 3)
		*dst++ = '=';
	}
	*dst = '\0';
	return ret;
}

