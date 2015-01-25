/*
 *  eio_main.c
 *
 *  Copyright (C) 2012 STEC, Inc. All rights not specifically granted
 *   under a license included herein are reserved
 *  Made EnhanceIO specific changes.
 *   Saied Kazemi <skazemi@stec-inc.com>
 *   Siddharth Choudhuri <schoudhuri@stec-inc.com>
 *  Amit Kale <akale@stec-inc.com>
 *   Restructured much of the io code to split bio within map function instead
 *   of letting dm do it.
 *   Simplified queued logic for write through.
 *   Created per-cache spinlocks for reducing contention in IO codepath.
 *  Amit Kale <akale@stec-inc.com>
 *  Harish Pujari <hpujari@stec-inc.com>
 *   Designed and implemented the writeback caching mode
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

#define CTRACE(X) { }

/*
 * TODO List :
 * 1) sysctls : Create per-cache device sysctls instead of global sysctls.
 * 2) Management of non cache pids : Needs improvement. Remove registration
 * on process exits (with  a pseudo filesstem'ish approach perhaps) ?
 * 3) Breaking up the cache spinlock : Right now contention on the spinlock
 * is not a problem. Might need change in future.
 * 4) Use the standard linked list manipulation macros instead rolling our own.
 * 5) Fix a security hole : A malicious process with 'ro' access to a file can
 * potentially corrupt file data. This can be fixed by copying the data on a
 * cache read miss.
 */

static int eio_read_peek(struct cache_c *dmc, struct eio_bio *ebio);
static int eio_write_peek(struct cache_c *dmc, struct eio_bio *ebio);
static void eio_read(struct cache_c *dmc, struct bio_container *bc,
		     struct eio_bio *ebegin);
static void eio_write(struct cache_c *dmc, struct bio_container *bc,
		      struct eio_bio *ebegin);
static int eio_inval_block(struct cache_c *dmc, sector_t iosector);
static void eio_enqueue_readfill(struct cache_c *dmc, struct kcached_job *job);
static int eio_acquire_set_locks(struct cache_c *dmc, struct bio_container *bc);
static int eio_release_io_resources(struct cache_c *dmc,
				    struct bio_container *bc);
static void eio_clean_set(struct cache_c *dmc, index_t set, int whole,
			  int force);
static void eio_do_mdupdate(struct work_struct *work);
static void eio_mdupdate_callback(int error, void *context);
static void eio_enq_mdupdate(struct bio_container *bc);
static void eio_uncached_read_done(struct kcached_job *job);
static void eio_addto_cleanq(struct cache_c *dmc, index_t set, int whole);
static int eio_alloc_mdreqs(struct cache_c *, struct bio_container *);
static void eio_check_dirty_set_thresholds(struct cache_c *dmc, index_t set);
static void eio_check_dirty_cache_thresholds(struct cache_c *dmc);
static void eio_post_mdupdate(struct work_struct *work);
static void eio_post_io_callback(struct work_struct *work);

static void bc_addfb(struct bio_container *bc, struct eio_bio *ebio)
{

	atomic_inc(&bc->bc_holdcount);

	ebio->eb_bc = bc;
}

static void bc_put(struct bio_container *bc, unsigned int doneio)
{
	struct cache_c *dmc;
	int data_dir;
	long elapsed;

	if (atomic_dec_and_test(&bc->bc_holdcount)) {
		if (bc->bc_dmc->mode == CACHE_MODE_WB)
			eio_release_io_resources(bc->bc_dmc, bc);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
		bc->bc_bio->bi_iter.bi_size = 0;
#else 
		bc->bc_bio->bi_size = 0;
#endif 
		dmc = bc->bc_dmc;

		/* update iotime for latency */
		data_dir = bio_data_dir(bc->bc_bio);
		elapsed = (long)jiffies_to_msecs(jiffies - bc->bc_iotime);

		if (data_dir == READ)
			atomic64_add(elapsed, &dmc->eio_stats.rdtime_ms);
		else
			atomic64_add(elapsed, &dmc->eio_stats.wrtime_ms);

		bio_endio(bc->bc_bio, bc->bc_error);
		atomic64_dec(&bc->bc_dmc->nr_ios);
		kfree(bc);
	}
}

static void eb_endio(struct eio_bio *ebio, int error)
{

	EIO_ASSERT(ebio->eb_bc);

	/*Propagate only main io errors and sizes*/
	if (ebio->eb_iotype == EB_MAIN_IO) {
		if (error)
			ebio->eb_bc->bc_error = error;
		bc_put(ebio->eb_bc, ebio->eb_size);
	} else
		bc_put(ebio->eb_bc, 0);
	ebio->eb_bc = NULL;
	kfree(ebio);
}

static int
eio_io_async_bvec(struct cache_c *dmc, struct eio_io_region *where, int rw,
		  struct bio_vec *pages, unsigned nr_bvecs, eio_notify_fn fn,
		  void *context, int hddio)
{
	struct eio_io_request req;
	int error = 0;

	memset((char *)&req, 0, sizeof(req));

	if (unlikely(CACHE_DEGRADED_IS_SET(dmc))) {
		if (where->bdev != dmc->disk_dev->bdev) {
			pr_err
				("eio_io_async_bvec: Cache is in degraded mode.\n");
			pr_err
				("eio_io_async_Bvec: Can not issue i/o to ssd device.\n");
			return -ENODEV;
		}
	}

	req.mtype = EIO_BVECS;
	req.dptr.pages = pages;
	req.num_bvecs = nr_bvecs;
	req.notify = fn;
	req.context = context;
	req.hddio = hddio;

	error = eio_do_io(dmc, where, rw, &req);

	return error;
}

static void
eio_flag_abios(struct cache_c *dmc, struct eio_bio *abio, int invalidated)
{
	struct eio_bio *nbio;

	while (abio) {
		int invalidate;
		unsigned long flags;
		int cwip_on = 0;
		int dirty_on = 0;
		int callendio = 0;
		nbio = abio->eb_next;

		EIO_ASSERT(!(abio->eb_iotype & EB_INVAL) || abio->eb_index == -1);
		invalidate = !invalidated && (abio->eb_iotype & EB_INVAL);

		spin_lock_irqsave(&dmc->cache_sets[abio->eb_cacheset].cs_lock,
				  flags);

		if (abio->eb_index != -1) {
			if (EIO_CACHE_STATE_GET(dmc, abio->eb_index) & DIRTY)
				dirty_on = 1;

			if (unlikely
				    (EIO_CACHE_STATE_GET(dmc, abio->eb_index) &
				    CACHEWRITEINPROG))
				cwip_on = 1;
		}

		if (dirty_on) {
			/*
			 * For dirty blocks, we don't change the cache state flags.
			 * We however, need to end the ebio, if this was the last
			 * hold on it.
			 */
			if (atomic_dec_and_test(&abio->eb_holdcount)) {
				callendio = 1;
				/* We shouldn't reach here when the DIRTY_INPROG flag
				 * is set on the cache block. It should either have been
				 * cleared to become DIRTY or INVALID elsewhere.
				 */
				EIO_ASSERT(EIO_CACHE_STATE_GET(dmc, abio->eb_index)
					   != DIRTY_INPROG);
			}
		} else if (abio->eb_index != -1) {
			if (invalidate) {
				if (cwip_on)
					EIO_CACHE_STATE_ON(dmc, abio->eb_index,
							   QUEUED);
				else {
					EIO_CACHE_STATE_SET(dmc, abio->eb_index,
							    INVALID);
					atomic64_dec_if_positive(&dmc->
								 eio_stats.
								 cached_blocks);
				}
			} else {
				if (cwip_on)
					EIO_CACHE_STATE_OFF(dmc, abio->eb_index,
							    DISKWRITEINPROG);
				else {
					if (EIO_CACHE_STATE_GET
						    (dmc, abio->eb_index) & QUEUED) {
						EIO_CACHE_STATE_SET(dmc,
								    abio->
								    eb_index,
								    INVALID);
						atomic64_dec_if_positive(&dmc->
									 eio_stats.
									 cached_blocks);
					} else {
						EIO_CACHE_STATE_SET(dmc,
								    abio->
								    eb_index,
								    VALID);
					}
				}
			}
		} else {
			EIO_ASSERT(invalidated || invalidate);
			if (invalidate)
				eio_inval_block(dmc, abio->eb_sector);
		}
		spin_unlock_irqrestore(&dmc->cache_sets[abio->eb_cacheset].
				       cs_lock, flags);
		if (!cwip_on && (!dirty_on || callendio))
			eb_endio(abio, 0);
		abio = nbio;
	}
}

static void eio_disk_io_callback(int error, void *context)
{
	struct kcached_job *job;
	struct eio_bio *ebio;
	struct cache_c *dmc;
	unsigned long flags;
	unsigned eb_cacheset;

	flags = 0;
	job = (struct kcached_job *)context;
	dmc = job->dmc;
	ebio = job->ebio;

	EIO_ASSERT(ebio != NULL);
	eb_cacheset = ebio->eb_cacheset;

	if (unlikely(error))
		dmc->eio_errors.disk_read_errors++;

	spin_lock_irqsave(&dmc->cache_sets[eb_cacheset].cs_lock, flags);
	/* Invalidate the cache block */
	EIO_CACHE_STATE_SET(dmc, ebio->eb_index, INVALID);
	atomic64_dec_if_positive(&dmc->eio_stats.cached_blocks);
	spin_unlock_irqrestore(&dmc->cache_sets[eb_cacheset].cs_lock, flags);

	if (unlikely(error))
		pr_err("disk_io_callback: io error %d block %llu action %d",
		       error,
		       (unsigned long long)job->job_io_regions.disk.sector,
		       job->action);

	eb_endio(ebio, error);
	ebio = NULL;
	job->ebio = NULL;
	eio_free_cache_job(job);
	job = NULL;
}

static void eio_uncached_read_done(struct kcached_job *job)
{
	struct eio_bio *ebio = job->ebio;
	struct cache_c *dmc = job->dmc;
	struct eio_bio *iebio;
	struct eio_bio *nebio;
	unsigned long flags = 0;

	if (ebio->eb_bc->bc_dir == UNCACHED_READ) {
		EIO_ASSERT(ebio != NULL);
		iebio = ebio->eb_next;
		while (iebio != NULL) {
			nebio = iebio->eb_next;
			if (iebio->eb_index != -1) {
				spin_lock_irqsave(&dmc->
						  cache_sets[iebio->
							     eb_cacheset].
						  cs_lock, flags);
				if (unlikely
					    (EIO_CACHE_STATE_GET(dmc, iebio->eb_index) &
					    QUEUED)) {
					EIO_CACHE_STATE_SET(dmc,
							    iebio->eb_index,
							    INVALID);
					atomic64_dec_if_positive(&dmc->
								 eio_stats.
								 cached_blocks);
				} else
				if (EIO_CACHE_STATE_GET
					    (dmc,
					    iebio->eb_index) & CACHEREADINPROG) {
					/*turn off the cache read in prog flag*/
					EIO_CACHE_STATE_OFF(dmc,
							    iebio->eb_index,
							    BLOCK_IO_INPROG);
				} else
					/*Should never reach here*/
					EIO_ASSERT(0);
				spin_unlock_irqrestore(&dmc->
						       cache_sets[iebio->
								  eb_cacheset].
						       cs_lock, flags);
			}
			eb_endio(iebio, 0);
			iebio = nebio;
		}
		eb_endio(ebio, 0);
		eio_free_cache_job(job);
	} else if (ebio->eb_bc->bc_dir == UNCACHED_READ_AND_READFILL) {
		/*
		 * Kick off the READFILL. It will also do a read
		 * from SSD, in case of ALREADY_DIRTY block
		 */
		job->action = READFILL;
		eio_enqueue_readfill(dmc, job);
	} else
		/* Should never reach here for uncached read */
		EIO_ASSERT(0);
}

static void eio_io_callback(int error, void *context)
{
	struct kcached_job *job = (struct kcached_job *)context;
	struct cache_c *dmc = job->dmc;

	job->error = error;
	INIT_WORK(&job->work, eio_post_io_callback);
	queue_work(dmc->callback_q, &job->work);
	return;
}

static void eio_post_io_callback(struct work_struct *work)
{
	struct kcached_job *job;
	struct cache_c *dmc;
	struct eio_bio *ebio;
	unsigned long flags = 0;
	index_t index;
	unsigned eb_cacheset;
	u_int8_t cstate;
	int callendio = 0;
	int error;

	job = container_of(work, struct kcached_job, work);
	dmc = job->dmc;
	index = job->index;
	error = job->error;

	EIO_ASSERT(index != -1 || job->action == WRITEDISK
		   || job->action == READDISK);
	ebio = job->ebio;
	EIO_ASSERT(ebio != NULL);
	EIO_ASSERT(ebio->eb_bc);

	eb_cacheset = ebio->eb_cacheset;
	if (error)
		pr_err("io_callback: io error %d block %llu action %d",
		       error,
		       (unsigned long long)job->job_io_regions.disk.sector,
		       job->action);

	switch (job->action) {
	case WRITEDISK:

		atomic64_inc(&dmc->eio_stats.writedisk);
		if (unlikely(error))
			dmc->eio_errors.disk_write_errors++;
		if (unlikely(error) || (ebio->eb_iotype & EB_INVAL))
			eio_inval_range(dmc, ebio->eb_sector, ebio->eb_size);
		if (ebio->eb_next)
			eio_flag_abios(dmc, ebio->eb_next,
				       error || (ebio->eb_iotype & EB_INVAL));
		eb_endio(ebio, error);
		job->ebio = NULL;
		eio_free_cache_job(job);
		return;

	case READDISK:

		if (unlikely(error) || unlikely(ebio->eb_iotype & EB_INVAL)
		    || CACHE_DEGRADED_IS_SET(dmc)) {
			if (error)
				dmc->eio_errors.disk_read_errors++;
			eio_inval_range(dmc, ebio->eb_sector, ebio->eb_size);
			eio_flag_abios(dmc, ebio->eb_next, 1);
		} else if (ebio->eb_next) {
			eio_uncached_read_done(job);
			return;
		}
		eb_endio(ebio, error);
		job->ebio = NULL;
		eio_free_cache_job(job);
		return;

	case READCACHE:

		/*atomic64_inc(&dmc->eio_stats.readcache);*/
		/*SECTOR_STATS(dmc->eio_stats.ssd_reads, ebio->eb_size);*/
		EIO_ASSERT(EIO_DBN_GET(dmc, index) ==
			   EIO_ROUND_SECTOR(dmc, ebio->eb_sector));
		cstate = EIO_CACHE_STATE_GET(dmc, index);
		/* We shouldn't reach here for DIRTY_INPROG blocks. */
		EIO_ASSERT(cstate != DIRTY_INPROG);
		if (unlikely(error)) {
			dmc->eio_errors.ssd_read_errors++;
			/* Retry read from HDD for non-DIRTY blocks. */
			if (cstate != ALREADY_DIRTY) {
				spin_lock_irqsave(&dmc->cache_sets[eb_cacheset].
						  cs_lock, flags);
				EIO_CACHE_STATE_OFF(dmc, ebio->eb_index,
						    CACHEREADINPROG);
				EIO_CACHE_STATE_ON(dmc, ebio->eb_index,
						   DISKREADINPROG);
				spin_unlock_irqrestore(&dmc->
						       cache_sets[eb_cacheset].
						       cs_lock, flags);

				eio_push_ssdread_failures(job);
				schedule_work(&_kcached_wq);

				return;
			}
		}
		callendio = 1;
		break;

	case READFILL:

		/*atomic64_inc(&dmc->eio_stats.readfill);*/
		/*SECTOR_STATS(dmc->eio_stats.ssd_writes, ebio->eb_size);*/
		EIO_ASSERT(EIO_DBN_GET(dmc, index) == ebio->eb_sector);
		if (unlikely(error))
			dmc->eio_errors.ssd_write_errors++;
		if (!(EIO_CACHE_STATE_GET(dmc, index) & CACHEWRITEINPROG)) {
			pr_debug("DISKWRITEINPROG absent in READFILL \
				sector %llu io size %u\n",
				(unsigned long long)ebio->eb_sector,
			       ebio->eb_size);
		}
		callendio = 1;
		break;

	case WRITECACHE:

		/*SECTOR_STATS(dmc->eio_stats.ssd_writes, ebio->eb_size);*/
		/*atomic64_inc(&dmc->eio_stats.writecache);*/
		cstate = EIO_CACHE_STATE_GET(dmc, index);
		EIO_ASSERT(EIO_DBN_GET(dmc, index) ==
			   EIO_ROUND_SECTOR(dmc, ebio->eb_sector));
		/* CWIP is a must for WRITECACHE, except when it is DIRTY */
		EIO_ASSERT(cstate & (CACHEWRITEINPROG | DIRTY));
		if (likely(error == 0)) {
			/* If it is a DIRTY inprog block, proceed for metadata update */
			if (cstate == DIRTY_INPROG) {
				eio_md_write(job);
				return;
			}
		} else {
			/* TODO: ask if this if condition is required */
			if (dmc->mode == CACHE_MODE_WT)
				dmc->eio_errors.disk_write_errors++;
			dmc->eio_errors.ssd_write_errors++;
		}
		job->ebio = NULL;
		break;

	default:
		pr_err("io_callback: invalid action %d", job->action);
		return;
	}

	spin_lock_irqsave(&dmc->cache_sets[eb_cacheset].cs_lock, flags);

	cstate = EIO_CACHE_STATE_GET(dmc, index);
	EIO_ASSERT(!(cstate & INVALID));

	if (unlikely
		    ((job->action == WRITECACHE) && !(cstate & DISKWRITEINPROG))) {
		/*
		 * Can reach here in 2 cases:
		 * 1. Uncached write case, where WRITEDISK has finished first
		 * 2. Cached write case
		 *
		 * For DIRTY or DIRTY inprog cases, use eb holdcount to determine
		 * if end ebio can be called. This is because, we don't set DWIP etc
		 * flags on those and we have to avoid double end ebio call
		 */
		EIO_ASSERT((cstate != DIRTY_INPROG) || error);
		callendio = 1;
		if ((cstate & DIRTY)
		    && !atomic_dec_and_test(&ebio->eb_holdcount))
			callendio = 0;
	}

	if (cstate & DISKWRITEINPROG) {
		/* uncached write and WRITEDISK is not yet finished */
		EIO_ASSERT(!(cstate & DIRTY));      /* For dirty blocks, we can't have DWIP flag */
		if (error)
			EIO_CACHE_STATE_ON(dmc, index, QUEUED);
		EIO_CACHE_STATE_OFF(dmc, index, CACHEWRITEINPROG);
	} else if (unlikely(error || (cstate & QUEUED))) {
		/* Error or QUEUED is set: mark block as INVALID for non-DIRTY blocks */
		if (cstate != ALREADY_DIRTY) {
			EIO_CACHE_STATE_SET(dmc, index, INVALID);
			atomic64_dec_if_positive(&dmc->eio_stats.cached_blocks);
		}
	} else if (cstate & VALID) {
		EIO_CACHE_STATE_OFF(dmc, index, BLOCK_IO_INPROG);
		/*
		 * If we have NO_SSD_IO_INPROG flag set, then this block needs to be
		 * invalidated. There are three things that can happen -- (i) error,
		 * (ii) IOs are queued on this block, and (iii) success.
		 *
		 * If there was an error or if the QUEUED bit was set, then the logic
		 * in the if part will take care of setting the block to INVALID.
		 * Therefore, this is the success path where we invalidate if need be.
		 */

		/*
		 * TBD
		 * NO_SSD_IO_INPROG need to be differently handled, in case block is DIRTY
		 */
		if ((cstate & NO_SSD_IO_INPROG) == NO_SSD_IO_INPROG)
			EIO_CACHE_STATE_OFF(dmc, index, VALID);
	}

	spin_unlock_irqrestore(&dmc->cache_sets[eb_cacheset].cs_lock, flags);

	if (callendio)
		eb_endio(ebio, error);

	eio_free_cache_job(job);
	job = NULL;

}

/*
 * This function processes the kcached_job that
 * needs to be scheduled on disk after ssd read failures.
 */
void eio_ssderror_diskread(struct kcached_job *job)
{
	struct cache_c *dmc;
	struct eio_bio *ebio;
	index_t index;
	int error;
	unsigned long flags = 0;

	dmc = job->dmc;
	error = 0;

	/*
	 * 1. Extract the ebio which needs to be scheduled on disk.
	 * 2. Verify cache block state is VALID
	 * 3. Make sure that the cache state in not IOINPROG
	 */
	/* Reset the ssd read error in the job. */
	job->error = 0;
	ebio = job->ebio;
	index = ebio->eb_index;

	EIO_ASSERT(index != -1);

	spin_lock_irqsave(&dmc->cache_sets[index / dmc->assoc].cs_lock, flags);
	EIO_ASSERT(EIO_CACHE_STATE_GET(dmc, index) & DISKREADINPROG);
	spin_unlock_irqrestore(&dmc->cache_sets[index / dmc->assoc].cs_lock,
			       flags);

	EIO_ASSERT(ebio->eb_dir == READ);

	atomic64_inc(&dmc->eio_stats.readdisk);
	SECTOR_STATS(dmc->eio_stats.disk_reads, ebio->eb_size);
	job->action = READDISK;

	error = eio_io_async_bvec(dmc, &job->job_io_regions.disk, ebio->eb_dir,
				  ebio->eb_bv, ebio->eb_nbvec,
				  eio_disk_io_callback, job, 1);

	/*
	 * In case of disk i/o submission error clear ebio and kcached_job.
	 * This would return the actual read that was issued on ssd.
	 */
	if (error)
		goto out;

	return;

out:
	/* We failed to submit the I/O to dm layer. The corresponding
	 * block should be marked as INVALID by turning off already set
	 * flags.
	 */
	spin_lock_irqsave(&dmc->cache_sets[index / dmc->assoc].cs_lock, flags);
	EIO_CACHE_STATE_SET(dmc, ebio->eb_index, INVALID);
	spin_unlock_irqrestore(&dmc->cache_sets[index / dmc->assoc].cs_lock,
			       flags);

	atomic64_dec_if_positive(&dmc->eio_stats.cached_blocks);

	eb_endio(ebio, error);
	ebio = NULL;
	job->ebio = NULL;
	eio_free_cache_job(job);
}

/* Adds clean set request to clean queue. */
static void eio_addto_cleanq(struct cache_c *dmc, index_t set, int whole)
{
	unsigned long flags = 0;

	spin_lock_irqsave(&dmc->cache_sets[set].cs_lock, flags);

	if (dmc->cache_sets[set].flags & SETFLAG_CLEAN_INPROG) {
		/* Clean already in progress, just add to clean pendings */
		spin_unlock_irqrestore(&dmc->cache_sets[set].cs_lock, flags);
		return;
	}

	dmc->cache_sets[set].flags |= SETFLAG_CLEAN_INPROG;
	if (whole)
		dmc->cache_sets[set].flags |= SETFLAG_CLEAN_WHOLE;

	spin_unlock_irqrestore(&dmc->cache_sets[set].cs_lock, flags);

	spin_lock_irqsave(&dmc->clean_sl, flags);
	list_add_tail(&dmc->cache_sets[set].list, &dmc->cleanq);
	atomic64_inc(&dmc->clean_pendings);
	EIO_SET_EVENT_AND_UNLOCK(&dmc->clean_event, &dmc->clean_sl, flags);
	return;
}

/*
 * Clean thread loops forever in this, waiting for
 * new clean set requests in the clean queue.
 */
int eio_clean_thread_proc(void *context)
{
	struct cache_c *dmc = (struct cache_c *)context;
	unsigned long flags = 0;
	u_int64_t systime;
	index_t index;

	/* Sync makes sense only for writeback cache */
	EIO_ASSERT(dmc->mode == CACHE_MODE_WB);

	dmc->clean_thread_running = 1;

	/*
	 * Using sysctl_fast_remove to stop the clean thread
	 * works for now. Should have another flag specifically
	 * for such notification.
	 */
	for (; !dmc->sysctl_active.fast_remove; ) {
		LIST_HEAD(setlist);
		struct cache_set *set;

		eio_comply_dirty_thresholds(dmc, -1);

		if (dmc->sysctl_active.do_clean) {
			/* pause the periodic clean */
			cancel_delayed_work_sync(&dmc->clean_aged_sets_work);

			/* clean all the sets */
			eio_clean_all(dmc);

			/* resume the periodic clean */
			spin_lock_irqsave(&dmc->dirty_set_lru_lock, flags);
			dmc->is_clean_aged_sets_sched = 0;
			if (dmc->sysctl_active.time_based_clean_interval
			    && atomic64_read(&dmc->nr_dirty)) {
				/* there is a potential race here, If a sysctl changes
				   the time_based_clean_interval to 0. However a strong
				   synchronisation is not necessary here
				 */
				schedule_delayed_work(&dmc->
						      clean_aged_sets_work,
						      dmc->sysctl_active.
						      time_based_clean_interval
						      * 60 * HZ);
				dmc->is_clean_aged_sets_sched = 1;
			}
			spin_unlock_irqrestore(&dmc->dirty_set_lru_lock, flags);
		}

		if (dmc->sysctl_active.fast_remove)
			break;

		spin_lock_irqsave(&dmc->clean_sl, flags);

		while (!
		       ((!list_empty(&dmc->cleanq))
			|| dmc->sysctl_active.fast_remove
			|| dmc->sysctl_active.do_clean))
			EIO_WAIT_EVENT(&dmc->clean_event, &dmc->clean_sl,
				       flags);

		/*
		 * Move cleanq elements to a private list for processing.
		 */

		list_add(&setlist, &dmc->cleanq);
		list_del(&dmc->cleanq);
		INIT_LIST_HEAD(&dmc->cleanq);

		spin_unlock_irqrestore(&dmc->clean_sl, flags);

		systime = jiffies;
		while (!list_empty(&setlist)) {
			set =
				list_entry((&setlist)->next, struct cache_set,
					   list);
			list_del(&set->list);
			index = set - dmc->cache_sets;
			if (!(dmc->sysctl_active.fast_remove)) {
				eio_clean_set(dmc, index,
					      set->flags & SETFLAG_CLEAN_WHOLE,
					      0);
			} else {

				/*
				 * Since we are not cleaning the set, we should
				 * put the set back in the lru list so that
				 * it is picked up at a later point.
				 * We also need to clear the clean inprog flag
				 * otherwise this set would never be cleaned.
				 */

				spin_lock_irqsave(&dmc->cache_sets[index].
						  cs_lock, flags);
				dmc->cache_sets[index].flags &=
					~(SETFLAG_CLEAN_INPROG |
					  SETFLAG_CLEAN_WHOLE);
				spin_unlock_irqrestore(&dmc->cache_sets[index].
						       cs_lock, flags);
				spin_lock_irqsave(&dmc->dirty_set_lru_lock,
						  flags);
				lru_touch(dmc->dirty_set_lru, index, systime);
				spin_unlock_irqrestore(&dmc->dirty_set_lru_lock,
						       flags);
			}
			atomic64_dec(&dmc->clean_pendings);
		}
	}

	/* notifier for cache delete that the clean thread has stopped running */
	dmc->clean_thread_running = 0;

	eio_thread_exit(0);

	/*Should never reach here*/
	return 0;
}

/*
 * Cache miss support. We read the data from disk, write it to the ssd.
 * To avoid doing 1 IO at a time to the ssd, when the IO is kicked off,
 * we enqueue it to a "readfill" queue in the cache in cache sector order.
 * The worker thread can then issue all of these IOs and do 1 unplug to
 * start them all.
 *
 */
static void eio_enqueue_readfill(struct cache_c *dmc, struct kcached_job *job)
{
	unsigned long flags = 0;
	struct kcached_job **j1, *next;
	int do_schedule = 0;

	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	/* Insert job in sorted order of cache sector */
	j1 = &dmc->readfill_queue;
	while (*j1 != NULL && (*j1)->job_io_regions.cache.sector <
	       job->job_io_regions.cache.sector)
		j1 = &(*j1)->next;
	next = *j1;
	*j1 = job;
	job->next = next;
	do_schedule = (dmc->readfill_in_prog == 0);
	spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	if (do_schedule)
		schedule_work(&dmc->readfill_wq);
}

void eio_do_readfill(struct work_struct *work)
{
	struct kcached_job *job, *joblist;
	struct eio_bio *ebio;
	unsigned long flags = 0;
	struct kcached_job *nextjob = NULL;
	struct cache_c *dmc = container_of(work, struct cache_c, readfill_wq);

	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	if (dmc->readfill_in_prog)
		goto out;
	dmc->readfill_in_prog = 1;
	while (dmc->readfill_queue != NULL) {
		joblist = dmc->readfill_queue;
		dmc->readfill_queue = NULL;
		spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
		for (job = joblist; job != NULL; job = nextjob) {
			struct eio_bio *iebio;
			struct eio_bio *next;

			nextjob = job->next;    /* save for later because 'job' will be freed */
			EIO_ASSERT(job->action == READFILL);
			/* Write to cache device */
			ebio = job->ebio;
			iebio = ebio->eb_next;
			EIO_ASSERT(iebio);
			/* other iebios are anchored on this bio. Create
			 * jobs for them and then issue ios
			 */
			do {
				struct kcached_job *job;
				int err;
				unsigned long flags;
				index_t index;
				next = iebio->eb_next;
				index = iebio->eb_index;
				if (index == -1) {
					CTRACE("eio_do_readfill:1\n");
					/* Any INPROG(including DIRTY_INPROG) case would fall here */
					eb_endio(iebio, 0);
					iebio = NULL;
				} else {
					spin_lock_irqsave(&dmc->
							  cache_sets[iebio->
								     eb_cacheset].
							  cs_lock, flags);
					/* If this block was already  valid, we don't need to write it */
					if (unlikely
						    (EIO_CACHE_STATE_GET(dmc, index) &
						    QUEUED)) {
						/*An invalidation request is queued. Can't do anything*/
						CTRACE("eio_do_readfill:2\n");
						EIO_CACHE_STATE_SET(dmc, index,
								    INVALID);
						spin_unlock_irqrestore(&dmc->
								       cache_sets
								       [iebio->
									eb_cacheset].
								       cs_lock,
								       flags);
						atomic64_dec_if_positive(&dmc->
									 eio_stats.
									 cached_blocks);
						eb_endio(iebio, 0);
						iebio = NULL;
					} else
					if ((EIO_CACHE_STATE_GET(dmc, index)
					     & (VALID | DISKREADINPROG))
					    == (VALID | DISKREADINPROG)) {
						/* Do readfill. */
						EIO_CACHE_STATE_SET(dmc, index,
								    VALID |
								    CACHEWRITEINPROG);
						EIO_ASSERT(EIO_DBN_GET(dmc, index)
							   == iebio->eb_sector);
						spin_unlock_irqrestore(&dmc->
								       cache_sets
								       [iebio->
									eb_cacheset].
								       cs_lock,
								       flags);
						job =
							eio_new_job(dmc, iebio,
								    iebio->
								    eb_index);
						if (unlikely(job == NULL))
							err = -ENOMEM;
						else {
							err = 0;
							job->action = READFILL;
							atomic_inc(&dmc->
								   nr_jobs);
							SECTOR_STATS(dmc->
								     eio_stats.
								     ssd_readfills,
								     iebio->
								     eb_size);
							SECTOR_STATS(dmc->
								     eio_stats.
								     ssd_writes,
								     iebio->
								     eb_size);
							atomic64_inc(&dmc->
								     eio_stats.
								     readfill);
							atomic64_inc(&dmc->
								     eio_stats.
								     writecache);
							err =
								eio_io_async_bvec
									(dmc,
									&job->
									job_io_regions.
									cache, WRITE,
									iebio->eb_bv,
									iebio->eb_nbvec,
									eio_io_callback,
									job, 0);
						}
						if (err) {
							pr_err
								("eio_do_readfill: IO submission failed, block %llu",
								EIO_DBN_GET(dmc,
									    index));
							spin_lock_irqsave(&dmc->
									  cache_sets
									  [iebio->
									   eb_cacheset].
									  cs_lock,
									  flags);
							EIO_CACHE_STATE_SET(dmc,
									    iebio->
									    eb_index,
									    INVALID);
							spin_unlock_irqrestore
								(&dmc->
								cache_sets[iebio->
									   eb_cacheset].
								cs_lock, flags);
							atomic64_dec_if_positive
								(&dmc->eio_stats.
								cached_blocks);
							eb_endio(iebio, err);

							if (job) {
								eio_free_cache_job
									(job);
								job = NULL;
							}
						}
					} else
					if (EIO_CACHE_STATE_GET(dmc, index)
					    == ALREADY_DIRTY) {

						spin_unlock_irqrestore(&dmc->
								       cache_sets
								       [iebio->
									eb_cacheset].
								       cs_lock,
								       flags);

						/*
						 * DIRTY block handling:
						 * Read the dirty data from the cache block to update
						 * the data buffer already read from the disk
						 */
						job =
							eio_new_job(dmc, iebio,
								    iebio->
								    eb_index);
						if (unlikely(job == NULL))
							err = -ENOMEM;
						else {
							job->action = READCACHE;
							SECTOR_STATS(dmc->
								     eio_stats.
								     ssd_reads,
								     iebio->
								     eb_size);
							atomic64_inc(&dmc->
								     eio_stats.
								     readcache);
							err =
								eio_io_async_bvec
									(dmc,
									&job->
									job_io_regions.
									cache, READ,
									iebio->eb_bv,
									iebio->eb_nbvec,
									eio_io_callback,
									job, 0);
						}

						if (err) {
							pr_err
								("eio_do_readfill: dirty block read IO submission failed, block %llu",
								EIO_DBN_GET(dmc,
									    index));
							/* can't invalidate the DIRTY block, just return error */
							eb_endio(iebio, err);
							if (job) {
								eio_free_cache_job
									(job);
								job = NULL;
							}
						}
					} else
					if ((EIO_CACHE_STATE_GET(dmc, index)
					     & (VALID | CACHEREADINPROG))
					    == (VALID | CACHEREADINPROG)) {
						/*turn off the cache read in prog flag
						   don't need to write the cache block*/
						CTRACE("eio_do_readfill:3\n");
						EIO_CACHE_STATE_OFF(dmc, index,
								    BLOCK_IO_INPROG);
						spin_unlock_irqrestore(&dmc->
								       cache_sets
								       [iebio->
									eb_cacheset].
								       cs_lock,
								       flags);
						eb_endio(iebio, 0);
						iebio = NULL;
					} else {
						panic("Unknown condition");
						spin_unlock_irqrestore(&dmc->
								       cache_sets
								       [iebio->
									eb_cacheset].
								       cs_lock,
								       flags);
					}
				}
				iebio = next;
			} while (iebio);
			eb_endio(ebio, 0);
			ebio = NULL;
			eio_free_cache_job(job);
		}
		spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	}
	dmc->readfill_in_prog = 0;
out:
	spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
	atomic64_inc(&dmc->eio_stats.ssd_readfill_unplugs);
	eio_unplug_cache_device(dmc);
}

/*
 * Map a block from the source device to a block in the cache device.
 */
static u_int32_t hash_block(struct cache_c *dmc, sector_t dbn)
{
	u_int32_t set_number;

	set_number = eio_hash_block(dmc, dbn);
	return set_number;
}

static void
find_valid_dbn(struct cache_c *dmc, sector_t dbn,
	       index_t start_index, index_t *index)
{
	index_t i;
	index_t end_index = start_index + dmc->assoc;

	for (i = start_index; i < end_index; i++) {
		if ((EIO_CACHE_STATE_GET(dmc, i) & VALID)
		    && EIO_DBN_GET(dmc, i) == dbn) {
			*index = i;
			if ((EIO_CACHE_STATE_GET(dmc, i) & BLOCK_IO_INPROG) ==
			    0)
				eio_policy_reclaim_lru_movetail(dmc, i,
								dmc->
								policy_ops);
			return;
		}
	}
	*index = -1;
}

static index_t find_invalid_dbn(struct cache_c *dmc, index_t start_index)
{
	index_t i;
	index_t end_index = start_index + dmc->assoc;

	/* Find INVALID slot that we can reuse */
	for (i = start_index; i < end_index; i++) {
		if (EIO_CACHE_STATE_GET(dmc, i) == INVALID) {
			eio_policy_reclaim_lru_movetail(dmc, i,
							dmc->policy_ops);
			return i;
		}
	}
	return -1;
}

/* Search for a slot that we can reclaim */
static void
find_reclaim_dbn(struct cache_c *dmc, index_t start_index, index_t *index)
{
	eio_find_reclaim_dbn(dmc->policy_ops, start_index, index);
}

void eio_set_warm_boot(void)
{
	eio_force_warm_boot = 1;
	return;
}

/*
 * dbn is the starting sector.
 */
static int
eio_lookup(struct cache_c *dmc, struct eio_bio *ebio, index_t *index)
{
	sector_t dbn = EIO_ROUND_SECTOR(dmc, ebio->eb_sector);
	u_int32_t set_number;
	index_t invalid, oldest_clean = -1;
	index_t start_index;

	/*ASK it is assumed that the lookup is being done for a single block*/
	set_number = hash_block(dmc, dbn);
	start_index = dmc->assoc * set_number;
	find_valid_dbn(dmc, dbn, start_index, index);
	if (*index >= 0)
		/* We found the exact range of blocks we are looking for */
		return VALID;

	invalid = find_invalid_dbn(dmc, start_index);
	if (invalid == -1)
		/* We didn't find an invalid entry, search for oldest valid entry */
		find_reclaim_dbn(dmc, start_index, &oldest_clean);
	/*
	 * Cache miss :
	 * We can't choose an entry marked INPROG, but choose the oldest
	 * INVALID or the oldest VALID entry.
	 */
	*index = start_index + dmc->assoc;
	if (invalid != -1) {
		*index = invalid;
		return INVALID;
	} else if (oldest_clean != -1) {
		*index = oldest_clean;
		return VALID;
	}
	return -1;
}

/* Do metadata update for a set */
static void eio_do_mdupdate(struct work_struct *work)
{
	struct mdupdate_request *mdreq;
	struct cache_set *set;
	struct cache_c *dmc;
	unsigned long flags;
	index_t i;
	index_t start_index;
	index_t end_index;
	index_t min_index;
	index_t max_index;
	struct flash_cacheblock *md_blocks;
	struct eio_bio *ebio;
	u_int8_t cstate;
	struct eio_io_region region;
	unsigned pindex;
	int error, j;
	index_t blk_index;
	int k;
	void *pg_virt_addr[2] = { NULL };
	u_int8_t sector_bits[2] = { 0 };
	int startbit, endbit;
	int rw_flags = 0;

	mdreq = container_of(work, struct mdupdate_request, work);
	dmc = mdreq->dmc;
	set = &dmc->cache_sets[mdreq->set];

	mdreq->error = 0;
	EIO_ASSERT(mdreq->mdblk_bvecs);

	/*
	 * md_size = dmc->assoc * sizeof(struct flash_cacheblock);
	 * Currently, md_size is 8192 bytes, mdpage_count is 2 pages maximum.
	 */

	EIO_ASSERT(mdreq->mdbvec_count && mdreq->mdbvec_count <= 2);
	EIO_ASSERT((dmc->assoc == 512) || mdreq->mdbvec_count == 1);
	for (k = 0; k < (int)mdreq->mdbvec_count; k++)
		pg_virt_addr[k] = kmap(mdreq->mdblk_bvecs[k].bv_page);

	spin_lock_irqsave(&set->cs_lock, flags);

	start_index = mdreq->set * dmc->assoc;
	end_index = start_index + dmc->assoc;

	pindex = 0;
	md_blocks = (struct flash_cacheblock *)pg_virt_addr[pindex];
	j = MD_BLOCKS_PER_PAGE;

	/* initialize the md blocks to write */
	for (i = start_index; i < end_index; i++) {
		cstate = EIO_CACHE_STATE_GET(dmc, i);
		md_blocks->dbn = cpu_to_le64(EIO_DBN_GET(dmc, i));
		if (cstate == ALREADY_DIRTY)
			md_blocks->cache_state = cpu_to_le64((VALID | DIRTY));
		else
			md_blocks->cache_state = cpu_to_le64(INVALID);
		md_blocks++;
		j--;

		if ((j == 0) && (++pindex < mdreq->mdbvec_count)) {
			md_blocks =
				(struct flash_cacheblock *)pg_virt_addr[pindex];
			j = MD_BLOCKS_PER_PAGE;
		}

	}

	/* Update the md blocks with the pending mdlist */
	min_index = start_index;
	max_index = start_index;

	pindex = 0;
	md_blocks = (struct flash_cacheblock *)pg_virt_addr[pindex];

	ebio = mdreq->pending_mdlist;
	while (ebio) {
		EIO_ASSERT(EIO_CACHE_STATE_GET(dmc, ebio->eb_index) ==
			   DIRTY_INPROG);

		blk_index = ebio->eb_index - start_index;
		pindex = INDEX_TO_MD_PAGE(blk_index);
		blk_index = INDEX_TO_MD_PAGE_OFFSET(blk_index);
		sector_bits[pindex] |= (1 << INDEX_TO_MD_SECTOR(blk_index));

		md_blocks = (struct flash_cacheblock *)pg_virt_addr[pindex];
		md_blocks[blk_index].cache_state = (VALID | DIRTY);

		if (min_index > ebio->eb_index)
			min_index = ebio->eb_index;

		if (max_index < ebio->eb_index)
			max_index = ebio->eb_index;

		ebio = ebio->eb_next;
	}

	/*
	 * Below code may be required when selective pages need to be
	 * submitted for metadata update. Currently avoiding the optimization
	 * for correctness validation.
	 */

	/*
	   min_cboff = (min_index - start_index) / MD_BLOCKS_PER_CBLOCK(dmc);
	   max_cboff = (max_index - start_index) / MD_BLOCKS_PER_CBLOCK(dmc);
	   write_size = ((uint32_t)(max_cboff - min_cboff + 1)) << dmc->block_shift;
	   EIO_ASSERT(write_size && (write_size <= eio_to_sector(mdreq->md_size)));
	 */

	/* Move the pending mdlist to inprog list */
	mdreq->inprog_mdlist = mdreq->pending_mdlist;
	mdreq->pending_mdlist = NULL;

	spin_unlock_irqrestore(&set->cs_lock, flags);

	for (k = 0; k < (int)mdreq->mdbvec_count; k++)
		kunmap(mdreq->mdblk_bvecs[k].bv_page);

	/*
	 * Initiate the I/O to SSD for on-disk md update.
	 * TBD. Optimize to write only the affected blocks
	 */

	region.bdev = dmc->cache_dev->bdev;
	/*region.sector = dmc->md_start_sect + INDEX_TO_MD_SECTOR(start_index) +
	   (min_cboff << dmc->block_shift); */

	atomic_set(&mdreq->holdcount, 1);
	for (i = 0; i < mdreq->mdbvec_count; i++) {
		if (!sector_bits[i])
			continue;
		startbit = -1;
		j = 0;
		while (startbit == -1) {
			if (sector_bits[i] & (1 << j))
				startbit = j;
			j++;
		}
		endbit = -1;
		j = 7;
		while (endbit == -1) {
			if (sector_bits[i] & (1 << j))
				endbit = j;
			j--;
		}
		EIO_ASSERT(startbit <= endbit && startbit >= 0 && startbit <= 7 &&
			   endbit >= 0 && endbit <= 7);
		EIO_ASSERT(dmc->assoc != 128 || endbit <= 3);
		region.sector =
			dmc->md_start_sect + INDEX_TO_MD_SECTOR(start_index) +
			i * SECTORS_PER_PAGE + startbit;
		region.count = endbit - startbit + 1;
		mdreq->mdblk_bvecs[i].bv_offset = to_bytes(startbit);
		mdreq->mdblk_bvecs[i].bv_len = to_bytes(region.count);

		EIO_ASSERT(region.sector <=
			   (dmc->md_start_sect + INDEX_TO_MD_SECTOR(end_index)));
		atomic64_inc(&dmc->eio_stats.md_ssd_writes);
		SECTOR_STATS(dmc->eio_stats.ssd_writes, to_bytes(region.count));
		atomic_inc(&mdreq->holdcount);

		/*
		 * Set SYNC for making metadata
		 * writes as high priority.
		 */
		rw_flags = WRITE | REQ_SYNC;
		error = eio_io_async_bvec(dmc, &region, rw_flags,
					  &mdreq->mdblk_bvecs[i], 1,
					  eio_mdupdate_callback, work, 0);
		if (error && !(mdreq->error))
			mdreq->error = error;
	}
	if (atomic_dec_and_test(&mdreq->holdcount)) {
		INIT_WORK(&mdreq->work, eio_post_mdupdate);
		queue_work(dmc->mdupdate_q, &mdreq->work);
	}
}

/* Callback function for ondisk metadata update */
static void eio_mdupdate_callback(int error, void *context)
{
	struct work_struct *work = (struct work_struct *)context;
	struct mdupdate_request *mdreq;

	mdreq = container_of(work, struct mdupdate_request, work);
	if (error && !(mdreq->error))
		mdreq->error = error;
	if (!atomic_dec_and_test(&mdreq->holdcount))
		return;
	INIT_WORK(&mdreq->work, eio_post_mdupdate);
	queue_work(mdreq->dmc->mdupdate_q, &mdreq->work);
}

static void eio_post_mdupdate(struct work_struct *work)
{
	struct mdupdate_request *mdreq;
	struct cache_set *set;
	struct cache_c *dmc;
	unsigned long flags;
	struct eio_bio *ebio;
	struct eio_bio *nebio;
	int more_pending_mdupdates = 0;
	int error;
	index_t set_index;

	mdreq = container_of(work, struct mdupdate_request, work);

	dmc = mdreq->dmc;
	EIO_ASSERT(dmc);
	set_index = mdreq->set;
	set = &dmc->cache_sets[set_index];
	error = mdreq->error;

	/* Update in-core cache metadata */

	spin_lock_irqsave(&set->cs_lock, flags);

	/*
	 * Update dirty inprog blocks.
	 * On error, convert them to INVALID
	 * On success, convert them to ALREADY_DIRTY
	 */
	ebio = mdreq->inprog_mdlist;
	while (ebio) {
		EIO_ASSERT(EIO_CACHE_STATE_GET(dmc, ebio->eb_index) ==
			   DIRTY_INPROG);
		if (unlikely(error)) {
			EIO_CACHE_STATE_SET(dmc, ebio->eb_index, INVALID);
			atomic64_dec_if_positive(&dmc->eio_stats.cached_blocks);
		} else {
			EIO_CACHE_STATE_SET(dmc, ebio->eb_index, ALREADY_DIRTY);
			set->nr_dirty++;
			atomic64_inc(&dmc->nr_dirty);
			atomic64_inc(&dmc->eio_stats.md_write_dirty);
		}
		ebio = ebio->eb_next;
	}

	/*
	 * If there are more pending requests for md update,
	 * need to pick up those using the current mdreq.
	 */
	if (mdreq->pending_mdlist)
		more_pending_mdupdates = 1;
	else
		/* No request pending, we can free the mdreq */
		set->mdreq = NULL;

	/*
	 * After we unlock the set, we need to end the I/Os,
	 * which were processed as part of this md update
	 */

	ebio = mdreq->inprog_mdlist;
	mdreq->inprog_mdlist = NULL;

	spin_unlock_irqrestore(&set->cs_lock, flags);

	/* End the processed I/Os */
	while (ebio) {
		nebio = ebio->eb_next;
		eb_endio(ebio, error);
		ebio = nebio;
	}

	/*
	 * if dirty block was added
	 * 1. update the cache set lru list
	 * 2. check and initiate cleaning if thresholds are crossed
	 */
	if (!error) {
		eio_touch_set_lru(dmc, set_index);
		eio_comply_dirty_thresholds(dmc, set_index);
	}

	if (more_pending_mdupdates) {
		/*
		 * Schedule work to process the new
		 * pending mdupdate requests
		 */
		INIT_WORK(&mdreq->work, eio_do_mdupdate);
		queue_work(dmc->mdupdate_q, &mdreq->work);
	} else {
		/*
		 * No more pending mdupdates.
		 * Free the mdreq.
		 */
		if (mdreq->mdblk_bvecs) {
			eio_free_wb_bvecs(mdreq->mdblk_bvecs,
					  mdreq->mdbvec_count,
					  SECTORS_PER_PAGE);
			kfree(mdreq->mdblk_bvecs);
		}

		kfree(mdreq);
	}
}

/* Enqueue metadata update for marking dirty blocks on-disk/in-core */
static void eio_enq_mdupdate(struct bio_container *bc)
{
	index_t set_index;
	struct eio_bio *ebio;
	struct cache_c *dmc = bc->bc_dmc;
	struct cache_set *set = NULL;
	struct mdupdate_request *mdreq;
	int do_schedule;

	ebio = bc->bc_mdlist;
	set_index = -1;
	do_schedule = 0;
	while (ebio) {
		if (ebio->eb_cacheset != set_index) {
			set_index = ebio->eb_cacheset;
			set = &dmc->cache_sets[set_index];
			spin_lock(&set->cs_lock);
		}
		EIO_ASSERT(ebio->eb_cacheset == set_index);

		bc->bc_mdlist = ebio->eb_next;

		if (!set->mdreq) {
			/* Pick up one mdreq from bc */
			mdreq = bc->mdreqs;
			EIO_ASSERT(mdreq != NULL);
			bc->mdreqs = bc->mdreqs->next;
			mdreq->next = NULL;
			mdreq->pending_mdlist = ebio;
			mdreq->dmc = dmc;
			mdreq->set = set_index;
			set->mdreq = mdreq;
			ebio->eb_next = NULL;
			do_schedule = 1;
		} else {
			mdreq = set->mdreq;
			EIO_ASSERT(mdreq != NULL);
			ebio->eb_next = mdreq->pending_mdlist;
			mdreq->pending_mdlist = ebio;
		}

		ebio = bc->bc_mdlist;
		if (!ebio || ebio->eb_cacheset != set_index) {
			spin_unlock(&set->cs_lock);
			if (do_schedule) {
				INIT_WORK(&mdreq->work, eio_do_mdupdate);
				queue_work(dmc->mdupdate_q, &mdreq->work);
				do_schedule = 0;
			}
		}
	}

	EIO_ASSERT(bc->bc_mdlist == NULL);
}

/* Kick-off a cache metadata update for marking the blocks dirty */
void eio_md_write(struct kcached_job *job)
{
	struct eio_bio *ebio = job->ebio;
	struct eio_bio *nebio;
	struct eio_bio *pebio;
	struct bio_container *bc = ebio->eb_bc;
	unsigned long flags;

	/*
	 * ebios are stored in ascending order of cache sets.
	 */

	spin_lock_irqsave(&bc->bc_lock, flags);
	EIO_ASSERT(bc->bc_mdwait > 0);
	nebio = bc->bc_mdlist;
	pebio = NULL;
	while (nebio) {
		if (nebio->eb_cacheset > ebio->eb_cacheset)
			break;
		pebio = nebio;
		nebio = nebio->eb_next;
	}
	ebio->eb_next = nebio;
	if (!pebio)
		bc->bc_mdlist = ebio;
	else
		pebio->eb_next = ebio;
	bc->bc_mdwait--;
	if (bc->bc_mdwait == 0)
		eio_enq_mdupdate(bc);
	spin_unlock_irqrestore(&bc->bc_lock, flags);

	eio_free_cache_job(job);
}

/* Ensure cache level dirty thresholds compliance. If required, trigger cache-wide clean */
static void eio_check_dirty_cache_thresholds(struct cache_c *dmc)
{
	if (DIRTY_CACHE_THRESHOLD_CROSSED(dmc)) {
		int64_t required_cleans;
		int64_t enqueued_cleans;
		u_int64_t set_time;
		index_t set_index;
		unsigned long flags;

		spin_lock_irqsave(&dmc->clean_sl, flags);
		if (atomic64_read(&dmc->clean_pendings)
		    || dmc->clean_excess_dirty) {
			/* Already excess dirty block cleaning is in progress */
			spin_unlock_irqrestore(&dmc->clean_sl, flags);
			return;
		}
		dmc->clean_excess_dirty = 1;
		spin_unlock_irqrestore(&dmc->clean_sl, flags);

		/* Clean needs to be triggered on the cache */
		required_cleans = atomic64_read(&dmc->nr_dirty) -
				  (EIO_DIV((dmc->sysctl_active.dirty_low_threshold * dmc->size),
					   100));
		enqueued_cleans = 0;

		spin_lock_irqsave(&dmc->dirty_set_lru_lock, flags);
		do {
			lru_rem_head(dmc->dirty_set_lru, &set_index, &set_time);
			if (set_index == LRU_NULL)
				break;

			enqueued_cleans += dmc->cache_sets[set_index].nr_dirty;
			spin_unlock_irqrestore(&dmc->dirty_set_lru_lock, flags);
			eio_addto_cleanq(dmc, set_index, 1);
			spin_lock_irqsave(&dmc->dirty_set_lru_lock, flags);
		} while (enqueued_cleans <= required_cleans);
		spin_unlock_irqrestore(&dmc->dirty_set_lru_lock, flags);
		spin_lock_irqsave(&dmc->clean_sl, flags);
		dmc->clean_excess_dirty = 0;
		spin_unlock_irqrestore(&dmc->clean_sl, flags);
	}
}

/* Ensure set level dirty thresholds compliance. If required, trigger set clean */
static void eio_check_dirty_set_thresholds(struct cache_c *dmc, index_t set)
{
	if (DIRTY_SET_THRESHOLD_CROSSED(dmc, set)) {
		eio_addto_cleanq(dmc, set, 0);
		return;
	}
}

/* Ensure various cache thresholds compliance. If required trigger clean */
void eio_comply_dirty_thresholds(struct cache_c *dmc, index_t set)
{
	/*
	 * 1. Don't trigger new cleanings if
	 *      - cache is not wb
	 *      - autoclean threshold is crossed
	 *      - fast remove in progress is set
	 *      - cache is in failed mode.
	 * 2. Initiate set-wide clean, if set level dirty threshold is crossed
	 * 3. Initiate cache-wide clean, if cache level dirty threshold is crossed
	 */

	if (unlikely(CACHE_FAILED_IS_SET(dmc))) {
		pr_debug
			("eio_comply_dirty_thresholds: Cache %s is in failed mode.\n",
			dmc->cache_name);
		return;
	}

	if (AUTOCLEAN_THRESHOLD_CROSSED(dmc) || (dmc->mode != CACHE_MODE_WB))
		return;

	if (set != -1)
		eio_check_dirty_set_thresholds(dmc, set);
	eio_check_dirty_cache_thresholds(dmc);
}

/* Do read from cache */
static void
eio_cached_read(struct cache_c *dmc, struct eio_bio *ebio, int rw_flags)
{
	struct kcached_job *job;
	index_t index = ebio->eb_index;
	int err = 0;

	job = eio_new_job(dmc, ebio, index);

	if (unlikely(job == NULL))
		err = -ENOMEM;
	else {
		job->action = READCACHE;        /* Fetch data from cache */
		atomic_inc(&dmc->nr_jobs);

		SECTOR_STATS(dmc->eio_stats.read_hits, ebio->eb_size);
		SECTOR_STATS(dmc->eio_stats.ssd_reads, ebio->eb_size);
		atomic64_inc(&dmc->eio_stats.readcache);
		err =
			eio_io_async_bvec(dmc, &job->job_io_regions.cache, rw_flags,
					  ebio->eb_bv, ebio->eb_nbvec,
					  eio_io_callback, job, 0);

	}
	if (err) {
		unsigned long flags;
		pr_err("eio_cached_read: IO submission failed, block %llu",
		       EIO_DBN_GET(dmc, index));
		spin_lock_irqsave(&dmc->cache_sets[ebio->eb_cacheset].cs_lock,
				  flags);
		/*
		 * For already DIRTY block, invalidation is too costly, skip it.
		 * For others, mark the block as INVALID and return error.
		 */
		if (EIO_CACHE_STATE_GET(dmc, ebio->eb_index) != ALREADY_DIRTY) {
			EIO_CACHE_STATE_SET(dmc, ebio->eb_index, INVALID);
			atomic64_dec_if_positive(&dmc->eio_stats.cached_blocks);
		}
		spin_unlock_irqrestore(&dmc->cache_sets[ebio->eb_cacheset].
				       cs_lock, flags);
		eb_endio(ebio, err);
		ebio = NULL;
		if (job) {
			job->ebio = NULL;
			eio_free_cache_job(job);
			job = NULL;
		}
	}
}

/*
 * Invalidate any colliding blocks if they are !BUSY and !DIRTY.  In BUSY case,
 * we need to wait until the underlying IO is finished, and then proceed with
 * the invalidation, so a QUEUED flag is added.
 */
static int
eio_inval_block_set_range(struct cache_c *dmc, int set, sector_t iosector,
			  unsigned iosize, int multiblk)
{
	int start_index, end_index, i;
	sector_t endsector = iosector + eio_to_sector(iosize);

	start_index = dmc->assoc * set;
	end_index = start_index + dmc->assoc;
	for (i = start_index; i < end_index; i++) {
		sector_t start_dbn;
		sector_t end_dbn;

		if (EIO_CACHE_STATE_GET(dmc, i) & INVALID)
			continue;
		start_dbn = EIO_DBN_GET(dmc, i);
		end_dbn = start_dbn + dmc->block_size;

		if (!(endsector <= start_dbn || iosector >= end_dbn)) {

			if (!
			    (EIO_CACHE_STATE_GET(dmc, i) &
			     (BLOCK_IO_INPROG | DIRTY | QUEUED))) {
				EIO_CACHE_STATE_SET(dmc, i, INVALID);
				atomic64_dec_if_positive(&dmc->eio_stats.
							 cached_blocks);
				if (multiblk)
					continue;
				return 0;
			}

			/* Skip queued flag for DIRTY(inprog or otherwise) blocks. */
			if (!(EIO_CACHE_STATE_GET(dmc, i) & (DIRTY | QUEUED)))
				/* BLOCK_IO_INPROG is set. Set QUEUED flag */
				EIO_CACHE_STATE_ON(dmc, i, QUEUED);

			if (!multiblk)
				return 1;
		}
	}
	return 0;
}

int
eio_invalidate_sanity_check(struct cache_c *dmc, u_int64_t iosector,
			    u_int64_t *num_sectors)
{
	u_int64_t disk_size;

	/*
	 * Sanity check the arguements
	 */
	if (unlikely(*num_sectors == 0)) {
		pr_info
			("invaldate_sector_range: nothing to do because number of sectors specified is zero");
		return -EINVAL;
	}

	disk_size = eio_to_sector(eio_get_device_size(dmc->disk_dev));
	if (iosector >= disk_size) {
		pr_err
			("eio_inval_range: nothing to do because starting sector is past last sector (%lu > %lu)",
			(long unsigned int)iosector, (long unsigned int)disk_size);
		return -EINVAL;
	}

	if ((iosector + (*num_sectors)) > disk_size) {
		pr_info
			("eio_inval_range: trimming range because there are less sectors to invalidate than requested. (%lu < %lu)",
			(long unsigned int)(disk_size - iosector),
			(long unsigned int)*num_sectors);
		*num_sectors = (disk_size - iosector);
	}

	return 0;
}

void eio_inval_range(struct cache_c *dmc, sector_t iosector, unsigned iosize)
{
	u_int32_t bset;
	sector_t snum;
	sector_t snext;
	unsigned ioinset;
	unsigned long flags;
	int totalsshift = dmc->block_shift + dmc->consecutive_shift;

	snum = iosector;
	while (iosize) {
		bset = hash_block(dmc, snum);
		snext = ((snum >> totalsshift) + 1) << totalsshift;
		ioinset = (unsigned)to_bytes(snext - snum);
		if (ioinset > iosize)
			ioinset = iosize;
		spin_lock_irqsave(&dmc->cache_sets[bset].cs_lock, flags);
		eio_inval_block_set_range(dmc, bset, snum, ioinset, 1);
		spin_unlock_irqrestore(&dmc->cache_sets[bset].cs_lock, flags);
		snum = snext;
		iosize -= ioinset;
	}
}

/*
 * Invalidates all cached blocks without waiting for them to complete
 * Should be called with incoming IO suspended
 */
int eio_invalidate_cache(struct cache_c *dmc)
{
	u_int64_t i = 0;
	unsigned long flags = 0;
	sector_t disk_dev_size = to_bytes(eio_get_device_size(dmc->disk_dev));

	/* invalidate the whole cache */
	for (i = 0; i < (dmc->size >> dmc->consecutive_shift); i++) {
		spin_lock_irqsave(&dmc->cache_sets[i].cs_lock, flags);
		/* TBD. Apply proper fix for the cast to disk_dev_size */
		(void)eio_inval_block_set_range(dmc, (int)i, 0,
						(unsigned)disk_dev_size, 0);
		spin_unlock_irqrestore(&dmc->cache_sets[i].cs_lock, flags);
	}                       /* end - for all cachesets (i) */

	return 0;               /* i suspect we may need to return different statuses in the future */
}                               /* eio_invalidate_cache */

static int eio_inval_block(struct cache_c *dmc, sector_t iosector)
{
	u_int32_t bset;
	int queued;

	/*Chop lower bits of iosector*/
	iosector = EIO_ROUND_SECTOR(dmc, iosector);
	bset = hash_block(dmc, iosector);
	queued = eio_inval_block_set_range(dmc, bset, iosector,
					   (unsigned)to_bytes(dmc->block_size),
					   0);

	return queued;
}

/* Serving write I/Os, that involves both SSD and HDD */
static int eio_uncached_write(struct cache_c *dmc, struct eio_bio *ebio)
{
	struct kcached_job *job;
	int err = 0;
	index_t index = ebio->eb_index;
	unsigned long flags = 0;
	u_int8_t cstate;

	if (index == -1) {
		/*
		 * No work, if block is not allocated.
		 * Ensure, invalidation of the block at the end
		 */
		ebio->eb_iotype |= EB_INVAL;
		return 0;
	}

	spin_lock_irqsave(&dmc->cache_sets[ebio->eb_cacheset].cs_lock, flags);
	cstate = EIO_CACHE_STATE_GET(dmc, index);
	EIO_ASSERT(cstate & (DIRTY | CACHEWRITEINPROG));
	if (cstate == ALREADY_DIRTY) {
		/*
		 * Treat the dirty block cache write failure as
		 * I/O failure for the entire I/O
		 * TBD
		 * Can we live without this restriction
		 */
		ebio->eb_iotype = EB_MAIN_IO;

		/*
		 * We don't set inprog flag on dirty block.
		 * In lieu of the inprog flag, we are using the
		 * eb_holdcount for dirty block, so that the
		 * endio can be called, only when the write to disk
		 * and the write to cache both complete for the ebio
		 */
		atomic_inc(&ebio->eb_holdcount);
	} else
		/* ensure DISKWRITEINPROG for uncached write on non-DIRTY blocks */
		EIO_CACHE_STATE_ON(dmc, index, DISKWRITEINPROG);

	spin_unlock_irqrestore(&dmc->cache_sets[ebio->eb_cacheset].cs_lock,
			       flags);

	job = eio_new_job(dmc, ebio, index);
	if (unlikely(job == NULL))
		err = -ENOMEM;
	else {
		job->action = WRITECACHE;
		SECTOR_STATS(dmc->eio_stats.ssd_writes, ebio->eb_size);
		atomic64_inc(&dmc->eio_stats.writecache);
		err = eio_io_async_bvec(dmc, &job->job_io_regions.cache, WRITE,
					ebio->eb_bv, ebio->eb_nbvec,
					eio_io_callback, job, 0);
	}

	if (err) {
		pr_err("eio_uncached_write: IO submission failed, block %llu",
		       EIO_DBN_GET(dmc, index));
		spin_lock_irqsave(&dmc->cache_sets[ebio->eb_cacheset].cs_lock,
				  flags);
		if (EIO_CACHE_STATE_GET(dmc, ebio->eb_index) == ALREADY_DIRTY)
			/*
			 * Treat I/O failure on a DIRTY block as failure of entire I/O.
			 * TBD
			 * Can do better error handling by invalidation of the dirty
			 * block, if the cache block write failed, but disk write succeeded
			 */
			ebio->eb_bc->bc_error = err;
		else {
			/* Mark the block as INVALID for non-DIRTY block. */
			EIO_CACHE_STATE_SET(dmc, ebio->eb_index, INVALID);
			atomic64_dec_if_positive(&dmc->eio_stats.cached_blocks);
			/* Set the INVAL flag to ensure block is marked invalid at the end */
			ebio->eb_iotype |= EB_INVAL;
			ebio->eb_index = -1;
		}
		spin_unlock_irqrestore(&dmc->cache_sets[ebio->eb_cacheset].
				       cs_lock, flags);
		if (job) {
			job->ebio = NULL;
			eio_free_cache_job(job);
			job = NULL;
		}
	}

	return err;
}

/* Serving write I/Os that can be fulfilled just by SSD */
static int
eio_cached_write(struct cache_c *dmc, struct eio_bio *ebio, int rw_flags)
{
	struct kcached_job *job;
	int err = 0;
	index_t index = ebio->eb_index;
	unsigned long flags = 0;
	u_int8_t cstate;

	/*
	 * WRITE (I->DV)
	 * WRITE (V->DV)
	 * WRITE (V1->DV2)
	 * WRITE (DV->DV)
	 */

	/* Possible only in writeback caching mode */
	EIO_ASSERT(dmc->mode == CACHE_MODE_WB);

	/*
	 * TBD
	 * Possibly don't need the spinlock-unlock here
	 */
	spin_lock_irqsave(&dmc->cache_sets[ebio->eb_cacheset].cs_lock, flags);
	cstate = EIO_CACHE_STATE_GET(dmc, index);
	if (!(cstate & DIRTY)) {
		EIO_ASSERT(cstate & CACHEWRITEINPROG);
		/* make sure the block is marked DIRTY inprogress */
		EIO_CACHE_STATE_SET(dmc, index, DIRTY_INPROG);
	}
	spin_unlock_irqrestore(&dmc->cache_sets[ebio->eb_cacheset].cs_lock,
			       flags);

	job = eio_new_job(dmc, ebio, index);
	if (unlikely(job == NULL))
		err = -ENOMEM;
	else {
		job->action = WRITECACHE;

		SECTOR_STATS(dmc->eio_stats.ssd_writes, ebio->eb_size);
		atomic64_inc(&dmc->eio_stats.writecache);
		EIO_ASSERT((rw_flags & 1) == WRITE);
		err =
			eio_io_async_bvec(dmc, &job->job_io_regions.cache, rw_flags,
					  ebio->eb_bv, ebio->eb_nbvec,
					  eio_io_callback, job, 0);

	}

	if (err) {
		pr_err("eio_cached_write: IO submission failed, block %llu",
		       EIO_DBN_GET(dmc, index));
		spin_lock_irqsave(&dmc->cache_sets[ebio->eb_cacheset].cs_lock,
				  flags);
		cstate = EIO_CACHE_STATE_GET(dmc, index);
		if (cstate == DIRTY_INPROG) {
			/* A DIRTY(inprog) block should be invalidated on error */
			EIO_CACHE_STATE_SET(dmc, ebio->eb_index, INVALID);
			atomic64_dec_if_positive(&dmc->eio_stats.cached_blocks);
		} else
			/* An already DIRTY block don't have an option but just return error. */
			EIO_ASSERT(cstate == ALREADY_DIRTY);
		spin_unlock_irqrestore(&dmc->cache_sets[ebio->eb_cacheset].
				       cs_lock, flags);
		eb_endio(ebio, err);
		ebio = NULL;
		if (job) {
			job->ebio = NULL;
			eio_free_cache_job(job);
			job = NULL;
		}
	}

	return err;
}

static struct eio_bio *eio_new_ebio(struct cache_c *dmc, struct bio *bio,
				    unsigned *presidual_biovec, sector_t snum,
				    int iosize, struct bio_container *bc,
				    int iotype)
{
	struct eio_bio *ebio;
	int residual_biovec = *presidual_biovec;
	int numbvecs = 0;
	int ios;

	if (residual_biovec) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
		int bvecindex = bio->bi_iter.bi_idx;
#else 
		int bvecindex = bio->bi_idx;
#endif 
		int rbvindex;

		/* Calculate the number of bvecs required */
		ios = iosize;
		while (ios > 0) {
			int len;

			if (ios == iosize)
				len =
					bio->bi_io_vec[bvecindex].bv_len -
					residual_biovec;
			else
				len = bio->bi_io_vec[bvecindex].bv_len;

			numbvecs++;
			if (len > ios)
				len = ios;
			ios -= len;
			bvecindex++;
		}
		ebio =
			kmalloc(sizeof(struct eio_bio) +
				numbvecs * sizeof(struct bio_vec), GFP_NOWAIT);

		if (!ebio)
			return ERR_PTR(-ENOMEM);

		rbvindex = 0;
		ios = iosize;
		while (ios > 0) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
			ebio->eb_rbv[rbvindex].bv_page =
				bio->bi_io_vec[bio->bi_iter.bi_idx].bv_page;
			ebio->eb_rbv[rbvindex].bv_offset =
				bio->bi_io_vec[bio->bi_iter.bi_idx].bv_offset +
				residual_biovec;
			ebio->eb_rbv[rbvindex].bv_len =
				bio->bi_io_vec[bio->bi_iter.bi_idx].bv_len -
				residual_biovec;
#else 
			ebio->eb_rbv[rbvindex].bv_page =
				bio->bi_io_vec[bio->bi_idx].bv_page;
			ebio->eb_rbv[rbvindex].bv_offset =
				bio->bi_io_vec[bio->bi_idx].bv_offset +
				residual_biovec;
			ebio->eb_rbv[rbvindex].bv_len =
				bio->bi_io_vec[bio->bi_idx].bv_len -
				residual_biovec;
#endif 
			if (ebio->eb_rbv[rbvindex].bv_len > (unsigned)ios) {
				residual_biovec += ios;
				ebio->eb_rbv[rbvindex].bv_len = ios;
			} else {
				residual_biovec = 0;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
				bio->bi_iter.bi_idx++;
#else 
				bio->bi_idx++;
#endif 
			}
			ios -= ebio->eb_rbv[rbvindex].bv_len;
			rbvindex++;
		}
		EIO_ASSERT(rbvindex == numbvecs);
		ebio->eb_bv = ebio->eb_rbv;
	} else {
		ebio = kmalloc(sizeof(struct eio_bio), GFP_NOWAIT);

		if (!ebio)
			return ERR_PTR(-ENOMEM);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
		ebio->eb_bv = bio->bi_io_vec + bio->bi_iter.bi_idx;
#else 
		ebio->eb_bv = bio->bi_io_vec + bio->bi_idx;
#endif 
		ios = iosize;
		while (ios > 0) {
			numbvecs++;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
			if ((unsigned)ios < bio->bi_io_vec[bio->bi_iter.bi_idx].bv_len) {
#else 
			if ((unsigned)ios < bio->bi_io_vec[bio->bi_idx].bv_len) {
#endif 
				residual_biovec = ios;
				ios = 0;
			} else {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
				ios -= bio->bi_io_vec[bio->bi_iter.bi_idx].bv_len;
				bio->bi_iter.bi_idx++;
#else 
				ios -= bio->bi_io_vec[bio->bi_idx].bv_len;
				bio->bi_idx++;
#endif 
			}
		}
	}
	EIO_ASSERT(ios == 0);
	EIO_ASSERT(numbvecs != 0);
	*presidual_biovec = residual_biovec;

	ebio->eb_sector = snum;
	ebio->eb_cacheset = hash_block(dmc, snum);
	ebio->eb_size = iosize;
	ebio->eb_dir = bio_data_dir(bio);
	ebio->eb_next = NULL;
	ebio->eb_index = -1;
	ebio->eb_iotype = iotype;
	ebio->eb_nbvec = numbvecs;

	bc_addfb(bc, ebio);

	/* Always set the holdcount for eb to 1, to begin with. */
	atomic_set(&ebio->eb_holdcount, 1);

	return ebio;
}

/* Issues HDD I/O */
static void
eio_disk_io(struct cache_c *dmc, struct bio *bio,
	    struct eio_bio *anchored_bios, struct bio_container *bc,
	    int force_inval)
{
	struct eio_bio *ebio;
	struct kcached_job *job;
	int residual_biovec = 0;
	int error = 0;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
	/*disk io happens on whole bio. Reset bi_iter.bi_idx*/
	bio->bi_iter.bi_idx = 0;
	ebio =
		eio_new_ebio(dmc, bio, &residual_biovec, bio->bi_iter.bi_sector,
				 bio->bi_iter.bi_size, bc, EB_MAIN_IO);
#else 
	/*disk io happens on whole bio. Reset bi_idx*/
	bio->bi_idx = 0;
	ebio =
		eio_new_ebio(dmc, bio, &residual_biovec, bio->bi_sector,
			     bio->bi_size, bc, EB_MAIN_IO);
#endif 

	if (unlikely(IS_ERR(ebio))) {
		bc->bc_error = error = PTR_ERR(ebio);
		ebio = NULL;
		goto errout;
	}

	if (force_inval)
		ebio->eb_iotype |= EB_INVAL;
	ebio->eb_next = anchored_bios;  /*Anchor the ebio list to this super bio*/
	job = eio_new_job(dmc, ebio, -1);

	if (unlikely(job == NULL)) {
		error = -ENOMEM;
		goto errout;
	}
	atomic_inc(&dmc->nr_jobs);
	if (ebio->eb_dir == READ) {
		job->action = READDISK;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
		SECTOR_STATS(dmc->eio_stats.disk_reads, bio->bi_iter.bi_size);
#else 
		SECTOR_STATS(dmc->eio_stats.disk_reads, bio->bi_size);
#endif 
		atomic64_inc(&dmc->eio_stats.readdisk);
	} else {
		job->action = WRITEDISK;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
		SECTOR_STATS(dmc->eio_stats.disk_writes, bio->bi_iter.bi_size);
#else 
		SECTOR_STATS(dmc->eio_stats.disk_writes, bio->bi_size);
#endif 
		atomic64_inc(&dmc->eio_stats.writedisk);
	}

	/*
	 * Pass the original bio flags as is, while doing
	 * read / write to HDD.
	 */
	VERIFY_BIO_FLAGS(ebio);
	error = eio_io_async_bvec(dmc, &job->job_io_regions.disk,
				  GET_BIO_FLAGS(ebio),
				  ebio->eb_bv, ebio->eb_nbvec,
				  eio_io_callback, job, 1);

	if (error) {
		job->ebio = NULL;
		eio_free_cache_job(job);
		goto errout;
	}
	return;

errout:
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
	eio_inval_range(dmc, bio->bi_iter.bi_sector, bio->bi_iter.bi_size);
#else 
	eio_inval_range(dmc, bio->bi_sector, bio->bi_size);
#endif 
	eio_flag_abios(dmc, anchored_bios, error);

	if (ebio)
		eb_endio(ebio, error);
	return;
}

/*Given a sector number and biosize, returns cache io size*/
static unsigned int
eio_get_iosize(struct cache_c *dmc, sector_t snum, unsigned int biosize)
{
	unsigned int iosize;
	unsigned int swithinblock = snum & (dmc->block_size - 1);

	/*Check whether io starts at a cache block boundary*/
	if (swithinblock)
		iosize = (unsigned int)to_bytes(dmc->block_size - swithinblock);
	else
		iosize = (unsigned int)to_bytes(dmc->block_size);
	if (iosize > biosize)
		iosize = biosize;
	return iosize;
}

/* Insert a new set sequence in sorted order to existing set sequence list */
static int
insert_set_seq(struct set_seq **seq_list, index_t first_set, index_t last_set)
{
	struct set_seq *cur_seq = NULL;
	struct set_seq *prev_seq = NULL;
	struct set_seq *new_seq = NULL;

	EIO_ASSERT((first_set != -1) && (last_set != -1)
		   && (last_set >= first_set));

	for (cur_seq = *seq_list; cur_seq;
	     prev_seq = cur_seq, cur_seq = cur_seq->next) {
		if (first_set > cur_seq->last_set)
			/* go for the next seq in the sorted seq list */
			continue;

		if (last_set < cur_seq->first_set)
			/* break here to insert the new seq to seq list at this point */
			break;

		/*
		 * There is an overlap of the new seq with the current seq.
		 * Adjust the first_set field of the current seq to consume
		 * the overlap.
		 */
		if (first_set < cur_seq->first_set)
			cur_seq->first_set = first_set;

		if (last_set <= cur_seq->last_set)
			/* The current seq now fully encompasses the first and last sets */
			return 0;

		/* Increment the first set so as to start from, where the current seq left */
		first_set = cur_seq->last_set + 1;
	}

	new_seq = kmalloc(sizeof(struct set_seq), GFP_NOWAIT);
	if (new_seq == NULL)
		return -ENOMEM;
	new_seq->first_set = first_set;
	new_seq->last_set = last_set;
	if (prev_seq) {
		new_seq->next = prev_seq->next;
		prev_seq->next = new_seq;
	} else {
		new_seq->next = *seq_list;
		*seq_list = new_seq;
	}

	return 0;
}

/* Acquire read/shared lock for the sets covering the entire I/O range */
static int eio_acquire_set_locks(struct cache_c *dmc, struct bio_container *bc)
{
	struct bio *bio = bc->bc_bio;
	sector_t round_sector;
	sector_t end_sector;
	sector_t set_size;
	index_t cur_set;
	index_t first_set;
	index_t last_set;
	index_t i;
	struct set_seq *cur_seq;
	struct set_seq *next_seq;
	int error;

	/*
	 * Find first set using start offset of the I/O and lock it.
	 * Find next sets by adding the set offsets to the previous set
	 * Identify all the sequences of set numbers that need locking.
	 * Keep the sequences in sorted list.
	 * For each set in each sequence
	 * - acquire read lock on the set.
	 */

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
	round_sector = EIO_ROUND_SET_SECTOR(dmc, bio->bi_iter.bi_sector);
	set_size = dmc->block_size * dmc->assoc;
	end_sector = bio->bi_iter.bi_sector + eio_to_sector(bio->bi_iter.bi_size);
#else 
	round_sector = EIO_ROUND_SET_SECTOR(dmc, bio->bi_sector);
	set_size = dmc->block_size * dmc->assoc;
	end_sector = bio->bi_sector + eio_to_sector(bio->bi_size);
#endif 
	first_set = -1;
	last_set = -1;
	cur_set = -1;
	bc->bc_setspan = NULL;

	while (round_sector < end_sector) {
		cur_set = hash_block(dmc, round_sector);
		if (first_set == -1) {
			first_set = cur_set;
			last_set = cur_set;
		} else if (cur_set == (last_set + 1))
			last_set = cur_set;
		else {
			/*
			 * Add the seq of start, end set to sorted (first, last) seq list
			 * and reinit the first and last set
			 */
			error =
				insert_set_seq(&bc->bc_setspan, first_set,
					       last_set);
			if (error)
				goto err_out;
			first_set = cur_set;
			last_set = cur_set;
		}

		round_sector += set_size;
	}

	/* Add the remaining first, last set sequence */

	EIO_ASSERT((first_set != -1) && (last_set == cur_set));

	if (bc->bc_setspan == NULL) {
		/* No sequence was added, can use singlespan */
		cur_seq = &bc->bc_singlesspan;
		cur_seq->first_set = first_set;
		cur_seq->last_set = last_set;
		cur_seq->next = NULL;
		bc->bc_setspan = cur_seq;
	} else {
		error = insert_set_seq(&bc->bc_setspan, first_set, last_set);
		if (error)
			goto err_out;
	}

	/* Acquire read locks on the sets in the set span */
	for (cur_seq = bc->bc_setspan; cur_seq; cur_seq = cur_seq->next)
		for (i = cur_seq->first_set; i <= cur_seq->last_set; i++)
			down_read(&dmc->cache_sets[i].rw_lock);

	return 0;

err_out:

	/* Free the seqs in the seq list, unless it is just the local seq */
	if (bc->bc_setspan != &bc->bc_singlesspan) {
		for (cur_seq = bc->bc_setspan; cur_seq; cur_seq = next_seq) {
			next_seq = cur_seq->next;
			kfree(cur_seq);
		}
	}
	return error;
}

/*
 * Allocate mdreq and md_blocks for each set.
 */
static int eio_alloc_mdreqs(struct cache_c *dmc, struct bio_container *bc)
{
	index_t i;
	struct mdupdate_request *mdreq;
	int nr_bvecs, ret;
	struct set_seq *cur_seq;

	bc->mdreqs = NULL;

	for (cur_seq = bc->bc_setspan; cur_seq; cur_seq = cur_seq->next) {
		for (i = cur_seq->first_set; i <= cur_seq->last_set; i++) {
			mdreq = kzalloc(sizeof(*mdreq), GFP_NOWAIT);
			if (mdreq) {
				mdreq->md_size =
					dmc->assoc *
					sizeof(struct flash_cacheblock);
				nr_bvecs =
					IO_BVEC_COUNT(mdreq->md_size,
						      SECTORS_PER_PAGE);

				mdreq->mdblk_bvecs =
					(struct bio_vec *)
					kmalloc(sizeof(struct bio_vec) * nr_bvecs,
						GFP_KERNEL);
				if (mdreq->mdblk_bvecs) {

					ret =
						eio_alloc_wb_bvecs(mdreq->
								   mdblk_bvecs,
								   nr_bvecs,
								   SECTORS_PER_PAGE);
					if (ret) {
						pr_err
							("eio_alloc_mdreqs: failed to allocated pages\n");
						kfree(mdreq->mdblk_bvecs);
						mdreq->mdblk_bvecs = NULL;
					}
					mdreq->mdbvec_count = nr_bvecs;
				}
			}

			if (unlikely
				    ((mdreq == NULL) || (mdreq->mdblk_bvecs == NULL))) {
				struct mdupdate_request *nmdreq;

				mdreq = bc->mdreqs;
				while (mdreq) {
					nmdreq = mdreq->next;
					if (mdreq->mdblk_bvecs) {
						eio_free_wb_bvecs(mdreq->
								  mdblk_bvecs,
								  mdreq->
								  mdbvec_count,
								  SECTORS_PER_PAGE);
						kfree(mdreq->mdblk_bvecs);
					}
					kfree(mdreq);
					mdreq = nmdreq;
				}
				bc->mdreqs = NULL;
				return -ENOMEM;
			} else {
				mdreq->next = bc->mdreqs;
				bc->mdreqs = mdreq;
			}
		}
	}

	return 0;

}

/*
 * Release:
 * 1. the set locks covering the entire I/O range
 * 2. any previously allocated memory for md update
 */
static int
eio_release_io_resources(struct cache_c *dmc, struct bio_container *bc)
{
	index_t i;
	struct mdupdate_request *mdreq;
	struct mdupdate_request *nmdreq;
	struct set_seq *cur_seq;
	struct set_seq *next_seq;

	/* Release read locks on the sets in the set span */
	for (cur_seq = bc->bc_setspan; cur_seq; cur_seq = cur_seq->next)
		for (i = cur_seq->first_set; i <= cur_seq->last_set; i++)
			up_read(&dmc->cache_sets[i].rw_lock);

	/* Free the seqs in the set span, unless it is single span */
	if (bc->bc_setspan != &bc->bc_singlesspan) {
		for (cur_seq = bc->bc_setspan; cur_seq; cur_seq = next_seq) {
			next_seq = cur_seq->next;
			kfree(cur_seq);
		}
	}

	mdreq = bc->mdreqs;
	while (mdreq) {
		nmdreq = mdreq->next;
		if (mdreq->mdblk_bvecs) {
			eio_free_wb_bvecs(mdreq->mdblk_bvecs,
					  mdreq->mdbvec_count,
					  SECTORS_PER_PAGE);
			kfree(mdreq->mdblk_bvecs);
		}
		kfree(mdreq);
		mdreq = nmdreq;
	}
	bc->mdreqs = NULL;

	return 0;
}

/*
 * Decide the mapping and perform necessary cache operations for a bio request.
 */
int eio_map(struct cache_c *dmc, struct request_queue *rq, struct bio *bio)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
	sector_t sectors = eio_to_sector(bio->bi_iter.bi_size);
#else 
	sector_t sectors = eio_to_sector(bio->bi_size);
#endif 
	struct eio_bio *ebio = NULL;
	struct bio_container *bc;
	sector_t snum;
	unsigned int iosize;
	unsigned int totalio;
	unsigned int biosize;
	unsigned int residual_biovec;
	unsigned int force_uncached = 0;
	int data_dir = bio_data_dir(bio);

	/*bio list*/
	struct eio_bio *ebegin = NULL;
	struct eio_bio *eend = NULL;
	struct eio_bio *enext = NULL;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
	EIO_ASSERT(bio->bi_iter.bi_idx == 0);
#else 
	EIO_ASSERT(bio->bi_idx == 0);
#endif 

	pr_debug("this needs to be removed immediately\n");

	if (bio_rw_flagged(bio, REQ_DISCARD)) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
		pr_debug
			("eio_map: Discard IO received. Invalidate incore start=%lu totalsectors=%d.\n",
			(unsigned long)bio->bi_iter.bi_sector,
			(int)eio_to_sector(bio->bi_iter.bi_size));
#else 
		pr_debug
			("eio_map: Discard IO received. Invalidate incore start=%lu totalsectors=%d.\n",
			(unsigned long)bio->bi_sector,
			(int)eio_to_sector(bio->bi_size));
#endif 
		bio_endio(bio, 0);
		pr_err
			("eio_map: I/O with Discard flag received. Discard flag is not supported.\n");
		return 0;
	}

	if (unlikely(dmc->cache_rdonly)) {
		if (data_dir != READ) {
			bio_endio(bio, -EPERM);
			pr_debug
				("eio_map: cache is read only, write not permitted\n");
			return 0;
		}
	}

	if (sectors < SIZE_HIST)
		atomic64_inc(&dmc->size_hist[sectors]);

	if (data_dir == READ) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
		SECTOR_STATS(dmc->eio_stats.reads, bio->bi_iter.bi_size);
#else 
		SECTOR_STATS(dmc->eio_stats.reads, bio->bi_size);
#endif 
		atomic64_inc(&dmc->eio_stats.readcount);
	} else {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
		SECTOR_STATS(dmc->eio_stats.writes, bio->bi_iter.bi_size);
#else 
		SECTOR_STATS(dmc->eio_stats.writes, bio->bi_size);
#endif 
		atomic64_inc(&dmc->eio_stats.writecount);
	}

	/*
	 * Cache FAILED mode is like Hard failure.
	 * Dont allow I/Os to go through.
	 */
	if (unlikely(CACHE_FAILED_IS_SET(dmc))) {
		/*ASK confirm that once failed is set, it's never reset*/
		/* Source device is not available. */
		CTRACE
			("eio_map:2 source device is not present. Cache is in Failed state\n");
		bio_endio(bio, -ENODEV);
		bio = NULL;
		return DM_MAPIO_SUBMITTED;
	}

	/* WB cache will never be in degraded mode. */
	if (unlikely(CACHE_DEGRADED_IS_SET(dmc))) {
		EIO_ASSERT(dmc->mode != CACHE_MODE_WB);
		force_uncached = 1;
	} else if (data_dir == WRITE && dmc->mode == CACHE_MODE_RO) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
		if (to_sector(bio->bi_iter.bi_size) != dmc->block_size)
#else
		if (to_sector(bio->bi_size) != dmc->block_size)
#endif
			atomic64_inc(&dmc->eio_stats.uncached_map_size);
		else
			atomic64_inc(&dmc->eio_stats.uncached_map_uncacheable);
		force_uncached = 1;
	}

	/*
	 * Process zero sized bios by passing original bio flags
	 * to both HDD and SSD.
	 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
	if (bio->bi_iter.bi_size == 0) {
#else 
	if (bio->bi_size == 0) {
#endif 
		eio_process_zero_size_bio(dmc, bio);
		return DM_MAPIO_SUBMITTED;
	}

	/* Create a bio container */

	bc = kzalloc(sizeof(struct bio_container), GFP_NOWAIT);
	if (!bc) {
		bio_endio(bio, -ENOMEM);
		return DM_MAPIO_SUBMITTED;
	}
	bc->bc_iotime = jiffies;
	bc->bc_bio = bio;
	bc->bc_dmc = dmc;
	spin_lock_init(&bc->bc_lock);
	atomic_set(&bc->bc_holdcount, 1);
	bc->bc_error = 0;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0))
	snum = bio->bi_iter.bi_sector;
	totalio = bio->bi_iter.bi_size;
	biosize = bio->bi_iter.bi_size;
#else 
	snum = bio->bi_sector;
	totalio = bio->bi_size;
	biosize = bio->bi_size;
#endif 
	residual_biovec = 0;

	if (dmc->mode == CACHE_MODE_WB) {
		int ret;
		/*
		 * For writeback, the app I/O and the clean I/Os
		 * need to be exclusive for a cache set. Acquire shared
		 * lock on the cache set for app I/Os and exclusive
		 * lock on the cache set for clean I/Os.
		 */
		ret = eio_acquire_set_locks(dmc, bc);
		if (ret) {
			bio_endio(bio, ret);
			kfree(bc);
			return DM_MAPIO_SUBMITTED;
		}
	}

	atomic64_inc(&dmc->nr_ios);

	/*
	 * Prepare for I/O processing.
	 * - Allocate ebios.
	 * - For reads, identify if we need to do uncached read
	 * - If force uncached I/O is set, invalidate the cache blocks for the I/O
	 */

	if (force_uncached)
		eio_inval_range(dmc, snum, totalio);
	else {
		while (biosize) {
			iosize = eio_get_iosize(dmc, snum, biosize);
			ebio = eio_new_ebio(dmc, bio, &residual_biovec, snum,
					iosize, bc, EB_SUBORDINATE_IO);
			if (IS_ERR(ebio)) {
				bc->bc_error = -ENOMEM;
				break;
			}

			/* Anchor this ebio on ebio list. Preserve the order */
			if (ebegin)
				eend->eb_next = ebio;
			else
				ebegin = ebio;
			eend = ebio;

			biosize -= iosize;
			snum += eio_to_sector(iosize);
		}
	}

	if (bc->bc_error) {
		/* Error. Do ebio and bc cleanup. */
		ebio = ebegin;
		while (ebio) {
			enext = ebio->eb_next;
			eb_endio(ebio, bc->bc_error);
			ebio = enext;
		}

		/* By now, the bc_holdcount must be 1 */
		EIO_ASSERT(atomic_read(&bc->bc_holdcount) == 1);

		/* Goto out to cleanup the bc(in bc_put()) */
		goto out;
	}

	/*
	 * Start processing of the ebios.
	 *
	 * Note: don't return error from this point on.
	 *      Error handling would be done as part of
	 *      the processing of the ebios internally.
	 */
	if (force_uncached) {
		EIO_ASSERT(dmc->mode != CACHE_MODE_WB);
		if (data_dir == READ)
			atomic64_inc(&dmc->eio_stats.uncached_reads);
		else
			atomic64_inc(&dmc->eio_stats.uncached_writes);
		eio_disk_io(dmc, bio, ebegin, bc, 1);
	} else if (data_dir == READ) {

		/* read io processing */
		eio_read(dmc, bc, ebegin);
	} else
		/* write io processing */
		eio_write(dmc, bc, ebegin);

out:

	if (bc)
		bc_put(bc, 0);

	return DM_MAPIO_SUBMITTED;
}

/*
 * Checks the cache block state, for deciding cached/uncached read.
 * Also reserves/allocates the cache block, wherever necessary.
 *
 * Return values
 * 1: cache hit
 * 0: cache miss
 */
static int eio_read_peek(struct cache_c *dmc, struct eio_bio *ebio)
{
	index_t index;
	int res;
	int retval = 0;
	unsigned long flags;
	u_int8_t cstate;

	spin_lock_irqsave(&dmc->cache_sets[ebio->eb_cacheset].cs_lock, flags);

	res = eio_lookup(dmc, ebio, &index);
	ebio->eb_index = -1;

	if (res < 0) {
		atomic64_inc(&dmc->eio_stats.noroom);
		goto out;
	}

	cstate = EIO_CACHE_STATE_GET(dmc, index);

	if (cstate & (BLOCK_IO_INPROG | QUEUED))
		/*
		 * We found a valid or invalid block but an io is on, so we can't
		 * proceed. Don't invalidate it. This implies that we'll
		 * have to read from disk.
		 * Read on a DIRTY | INPROG block (block which is going to be DIRTY)
		 * is also redirected to read from disk.
		 */
		goto out;

	if (res == VALID) {
		EIO_ASSERT(cstate & VALID);
		if ((EIO_DBN_GET(dmc, index) ==
		     EIO_ROUND_SECTOR(dmc, ebio->eb_sector))) {
			/*
			 * Read/write should be done on already DIRTY block
			 * without any inprog flag.
			 * Ensure that a failure of DIRTY block read is propagated to app.
			 * non-DIRTY valid blocks should have inprog flag.
			 */
			if (cstate == ALREADY_DIRTY) {
				ebio->eb_iotype = EB_MAIN_IO;
				/*
				 * Set to uncached read and readfill for now.
				 * It may change to CACHED_READ later, if all
				 * the blocks are found to be cached
				 */
				ebio->eb_bc->bc_dir =
					UNCACHED_READ_AND_READFILL;
			} else
				EIO_CACHE_STATE_ON(dmc, index, CACHEREADINPROG);
			retval = 1;
			ebio->eb_index = index;
			goto out;
		}

		/* cache is marked readonly or set to wronly mode. */
		/* Do not allow READFILL on SSD */
		if (dmc->cache_rdonly || dmc->sysctl_active.cache_wronly)
			goto out;

		/*
		 * Found a block to be recycled.
		 * Its guranteed that it will be a non-DIRTY block
		 */
		EIO_ASSERT(!(cstate & DIRTY));
		if (eio_to_sector(ebio->eb_size) == dmc->block_size) {
			/*We can recycle and then READFILL only if iosize is block size*/
			atomic64_inc(&dmc->eio_stats.rd_replace);
			EIO_CACHE_STATE_SET(dmc, index, VALID | DISKREADINPROG);
			EIO_DBN_SET(dmc, index, (sector_t)ebio->eb_sector);
			ebio->eb_index = index;
			ebio->eb_bc->bc_dir = UNCACHED_READ_AND_READFILL;
		}
		goto out;
	}
	EIO_ASSERT(res == INVALID);
	
	/* cache is marked readonly or set to wronly mode. */
	/* Do not allow READFILL on SSD */
	if (dmc->cache_rdonly || dmc->sysctl_active.cache_wronly)
		goto out;
	/*
	 * Found an invalid block to be used.
	 * Can recycle only if iosize is block size
	 */
	if (eio_to_sector(ebio->eb_size) == dmc->block_size) {
		EIO_ASSERT(cstate & INVALID);
		EIO_CACHE_STATE_SET(dmc, index, VALID | DISKREADINPROG);
		atomic64_inc(&dmc->eio_stats.cached_blocks);
		EIO_DBN_SET(dmc, index, (sector_t)ebio->eb_sector);
		ebio->eb_index = index;
		ebio->eb_bc->bc_dir = UNCACHED_READ_AND_READFILL;
	}

out:

	spin_unlock_irqrestore(&dmc->cache_sets[ebio->eb_cacheset].cs_lock,
			       flags);

	/*
	 * Enqueue clean set if there is no room in the set
	 * TBD
	 * Ensure, a force clean
	 */
	if (res < 0)
		eio_comply_dirty_thresholds(dmc, ebio->eb_cacheset);

	return retval;
}

/*
 * Checks the cache block state, for deciding cached/uncached write.
 * Also reserves/allocates the cache block, wherever necessary.
 *
 * Return values
 * 1: cache block is available or newly allocated
 * 0: cache block could not be got for the ebio
 */
static int eio_write_peek(struct cache_c *dmc, struct eio_bio *ebio)
{
	index_t index;
	int res;
	int retval;
	u_int8_t cstate;
	unsigned long flags;

	spin_lock_irqsave(&dmc->cache_sets[ebio->eb_cacheset].cs_lock, flags);

	res = eio_lookup(dmc, ebio, &index);
	ebio->eb_index = -1;
	retval = 0;

	if (res < 0) {
		/* cache block not found and new block couldn't be allocated */
		atomic64_inc(&dmc->eio_stats.noroom);
		ebio->eb_iotype |= EB_INVAL;
		goto out;
	}

	cstate = EIO_CACHE_STATE_GET(dmc, index);

	if (cstate & (BLOCK_IO_INPROG | QUEUED)) {
		ebio->eb_iotype |= EB_INVAL;
		/* treat as if cache block is not available */
		goto out;
	}

	if ((res == VALID) && (EIO_DBN_GET(dmc, index) ==
			       EIO_ROUND_SECTOR(dmc, ebio->eb_sector))) {
		/*
		 * Cache hit.
		 * All except an already DIRTY block should have an INPROG flag.
		 * If it is a cached write, a DIRTY flag would be added later.
		 */
		SECTOR_STATS(dmc->eio_stats.write_hits, ebio->eb_size);
		if (cstate != ALREADY_DIRTY)
			EIO_CACHE_STATE_ON(dmc, index, CACHEWRITEINPROG);
		else
			atomic64_inc(&dmc->eio_stats.dirty_write_hits);
		ebio->eb_index = index;
		/*
		 * A VALID block should get upgraded to DIRTY, only when we
		 * are updating the entire cache block(not partially).
		 * Otherwise, 2 sequential partial writes can lead to missing
		 * data when one write upgrades the cache block to DIRTY, while
		 * the other just writes to HDD. Subsequent read would be
		 * served from the cache block, which won't have the data from
		 * 2nd write.
		 */
		if ((cstate == ALREADY_DIRTY) ||
		    (eio_to_sector(ebio->eb_size) == dmc->block_size))
			retval = 1;
		else
			retval = 0;
		goto out;

	}

	/*
	 * cache miss with a new block allocated for recycle.
	 * Set INPROG flag, if the ebio size is equal to cache block size
	 */
	EIO_ASSERT(!(EIO_CACHE_STATE_GET(dmc, index) & DIRTY));
	if (eio_to_sector(ebio->eb_size) == dmc->block_size) {
		if (res == VALID)
			atomic64_inc(&dmc->eio_stats.wr_replace);
		else
			atomic64_inc(&dmc->eio_stats.cached_blocks);
		EIO_CACHE_STATE_SET(dmc, index, VALID | CACHEWRITEINPROG);
		EIO_DBN_SET(dmc, index, (sector_t)ebio->eb_sector);
		ebio->eb_index = index;
		retval = 1;
	} else {
		/*
		 * eb iosize smaller than cache block size shouldn't
		 * do cache write on a cache miss
		 */
		retval = 0;
		ebio->eb_iotype |= EB_INVAL;
	}

out:
	if ((retval == 1) && (dmc->mode == CACHE_MODE_WB) &&
	    (cstate != ALREADY_DIRTY))
		ebio->eb_bc->bc_mdwait++;

	spin_unlock_irqrestore(&dmc->cache_sets[ebio->eb_cacheset].cs_lock,
			       flags);

	/*
	 * Enqueue clean set if there is no room in the set
	 * TBD
	 * Ensure, a force clean
	 */
	if (res < 0)
		eio_comply_dirty_thresholds(dmc, ebio->eb_cacheset);

	return retval;
}

/* Top level read function, called from eio_map */
static void
eio_read(struct cache_c *dmc, struct bio_container *bc, struct eio_bio *ebegin)
{
	int ucread = 0;
	struct eio_bio *ebio;
	struct eio_bio *enext;

	bc->bc_dir = UNCACHED_READ;
	ebio = ebegin;
	while (ebio) {
		enext = ebio->eb_next;
		if (eio_read_peek(dmc, ebio) == 0)
			ucread = 1;
		ebio = enext;
	}

	if (ucread) {
		/*
		 * Uncached read.
		 * Start HDD I/O. Once that is finished
		 * readfill or dirty block re-read would start
		 */
		atomic64_inc(&dmc->eio_stats.uncached_reads);
		eio_disk_io(dmc, bc->bc_bio, ebegin, bc, 0);
	} else {
		/* Cached read. Serve the read from SSD */

		/*
		 * Pass all orig bio flags except UNPLUG.
		 * Unplug in the end if flagged.
		 */
		int rw_flags;

		rw_flags = 0;

		bc->bc_dir = CACHED_READ;
		ebio = ebegin;

		VERIFY_BIO_FLAGS(ebio);

		EIO_ASSERT((rw_flags & 1) == READ);
		while (ebio) {
			enext = ebio->eb_next;
			ebio->eb_iotype = EB_MAIN_IO;

			eio_cached_read(dmc, ebio, rw_flags);
			ebio = enext;
		}
	}
}

/* Top level write function called from eio_map */
static void
eio_write(struct cache_c *dmc, struct bio_container *bc, struct eio_bio *ebegin)
{
	int ucwrite = 0;
	int error = 0;
	struct eio_bio *ebio;
	struct eio_bio *enext;

	if ((dmc->mode != CACHE_MODE_WB) ||
	    (dmc->sysctl_active.do_clean & EIO_CLEAN_KEEP))
		ucwrite = 1;

	ebio = ebegin;
	while (ebio) {
		enext = ebio->eb_next;
		if (eio_write_peek(dmc, ebio) == 0)
			ucwrite = 1;
		ebio = enext;
	}

	if (ucwrite) {
		/*
		 * Uncached write.
		 * Start both SSD and HDD writes
		 */
		atomic64_inc(&dmc->eio_stats.uncached_writes);
		bc->bc_mdwait = 0;
		bc->bc_dir = UNCACHED_WRITE;
		ebio = ebegin;
		while (ebio) {
			enext = ebio->eb_next;
			eio_uncached_write(dmc, ebio);
			ebio = enext;
		}

		eio_disk_io(dmc, bc->bc_bio, ebegin, bc, 0);
	} else {
		/* Cached write. Start writes to SSD blocks */

		int rw_flags;
		rw_flags = 0;

		bc->bc_dir = CACHED_WRITE;
		if (bc->bc_mdwait) {

			/*
			 * mdreqs are required only if the write would cause a metadata
			 * update.
			 */

			error = eio_alloc_mdreqs(dmc, bc);
		}

		/*
		 * Pass all orig bio flags except UNPLUG.
		 * UNPLUG in the end if flagged.
		 */
		ebio = ebegin;
		VERIFY_BIO_FLAGS(ebio);

		while (ebio) {
			enext = ebio->eb_next;
			ebio->eb_iotype = EB_MAIN_IO;

			if (!error) {

				eio_cached_write(dmc, ebio, WRITE | rw_flags);

			} else {
				unsigned long flags;
				u_int8_t cstate;

				pr_err
					("eio_write: IO submission failed, block %llu",
					EIO_DBN_GET(dmc, ebio->eb_index));
				spin_lock_irqsave(&dmc->
						  cache_sets[ebio->eb_cacheset].
						  cs_lock, flags);
				cstate =
					EIO_CACHE_STATE_GET(dmc, ebio->eb_index);
				if (cstate != ALREADY_DIRTY) {

					/*
					 * A DIRTY(inprog) block should be invalidated on error.
					 */

					EIO_CACHE_STATE_SET(dmc, ebio->eb_index,
							    INVALID);
					atomic64_dec_if_positive(&dmc->
								 eio_stats.
								 cached_blocks);
				}
				spin_unlock_irqrestore(&dmc->
						       cache_sets[ebio->
								  eb_cacheset].
						       cs_lock, flags);
				eb_endio(ebio, error);
			}
			ebio = enext;
		}
	}
}

/*
 * Synchronous clean of all the cache sets. Callers of this function needs
 * to handle the situation that clean operation was aborted midway.
 */

void eio_clean_all(struct cache_c *dmc)
{
	unsigned long flags = 0;

	EIO_ASSERT(dmc->mode == CACHE_MODE_WB);
	for (atomic_set(&dmc->clean_index, 0);
	     (atomic_read(&dmc->clean_index) <
	      (s32)(dmc->size >> dmc->consecutive_shift))
	     && (dmc->sysctl_active.do_clean & EIO_CLEAN_START)
	     && (atomic64_read(&dmc->nr_dirty) > 0)
	     && (!(dmc->cache_flags & CACHE_FLAGS_SHUTDOWN_INPROG)
		 && !dmc->sysctl_active.fast_remove);
	     atomic_inc(&dmc->clean_index)) {

		if (unlikely(CACHE_FAILED_IS_SET(dmc))) {
			pr_err("clean_all: CACHE \"%s\" is in FAILED state.",
			       dmc->cache_name);
			break;
		}

		eio_clean_set(dmc, (index_t)(atomic_read(&dmc->clean_index)),
				/* whole */ 1, /* force */ 1);
	}

	spin_lock_irqsave(&dmc->cache_spin_lock, flags);
	dmc->sysctl_active.do_clean &= ~EIO_CLEAN_START;
	spin_unlock_irqrestore(&dmc->cache_spin_lock, flags);
}

/*
 * Do unconditional clean of a cache.
 * Useful for a cold enabled writeback cache.
 */
void eio_clean_for_reboot(struct cache_c *dmc)
{
	index_t i;

	for (i = 0; i < (index_t)(dmc->size >> dmc->consecutive_shift); i++)
		eio_clean_set(dmc, i, /* whole */ 1, /* force */ 1);
}

/*
 * Used during the partial cache set clean.
 * Uses reclaim policy(LRU/FIFO) information to
 * identify the cache blocks that needs cleaning.
 * The number of such cache blocks is determined
 * by the high and low thresholds set.
 */
static void
eio_get_setblks_to_clean(struct cache_c *dmc, index_t set, int *ncleans)
{
	int i = 0;
	int max_clean;
	index_t start_index;
	int nr_writes = 0;

	*ncleans = 0;

	max_clean = dmc->cache_sets[set].nr_dirty -
		    ((dmc->sysctl_active.dirty_set_low_threshold * dmc->assoc) / 100);
	if (max_clean <= 0)
		/* Nothing to clean */
		return;

	start_index = set * dmc->assoc;

	/*
	 * Spinlock is not required here, as we assume that we have
	 * taken a write lock on the cache set, when we reach here
	 */
	if (dmc->policy_ops == NULL) {
		/* Scan sequentially in the set and pick blocks to clean */
		while ((i < (int)dmc->assoc) && (nr_writes < max_clean)) {
			if ((EIO_CACHE_STATE_GET(dmc, start_index + i) &
			     (DIRTY | BLOCK_IO_INPROG)) == DIRTY) {
				EIO_CACHE_STATE_ON(dmc, start_index + i,
						   DISKWRITEINPROG);
				nr_writes++;
			}
			i++;
		}
	} else
		nr_writes =
			eio_policy_clean_set(dmc->policy_ops, set, max_clean);

	*ncleans = nr_writes;
}

/* Callback function, when synchronous I/O completes */
static void eio_sync_io_callback(int error, void *context)
{
	struct sync_io_context *sioc = (struct sync_io_context *)context;

	if (error)
		sioc->sio_error = error;
	up_read(&sioc->sio_lock);
}

/*
 * Setup biovecs for preallocated biovecs per cache set.
 */

struct bio_vec *setup_bio_vecs(struct bio_vec *bvec, index_t block_index,
			       unsigned block_size, unsigned total,
			       unsigned *num_bvecs)
{
	struct bio_vec *data = NULL;
	index_t iovec_index;

	switch (block_size) {
	case BLKSIZE_2K:
		*num_bvecs = total;
		iovec_index = block_index;
		data = &bvec[iovec_index];
		break;

	case BLKSIZE_4K:
		*num_bvecs = total;
		iovec_index = block_index;
		data = &bvec[iovec_index];
		break;

	case BLKSIZE_8K:
		/*
		 * For 8k data block size, we need 2 bio_vecs
		 * per data block.
		 */
		*num_bvecs = total * 2;
		iovec_index = block_index * 2;
		data = &bvec[iovec_index];
		break;
	}

	return data;
}

/* Cleans a given cache set */
static void
eio_clean_set(struct cache_c *dmc, index_t set, int whole, int force)
{
	struct eio_io_region where;
	int error;
	index_t i;
	index_t j;
	index_t start_index;
	index_t end_index;
	struct sync_io_context sioc;
	int ncleans = 0;
	int alloc_size;
	struct flash_cacheblock *md_blocks = NULL;
	unsigned long flags;

	int pindex, k;
	index_t blkindex;
	struct bio_vec *bvecs;
	unsigned nr_bvecs = 0, total;
	void *pg_virt_addr[2] = { NULL };

	/* Cache is failed mode, do nothing. */
	if (unlikely(CACHE_FAILED_IS_SET(dmc))) {
		pr_debug("clean_set: CACHE \"%s\" is in FAILED state.",
			 dmc->cache_name);
		goto err_out1;
	}

	/* Nothing to clean, if there are no dirty blocks */
	if (dmc->cache_sets[set].nr_dirty == 0)
		goto err_out1;

	/* If this is not the suitable time to clean, postpone it */
	if ((!force) && AUTOCLEAN_THRESHOLD_CROSSED(dmc)) {
		eio_touch_set_lru(dmc, set);
		goto err_out1;
	}

	/*
	 * 1. Take exclusive lock on the cache set
	 * 2. Verify that there are dirty blocks to clean
	 * 3. Identify the cache blocks to clean
	 * 4. Read the cache blocks data from ssd
	 * 5. Write the cache blocks data to hdd
	 * 6. Update on-disk cache metadata
	 * 7. Update in-core cache metadata
	 */

	start_index = set * dmc->assoc;
	end_index = start_index + dmc->assoc;

	/* 1. exclusive lock. Let the ongoing writes to finish. Pause new writes */
	down_write(&dmc->cache_sets[set].rw_lock);

	/* 2. Return if there are no dirty blocks to clean */
	if (dmc->cache_sets[set].nr_dirty == 0)
		goto err_out2;

	/* 3. identify and mark cache blocks to clean */
	if (!whole)
		eio_get_setblks_to_clean(dmc, set, &ncleans);
	else {
		for (i = start_index; i < end_index; i++) {
			if (EIO_CACHE_STATE_GET(dmc, i) == ALREADY_DIRTY) {
				EIO_CACHE_STATE_SET(dmc, i, CLEAN_INPROG);
				ncleans++;
			}
		}
	}

	/* If nothing to clean, return */
	if (!ncleans)
		goto err_out2;

	/*
	 * From this point onwards, make sure to reset
	 * the clean inflag on cache blocks before returning
	 */

	/* 4. read cache set data */

	init_rwsem(&sioc.sio_lock);
	sioc.sio_error = 0;

	for (i = start_index; i < end_index; i++) {
		if (EIO_CACHE_STATE_GET(dmc, i) == CLEAN_INPROG) {

			for (j = i; ((j < end_index) &&
				(EIO_CACHE_STATE_GET(dmc, j) == CLEAN_INPROG));
				j++);

			blkindex = (i - start_index);
			total = (j - i);

			/*
			 * Get the correct index and number of bvecs
			 * setup from dmc->clean_dbvecs before issuing i/o.
			 */
			bvecs =
				setup_bio_vecs(dmc->clean_dbvecs, blkindex,
					       dmc->block_size, total, &nr_bvecs);
			EIO_ASSERT(bvecs != NULL);
			EIO_ASSERT(nr_bvecs > 0);

			where.bdev = dmc->cache_dev->bdev;
			where.sector =
				(i << dmc->block_shift) + dmc->md_sectors;
			where.count = total * dmc->block_size;

			SECTOR_STATS(dmc->eio_stats.ssd_reads,
				     to_bytes(where.count));
			down_read(&sioc.sio_lock);
			error =
				eio_io_async_bvec(dmc, &where, READ, bvecs,
						  nr_bvecs, eio_sync_io_callback,
						  &sioc, 0);
			if (error) {
				sioc.sio_error = error;
				up_read(&sioc.sio_lock);
			}

			bvecs = NULL;
			i = j;
		}
	}
	/*
	 * In above for loop, submit all READ I/Os to SSD
	 * and unplug the device for immediate submission to
	 * underlying device driver.
	 */
	eio_unplug_cache_device(dmc);

	/* wait for all I/Os to complete and release sync lock */
	down_write(&sioc.sio_lock);
	up_write(&sioc.sio_lock);

	error = sioc.sio_error;
	if (error)
		goto err_out3;

	/* 5. write to hdd */
	/*
	 * While writing the data to HDD, explicitly enable
	 * BIO_RW_SYNC flag to hint higher priority for these
	 * I/Os.
	 */
	for (i = start_index; i < end_index; i++) {
		if (EIO_CACHE_STATE_GET(dmc, i) == CLEAN_INPROG) {

			blkindex = (i - start_index);
			total = 1;

			bvecs =
				setup_bio_vecs(dmc->clean_dbvecs, blkindex,
					       dmc->block_size, total, &nr_bvecs);
			EIO_ASSERT(bvecs != NULL);
			EIO_ASSERT(nr_bvecs > 0);

			where.bdev = dmc->disk_dev->bdev;
			where.sector = EIO_DBN_GET(dmc, i);
			where.count = dmc->block_size;

			SECTOR_STATS(dmc->eio_stats.disk_writes,
				     to_bytes(where.count));
			down_read(&sioc.sio_lock);
			error = eio_io_async_bvec(dmc, &where, WRITE | REQ_SYNC,
						  bvecs, nr_bvecs,
						  eio_sync_io_callback, &sioc,
						  1);

			if (error) {
				sioc.sio_error = error;
				up_read(&sioc.sio_lock);
			}
			bvecs = NULL;
		}
	}

	/* wait for all I/Os to complete and release sync lock */
	down_write(&sioc.sio_lock);
	up_write(&sioc.sio_lock);

	error = sioc.sio_error;
	if (error)
		goto err_out3;

	/* 6. update on-disk cache metadata */

	/* TBD. Do we have to consider sector alignment here ? */

	/*
	 * md_size = dmc->assoc * sizeof(struct flash_cacheblock);
	 * Currently, md_size is 8192 bytes, mdpage_count is 2 pages maximum.
	 */

	EIO_ASSERT(dmc->mdpage_count <= 2);
	for (k = 0; k < dmc->mdpage_count; k++)
		pg_virt_addr[k] = kmap(dmc->clean_mdpages[k]);

	alloc_size = dmc->assoc * sizeof(struct flash_cacheblock);
	pindex = 0;
	md_blocks = (struct flash_cacheblock *)pg_virt_addr[pindex];
	k = MD_BLOCKS_PER_PAGE;

	for (i = start_index; i < end_index; i++) {

		md_blocks->dbn = cpu_to_le64(EIO_DBN_GET(dmc, i));

		if (EIO_CACHE_STATE_GET(dmc, i) == CLEAN_INPROG)
			md_blocks->cache_state = cpu_to_le64(INVALID);
		else if (EIO_CACHE_STATE_GET(dmc, i) == ALREADY_DIRTY)
			md_blocks->cache_state = cpu_to_le64((VALID | DIRTY));
		else
			md_blocks->cache_state = cpu_to_le64(INVALID);

		/* This was missing earlier. */
		md_blocks++;
		k--;

		if (k == 0) {
			md_blocks =
				(struct flash_cacheblock *)pg_virt_addr[++pindex];
			k = MD_BLOCKS_PER_PAGE;
		}
	}

	for (k = 0; k < dmc->mdpage_count; k++)
		kunmap(dmc->clean_mdpages[k]);

	where.bdev = dmc->cache_dev->bdev;
	where.sector = dmc->md_start_sect + INDEX_TO_MD_SECTOR(start_index);
	where.count = eio_to_sector(alloc_size);
	error =
		eio_io_sync_pages(dmc, &where, WRITE, dmc->clean_mdpages,
				  dmc->mdpage_count);

	if (error)
		goto err_out3;

err_out3:

	/*
	 * 7. update in-core cache metadata for clean_inprog blocks.
	 * If there was an error, set them back to ALREADY_DIRTY
	 * If no error, set them to VALID
	 */
	for (i = start_index; i < end_index; i++) {
		if (EIO_CACHE_STATE_GET(dmc, i) == CLEAN_INPROG) {
			if (error)
				EIO_CACHE_STATE_SET(dmc, i, ALREADY_DIRTY);
			else {
				EIO_CACHE_STATE_SET(dmc, i, VALID);
				EIO_ASSERT(dmc->cache_sets[set].nr_dirty > 0);
				dmc->cache_sets[set].nr_dirty--;
				atomic64_dec(&dmc->nr_dirty);
			}
		}
	}

err_out2:

	up_write(&dmc->cache_sets[set].rw_lock);

err_out1:

	/* Reset clean flags on the set */

	if (!force) {
		spin_lock_irqsave(&dmc->cache_sets[set].cs_lock, flags);
		dmc->cache_sets[set].flags &=
			~(SETFLAG_CLEAN_INPROG | SETFLAG_CLEAN_WHOLE);
		spin_unlock_irqrestore(&dmc->cache_sets[set].cs_lock, flags);
	}

	if (dmc->cache_sets[set].nr_dirty)
		/*
		 * Lru touch the set, so that it can be picked
		 * up for whole set clean by clean thread later
		 */
		eio_touch_set_lru(dmc, set);

	return;
}

/*
 * Enqueues the dirty sets for clean, which had got dirtied long
 * time back(aged). User tunable values to determine if a set has aged
 */
void eio_clean_aged_sets(struct work_struct *work)
{
	struct cache_c *dmc;
	unsigned long flags = 0;
	index_t set_index;
	u_int64_t set_time;
	u_int64_t cur_time;

	dmc = container_of(work, struct cache_c, clean_aged_sets_work.work);

	/*
	 * In FAILED state, dont schedule cleaning of sets.
	 */
	if (unlikely(CACHE_FAILED_IS_SET(dmc))) {
		pr_debug("clean_aged_sets: Cache \"%s\" is in failed mode.\n",
			 dmc->cache_name);
		/*
		 * This is to make sure that this thread is rescheduled
		 * once CACHE is ACTIVE again.
		 */
		spin_lock_irqsave(&dmc->dirty_set_lru_lock, flags);
		dmc->is_clean_aged_sets_sched = 0;
		spin_unlock_irqrestore(&dmc->dirty_set_lru_lock, flags);

		return;
	}

	cur_time = jiffies;

	/* Use the set LRU list to pick up the most aged sets. */
	spin_lock_irqsave(&dmc->dirty_set_lru_lock, flags);
	do {
		lru_read_head(dmc->dirty_set_lru, &set_index, &set_time);
		if (set_index == LRU_NULL)
			break;

		if ((EIO_DIV((cur_time - set_time), HZ)) <
		    (dmc->sysctl_active.time_based_clean_interval * 60))
			break;
		lru_rem(dmc->dirty_set_lru, set_index);

		if (dmc->cache_sets[set_index].nr_dirty > 0) {
			spin_unlock_irqrestore(&dmc->dirty_set_lru_lock, flags);
			eio_addto_cleanq(dmc, set_index, 1);
			spin_lock_irqsave(&dmc->dirty_set_lru_lock, flags);
		}
	} while (1);
	spin_unlock_irqrestore(&dmc->dirty_set_lru_lock, flags);

	/* Re-schedule the aged set clean, unless the clean has to stop now */

	if (dmc->sysctl_active.time_based_clean_interval == 0)
		goto out;

	schedule_delayed_work(&dmc->clean_aged_sets_work,
			      dmc->sysctl_active.time_based_clean_interval *
			      60 * HZ);
out:
	return;
}

/* Move the given set at the head of the set LRU list */
void eio_touch_set_lru(struct cache_c *dmc, index_t set)
{
	u_int64_t systime;
	unsigned long flags;

	systime = jiffies;
	spin_lock_irqsave(&dmc->dirty_set_lru_lock, flags);
	lru_touch(dmc->dirty_set_lru, set, systime);

	if ((dmc->sysctl_active.time_based_clean_interval > 0) &&
	    (dmc->is_clean_aged_sets_sched == 0)) {
		schedule_delayed_work(&dmc->clean_aged_sets_work,
				      dmc->sysctl_active.
				      time_based_clean_interval * 60 * HZ);
		dmc->is_clean_aged_sets_sched = 1;
	}

	spin_unlock_irqrestore(&dmc->dirty_set_lru_lock, flags);
}
