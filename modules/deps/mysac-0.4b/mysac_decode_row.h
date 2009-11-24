/*
 * Copyright (c) 2009 Thierry FOURNIER
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License.
 *
 */

/** @file */ 

#ifndef __MYSAC_DECODE_ROW_H__
#define __MYSAC_DECODE_ROW_H__

#include "mysac.h"

/**
 * This decode mysql row binary format packet 
 *
 * @param buf is the buffer containing packet the strings stored into col, are
 *        allocated into this buffer
 * @param len is the length of the packet
 * @param res is valid MYSAC_RES
 * @param row is valid col struct space for storing pointers and values
 *
 * @return the len of the buffer used for storing data or
 *         -1 if the packet is corrupted
 */
int mysac_decode_binary_row(char *buf, int len, MYSAC_RES *res, MYSAC_ROWS *row);

/**
 * This decode mysql row string format packet 
 *
 * @param buf is the buffer containing packet the strings stored into col, are
 *        allocated into this buffer
 * @param len is the length of the packet
 * @param res is valid MYSAC_RES
 * @param row is valid col struct space for storing pointers and values
 * 
 * @return the len of the buffer used for storing data or
 *         -1 if the packet is corrupted
 */
int mysac_decode_string_row(char *buf, int len, MYSAC_RES *res, MYSAC_ROWS *row);

#endif
