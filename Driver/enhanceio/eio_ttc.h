/*
 * Copyright (C) 2012 STEC, Inc. All rights not specifically granted
 * under a license included herein are reserved.
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

#ifndef EIO_TTC_H
#define EIO_TTC_H

#ifdef __KERNEL__
#include <linux/device-mapper.h>
#define curthread       get_current()
#else
#include <stdint.h>
#endif                          /* __KERNEL__ */

static inline bool bio_rw_flagged(struct bio *bio, int flag)
{
	return (bio->bi_rw & flag) != 0;
}

/*
 * Whether the cached (source) device is a partition or a whole device.
 * dmc->dev_info stores this info.
 */
enum eio_io_mem_type {
	EIO_BVECS,              /* bio vectors */
	EIO_PAGES,              /* array of pages */
};

struct eio_io_request {
	enum eio_io_mem_type mtype;

	union {
		struct bio_vec *pages;
		struct page **plist;
	} dptr;

	unsigned num_bvecs;
	eio_notify_fn notify;
	void *context;
	unsigned hddio;
};

struct eio_context {
	atomic_t count;
	int error;
	struct completion *event;
	eio_notify_fn callback;
	void *context;
};

int eio_do_io(struct cache_c *dmc, struct eio_io_region *where, int rw,
	      struct eio_io_request *io_req);

enum eio_device {
	EIO_HDD_DEVICE = 1,
	EIO_SSD_DEVICE,
};

enum eio_dev_info {
	EIO_DEV_PARTITION = 1,
	EIO_DEV_WHOLE_DISK
};

enum eio_cache_state {
	DMC_TTC_INITIALIZING = 1,
	DMC_TTC_READY,
	DMC_TTC_IO_FREEZE,
	DMC_TTC_UNINITIALIZING,
	DMC_TTC_UNINITIALIZED
};

#ifdef __KERNEL__

#define EIO_HASHTBL_SIZE        1024

/*
 * In case of i/o errors while eio_clean_all, retry for
 * finish_nrdirty_retry count.
 */
#define FINISH_NRDIRTY_RETRY_COUNT      2

#define EIO_HASH_BDEV(dev)	\
	((MAJOR(dev) * EIO_MAGIC + MINOR(dev)) % EIO_HASHTBL_SIZE)

/*
 * Reboot status flags.
 */

#define EIO_REBOOT_HANDLING_INPROG      0x01
#define EIO_REBOOT_HANDLING_DONE        0x02

/*
 * kernel function prototypes.
 */

extern int eio_create_misc_device(void);
extern int eio_delete_misc_device(void);

extern int eio_ttc_get_device(const char *, fmode_t, struct eio_bdev **);
extern void eio_ttc_put_device(struct eio_bdev **);

extern struct cache_c *eio_cache_lookup(char *);
extern int eio_ttc_activate(struct cache_c *);
extern int eio_ttc_deactivate(struct cache_c *, int);
extern void eio_ttc_init(void);

extern int eio_cache_create(struct cache_rec_short *);
extern int eio_cache_delete(char *, int);
extern uint64_t eio_get_cache_count(void);
extern int eio_get_cache_list(unsigned long *);

extern int eio_handle_ssd_message(char *cache_name, char *ssd_name,
				  enum dev_notifier note);

int eio_do_preliminary_checks(struct cache_c *);

extern int eio_allocate_wb_resources(struct cache_c *);
extern void eio_free_wb_resources(struct cache_c *);

extern int eio_cache_edit(char *, u_int32_t, u_int32_t);

extern void eio_stop_async_tasks(struct cache_c *dmc);
extern int eio_start_clean_thread(struct cache_c *dmc);

extern int eio_policy_init(struct cache_c *);
extern void eio_policy_free(struct cache_c *);
extern int eio_alloc_wb_pages(struct page **pages, int max);
extern void eio_free_wb_pages(struct page **pages, int allocated);
extern int eio_alloc_wb_bvecs(struct bio_vec *bvec, int max, int blksize);
extern void eio_free_wb_bvecs(struct bio_vec *bvec, int allocated, int blksize);
extern struct bio_vec *eio_alloc_pages(u_int32_t max_pages, int *page_count);
extern int eio_md_store(struct cache_c *);
extern int eio_reboot_handling(void);
extern void eio_process_zero_size_bio(struct cache_c *dmc, struct bio *origbio);

#endif                          /* __KERNEL__ */

#endif                          /* EIO_TTC_H */
