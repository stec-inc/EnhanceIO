/*
 *  eio_lru.c
 *
 *  Copyright (C) 2012 STEC, Inc. All rights not specifically granted
 *   under a license included herein are reserved
 *  Made EnhanceIO specific changes.
 *   Saied Kazemi <skazemi@stec-inc.com>
 *   Siddharth Choudhuri <schoudhuri@stec-inc.com>
 *
 *  Copyright 2010 Facebook, Inc.
 *   Author: Mohan Srinivasan (mohan@facebook.com)
 *
 *  Based on DM-Cache:
 *   Copyright (C) International Business Machines Corp., 2006
 *   Author: Ming Zhao (mingzhao@ufl.edu)
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "eio.h"
/* Generic policy functions prototyes */
int eio_lru_init(struct cache_c *);
void eio_lru_exit(void);
int eio_lru_cache_sets_init(struct eio_policy *);
int eio_lru_cache_blk_init(struct eio_policy *);
void eio_lru_find_reclaim_dbn(struct eio_policy *, index_t, index_t *);
int eio_lru_clean_set(struct eio_policy *, index_t, int);
/* Per policy instance initialization */
struct eio_policy *eio_lru_instance_init(void);

/* LRU specific policy functions prototype */
void eio_lru_pushblks(struct eio_policy *);
void eio_reclaim_lru_movetail(struct cache_c *, index_t, struct eio_policy *);

/* Per cache set data structure */
struct eio_lru_cache_set {
	u_int16_t lru_head, lru_tail;
};

/* Per cache block data structure */
struct eio_lru_cache_block {
	u_int16_t lru_prev, lru_next;
};

/* LRU specifc data structures */
static struct eio_lru eio_lru = {
	.sl_lru_pushblks		= eio_lru_pushblks,
	.sl_reclaim_lru_movetail	= eio_reclaim_lru_movetail,
};

/*
 * Context that captures the LRU replacement policy
 */
static struct eio_policy_header eio_lru_ops = {
	.sph_name		= CACHE_REPL_LRU,
	.sph_instance_init	= eio_lru_instance_init,
};

/*
 * Intialize LRU. Called from ctr.
 */
int eio_lru_init(struct cache_c *dmc)
{
	return 0;
}

/*
 * Initialize per set LRU data structures.
 */
int eio_lru_cache_sets_init(struct eio_policy *p_ops)
{
	sector_t order;
	int i;
	struct cache_c *dmc = p_ops->sp_dmc;
	struct eio_lru_cache_set *cache_sets;

	order =
		(dmc->size >> dmc->consecutive_shift) *
		sizeof(struct eio_lru_cache_set);

	dmc->sp_cache_set = vmalloc((size_t)order);
	if (dmc->sp_cache_set == NULL)
		return -ENOMEM;

	cache_sets = (struct eio_lru_cache_set *)dmc->sp_cache_set;

	for (i = 0; i < (int)(dmc->size >> dmc->consecutive_shift); i++) {
		cache_sets[i].lru_tail = EIO_LRU_NULL;
		cache_sets[i].lru_head = EIO_LRU_NULL;
	}
	pr_info("Initialized %d sets in LRU", i);

	return 0;
}

/*
 * Initialize per block LRU data structures
 */
int eio_lru_cache_blk_init(struct eio_policy *p_ops)
{
	sector_t order;
	struct cache_c *dmc = p_ops->sp_dmc;

	order = dmc->size * sizeof(struct eio_lru_cache_block);

	dmc->sp_cache_blk = vmalloc((size_t)order);
	if (dmc->sp_cache_blk == NULL)
		return -ENOMEM;

	return 0;
}

/*
 * Allocate a new instance of eio_policy per dmc
 */
struct eio_policy *eio_lru_instance_init(void)
{
	struct eio_policy *new_instance;

	new_instance = vmalloc(sizeof(struct eio_policy));
	if (new_instance == NULL) {
		pr_err("eio_lru_instance_init: vmalloc failed");
		return NULL;
	}

	/* Initialize the LRU specific functions and variables */
	new_instance->sp_name = CACHE_REPL_LRU;
	new_instance->sp_policy.lru = &eio_lru;
	new_instance->sp_repl_init = eio_lru_init;
	new_instance->sp_repl_exit = eio_lru_exit;
	new_instance->sp_repl_sets_init = eio_lru_cache_sets_init;
	new_instance->sp_repl_blk_init = eio_lru_cache_blk_init;
	new_instance->sp_find_reclaim_dbn = eio_lru_find_reclaim_dbn;
	new_instance->sp_clean_set = eio_lru_clean_set;
	new_instance->sp_dmc = NULL;

	try_module_get(THIS_MODULE);

	pr_info("eio_lru_instance_init: created new instance of LRU");

	return new_instance;
}

/*
 * Cleanup an instance of eio_policy (called from dtr).
 */
void eio_lru_exit(void)
{
	module_put(THIS_MODULE);
}

/*
 * Find a victim block to evict and return it in index.
 */
void
eio_lru_find_reclaim_dbn(struct eio_policy *p_ops,
			 index_t start_index, index_t *index)
{
	index_t lru_rel_index;
	struct eio_lru_cache_set *lru_sets;
	struct eio_lru_cache_block *lru_blk;
	struct cache_c *dmc = p_ops->sp_dmc;
	index_t set;

	set = start_index / dmc->assoc;
	lru_sets = (struct eio_lru_cache_set *)(dmc->sp_cache_set);

	lru_rel_index = lru_sets[set].lru_head;
	while (lru_rel_index != EIO_LRU_NULL) {
		lru_blk =
			((struct eio_lru_cache_block *)dmc->sp_cache_blk +
			 lru_rel_index + start_index);
		if (EIO_CACHE_STATE_GET(dmc, (lru_rel_index + start_index)) ==
		    VALID) {
			EIO_ASSERT((lru_blk - (struct eio_lru_cache_block *)
				    dmc->sp_cache_blk) ==
				   (lru_rel_index + start_index));
			*index = lru_rel_index + start_index;
			eio_reclaim_lru_movetail(dmc, *index, p_ops);
			break;
		}
		lru_rel_index = lru_blk->lru_next;
	}

	return;
}

/*
 * Go through the entire set and clean.
 */
int eio_lru_clean_set(struct eio_policy *p_ops, index_t set, int to_clean)
{
	struct cache_c *dmc = p_ops->sp_dmc;
	index_t lru_rel_index;
	int nr_writes = 0;
	struct eio_lru_cache_set *lru_cache_sets;
	struct eio_lru_cache_block *lru_cacheblk;
	index_t dmc_idx;
	index_t start_index;

	lru_cache_sets = (struct eio_lru_cache_set *)dmc->sp_cache_set;
	start_index = set * dmc->assoc;
	lru_rel_index = lru_cache_sets[set].lru_head;

	while ((lru_rel_index != EIO_LRU_NULL) && (nr_writes < to_clean)) {
		dmc_idx = lru_rel_index + start_index;
		lru_cacheblk =
			((struct eio_lru_cache_block *)dmc->sp_cache_blk +
			 lru_rel_index + start_index);
		EIO_ASSERT((lru_cacheblk -
			    (struct eio_lru_cache_block *)dmc->sp_cache_blk) ==
			   (lru_rel_index + start_index));
		if ((EIO_CACHE_STATE_GET(dmc, dmc_idx) &
		     (DIRTY | BLOCK_IO_INPROG)) == DIRTY) {
			EIO_CACHE_STATE_ON(dmc, dmc_idx, DISKWRITEINPROG);
			nr_writes++;
		}
		lru_rel_index = lru_cacheblk->lru_next;
	}

	return nr_writes;
}

/*
 * LRU specific functions.
 */
void
eio_reclaim_lru_movetail(struct cache_c *dmc, index_t index,
			 struct eio_policy *p_ops)
{
	index_t set = index / dmc->assoc;
	index_t start_index = set * dmc->assoc;
	index_t my_index = index - start_index;
	struct eio_lru_cache_block *cacheblk;
	struct eio_lru_cache_set *cache_sets;
	struct eio_lru_cache_block *blkptr;

	cacheblk =
		(((struct eio_lru_cache_block *)(dmc->sp_cache_blk)) + index);
	cache_sets = (struct eio_lru_cache_set *)dmc->sp_cache_set;
	blkptr = (struct eio_lru_cache_block *)(dmc->sp_cache_blk);

	/* Remove from LRU */
	if (likely((cacheblk->lru_prev != EIO_LRU_NULL) ||
		   (cacheblk->lru_next != EIO_LRU_NULL))) {
		if (cacheblk->lru_prev != EIO_LRU_NULL)
			blkptr[cacheblk->lru_prev + start_index].lru_next =
				cacheblk->lru_next;
		else
			cache_sets[set].lru_head = cacheblk->lru_next;
		if (cacheblk->lru_next != EIO_LRU_NULL)
			blkptr[cacheblk->lru_next + start_index].lru_prev =
				cacheblk->lru_prev;
		else
			cache_sets[set].lru_tail = cacheblk->lru_prev;
	}
	/* And add it to LRU Tail */
	cacheblk->lru_next = EIO_LRU_NULL;
	cacheblk->lru_prev = cache_sets[set].lru_tail;
	if (cache_sets[set].lru_tail == EIO_LRU_NULL)
		cache_sets[set].lru_head = (u_int16_t)my_index;
	else
		blkptr[cache_sets[set].lru_tail + start_index].lru_next =
			(u_int16_t)my_index;
	cache_sets[set].lru_tail = (u_int16_t)my_index;
}

void eio_lru_pushblks(struct eio_policy *p_ops)
{
	struct cache_c *dmc = p_ops->sp_dmc;
	struct eio_lru_cache_block *cache_block;
	int i;

	cache_block = dmc->sp_cache_blk;
	for (i = 0; i < (int)dmc->size; i++) {
		cache_block[i].lru_prev = EIO_LRU_NULL;
		cache_block[i].lru_next = EIO_LRU_NULL;
		eio_reclaim_lru_movetail(dmc, i, p_ops);
	}
	return;
}

static
int __init lru_register(void)
{
	int ret;

	ret = eio_register_policy(&eio_lru_ops);
	if (ret != 0)
		pr_info("eio_lru already registered");

	return ret;
}

static
void __exit lru_unregister(void)
{
	int ret;

	ret = eio_unregister_policy(&eio_lru_ops);
	if (ret != 0)
		pr_err("eio_lru unregister failed");
}

module_init(lru_register);
module_exit(lru_unregister);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("LRU policy for EnhanceIO");
MODULE_AUTHOR("STEC, Inc. based on code by Facebook");
