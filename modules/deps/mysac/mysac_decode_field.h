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

/** @file */ 

#ifndef __MYSAC_DECODE_PAQUET__
#define __MYSAC_DECODE_PAQUET__

#include "mysac.h"

/** 
 * mysac_decode_field decode field header packet
 *
 * @param buf id the buffer containing packet
 *        the strings stored into col, are allocated into
 *        this buffer
 * @param len is then length of the packet
 * @param col is valid col struct space for storing pointers
 *        and values
 *
 * @return the len of the buffer used for storing data or
 *         -1 if the packet is corrupted
 */
int mysac_decode_field(char *buf, int len, MYSQL_FIELD *col);

#endif
