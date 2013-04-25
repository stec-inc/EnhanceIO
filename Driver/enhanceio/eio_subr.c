/*
 *  eio_subr.c
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
#include "eio_ttc.h"

static DEFINE_SPINLOCK(_job_lock);
static LIST_HEAD(_io_jobs);
static LIST_HEAD(_disk_read_jobs);

int eio_io_empty(void)
{

	return list_empty(&_io_jobs);
}

struct kcached_job *eio_alloc_cache_job(void)
{
	struct kcached_job *job;

	job = mempool_alloc(_job_pool, GFP_NOIO);
	if (likely(job))
		atomic_inc(&nr_cache_jobs);
	return job;
}

void eio_free_cache_job(struct kcached_job *job)
{

	mempool_free(job, _job_pool);
	atomic_dec(&nr_cache_jobs);
}

/*
 * Functions to push and pop a job onto the head of a given job list.
 */
static struct kcached_job *eio_pop(struct list_head *jobs)
{
	struct kcached_job *job = NULL;
	unsigned long flags = 0;

	spin_lock_irqsave(&_job_lock, flags);
	if (!list_empty(jobs)) {
		job = list_entry(jobs->next, struct kcached_job, list);
		list_del(&job->list);
	}
	spin_unlock_irqrestore(&_job_lock, flags);
	return job;
}

static void eio_push(struct list_head *jobs, struct kcached_job *job)
{
	unsigned long flags = 0;

	spin_lock_irqsave(&_job_lock, flags);
	list_add_tail(&job->list, jobs);
	spin_unlock_irqrestore(&_job_lock, flags);
}

void eio_push_ssdread_failures(struct kcached_job *job)
{

	eio_push(&_disk_read_jobs, job);
}

static void
eio_process_jobs(struct list_head *jobs, void (*fn) (struct kcached_job *))
{
	struct kcached_job *job;

	while ((job = eio_pop(jobs)) != NULL)
		(void)fn(job);
}

static void eio_process_ssd_rm_list(void)
{
	unsigned long int flags = 0;
	struct ssd_rm_list *ssd_list_ptr;
	extern int ssd_rm_list_not_empty;
	extern spinlock_t ssd_rm_list_lock;
	extern struct list_head ssd_rm_list;

	spin_lock_irqsave(&ssd_rm_list_lock, flags);
	if (likely(list_empty(&ssd_rm_list))) {
		spin_unlock_irqrestore(&ssd_rm_list_lock, flags);
		return;
	}

	while (!list_empty(&ssd_rm_list)) {
		ssd_list_ptr =
		    list_entry(ssd_rm_list.next, struct ssd_rm_list, list);
		if (ssd_list_ptr->action == BUS_NOTIFY_DEL_DEVICE)
			eio_suspend_caching(ssd_list_ptr->dmc,
					    ssd_list_ptr->note);
		else
			pr_err("eio_process_ssd_rm_list:"
			       "Unknown status (0x%x)\n", ssd_list_ptr->action);
		list_del(&ssd_list_ptr->list);
		kfree(ssd_list_ptr);
	}
	ssd_rm_list_not_empty = 0;
	spin_unlock_irqrestore(&ssd_rm_list_lock, flags);
}

/*
 * Entry point of the "events" kernel thread.
 */
void eio_do_work(struct work_struct *unused)
{
	extern int ssd_rm_list_not_empty;

	if (unlikely(ssd_rm_list_not_empty))
		eio_process_ssd_rm_list();
	eio_process_jobs(&_disk_read_jobs, eio_ssderror_diskread);
}

struct kcached_job *eio_new_job(struct cache_c *dmc, struct eio_bio *bio,
				index_t index)
{
	struct kcached_job *job;

	EIO_ASSERT((bio != NULL) || (index != -1));

	job = eio_alloc_cache_job();
	if (unlikely(job == NULL)) {
		spin_lock_irqsave(&dmc->cache_spin_lock,
				  dmc->cache_spin_lock_flags);
		dmc->eio_errors.memory_alloc_errors++;
		spin_unlock_irqrestore(&dmc->cache_spin_lock,
				       dmc->cache_spin_lock_flags);
		return NULL;
	}
	job->dmc = dmc;
	job->index = index;
	job->error = 0;
	job->ebio = bio;
	if (index != -1) {
		job->job_io_regions.cache.bdev = dmc->cache_dev->bdev;
		if (bio) {
			job->job_io_regions.cache.sector =
			    (index << dmc->block_shift) + dmc->md_sectors +
			    (bio->eb_sector -
			     EIO_ROUND_SECTOR(dmc, bio->eb_sector));
			EIO_ASSERT(eio_to_sector(bio->eb_size) <=
				   dmc->block_size);
			job->job_io_regions.cache.count =
			    eio_to_sector(bio->eb_size);
		} else {
			job->job_io_regions.cache.sector =
			    (index << dmc->block_shift) + dmc->md_sectors;
			job->job_io_regions.cache.count = dmc->block_size;
		}
	}

	job->job_io_regions.disk.bdev = dmc->disk_dev->bdev;
	if (bio) {
		job->job_io_regions.disk.sector = bio->eb_sector;
		job->job_io_regions.disk.count = eio_to_sector(bio->eb_size);
	} else {
		job->job_io_regions.disk.sector = EIO_DBN_GET(dmc, index);
		job->job_io_regions.disk.count = dmc->block_size;
	}
	job->next = NULL;
	job->md_sector = NULL;

	return job;
}

int
eio_io_sync_pages(struct cache_c *dmc, struct eio_io_region *where, int rw,
		  struct page **pages, int num_bvecs)
{
	struct eio_io_request req;
	int error;

	req.mtype = EIO_PAGES;
	req.dptr.plist = pages;
	req.num_bvecs = num_bvecs;
	req.notify = NULL;
	req.context = NULL;
	req.hddio = 0;

	if ((unlikely(CACHE_FAILED_IS_SET(dmc)) ||
	     unlikely(CACHE_DEGRADED_IS_SET(dmc))) &&
	    (!CACHE_SSD_ADD_INPROG_IS_SET(dmc)))
		error = -ENODEV;
	else
		error = eio_do_io(dmc, where, rw, &req);

	if (error)
		return error;

	return 0;
}

int
eio_io_sync_vm(struct cache_c *dmc, struct eio_io_region *where, int rw,
	       struct bio_vec *pages, int num_bvecs)
{
	struct eio_io_request req;
	int error;

	memset((char *)&req, 0, sizeof(req));
	/* Fill up the appropriate fields
	 * in eio_io_request */
	req.mtype = EIO_BVECS;
	req.dptr.pages = pages;
	req.num_bvecs = num_bvecs;
	req.notify = NULL;
	req.context = NULL;
	req.hddio = 0;
	if ((unlikely(CACHE_FAILED_IS_SET(dmc)) ||
	     unlikely(CACHE_DEGRADED_IS_SET(dmc))) &&
	    (!CACHE_SSD_ADD_INPROG_IS_SET(dmc)))
		error = -ENODEV;
	else
		error = eio_do_io(dmc, where, rw, &req);
	if (error)
		return error;
	return 0;
}

void eio_unplug_cache_device(struct cache_c *dmc)
{
	struct request_queue *q;
	struct block_device *bdev;

	if (unlikely(CACHE_FAILED_IS_SET(dmc)) ||
	    unlikely(CACHE_DEGRADED_IS_SET(dmc)))
		return;
	bdev = dmc->cache_dev->bdev;
	q = bdev_get_queue(bdev);
}

void eio_unplug_disk_device(struct cache_c *dmc)
{
	struct request_queue *q;
	struct block_device *bdev;

	if (unlikely(CACHE_DEGRADED_IS_SET(dmc)))
		return;
	bdev = dmc->disk_dev->bdev;
	q = bdev_get_queue(bdev);
}

void eio_plug_cache_device(struct cache_c *dmc)
{
	struct block_device *bdev;
	struct request_queue *q;

	if (unlikely(CACHE_FAILED_IS_SET(dmc)) ||
	    unlikely(CACHE_DEGRADED_IS_SET(dmc)))
		return;
	bdev = dmc->cache_dev->bdev;
	q = bdev_get_queue(bdev);
}

void eio_plug_disk_device(struct cache_c *dmc)
{
	struct block_device *bdev;
	struct request_queue *q;

	if (unlikely(CACHE_DEGRADED_IS_SET(dmc)))
		return;
	bdev = dmc->disk_dev->bdev;
	q = bdev_get_queue(bdev);
}

/*
 * For Linux, we do not do a dm_put_device() when the device underneath
 * disappears. The logic to handle the IOs to a missing device is handled
 * by the kernel proper. We will get an IO error if an IO is done on a
 * device that does not exist.
 */
void eio_suspend_caching(struct cache_c *dmc, enum dev_notifier note)
{

	spin_lock_irqsave(&dmc->cache_spin_lock, dmc->cache_spin_lock_flags);
	if (dmc->mode != CACHE_MODE_WB && CACHE_FAILED_IS_SET(dmc)) {
		pr_err("suspend caching: Cache "
		       "%s is already in FAILED state\n", dmc->cache_name);
		spin_unlock_irqrestore(&dmc->cache_spin_lock,
				       dmc->cache_spin_lock_flags);
		return;
	}

	switch (note) {

	case NOTIFY_SRC_REMOVED:
		if (CACHE_DEGRADED_IS_SET(dmc))
			dmc->cache_flags &= ~CACHE_FLAGS_DEGRADED;
		dmc->cache_flags |= CACHE_FLAGS_FAILED;
		dmc->eio_errors.no_source_dev = 1;
		atomic64_set(&dmc->eio_stats.cached_blocks, 0);
		pr_info("suspend_caching: Source Device Removed."
			"Cache \"%s\" is in Failed mode.\n", dmc->cache_name);
		break;
	case NOTIFY_SSD_REMOVED:
		if (dmc->mode == CACHE_MODE_WB) {
			/*
			 * For writeback
			 * - Cache should never be in degraded mode
			 * - ssd removal should result in FAILED state
			 * - the cached block should not be reset.
			 */
			EIO_ASSERT(!CACHE_DEGRADED_IS_SET(dmc));
			dmc->cache_flags |= CACHE_FLAGS_FAILED;
			pr_info("suspend caching: SSD Device Removed.\
				 Cache \"%s\" is in Failed mode.\n", dmc->cache_name);
		} else {
			if (CACHE_DEGRADED_IS_SET(dmc) ||
			    CACHE_SSD_ADD_INPROG_IS_SET(dmc)) {
				spin_unlock_irqrestore(&dmc->cache_spin_lock,
						       dmc->
						       cache_spin_lock_flags);
				pr_err("suspend_caching: Cache \
				       \"%s\" is either degraded \
				       or device add in progress, exiting.\n", dmc->cache_name);
				return;
			}
			dmc->cache_flags |= CACHE_FLAGS_DEGRADED;
			atomic64_set(&dmc->eio_stats.cached_blocks, 0);
			pr_info("suspend caching: Cache \"%s\" \
				is in Degraded mode.\n", dmc->cache_name);
		}
		dmc->eio_errors.no_cache_dev = 1;
		break;
	default:
		pr_err("suspend_caching: incorrect notify message.\n");
		break;
	}

	spin_unlock_irqrestore(&dmc->cache_spin_lock,
			       dmc->cache_spin_lock_flags);
}

void eio_put_cache_device(struct cache_c *dmc)
{

	eio_ttc_put_device(&dmc->cache_dev);
}

void eio_resume_caching(struct cache_c *dmc, char *dev)
{
	int r;

	if (dmc == NULL || dev == NULL) {
		pr_err("resume_caching: Null device or"
		       "cache instance when resuming caching.\n");
		return;
	}
	if (strlen(dev) >= DEV_PATHLEN) {
		pr_err("resume_caching: Device name %s too long.\n", dev);
		return;
	}

	spin_lock_irqsave(&dmc->cache_spin_lock, dmc->cache_spin_lock_flags);
	if (CACHE_STALE_IS_SET(dmc)) {
		pr_err("eio_resume_caching: Hard Failure Detected!!"
		       "Cache \"%s\" can not be resumed.", dmc->cache_name);
		spin_unlock_irqrestore(&dmc->cache_spin_lock,
				       dmc->cache_spin_lock_flags);
		return;
	}

	/* sanity check for writeback */
	if (dmc->mode == CACHE_MODE_WB) {
		if (!CACHE_FAILED_IS_SET(dmc) || CACHE_SRC_IS_ABSENT(dmc) ||
		    CACHE_SSD_ADD_INPROG_IS_SET(dmc)) {
			pr_debug("eio_resume_caching: Cache not in Failed "
				 "state or Source is absent"
				 "or SSD add already in progress for cache \"%s\".\n",
				 dmc->cache_name);
			spin_unlock_irqrestore(&dmc->cache_spin_lock,
					       dmc->cache_spin_lock_flags);
			return;
		}
	} else {
		/* sanity check for WT or RO cache. */
		if (CACHE_FAILED_IS_SET(dmc) || !CACHE_DEGRADED_IS_SET(dmc) ||
		    CACHE_SSD_ADD_INPROG_IS_SET(dmc)) {
			pr_err("resume_caching: Cache \"%s\" "
			       "is either in failed mode or "
			       "cache device add in progress, ignoring.\n ",
			       dmc->cache_name);
			spin_unlock_irqrestore(&dmc->cache_spin_lock,
					       dmc->cache_spin_lock_flags);
			return;
		}
	}

	dmc->cache_flags |= CACHE_FLAGS_SSD_ADD_INPROG;
	spin_unlock_irqrestore(&dmc->cache_spin_lock,
			       dmc->cache_spin_lock_flags);
	r = eio_ctr_ssd_add(dmc, dev);
	if (r) {
		/* error */
		pr_debug(" resume caching: returned error: %d\n ", r);
		spin_lock_irqsave(&dmc->cache_spin_lock,
				  dmc->cache_spin_lock_flags);
		dmc->cache_flags &= ~CACHE_FLAGS_SSD_ADD_INPROG;
		spin_unlock_irqrestore(&dmc->cache_spin_lock,
				       dmc->cache_spin_lock_flags);
		return;
	}

	spin_lock_irqsave(&dmc->cache_spin_lock, dmc->cache_spin_lock_flags);
	dmc->eio_errors.no_cache_dev = 0;
	if (dmc->mode != CACHE_MODE_WB)
		dmc->cache_flags &= ~CACHE_FLAGS_DEGRADED;
	else
		dmc->cache_flags &= ~CACHE_FLAGS_FAILED;
	dmc->cache_flags &= ~CACHE_FLAGS_SSD_ADD_INPROG;
	spin_unlock_irqrestore(&dmc->cache_spin_lock,
			       dmc->cache_spin_lock_flags);
	pr_info(" resume_caching:cache %s is restored to ACTIVE mode.\n",
		dmc->cache_name);
}
