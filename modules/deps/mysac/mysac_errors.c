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

#include "mysac.h"

const char *mysac_errors[]  = {
	[0]                      = "no error",
	[MYERR_PROTOCOL_ERROR]   = "mysql protocol error",
	[MYERR_BUFFER_OVERSIZE]  = "buffer oversize",
	[MYERR_PACKET_CORRUPT]   = "packet corrupted",
	[MYERR_WANT_READ]        = "mysac need to read data on socket",
	[MYERR_WANT_WRITE]       = "mysac need to write data on socket",
	[MYERR_UNKNOWN_ERROR]    = "unknown error",
	[MYERR_MYSQL_ERROR]      = "mysql server return an error",
	[MYERR_SERVER_LOST]      = "server network connexion is break",
	[MYERR_BAD_PORT]         = "bad port number",
	[MYERR_BAD_STATE]        = "unexpected internal error: bad state",
	[MYERR_RESOLV_HOST]      = "can not resolve host name",
	[MYERR_SYSTEM]           = "system error (see errno)",
	[MYERR_CANT_CONNECT]     = "can not connect to host",
	[MYERR_BUFFER_TOO_SMALL] = "the buffer can not contain request",
	[MYERR_UNEXPECT_R_STATE] = "Unexpected state when reading data",
	[MYERR_STRFIELD_CORRUPT] = "Mysql string mode field corrupt",
	[MYERR_BINFIELD_CORRUPT] = "Mysql binary mode field corrupt",
	[MYERR_BAD_LCB]          = "Mysql protocol bad length coded binary",
	[MYERR_LEN_OVER_BUFFER]  = "Mysql protocol give len over the packet size",
	[MYERR_CONVLONG]         = "Error in string to long int type conversion",
	[MYERR_CONVLONGLONG]     = "Error in string to long long int type conversion",
	[MYERR_CONVFLOAT]        = "Error in string to float type conversion",
	[MYERR_CONVDOUBLE]       = "Error in string to double type conversion",
	[MYERR_CONVTIME]         = "Error in time string to time conversion",
	[MYERR_CONVTIMESTAMP]    = "Error in timestamp string to time conversion",
	[MYERR_CONVDATE]         = "Error in date string to time conversion"
};

