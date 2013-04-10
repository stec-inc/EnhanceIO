/*
 *  eio_policy.h
 *
 *  Copyright (C) 2012 STEC, Inc. All rights not specifically granted
 *   under a license included herein are reserved
 *  Made EnhanceIO specific changes.
 *   Saied Kazemi <skazemi@stec-inc.com>
 *   Siddharth Choudhuri <schoudhuri@stec-inc.com>
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

#ifndef EIO_POLICY_H
#define EIO_POLICY_H

#include <linux/module.h>
#include <linux/list.h>

/*
 * Defines for policy types (EIO_REPL_XXX are in eio.h
 * so that user space utilties can use those definitions.
 */

/*
 * The LRU pointers are maintained as set-relative offsets, instead of
 * pointers. This enables us to store the LRU pointers per cacheblock
 * using 4 bytes instead of 16 bytes. The upshot of this is that we
 * are required to clamp the associativity at an 8K max.
 *
 * XXX - The above comment is from the original code. Looks like an error,
 * maximum associativity should be 32K (2^15) and not 8K.
 */
#define EIO_MAX_ASSOC   8192
#define EIO_LRU_NULL    0xFFFF

/* Declerations to keep the compiler happy */
struct cache_c;
struct eio_policy;
struct eio_lru;

/* LRU specific data structures and functions */
struct eio_lru {
	void (*sl_lru_pushblks)(struct eio_policy *);
	void (*sl_reclaim_lru_movetail)(struct cache_c *, index_t,
					struct eio_policy *);
};

/* Function prototypes for LRU wrappers in eio_policy.c */
void eio_policy_lru_pushblks(struct eio_policy *);
void eio_policy_reclaim_lru_movetail(struct cache_c *, index_t,
				     struct eio_policy *);

/*
 * Context that captures the cache block replacement policy.
 * There is one instance of this struct per dmc (cache)
 */
struct eio_policy {
	int sp_name;
	union {
		struct eio_lru *lru;
	} sp_policy;
	int (*sp_repl_init)(struct cache_c *);
	void (*sp_repl_exit)(void);
	int (*sp_repl_sets_init)(struct eio_policy *);
	int (*sp_repl_blk_init)(struct eio_policy *);
	void (*sp_find_reclaim_dbn)(struct eio_policy *,
				    index_t start_index, index_t *index);
	int (*sp_clean_set)(struct eio_policy *, index_t set, int);
	struct cache_c *sp_dmc;
};

/*
 * List of registered policies. There is one instance
 * of this structure per policy type.
 */
struct eio_policy_header {
	int sph_name;
	struct eio_policy *(*sph_instance_init)(void);
	struct list_head sph_list;
};

/* Prototypes of generic functions in eio_policy */
int *eio_repl_init(struct cache_c *);
int eio_repl_sets_init(struct eio_policy *);
int eio_repl_blk_init(struct eio_policy *);
void eio_find_reclaim_dbn(struct eio_policy *, index_t start_index,
			  index_t *index);
int eio_policy_clean_set(struct eio_policy *, index_t, int);

int eio_register_policy(struct eio_policy_header *);
int eio_unregister_policy(struct eio_policy_header *);
struct eio_policy *eio_get_policy(int);
void eio_put_policy(struct eio_policy *);

#endif                          /* EIO_POLICY_H */
