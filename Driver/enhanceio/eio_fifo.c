/*
 *  eio_fifo.c
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
/* Generic policy functions prototypes */
int eio_fifo_init(struct cache_c *);
void eio_fifo_exit(void);
int eio_fifo_cache_sets_init(struct eio_policy *);
int eio_fifo_cache_blk_init(struct eio_policy *);
void eio_fifo_find_reclaim_dbn(struct eio_policy *, index_t, index_t *);
int eio_fifo_clean_set(struct eio_policy *, index_t, int);

/* Per policy instance initialization */
struct eio_policy *eio_fifo_instance_init(void);

/* Per cache set data structure */
struct eio_fifo_cache_set {
	index_t set_fifo_next;
	index_t set_clean_next;
};

/*
 * Context that captures the FIFO replacement policy
 */
static struct eio_policy_header eio_fifo_ops = {
	.sph_name		= CACHE_REPL_FIFO,
	.sph_instance_init	= eio_fifo_instance_init,
};

/*
 * Initialize FIFO policy.
 */
int eio_fifo_init(struct cache_c *dmc)
{
	return 0;
}

/*
 * Initialize FIFO data structure called from ctr.
 */
int eio_fifo_cache_sets_init(struct eio_policy *p_ops)
{
	int i;
	sector_t order;
	struct cache_c *dmc = p_ops->sp_dmc;
	struct eio_fifo_cache_set *cache_sets;

	pr_info("Initializing fifo cache sets\n");
	order = (dmc->size >> dmc->consecutive_shift) *
		sizeof(struct eio_fifo_cache_set);

	dmc->sp_cache_set =
		(struct eio_fifo_cache_set *)vmalloc((size_t)order);
	if (dmc->sp_cache_set == NULL)
		return -ENOMEM;

	cache_sets = (struct eio_fifo_cache_set *)dmc->sp_cache_set;

	for (i = 0; i < (int)(dmc->size >> dmc->consecutive_shift); i++) {
		cache_sets[i].set_fifo_next = i * dmc->assoc;
		cache_sets[i].set_clean_next = i * dmc->assoc;
	}

	return 0;
}

/*
 * The actual function that returns a victim block in index.
 */
void
eio_fifo_find_reclaim_dbn(struct eio_policy *p_ops, index_t start_index,
			  index_t *index)
{
	index_t end_index;
	int slots_searched = 0;
	index_t i;
	index_t set;
	struct eio_fifo_cache_set *cache_sets;
	struct cache_c *dmc = p_ops->sp_dmc;

	set = start_index / dmc->assoc;
	end_index = start_index + dmc->assoc;
	cache_sets = (struct eio_fifo_cache_set *)dmc->sp_cache_set;

	i = cache_sets[set].set_fifo_next;
	while (slots_searched < (int)dmc->assoc) {
		EIO_ASSERT(i >= start_index);
		EIO_ASSERT(i < end_index);
		if (EIO_CACHE_STATE_GET(dmc, i) == VALID) {
			*index = i;
			break;
		}
		slots_searched++;
		i++;
		if (i == end_index)
			i = start_index;
	}
	i++;
	if (i == end_index)
		i = start_index;
	cache_sets[set].set_fifo_next = i;
}

/*
 * Go through the entire set and clean.
 */
int eio_fifo_clean_set(struct eio_policy *p_ops, index_t set, int to_clean)
{
	index_t i;
	int scanned = 0, nr_writes = 0;
	index_t start_index;
	index_t end_index;
	struct eio_fifo_cache_set *cache_sets;
	struct cache_c *dmc;

	dmc = p_ops->sp_dmc;
	cache_sets = (struct eio_fifo_cache_set *)dmc->sp_cache_set;
	start_index = set * dmc->assoc;
	end_index = start_index + dmc->assoc;
	i = cache_sets[set].set_clean_next;

	while ((scanned < (int)dmc->assoc) && (nr_writes < to_clean)) {
		if ((EIO_CACHE_STATE_GET(dmc, i) & (DIRTY | BLOCK_IO_INPROG)) ==
		    DIRTY) {
			EIO_CACHE_STATE_ON(dmc, i, DISKWRITEINPROG);
			nr_writes++;
		}
		scanned++;
		i++;
		if (i == end_index)
			i = start_index;
	}
	cache_sets[set].set_clean_next = i;

	return nr_writes;
}

/*
 * FIFO is per set, so do nothing on a per block init.
 */
int eio_fifo_cache_blk_init(struct eio_policy *p_ops)
{
	return 0;
}

/*
 * Allocate a new instance of eio_policy per dmc
 */
struct eio_policy *eio_fifo_instance_init(void)
{
	struct eio_policy *new_instance;

	new_instance = (struct eio_policy *)vmalloc(sizeof(struct eio_policy));
	if (new_instance == NULL) {
		pr_err("ssdscache_fifo_instance_init: vmalloc failed");
		return NULL;
	}

	/* Initialize the FIFO specific functions and variables */
	new_instance->sp_name = CACHE_REPL_FIFO;
	new_instance->sp_policy.lru = NULL;
	new_instance->sp_repl_init = eio_fifo_init;
	new_instance->sp_repl_exit = eio_fifo_exit;
	new_instance->sp_repl_sets_init = eio_fifo_cache_sets_init;
	new_instance->sp_repl_blk_init = eio_fifo_cache_blk_init;
	new_instance->sp_find_reclaim_dbn = eio_fifo_find_reclaim_dbn;
	new_instance->sp_clean_set = eio_fifo_clean_set;
	new_instance->sp_dmc = NULL;

	try_module_get(THIS_MODULE);

	pr_info("eio_fifo_instance_init: created new instance of FIFO");

	return new_instance;
}

/*
 * Cleanup an instance of eio_policy (called from dtr).
 */
void eio_fifo_exit(void)
{
	module_put(THIS_MODULE);
}

static
int __init fifo_register(void)
{
	int ret;

	ret = eio_register_policy(&eio_fifo_ops);
	if (ret != 0)
		pr_info("eio_fifo already registered");

	return ret;
}

static
void __exit fifo_unregister(void)
{
	int ret;

	ret = eio_unregister_policy(&eio_fifo_ops);
	if (ret != 0)
		pr_err("eio_fifo unregister failed");
}

module_init(fifo_register);
module_exit(fifo_unregister);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FIFO policy for EnhanceIO");
MODULE_AUTHOR("STEC, Inc. based on code by Facebook");
