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

/* the order of theses headers and defines
 * is important */
#include <mysql/my_global.h>
#undef _ISOC99_SOURCE
#define _ISOC99_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "mysac.h"
#include "mysac_utils.h"




/**************************************************

   read data in binary type 

**************************************************/ 
int mysac_decode_binary_row(char *buf, int packet_len,
                            MYSAC_RES *res, MYSAC_ROWS *row) {
	int j;
	int i;
	char nul;
	unsigned long len;
	int tmp_len;
	char *wh;
	char _null_ptr[16];
	char *null_ptr;
	unsigned char bit;

	wh = buf;
	null_ptr = _null_ptr;
	bit = 4; /* first 2 bits are reserved */

	/* first bit is unused */
	i = 1;

	/* skip null bits */
	tmp_len = ( (res->nb_cols + 9) / 8 );
	if (i + tmp_len > packet_len)
		return -1;

	memcpy(_null_ptr, &buf[i], tmp_len);
	i += tmp_len;

	for (j = 0; j < res->nb_cols; j++) {

		/*
		   We should set both row_ptr and is_null to be able to see
		   nulls in mysql_stmt_fetch_column. This is because is_null may point
		   to user data which can be overwritten between mysql_stmt_fetch and
		   mysql_stmt_fetch_column, and in this case nullness of column will be
		   lost. See mysql_stmt_fetch_column for details.
		 */
		if ( (*null_ptr & bit) != 0 ) {
			/* do nothing */
		}

		else {
			switch (res->cols[j].type) {
	
			/* read null */
			case MYSQL_TYPE_NULL:
				row->data[j].blob = NULL;
	
			/* read blob */
			case MYSQL_TYPE_TINY_BLOB:
			case MYSQL_TYPE_MEDIUM_BLOB:
			case MYSQL_TYPE_LONG_BLOB:
			case MYSQL_TYPE_BLOB:
			/* decimal ? maybe for very big num ... crypto key ? */
			case MYSQL_TYPE_DECIMAL:
			case MYSQL_TYPE_NEWDECIMAL:
			/* .... */
			case MYSQL_TYPE_BIT:
			/* read text */
			case MYSQL_TYPE_STRING:
			case MYSQL_TYPE_VAR_STRING:
			case MYSQL_TYPE_VARCHAR:
			/* read date */
			case MYSQL_TYPE_NEWDATE:
				tmp_len = my_lcb(&buf[i], &len, &nul, packet_len-i);
				if (tmp_len == -1)
					return -1;
				i += tmp_len;
				if (i + len > packet_len)
					return -1;
				if (nul == 1)
					row->data[j].blob = NULL;
				else {
					memmove(wh, &buf[i], len);
					row->data[j].blob = wh;
					row->data[j].blob[len] = '\0';
					i += len;
					wh += len + 1;
				}
				row->lengths[j] = len;
				break;
	
			case MYSQL_TYPE_TINY:
				if (i > packet_len - 1)
					return -1;
				row->data[j].stiny = buf[i];
				i++;
				break;
	
			case MYSQL_TYPE_SHORT:
				if (i > packet_len - 2)
					return -1;
				row->data[j].ssmall = sint2korr(&buf[i]);
				i += 2;
				break;
	
			case MYSQL_TYPE_INT24:
			case MYSQL_TYPE_LONG:
				if (i > packet_len - 4)
					return -1;
				row->data[j].sint = sint4korr(&buf[i]);
				i += 4;
				break;
	
			case MYSQL_TYPE_LONGLONG:
				if (i > packet_len - 8)
					return -1;
				row->data[j].sbigint = sint8korr(&buf[i]);
				i += 8;
				break;
	
			case MYSQL_TYPE_FLOAT:
				if (i > packet_len - 4)
					return -1;
				float4get(row->data[j].mfloat, &buf[i]);
				i += 4;
				break;
	
			case MYSQL_TYPE_DOUBLE:
				if (i > packet_len - 8)
					return -1;
				float8get(row->data[j].mdouble, &buf[i]);
				i += 8;
				break;
	
			/* libmysql/libmysql.c:3370
			 * static void read_binary_time(MYSQL_TIME *tm, uchar **pos) */
			case MYSQL_TYPE_TIME:
				tmp_len = my_lcb(&buf[i], &len, &nul, packet_len-i);
				if (tmp_len == -1)
					return -1;
				i += tmp_len;
				if (i + len > packet_len)
					return -1;
				if (nul == 1)
					row->data[j].blob = NULL;
	
				if (len > 0) {
					row->data[j].tv.tv_sec = 
					              ( uint4korr(&buf[i+1]) * 86400 ) +
					              ( buf[i+5] * 3600 ) +
					              ( buf[i+6] * 60 ) +
					                buf[i+7];
					if (buf[i] != 0)
						row->data[j].tv.tv_sec = - row->data[j].tv.tv_sec;
					if (len > 8)
						row->data[j].tv.tv_usec = uint4korr(&buf[i+8]);
					else
						row->data[j].tv.tv_usec = 0;
				}
				i += len;
				break;
	
			case MYSQL_TYPE_YEAR:
				row->data[j].tm->tm_year = uint2korr(&buf[i]) - 1900;
				row->data[j].tm->tm_mday = 1;
				i += 2;
				break;
	
			/* libmysql/libmysql.c:3400
			 * static void read_binary_datetime(MYSQL_TIME *tm, uchar **pos) */
			case MYSQL_TYPE_TIMESTAMP:
			case MYSQL_TYPE_DATETIME:
				tmp_len = my_lcb(&buf[i], &len, &nul, packet_len-i);
				if (tmp_len == -1)
					return -1;
				i += tmp_len;
				if (i + len > packet_len)
					return -1;
				if (nul == 1)
					row->data[j].blob = NULL;
	
				row->data[j].tm->tm_year = uint2korr(&buf[i+0]) - 1900;
				row->data[j].tm->tm_mon  = buf[i+2] - 1;
				row->data[j].tm->tm_mday = buf[i+3];
				if (len > 4) {
					row->data[j].tm->tm_hour = buf[i+4];
					row->data[j].tm->tm_min  = buf[i+5];
					row->data[j].tm->tm_sec  = buf[i+6];
				}
				if (len > 7) {
					/* les microsecondes ... */
				}
				i += len;
				break;
	
			/* libmysql/libmysql.c:3430
			 * static void read_binary_date(MYSQL_TIME *tm, uchar **pos) */
			case MYSQL_TYPE_DATE:
				tmp_len = my_lcb(&buf[i], &len, &nul, packet_len-i);
				if (tmp_len == -1)
					return -1;
				i += tmp_len;
				if (i + len > packet_len)
					return -1;
				if (nul == 1)
					row->data[j].blob = NULL;
	
				row->data[j].tm->tm_year = uint2korr(&buf[i+0]) - 1900;
				row->data[j].tm->tm_mon  = buf[i+2] - 1;
				row->data[j].tm->tm_mday = buf[i+3];
				i += len;
				break;
	
			case MYSQL_TYPE_ENUM:
			case MYSQL_TYPE_SET:
			case MYSQL_TYPE_GEOMETRY:
				break;
			}
		}

		/* To next bit */
		bit <<= 1;

		/* To next byte */
		if ( (bit & 255) == 0 ) {
			bit = 1;
			null_ptr++;
		}
	}
	return wh - buf;
}

/**************************************************

   read data in string type 

**************************************************/ 
int mysac_decode_string_row(char *buf, int packet_len,
                            MYSAC_RES *res, MYSAC_ROWS *row) {
	int i, j;
	int tmp_len;
	unsigned long len;
	char nul;
	char *wh;
	char mem;
	char *error;

	i = 0;
	wh = buf;

	for (j = 0; j < res->nb_cols; j++) {

		tmp_len = my_lcb(&buf[i], &len, &nul, packet_len-i);
		if (tmp_len == -1)
			return -MYERR_BAD_LCB;

		i += tmp_len;

		if (i + len > packet_len)
			return -MYERR_LEN_OVER_BUFFER;

		if (nul == 1) {
			row->data[j].blob = NULL;
			continue;
		}

		memmove(wh, &buf[i], len);
		row->data[j].blob = wh;
		row->data[j].blob[len] = '\0';
		wh += len + 1;
		row->lengths[j] = len;

		/* next packet */
		i += len;
	}

	return wh - buf;
}

