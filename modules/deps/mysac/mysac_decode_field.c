/*
 * Copyright (c) 2009 Thierry FOURNIER
 *
 * This file is part of MySAC.
 *
 * MySAC is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License
 *
 * MySAC is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MySAC.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <mysql/mysql.h>
#include <mysql/my_global.h>

#include "mysac_utils.h"
#include "mysac.h"

int mysac_decode_field(char *buf, int len, MYSQL_FIELD *col) {
	int i;
	unsigned long size;
	char nul;
	char *wh;
	int tmp_len;

	/*
	VERSION 4.0
	 Bytes                      Name
	 -----                      ----
	 n (Length Coded String)    table
	 n (Length Coded String)    name
	 4 (Length Coded Binary)    length
	 2 (Length Coded Binary)    type
	 2 (Length Coded Binary)    flags
	 1                          decimals
	 n (Length Coded Binary)    default
	 
	 -> VERSION 4.1
	 Bytes                      Name
	 -----                      ----
	 n (Length Coded String)    catalog
	 n (Length Coded String)    db
	 n (Length Coded String)    table
	 n (Length Coded String)    org_table
	 n (Length Coded String)    name
	 n (Length Coded String)    org_name
	 1                          (filler)
	 2                          charsetnr
	 4                          length
	 1                          type
	 2                          flags
	 1                          decimals
	 2                          (filler), always 0x00
	 n (Length Coded Binary)    default
	*/

	wh = buf;

	i = 0;

	/* n (Length Coded String)   catalog */
	tmp_len = my_lcb(&buf[i], &size, &nul, len-i);
	if (tmp_len == -1)
		return -MYERR_BAD_LCB;
	i += tmp_len;
	if (i + size > len)
		return -MYERR_LEN_OVER_BUFFER;
	col->catalog_length = size;
	memmove(wh, &buf[i], size);
	col->catalog = wh;
	col->catalog[size] = '\0';
	wh += size + 1;
	i += size;

	/* n (Length Coded String)    db */
	tmp_len = my_lcb(&buf[i], &size, &nul, len-i);
	if (tmp_len == -1)
		return -MYERR_BAD_LCB;
	i += tmp_len;
	if (i + size > len)
		return -MYERR_LEN_OVER_BUFFER;
	col->db_length = size;
	memmove(wh, &buf[i], size);
	col->db = wh;
	col->db[size] = '\0';
	wh += size + 1;
	i += size;

	/* n (Length Coded String)    table */
	tmp_len = my_lcb(&buf[i], &size, &nul, len-i);
	if (tmp_len == -1)
		return -MYERR_BAD_LCB;
	i += tmp_len;
	if (i + size > len)
		return -MYERR_LEN_OVER_BUFFER;
	col->table_length = size;
	memmove(wh, &buf[i], size);
	col->table = wh;
	col->table[size] = '\0';
	wh += size + 1;
	i += size;

	/* n (Length Coded String)    org_table */
	tmp_len = my_lcb(&buf[i], &size, &nul, len-i);
	if (tmp_len == -1)
		return -MYERR_BAD_LCB;
	i += tmp_len;
	if (i + size > len)
		return -MYERR_LEN_OVER_BUFFER;
	col->org_table_length = size;
	memmove(wh, &buf[i], size);
	col->org_table = wh;
	col->org_table[size] = '\0';
	wh += size + 1;
	i += size;

	/* n (Length Coded String)    name */
	tmp_len = my_lcb(&buf[i], &size, &nul, len-i);
	if (tmp_len == -1)
		return -MYERR_BAD_LCB;
	i += tmp_len;
	if (i + size > len)
		return -MYERR_LEN_OVER_BUFFER;
	col->name_length = size;
	memmove(wh, &buf[i], size);
	col->name = wh;
	col->name[size] = '\0';
	wh += size + 1;
	i += size;

	/* n (Length Coded String)    org_name */
	tmp_len = my_lcb(&buf[i], &size, &nul, len-i);
	if (tmp_len == -1)
		return -MYERR_BAD_LCB;
	i += tmp_len;
	if (i + size > len)
		return -MYERR_LEN_OVER_BUFFER;
	col->org_name_length = size;
	memmove(wh, &buf[i], size);
	col->org_name = wh;
	col->org_name[size] = '\0';
	wh += size + 1;
	i += size;

	/* check len */
	if (i + 13 > len)
		return -MYERR_LEN_OVER_BUFFER;

	/* (filler) */
	i += 1;

	/* charset */
	col->charsetnr = uint2korr(&buf[i]);
	i += 2;

	/* length */
	col->length = uint4korr(&buf[i]);
	i += 4;

	/* type */
	col->type = (unsigned char)buf[i];
	i += 1;

	/* flags */
	col->flags = uint3korr(&buf[i]);
	i += 2;

	/* decimals */
	col->decimals = buf[i];
	i += 1;

	/* filler */
	i += 2;

	/* default - a priori facultatif */
	if (len-i > 0) {
		tmp_len = my_lcb(&buf[i], &size, &nul, len-i);
		if (tmp_len == -1)
			return -MYERR_BAD_LCB;
		i += tmp_len;
		if (i + size > len)
			return -MYERR_LEN_OVER_BUFFER;
		col->def_length = size;
		memmove(wh, &buf[i], size);
		col->def = wh;
		col->def[size] = '\0';
		wh += size + 1;
		i += size;
	}
	else {
		col->def = NULL;
		col->def_length = 0;
	}
		

	/* set write pointer */
	return wh - buf;
}
