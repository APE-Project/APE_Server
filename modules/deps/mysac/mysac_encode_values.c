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
//#undef _ISOC99_SOURCE
//#define _ISOC99_SOURCE
//#include <stdlib.h>
//#include <stdio.h>
//#include <stdint.h>
//#include <string.h>
//#include <stdarg.h>
//#include <time.h>

#include "mysac.h"
#include "mysac_utils.h"




/**************************************************

   read data in binary type 

**************************************************/ 
int mysac_encode_value(MYSAC_BIND *val, char *out, int len) {
	/*
	int j;
	int i;
	char nul;
	unsigned long len;
	int tmp_len;
	char *wh;
	char _null_ptr[16];
	char *null_ptr;
	unsigned char bit;
	*/
	int l;
//	int data_len;
	struct timeval *tv;
	struct tm *tm;
	unsigned int v;
	char sign;

	switch (val->type) {
	
	/* read null */
	case MYSQL_TYPE_NULL:
		l = 0;
		break;
	
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

		l = set_my_lcb(val->value_len, 0, out, len);
		if (l < 0)
			return -1;

		len -= l;

		if (len < val->value_len)
			return -1;

		memcpy(&out[l], val->value, val->value_len);
		l += val->value_len;
		break;
	
	case MYSQL_TYPE_TINY:
		if (len < 1)
			return -1;
		l = 1;
		out[0] = *(int *)val->value;
		break;
	
	case MYSQL_TYPE_SHORT:
		if (len < 2)
			return -1;
		l = 2;
		to_my_2(*(int *)val->value, out);
		break;
	
	case MYSQL_TYPE_INT24:
	case MYSQL_TYPE_LONG:
		if (len < 4)
			return -1;
		l = 4;
		to_my_4(*(int *)val->value, out);
		break;
	
	case MYSQL_TYPE_LONGLONG:
		if (len < 8)
			return -1;
		l = 8;
		to_my_8(*(long long int *)val->value, out);
		break;
	
	case MYSQL_TYPE_FLOAT:
		if (len < 4)
			return -1;
		l = 4;
		/* TODO: cast a revoir */
		float4store((*(int *)val->value), out);
		break;
	
	case MYSQL_TYPE_DOUBLE:
		if (len < 8)
			return -1;
		l = 8;
		/* TODO: cast a revoir */
		float8store(*(int *)val->value, out);
		break;
	
	/* libmysql/libmysql.c:3370
	 * static void read_binary_time(MYSQL_TIME *tm, uchar **pos) 
	 *
	 * n*lcb 4*j 1*h 1*m 1*s 1*sig 4*usec soit 12 bytes
	 */
	case MYSQL_TYPE_TIME:

		l = set_my_lcb(12, 0, out, len);
		if (l < 0)
			return -1;

		len -= l;

		if (len < 12)
			return -1;

		tv = (struct timeval *)val->value;
		if (tv->tv_sec < 0) {
			tv->tv_sec = - tv->tv_sec;
			sign = 1;
		}
		else
			sign = 0;

		/* nb days */
		to_my_4(tv->tv_sec / 86400, &out[l]);
		l += 4;

		/* remainder in secs */
		v = tv->tv_sec % 86400;

		/* nb hours */
		out[l] = v / 3600;
		l++;

		/* remainder in secs */
		v = v % 3600;

		/* nb mins */
		out[l] = v / 60;
		l++;

		/* secs */
		out[l] = v % 60;
		l++;

		/* signe */
		out[l] = sign;
		l++;

		/* u secs */
		to_my_4(tv->tv_usec, &out[l]);
		l += 4;

		break;
	
	case MYSQL_TYPE_YEAR:
		if (len < 2)
			return -1;

		tm = (struct tm *)val->value;
		to_my_2(tm->tm_year + 1900, out);
		l = 2;
		break;
	
	/* libmysql/libmysql.c:3400
	 * static void read_binary_datetime(MYSQL_TIME *tm, uchar **pos) 
	 *
	 * 1*lcb 2*year 1*mon 1*day 1*hour 1*min 1*sec
	 */
	case MYSQL_TYPE_TIMESTAMP:
	case MYSQL_TYPE_DATETIME:
		
		l = set_my_lcb(7, 0, out, len);
		if (l < 0)
			return -1;

		len -= l;

		if (len < 7)
			return -1;

		tm = (struct tm *)val->value;

		to_my_2(tm->tm_year + 1900, &out[l]);
		l += 2;

		out[l] = tm->tm_mon + 1;
		l++;

		out[l] = tm->tm_mday;
		l++;

		out[l] = tm->tm_hour;
		l++;

		out[l] = tm->tm_min;
		l++;

		out[l] = tm->tm_sec;
		l++;

		break;
	
	/* libmysql/libmysql.c:3430
	 * static void read_binary_date(MYSQL_TIME *tm, uchar **pos)
	 *
	 * 1*lcb 2*year 1*mon 1*day
	 */
	case MYSQL_TYPE_DATE:
		
		l = set_my_lcb(4, 0, out, len);
		if (l < 0)
			return -1;

		len -= l;

		if (len < 4)
			return -1;

		tm = (struct tm *)val->value;

		to_my_2(tm->tm_year + 1900, &out[l]);
		l += 2;

		out[l] = tm->tm_mon + 1;
		l++;

		out[l] = tm->tm_mday;
		l++;

		break;
	
	case MYSQL_TYPE_ENUM:
	case MYSQL_TYPE_SET:
	case MYSQL_TYPE_GEOMETRY:
		/* TODO: a faire */
		break;
	}

	return l;
}

