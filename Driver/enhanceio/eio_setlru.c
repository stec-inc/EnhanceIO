/*
 *  eio_setlru.c
 *
 *  Copyright (C) 2012 STEC, Inc. All rights not specifically granted
 *   under a license included herein are reserved
 *  Amit Kale <akale@stec-inc.com>
 *  Harish Pujari <hpujari@stec-inc.com>
 *   Generic lru implementation used mainly for cache sets.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; under version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 */

#include "eio.h"

/* Initialize the lru list */
int lru_init(struct lru_ls **llist, index_t max)
{
	index_t i = 0;

	EIO_ASSERT(max > 0);
	*llist = vmalloc((sizeof(struct lru_ls) + (max - 1) * sizeof(struct lru_elem)));
	if (*llist == NULL)
		return -ENOMEM;

	(*llist)->ll_head = LRU_NULL;
	(*llist)->ll_tail = LRU_NULL;
	(*llist)->ll_max = max;
	(*llist)->ll_size = 0;

	for (i = 0; i < max; i++) {
		(*llist)->ll_elem[i].le_next = LRU_NULL;
		(*llist)->ll_elem[i].le_prev = LRU_NULL;
		(*llist)->ll_elem[i].le_key = 0;
	}

	return 0;
}

/* Uninitialize the lru list */
void lru_uninit(struct lru_ls *llist)
{
	if (llist)
		vfree(llist);
}

/* Add a new entry to lru list */
int lru_add(struct lru_ls *llist, index_t index, u_int64_t key)
{
	if (!llist || (index >= llist->ll_max))
		return -EINVAL;

	llist->ll_elem[index].le_prev = llist->ll_tail;
	llist->ll_elem[index].le_next = LRU_NULL;
	llist->ll_elem[index].le_key = key;

	if (llist->ll_tail != LRU_NULL)
		llist->ll_elem[llist->ll_tail].le_next = index;
	else {
		EIO_ASSERT(llist->ll_head == LRU_NULL);
		llist->ll_head = index;
	}
	llist->ll_tail = index;
	llist->ll_size++;

	return 0;
}

/* Remove an entry from the lru list */
int lru_rem(struct lru_ls *llist, index_t index)
{
	if (!llist || (index >= llist->ll_max) || (index == LRU_NULL))
		return -EINVAL;

	if (llist->ll_head == LRU_NULL && llist->ll_tail == LRU_NULL)
		/*
		 * No element in the list.
		 */
		return -EINVAL;

	if (llist->ll_elem[index].le_prev == LRU_NULL &&
	    llist->ll_elem[index].le_next == LRU_NULL &&
	    llist->ll_head != index && llist->ll_tail != index)
		/*
		 * Element not in list.
		 */
		return 0;

	if (llist->ll_elem[index].le_prev != LRU_NULL)
		llist->ll_elem[llist->ll_elem[index].le_prev].le_next =
			llist->ll_elem[index].le_next;

	if (llist->ll_elem[index].le_next != LRU_NULL)
		llist->ll_elem[llist->ll_elem[index].le_next].le_prev =
			llist->ll_elem[index].le_prev;

	if (llist->ll_head == index)
		llist->ll_head = llist->ll_elem[index].le_next;

	if (llist->ll_tail == index)
		llist->ll_tail = llist->ll_elem[index].le_prev;

	llist->ll_elem[index].le_prev = LRU_NULL;
	llist->ll_elem[index].le_next = LRU_NULL;
	EIO_ASSERT(llist->ll_size != 0);
	llist->ll_size--;

	return 0;
}

/* Move up the given lru element */
int lru_touch(struct lru_ls *llist, index_t index, u_int64_t key)
{
	if (!llist || (index >= llist->ll_max))
		return -EINVAL;

	if (llist->ll_tail == index)
		llist->ll_elem[index].le_key = key;
	else {
		lru_rem(llist, index);
		lru_add(llist, index, key);
	}

	return 0;
}

/* Read the element at the head of the lru */
int lru_read_head(struct lru_ls *llist, index_t *index, u_int64_t *key)
{
	if (!llist || !index || !key)
		return -EINVAL;

	*index = llist->ll_head;
	if (llist->ll_head == LRU_NULL) {
		*index = LRU_NULL;
		*key = 0;
	} else {
		*index = llist->ll_head;
		*key = llist->ll_elem[*index].le_key;
	}

	return 0;
}

/* Remove the element at the head of the lru */
int lru_rem_head(struct lru_ls *llist, index_t *index, u_int64_t *key)
{
	if (!llist || !index || !key)
		return -EINVAL;

	*index = llist->ll_head;
	if (llist->ll_head == LRU_NULL) {
		*index = LRU_NULL;
		*key = 0;
	} else {
		*index = llist->ll_head;
		*key = llist->ll_elem[*index].le_key;
		lru_rem(llist, *index);
	}

	return 0;
}
