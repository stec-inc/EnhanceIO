/*
 *  eio_policy.c
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

#include "eio.h"

LIST_HEAD(eio_policy_list);

int eio_register_policy(struct eio_policy_header *new_policy)
{
	struct list_head *ptr;
	struct eio_policy_header *curr;

	list_for_each(ptr, &eio_policy_list) {
		curr = list_entry(ptr, struct eio_policy_header, sph_list);
		if (curr->sph_name == new_policy->sph_name)
			return 1;
	}
	list_add_tail(&new_policy->sph_list, &eio_policy_list);

	pr_info("register_policy: policy %d added", new_policy->sph_name);

	return 0;
}
EXPORT_SYMBOL(eio_register_policy);

int eio_unregister_policy(struct eio_policy_header *p_ops)
{
	struct list_head *ptr;
	struct eio_policy_header *curr;

	list_for_each(ptr, &eio_policy_list) {
		curr = list_entry(ptr, struct eio_policy_header, sph_list);
		if (curr->sph_name == p_ops->sph_name) {
			list_del(&curr->sph_list);
			pr_info("unregister_policy: policy %d removed",
				(int)p_ops->sph_name);
			return 0;
		}
	}

	return 1;
}
EXPORT_SYMBOL(eio_unregister_policy);

struct eio_policy *eio_get_policy(int policy)
{
	struct list_head *ptr;
	struct eio_policy_header *curr;

	list_for_each(ptr, &eio_policy_list) {
		curr = list_entry(ptr, struct eio_policy_header, sph_list);
		if (curr->sph_name == policy) {
			pr_info("get_policy: policy %d found", policy);
			return curr->sph_instance_init();
		}
	}
	pr_info("get_policy: cannot find policy %d", policy);

	return NULL;
}

/*
 * Decrement the reference count of the policy specific module
 * and any other cleanup that is required when an instance of a
 * policy is no longer required.
 */
void eio_put_policy(struct eio_policy *p_ops)
{

	if (p_ops == NULL) {
		pr_err("put_policy: Cannot decrement reference"
		       "count of NULL policy");
		return;
	}
	p_ops->sp_repl_exit();
}

/*
 * Wrappers for policy specific functions. These default to nothing if the
 * default policy is being used.
 */
int eio_repl_sets_init(struct eio_policy *p_ops)
{

	return (p_ops &&
		p_ops->sp_repl_sets_init) ? p_ops->sp_repl_sets_init(p_ops) : 0;
}

int eio_repl_blk_init(struct eio_policy *p_ops)
{

	return (p_ops &&
		p_ops->sp_repl_blk_init) ? p_ops->sp_repl_blk_init(p_ops) : 0;
}

void
eio_find_reclaim_dbn(struct eio_policy *p_ops,
		     index_t start_index, index_t *index)
{

	p_ops->sp_find_reclaim_dbn(p_ops, start_index, index);
}

int eio_policy_clean_set(struct eio_policy *p_ops, index_t set, int to_clean)
{

	return p_ops->sp_clean_set(p_ops, set, to_clean);
}

/*
 * LRU Specific functions
 */
void eio_policy_lru_pushblks(struct eio_policy *p_ops)
{

	if (p_ops && p_ops->sp_name == CACHE_REPL_LRU)
		p_ops->sp_policy.lru->sl_lru_pushblks(p_ops);
}

void
eio_policy_reclaim_lru_movetail(struct cache_c *dmc, index_t i,
				struct eio_policy *p_ops)
{

	if (p_ops && p_ops->sp_name == CACHE_REPL_LRU)
		p_ops->sp_policy.lru->sl_reclaim_lru_movetail(dmc, i, p_ops);
}
