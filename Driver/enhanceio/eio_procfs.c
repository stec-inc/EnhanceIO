/*
 *  eio_procfs.c
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

#include "eio.h"
#define EIO_RELEASE "ENHANCEIO"

#ifndef ENHANCEIO_GIT_COMMIT_HASH
#define ENHANCEIO_GIT_COMMIT_HASH "unknown-git-version"
#endif                          /* !ENHANCEIO_GIT_COMMIT_HASH */

int eio_version_query(size_t buf_sz, char *bufp)
{
	if (unlikely(buf_sz == 0) || unlikely(bufp == NULL))
		return -EINVAL;
	snprintf(bufp, buf_sz, "EnhanceIO Version: %s %s (checksum disabled)",
		 EIO_RELEASE, ENHANCEIO_GIT_COMMIT_HASH);

	bufp[buf_sz - 1] = '\0';

	return 0;
}

static struct sysctl_table_dir *sysctl_handle_dir;

/*
 * eio_zerostats_sysctl
 */
static int
eio_zerostats_sysctl(ctl_table *table, int write, void __user *buffer,
		     size_t *length, loff_t *ppos)
{
	struct cache_c *dmc = (struct cache_c *)table->extra1;
	long long cached_blocks;
	unsigned long flags = 0;

	/* fetch the new tunable value or post the existing value */

	if (!write) {
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->sysctl_pending.zerostats = dmc->sysctl_active.zerostats;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	}

	proc_dointvec(table, write, buffer, length, ppos);

	/* do write processing */

	if (write) {
		/* do sanity check */

		if ((dmc->sysctl_pending.zerostats != 0) &&
		    (dmc->sysctl_pending.zerostats != 1)) {
			pr_err
				("0 or 1 are the only valid values for zerostats");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.zerostats ==
		    dmc->sysctl_active.zerostats)
			/* same value. Nothing to work */
			return 0;

		/* Copy to active */
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->sysctl_active.zerostats = dmc->sysctl_pending.zerostats;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);

		/* apply the new tunable value */

		if (dmc->sysctl_active.zerostats) {
			/*
			 * The number of cached blocks should not be zero'd since
			 * these blocks are already on cache dev. Making this zero
			 * may lead to -ve count during block invalidate, and also,
			 * incorrectly indicating how much data is cached.
			 *
			 * TODO - should have used an spinlock, but existing spinlocks
			 * are inadequate to fully protect this
			 */

			cached_blocks =
				atomic64_read(&dmc->eio_stats.cached_blocks);
			memset(&dmc->eio_stats, 0, sizeof(struct eio_stats));
			atomic64_set(&dmc->eio_stats.cached_blocks,
				     cached_blocks);
		}
	}

	return 0;
}

/*
 * eio_mem_limit_pct_sysctl
 * - sets the eio sysctl mem_limit_pct value
 */
static int
eio_mem_limit_pct_sysctl(ctl_table *table, int write, void __user *buffer,
			 size_t *length, loff_t *ppos)
{
	struct cache_c *dmc = (struct cache_c *)table->extra1;
	unsigned long flags = 0;

	/* fetch the new tunable value or post the existing value */

	if (!write) {
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->sysctl_pending.mem_limit_pct =
			dmc->sysctl_active.mem_limit_pct;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	}

	proc_dointvec(table, write, buffer, length, ppos);

	/* do write processing */

	if (write) {
		/* do sanity check */
		if ((dmc->sysctl_pending.mem_limit_pct < 0) ||
		    (dmc->sysctl_pending.mem_limit_pct > 100)) {
			pr_err
				("only valid percents are [0 - 100] for mem_limit_pct");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.mem_limit_pct ==
		    dmc->sysctl_active.mem_limit_pct)
			/* same value. Nothing more to do */
			return 0;

		/* Copy to active */
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->sysctl_active.mem_limit_pct =
			dmc->sysctl_pending.mem_limit_pct;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	}

	return 0;
}

/*
 * eio_clean_sysctl
 */
static int
eio_clean_sysctl(ctl_table *table, int write, void __user *buffer,
		 size_t *length, loff_t *ppos)
{
	struct cache_c *dmc = (struct cache_c *)table->extra1;
	unsigned long flags = 0;

	/* fetch the new tunable value or post the existing value */

	if (!write) {
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->sysctl_pending.do_clean = dmc->sysctl_active.do_clean;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	}

	proc_dointvec(table, write, buffer, length, ppos);

	/* do write processing */

	if (write) {
		/* Do sanity check */

		if (dmc->mode != CACHE_MODE_WB) {
			/* do_clean is only valid for writeback cache */
			pr_err("do_clean is only valid for writeback cache");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.
		    do_clean & ~(EIO_CLEAN_START | EIO_CLEAN_KEEP)) {
			pr_err
				("do_clean should be either clean start/clean keep");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.do_clean == dmc->sysctl_active.do_clean)
			/* New and old values are same. No work required */
			return 0;

		/* Copy to active and apply the new tunable value */

		spin_lock_irqsave(&dmc->cache_spin_lock, flags);

		if (dmc->cache_flags & CACHE_FLAGS_MOD_INPROG) {
			spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
			pr_err
				("do_clean called while cache modification in progress");
			return -EBUSY;
		} else {
			dmc->sysctl_active.do_clean =
				dmc->sysctl_pending.do_clean;

			if (dmc->sysctl_active.do_clean) {
				atomic_set(&dmc->clean_index, 0);
				dmc->sysctl_active.do_clean |= EIO_CLEAN_START;
				spin_unlock_irqrestore(&dmc->cache_spin_lock,
						       flags);

				/*
				 * Wake up the clean thread.
				 * Sync thread will do the clean and once complete
				 * will reset the clean_start flag.
				 * The clean_keep flag will remain set(unless reset
				 * by user) and will prevent new I/Os from making
				 * the blocks dirty.
				 */

				spin_lock_irqsave(&dmc->clean_sl, flags);
				EIO_SET_EVENT_AND_UNLOCK(&dmc->clean_event,
							 &dmc->clean_sl, flags);
			} else
				spin_unlock_irqrestore(&dmc->cache_spin_lock,
						       flags);
		}
	}

	return 0;
}

/*
 * eio_dirty_high_threshold_sysctl
 */
static int
eio_dirty_high_threshold_sysctl(ctl_table *table, int write,
				void __user *buffer, size_t *length,
				loff_t *ppos)
{
	struct cache_c *dmc = (struct cache_c *)table->extra1;
	unsigned long flags = 0;

	/* fetch the new tunable value or post the existing value */

	if (!write) {
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->sysctl_pending.dirty_high_threshold =
			dmc->sysctl_active.dirty_high_threshold;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	}

	proc_dointvec(table, write, buffer, length, ppos);

	/* do write processing */

	if (write) {
		int error;
		uint32_t old_value;

		/* do sanity check */

		if (dmc->mode != CACHE_MODE_WB) {
			pr_err
				("dirty_high_threshold is only valid for writeback cache");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.dirty_high_threshold > 100) {
			pr_err
				("dirty_high_threshold percentage should be [0 - 100]");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.dirty_high_threshold <
		    dmc->sysctl_active.dirty_low_threshold) {
			pr_err
				("dirty high shouldn't be less than dirty low threshold");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.dirty_high_threshold ==
		    dmc->sysctl_active.dirty_high_threshold)
			/* new is same as old value. No need to take any action */
			return 0;

		/* update the active value with the new tunable value */
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		old_value = dmc->sysctl_active.dirty_high_threshold;
		dmc->sysctl_active.dirty_high_threshold =
			dmc->sysctl_pending.dirty_high_threshold;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);

		/* apply the new tunable value */

		/* Store the change persistently */
		error = eio_sb_store(dmc);
		if (error) {
			/* restore back the old value and return error */
			spin_lock_irqsave(&dmc->cache_spin_lock, flags);
			dmc->sysctl_active.dirty_high_threshold = old_value;
			spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);

			return error;
		}

		/* if we reduced the high threshold, check if we require cache cleaning */
		if (old_value > dmc->sysctl_active.dirty_high_threshold)
			eio_comply_dirty_thresholds(dmc, -1);
	}

	return 0;
}

/*
 * eio_dirty_low_threshold_sysctl
 */
static int
eio_dirty_low_threshold_sysctl(ctl_table *table, int write,
			       void __user *buffer, size_t *length,
			       loff_t *ppos)
{
	struct cache_c *dmc = (struct cache_c *)table->extra1;
	unsigned long flags = 0;

	/* fetch the new tunable value or post the existing value */

	if (!write) {
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->sysctl_pending.dirty_low_threshold =
			dmc->sysctl_active.dirty_low_threshold;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	}

	proc_dointvec(table, write, buffer, length, ppos);

	/* do write processing */

	if (write) {
		int error;
		uint32_t old_value;

		/* do sanity check */

		if (dmc->mode != CACHE_MODE_WB) {
			pr_err
				("dirty_low_threshold is valid for only writeback cache");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.dirty_low_threshold > 100) {
			pr_err
				("dirty_low_threshold percentage should be [0 - 100]");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.dirty_low_threshold >
		    dmc->sysctl_active.dirty_high_threshold) {
			pr_err
				("dirty low shouldn't be more than dirty high threshold");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.dirty_low_threshold ==
		    dmc->sysctl_active.dirty_low_threshold)
			/* new is same as old value. No need to take any action */
			return 0;

		/* update the active value with the new tunable value */
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		old_value = dmc->sysctl_active.dirty_low_threshold;
		dmc->sysctl_active.dirty_low_threshold =
			dmc->sysctl_pending.dirty_low_threshold;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);

		/* apply the new tunable value */

		/* Store the change persistently */
		error = eio_sb_store(dmc);
		if (error) {
			/* restore back the old value and return error */
			spin_lock_irqsave(&dmc->cache_spin_lock, flags);
			dmc->sysctl_active.dirty_low_threshold = old_value;
			spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);

			return error;
		}

		if (old_value > dmc->sysctl_active.dirty_low_threshold)
			/*
			 * Although the low threshold set shouldn't trigger new cleans,
			 * but because we set the tunables one at a time from user mode,
			 * it is possible that the high threshold value triggering clean
			 * did not happen and should get triggered now that the low value
			 * has been changed, so we are calling the comply function here
			 */
			eio_comply_dirty_thresholds(dmc, -1);
	}

	return 0;
}

/*
 * eio_dirty_set_high_threshold_sysctl
 */
static int
eio_dirty_set_high_threshold_sysctl(ctl_table *table, int write,
				    void __user *buffer, size_t *length,
				    loff_t *ppos)
{
	struct cache_c *dmc = (struct cache_c *)table->extra1;
	unsigned long flags = 0;

	/* fetch the new tunable value or post the existing value */

	if (!write) {
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->sysctl_pending.dirty_set_high_threshold =
			dmc->sysctl_active.dirty_set_high_threshold;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	}

	proc_dointvec(table, write, buffer, length, ppos);

	/* do write processing */

	if (write) {
		int error;
		uint32_t old_value;
		u_int64_t i;

		/* do sanity check */

		if (dmc->mode != CACHE_MODE_WB) {
			pr_err
				("dirty_set_high_threshold is valid only for writeback cache");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.dirty_set_high_threshold > 100) {
			pr_err
				("dirty_set_high_threshold percentage should be [0 - 100]");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.dirty_set_high_threshold <
		    dmc->sysctl_active.dirty_set_low_threshold) {
			pr_err
				("dirty_set_high_threshold shouldn't be less than dirty low threshold");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.dirty_set_high_threshold ==
		    dmc->sysctl_active.dirty_set_high_threshold)
			/* new is same as old value. No need to take any action */
			return 0;

		/* update the active value with the new tunable value */
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		old_value = dmc->sysctl_active.dirty_set_high_threshold;
		dmc->sysctl_active.dirty_set_high_threshold =
			dmc->sysctl_pending.dirty_set_high_threshold;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);

		/* apply the new tunable value */

		/* Store the change persistently */
		error = eio_sb_store(dmc);
		if (error) {
			/* restore back the old value and return error */
			spin_lock_irqsave(&dmc->cache_spin_lock, flags);
			dmc->sysctl_active.dirty_set_high_threshold = old_value;
			spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);

			return error;
		}

		if (old_value > dmc->sysctl_active.dirty_set_high_threshold) {
			/* Check each set for dirty blocks cleaning */
			for (i = 0; i < (dmc->size >> dmc->consecutive_shift);
			     i++)
				eio_comply_dirty_thresholds(dmc, i);
		}
	}

	return 0;
}

/*
 * eio_dirty_set_low_threshold_sysctl
 */
static int
eio_dirty_set_low_threshold_sysctl(ctl_table *table, int write,
				   void __user *buffer, size_t *length,
				   loff_t *ppos)
{
	struct cache_c *dmc = (struct cache_c *)table->extra1;
	unsigned long flags = 0;

	/* fetch the new tunable value or post the existing value */

	if (!write) {
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->sysctl_pending.dirty_set_low_threshold =
			dmc->sysctl_active.dirty_set_low_threshold;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	}

	proc_dointvec(table, write, buffer, length, ppos);

	/* do write processing */

	if (write) {
		int error;
		uint32_t old_value;
		u_int64_t i;

		/* do sanity check */

		if (dmc->mode != CACHE_MODE_WB) {
			pr_err
				("dirty_set_low_threshold is valid only for writeback cache");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.dirty_set_low_threshold > 100) {
			pr_err
				("dirty_set_low_threshold percentage should be [0 - 100]");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.dirty_set_low_threshold >
		    dmc->sysctl_active.dirty_set_high_threshold) {
			pr_err
				("dirty_set_low_threshold shouldn't be more than dirty_set_high_threshold");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.dirty_set_low_threshold ==
		    dmc->sysctl_active.dirty_set_low_threshold)
			/* new is same as old value. No need to take any action */
			return 0;

		/* update the active value with the new tunable value */
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		old_value = dmc->sysctl_active.dirty_set_low_threshold;
		dmc->sysctl_active.dirty_set_low_threshold =
			dmc->sysctl_pending.dirty_set_low_threshold;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);

		/* apply the new tunable value */

		/* Store the change persistently */
		error = eio_sb_store(dmc);
		if (error) {
			/* restore back the old value and return error */
			spin_lock_irqsave(&dmc->cache_spin_lock, flags);
			dmc->sysctl_active.dirty_set_low_threshold = old_value;
			spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);

			return error;
		}

		/*
		 * Although the low threshold value shouldn't trigger new cleans,
		 * but because we set the tunables one at a time from user mode,
		 * it is possible that the high threshold value triggering clean
		 * did not happen and should get triggered now that the low value
		 * has been changed, so we are calling the comply function again
		 */
		if (old_value > dmc->sysctl_active.dirty_set_low_threshold) {
			/* Check each set for dirty blocks cleaning */
			for (i = 0; i < (dmc->size >> dmc->consecutive_shift);
			     i++)
				eio_comply_dirty_thresholds(dmc, i);
		}
	}

	return 0;
}

/*
 * eio_autoclean_threshold_sysctl
 */
static int
eio_autoclean_threshold_sysctl(ctl_table *table, int write,
			       void __user *buffer, size_t *length,
			       loff_t *ppos)
{
	struct cache_c *dmc = (struct cache_c *)table->extra1;
	unsigned long flags = 0;

	/* fetch the new tunable value or post existing value */

	if (!write) {
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->sysctl_pending.autoclean_threshold =
			dmc->sysctl_active.autoclean_threshold;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	}

	proc_dointvec(table, write, buffer, length, ppos);

	/* do write processing */

	if (write) {
		int error;
		int old_value;

		/* do sanity check */

		if (dmc->mode != CACHE_MODE_WB) {
			pr_err
				("autoclean_threshold is valid only for writeback cache");
			return -EINVAL;
		}

		if ((dmc->sysctl_pending.autoclean_threshold < 0) ||
		    (dmc->sysctl_pending.autoclean_threshold >
		     AUTOCLEAN_THRESH_MAX)) {
			pr_err("autoclean_threshold is valid range is 0 to %d",
			       AUTOCLEAN_THRESH_MAX);
			return -EINVAL;
		}

		if (dmc->sysctl_pending.autoclean_threshold ==
		    dmc->sysctl_active.autoclean_threshold)
			/* new is same as old value. No need to take any action */
			return 0;

		/* update the active value with the new tunable value */
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		old_value = dmc->sysctl_active.autoclean_threshold;
		dmc->sysctl_active.autoclean_threshold =
			dmc->sysctl_pending.autoclean_threshold;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);

		/* apply the new tunable value */

		/* Store the change persistently */
		error = eio_sb_store(dmc);
		if (error) {
			/* restore back the old value and return error */
			spin_lock_irqsave(&dmc->cache_spin_lock, flags);
			dmc->sysctl_active.autoclean_threshold = old_value;
			spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);

			return error;
		}

		/* Ensure new thresholds are being complied */
		eio_comply_dirty_thresholds(dmc, -1);
	}

	return 0;
}

/*
 * eio_time_based_clean_interval_sysctl
 */
static int
eio_time_based_clean_interval_sysctl(ctl_table *table, int write,
				     void __user *buffer, size_t *length,
				     loff_t *ppos)
{
	struct cache_c *dmc = (struct cache_c *)table->extra1;
	unsigned long flags = 0;

	/* fetch the new tunable value or post existing value */

	if (!write) {
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->sysctl_pending.time_based_clean_interval =
			dmc->sysctl_active.time_based_clean_interval;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	}

	proc_dointvec(table, write, buffer, length, ppos);

	/* do write processing */

	if (write) {
		int error;
		uint32_t old_value;

		/* do sanity check */

		if (dmc->mode != CACHE_MODE_WB) {
			pr_err
				("time_based_clean_interval is valid only for writeback cache");
			return -EINVAL;
		}

		if (dmc->sysctl_pending.time_based_clean_interval >
		    TIME_BASED_CLEAN_INTERVAL_MAX) {
			/* valid values are 0 to TIME_BASED_CLEAN_INTERVAL_MAX */
			pr_err
				("time_based_clean_interval valid range is 0 to %u",
				TIME_BASED_CLEAN_INTERVAL_MAX);
			return -EINVAL;
		}

		if (dmc->sysctl_pending.time_based_clean_interval ==
		    dmc->sysctl_active.time_based_clean_interval)
			/* new is same as old value */
			return 0;

		/* update the active value with the new tunable value */
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		old_value = dmc->sysctl_active.time_based_clean_interval;
		dmc->sysctl_active.time_based_clean_interval =
			dmc->sysctl_pending.time_based_clean_interval;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);

		/* apply the new tunable value */

		/* Store the change persistently */
		error = eio_sb_store(dmc);
		if (error) {
			/* restore back the old value and return error */
			spin_lock_irqsave(&dmc->cache_spin_lock, flags);
			dmc->sysctl_active.time_based_clean_interval =
				old_value;
			spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);

			return error;
		}

		/* Reschedule the time based clean, based on new interval */
		cancel_delayed_work_sync(&dmc->clean_aged_sets_work);
		spin_lock_irqsave(&dmc->dirty_set_lru_lock, flags);
		dmc->is_clean_aged_sets_sched = 0;
		if (dmc->sysctl_active.time_based_clean_interval
		    && atomic64_read(&dmc->nr_dirty)) {
			schedule_delayed_work(&dmc->clean_aged_sets_work,
					      dmc->sysctl_active.
					      time_based_clean_interval * 60 *
					      HZ);
			dmc->is_clean_aged_sets_sched = 1;
		}
		spin_unlock_irqrestore(&dmc->dirty_set_lru_lock, flags);
	}

	return 0;
}

static void eio_sysctl_register_writeback(struct cache_c *dmc);
static void eio_sysctl_unregister_writeback(struct cache_c *dmc);
static void eio_sysctl_register_invalidate(struct cache_c *dmc);
static void eio_sysctl_unregister_invalidate(struct cache_c *dmc);

/*
 * eio_control_sysctl
 */
int
eio_control_sysctl(ctl_table *table, int write, void __user *buffer,
		   size_t *length, loff_t *ppos)
{
	int rv = 0;
	struct cache_c *dmc = (struct cache_c *)table->extra1;
	unsigned long flags = 0;

	/* fetch the new tunable value */

	if (!write) {
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->sysctl_pending.control = dmc->sysctl_active.control;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	}

	proc_dointvec(table, write, buffer, length, ppos);

	/* do write processing */

	if (write) {
		/* do sanity check */

		if (dmc->sysctl_pending.control > CACHE_CONTROL_FLAG_MAX ||
		    dmc->sysctl_pending.control < 0) {
			/* valid values are from 0 till CACHE_CONTROL_FLAG_MAX */
			pr_err("control valid values are from 0 till %d",
			       CACHE_CONTROL_FLAG_MAX);
			return -EINVAL;
		}

		if (dmc->sysctl_pending.control == dmc->sysctl_active.control)
			/* new is same as old value. No work required */
			return 0;

		/* update the active value with the new tunable value */
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->sysctl_active.control = dmc->sysctl_pending.control;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);

		/* apply the new tunable value */

		switch (dmc->sysctl_active.control) {
		case CACHE_VERBOSE_OFF:
			spin_lock_irqsave(&dmc->cache_spin_lock,
					  dmc->cache_spin_lock_flags);
			dmc->cache_flags &= ~CACHE_FLAGS_VERBOSE;
			spin_unlock_irqrestore(&dmc->cache_spin_lock,
					       dmc->cache_spin_lock_flags);
			pr_info("Turning off verbose mode");
			break;
		case CACHE_VERBOSE_ON:
			spin_lock_irqsave(&dmc->cache_spin_lock,
					  dmc->cache_spin_lock_flags);
			dmc->cache_flags |= CACHE_FLAGS_VERBOSE;
			spin_unlock_irqrestore(&dmc->cache_spin_lock,
					       dmc->cache_spin_lock_flags);
			pr_info("Turning on verbose mode");
			break;
		case CACHE_WRITEBACK_ON:
			if (dmc->sysctl_handle_writeback == NULL)
				eio_sysctl_register_writeback(dmc);
			break;
		case CACHE_WRITEBACK_OFF:
			if (dmc->sysctl_handle_writeback)
				eio_sysctl_unregister_writeback(dmc);
			break;
		case CACHE_INVALIDATE_ON:
			if (dmc->sysctl_handle_invalidate == NULL) {
				eio_sysctl_register_invalidate(dmc);
				spin_lock_irqsave(&dmc->cache_spin_lock,
						  dmc->cache_spin_lock_flags);
				dmc->cache_flags |= CACHE_FLAGS_INVALIDATE;
				spin_unlock_irqrestore(&dmc->cache_spin_lock,
						       dmc->
						       cache_spin_lock_flags);
			} else
				pr_info("Invalidate API already registered");
			break;
		case CACHE_INVALIDATE_OFF:
			if (dmc->sysctl_handle_invalidate) {
				eio_sysctl_unregister_invalidate(dmc);
				spin_lock_irqsave(&dmc->cache_spin_lock,
						  dmc->cache_spin_lock_flags);
				dmc->cache_flags &= ~CACHE_FLAGS_INVALIDATE;
				spin_unlock_irqrestore(&dmc->cache_spin_lock,
						       dmc->
						       cache_spin_lock_flags);
			} else
				pr_info("Invalidate API not registered");
			break;
		case CACHE_FAST_REMOVE_ON:
			if (dmc->mode != CACHE_MODE_WB) {
				spin_lock_irqsave(&dmc->cache_spin_lock,
						  dmc->cache_spin_lock_flags);
				dmc->cache_flags |= CACHE_FLAGS_FAST_REMOVE;
				spin_unlock_irqrestore(&dmc->cache_spin_lock,
						       dmc->
						       cache_spin_lock_flags);
				if (CACHE_VERBOSE_IS_SET(dmc))
					pr_info("Turning on fast remove");
			} else {
#ifdef EIO_DEBUG
				spin_lock_irqsave(&dmc->cache_spin_lock,
						  dmc->cache_spin_lock_flags);
				dmc->cache_flags |= CACHE_FLAGS_FAST_REMOVE;
				spin_unlock_irqrestore(&dmc->cache_spin_lock,
						       dmc->
						       cache_spin_lock_flags);
				if (CACHE_VERBOSE_IS_SET(dmc))
					pr_info("Turning on fast remove");
#else
				pr_err("Invalid control value: 0x%x",
				       dmc->sysctl_active.control);
				rv = -1;
#endif                          /* EIO_DEBUG */
			}
			break;
		case CACHE_FAST_REMOVE_OFF:
			if (dmc->mode != CACHE_MODE_WB) {
				spin_lock_irqsave(&dmc->cache_spin_lock,
						  dmc->cache_spin_lock_flags);
				dmc->cache_flags &= ~CACHE_FLAGS_FAST_REMOVE;
				spin_unlock_irqrestore(&dmc->cache_spin_lock,
						       dmc->
						       cache_spin_lock_flags);
				if (CACHE_VERBOSE_IS_SET(dmc))
					pr_info("Turning off fast remove");
			} else {
#ifdef EIO_DEBUG
				spin_lock_irqsave(&dmc->cache_spin_lock,
						  dmc->cache_spin_lock_flags);
				dmc->cache_flags &= ~CACHE_FLAGS_FAST_REMOVE;
				spin_unlock_irqrestore(&dmc->cache_spin_lock,
						       dmc->
						       cache_spin_lock_flags);
				if (CACHE_VERBOSE_IS_SET(dmc))
					pr_info("Turning off fast remove");
#else
				pr_err("Invalid control value: 0x%x",
				       dmc->sysctl_active.control);
				rv = -1;
#endif                          /* EIO_DEBUG */
			}
			break;
		default:
			pr_err("Invalid control value: 0x%x",
			       dmc->sysctl_active.control);
			rv = -1;
		}
	}

	return rv;
}

#define PROC_STR                "enhanceio"
#define PROC_VER_STR            "enhanceio/version"
#define PROC_STATS              "stats"
#define PROC_ERRORS             "errors"
#define PROC_IOSZ_HIST          "io_hist"
#define PROC_CONFIG             "config"

static int eio_invalidate_sysctl(ctl_table *table, int write,
				 void __user *buffer, size_t *length,
				 loff_t *ppos);
static void *eio_find_sysctl_data(struct cache_c *dmc, ctl_table *vars);
static char *eio_cons_sysctl_devname(struct cache_c *dmc);
static char *eio_cons_procfs_cachename(struct cache_c *dmc,
				       char *path_component);
static void eio_sysctl_register_common(struct cache_c *dmc);
static void eio_sysctl_unregister_common(struct cache_c *dmc);
static void eio_sysctl_register_dir(void);
static void eio_sysctl_unregister_dir(void);
static int eio_stats_show(struct seq_file *seq, void *v);
static int eio_stats_open(struct inode *inode, struct file *file);
static int eio_errors_show(struct seq_file *seq, void *v);
static int eio_errors_open(struct inode *inode, struct file *file);
static int eio_iosize_hist_show(struct seq_file *seq, void *v);
static int eio_iosize_hist_open(struct inode *inode, struct file *file);
static int eio_version_show(struct seq_file *seq, void *v);
static int eio_version_open(struct inode *inode, struct file *file);
static int eio_config_show(struct seq_file *seq, void *v);
static int eio_config_open(struct inode *inode, struct file *file);

static struct file_operations eio_version_operations = {
	.open		= eio_version_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static struct file_operations eio_stats_operations = {
	.open		= eio_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static struct file_operations eio_errors_operations = {
	.open		= eio_errors_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static struct file_operations eio_iosize_hist_operations = {
	.open		= eio_iosize_hist_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static struct file_operations eio_config_operations = {
	.open		= eio_config_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/*
 * Each ctl_table array needs to be 1 more than the actual number of
 * entries - zero padded at the end ! Therefore the NUM_*_SYSCTLS
 * is 1 more than then number of sysctls.
 */

#define PROC_SYS_ROOT_NAME              "dev"
#define PROC_SYS_DIR_NAME               "enhanceio"
#define PROC_SYS_CACHE_NAME             "enhanceio-dev"

/*
 * The purpose of sysctl_table_dir is to create the "enhanceio"
 * dir under /proc/sys/dev/. The creation is done during module
 * load time and the dir is removed when module is removed.
 *
 * This was added because otherwise, the first cache instance
 * falsely assumes that /proc/sys/kernel/ is its parent instead
 * of /proc/sys/dev leading to an incorrect number of reference
 * count. When you have multiple cache instances, removing the
 * last one results in the kernel's reference count to be 0
 * leading to a kernel warning at runtime.  Hopefully, this will
 * be fixed in the kernel sometime.
 */
static struct sysctl_table_dir {
	struct ctl_table_header *sysctl_header;
	ctl_table vars[0 + 1];
	ctl_table dev[0 + 1];
	ctl_table dir[1 + 1];
	ctl_table root[1 + 1];
} sysctl_template_dir = {
	.vars = {
	}, .dev	= {
	}, .dir	= {
		{
			.procname = PROC_SYS_DIR_NAME,
			.maxlen = 0,
			.mode =	S_IRUGO | S_IXUGO,
			.child = sysctl_template_dir.dev,
		},
	}, .root = {
		{
			.procname = PROC_SYS_ROOT_NAME,
			.maxlen = 0,
			.mode =	0555,
			.child = sysctl_template_dir.dir,
		},
	},
};

#define NUM_COMMON_SYSCTLS      3

static struct sysctl_table_common {
	struct ctl_table_header *sysctl_header;
	ctl_table vars[NUM_COMMON_SYSCTLS + 1];
	ctl_table dev[1 + 1];
	ctl_table dir[1 + 1];
	ctl_table root[1 + 1];
} sysctl_template_common = {
	.vars = {
		{               /* 1 */
			.procname = "zero_stats",
			.maxlen = sizeof(int),
			.mode = 0644,
			.proc_handler = &eio_zerostats_sysctl,
		}, {            /* 2 */
			.procname = "mem_limit_pct",
			.maxlen = sizeof(int), .mode = 0644,
			.proc_handler = &eio_mem_limit_pct_sysctl,
		}, {            /* 3 */
			.procname = "control",
			.maxlen = sizeof(int),
			.mode = 0644,
			.proc_handler = &eio_control_sysctl,
		},
	}, .dev = {
		{
			.procname = PROC_SYS_CACHE_NAME,
			.maxlen = 0,
			.mode =	S_IRUGO | S_IXUGO,
			.child = sysctl_template_common.vars,
		},
	}, .dir = {
		{
			.procname = PROC_SYS_DIR_NAME,
			.maxlen = 0,
			.mode = S_IRUGO | S_IXUGO,
			.child = sysctl_template_common.dev,
		},
	}, .root = {
		{
			.procname = PROC_SYS_ROOT_NAME,
			.maxlen = 0,
			.mode = 0555,
			.child = sysctl_template_common.dir,
		},
	},
};

#define NUM_WRITEBACK_SYSCTLS   7

static struct sysctl_table_writeback {
	struct ctl_table_header *sysctl_header;
	ctl_table vars[NUM_WRITEBACK_SYSCTLS + 1];
	ctl_table dev[1 + 1];
	ctl_table dir[1 + 1];
	ctl_table root[1 + 1];
} sysctl_template_writeback = {
	.vars = {
		{               /* 1 */
			.procname = "do_clean",
			.maxlen = sizeof(int),
			.mode = 0644,
			.proc_handler = &eio_clean_sysctl,
		}, {            /* 2 */
			.procname = "time_based_clean_interval",
			.maxlen = sizeof(unsigned int),
			.mode = 0644,
			.proc_handler = &eio_time_based_clean_interval_sysctl,
		}, {            /* 3 */
			.procname = "autoclean_threshold",
			.maxlen = sizeof(int),
			.mode = 0644,
			.proc_handler = &eio_autoclean_threshold_sysctl,
		}, {            /* 4 */
			.procname = "dirty_high_threshold",
			.maxlen = sizeof(uint32_t),
			.mode = 0644,
			.proc_handler = &eio_dirty_high_threshold_sysctl,
		}
		, {             /* 5 */
			.procname = "dirty_low_threshold",
			.maxlen = sizeof(uint32_t),
			.mode = 0644,
			.proc_handler = &eio_dirty_low_threshold_sysctl,
		}
		, {             /* 6 */
			.procname = "dirty_set_high_threshold",
			.maxlen = sizeof(uint32_t),
			.mode = 0644,
			.proc_handler = &eio_dirty_set_high_threshold_sysctl,
		}
		, {             /* 7 */
			.procname = "dirty_set_low_threshold",
			.maxlen = sizeof(uint32_t),
			.mode = 0644,
			.proc_handler = &eio_dirty_set_low_threshold_sysctl,
		}
		,
	}
	, .dev = {
		{
			.procname = PROC_SYS_CACHE_NAME, .maxlen = 0, .mode =
				S_IRUGO | S_IXUGO, .child =
				sysctl_template_writeback.vars,
		}
		,
	}
	, .dir = {
		{
			.procname = PROC_SYS_DIR_NAME, .maxlen = 0, .mode =
				S_IRUGO | S_IXUGO, .child =
				sysctl_template_writeback.dev,
		}
		,
	}
	, .root = {
		{
			.procname = PROC_SYS_ROOT_NAME, .maxlen = 0, .mode =
				0555, .child = sysctl_template_writeback.dir,
		}
		,
	}
	,
};

#define NUM_INVALIDATE_SYSCTLS          (1)
static struct sysctl_table_invalidate {
	struct ctl_table_header *sysctl_header;
	ctl_table vars[NUM_INVALIDATE_SYSCTLS + 1];
	ctl_table dev[1 + 1];
	ctl_table dir[1 + 1];
	ctl_table root[1 + 1];
} sysctl_template_invalidate = {
	.vars = {
		{        /* 1 */
			.procname = "invalidate",
			.maxlen = sizeof(u_int64_t), 
			.mode = 0644,
			.proc_handler = &eio_invalidate_sysctl,
		}
		,
	}
	, .dev = {
		{
			.procname = PROC_SYS_CACHE_NAME, .maxlen = 0, .mode =
				S_IRUGO | S_IXUGO, .child =
				sysctl_template_invalidate.vars,
		}
		,
	}
	, .dir = {
		{
			.procname = PROC_SYS_DIR_NAME, .maxlen = 0, .mode =
				S_IRUGO | S_IXUGO, .child =
				sysctl_template_invalidate.dev,
		}
		,
	}
	, .root = {
		{
			.procname = PROC_SYS_ROOT_NAME,
			.maxlen = 0,
			.mode = 0555,
			.child = sysctl_template_invalidate.dir,
		}
		,
	}
	,
};

/*
 * eio_module_procfs_init -- called from "eio_init()"
 */
void eio_module_procfs_init(void)
{
	struct proc_dir_entry *entry;

	if (proc_mkdir(PROC_STR, NULL)) {
		entry = create_proc_entry(PROC_VER_STR, 0, NULL);
		if (entry)
			entry->proc_fops = &eio_version_operations;
	}
	eio_sysctl_register_dir();
}

/*
 * eio_module_procfs_exit -- called from "eio_exit()"
 */
void eio_module_procfs_exit(void)
{
	(void)remove_proc_entry(PROC_VER_STR, NULL);
	(void)remove_proc_entry(PROC_STR, NULL);

	eio_sysctl_unregister_dir();
}

/*
 * eio_procfs_ctr -- called from "eio_ctr()"
 */
void eio_procfs_ctr(struct cache_c *dmc)
{
	char *s;
	struct proc_dir_entry *entry;

	s = eio_cons_procfs_cachename(dmc, "");
	entry = proc_mkdir(s, NULL);
	kfree(s);
	if (entry == NULL) {
		pr_err("Failed to create /proc/%s", s);
		return;
	}

	s = eio_cons_procfs_cachename(dmc, PROC_STATS);
	entry = create_proc_entry(s, 0, NULL);
	if (entry) {
		entry->proc_fops = &eio_stats_operations;
		entry->data = dmc;
	}
	kfree(s);

	s = eio_cons_procfs_cachename(dmc, PROC_ERRORS);
	entry = create_proc_entry(s, 0, NULL);
	if (entry) {
		entry->proc_fops = &eio_errors_operations;
		entry->data = dmc;
	}
	kfree(s);

	s = eio_cons_procfs_cachename(dmc, PROC_IOSZ_HIST);
	entry = create_proc_entry(s, 0, NULL);
	if (entry) {
		entry->proc_fops = &eio_iosize_hist_operations;
		entry->data = dmc;
	}
	kfree(s);

	s = eio_cons_procfs_cachename(dmc, PROC_CONFIG);
	entry = create_proc_entry(s, 0, NULL);
	if (entry) {
		entry->proc_fops = &eio_config_operations;
		entry->data = dmc;
	}
	kfree(s);

	eio_sysctl_register_common(dmc);
	if (dmc->mode == CACHE_MODE_WB)
		eio_sysctl_register_writeback(dmc);
	if (CACHE_INVALIDATE_IS_SET(dmc))
		eio_sysctl_register_invalidate(dmc);
}

/*
 * eio_procfs_dtr -- called from "eio_dtr()"
 */
void eio_procfs_dtr(struct cache_c *dmc)
{
	char *s;

	s = eio_cons_procfs_cachename(dmc, PROC_STATS);
	remove_proc_entry(s, NULL);
	kfree(s);

	s = eio_cons_procfs_cachename(dmc, PROC_ERRORS);
	remove_proc_entry(s, NULL);
	kfree(s);

	s = eio_cons_procfs_cachename(dmc, PROC_IOSZ_HIST);
	remove_proc_entry(s, NULL);
	kfree(s);

	s = eio_cons_procfs_cachename(dmc, PROC_CONFIG);
	remove_proc_entry(s, NULL);
	kfree(s);

	s = eio_cons_procfs_cachename(dmc, "");
	remove_proc_entry(s, NULL);
	kfree(s);

	if (dmc->sysctl_handle_invalidate)
		eio_sysctl_unregister_invalidate(dmc);
	if (dmc->sysctl_handle_writeback)
		eio_sysctl_unregister_writeback(dmc);
	eio_sysctl_unregister_common(dmc);
}

static spinlock_t invalidate_spin_lock;

/*
 * eio_invalidate_sysctl
 */
static int
eio_invalidate_sysctl(ctl_table *table, int write, void __user *buffer,
		      size_t *length, loff_t *ppos)
{
	static int have_sector;
	static u_int64_t sector;
	static u_int64_t num_sectors;
	int rv;
	unsigned long int flags;
	struct cache_c *dmc;

	spin_lock_irqsave(&invalidate_spin_lock, flags);

	dmc = (struct cache_c *)table->extra1;
	if (dmc == NULL) {
		pr_err
			("Cannot invalidate due to unexpected NULL cache pointer");
		spin_unlock_irqrestore(&invalidate_spin_lock, flags);
		return -EBUSY;
	}

	table->extra1 = NULL;
	proc_doulongvec_minmax(table, write, buffer, length, ppos);
	table->extra1 = dmc;

	spin_unlock_irqrestore(&invalidate_spin_lock, flags);

	rv = 0;

	if (write) {
		/* TBD. Need to put appropriate sanity checks */

		/* update the active value with the new tunable value */
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
		dmc->sysctl_active.invalidate = dmc->sysctl_pending.invalidate;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);

		/* apply the new tunable value */

		if (have_sector) {
			num_sectors = dmc->sysctl_active.invalidate;

			rv = eio_invalidate_sanity_check(dmc, sector,
							 &num_sectors);

			/* Invalidate only if sanity passes and reset the return value. */
			if (rv == 0)
				eio_inval_range(dmc, sector,
						(unsigned)
						to_bytes(num_sectors));

			rv = 0;
			have_sector = 0;

		} else {
			sector = dmc->sysctl_active.invalidate;
			have_sector = 1;
			num_sectors = 0;
		}
	}

	if (CACHE_VERBOSE_IS_SET(dmc) && num_sectors) {
		pr_info
			("eio_inval_range: Invalidated sector range from sector=%lu to sector=%lu",
			(long unsigned int)sector, (long unsigned int)num_sectors);
	}

	return rv;
}

/*
 * eio_find_sysctl_data
 */
static void *eio_find_sysctl_data(struct cache_c *dmc, ctl_table *vars)
{

	if (strcmp(vars->procname, "do_clean") == 0)
		return (void *)&dmc->sysctl_pending.do_clean;
	if (strcmp(vars->procname, "time_based_clean_interval") == 0)
		return (void *)&dmc->sysctl_pending.time_based_clean_interval;
	if (strcmp(vars->procname, "dirty_high_threshold") == 0)
		return (void *)&dmc->sysctl_pending.dirty_high_threshold;
	if (strcmp(vars->procname, "dirty_low_threshold") == 0)
		return (void *)&dmc->sysctl_pending.dirty_low_threshold;
	if (strcmp(vars->procname, "dirty_set_high_threshold") == 0)
		return (void *)&dmc->sysctl_pending.dirty_set_high_threshold;
	if (strcmp(vars->procname, "dirty_set_low_threshold") == 0)
		return (void *)&dmc->sysctl_pending.dirty_set_low_threshold;
	if (strcmp(vars->procname, "autoclean_threshold") == 0)
		return (void *)&dmc->sysctl_pending.autoclean_threshold;
	if (strcmp(vars->procname, "zero_stats") == 0)
		return (void *)&dmc->sysctl_pending.zerostats;
	if (strcmp(vars->procname, "mem_limit_pct") == 0)
		return (void *)&dmc->sysctl_pending.mem_limit_pct;
	if (strcmp(vars->procname, "control") == 0)
		return (void *)&dmc->sysctl_pending.control;
	if (strcmp(vars->procname, "invalidate") == 0)
		return (void *)&dmc->sysctl_pending.invalidate;

	pr_err("Cannot find sysctl data for %s", vars->procname);
	return NULL;
}

/*
 * eio_cons_sysctl_devname
 */
static char *eio_cons_sysctl_devname(struct cache_c *dmc)
{
	char *pathname;

	if (dmc->cache_name[0]) {
		pathname = kzalloc(strlen(dmc->cache_name) + 1, GFP_KERNEL);
		if (pathname)
			strcpy(pathname, dmc->cache_name);
		else
			pr_err("Failed to allocate memory");
	} else {
		pr_err("Cache name is NULL");
		pathname = NULL;
	}

	return pathname;
}

/*
 * eio_cons_procfs_cachename
 */
static char *eio_cons_procfs_cachename(struct cache_c *dmc,
				       char *path_component)
{
	char *pathname;

	if (dmc->cache_name[0]) {
		pathname =
			kzalloc(strlen(PROC_SYS_DIR_NAME) + 1 +
				strlen(dmc->cache_name) + 1 +
				strlen(path_component) + 1, GFP_KERNEL);
		if (pathname) {
			strcpy(pathname, PROC_SYS_DIR_NAME);
			strcat(pathname, "/");
			strcat(pathname, dmc->cache_name);
			if (strcmp(path_component, "") != 0) {
				strcat(pathname, "/");
				strcat(pathname, path_component);
			}
		} else
			pr_err("Failed to allocate memory");
	} else {
		pr_err("Cache name is NULL");
		pathname = NULL;
	}

	return pathname;
}

static void eio_sysctl_register_dir(void)
{
	struct sysctl_table_dir *dir;

	dir =
		kmemdup(&sysctl_template_dir, sizeof sysctl_template_dir,
			GFP_KERNEL);
	if (unlikely(dir == NULL)) {
		pr_err("Failed to allocate memory for dir sysctl");
		return;
	}

	dir->dir[0].child = dir->dev;
	dir->root[0].child = dir->dir;
	dir->sysctl_header = register_sysctl_table(dir->root);
	if (unlikely(dir->sysctl_header == NULL)) {
		pr_err("Failed to register dir sysctl");
		goto out;
	}

	sysctl_handle_dir = dir;
	return;
out:
	kfree(dir);
}

static void eio_sysctl_unregister_dir(void)
{
	if (sysctl_handle_dir != NULL) {
		unregister_sysctl_table(sysctl_handle_dir->sysctl_header);
		kfree(sysctl_handle_dir);
		sysctl_handle_dir = NULL;
	}
}

/*
 * eio_sysctl_register_common
 */
static void eio_sysctl_register_common(struct cache_c *dmc)
{
	unsigned int i;
	struct sysctl_table_common *common;

	common =
		kmemdup(&sysctl_template_common, sizeof sysctl_template_common,
			GFP_KERNEL);
	if (common == NULL) {
		pr_err("Failed to allocate memory for common sysctl");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(common->vars) - 1; i++) {
		common->vars[i].data =
			eio_find_sysctl_data(dmc, &common->vars[i]);
		common->vars[i].extra1 = dmc;
	}

	common->dev[0].procname = eio_cons_sysctl_devname(dmc);
	common->dev[0].child = common->vars;
	common->dir[0].child = common->dev;
	common->root[0].child = common->dir;
	common->sysctl_header = register_sysctl_table(common->root);
	if (common->sysctl_header == NULL) {
		pr_err("Failed to register common sysctl");
		goto out;
	}

	dmc->sysctl_handle_common = common;
	return;
out:
	kfree(common->dev[0].procname);
	kfree(common);
}

/*
 * eio_sysctl_unregister_common
 */
static void eio_sysctl_unregister_common(struct cache_c *dmc)
{
	struct sysctl_table_common *common;

	common = dmc->sysctl_handle_common;
	if (common != NULL) {
		dmc->sysctl_handle_common = NULL;
		unregister_sysctl_table(common->sysctl_header);
		kfree(common->dev[0].procname);
		kfree(common);
	}
}

/*
 * eio_sysctl_register_writeback
 */
static void eio_sysctl_register_writeback(struct cache_c *dmc)
{
	unsigned int i;
	struct sysctl_table_writeback *writeback;

	writeback =
		kmemdup(&sysctl_template_writeback,
			sizeof sysctl_template_writeback, GFP_KERNEL);
	if (writeback == NULL) {
		pr_err("Failed to allocate memory for writeback sysctl");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(writeback->vars) - 1; i++) {
		writeback->vars[i].data =
			eio_find_sysctl_data(dmc, &writeback->vars[i]);
		writeback->vars[i].extra1 = dmc;
	}

	writeback->dev[0].procname = eio_cons_sysctl_devname(dmc);
	writeback->dev[0].child = writeback->vars;
	writeback->dir[0].child = writeback->dev;
	writeback->root[0].child = writeback->dir;
	writeback->sysctl_header = register_sysctl_table(writeback->root);
	if (writeback->sysctl_header == NULL) {
		pr_err("Failed to register writeback sysctl");
		goto out;
	}

	dmc->sysctl_handle_writeback = writeback;
	return;
out:
	kfree(writeback->dev[0].procname);
	kfree(writeback);
}

/*
 * eio_sysctl_unregister_writeback
 */
static void eio_sysctl_unregister_writeback(struct cache_c *dmc)
{
	struct sysctl_table_writeback *writeback;

	writeback = dmc->sysctl_handle_writeback;
	if (writeback != NULL) {
		dmc->sysctl_handle_writeback = NULL;
		unregister_sysctl_table(writeback->sysctl_header);
		kfree(writeback->dev[0].procname);
		kfree(writeback);
	}
}

/*
 * eio_sysctl_register_invalidate
 */
static void eio_sysctl_register_invalidate(struct cache_c *dmc)
{
	unsigned int i;
	struct sysctl_table_invalidate *invalidate;

	invalidate =
		kmemdup(&sysctl_template_invalidate,
			sizeof sysctl_template_invalidate, GFP_KERNEL);
	if (invalidate == NULL) {
		pr_err("Failed to allocate memory for invalidate sysctl");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(invalidate->vars) - 1; i++) {
		invalidate->vars[i].data =
			eio_find_sysctl_data(dmc, &invalidate->vars[i]);
		invalidate->vars[i].extra1 = dmc;
	}

	invalidate->dev[0].procname = eio_cons_sysctl_devname(dmc);
	invalidate->dev[0].child = invalidate->vars;
	invalidate->dir[0].child = invalidate->dev;
	invalidate->root[0].child = invalidate->dir;
	invalidate->sysctl_header = register_sysctl_table(invalidate->root);
	if (invalidate->sysctl_header == NULL) {
		pr_err("Failed to register invalidate sysctl");
		goto out;
	}

	dmc->sysctl_handle_invalidate = invalidate;
	spin_lock_init(&invalidate_spin_lock);
	return;
out:
	kfree(invalidate->dev[0].procname);
	kfree(invalidate);
}

/*
 * eio_sysctl_unregister_invalidate
 */
static void eio_sysctl_unregister_invalidate(struct cache_c *dmc)
{
	struct sysctl_table_invalidate *invalidate;

	invalidate = dmc->sysctl_handle_invalidate;
	if (invalidate != NULL) {
		dmc->sysctl_handle_invalidate = NULL;
		unregister_sysctl_table(invalidate->sysctl_header);
		kfree(invalidate->dev[0].procname);
		kfree(invalidate);
	}
}

/*
 * eio_stats_show
 */
static int eio_stats_show(struct seq_file *seq, void *v)
{
	struct cache_c *dmc = seq->private;
	struct eio_stats *stats = &dmc->eio_stats;
	int read_hit_pct, write_hit_pct, dirty_write_hit_pct;

	if (atomic64_read(&stats->reads) > 0)
		read_hit_pct =
			EIO_DIV((atomic64_read(&stats->read_hits) * 100LL),
			atomic64_read(&stats->reads));
	else
		read_hit_pct = 0;

	if (atomic64_read(&stats->writes) > 0) {
		write_hit_pct =
			EIO_DIV((atomic64_read(&stats->write_hits) * 100LL),
			atomic64_read(&stats->writes));
		dirty_write_hit_pct =
			EIO_DIV((atomic64_read(&stats->dirty_write_hits) * 100),
			atomic64_read(&stats->writes));
	} else {
		write_hit_pct = 0;
		dirty_write_hit_pct = 0;
	}

	seq_printf(seq, "%-26s %12lld\n", "reads",
		   (int64_t)atomic64_read(&stats->reads));
	seq_printf(seq, "%-26s %12lld\n", "writes",
		   (int64_t)atomic64_read(&stats->writes));

	seq_printf(seq, "%-26s %12lld\n", "read_hits",
		   (int64_t)atomic64_read(&stats->read_hits));
	seq_printf(seq, "%-26s %12d\n", "read_hit_pct", read_hit_pct);

	seq_printf(seq, "%-26s %12lld\n", "write_hits",
		   (int64_t)atomic64_read(&stats->write_hits));
	seq_printf(seq, "%-26s %12u\n", "write_hit_pct", write_hit_pct);

	seq_printf(seq, "%-26s %12lld\n", "dirty_write_hits",
		   (int64_t)atomic64_read(&stats->dirty_write_hits));
	seq_printf(seq, "%-26s %12d\n", "dirty_write_hit_pct",
		   dirty_write_hit_pct);

	if ((int64_t)(atomic64_read(&stats->cached_blocks)) < 0)
		atomic64_set(&stats->cached_blocks, 0);
	seq_printf(seq, "%-26s %12lld\n", "cached_blocks",
		   (int64_t)atomic64_read(&stats->cached_blocks));

	seq_printf(seq, "%-26s %12lld\n", "rd_replace",
		   (int64_t)atomic64_read(&stats->rd_replace));
	seq_printf(seq, "%-26s %12lld\n", "wr_replace",
		   (int64_t)atomic64_read(&stats->wr_replace));

	seq_printf(seq, "%-26s %12lld\n", "noroom",
		   (int64_t)atomic64_read(&stats->noroom));

	seq_printf(seq, "%-26s %12lld\n", "cleanings",
		   (int64_t)atomic64_read(&stats->cleanings));
	seq_printf(seq, "%-26s %12lld\n", "md_write_dirty",
		   (int64_t)atomic64_read(&stats->md_write_dirty));
	seq_printf(seq, "%-26s %12lld\n", "md_write_clean",
		   (int64_t)atomic64_read(&stats->md_write_clean));
	seq_printf(seq, "%-26s %12lld\n", "md_ssd_writes",
		   (int64_t)atomic64_read(&stats->md_ssd_writes));
	seq_printf(seq, "%-26s %12d\n", "do_clean",
		   dmc->sysctl_active.do_clean);
	seq_printf(seq, "%-26s %12lld\n", "nr_blocks", dmc->size);
	seq_printf(seq, "%-26s %12lld\n", "nr_dirty",
		   (int64_t)atomic64_read(&dmc->nr_dirty));
	seq_printf(seq, "%-26s %12u\n", "nr_sets", (uint32_t)dmc->num_sets);
	seq_printf(seq, "%-26s %12d\n", "clean_index",
		   (uint32_t)atomic_read(&dmc->clean_index));

	seq_printf(seq, "%-26s %12lld\n", "uncached_reads",
		   (int64_t)atomic64_read(&stats->uncached_reads));
	seq_printf(seq, "%-26s %12lld\n", "uncached_writes",
		   (int64_t)atomic64_read(&stats->uncached_writes));
	seq_printf(seq, "%-26s %12lld\n", "uncached_map_size",
		   (int64_t)atomic64_read(&stats->uncached_map_size));
	seq_printf(seq, "%-26s %12lld\n", "uncached_map_uncacheable",
		   (int64_t)atomic64_read(&stats->uncached_map_uncacheable));

	seq_printf(seq, "%-26s %12lld\n", "disk_reads",
		   (int64_t)atomic64_read(&stats->disk_reads));
	seq_printf(seq, "%-26s %12lld\n", "disk_writes",
		   (int64_t)atomic64_read(&stats->disk_writes));
	seq_printf(seq, "%-26s %12lld\n", "ssd_reads",
		   (int64_t)atomic64_read(&stats->ssd_reads));
	seq_printf(seq, "%-26s %12lld\n", "ssd_writes",
		   (int64_t)atomic64_read(&stats->ssd_writes));
	seq_printf(seq, "%-26s %12lld\n", "ssd_readfills",
		   (int64_t)atomic64_read(&stats->ssd_readfills));
	seq_printf(seq, "%-26s %12lld\n", "ssd_readfill_unplugs",
		   (int64_t)atomic64_read(&stats->ssd_readfill_unplugs));

	seq_printf(seq, "%-26s %12lld\n", "readdisk",
		   (int64_t)atomic64_read(&stats->readdisk));
	seq_printf(seq, "%-26s %12lld\n", "writedisk",
		   (int64_t)atomic64_read(&stats->readdisk));
	seq_printf(seq, "%-26s %12lld\n", "readcache",
		   (int64_t)atomic64_read(&stats->readcache));
	seq_printf(seq, "%-26s %12lld\n", "readfill",
		   (int64_t)atomic64_read(&stats->readfill));
	seq_printf(seq, "%-26s %12lld\n", "writecache",
		   (int64_t)atomic64_read(&stats->writecache));

	seq_printf(seq, "%-26s %12lld\n", "readcount",
		   (int64_t)atomic64_read(&stats->readcount));
	seq_printf(seq, "%-26s %12lld\n", "writecount",
		   (int64_t)atomic64_read(&stats->writecount));
	seq_printf(seq, "%-26s %12lld\n", "kb_reads",
		   (int64_t)atomic64_read(&stats->reads) / 2);
	seq_printf(seq, "%-26s %12lld\n", "kb_writes",
		   (int64_t)atomic64_read(&stats->writes) / 2);
	seq_printf(seq, "%-26s %12lld\n", "rdtime_ms",
		   (int64_t)atomic64_read(&stats->rdtime_ms));
	seq_printf(seq, "%-26s %12lld\n", "wrtime_ms",
		   (int64_t)atomic64_read(&stats->wrtime_ms));
	return 0;
}

/*
 * eio_stats_open
 */
static int eio_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, &eio_stats_show, PDE(inode)->data);
}

/*
 * eio_errors_show
 */
static int eio_errors_show(struct seq_file *seq, void *v)
{
	struct cache_c *dmc = seq->private;

	seq_printf(seq, "disk_read_errors    %4u\n",
		   dmc->eio_errors.disk_read_errors);
	seq_printf(seq, "disk_write_errors   %4u\n",
		   dmc->eio_errors.disk_write_errors);
	seq_printf(seq, "ssd_read_errors     %4u\n",
		   dmc->eio_errors.ssd_read_errors);
	seq_printf(seq, "ssd_write_errors    %4u\n",
		   dmc->eio_errors.ssd_write_errors);
	seq_printf(seq, "memory_alloc_errors %4u\n",
		   dmc->eio_errors.memory_alloc_errors);
	seq_printf(seq, "no_cache_dev        %4u\n",
		   dmc->eio_errors.no_cache_dev);
	seq_printf(seq, "no_source_dev       %4u\n",
		   dmc->eio_errors.no_source_dev);

	return 0;
}

/*
 * eio_errors_open
 */
static int eio_errors_open(struct inode *inode, struct file *file)
{
	return single_open(file, &eio_errors_show, PDE(inode)->data);
}

/*
 * eio_iosize_hist_show
 */
static int eio_iosize_hist_show(struct seq_file *seq, void *v)
{
	int i;
	struct cache_c *dmc = seq->private;

	for (i = 1; i <= SIZE_HIST - 1; i++) {
		if (atomic64_read(&dmc->size_hist[i]) == 0)
			continue;

		if (i == 1)
			seq_printf(seq, "%u   %12lld\n", i * 512,
				   (int64_t)atomic64_read(&dmc->size_hist[i]));
		else if (i < 20)
			seq_printf(seq, "%u  %12lld\n", i * 512,
				   (int64_t)atomic64_read(&dmc->size_hist[i]));
		else
			seq_printf(seq, "%u %12lld\n", i * 512,
				   (int64_t)atomic64_read(&dmc->size_hist[i]));
	}

	return 0;
}

/*
 * eio_iosize_hist_open
 */
static int eio_iosize_hist_open(struct inode *inode, struct file *file)
{

	return single_open(file, &eio_iosize_hist_show, PDE(inode)->data);
}

/*
 * eio_version_show
 */
static int eio_version_show(struct seq_file *seq, void *v)
{
	char buf[128];

	memset(buf, 0, sizeof buf);
	eio_version_query(sizeof buf, buf);
	seq_printf(seq, "%s\n", buf);

	return 0;
}

/*
 * eio_version_open
 */
static int eio_version_open(struct inode *inode, struct file *file)
{
	return single_open(file, &eio_version_show, PDE(inode)->data);
}

/*
 * eio_config_show
 */
static int eio_config_show(struct seq_file *seq, void *v)
{
	struct cache_c *dmc = seq->private;

	seq_printf(seq, "src_name   %s\n", dmc->disk_devname);
	seq_printf(seq, "ssd_name   %s\n", dmc->cache_devname);
	seq_printf(seq, "src_size   %lu\n", (long unsigned int)dmc->disk_size);
	seq_printf(seq, "ssd_size   %lu\n", (long unsigned int)dmc->size);

	seq_printf(seq, "set_size   %10u\n", dmc->assoc);
	seq_printf(seq, "block_size %10u\n", (dmc->block_size) << SECTOR_SHIFT);
	seq_printf(seq, "mode       %10u\n", dmc->mode);
	seq_printf(seq, "eviction   %10u\n", dmc->req_policy);
	seq_printf(seq, "num_sets   %10u\n", dmc->num_sets);
	seq_printf(seq, "num_blocks %10lu\n", (long unsigned int)dmc->size);
	seq_printf(seq, "metadata        %s\n",
		   CACHE_MD8_IS_SET(dmc) ? "large" : "small");
	seq_printf(seq, "state        %s\n",
		   CACHE_DEGRADED_IS_SET(dmc) ? "degraded"
		   : (CACHE_FAILED_IS_SET(dmc) ? "failed" : "normal"));
	seq_printf(seq, "flags      0x%08x\n", dmc->cache_flags);

	return 0;
}

/*
 * eio_config_open
 */
static int eio_config_open(struct inode *inode, struct file *file)
{

	return single_open(file, &eio_config_show, PDE(inode)->data);
}
