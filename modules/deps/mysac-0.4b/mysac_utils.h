/*
 * Copyright (c) 2009 Thierry FOURNIER
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License.
 *
 */

#ifndef __MYSAC_UTILS_H__
#define __MYSAC_UTILS_H__

#include <stdint.h>

#include "mysac.h"

/* definitions imported from linux-2.6.24/include/linux/list.h */

static inline void INIT_LIST_HEAD(struct mysac_list_head *list) {
	list->next = list;
	list->prev = list;
}

/*
 * Insert a new entry between two known consecutive entries.
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static inline void __list_add(struct mysac_list_head *new,
                              struct mysac_list_head *prev,
                              struct mysac_list_head *next) {
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

/**
 * list_add_tail - add a new entry
 * @new: new entry to be added
 * @head: list head to add it before
 *
 * Insert a new entry before the specified head.
 * This is useful for implementing queues.
 */
static inline void list_add_tail(struct mysac_list_head *new,
                                 struct mysac_list_head *head) {
	__list_add(new, head->prev, head);
}

/*
 * Delete a list entry by making the prev/next entries
 * point to each other.
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static inline void __list_del(struct mysac_list_head * prev,
                              struct mysac_list_head * next) {
	next->prev = prev;
	prev->next = next;
}

/**
 * list_del - deletes entry from list.
 * @entry: the element to delete from the list.
 * Note: list_empty() on entry does not return true after this, the entry is
 * in an undefined state.
 */
static inline void list_del(struct mysac_list_head *entry) {
	__list_del(entry->prev, entry->next);
}



static inline void to_my_2(int value, char *m) {
	m[1] = value >> 8;
	m[0] = value;
}
static inline void to_my_3(int value, char *m) {
	m[2] = value >> 16;
	m[1] = value >> 8;
	m[0] = value;
}
static inline void to_my_4(int value, char *m) {
	m[3] = value >> 24;
	m[2] = value >> 16;
	m[1] = value >> 8;
	m[0] = value;
}

/* length coded binary
  0-250        0           = value of first byte
  251          0           column value = NULL
	                        only appropriate in a Row Data Packet
  252          2           = value of following 16-bit word
  253          3           = value of following 24-bit word
  254          8           = value of following 64-bit word

  fichier mysql: source mysql: sql/pack.c
*/
static inline int my_lcb_ll(char *m, unsigned long long *r,  char *nul, int len) {
	if (len < 1)
		return -1;
	switch ((unsigned char)m[0]) {

	case 251: /* fb : 1 octet */
		*r = 0;
		*nul=1;
		return 1;

	case 252: /* fc : 2 octets */
		if (len < 3)
			return -1;
		*r = uint2korr(&m[1]);
		*nul=0;
		return 3;

	case 253: /* fd : 3 octets */
		if (len < 4)
			return -1;
		*r = uint3korr(&m[1]);
		*nul=0;
		return 4;

	case 254: /* fe */
		if (len < 9)
			return -1;
		*r = uint8korr(&m[1]);
		*nul=0;
		return 9;

	default:
		*r = (unsigned char)m[0];
		*nul=0;
		return 1;
	}
}

static inline int my_lcb(char *m, unsigned long *r,  char *nul, int len) {
	unsigned long long val;
	int retcode;
	retcode = my_lcb_ll(m, &val, nul, len);
	*r = val;
	return retcode;
}
#endif
