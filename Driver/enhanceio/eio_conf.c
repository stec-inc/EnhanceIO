/*
 *  eio_conf.c
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
 *  Amit Kale <akale@stec-inc.com>
 *  Harish Pujari <hpujari@stec-inc.com>
 *   Designed and implemented the writeback caching mode
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
#include "eio_ttc.h"

#define KMEM_CACHE_JOB          "eio-kcached-jobs"
#define KMEM_EIO_IO             "eio-io-context"
#define KMEM_DMC_BIO_PAIR       "eio-dmc-bio-pair"
/* #define KMEM_CACHE_PENDING_JOB	"eio-pending-jobs" */

static struct cache_c *cache_list_head = NULL;
struct work_struct _kcached_wq;

static struct kmem_cache *_job_cache;
struct kmem_cache *_io_cache;   /* cache of eio_context objects */
mempool_t *_job_pool;
mempool_t *_io_pool;            /* pool of eio_context object */

atomic_t nr_cache_jobs;


LIST_HEAD(ssd_rm_list);
int ssd_rm_list_not_empty;
spinlock_t ssd_rm_list_lock;

struct eio_control_s *eio_control;

int eio_force_warm_boot;
static int eio_notify_reboot(struct notifier_block *nb, unsigned long action,
			     void *x);
void eio_stop_async_tasks(struct cache_c *dmc);
static int eio_notify_ssd_rm(struct notifier_block *nb, unsigned long action,
			     void *x);

/*
 * The notifiers are registered in descending order of priority and
 * executed in descending order or priority. We should be run before
 * any notifiers of ssd's or other block devices. Typically, devices
 * use a priority of 0.
 * XXX - If in the future we happen to use a md device as the cache
 * block device, we have a problem because md uses a priority of
 * INT_MAX as well. But we want to run before the md's reboot notifier !
 */
static struct notifier_block eio_reboot_notifier = {
	.notifier_call	= eio_notify_reboot,
	.next		= NULL,
	.priority	= INT_MAX,         /* should be > ssd pri's and disk dev pri's */
};

static struct notifier_block eio_ssd_rm_notifier = {
	.notifier_call	= eio_notify_ssd_rm,
	.next		= NULL,
	.priority	= 0,
};

int eio_wait_schedule(void *unused)
{

	schedule();
	return 0;
}

/*
 * Check if the System RAM threshold > requested memory, don't care
 * if threshold is set to 0. Return value is 0 for fail and 1 for success.
 */
static inline int eio_mem_available(struct cache_c *dmc, size_t size)
{
	struct sysinfo si;

	if (unlikely
		    (dmc->sysctl_active.mem_limit_pct <= 0
		    || dmc->sysctl_active.mem_limit_pct >= 100))
		return 1;

	si_meminfo(&si);
	return (((si.freeram << PAGE_SHIFT) *
		 dmc->sysctl_active.mem_limit_pct) / 100) > size;
}

/* create a new thread and call the specified function */
void *eio_create_thread(int (*func)(void *), void *context, char *name)
{
	return kthread_run(func, context, name);
}

/* wait for the given thread to exit */
void eio_wait_thread_exit(void *thrdptr, int *running)
{
	while (*running)
		msleep(1);

	//do_exit() would be called within the thread func itself

	return;
}

/* thread exit self */
void eio_thread_exit(long exit_code)
{
	do_exit(exit_code);
}

inline int eio_policy_init(struct cache_c *dmc)
{
	int error = 0;

	if (dmc->req_policy == 0)
		dmc->req_policy = CACHE_REPL_DEFAULT;

	if (dmc->req_policy == CACHE_REPL_RANDOM) {
		dmc->policy_ops = NULL;
		pr_info("Setting replacement policy to random");
	} else {
		dmc->policy_ops = eio_get_policy(dmc->req_policy);
		if (dmc->policy_ops == NULL) {
			dmc->req_policy = CACHE_REPL_RANDOM;
			pr_err
				("policy_init: Cannot find requested policy, defaulting to random");
			error = -ENOMEM;
		} else {
			/* Back pointer to reference dmc from policy_ops */
			dmc->policy_ops->sp_dmc = dmc;
			pr_info("Setting replacement policy to %s (%d)",
				(dmc->policy_ops->sp_name ==
				 CACHE_REPL_FIFO) ? "fifo" : "lru",
				dmc->policy_ops->sp_name);
		}
	}
	return error;
}

static int eio_jobs_init(void)
{

	_job_cache = _io_cache = NULL;
	_job_pool = _io_pool = NULL;

	_job_cache = kmem_cache_create(KMEM_CACHE_JOB,
				       sizeof(struct kcached_job),
				       __alignof__(struct kcached_job),
				       0, NULL);
	if (!_job_cache)
		return -ENOMEM;

	_job_pool = mempool_create(MIN_JOBS, mempool_alloc_slab,
				   mempool_free_slab, _job_cache);
	if (!_job_pool)
		goto out;

	_io_cache = kmem_cache_create(KMEM_EIO_IO,
				      sizeof(struct eio_context),
				      __alignof__(struct eio_context), 0, NULL);
	if (!_io_cache)
		goto out;

	_io_pool = mempool_create(MIN_EIO_IO, mempool_alloc_slab,
				  mempool_free_slab, _io_cache);
	if (!_io_pool)
		goto out;

	return 0;

out:
	if (_io_pool)
		mempool_destroy(_io_pool);
	if (_io_cache)
		kmem_cache_destroy(_io_cache);
	if (_job_pool)
		mempool_destroy(_job_pool);
	if (_job_cache)
		kmem_cache_destroy(_job_cache);

	_job_pool = _io_pool = NULL;
	_job_cache = _io_cache = NULL;
	return -ENOMEM;
}

static void eio_jobs_exit(void)
{

	mempool_destroy(_io_pool);
	mempool_destroy(_job_pool);
	kmem_cache_destroy(_io_cache);
	kmem_cache_destroy(_job_cache);

	_job_pool = _io_pool = NULL;
	_job_cache = _io_cache = NULL;
}

static int eio_kcached_init(struct cache_c *dmc)
{

	/* init_waitqueue_head(&dmc->destroyq); */
	atomic_set(&dmc->nr_jobs, 0);
	return 0;
}

static void eio_kcached_client_destroy(struct cache_c *dmc)
{

	/* Wait for all IOs */
	//wait_event(dmc->destroyq, !atomic_read(&dmc->nr_jobs));
}

/* Store the cache superblock on ssd */
int eio_sb_store(struct cache_c *dmc)
{
	union eio_superblock *sb = NULL;
	struct eio_io_region where;
	int error;

	struct bio_vec *sb_pages;
	int nr_pages;
	int page_count, page_index;

	if ((unlikely(CACHE_FAILED_IS_SET(dmc)) || CACHE_DEGRADED_IS_SET(dmc))
	    && (!CACHE_SSD_ADD_INPROG_IS_SET(dmc))) {
		pr_err
			("sb_store: Cannot write superblock for cache \"%s\", in degraded/failed mode.\n",
			dmc->cache_name);
		return -ENODEV;
	}

	page_count = 0;
	nr_pages = EIO_SUPERBLOCK_SIZE / PAGE_SIZE;
	EIO_ASSERT(nr_pages != 0);

	sb_pages = eio_alloc_pages(nr_pages, &page_count);
	if (sb_pages == NULL) {
		pr_err("sb_store: System memory too low.\n");
		return -ENOMEM;
	}

	EIO_ASSERT(page_count == nr_pages);

	nr_pages = page_count;
	page_index = 0;
	sb = (union eio_superblock *)kmap(sb_pages[page_index].bv_page);

	sb->sbf.cache_sb_state = dmc->sb_state;
	sb->sbf.block_size = dmc->block_size;
	sb->sbf.size = dmc->size;
	sb->sbf.assoc = dmc->assoc;
	sb->sbf.cache_md_start_sect = dmc->md_start_sect;
	sb->sbf.cache_data_start_sect = dmc->md_sectors;
	strncpy(sb->sbf.disk_devname, dmc->disk_devname, DEV_PATHLEN);
	strncpy(sb->sbf.cache_devname, dmc->cache_devname, DEV_PATHLEN);
	strncpy(sb->sbf.ssd_uuid, dmc->ssd_uuid, DEV_PATHLEN - 1);
	sb->sbf.cache_devsize = to_sector(eio_get_device_size(dmc->cache_dev));
	sb->sbf.disk_devsize = to_sector(eio_get_device_size(dmc->disk_dev));
	sb->sbf.cache_version = dmc->sb_version;
	strncpy(sb->sbf.cache_name, dmc->cache_name, DEV_PATHLEN);
	sb->sbf.cache_name[DEV_PATHLEN - 1] = '\0';
	sb->sbf.mode = dmc->mode;
	spin_lock_irqsave(&dmc->cache_spin_lock, dmc->cache_spin_lock_flags);
	sb->sbf.repl_policy = dmc->req_policy;
	sb->sbf.cache_flags = dmc->cache_flags & ~CACHE_FLAGS_INCORE_ONLY;
	spin_unlock_irqrestore(&dmc->cache_spin_lock,
			       dmc->cache_spin_lock_flags);
	if (dmc->sb_version)
		sb->sbf.magic = EIO_MAGIC;
	else
		sb->sbf.magic = EIO_BAD_MAGIC;

	sb->sbf.cold_boot = dmc->cold_boot;
	if (sb->sbf.cold_boot && eio_force_warm_boot)
		sb->sbf.cold_boot |= BOOT_FLAG_FORCE_WARM;

	sb->sbf.dirty_high_threshold = dmc->sysctl_active.dirty_high_threshold;
	sb->sbf.dirty_low_threshold = dmc->sysctl_active.dirty_low_threshold;
	sb->sbf.dirty_set_high_threshold =
		dmc->sysctl_active.dirty_set_high_threshold;
	sb->sbf.dirty_set_low_threshold =
		dmc->sysctl_active.dirty_set_low_threshold;
	sb->sbf.time_based_clean_interval =
		dmc->sysctl_active.time_based_clean_interval;
	sb->sbf.autoclean_threshold = dmc->sysctl_active.autoclean_threshold;

	/* write out to ssd */
	where.bdev = dmc->cache_dev->bdev;
	where.sector = EIO_SUPERBLOCK_START;
	where.count = to_sector(EIO_SUPERBLOCK_SIZE);
	error = eio_io_sync_vm(dmc, &where, WRITE, sb_pages, nr_pages);
	if (error) {
		pr_err
			("sb_store: Could not write out superblock to sector %lu (error %d) for cache \"%s\".\n",
			where.sector, error, dmc->cache_name);
	}

	/* free the allocated pages here */
	if (sb_pages) {
		kunmap(sb_pages[0].bv_page);
		for (page_index = 0; page_index < nr_pages; page_index++)
			put_page(sb_pages[page_index].bv_page);
		kfree(sb_pages);
		sb_pages = NULL;
	}

	return error;
}

/*
 * Write out the metadata one sector at a time.
 * Then dump out the superblock.
 */
int eio_md_store(struct cache_c *dmc)
{
	struct flash_cacheblock *next_ptr;
	struct eio_io_region where;
	sector_t i;
	int j, k;
	int num_valid = 0, num_dirty = 0;
	int error;
	int write_errors = 0;
	sector_t sectors_written = 0, sectors_expected = 0;     /* debug */
	int slots_written = 0;                                  /* How many cache slots did we fill in this MD io block ? */

	struct bio_vec *pages;
	int nr_pages;
	int page_count, page_index;
	void **pg_virt_addr;

	if (unlikely(CACHE_FAILED_IS_SET(dmc))
	    || unlikely(CACHE_DEGRADED_IS_SET(dmc))) {
		pr_err
			("md_store: Cannot write metadata in failed/degraded mode for cache \"%s\".",
			dmc->cache_name);
		return -ENODEV;
	}

	if (CACHE_FAST_REMOVE_IS_SET(dmc)) {
		if (CACHE_VERBOSE_IS_SET(dmc))
			pr_info("Skipping writing out metadata to cache");
		if (!dmc->sb_version) {

			/*
			 * Incase of delete, flush the superblock
			 * irrespective of fast_remove being set.
			 */

			goto sb_store;
		}
		return 0;
	}

	if (!eio_mem_available(dmc, METADATA_IO_BLOCKSIZE_SECT)) {
		pr_err
			("md_store: System memory too low for allocating metadata IO buffers");
		return -ENOMEM;
	}

	page_count = 0;
	pages = eio_alloc_pages(dmc->bio_nr_pages, &page_count);
	if (pages == NULL) {
		pr_err("eio_md_store: System memory too low.");
		return -ENOMEM;
	}

	/* get the exact number of pages allocated */
	nr_pages = page_count;
	where.bdev = dmc->cache_dev->bdev;
	where.sector = dmc->md_start_sect;
	slots_written = 0;
	page_index = 0;

	pg_virt_addr = kmalloc(nr_pages * (sizeof(void *)), GFP_KERNEL);
	if (pg_virt_addr == NULL) {
		pr_err("eio_md_store: System memory too low.");
		for (k = 0; k < nr_pages; k++)
			put_page(pages[k].bv_page);
		kfree(pages);
		return -ENOMEM;
	}

	for (k = 0; k < nr_pages; k++)
		pg_virt_addr[k] = kmap(pages[k].bv_page);

	next_ptr = (struct flash_cacheblock *)pg_virt_addr[page_index];
	j = MD_BLOCKS_PER_PAGE;

	pr_info("Writing out metadata to cache device. Please wait...");

	for (i = 0; i < dmc->size; i++) {
		if (EIO_CACHE_STATE_GET(dmc, (index_t)i) & VALID)
			num_valid++;
		if (EIO_CACHE_STATE_GET(dmc, (index_t)i) & DIRTY)
			num_dirty++;
		next_ptr->dbn = EIO_DBN_GET(dmc, i);
		next_ptr->cache_state = EIO_CACHE_STATE_GET(dmc, (index_t)i) &
					(INVALID | VALID | DIRTY);

		next_ptr++;
		slots_written++;
		j--;
		if (j == 0) {
			/*
			 * Filled the page, goto the next page.
			 */
			page_index++;

			if (slots_written ==
			    (int)(MD_BLOCKS_PER_PAGE * nr_pages)) {
				/*
				 * Wrote out an entire metadata IO block, write the block to the ssd.
				 */
				where.count =
					slots_written / MD_BLOCKS_PER_SECTOR;
				slots_written = 0;
				page_index = 0;
				sectors_written += where.count; /* debug */

				error =
					eio_io_sync_vm(dmc, &where, WRITE, pages,
						       nr_pages);

				if (error) {
					write_errors++;
					pr_err
						("md_store: Could not write out metadata to sector %lu (error %d)",
						where.sector, error);
				}
				where.sector += where.count;    /* Advance offset */
			}
			/* Move next slot pointer into next sector */
			next_ptr =
				(struct flash_cacheblock *)pg_virt_addr[page_index];
			j = MD_BLOCKS_PER_PAGE;
		}
	}

	if (next_ptr != (struct flash_cacheblock *)pg_virt_addr[0]) {
		/* Write the remaining last page out */
		EIO_ASSERT(slots_written > 0);

		where.count = slots_written / MD_BLOCKS_PER_SECTOR;

		if (slots_written % MD_BLOCKS_PER_SECTOR)
			where.count++;

		sectors_written += where.count;

		/*
		 * This may happen that we are at the beginning of the next page
		 * and did not fill up any slots in this page. Verify this condition
		 * and set page_index accordingly.
		 */

		if (next_ptr !=
		    (struct flash_cacheblock *)pg_virt_addr[page_index]) {
			unsigned offset;

			slots_written = slots_written % MD_BLOCKS_PER_PAGE;

			/*
			 * We have some extra slots written at this page_index.
			 * Let us try to zero out the remaining page size before submitting
			 * this page.
			 */
			offset =
				slots_written * (sizeof(struct flash_cacheblock));
			memset(pg_virt_addr[page_index] + offset, 0,
			       PAGE_SIZE - offset);

			page_index++;
		}

		error = eio_io_sync_vm(dmc, &where, WRITE, pages, page_index);
		/* XXX: should we call eio_sb_store() on error ?? */
		if (error) {
			write_errors++;
			pr_err
				("md_store: Could not write out metadata to sector %lu (error %d)",
				where.sector, error);
		}
	}

	/* Debug Tests */
	sectors_expected = dmc->size / MD_BLOCKS_PER_SECTOR;
	if (dmc->size % MD_BLOCKS_PER_SECTOR)
		sectors_expected++;
	EIO_ASSERT(sectors_expected == sectors_written);
	/* XXX: should we call eio_sb_store() on error ?? */
	if (sectors_expected != sectors_written) {
		pr_err
			("md_store: Sector mismatch! sectors_expected=%ld, sectors_written=%ld\n",
			sectors_expected, sectors_written);
	}

	for (k = 0; k < nr_pages; k++)
		kunmap(pages[k].bv_page);
	kfree(pg_virt_addr);

	if (pages)
		for (k = 0; k < nr_pages; k++)
			put_page(pages[k].bv_page);
	kfree(pages);
	pages = NULL;

	if (write_errors == 0) {
		if (num_dirty == 0)
			dmc->sb_state = CACHE_MD_STATE_CLEAN;
		else
			dmc->sb_state = CACHE_MD_STATE_FASTCLEAN;
	} else
		dmc->sb_state = CACHE_MD_STATE_UNSTABLE;

sb_store:
	error = eio_sb_store(dmc);
	if (error) {
		/* Harish: TBD. should we return error */
		write_errors++;
		pr_err("md_store: superblock store failed(error %d)", error);
	}
	if (!dmc->sb_version && CACHE_FAST_REMOVE_IS_SET(dmc))
		return 0;

	if (write_errors == 0)
		pr_info("Metadata saved on the cache device");
	else {
		pr_info
			("CRITICAL: There were %d errors in saving metadata on cache device",
			write_errors);
		if (num_dirty)
			pr_info
				("CRITICAL: %d dirty blocks could not be written out",
				num_dirty);
	}

	pr_info("Valid blocks: %d, Dirty blocks: %d, Metadata sectors: %lu",
		num_valid, num_dirty, (long unsigned int)dmc->md_sectors);

	return 0;
}

static int eio_md_create(struct cache_c *dmc, int force, int cold)
{
	struct flash_cacheblock *next_ptr;
	union eio_superblock *header;
	struct eio_io_region where;
	sector_t i;
	int j, error;
	sector_t cache_size, dev_size;
	sector_t order;
	sector_t sectors_written = 0, sectors_expected = 0;     /* debug */
	int slots_written = 0;                                  /* How many cache slots did we fill in this MD io block ? */

	struct bio_vec *header_page = NULL;                     /* Header page */
	struct bio_vec *pages = NULL;                           /* Metadata pages */
	int nr_pages = 0;
	int page_count, page_index;
	int ret = 0, k;
	void **pg_virt_addr = NULL;

	// Allocate single page for superblock header.
	page_count = 0;
	header_page = eio_alloc_pages(1, &page_count);
	if (header_page == NULL) {
		pr_err("eio_md_create: System memory too low.");
		return -ENOMEM;
	}

	EIO_ASSERT(page_count = 1);
	header = (union eio_superblock *)kmap(header_page[0].bv_page);

	/*
	 * Apart from normal cache creation, eio_md_create() is also called when
	 * the SSD is added as part of eio_resume_caching(). At this point,
	 * the CACHE_FLAGS_DEGRADED is set, but we do want to write to the md area.
	 * Therefore, if the CACHE_FLAGS_SSD_ADD_INPROG is set, then proceed instead
	 * of returning -ENODEV.
	 */
	if ((unlikely(CACHE_FAILED_IS_SET(dmc))
	     || unlikely(CACHE_DEGRADED_IS_SET(dmc)))
	    && (!CACHE_SSD_ADD_INPROG_IS_SET(dmc))) {
		pr_err
			("md_create: Cannot write metadata in failed/degraded mode for cache \"%s\".\n",
			dmc->cache_name);
		ret = -ENODEV;
		goto free_header;
	}

	where.bdev = dmc->cache_dev->bdev;
	where.sector = EIO_SUPERBLOCK_START;
	where.count = to_sector(EIO_SUPERBLOCK_SIZE);
	error = eio_io_sync_vm(dmc, &where, READ, header_page, 1);
	if (error) {
		pr_err
			("md_create: Could not read superblock sector %lu error %d for cache \"%s\".\n",
			where.sector, error, dmc->cache_name);
		ret = -EINVAL;
		goto free_header;
	}

	if (!force &&
	    ((header->sbf.cache_sb_state == CACHE_MD_STATE_DIRTY) ||
	     (header->sbf.cache_sb_state == CACHE_MD_STATE_CLEAN) ||
	     (header->sbf.cache_sb_state == CACHE_MD_STATE_FASTCLEAN))) {
		pr_err
			("md_create: Existing cache detected, use force to re-create.\n");
		ret = -EINVAL;
		goto free_header;
	}

	/*
	 * Compute the size of the metadata including header.
	 * and here we also are making sure that metadata and userdata
	 * on SSD is aligned at 8K boundary.
	 *
	 * Note dmc->size is in raw sectors
	 */
	dmc->md_start_sect = EIO_METADATA_START(dmc->cache_dev_start_sect);
	dmc->md_sectors =
		INDEX_TO_MD_SECTOR(dmc->size / (sector_t)dmc->block_size);
	dmc->md_sectors +=
		EIO_EXTRA_SECTORS(dmc->cache_dev_start_sect, dmc->md_sectors);
	dmc->size -= dmc->md_sectors;   /* total sectors available for cache */
	dmc->size /= dmc->block_size;
	dmc->size = (dmc->size / (sector_t)dmc->assoc) * (sector_t)dmc->assoc;
	/* Recompute since dmc->size was possibly trunc'ed down */
	dmc->md_sectors = INDEX_TO_MD_SECTOR(dmc->size);
	dmc->md_sectors +=
		EIO_EXTRA_SECTORS(dmc->cache_dev_start_sect, dmc->md_sectors);

	if ((error = eio_mem_init(dmc)) == -1) {
		ret = -EINVAL;
		goto free_header;
	}
	if ((unlikely(CACHE_FAILED_IS_SET(dmc))
	     || unlikely(CACHE_DEGRADED_IS_SET(dmc)))
	    && (!CACHE_SSD_ADD_INPROG_IS_SET(dmc))) {
		pr_err
			("md_create: Cannot write metadata in failed/degraded mode for cache \"%s\".\n",
			dmc->cache_name);
		ret = -ENODEV;
		goto free_header;
	}
	dev_size = to_sector(eio_get_device_size(dmc->cache_dev));
	cache_size = dmc->md_sectors + (dmc->size * dmc->block_size);
	if (cache_size > dev_size) {
		pr_err
			("md_create: Requested cache size exceeds the cache device's capacity (%lu > %lu)",
			cache_size, dev_size);
		ret = -EINVAL;
		goto free_header;
	}

	order =
		dmc->size *
		(EIO_MD8(dmc) ? sizeof(struct cacheblock_md8) :
		 sizeof(struct cacheblock));
	i = EIO_MD8(dmc) ? sizeof(struct cacheblock_md8) : sizeof(struct
								  cacheblock);
	pr_info("Allocate %luKB (%luB per) mem for %lu-entry cache "
		"(capacity:%luMB, associativity:%u, block size:%u bytes)",
		order >> 10, i, (long unsigned int)dmc->size,
		(cache_size >> (20 - SECTOR_SHIFT)), dmc->assoc,
		dmc->block_size << SECTOR_SHIFT);

	if (!eio_mem_available(dmc, order) && !CACHE_SSD_ADD_INPROG_IS_SET(dmc)) {
		pr_err
			("md_create: System memory too low for allocating cache metadata.\n");
		ret = -ENOMEM;
		goto free_header;
	}

	/*
	 * If we are called due to SSD add, the memory was already allocated
	 * as part of cache creation (i.e., eio_ctr()) in the past.
	 */
	if (!CACHE_SSD_ADD_INPROG_IS_SET(dmc)) {
		if (EIO_MD8(dmc))
			dmc->cache_md8 =
				(struct cacheblock_md8 *)vmalloc((size_t)order);
		else
			dmc->cache =
				(struct cacheblock *)vmalloc((size_t)order);
		if ((EIO_MD8(dmc) && !dmc->cache_md8)
		    || (!EIO_MD8(dmc) && !dmc->cache)) {
			pr_err
				("md_create: Unable to allocate cache md for cache \"%s\".\n",
				dmc->cache_name);
			ret = -ENOMEM;
			goto free_header;
		}
	}
	if (eio_repl_blk_init(dmc->policy_ops) != 0) {
		pr_err
			("md_create: Unable to allocate memory for policy cache block for cache \"%s\".\n",
			dmc->cache_name);
		ret = -ENOMEM;
		goto free_header;
	}

	if (cold) {
		int retry = 0;
		do {
			for (i = 0; i < dmc->size; i++) {
				if (CACHE_SSD_ADD_INPROG_IS_SET(dmc)) {
					u_int8_t cache_state =
						EIO_CACHE_STATE_GET(dmc, i);
					if (cache_state & BLOCK_IO_INPROG) {
						/* sleep for 1 sec and retry */
						msleep(1000);
						break;
					}
				}
				eio_invalidate_md(dmc, i);
			}
		} while ((retry++ < 10) && (i < dmc->size));

		if (i < dmc->size) {
			pr_err
				("md_create: Cache \"%s\" is not in quiesce state. Can't proceed to resume.\n",
				dmc->cache_name);
			ret = -EBUSY;
			goto free_header;
		}

		/* Allocate pages of the order dmc->bio_nr_pages */
		page_count = 0;
		pages = eio_alloc_pages(dmc->bio_nr_pages, &page_count);
		if (!pages) {
			pr_err
				("md_create: Unable to allocate pages for cache \"%s\".\n",
				dmc->cache_name);
			pr_err
				("md_create: Could not write out cache metadata.\n");
			ret = -ENOMEM;
			goto free_header;
		}

		/* nr_pages is used for freeing the pages */
		nr_pages = page_count;

		where.bdev = dmc->cache_dev->bdev;
		where.sector = dmc->md_start_sect;
		slots_written = 0;
		page_index = 0;

		pg_virt_addr = kmalloc(nr_pages * (sizeof(void *)), GFP_KERNEL);
		if (pg_virt_addr == NULL) {
			pr_err("md_create: System memory too low.\n");
			for (k = 0; k < nr_pages; k++)
				put_page(pages[k].bv_page);
			kfree(pages);
			ret = -ENOMEM;
			goto free_header;
		}

		for (k = 0; k < nr_pages; k++)
			pg_virt_addr[k] = kmap(pages[k].bv_page);

		next_ptr = (struct flash_cacheblock *)pg_virt_addr[page_index];
		j = MD_BLOCKS_PER_PAGE;

		for (i = 0; i < dmc->size; i++) {
			next_ptr->dbn = EIO_DBN_GET(dmc, i);
			next_ptr->cache_state =
				EIO_CACHE_STATE_GET(dmc,
						    (index_t)i) & (INVALID | VALID
								   | DIRTY);
			next_ptr++;
			slots_written++;
			j--;

			if (j == 0) {

				page_index++;

				if ((unsigned)slots_written ==
				    MD_BLOCKS_PER_PAGE * nr_pages) {

					where.count =
						slots_written /
						MD_BLOCKS_PER_SECTOR;
					slots_written = 0;
					page_index = 0;
					sectors_written += where.count; /* debug */
					error =
						eio_io_sync_vm(dmc, &where, WRITE,
							       pages, nr_pages);

					if (error) {
						if (!CACHE_SSD_ADD_INPROG_IS_SET
							    (dmc))
							vfree(EIO_CACHE(dmc));
						pr_err
							("md_create: Could not write cache metadata sector %lu error %d.\n for cache \"%s\".\n",
							where.sector, error,
							dmc->cache_name);
						ret = -EIO;
						goto free_md;
					}
					where.sector += where.count;    /* Advance offset */
				}

				/* Move next slot pointer into next page */
				next_ptr =
					(struct flash_cacheblock *)
					pg_virt_addr[page_index];
				j = MD_BLOCKS_PER_PAGE;
			}
		}

		if (next_ptr != (struct flash_cacheblock *)pg_virt_addr[0]) {
			/* Write the remaining last page out */
			EIO_ASSERT(slots_written > 0);

			where.count = slots_written / MD_BLOCKS_PER_SECTOR;

			if (slots_written % MD_BLOCKS_PER_SECTOR)
				where.count++;

			sectors_written += where.count;

			if (next_ptr !=
			    (struct flash_cacheblock *)pg_virt_addr[page_index]) {
				unsigned offset;

				slots_written =
					slots_written % MD_BLOCKS_PER_PAGE;

				/*
				 * We have some extra slots written at this page_index.
				 * Let us try to zero out the remaining page size before submitting
				 * this page.
				 */
				offset =
					slots_written *
					(sizeof(struct flash_cacheblock));
				memset(pg_virt_addr[page_index] + offset, 0,
				       PAGE_SIZE - offset);

				page_index = page_index + 1;
			}

			error =
				eio_io_sync_vm(dmc, &where, WRITE, pages,
					       page_index);
			if (error) {
				if (!CACHE_SSD_ADD_INPROG_IS_SET(dmc))
					vfree((void *)EIO_CACHE(dmc));
				pr_err
					("md_create: Could not write cache metadata sector %lu error %d for cache \"%s\".\n",
					where.sector, error, dmc->cache_name);
				ret = -EIO;
				goto free_md;
			}
		}

		/* Debug Tests */
		sectors_expected = dmc->size / MD_BLOCKS_PER_SECTOR;
		if (dmc->size % MD_BLOCKS_PER_SECTOR)
			sectors_expected++;
		if (sectors_expected != sectors_written) {
			pr_err
				("md_create: Sector mismatch! sectors_expected=%ld, sectors_written=%ld for cache \"%s\".\n",
				sectors_expected, sectors_written,
				dmc->cache_name);
			ret = -EIO;
			goto free_md;
		}
	}

	/* if cold ends here */
	/* Write the superblock */
	if ((unlikely(CACHE_FAILED_IS_SET(dmc))
	     || unlikely(CACHE_DEGRADED_IS_SET(dmc)))
	    && (!CACHE_SSD_ADD_INPROG_IS_SET(dmc))) {
		pr_err
			("md_create: Cannot write metadata in failed/degraded mode for cache \"%s\".\n",
			dmc->cache_name);
		vfree((void *)EIO_CACHE(dmc));
		ret = -ENODEV;
		goto free_md;
	}

	dmc->sb_state = CACHE_MD_STATE_DIRTY;
	dmc->sb_version = EIO_SB_VERSION;
	error = eio_sb_store(dmc);
	if (error) {
		if (!CACHE_SSD_ADD_INPROG_IS_SET(dmc))
			vfree((void *)EIO_CACHE(dmc));
		pr_err
			("md_create: Could not write cache superblock sector(error %d) for cache \"%s\"\n",
			error, dmc->cache_name);
		ret = -EIO;
		goto free_md;
	}

free_md:
	for (k = 0; k < nr_pages; k++)
		kunmap(pages[k].bv_page);
	kfree(pg_virt_addr);

	/* Free metadata pages here. */
	if (pages) {
		for (k = 0; k < nr_pages; k++)
			put_page(pages[k].bv_page);
		kfree(pages);
		pages = NULL;
	}

free_header:
	/* Free header page here */
	if (header_page) {
		kunmap(header_page[0].bv_page);
		put_page(header_page[0].bv_page);
		kfree(header_page);
		header_page = NULL;
	}

	return ret;
}

static int eio_md_load(struct cache_c *dmc)
{
	struct flash_cacheblock *meta_data_cacheblock, *next_ptr;
	union eio_superblock *header;
	struct eio_io_region where;
	int i;
	index_t j, slots_read;
	sector_t size;
	int clean_shutdown;
	int dirty_loaded = 0;
	sector_t order, data_size;
	int num_valid = 0;
	int error;
	sector_t sectors_read = 0, sectors_expected = 0;        /* Debug */
	int force_warm_boot = 0;

	struct bio_vec *header_page, *pages;
	int nr_pages, page_count, page_index;
	int ret = 0;
	void **pg_virt_addr;

	page_count = 0;
	header_page = eio_alloc_pages(1, &page_count);
	if (header_page == NULL) {
		pr_err("md_load: Unable to allocate memory");
		return -ENOMEM;
	}

	EIO_ASSERT(page_count == 1);
	header = (union eio_superblock *)kmap(header_page[0].bv_page);

	if (CACHE_FAILED_IS_SET(dmc) || CACHE_DEGRADED_IS_SET(dmc)) {
		pr_err
			("md_load: Cannot load metadata in failed / degraded mode");
		ret = -ENODEV;
		goto free_header;
	}

	where.bdev = dmc->cache_dev->bdev;
	where.sector = EIO_SUPERBLOCK_START;
	where.count = to_sector(EIO_SUPERBLOCK_SIZE);
	error = eio_io_sync_vm(dmc, &where, READ, header_page, 1);
	if (error) {
		pr_err
			("md_load: Could not read cache superblock sector %lu error %d",
			where.sector, error);
		ret = -EINVAL;
		goto free_header;
	}

	/* check ondisk superblock version */
	if (header->sbf.cache_version != EIO_SB_VERSION) {
		pr_info("md_load: Cache superblock mismatch detected."
			" (current: %u, ondisk: %u)", EIO_SB_VERSION,
			header->sbf.cache_version);

		if (header->sbf.cache_version == 0) {
			pr_err("md_load: Can't enable cache %s. Either "
			       "superblock version is invalid or cache has"
			       " been deleted", header->sbf.cache_name);
			ret = 1;
			goto free_header;
		}

		if (header->sbf.cache_version > EIO_SB_VERSION) {
			pr_err("md_load: Can't enable cache %s with newer "
			       " superblock version.", header->sbf.cache_name);
			ret = 1;
			goto free_header;
		}

		if (header->sbf.mode == CACHE_MODE_WB) {
			pr_err("md_load: Can't enable write-back cache %s"
			       " with newer superblock version.",
			       header->sbf.cache_name);
			ret = 1;
			goto free_header;
		} else if ((header->sbf.mode == CACHE_MODE_RO) ||
			   (header->sbf.mode == CACHE_MODE_WT)) {
			dmc->persistence = CACHE_FORCECREATE;
			pr_info("md_load: Can't enable cache, recreating"
				" cache %s with newer superblock version.",
				header->sbf.cache_name);
			ret = 0;
			goto free_header;
		}
	}

	/* check ondisk magic number */

	if (header->sbf.cache_version >= EIO_SB_MAGIC_VERSION &&
	    header->sbf.magic != EIO_MAGIC) {
		pr_err("md_load: Magic number mismatch in superblock detected."
		       " (current: %u, ondisk: %u)", EIO_MAGIC,
		       header->sbf.magic);
		ret = 1;
		goto free_header;
	}

	dmc->sb_version = EIO_SB_VERSION;

	/*
	 * Harish: TBD
	 * For writeback, only when the dirty blocks are non-zero
	 * and header state is unexpected, we should treat it as md corrupted.
	 * Otherwise, a bad write in last shutdown, can lead to data inaccessible
	 * in writeback case.
	 */
	if (!((header->sbf.cache_sb_state == CACHE_MD_STATE_DIRTY) ||
	      (header->sbf.cache_sb_state == CACHE_MD_STATE_CLEAN) ||
	      (header->sbf.cache_sb_state == CACHE_MD_STATE_FASTCLEAN))) {
		pr_err("md_load: Corrupt cache superblock");
		ret = -EINVAL;
		goto free_header;
	}

	if (header->sbf.cold_boot & BOOT_FLAG_FORCE_WARM) {
		force_warm_boot = 1;
		header->sbf.cold_boot &= ~BOOT_FLAG_FORCE_WARM;
	}

	/*
	 * Determine if we can start as cold or hot cache
	 * - if cold_boot is set(unless force_warm_boot), start as cold cache
	 * - else if it is unclean shutdown, start as cold cache
	 * cold cache will still treat the dirty blocks as hot
	 */
	if (dmc->cold_boot != header->sbf.cold_boot) {
		pr_info
			("superblock(%u) and config(%u) cold boot values do not match. Relying on config",
			header->sbf.cold_boot, dmc->cold_boot);
	}
	if (dmc->cold_boot && !force_warm_boot) {
		pr_info
			("Cold boot is set, starting as if unclean shutdown(only dirty blocks will be hot)");
		clean_shutdown = 0;
	} else {
		if (header->sbf.cache_sb_state == CACHE_MD_STATE_DIRTY) {
			pr_info("Unclean shutdown detected");
			pr_info("Only dirty blocks exist in cache");
			clean_shutdown = 0;
		} else if (header->sbf.cache_sb_state == CACHE_MD_STATE_CLEAN) {
			pr_info("Slow (clean) shutdown detected");
			pr_info("Only clean blocks exist in cache");
			clean_shutdown = 1;
		} else if (header->sbf.cache_sb_state ==
			   CACHE_MD_STATE_FASTCLEAN) {
			pr_info("Fast (clean) shutdown detected");
			pr_info("Both clean and dirty blocks exist in cache");
			clean_shutdown = 1;
		} else {
			/* Harish: Won't reach here, but TBD may change the previous if condition */
			pr_info
				("cache state is %d. Treating as unclean shutdown",
				header->sbf.cache_sb_state);
			pr_info("Only dirty blocks exist in cache");
			clean_shutdown = 0;
		}
	}

	if (!dmc->mode)
		dmc->mode = header->sbf.mode;
	if (!dmc->req_policy)
		dmc->req_policy = header->sbf.repl_policy;

	if (!dmc->cache_flags)
		dmc->cache_flags = header->sbf.cache_flags;

	(void)eio_policy_init(dmc);

	dmc->block_size = header->sbf.block_size;
	dmc->block_shift = ffs(dmc->block_size) - 1;
	dmc->block_mask = dmc->block_size - 1;
	dmc->size = header->sbf.size;
	dmc->cache_size = header->sbf.cache_devsize;
	dmc->assoc = header->sbf.assoc;
	dmc->consecutive_shift = ffs(dmc->assoc) - 1;
	dmc->md_start_sect = header->sbf.cache_md_start_sect;
	dmc->md_sectors = header->sbf.cache_data_start_sect;
	dmc->sysctl_active.dirty_high_threshold =
		header->sbf.dirty_high_threshold;
	dmc->sysctl_active.dirty_low_threshold =
		header->sbf.dirty_low_threshold;
	dmc->sysctl_active.dirty_set_high_threshold =
		header->sbf.dirty_set_high_threshold;
	dmc->sysctl_active.dirty_set_low_threshold =
		header->sbf.dirty_set_low_threshold;
	dmc->sysctl_active.time_based_clean_interval =
		header->sbf.time_based_clean_interval;
	dmc->sysctl_active.autoclean_threshold =
		header->sbf.autoclean_threshold;

	if ((i = eio_mem_init(dmc)) == -1) {
		pr_err("eio_md_load: Failed to initialize memory.");
		ret = -EINVAL;
		goto free_header;
	}

	order =
		dmc->size *
		((i ==
		  1) ? sizeof(struct cacheblock_md8) : sizeof(struct cacheblock));
	data_size = dmc->size * dmc->block_size;
	size =
		EIO_MD8(dmc) ? sizeof(struct cacheblock_md8) : sizeof(struct
								      cacheblock);
	pr_info("Allocate %luKB (%ldB per) mem for %lu-entry cache "
		"(capacity:%luMB, associativity:%u, block size:%u bytes)",
		order >> 10, size, (long unsigned int)dmc->size,
		(long unsigned int)(dmc->md_sectors + data_size) >> (20 -
								     SECTOR_SHIFT),
		dmc->assoc, dmc->block_size << SECTOR_SHIFT);

	if (EIO_MD8(dmc))
		dmc->cache_md8 =
			(struct cacheblock_md8 *)vmalloc((size_t)order);
	else
		dmc->cache = (struct cacheblock *)vmalloc((size_t)order);

	if ((EIO_MD8(dmc) && !dmc->cache_md8) || (!EIO_MD8(dmc) && !dmc->cache)) {
		pr_err("md_load: Unable to allocate memory");
		vfree((void *)header);
		return 1;
	}

	if (eio_repl_blk_init(dmc->policy_ops) != 0) {
		vfree((void *)EIO_CACHE(dmc));
		pr_err
			("md_load: Unable to allocate memory for policy cache block");
		ret = -EINVAL;
		goto free_header;
	}

	/* Allocate pages of the order dmc->bio_nr_pages */
	page_count = 0;
	pages = eio_alloc_pages(dmc->bio_nr_pages, &page_count);
	if (!pages) {
		pr_err("md_create: unable to allocate pages");
		pr_err("md_create: Could not write out cache metadata");
		vfree((void *)EIO_CACHE(dmc));
		ret = -ENOMEM;
		goto free_header;
	}

	/* nr_pages is used for freeing the pages */
	nr_pages = page_count;

	pg_virt_addr = kmalloc(nr_pages * (sizeof(void *)), GFP_KERNEL);
	if (pg_virt_addr == NULL) {
		pr_err("eio_md_store: System memory too low.");
		for (i = 0; i < nr_pages; i++)
			put_page(pages[i].bv_page);
		kfree(pages);
		ret = -ENOMEM;
		goto free_header;
	}

	for (i = 0; i < nr_pages; i++)
		pg_virt_addr[i] = kmap(pages[i].bv_page);

	/*
	 * Read 1 PAGE of the metadata at a time and load up the
	 * incore metadata struct.
	 */

	page_index = 0;
	page_count = 0;
	meta_data_cacheblock =
		(struct flash_cacheblock *)pg_virt_addr[page_index];

	where.bdev = dmc->cache_dev->bdev;
	where.sector = dmc->md_start_sect;
	size = dmc->size;
	i = 0;
	while (size > 0) {
		slots_read =
			min((long)size, ((long)MD_BLOCKS_PER_PAGE * nr_pages));

		if (slots_read % MD_BLOCKS_PER_SECTOR)
			where.count = 1 + (slots_read / MD_BLOCKS_PER_SECTOR);
		else
			where.count = slots_read / MD_BLOCKS_PER_SECTOR;

		if (slots_read % MD_BLOCKS_PER_PAGE)
			page_count = 1 + (slots_read / MD_BLOCKS_PER_PAGE);
		else
			page_count = slots_read / MD_BLOCKS_PER_PAGE;

		sectors_read += where.count;    /* Debug */
		error = eio_io_sync_vm(dmc, &where, READ, pages, page_count);
		if (error) {
			vfree((void *)EIO_CACHE(dmc));
			pr_err
				("md_load: Could not read cache metadata sector %lu error %d",
				where.sector, error);
			ret = -EIO;
			goto free_md;
		}

		where.sector += where.count;
		next_ptr = meta_data_cacheblock;

		for (j = 0, page_index = 0; j < slots_read; j++) {

			if ((j % MD_BLOCKS_PER_PAGE) == 0)
				next_ptr =
					(struct flash_cacheblock *)
					pg_virt_addr[page_index++];

			// If unclean shutdown, only the DIRTY blocks are loaded.
			if (clean_shutdown || (next_ptr->cache_state & DIRTY)) {

				if (next_ptr->cache_state & DIRTY)
					dirty_loaded++;

				EIO_CACHE_STATE_SET(dmc, i,
						    (u_int8_t)next_ptr->
						    cache_state & ~QUEUED);

				EIO_ASSERT((EIO_CACHE_STATE_GET(dmc, i) &
					(VALID | INVALID))
				       != (VALID | INVALID));

				if (EIO_CACHE_STATE_GET(dmc, i) & VALID)
					num_valid++;
				EIO_DBN_SET(dmc, i, next_ptr->dbn);
			} else
				eio_invalidate_md(dmc, i);
			next_ptr++;
			i++;
		}
		size -= slots_read;
	}

	/*
	 * If the cache contains dirty data, the only valid mode is write back.
	 */
	if (dirty_loaded && dmc->mode != CACHE_MODE_WB) {
		vfree((void *)EIO_CACHE(dmc));
		pr_err
			("md_load: Cannot use %s mode because dirty data exists in the cache",
			(dmc->mode ==
			 CACHE_MODE_RO) ? "read only" : "write through");
		ret = -EINVAL;
		goto free_md;
	}

	/* Debug Tests */
	sectors_expected = dmc->size / MD_BLOCKS_PER_SECTOR;
	if (dmc->size % MD_BLOCKS_PER_SECTOR)
		sectors_expected++;
	if (sectors_expected != sectors_read) {
		pr_err
			("md_load: Sector mismatch! sectors_expected=%ld, sectors_read=%ld\n",
			sectors_expected, sectors_read);
		vfree((void *)EIO_CACHE(dmc));
		ret = -EIO;
		goto free_md;
	}

	/* Before we finish loading, we need to dirty the superblock and write it out */
	dmc->sb_state = CACHE_MD_STATE_DIRTY;
	error = eio_sb_store(dmc);
	if (error) {
		vfree((void *)EIO_CACHE(dmc));
		pr_err
			("md_load: Could not write cache superblock sector(error %d)",
			error);
		ret = 1;
		goto free_md;
	}

free_md:
	for (i = 0; i < nr_pages; i++)
		kunmap(pages[i].bv_page);
	kfree(pg_virt_addr);

	if (pages) {
		for (i = 0; i < nr_pages; i++)
			put_page(pages[i].bv_page);
		kfree(pages);
		pages = NULL;
	}

free_header:
	/* Free header page here */
	if (header_page) {
		kunmap(header_page[0].bv_page);
		put_page(header_page[0].bv_page);
		kfree(header_page);
		header_page = NULL;
	}

	pr_info("Cache metadata loaded from disk with %d valid %d dirty blocks",
		num_valid, dirty_loaded);
	return ret;
}

void eio_policy_free(struct cache_c *dmc)
{

	if (dmc->policy_ops != NULL) {
		eio_put_policy(dmc->policy_ops);
		vfree(dmc->policy_ops);
	}
	if (dmc->sp_cache_blk != NULL)
		vfree(dmc->sp_cache_blk);
	if (dmc->sp_cache_set != NULL)
		vfree(dmc->sp_cache_set);

	dmc->policy_ops = NULL;
	dmc->sp_cache_blk = dmc->sp_cache_set = NULL;
	return;
}

static int eio_clean_thread_init(struct cache_c *dmc)
{
	INIT_LIST_HEAD(&dmc->cleanq);
	spin_lock_init(&dmc->clean_sl);
	EIO_INIT_EVENT(&dmc->clean_event);
	return eio_start_clean_thread(dmc);
}

int
eio_handle_ssd_message(char *cache_name, char *ssd_name, enum dev_notifier note)
{
	struct cache_c *dmc;

	dmc = eio_cache_lookup(cache_name);
	if (NULL == dmc) {
		pr_err("eio_handle_ssd_message: cache %s does not exist",
		       cache_name);
		return -EINVAL;
	}

	switch (note) {

	case NOTIFY_SSD_ADD:
		/* Making sure that CACHE state is not active */
		if (CACHE_FAILED_IS_SET(dmc) || CACHE_DEGRADED_IS_SET(dmc))
			eio_resume_caching(dmc, ssd_name);
		else
			pr_err
				("eio_handle_ssd_message: SSD_ADD event called for ACTIVE cache \"%s\", ignoring!!!",
				dmc->cache_name);
		break;

	case NOTIFY_SSD_REMOVED:
		eio_suspend_caching(dmc, note);
		break;

	default:
		pr_err("Wrong notifier passed for eio_handle_ssd_message\n");
	}

	return 0;
}

static void eio_init_ssddev_props(struct cache_c *dmc)
{
	struct request_queue *rq;
	uint32_t max_hw_sectors, max_nr_pages;
	uint32_t nr_pages = 0;

	rq = bdev_get_queue(dmc->cache_dev->bdev);
	max_hw_sectors = to_bytes(queue_max_hw_sectors(rq)) / PAGE_SIZE;
	max_nr_pages = (u_int32_t)bio_get_nr_vecs(dmc->cache_dev->bdev);
	nr_pages = min_t(u_int32_t, max_hw_sectors, max_nr_pages);
	dmc->bio_nr_pages = nr_pages;

	/*
	 * If the cache device is not a physical device (eg: lv), then
	 * driverfs_dev will be null and we make cache_gendisk_name a null
	 * string. The eio_notify_ssd_rm() function in this case,
	 * cannot detect device removal, and therefore, we will have to rely
	 * on user space udev for the notification.
	 */

	if (dmc->cache_dev && dmc->cache_dev->bdev &&
	    dmc->cache_dev->bdev->bd_disk &&
	    dmc->cache_dev->bdev->bd_disk->driverfs_dev) {
		strncpy(dmc->cache_gendisk_name,
			dev_name(dmc->cache_dev->bdev->bd_disk->driverfs_dev),
			DEV_PATHLEN);
	} else
		dmc->cache_gendisk_name[0] = '\0';
}

static void eio_init_srcdev_props(struct cache_c *dmc)
{
	/* Same applies for source device as well. */
	if (dmc->disk_dev && dmc->disk_dev->bdev &&
	    dmc->disk_dev->bdev->bd_disk &&
	    dmc->disk_dev->bdev->bd_disk->driverfs_dev) {
		strncpy(dmc->cache_srcdisk_name,
			dev_name(dmc->disk_dev->bdev->bd_disk->driverfs_dev),
			DEV_PATHLEN);
	} else
		dmc->cache_srcdisk_name[0] = '\0';
}

int eio_cache_create(struct cache_rec_short *cache)
{
	struct cache_c *dmc;
	struct cache_c **nodepp;
	unsigned int consecutive_blocks;
	u_int64_t i;
	index_t prev_set;
	index_t cur_set;
	sector_t order;
	int error = -EINVAL;
	uint32_t persistence = 0;
	fmode_t mode = (FMODE_READ | FMODE_WRITE);
	char *strerr = NULL;

	dmc = (struct cache_c *)kzalloc(sizeof(*dmc), GFP_KERNEL);
	if (dmc == NULL) {
		strerr = "Failed to allocate memory for cache context";
		error = -ENOMEM;
		goto bad;
	}

	/*
	 * Source device.
	 */

	error = eio_ttc_get_device(cache->cr_src_devname, mode, &dmc->disk_dev);
	if (error) {
		strerr = "get_device for source device failed";
		goto bad1;
	}
	if (NULL == dmc->disk_dev) {
		error = -EINVAL;
		strerr = "Failed to lookup source device";
		goto bad1;
	}
	if ((dmc->disk_size =
		     to_sector(eio_get_device_size(dmc->disk_dev))) >= EIO_MAX_SECTOR) {
		strerr = "Source device too big to support";
		error = -EFBIG;
		goto bad2;
	}
	strncpy(dmc->disk_devname, cache->cr_src_devname, DEV_PATHLEN);

	/*
	 * Cache device.
	 */

	error =
		eio_ttc_get_device(cache->cr_ssd_devname, mode, &dmc->cache_dev);
	if (error) {
		strerr = "get_device for cache device failed";
		goto bad2;
	}
	if (NULL == dmc->cache_dev) {
		error = -EINVAL;
		strerr = "Failed to lookup source device";
		goto bad2;
	}
	if (dmc->disk_dev == dmc->cache_dev) {
		error = -EINVAL;
		strerr = "Same devices specified";
		goto bad3;
	}
	strncpy(dmc->cache_devname, cache->cr_ssd_devname, DEV_PATHLEN);

	if (cache->cr_name[0] != '\0') {
		strncpy(dmc->cache_name, cache->cr_name,
			sizeof(dmc->cache_name));
		/* make sure it is zero terminated */
		dmc->cache_name[sizeof(dmc->cache_name) - 1] = '\x00';
	} else {
		strerr = "Need cache name";
		error = -EINVAL;
		goto bad3;
	}

	strncpy(dmc->ssd_uuid, cache->cr_ssd_uuid, DEV_PATHLEN - 1);

	dmc->cache_dev_start_sect = eio_get_device_start_sect(dmc->cache_dev);
	error = eio_do_preliminary_checks(dmc);
	if (error) {
		if (error == -EINVAL)
			strerr = "Either Source and Cache devices belong to "
				 "same device or a cache already exists on"
				 " specified source device";
		else if (error == -EEXIST)
			strerr = "Cache already exists";
		goto bad3;
	}

	eio_init_ssddev_props(dmc);
	eio_init_srcdev_props(dmc);

	/*
	 * Initialize the io callback queue.
	 */

	dmc->callback_q = create_singlethread_workqueue("eio_callback");
	if (!dmc->callback_q) {
		error = -ENOMEM;
		strerr = "Failed to initialize callback workqueue";
		goto bad4;
	}
	error = eio_kcached_init(dmc);
	if (error) {
		strerr = "Failed to initialize kcached";
		goto bad4;
	}

	/*
	 * We read policy before reading other args. The reason is that
	 * if there is a policy module loaded, we first need dmc->p_ops to be
	 * allocated so that it is non NULL. Once p_ops is !NULL, cache_blk_init
	 * and cache_set_init can set their pointers to dmc->p_ops->xxx
	 *
	 * policy_ops == NULL is not really an error. It just means that there
	 * is no registered policy and therefore we use EIO_REPL_RANDOM (random)
	 * as the replacement policy.
	 */

	/* We do a kzalloc for dmc, but being extra careful here */
	dmc->sp_cache_blk = NULL;
	dmc->sp_cache_set = NULL;
	dmc->policy_ops = NULL;
	if (cache->cr_policy) {
		dmc->req_policy = cache->cr_policy;
		if (dmc->req_policy && (dmc->req_policy < CACHE_REPL_FIRST ||
					dmc->req_policy > CACHE_REPL_LAST)) {
			strerr = "Invalid cache policy";
			error = -EINVAL;
			goto bad5;
		}
	}

	/*
	 * We need to determine the requested cache mode before we call
	 * eio_md_load becuase it examines dmc->mode. The cache mode is
	 * set as follows:
	 * 1. For a "reload" operation:
	 *      - if mode is not provided as an argument,
	   it is read from superblock.
	 *      - if mode is provided as an argument,
	   eio_md_load verifies that it is valid.
	 * 2. For a "create" operation:
	 *      - if mode is not provided, it is set to CACHE_MODE_DEFAULT.
	 *      - if mode is provided, it is validate and set.
	 */
	if (cache->cr_mode) {
		dmc->mode = cache->cr_mode;
		if (dmc->mode && (dmc->mode < CACHE_MODE_FIRST ||
				  dmc->mode > CACHE_MODE_LAST)) {
			strerr = "Invalid cache mode";
			error = -EINVAL;
			goto bad5;
		}
	}

	dmc->cold_boot = cache->cr_cold_boot;
	if ((dmc->cold_boot != 0) && (dmc->cold_boot != BOOT_FLAG_COLD_ENABLE)) {
		strerr = "Invalid cold boot option";
		error = -EINVAL;
		goto bad5;
	}

	if (cache->cr_persistence) {
		persistence = cache->cr_persistence;
		if (persistence < CACHE_RELOAD ||
		    persistence > CACHE_FORCECREATE) {
			pr_err("ctr: persistence = %d", persistence);
			strerr = "Invalid cache persistence";
			error = -EINVAL;
			goto bad5;
		}
		dmc->persistence = persistence;
	}
	if (persistence == CACHE_RELOAD) {
		if (eio_md_load(dmc)) {
			strerr = "Failed to reload cache";
			error = -EINVAL;
			goto bad5;
		}

		/*
		 * "eio_md_load" will reset "dmc->persistence" from
		 * CACHE_RELOAD to CACHE_FORCECREATE in the case of
		 * cache superblock version mismatch and cache mode
		 * is Read-Only or Write-Through.
		 */
		if (dmc->persistence != persistence)
			persistence = dmc->persistence;
	}

	/*
	 * Now that we're back from "eio_md_load" in the case of a reload,
	 * we're ready to finish setting up the mode and policy.
	 */
	if (dmc->mode == 0) {
		dmc->mode = CACHE_MODE_DEFAULT;
		pr_info("Setting mode to default");
	} else {
		pr_info("Setting mode to %s ",
			(dmc->mode == CACHE_MODE_WB) ? "write back" :
			((dmc->mode == CACHE_MODE_RO) ? "read only" :
			 "write through"));
	}

	/* eio_policy_init() is already called from within eio_md_load() */
	if (persistence != CACHE_RELOAD)
		(void)eio_policy_init(dmc);

	if (cache->cr_flags) {
		int flags;
		flags = cache->cr_flags;
		if (flags == 0)
			dmc->cache_flags &= ~CACHE_FLAGS_INVALIDATE;
		else if (flags == 1) {
			dmc->cache_flags |= CACHE_FLAGS_INVALIDATE;
			pr_info("Enabling invalidate API");
		} else
			pr_info("Ignoring unknown flags value: %u", flags);
	}

	if (persistence == CACHE_RELOAD)
		goto init;      /* Skip reading cache parameters from command line */

	if (cache->cr_blksize && cache->cr_ssd_sector_size) {
		dmc->block_size = cache->cr_blksize / cache->cr_ssd_sector_size;
		if (dmc->block_size & (dmc->block_size - 1)) {
			strerr = "Invalid block size";
			error = -EINVAL;
			goto bad5;
		}
		if (dmc->block_size == 0)
			dmc->block_size = DEFAULT_CACHE_BLKSIZE;
	} else
		dmc->block_size = DEFAULT_CACHE_BLKSIZE;
	dmc->block_shift = ffs(dmc->block_size) - 1;
	dmc->block_mask = dmc->block_size - 1;

	/*
	 * dmc->size is specified in sectors here, and converted to blocks later
	 *
	 * Giving preference to kernel got cache size.
	 * Only when we can't get the cache size in kernel, we accept user passed size.
	 * User mode may be using a different API or could also do some rounding, so we
	 * prefer kernel getting the cache size. In case of device failure and coming back, we
	 * rely on the device size got in kernel and we hope that it is equal to the
	 * one we used for creating the cache, so we ideally should always use the kernel
	 * got cache size.
	 */
	dmc->size = to_sector(eio_get_device_size(dmc->cache_dev));
	if (dmc->size == 0) {
		if (cache->cr_ssd_dev_size && cache->cr_ssd_sector_size)
			dmc->size =
				cache->cr_ssd_dev_size / cache->cr_ssd_sector_size;

		if (dmc->size == 0) {
			strerr = "Invalid cache size or can't be fetched";
			error = -EINVAL;
			goto bad5;
		}
	}

	dmc->cache_size = dmc->size;

	if (cache->cr_assoc) {
		dmc->assoc = cache->cr_assoc;
		if ((dmc->assoc & (dmc->assoc - 1)) ||
		    dmc->assoc > EIO_MAX_ASSOC || dmc->size < dmc->assoc) {
			strerr = "Invalid cache associativity";
			error = -EINVAL;
			goto bad5;
		}
		if (dmc->assoc == 0)
			dmc->assoc = DEFAULT_CACHE_ASSOC;
	} else
		dmc->assoc = DEFAULT_CACHE_ASSOC;

	/*
	 * initialize to an invalid index
	 */

	dmc->index_zero = dmc->assoc + 1;

	/*
	 * Although it's very unlikely, we need to make sure that
	 * for the given associativity and block size our source
	 * device will have less than 4 billion sets.
	 */

	i = to_sector(eio_get_device_size(dmc->disk_dev)) /
	    (dmc->assoc * dmc->block_size);
	if (i >= (((u_int64_t)1) << 32)) {
		strerr = "Too many cache sets to support";
		goto bad5;
	}

	consecutive_blocks = dmc->assoc;
	dmc->consecutive_shift = ffs(consecutive_blocks) - 1;

	/* Initialize persistent thresholds */
	dmc->sysctl_active.dirty_high_threshold = DIRTY_HIGH_THRESH_DEF;
	dmc->sysctl_active.dirty_low_threshold = DIRTY_LOW_THRESH_DEF;
	dmc->sysctl_active.dirty_set_high_threshold = DIRTY_SET_HIGH_THRESH_DEF;
	dmc->sysctl_active.dirty_set_low_threshold = DIRTY_SET_LOW_THRESH_DEF;
	dmc->sysctl_active.autoclean_threshold = AUTOCLEAN_THRESH_DEF;
	dmc->sysctl_active.time_based_clean_interval =
		TIME_BASED_CLEAN_INTERVAL_DEF(dmc);

	spin_lock_init(&dmc->cache_spin_lock);
	if (persistence == CACHE_CREATE) {
		error = eio_md_create(dmc, /* force */ 0, /* cold */ 1);
		if (error) {
			strerr = "Failed to create cache";
			goto bad5;
		}
	} else {
		error = eio_md_create(dmc, /* force */ 1, /* cold */ 1);
		if (error) {
			strerr = "Failed to force create cache";
			goto bad5;
		}
	}

init:
	order = (dmc->size >> dmc->consecutive_shift) *
		sizeof(struct cache_set);

	if (!eio_mem_available(dmc, order)) {
		strerr = "System memory too low"
			 " for allocating cache set metadata";
		error = -ENOMEM;
		vfree((void *)EIO_CACHE(dmc));
		goto bad5;
	}

	dmc->cache_sets = (struct cache_set *)vmalloc((size_t)order);
	if (!dmc->cache_sets) {
		strerr = "Failed to allocate memory";
		error = -ENOMEM;
		vfree((void *)EIO_CACHE(dmc));
		goto bad5;
	}

	for (i = 0; i < (dmc->size >> dmc->consecutive_shift); i++) {
		dmc->cache_sets[i].nr_dirty = 0;
		spin_lock_init(&dmc->cache_sets[i].cs_lock);
		init_rwsem(&dmc->cache_sets[i].rw_lock);
		dmc->cache_sets[i].mdreq = NULL;
		dmc->cache_sets[i].flags = 0;
	}
	error = eio_repl_sets_init(dmc->policy_ops);
	if (error < 0) {
		strerr = "Failed to allocate memory for cache policy";
		vfree((void *)dmc->cache_sets);
		vfree((void *)EIO_CACHE(dmc));
		goto bad5;
	}
	eio_policy_lru_pushblks(dmc->policy_ops);

	if (dmc->mode == CACHE_MODE_WB) {
		error = eio_allocate_wb_resources(dmc);
		if (error) {
			vfree((void *)dmc->cache_sets);
			vfree((void *)EIO_CACHE(dmc));
			goto bad5;
		}
	}

	dmc->sysctl_active.error_inject = 0;
	dmc->sysctl_active.fast_remove = 0;
	dmc->sysctl_active.zerostats = 0;
	dmc->sysctl_active.do_clean = 0;

	atomic_set(&dmc->clean_index, 0);

	atomic64_set(&dmc->nr_ios, 0);

	/*
	 * sysctl_mem_limit_pct [0 - 100]. Before doing a vmalloc()
	 * make sure that the allocation size requested is less than
	 * sysctl_mem_limit_pct percentage of the free RAM available
	 * in the system. This is to avoid OOM errors in Linux.
	 * 0 => do the vmalloc without checking system memory.
	 */

	dmc->sysctl_active.mem_limit_pct = 75;

	(void)wait_on_bit_lock((void *)&eio_control->synch_flags,
			       EIO_UPDATE_LIST, eio_wait_schedule,
			       TASK_UNINTERRUPTIBLE);
	dmc->next_cache = cache_list_head;
	cache_list_head = dmc;
	clear_bit(EIO_UPDATE_LIST, (void *)&eio_control->synch_flags);
	smp_mb__after_clear_bit();
	wake_up_bit((void *)&eio_control->synch_flags, EIO_UPDATE_LIST);

	prev_set = -1;
	for (i = 0; i < dmc->size; i++) {
		if (EIO_CACHE_STATE_GET(dmc, i) & VALID)
			atomic64_inc(&dmc->eio_stats.cached_blocks);
		if (EIO_CACHE_STATE_GET(dmc, i) & DIRTY) {
			dmc->cache_sets[i / dmc->assoc].nr_dirty++;
			atomic64_inc(&dmc->nr_dirty);
			cur_set = i / dmc->assoc;
			if (prev_set != cur_set) {
				/* Move the given set at the head of the set LRU list */
				eio_touch_set_lru(dmc, cur_set);
				prev_set = cur_set;
			}
		}
	}

	INIT_WORK(&dmc->readfill_wq, eio_do_readfill);

	/*
	 * invalid index, but signifies cache successfully built
	 */

	dmc->index_zero = dmc->assoc;

	eio_procfs_ctr(dmc);

	/*
	 * Activate Application Transparent Caching.
	 */

	error = eio_ttc_activate(dmc);
	if (error)
		goto bad6;

	/*
	 * In future if anyone adds code here and something fails,
	 * do call eio_ttc_deactivate(dmc) as part of cleanup.
	 */

	return 0;

bad6:
	eio_procfs_dtr(dmc);
	if (dmc->mode == CACHE_MODE_WB) {
		eio_stop_async_tasks(dmc);
		eio_free_wb_resources(dmc);
	}
	vfree((void *)dmc->cache_sets);
	vfree((void *)EIO_CACHE(dmc));

	(void)wait_on_bit_lock((void *)&eio_control->synch_flags,
			       EIO_UPDATE_LIST, eio_wait_schedule,
			       TASK_UNINTERRUPTIBLE);
	nodepp = &cache_list_head;
	while (*nodepp != NULL) {
		if (*nodepp == dmc) {
			*nodepp = dmc->next_cache;
			break;
		}
		nodepp = &((*nodepp)->next_cache);
	}
	clear_bit(EIO_UPDATE_LIST, (void *)&eio_control->synch_flags);
	smp_mb__after_clear_bit();
	wake_up_bit((void *)&eio_control->synch_flags, EIO_UPDATE_LIST);
bad5:
	eio_kcached_client_destroy(dmc);
bad4:
bad3:
	eio_put_cache_device(dmc);
bad2:
	eio_ttc_put_device(&dmc->disk_dev);
bad1:
	eio_policy_free(dmc);
	kfree(dmc);
bad:
	if (strerr)
		pr_err("Cache creation failed: %s.\n", strerr);
	return error;
}

/*
 * Destroy the cache mapping.
 */

int eio_cache_delete(char *cache_name, int do_delete)
{
	struct cache_c *dmc;
	struct cache_c **nodepp;
	int ret, error;
	int restart_async_task;

	ret = 0;
	restart_async_task = 0;

	dmc = eio_cache_lookup(cache_name);
	if (NULL == dmc) {
		pr_err("cache delete: cache \"%s\" doesn't exist.", cache_name);
		return -EINVAL;
	}

	spin_lock_irqsave(&dmc->cache_spin_lock, dmc->cache_spin_lock_flags);
	if (dmc->cache_flags & CACHE_FLAGS_SHUTDOWN_INPROG) {
		pr_err("cache_delete: system shutdown in progress, cannot "
		       "delete cache %s", cache_name);
		spin_unlock_irqrestore(&dmc->cache_spin_lock,
				       dmc->cache_spin_lock_flags);
		return -EINVAL;
	}
	if (dmc->cache_flags & CACHE_FLAGS_MOD_INPROG) {
		pr_err
			("cache_delete: simultaneous edit/delete operation on cache"
			" %s is not permitted", cache_name);
		spin_unlock_irqrestore(&dmc->cache_spin_lock,
				       dmc->cache_spin_lock_flags);
		return -EINVAL;
	}
	dmc->cache_flags |= CACHE_FLAGS_MOD_INPROG;
	spin_unlock_irqrestore(&dmc->cache_spin_lock,
			       dmc->cache_spin_lock_flags);

	/*
	 * Earlier attempt to delete failed.
	 * Allow force deletes only for FAILED caches.
	 */
	if (unlikely(CACHE_STALE_IS_SET(dmc))) {
		if (likely(CACHE_FAILED_IS_SET(dmc))) {
			pr_err
				("cache_delete: Cache \"%s\" is in STALE state. Force deleting!!!",
				dmc->cache_name);
			goto force_delete;
		} else {
			if (atomic64_read(&dmc->nr_dirty) != 0) {
				spin_lock_irqsave(&dmc->cache_spin_lock,
						  dmc->cache_spin_lock_flags);
				dmc->cache_flags &= ~CACHE_FLAGS_MOD_INPROG;
				spin_unlock_irqrestore(&dmc->cache_spin_lock,
						       dmc->
						       cache_spin_lock_flags);
				pr_err
					("cache_delete: Stale Cache detected with dirty blocks=%ld.\n",
					atomic64_read(&dmc->nr_dirty));
				pr_err
					("cache_delete: Cache \"%s\" wont be deleted. Deleting will result in data corruption.\n",
					dmc->cache_name);
				return -EINVAL;
			}
		}
	}

	eio_stop_async_tasks(dmc);

	/*
	 * Deactivate Application Transparent Caching.
	 * For wb cache, finish_nr_dirty may take long time.
	 * It should be guaranteed that normal cache delete should succeed
	 * only when finish_nr_dirty is completely done.
	 */

	if (eio_ttc_deactivate(dmc, 0)) {

		/* If deactivate fails; only option is to delete cache. */
		pr_err("cache_delete: Failed to deactivate the cache \"%s\".",
		       dmc->cache_name);
		if (CACHE_FAILED_IS_SET(dmc))
			pr_err
				("cache_delete: Use -f option to delete the cache \"%s\".",
				dmc->cache_name);
		ret = -EPERM;
		dmc->cache_flags |= CACHE_FLAGS_STALE;

		/* Restart async tasks. */
		restart_async_task = 1;
		goto out;
	}

	if (!CACHE_FAILED_IS_SET(dmc))
		EIO_ASSERT(dmc->sysctl_active.fast_remove
		       || (atomic64_read(&dmc->nr_dirty) == 0));

	/*
	 * If ttc_deactivate succeeded... proceed with cache delete.
	 * Dont entertain device failure hereafter.
	 */
	if (unlikely(CACHE_FAILED_IS_SET(dmc)) ||
	    unlikely(CACHE_DEGRADED_IS_SET(dmc))) {
		pr_err
			("cache_delete: Cannot update metadata of cache \"%s\" in failed/degraded mode.",
			dmc->cache_name);
	} else
		eio_md_store(dmc);

force_delete:
	eio_procfs_dtr(dmc);

	if (CACHE_STALE_IS_SET(dmc)) {
		pr_info("Force deleting cache \"%s\"!!!.", dmc->cache_name);
		eio_ttc_deactivate(dmc, 1);
	}

	eio_free_wb_resources(dmc);
	vfree((void *)EIO_CACHE(dmc));
	vfree((void *)dmc->cache_sets);
	eio_ttc_put_device(&dmc->disk_dev);
	eio_put_cache_device(dmc);
	(void)wait_on_bit_lock((void *)&eio_control->synch_flags,
			       EIO_UPDATE_LIST, eio_wait_schedule,
			       TASK_UNINTERRUPTIBLE);
	nodepp = &cache_list_head;
	while (*nodepp != NULL) {
		if (*nodepp == dmc) {
			*nodepp = dmc->next_cache;
			break;
		}
		nodepp = &((*nodepp)->next_cache);
	}
	clear_bit(EIO_UPDATE_LIST, &eio_control->synch_flags);
	smp_mb__after_clear_bit();
	wake_up_bit((void *)&eio_control->synch_flags, EIO_UPDATE_LIST);

out:
	if (restart_async_task) {
		EIO_ASSERT(dmc->clean_thread == NULL);
		error = eio_start_clean_thread(dmc);
		if (error)
			pr_err
				("cache_delete: Failed to restart async tasks. error=%d\n",
				error);
	}
	spin_lock_irqsave(&dmc->cache_spin_lock, dmc->cache_spin_lock_flags);
	dmc->cache_flags &= ~CACHE_FLAGS_MOD_INPROG;
	if (!ret)
		dmc->cache_flags |= CACHE_FLAGS_DELETED;
	spin_unlock_irqrestore(&dmc->cache_spin_lock,
			       dmc->cache_spin_lock_flags);

	if (!ret) {
		eio_policy_free(dmc);

		/*
		 * We don't need synchronisation since at this point the dmc is
		 * no more accessible via lookup.
		 */

		if (!(dmc->cache_flags & CACHE_FLAGS_SHUTDOWN_INPROG))
			kfree(dmc);
	}

	return ret;
}

/*
 * Reconstruct a degraded cache after the SSD is added.
 * This function mimics the constructor eio_ctr() except
 * for code that does not require re-initialization.
 */
int eio_ctr_ssd_add(struct cache_c *dmc, char *dev)
{
	int r = 0;
	struct eio_bdev *prev_cache_dev;
	u_int32_t prev_persistence = dmc->persistence;
	fmode_t mode = (FMODE_READ | FMODE_WRITE);

	/* verify if source device is present */
	EIO_ASSERT(dmc->eio_errors.no_source_dev == 0);

	/* mimic relevant portions from eio_ctr() */

	prev_cache_dev = dmc->cache_dev;
	r = eio_ttc_get_device(dev, mode, &dmc->cache_dev);
	if (r) {
		dmc->cache_dev = prev_cache_dev;
		pr_err("ctr_ssd_add: Failed to lookup cache device %s", dev);
		return -EINVAL;
	}
	/*
	 * For Linux, we have to put the old SSD device now because
	 * we did not do so during SSD removal.
	 */
	eio_ttc_put_device(&prev_cache_dev);

	/* sanity check */
	if (dmc->cache_size != to_sector(eio_get_device_size(dmc->cache_dev))) {
		pr_err("ctr_ssd_add: Cache device size has changed, expected (%lu) found (%lu) \
				continuing in degraded mode", dmc->cache_size,
		       to_sector(eio_get_device_size(dmc->cache_dev)));
		r = -EINVAL;
		goto out;
	}

	/* sanity check for cache device start sector */
	if (dmc->cache_dev_start_sect !=
	    eio_get_device_start_sect(dmc->cache_dev)) {
		pr_err("ctr_ssd_add: Cache device starting sector changed, \
				expected (%lu) found (%lu) continuing in \
				degraded mode", dmc->cache_dev_start_sect, eio_get_device_start_sect(dmc->cache_dev));
		r = -EINVAL;
		goto out;
	}

	strncpy(dmc->cache_devname, dev, DEV_PATHLEN);
	eio_init_ssddev_props(dmc);
	dmc->size = dmc->cache_size;    /* dmc->size will be recalculated in eio_md_create() */

	/*
	 * In case of writeback mode, trust the content of SSD and reload the MD.
	 */
	dmc->persistence = CACHE_FORCECREATE;

	eio_policy_free(dmc);
	(void)eio_policy_init(dmc);

	r = eio_md_create(dmc, /* force */ 1, /* cold */
			  (dmc->mode != CACHE_MODE_WB));
	if (r) {
		pr_err
			("ctr_ssd_add: Failed to create md, continuing in degraded mode");
		goto out;
	}

	r = eio_repl_sets_init(dmc->policy_ops);
	if (r < 0) {
		pr_err
			("ctr_ssd_add: Failed to allocate memory for cache policy");
		goto out;
	}
	eio_policy_lru_pushblks(dmc->policy_ops);
	if (dmc->mode != CACHE_MODE_WB)
		/* Cold cache will reset the stats */
		memset(&dmc->eio_stats, 0, sizeof(dmc->eio_stats));

	return 0;
out:
	dmc->persistence = prev_persistence;

	return r;
}

/*
 * Stop the async tasks for a cache(threads, scheduled works).
 * Used during the cache remove
 */
void eio_stop_async_tasks(struct cache_c *dmc)
{
	unsigned long flags = 0;

	if (dmc->clean_thread) {
		dmc->sysctl_active.fast_remove = 1;
		spin_lock_irqsave(&dmc->clean_sl, flags);
		EIO_SET_EVENT_AND_UNLOCK(&dmc->clean_event, &dmc->clean_sl,
					 flags);
		eio_wait_thread_exit(dmc->clean_thread,
				     &dmc->clean_thread_running);
		EIO_CLEAR_EVENT(&dmc->clean_event);
		dmc->clean_thread = NULL;
	}

	dmc->sysctl_active.fast_remove = CACHE_FAST_REMOVE_IS_SET(dmc) ? 1 : 0;

	if (dmc->mode == CACHE_MODE_WB) {
		/*
		 * Prevent new I/Os to schedule the time based cleaning.
		 * Cancel existing delayed work
		 */
		dmc->sysctl_active.time_based_clean_interval = 0;
		cancel_delayed_work_sync(&dmc->clean_aged_sets_work);
	}
}

int eio_start_clean_thread(struct cache_c *dmc)
{
	EIO_ASSERT(dmc->clean_thread == NULL);
	EIO_ASSERT(dmc->mode == CACHE_MODE_WB);
	EIO_ASSERT(dmc->clean_thread_running == 0);
	EIO_ASSERT(!(dmc->sysctl_active.do_clean & EIO_CLEAN_START));

	dmc->clean_thread = eio_create_thread(eio_clean_thread_proc,
					      (void *)dmc, "eio_clean_thread");
	if (!dmc->clean_thread)
		return -EFAULT;
	return 0;
}

int eio_allocate_wb_resources(struct cache_c *dmc)
{
	int nr_bvecs, nr_pages;
	unsigned iosize;
	int ret;

	EIO_ASSERT(dmc->clean_dbvecs == NULL);
	EIO_ASSERT(dmc->clean_mdpages == NULL);
	EIO_ASSERT(dmc->dbvec_count == 0);
	EIO_ASSERT(dmc->mdpage_count == 0);

	/* Data page allocations are done in terms of "bio_vec" structures */
	iosize = (dmc->block_size * dmc->assoc) << SECTOR_SHIFT;
	nr_bvecs = IO_BVEC_COUNT(iosize, dmc->block_size);
	dmc->clean_dbvecs =
		(struct bio_vec *)kmalloc(sizeof(struct bio_vec) * nr_bvecs,
					  GFP_KERNEL);
	if (dmc->clean_dbvecs == NULL) {
		pr_err("cache_create: Failed to allocated memory.\n");
		ret = -ENOMEM;
		goto errout;
	}
	/* Allocate pages for each bio_vec */
	ret = eio_alloc_wb_bvecs(dmc->clean_dbvecs, nr_bvecs, dmc->block_size);
	if (ret)
		goto errout;
	EIO_ASSERT(dmc->clean_dbvecs != NULL);
	dmc->dbvec_count = nr_bvecs;

	/* Metadata page allocations are done in terms of pages only */
	iosize = dmc->assoc * sizeof(struct flash_cacheblock);
	nr_pages = IO_PAGE_COUNT(iosize);
	dmc->clean_mdpages =
		(struct page **)kmalloc(sizeof(struct page *) * nr_pages,
					GFP_KERNEL);
	if (dmc->clean_mdpages == NULL) {
		pr_err("cache_create: Failed to allocated memory.\n");
		ret = -ENOMEM;
		eio_free_wb_bvecs(dmc->clean_dbvecs, dmc->dbvec_count,
				  dmc->block_size);
		goto errout;
	}
	ret = eio_alloc_wb_pages(dmc->clean_mdpages, nr_pages);
	if (ret) {
		eio_free_wb_bvecs(dmc->clean_dbvecs, dmc->dbvec_count,
				  dmc->block_size);
		goto errout;
	}
	EIO_ASSERT(dmc->clean_mdpages != NULL);
	dmc->mdpage_count = nr_pages;

	/*
	 * For writeback cache:
	 * 1. Initialize the time based clean work queue
	 * 2. Initialize the dirty set lru
	 * 3. Initialize clean thread
	 */

	/*
	 * Reset dmc->is_clean_aged_sets_sched.
	 * Time based clean will be enabled in eio_touch_set_lru()
	 * only when dmc->is_clean_aged_sets_sched  is zero and
	 * dmc->sysctl_active.time_based_clean_interval > 0.
	 */

	dmc->is_clean_aged_sets_sched = 0;
	INIT_DELAYED_WORK(&dmc->clean_aged_sets_work, eio_clean_aged_sets);
	dmc->dirty_set_lru = NULL;
	ret =
		lru_init(&dmc->dirty_set_lru,
			 (dmc->size >> dmc->consecutive_shift));
	if (ret == 0) {
		spin_lock_init(&dmc->dirty_set_lru_lock);
		ret = eio_clean_thread_init(dmc);
	}
	EIO_ASSERT(dmc->mdupdate_q == NULL);
	dmc->mdupdate_q = create_singlethread_workqueue("eio_mdupdate");
	if (!dmc->mdupdate_q)
		ret = -ENOMEM;

	if (ret < 0) {
		pr_err("cache_create: Failed to initialize dirty lru set or"
		       "clean/mdupdate thread for wb cache.\n");
		if (dmc->dirty_set_lru) {
			lru_uninit(dmc->dirty_set_lru);
			dmc->dirty_set_lru = NULL;
		}

		eio_free_wb_pages(dmc->clean_mdpages, dmc->mdpage_count);
		eio_free_wb_bvecs(dmc->clean_dbvecs, dmc->dbvec_count,
				  dmc->block_size);
		goto errout;
	}

	goto out;

errout:
	if (dmc->clean_mdpages) {
		kfree(dmc->clean_mdpages);
		dmc->clean_mdpages = NULL;
		dmc->mdpage_count = 0;
	}
	if (dmc->clean_dbvecs) {
		kfree(dmc->clean_dbvecs);
		dmc->clean_dbvecs = NULL;
		dmc->dbvec_count = 0;
	}

out:
	return ret;
}

void eio_free_wb_resources(struct cache_c *dmc)
{

	if (dmc->mdupdate_q) {
		flush_workqueue(dmc->mdupdate_q);
		destroy_workqueue(dmc->mdupdate_q);
		dmc->mdupdate_q = NULL;
	}
	if (dmc->dirty_set_lru) {
		lru_uninit(dmc->dirty_set_lru);
		dmc->dirty_set_lru = NULL;
	}
	if (dmc->clean_mdpages) {
		eio_free_wb_pages(dmc->clean_mdpages, dmc->mdpage_count);
		kfree(dmc->clean_mdpages);
		dmc->clean_mdpages = NULL;
	}
	if (dmc->clean_dbvecs) {
		eio_free_wb_bvecs(dmc->clean_dbvecs, dmc->dbvec_count,
				  dmc->block_size);
		kfree(dmc->clean_dbvecs);
		dmc->clean_dbvecs = NULL;
	}

	dmc->dbvec_count = dmc->mdpage_count = 0;
	return;
}

static int
eio_notify_reboot(struct notifier_block *this, unsigned long code, void *x)
{
	struct cache_c *dmc;

	if (eio_reboot_notified == EIO_REBOOT_HANDLING_DONE)
		return NOTIFY_DONE;

	(void)wait_on_bit_lock((void *)&eio_control->synch_flags,
			       EIO_HANDLE_REBOOT, eio_wait_schedule,
			       TASK_UNINTERRUPTIBLE);
	if (eio_reboot_notified == EIO_REBOOT_HANDLING_DONE) {
		clear_bit(EIO_HANDLE_REBOOT, (void *)&eio_control->synch_flags);
		smp_mb__after_clear_bit();
		wake_up_bit((void *)&eio_control->synch_flags,
			    EIO_HANDLE_REBOOT);
		return NOTIFY_DONE;
	}
	EIO_ASSERT(eio_reboot_notified == 0);
	eio_reboot_notified = EIO_REBOOT_HANDLING_INPROG;

	(void)wait_on_bit_lock((void *)&eio_control->synch_flags,
			       EIO_UPDATE_LIST, eio_wait_schedule,
			       TASK_UNINTERRUPTIBLE);
	for (dmc = cache_list_head; dmc != NULL; dmc = dmc->next_cache) {
		if (unlikely(CACHE_FAILED_IS_SET(dmc))
		    || unlikely(CACHE_DEGRADED_IS_SET(dmc))) {
			pr_err
				("notify_reboot: Cannot sync in failed / degraded mode");
			continue;
		}
		if (dmc->cold_boot && atomic64_read(&dmc->nr_dirty)
		    && !eio_force_warm_boot) {
			pr_info
				("Cold boot set for cache %s: Draining dirty blocks: %ld",
				dmc->cache_name, atomic64_read(&dmc->nr_dirty));
			eio_clean_for_reboot(dmc);
		}
		eio_md_store(dmc);
	}
	clear_bit(EIO_UPDATE_LIST, (void *)&eio_control->synch_flags);
	smp_mb__after_clear_bit();
	wake_up_bit((void *)&eio_control->synch_flags, EIO_UPDATE_LIST);

	eio_reboot_notified = EIO_REBOOT_HANDLING_DONE;
	clear_bit(EIO_HANDLE_REBOOT, (void *)&eio_control->synch_flags);
	smp_mb__after_clear_bit();
	wake_up_bit((void *)&eio_control->synch_flags, EIO_HANDLE_REBOOT);
	return NOTIFY_DONE;
}

/*
 * The SSD add/remove is handled using udev from the user space. The driver
 * is notified from the user space via dmsetup message. Both device addition
 * and removal events are handled in the driver by eio_handle_message().
 *
 * The device remove has a special case. From the time the device is removed,
 * until the time the driver gets notified from the user space could be a few msec
 * or a couple of seconds. During this time, any IO to the SSD fails. While this
 * is handled gracefully, the logs can get filled with IO error messages.
 *
 * In order to cover that gap, we handle the device removal within the kernel
 * using this function. Note that using the scsi notifier function in the kernel
 * (vs. receiving the message from user space) minimizes the notification delay
 * between the time the SSD is removed until the driver is notified. This cannot,
 * however, make this delay zero. Therefore, there will be a small window during
 * which eio_io_callback() may fail on CACHEWRITE action.
 *
 * We still need the user space (udev) method of handling for the following
 * reasons:
 * (i)  This notifier is only for a scsi device.
 * (ii) The add/remove feature in user space can also be used to dynamically
 *      turn the cache on and off.
 *
 * This notifier is used only when SSD is removed. The add event can
 * be caught using the BUS_NOTIFY_ADD_DEVICE in action. However, we only
 * get a scsi handle and do not have a reference to our device pointer.
 */
static int
eio_notify_ssd_rm(struct notifier_block *nb, unsigned long action, void *data)
{
	struct device *dev = data;
	struct cache_c *dmc;
	const char *device_name;
	size_t len;
	unsigned long int flags = 0;
	struct ssd_rm_list *ssd_list_ptr;
	unsigned check_src = 0, check_ssd = 0;
	enum dev_notifier notify = NOTIFY_INITIALIZER;

	if (likely(action != BUS_NOTIFY_DEL_DEVICE))
		return 0;

	if (unlikely(dev == NULL)) {
		pr_info("notify_cache_dev: device is NULL!");
		return 0;
	}

	if (!scsi_is_sdev_device(dev))
		return 0;

	if ((device_name = dev_name(dev)) == NULL)
		return 0;
	len = strlen(device_name);

	/* push to a list for future processing as we could be in an interrupt context */
	for (dmc = cache_list_head; dmc != NULL; dmc = dmc->next_cache) {
		notify = NOTIFY_INITIALIZER;
		check_src = ('\0' == dmc->cache_srcdisk_name[0] ? 0 : 1);
		check_ssd = ('\0' == dmc->cache_gendisk_name[0] ? 0 : 1);

		if (check_src == 0 && check_ssd == 0)
			continue;

		/*Check if source dev name or ssd dev name is available or not. */
		if (check_ssd
		    && 0 == strncmp(device_name, dmc->cache_gendisk_name,
				    len)) {
			pr_info("SSD Removed for cache name %s",
				dmc->cache_name);
			notify = NOTIFY_SSD_REMOVED;
		}

		if (check_src
		    && 0 == strncmp(device_name, dmc->cache_srcdisk_name,
				    len)) {
			pr_info("SRC Removed for cache name %s",
				dmc->cache_name);
			notify = NOTIFY_SRC_REMOVED;
		}

		if (notify == NOTIFY_INITIALIZER)
			continue;

		ssd_list_ptr = kmalloc(sizeof(struct ssd_rm_list), GFP_ATOMIC);
		if (unlikely(ssd_list_ptr == NULL)) {
			pr_err("Cannot allocate memory for ssd_rm_list");
			return -ENOMEM;
		}
		ssd_list_ptr->dmc = dmc;
		ssd_list_ptr->action = action;
		ssd_list_ptr->devt = dev->devt;
		ssd_list_ptr->note = notify;
		spin_lock_irqsave(&ssd_rm_list_lock, flags);
		list_add_tail(&ssd_list_ptr->list, &ssd_rm_list);
		ssd_rm_list_not_empty = 1;
		spin_unlock_irqrestore(&ssd_rm_list_lock, flags);
	}

	spin_lock_irqsave(&ssd_rm_list_lock, flags);
	if (ssd_rm_list_not_empty) {
		spin_unlock_irqrestore(&ssd_rm_list_lock, flags);
		schedule_work(&_kcached_wq);
	} else
		spin_unlock_irqrestore(&ssd_rm_list_lock, flags);

	return 0;
}

/*
 * Initiate a cache target.
 */
static int __init eio_init(void)
{
	int r;
	extern struct bus_type scsi_bus_type;

	if (sizeof(sector_t) != 8 || sizeof(index_t) != 8) {
		pr_err("init: EnhanceIO runs only in 64-bit architectures");
		return -EPERM;
	}

	eio_ttc_init();
	r = eio_create_misc_device();
	if (r)
		return r;

	r = eio_jobs_init();
	if (r) {
		(void)eio_delete_misc_device();
		return r;
	}
	atomic_set(&nr_cache_jobs, 0);
	INIT_WORK(&_kcached_wq, eio_do_work);

	eio_module_procfs_init();
	eio_control = kmalloc(sizeof *eio_control, GFP_KERNEL);
	if (eio_control == NULL) {
		pr_err("init: Cannot allocate memory for eio_control");
		(void)eio_delete_misc_device();
		return -ENOMEM;
	}
	eio_control->synch_flags = 0;

	register_reboot_notifier(&eio_reboot_notifier);
	r = bus_register_notifier(&scsi_bus_type, &eio_ssd_rm_notifier);
	if (r) {
		pr_err("init: bus register notifier failed %d", r);
		(void)eio_delete_misc_device();
	}
	return r;
}

/*
 * Destroy a cache target.
 */
static void eio_exit(void)
{
	int r;
	extern struct bus_type scsi_bus_type;

	unregister_reboot_notifier(&eio_reboot_notifier);
	r = bus_unregister_notifier(&scsi_bus_type, &eio_ssd_rm_notifier);
	if (r)
		pr_err("exit: Bus unregister notifier failed %d", r);

	eio_jobs_exit();
	eio_module_procfs_exit();
	if (eio_control) {
		eio_control->synch_flags = 0;
		kfree(eio_control);
		eio_control = NULL;
	}
	(void)eio_delete_misc_device();
}

/*
 * eio_get_device_size
 */
sector_t eio_get_device_size(struct eio_bdev *dev)
{

	return dev->bdev->bd_inode->i_size;
}

/*
 * To get starting sector of the device
 */
sector_t eio_get_device_start_sect(struct eio_bdev *dev)
{

	if (dev == NULL || dev->bdev == NULL || dev->bdev->bd_part == NULL)
		return 0;

	return dev->bdev->bd_part->start_sect;
}

module_init(eio_init);
module_exit(eio_exit);

MODULE_DESCRIPTION(DM_NAME "STEC EnhanceIO target");
MODULE_AUTHOR("STEC, Inc. based on code by Facebook");

MODULE_LICENSE("GPL");
