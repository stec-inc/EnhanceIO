/*
 *  eio.h
 *
 *  Copyright (C) 2012 STEC, Inc. All rights not specifically granted
 *   under a license included herein are reserved
 *  Saied Kazemi <skazemi@stec-inc.com>
 *   Added EnhanceIO-specific code.
 *  Siddharth Choudhuri <schoudhuri@stec-inc.com>
 *   Common data structures and definitions between Windows and Linux.
 *  Amit Kale <akale@stec-inc.com>
 *   Restructured much of the io code to split bio within map function instead
 *   of letting dm do it.
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

#include <asm/atomic.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/hash.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/pagemap.h>
#include <linux/random.h>
#include <linux/hardirq.h>
#include <linux/sysctl.h>
#include <linux/version.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/device-mapper.h>
#include <linux/dm-kcopyd.h>
#include <linux/sort.h>         /* required for eio_subr.c */
#include <linux/kthread.h>
#include <linux/jiffies.h>
#include <linux/vmalloc.h>      /* for sysinfo (mem) variables */
#include <linux/mm.h>
#include <scsi/scsi_device.h>   /* required for SSD failure handling */
/* resolve conflict with scsi/scsi_device.h */
#ifdef QUEUED
#undef QUEUED
#endif

#if defined(__KERNEL__) && !defined(CONFIG_PROC_FS)
#error "EnhanceIO requires CONFIG_PROC_FS"
#endif                          /* __KERNEL__ && !CONFIG_PROC_FS */

#ifndef EIO_INC_H
#define EIO_INC_H

/* Bit offsets for wait_on_bit_lock() */
#define EIO_UPDATE_LIST         0
#define EIO_HANDLE_REBOOT       1


static __inline__ uint32_t
EIO_DIV(uint64_t dividend_64, uint32_t divisor_32)
{
	uint64_t result;
	result = dividend_64;
	do_div(result, divisor_32);
	return result;
}


static __inline__ uint32_t
EIO_REM(uint64_t dividend_64, uint32_t divisor_32)
{
	uint64_t temp;
	temp = dividend_64;
	return do_div(temp, divisor_32);
}


struct eio_control_s {
	unsigned long synch_flags;
};

int eio_wait_schedule(void *unused);

struct eio_event {
	struct task_struct *process;    /* handle of the sleeping process */
};

typedef long int index_t;

extern int eio_reboot_notified;
extern mempool_t *_io_pool;
extern struct eio_control_s *eio_control;
extern struct work_struct _kcached_wq;
extern int eio_force_warm_boot;
extern atomic_t nr_cache_jobs;
extern mempool_t *_job_pool;

/*
 * This file has three sections as follows:
 *
 *      Section 1: User space only
 *      Section 2: User space and kernel
 *      Section 3: Kernel only
 *
 * Each section may contain its own subsections.
 */

/*
 * Begin Section 1: User space only.
 */

/*
 * End Section 1: User space only.
 */

/*
 * Begin Section 2: User space and kernel.
 */

/* States of a cache block */
#define INVALID                 0x0001
#define VALID                   0x0002  /* Valid */
#define DISKREADINPROG          0x0004  /* Read from disk in progress */
#define DISKWRITEINPROG         0x0008  /* Write to disk in progress */
#define CACHEREADINPROG         0x0010  /* Read from cache in progress */
#define CACHEWRITEINPROG        0x0020  /* Write to cache in progress */
#define DIRTY                   0x0040  /* Dirty, needs writeback to disk */
#define QUEUED                  0x0080  /* Other requests are queued for this block */

#define BLOCK_IO_INPROG (DISKREADINPROG | DISKWRITEINPROG | \
			 CACHEREADINPROG | CACHEWRITEINPROG)
#define DIRTY_INPROG    (VALID | DIRTY | CACHEWRITEINPROG)      /* block being dirtied */
#define CLEAN_INPROG    (VALID | DIRTY | DISKWRITEINPROG)       /* ongoing clean */
#define ALREADY_DIRTY   (VALID | DIRTY)                         /* block which is dirty to begin with for an I/O */

/*
 * This is a special state used only in the following scenario as
 * part of device (SSD) failure handling:
 *
 * ------| dev fail |------| dev resume |------------
 *   ...-<--- Tf --><- Td -><---- Tr ---><-- Tn ---...
 * |---- Normal ----|-- Degraded -------|-- Normal ---|
 *
 * Tf: Time during device failure.
 * Td: Time after failure when the cache is in degraded mode.
 * Tr: Time when the SSD comes back online.
 *
 * When a failed SSD is added back again, it should be treated
 * as a cold SSD.
 *
 * If Td is very small, then there can be IOs that were initiated
 * before or during Tf, and did not finish until the end of Tr.  From
 * the IO's viewpoint, the SSD was there when the IO was initiated
 * and it was there when the IO was finished.  These IOs need special
 * handling as described below.
 *
 * To add the SSD as a cold cache device, we initialize all blocks
 * to INVALID, execept for the ones that had IOs in progress before
 * or during Tf.  We mark such blocks as both VALID and INVALID.
 * These blocks will be marked INVALID when finished.
 */
#define NO_SSD_IO_INPROG        (VALID | INVALID)

/*
 * On Flash (cache metadata) Structures
 */
#define CACHE_MD_STATE_DIRTY            0x55daddee
#define CACHE_MD_STATE_CLEAN            0xacceded1
#define CACHE_MD_STATE_FASTCLEAN        0xcafebabf
#define CACHE_MD_STATE_UNSTABLE         0xdeaddeee

/* Do we have a read cache or a read-write cache */
#define CACHE_MODE_WB           1
#define CACHE_MODE_RO           2
#define CACHE_MODE_WT           3
#define CACHE_MODE_FIRST        CACHE_MODE_WB
#define CACHE_MODE_LAST         CACHE_MODE_WT
#define CACHE_MODE_DEFAULT      CACHE_MODE_WT

#define DEV_PATHLEN             128
#define EIO_SUPERBLOCK_SIZE     4096

#define EIO_CLEAN_ABORT         0x00000000
#define EIO_CLEAN_START         0x00000001
#define EIO_CLEAN_KEEP          0x00000002

/* EIO magic number */
#define EIO_MAGIC               0xE10CAC6E
#define EIO_BAD_MAGIC           0xBADCAC6E

/* EIO version */
#define EIO_SB_VERSION          3       /* kernel superblock version */
#define EIO_SB_MAGIC_VERSION    3       /* version in which magic number was introduced */

union eio_superblock {
	struct superblock_fields {
		__le64 size;                  /* Cache size */
		__le32 block_size;           /* Cache block size */
		__le32 assoc;                /* Cache associativity */
		__le32 cache_sb_state;       /* Clean shutdown ? */
		char cache_devname[DEV_PATHLEN];
		__le64 cache_devsize;
		char disk_devname[DEV_PATHLEN];
		__le64 disk_devsize;
		__le32 cache_version;
		char cache_name[DEV_PATHLEN];
		__le32 mode;
		__le32 repl_policy;
		__le32 cache_flags;
		__le32 magic;                
		__le32 cold_boot;            /* cache to be started as cold after boot */
		char ssd_uuid[DEV_PATHLEN];
		__le64 cache_md_start_sect;   /* cache metadata start (8K aligned) */
		__le64 cache_data_start_sect; /* cache data start (8K aligned) */
		__le32 dirty_high_threshold;
		__le32 dirty_low_threshold;
		__le32 dirty_set_high_threshold;
		__le32 dirty_set_low_threshold;
		__le32 time_based_clean_interval;
		__le32 autoclean_threshold;
	} sbf;
	u_int8_t padding[EIO_SUPERBLOCK_SIZE];
};

/*
 * For EnhanceIO, we move the superblock from sector 0 to 128
 * and give it a full 4K.  Also, in addition to the single
 * "red-zone" buffer that separates metadata sectors from the
 * data sectors, we allocate extra sectors so that we can
 * align the data sectors on a 4K boundary.
 *
 *    64K    4K  variable variable  8K variable  variable
 * +--------+--+--------+---------+---+--------+---------+
 * | unused |SB| align1 |metadata | Z | align2 | data... |
 * +--------+--+--------+---------+---+--------+---------+
 * <------------- dmc->md_sectors ------------>
 */
#define EIO_UNUSED_SECTORS              128
#define EIO_SUPERBLOCK_SECTORS          8
#define EIO_REDZONE_SECTORS             16
#define EIO_START                       0

#define EIO_ALIGN1_SECTORS(index)               ((index % 16) ? (24 - (index % 16)) : 8)
#define EIO_ALIGN2_SECTORS(index)               ((index % 16) ? (16 - (index % 16)) : 0)
#define EIO_SUPERBLOCK_START                    (EIO_START + EIO_UNUSED_SECTORS)
#define EIO_METADATA_START(hd_start_sect)       (EIO_SUPERBLOCK_START +	\
						 EIO_SUPERBLOCK_SECTORS + \
						 EIO_ALIGN1_SECTORS(hd_start_sect))

#define EIO_EXTRA_SECTORS(start_sect, md_sects) (EIO_METADATA_START(start_sect) + \
						 EIO_REDZONE_SECTORS + \
						 EIO_ALIGN2_SECTORS(md_sects))

/*
 * We do metadata updates only when a block trasitions from DIRTY -> CLEAN
 * or from CLEAN -> DIRTY. Consequently, on an unclean shutdown, we only
 * pick up blocks that are marked (DIRTY | CLEAN), we clean these and stick
 * them in the cache.
 * On a clean shutdown, we will sync the state for every block, and we will
 * load every block back into cache on a restart.
 */
struct flash_cacheblock {
	sector_t dbn;           /* Sector number of the cached block */
	u_int32_t cache_state;
};

/* blksize in terms of no. of sectors */
#define BLKSIZE_2K      4
#define BLKSIZE_4K      8
#define BLKSIZE_8K      16

/*
 * Give me number of pages to allocated for the
 * iosize x specified in terms of bytes.
 */
#define IO_PAGE_COUNT(x)        (((x) + (PAGE_SIZE - 1)) / PAGE_SIZE)

/*
 * Macro that calculates number of biovecs to be
 * allocated depending on the iosize and cache
 * block size.
 */
#define IO_BVEC_COUNT(x, blksize) ({		\
					   int count = IO_PAGE_COUNT(x);	   \
					   switch ((blksize)) {			    \
					   case BLKSIZE_2K:			   \
						   count = count * 2;		   \
						   break;			   \
					   case BLKSIZE_4K:			   \
					   case BLKSIZE_8K:			   \
						   break;			   \
					   }					   \
					   count;				   \
				   })

#define MD_MAX_NR_PAGES                         16
#define MD_BLOCKS_PER_PAGE                      ((PAGE_SIZE) / sizeof(struct flash_cacheblock))
#define INDEX_TO_MD_PAGE(INDEX)                 ((INDEX) / MD_BLOCKS_PER_PAGE)
#define INDEX_TO_MD_PAGE_OFFSET(INDEX)          ((INDEX) % MD_BLOCKS_PER_PAGE)

#define MD_BLOCKS_PER_SECTOR                    (512 / (sizeof(struct flash_cacheblock)))
#define INDEX_TO_MD_SECTOR(INDEX)               (EIO_DIV((INDEX), MD_BLOCKS_PER_SECTOR))
#define INDEX_TO_MD_SECTOR_OFFSET(INDEX)        (EIO_REM((INDEX), MD_BLOCKS_PER_SECTOR))
#define MD_BLOCKS_PER_CBLOCK(dmc)               (MD_BLOCKS_PER_SECTOR * (dmc)->block_size)

#define METADATA_IO_BLOCKSIZE                   (256 * 1024)
#define METADATA_IO_BLOCKSIZE_SECT              (METADATA_IO_BLOCKSIZE / 512)
#define SECTORS_PER_PAGE                        ((PAGE_SIZE) / 512)

/*
 * Cache persistence.
 */
#define CACHE_RELOAD            1
#define CACHE_CREATE            2
#define CACHE_FORCECREATE       3

/*
 * Cache replacement policy.
 */
#define CACHE_REPL_FIFO         1
#define CACHE_REPL_LRU          2
#define CACHE_REPL_RANDOM       3
#define CACHE_REPL_FIRST        CACHE_REPL_FIFO
#define CACHE_REPL_LAST         CACHE_REPL_RANDOM
#define CACHE_REPL_DEFAULT      CACHE_REPL_FIFO

/*
 * Default cache parameters.
 */
#define DEFAULT_CACHE_ASSOC     512
#define DEFAULT_CACHE_BLKSIZE   8       /* 4 KB */

/*
 * Valid commands that can be written to "control".
 * NOTE: Update CACHE_CONTROL_FLAG_MAX value whenever a new control flag is added
 */
#define CACHE_CONTROL_FLAG_MAX  7
#define CACHE_VERBOSE_OFF       0
#define CACHE_VERBOSE_ON        1
#define CACHE_WRITEBACK_ON      2       /* register write back variables */
#define CACHE_WRITEBACK_OFF     3
#define CACHE_INVALIDATE_ON     4       /* register invalidate variables */
#define CACHE_INVALIDATE_OFF    5
#define CACHE_FAST_REMOVE_ON    6       /* do not write MD when destroying cache */
#define CACHE_FAST_REMOVE_OFF   7

/*
 * Bit definitions in "cache_flags". These are exported in Linux as
 * hex in the "flags" output line of /proc/enhanceio/<cache_name>/config.
 */

#define CACHE_FLAGS_VERBOSE             (1 << 0)
#define CACHE_FLAGS_INVALIDATE          (1 << 1)
#define CACHE_FLAGS_FAST_REMOVE         (1 << 2)
#define CACHE_FLAGS_DEGRADED            (1 << 3)
#define CACHE_FLAGS_SSD_ADD_INPROG      (1 << 4)
#define CACHE_FLAGS_MD8                 (1 << 5)        /* using 8-byte metadata (instead of 4-byte md) */
#define CACHE_FLAGS_FAILED              (1 << 6)
#define CACHE_FLAGS_STALE               (1 << 7)
#define CACHE_FLAGS_SHUTDOWN_INPROG     (1 << 8)
#define CACHE_FLAGS_MOD_INPROG          (1 << 9)        /* cache modification such as edit/delete in progress */
#define CACHE_FLAGS_DELETED             (1 << 10)
#define CACHE_FLAGS_INCORE_ONLY         (CACHE_FLAGS_DEGRADED |		\
					 CACHE_FLAGS_SSD_ADD_INPROG |	\
					 CACHE_FLAGS_FAILED |		\
					 CACHE_FLAGS_SHUTDOWN_INPROG |	\
					 CACHE_FLAGS_MOD_INPROG |	\
					 CACHE_FLAGS_STALE |		\
					 CACHE_FLAGS_DELETED)   /* need a proper definition */

/* flags that govern cold/warm enable after reboot */
#define BOOT_FLAG_COLD_ENABLE           (1 << 0)        /* enable the cache as cold */
#define BOOT_FLAG_FORCE_WARM            (1 << 1)        /* override the cold enable flag */

enum dev_notifier {
	NOTIFY_INITIALIZER,
	NOTIFY_SSD_ADD,
	NOTIFY_SSD_REMOVED,
	NOTIFY_SRC_REMOVED
};

/*
 * End Section 2: User space and kernel.
 */

/*
 * Begin Section 3: Kernel only.
 */
#if defined(__KERNEL__)

/*
 * Subsection 3.1: Definitions.
 */

#define EIO_SB_VERSION          3       /* kernel superblock version */

/* kcached/pending job states */
#define READCACHE               1
#define WRITECACHE              2
#define READDISK                3
#define WRITEDISK               4
#define READFILL                5       /* Read Cache Miss Fill */
#define INVALIDATE              6

/* Cache persistence */
#define CACHE_RELOAD            1
#define CACHE_CREATE            2
#define CACHE_FORCECREATE       3

/* Sysctl defined */
#define MAX_CLEAN_IOS_SET       2
#define MAX_CLEAN_IOS_TOTAL     4

/*
 * TBD
 * Rethink on max, min, default values
 */
#define DIRTY_HIGH_THRESH_DEF           30
#define DIRTY_LOW_THRESH_DEF            10
#define DIRTY_SET_HIGH_THRESH_DEF       100
#define DIRTY_SET_LOW_THRESH_DEF        30

#define CLEAN_FACTOR(sectors)           ((sectors) >> 25)       /* in 16 GB multiples */
#define TIME_BASED_CLEAN_INTERVAL_DEF(dmc)      (uint32_t)(CLEAN_FACTOR((dmc)->cache_size) ? \
							   CLEAN_FACTOR((dmc)->cache_size) : 1)
#define TIME_BASED_CLEAN_INTERVAL_MAX   720     /* in minutes */

#define AUTOCLEAN_THRESH_DEF            128     /* Number of I/Os which puts a hold on time based cleaning */
#define AUTOCLEAN_THRESH_MAX            1024    /* Number of I/Os which puts a hold on time based cleaning */

/* Inject a 5s delay between cleaning blocks and metadata */
#define CLEAN_REMOVE_DELAY      5000

/*
 * Subsection 2: Data structures.
 */

typedef void (*eio_notify_fn)(int error, void *context);

/*
 * 4-byte metadata support.
 */

#define EIO_MAX_SECTOR                  (((u_int64_t)1) << 40)

struct md4 {
	u_int16_t bytes1_2;
	u_int8_t byte3;
	u_int8_t cache_state;
};

struct cacheblock {
	union {
		u_int32_t u_i_md4;
		struct md4 u_s_md4;
	} md4_u;
#ifdef DO_CHECKSUM
	u_int64_t checksum;
#endif                          /* DO_CHECKSUM */
};

#define md4_md                          md4_u.u_i_md4
#define md4_cache_state                 md4_u.u_s_md4.cache_state
#define EIO_MD4_DBN_BITS                (32 - 8)        /* 8 bits for state */
#define EIO_MD4_DBN_MASK                ((1 << EIO_MD4_DBN_BITS) - 1)
#define EIO_MD4_INVALID                 (INVALID << EIO_MD4_DBN_BITS)
#define EIO_MD4_CACHE_STATE(dmc, index) (dmc->cache[index].md4_cache_state)

/*
 * 8-byte metadata support.
 */

struct md8 {
	u_int32_t bytes1_4;
	u_int16_t bytes5_6;
	u_int8_t byte7;
	u_int8_t cache_state;
};

struct cacheblock_md8 {
	union {
		u_int64_t u_i_md8;
		struct md8 u_s_md8;
	} md8_u;
#ifdef DO_CHECKSUM
	u_int64_t checksum;
#endif                          /* DO_CHECKSUM */
};

#define md8_md                          md8_u.u_i_md8
#define md8_cache_state                 md8_u.u_s_md8.cache_state
#define EIO_MD8_DBN_BITS                (64 - 8)        /* 8 bits for state */
#define EIO_MD8_DBN_MASK                ((((u_int64_t)1) << EIO_MD8_DBN_BITS) - 1)
#define EIO_MD8_INVALID                 (((u_int64_t)INVALID) << EIO_MD8_DBN_BITS)
#define EIO_MD8_CACHE_STATE(dmc, index) ((dmc)->cache_md8[index].md8_cache_state)
#define EIO_MD8(dmc)                    CACHE_MD8_IS_SET(dmc)

/* Structure used for metadata update on-disk and in-core for writeback cache */
struct mdupdate_request {
	struct list_head list;          /* to build mdrequest chain */
	struct work_struct work;        /* work structure */
	struct cache_c *dmc;            /* cache pointer */
	index_t set;                    /* set index */
	unsigned md_size;               /* metadata size */
	unsigned mdbvec_count;          /* count of bvecs allocated. */
	struct bio_vec *mdblk_bvecs;    /* bvecs for updating md_blocks */
	atomic_t holdcount;             /* I/O hold count */
	struct eio_bio *pending_mdlist; /* ebios pending for md update */
	struct eio_bio *inprog_mdlist;  /* ebios processed for md update */
	int error;                      /* error during md update */
	struct mdupdate_request *next;  /* next mdreq in the mdreq list .TBD. Deprecate */
};

#define SETFLAG_CLEAN_INPROG    0x00000001      /* clean in progress on a set */
#define SETFLAG_CLEAN_WHOLE     0x00000002      /* clean the set fully */

/* Structure used for doing operations and storing cache set level info */
struct cache_set {
	struct list_head list;
	u_int32_t nr_dirty;             /* number of dirty blocks */
	spinlock_t cs_lock;             /* spin lock to protect struct fields */
	struct rw_semaphore rw_lock;    /* reader-writer lock used for clean */
	unsigned int flags;             /* misc cache set specific flags */
	struct mdupdate_request *mdreq; /* metadata update request pointer */
};

struct eio_errors {
	int disk_read_errors;
	int disk_write_errors;
	int ssd_read_errors;
	int ssd_write_errors;
	int memory_alloc_errors;
	int no_cache_dev;
	int no_source_dev;
};

/*
 * Stats. Note that everything should be "atomic64_t" as
 * code relies on it.
 */
#define SECTOR_STATS(statval, io_size)	\
	atomic64_add(to_sector(io_size), &statval);

struct eio_stats {
	atomic64_t reads;               /* Number of reads */
	atomic64_t writes;              /* Number of writes */
	atomic64_t read_hits;           /* Number of cache hits */
	atomic64_t write_hits;          /* Number of write hits (includes dirty write hits) */
	atomic64_t dirty_write_hits;    /* Number of "dirty" write hits */
	atomic64_t cached_blocks;       /* Number of cached blocks */
	atomic64_t rd_replace;          /* Number of read cache replacements. TBD modify def doc */
	atomic64_t wr_replace;          /* Number of write cache replacements. TBD modify def doc */
	atomic64_t noroom;              /* No room in set */
	atomic64_t cleanings;           /* blocks cleaned TBD modify def doc */
	atomic64_t md_write_dirty;      /* Metadata sector writes dirtying block */
	atomic64_t md_write_clean;      /* Metadata sector writes cleaning block */
	atomic64_t md_ssd_writes;       /* How many md ssd writes did we do ? */
	atomic64_t uncached_reads;
	atomic64_t uncached_writes;
	atomic64_t uncached_map_size;
	atomic64_t uncached_map_uncacheable;
	atomic64_t disk_reads;
	atomic64_t disk_writes;
	atomic64_t ssd_reads;
	atomic64_t ssd_writes;
	atomic64_t ssd_readfills;
	atomic64_t ssd_readfill_unplugs;
	atomic64_t readdisk;
	atomic64_t writedisk;
	atomic64_t readcache;
	atomic64_t readfill;
	atomic64_t writecache;
	atomic64_t wrtime_ms;   /* total write time in ms */
	atomic64_t rdtime_ms;   /* total read time in ms */
	atomic64_t readcount;   /* total reads received so far */
	atomic64_t writecount;  /* total writes received so far */
};

#define PENDING_JOB_HASH_SIZE                   32
#define PENDING_JOB_HASH(index)                 ((index) % PENDING_JOB_HASH_SIZE)
#define SIZE_HIST                               (128 + 1)
#define EIO_COPY_PAGES                          1024    /* Number of pages for I/O */
#define MIN_JOBS                                1024
#define MIN_EIO_IO                              4096
#define MIN_DMC_BIO_PAIR                        8192

/* Structure representing a sequence of sets(first to last set index) */
struct set_seq {
	index_t first_set;
	index_t last_set;
	struct set_seq *next;
};

/* EIO system control variables(tunables) */
/*
 * Adding synchonization is not worth the benefits.
 */
struct eio_sysctl {
	uint32_t error_inject;
	int32_t fast_remove;
	int32_t zerostats;
	int32_t do_clean;
	uint32_t dirty_high_threshold;
	uint32_t dirty_low_threshold;
	uint32_t dirty_set_high_threshold;
	uint32_t dirty_set_low_threshold;
	uint32_t time_based_clean_interval;    /* time after which dirty sets should clean */
	int32_t autoclean_threshold;
	int32_t mem_limit_pct;
	int32_t control;
	u_int64_t invalidate;
};

/* forward declaration */
struct lru_ls;

/* Replacement for 'struct dm_dev' */
struct eio_bdev {
	struct block_device *bdev;
	fmode_t mode;
	char name[16];
};

/* Replacement for 'struct dm_io_region */
struct eio_io_region {
	struct block_device *bdev;
	sector_t sector;
	sector_t count;         /* If zero the region is ignored */
};

/*
 * Cache context
 */
struct cache_c {
	struct list_head cachelist;
	make_request_fn *origmfn;
	char dev_info;          /* partition or whole device */

	sector_t dev_start_sect;
	sector_t dev_end_sect;
	int cache_rdonly;               /* protected by ttc_write lock */
	struct eio_bdev *disk_dev;      /* Source device */
	struct eio_bdev *cache_dev;     /* Cache device */
	struct cacheblock *cache;       /* Hash table for cache blocks */
	struct cache_set *cache_sets;
	struct cache_c *next_cache;
	struct kcached_job *readfill_queue;
	struct work_struct readfill_wq;

	struct list_head cleanq;        /* queue of sets to awaiting clean */
	struct eio_event clean_event;   /* event to wait for, when cleanq is empty */
	spinlock_t clean_sl;            /* spinlock to protect cleanq etc */
	void *clean_thread;             /* OS specific thread object to handle cleanq */
	int clean_thread_running;       /* to indicate that clean thread is running */
	atomic64_t clean_pendings;      /* Number of sets pending to be cleaned */
	struct bio_vec *clean_dbvecs;   /* Data bvecs for clean set */
	struct page **clean_mdpages;    /* Metadata pages for clean set */
	int dbvec_count;
	int mdpage_count;
	int clean_excess_dirty;         /* Clean in progress to bring cache dirty blocks in limits */
	atomic_t clean_index;           /* set being cleaned, in case of force clean */

	u_int64_t md_start_sect;        /* Sector no. at which Metadata starts */
	u_int64_t md_sectors;           /* Numbers of metadata sectors, including header */
	u_int64_t disk_size;            /* Source size */
	u_int64_t size;                 /* Cache size */
	u_int32_t assoc;                /* Cache associativity */
	u_int32_t block_size;           /* Cache block size */
	u_int32_t block_shift;          /* Cache block size in bits */
	u_int32_t block_mask;           /* Cache block mask */
	u_int32_t consecutive_shift;    /* Consecutive blocks size in bits */
	u_int32_t persistence;          /* Create | Force create | Reload */
	u_int32_t mode;                 /* CACHE_MODE_{WB, RO, WT} */
	u_int32_t cold_boot;            /* Cache should be started as cold after boot */
	u_int32_t bio_nr_pages;         /* number of hardware sectors supported by SSD in terms of PAGE_SIZE */

	spinlock_t cache_spin_lock;
	long unsigned int cache_spin_lock_flags;        /* See comments above spin_lock_irqsave_FLAGS */
	atomic_t nr_jobs;                               /* Number of I/O jobs */

	u_int32_t cache_flags;
	u_int32_t sb_state;     /* Superblock state */
	u_int32_t sb_version;   /* Superblock version */

	int readfill_in_prog;
	struct eio_stats eio_stats;     /* Run time stats */
	struct eio_errors eio_errors;   /* Error stats */
	int max_clean_ios_set;          /* Max cleaning IOs per set */
	int max_clean_ios_total;        /* Total max cleaning IOs */
	int clean_inprog;
	atomic64_t nr_dirty;
	atomic64_t nr_ios;
	atomic64_t size_hist[SIZE_HIST];

	void *sysctl_handle_common;
	void *sysctl_handle_writeback;
	void *sysctl_handle_invalidate;

	struct eio_sysctl sysctl_pending;       /* sysctl values pending to become active */
	struct eio_sysctl sysctl_active;        /* sysctl currently active */

	char cache_devname[DEV_PATHLEN];
	char disk_devname[DEV_PATHLEN];
	char cache_name[DEV_PATHLEN];
	char cache_gendisk_name[DEV_PATHLEN];   /* Used for SSD failure checks */
	char cache_srcdisk_name[DEV_PATHLEN];   /* Used for SRC failure checks */
	char ssd_uuid[DEV_PATHLEN];

	struct cacheblock_md8 *cache_md8;
	sector_t cache_size;                            /* Cache size passed to ctr(), used by dmsetup info */
	sector_t cache_dev_start_sect;                  /* starting sector of cache device */
	u_int64_t index_zero;                           /* index of cache block with starting sector 0 */
	u_int32_t num_sets;                             /* number of cache sets */
	u_int32_t num_sets_bits;                        /* number of bits to encode "num_sets" */
	u_int64_t num_sets_mask;                        /* mask value for bits in "num_sets" */

	struct eio_policy *policy_ops;                  /* Cache block Replacement policy */
	u_int32_t req_policy;                           /* Policy requested by the user */
	u_int32_t random;                               /* Use for random replacement policy */
	void *sp_cache_blk;                             /* Per cache-block data structure */
	void *sp_cache_set;                             /* Per cache-set data structure */
	struct lru_ls *dirty_set_lru;                   /* lru for dirty sets : lru_list_t */
	spinlock_t dirty_set_lru_lock;                  /* spinlock for dirty set lru */
	struct delayed_work clean_aged_sets_work;       /* work item for clean_aged_sets */
	int is_clean_aged_sets_sched;                   /* to know whether clean aged sets is scheduled */
	struct workqueue_struct *mdupdate_q;            /* Workqueue to handle md updates */
	struct workqueue_struct *callback_q;            /* Workqueue to handle io callbacks */
};

#define EIO_CACHE_IOSIZE                0

#define EIO_ROUND_SECTOR(dmc, sector) (sector & (~(unsigned)(dmc->block_size - 1)))
#define EIO_ROUND_SET_SECTOR(dmc, sector) (sector & (~(unsigned)((dmc->block_size * dmc->assoc) - 1)))

/*
 * The bit definitions are exported to the user space and are in the very beginning of the file.
 */
#define CACHE_VERBOSE_IS_SET(dmc)               (((dmc)->cache_flags & CACHE_FLAGS_VERBOSE) ? 1 : 0)
#define CACHE_INVALIDATE_IS_SET(dmc)            (((dmc)->cache_flags & CACHE_FLAGS_INVALIDATE) ? 1 : 0)
#define CACHE_FAST_REMOVE_IS_SET(dmc)           (((dmc)->cache_flags & CACHE_FLAGS_FAST_REMOVE) ? 1 : 0)
#define CACHE_DEGRADED_IS_SET(dmc)              (((dmc)->cache_flags & CACHE_FLAGS_DEGRADED) ? 1 : 0)
#define CACHE_SSD_ADD_INPROG_IS_SET(dmc)        (((dmc)->cache_flags & CACHE_FLAGS_SSD_ADD_INPROG) ? 1 : 0)
#define CACHE_MD8_IS_SET(dmc)                   (((dmc)->cache_flags & CACHE_FLAGS_MD8) ? 1 : 0)
#define CACHE_FAILED_IS_SET(dmc)                (((dmc)->cache_flags & CACHE_FLAGS_FAILED) ? 1 : 0)
#define CACHE_STALE_IS_SET(dmc)                 (((dmc)->cache_flags & CACHE_FLAGS_STALE) ? 1 : 0)

/* Device failure handling.  */
#define CACHE_SRC_IS_ABSENT(dmc)                (((dmc)->eio_errors.no_source_dev == 1) ? 1 : 0)

#define AUTOCLEAN_THRESHOLD_CROSSED(dmc)	\
	((atomic64_read(&(dmc)->nr_ios) > (int64_t)(dmc)->sysctl_active.autoclean_threshold) ||	\
	 ((dmc)->sysctl_active.autoclean_threshold == 0))

#define DIRTY_CACHE_THRESHOLD_CROSSED(dmc)	\
	((atomic64_read(&(dmc)->nr_dirty) - atomic64_read(&(dmc)->clean_pendings)) >= \
	  (int64_t)((dmc)->sysctl_active.dirty_high_threshold * EIO_DIV((dmc)->size, 100)) && \
	 ((dmc)->sysctl_active.dirty_high_threshold > (dmc)->sysctl_active.dirty_low_threshold))

#define DIRTY_SET_THRESHOLD_CROSSED(dmc, set)	\
	(((dmc)->cache_sets[(set)].nr_dirty >= (u_int32_t)((dmc)->sysctl_active.dirty_set_high_threshold * (dmc)->assoc) / 100) && \
	 ((dmc)->sysctl_active.dirty_set_high_threshold > (dmc)->sysctl_active.dirty_set_low_threshold))

/*
 * Do not reverse the order of disk and cache! Code
 * relies on this ordering. (Eg: eio_dm_io_async_bvec()).
 */
struct job_io_regions {
	struct eio_io_region disk;      /* has to be the first member */
	struct eio_io_region cache;     /* has to be the second member */
};

#define EB_MAIN_IO 1
#define EB_SUBORDINATE_IO 2
#define EB_INVAL 4
#define GET_BIO_FLAGS(ebio)             ((ebio)->eb_bc->bc_bio->bi_rw)
#define VERIFY_BIO_FLAGS(ebio)          EIO_ASSERT((ebio) && (ebio)->eb_bc && (ebio)->eb_bc->bc_bio)

#define SET_BARRIER_FLAGS(rw_flags) (rw_flags |= (REQ_WRITE | REQ_FLUSH))

struct eio_bio {
	int eb_iotype;
	struct bio_container *eb_bc;
	unsigned eb_cacheset;
	sector_t eb_sector;             /*sector number*/
	unsigned eb_size;               /*size in bytes*/
	struct bio_vec *eb_bv;          /*bvec pointer*/
	unsigned eb_nbvec;              /*number of bio_vecs*/
	int eb_dir;                     /* io direction*/
	struct eio_bio *eb_next;        /*used for splitting reads*/
	index_t eb_index;               /*for read bios*/
	atomic_t eb_holdcount;          /* ebio hold count, currently used only for dirty block I/O */
	struct bio_vec eb_rbv[0];
};

enum eio_io_dir {
	EIO_IO_INVALID_DIR = 0,
	CACHED_WRITE,
	CACHED_READ,
	UNCACHED_WRITE,
	UNCACHED_READ,
	UNCACHED_READ_AND_READFILL
};

/* ASK
 * Container for all eio_bio corresponding to a given bio
 */
struct bio_container {
	spinlock_t bc_lock;                     /* lock protecting the bc fields */
	atomic_t bc_holdcount;                  /* number of ebios referencing bc */
	struct bio *bc_bio;                     /* bio for the bc */
	struct cache_c *bc_dmc;                 /* cache structure */
	struct eio_bio *bc_mdlist;              /* ebios waiting for md update */
	int bc_mdwait;                          /* count of ebios that will do md update */
	struct mdupdate_request *mdreqs;        /* mdrequest structures required for md update */
	struct set_seq *bc_setspan;             /* sets spanned by the bc(used only for wb) */
	struct set_seq bc_singlesspan;          /* used(by wb) if bc spans a single set sequence */
	enum eio_io_dir bc_dir;                 /* bc I/O direction */
	int bc_error;                           /* error encountered during processing bc */
	unsigned long bc_iotime;                /* maintains i/o time in jiffies */
	struct bio_container *bc_next;          /* next bc in the chain */
};

/* structure used as callback context during synchronous I/O */
struct sync_io_context {
	struct rw_semaphore sio_lock;
	unsigned long sio_error;
};

struct kcached_job {
	struct list_head list;
	struct work_struct work;
	struct cache_c *dmc;
	struct eio_bio *ebio;
	struct job_io_regions job_io_regions;
	index_t index;
	int action;
	int error;
	struct flash_cacheblock *md_sector;
	struct bio_vec md_io_bvec;
	struct kcached_job *next;
};

struct ssd_rm_list {
	struct cache_c *dmc;
	int action;
	dev_t devt;
	enum dev_notifier note;
	struct list_head list;
};

struct dbn_index_pair {
	sector_t dbn;
	index_t index;
};

/*
 * Subsection 3: Function prototypes and definitions.
 */

struct kcached_job *eio_alloc_cache_job(void);
void eio_free_cache_job(struct kcached_job *job);
struct kcached_job *pop(struct list_head *jobs);
void push(struct list_head *jobs, struct kcached_job *job);
void do_work(struct work_struct *unused);
void update_job_cacheregion(struct kcached_job *job, struct cache_c *dmc,
			    struct eio_bio *bio);
void push_io(struct kcached_job *job);
void push_md_io(struct kcached_job *job);
void push_md_complete(struct kcached_job *job);
void push_uncached_io_complete(struct kcached_job *job);
int eio_io_empty(void);
int eio_md_io_empty(void);
int eio_md_complete_empty(void);
void eio_md_write_done(struct kcached_job *job);
void eio_ssderror_diskread(struct kcached_job *job);
void eio_md_write(struct kcached_job *job);
void eio_md_write_kickoff(struct kcached_job *job);
void eio_do_readfill(struct work_struct *work);
void eio_comply_dirty_thresholds(struct cache_c *dmc, index_t set);
void eio_clean_all(struct cache_c *dmc);
void eio_clean_for_reboot(struct cache_c *dmc);
void eio_clean_aged_sets(struct work_struct *work);
void eio_comply_dirty_thresholds(struct cache_c *dmc, index_t set);
#ifndef SSDCACHE
void eio_reclaim_lru_movetail(struct cache_c *dmc, index_t index,
			      struct eio_policy *);
#endif                          /* !SSDCACHE */
int eio_io_sync_vm(struct cache_c *dmc, struct eio_io_region *where, int rw,
		   struct bio_vec *bvec, int nbvec);
int eio_io_sync_pages(struct cache_c *dmc, struct eio_io_region *where, int rw,
		      struct page **pages, int num_bvecs);
void eio_update_sync_progress(struct cache_c *dmc);
void eio_plug_cache_device(struct cache_c *dmc);
void eio_unplug_cache_device(struct cache_c *dmc);
void eio_plug_disk_device(struct cache_c *dmc);
void eio_unplug_disk_device(struct cache_c *dmc);
int dm_io_async_bvec(unsigned int num_regions, struct eio_io_region *where,
		     int rw, struct bio_vec *bvec, eio_notify_fn fn,
		     void *context);
void eio_put_cache_device(struct cache_c *dmc);
void eio_suspend_caching(struct cache_c *dmc, enum dev_notifier note);
void eio_resume_caching(struct cache_c *dmc, char *dev);
int eio_ctr_ssd_add(struct cache_c *dmc, char *dev);

/* procfs */
void eio_module_procfs_init(void);
void eio_module_procfs_exit(void);
void eio_procfs_ctr(struct cache_c *dmc);
void eio_procfs_dtr(struct cache_c *dmc);

int eio_sb_store(struct cache_c *dmc);

int eio_md_destroy(struct dm_target *tip, char *namep, char *srcp, char *cachep,
		   int force);

/* eio_conf.c */
extern int eio_ctr(struct dm_target *ti, unsigned int argc, char **argv);
extern void eio_dtr(struct dm_target *ti);
extern int eio_md_destroy(struct dm_target *tip, char *namep, char *srcp,
			  char *cachep, int force);
extern int eio_ctr_ssd_add(struct cache_c *dmc, char *dev);

/* thread related functions */
void *eio_create_thread(int (*func)(void *), void *context, char *name);
void eio_thread_exit(long exit_code);
void eio_wait_thread_exit(void *thrdptr, int *notifier);

/* eio_main.c */
extern int eio_map(struct cache_c *, struct request_queue *, struct bio *);
extern void eio_md_write_done(struct kcached_job *job);
extern void eio_ssderror_diskread(struct kcached_job *job);
extern void eio_md_write(struct kcached_job *job);
extern void eio_md_write_kickoff(struct kcached_job *job);
extern void eio_do_readfill(struct work_struct *work);
extern void eio_check_dirty_thresholds(struct cache_c *dmc, index_t set);
extern void eio_clean_all(struct cache_c *dmc);
extern int eio_clean_thread_proc(void *context);
extern void eio_touch_set_lru(struct cache_c *dmc, index_t set);
extern void eio_inval_range(struct cache_c *dmc, sector_t iosector,
			    unsigned iosize);
extern int eio_invalidate_sanity_check(struct cache_c *dmc, u_int64_t iosector,
				       u_int64_t *iosize);
/*
 * Invalidates all cached blocks without waiting for them to complete
 * Should be called with incoming IO suspended
 */
extern int eio_invalidate_cache(struct cache_c *dmc);

/* eio_mem.c */
extern int eio_mem_init(struct cache_c *dmc);
extern u_int32_t eio_hash_block(struct cache_c *dmc, sector_t dbn);
extern unsigned int eio_shrink_dbn(struct cache_c *dmc, sector_t dbn);
extern sector_t eio_expand_dbn(struct cache_c *dmc, u_int64_t index);
extern void eio_invalidate_md(struct cache_c *dmc, u_int64_t index);
extern void eio_md4_dbn_set(struct cache_c *dmc, u_int64_t index,
			    u_int32_t dbn_24);
extern void eio_md8_dbn_set(struct cache_c *dmc, u_int64_t index, sector_t dbn);

/* eio_procfs.c */
extern void eio_module_procfs_init(void);
extern void eio_module_procfs_exit(void);
extern void eio_procfs_ctr(struct cache_c *dmc);
extern void eio_procfs_dtr(struct cache_c *dmc);
extern int eio_version_query(size_t buf_sz, char *bufp);

/* eio_subr.c */
extern void eio_free_cache_job(struct kcached_job *job);
extern void eio_do_work(struct work_struct *unused);
extern struct kcached_job *eio_new_job(struct cache_c *dmc, struct eio_bio *bio,
				       index_t index);
extern void eio_push_ssdread_failures(struct kcached_job *job);
extern void eio_push_md_io(struct kcached_job *job);
extern void eio_push_md_complete(struct kcached_job *job);
extern void eio_push_uncached_io_complete(struct kcached_job *job);
extern int eio_io_empty(void);
extern int eio_io_sync_vm(struct cache_c *dmc, struct eio_io_region *where,
			  int rw, struct bio_vec *bvec, int nbvec);
extern void eio_unplug_cache_device(struct cache_c *dmc);
extern void eio_put_cache_device(struct cache_c *dmc);
extern void eio_suspend_caching(struct cache_c *dmc, enum dev_notifier note);
extern void eio_resume_caching(struct cache_c *dmc, char *dev);

static __inline__ void
EIO_DBN_SET(struct cache_c *dmc, u_int64_t index, sector_t dbn)
{
	if (EIO_MD8(dmc))
		eio_md8_dbn_set(dmc, index, dbn);
	else
		eio_md4_dbn_set(dmc, index, eio_shrink_dbn(dmc, dbn));
	if (dbn == 0)
		dmc->index_zero = index;
}

static __inline__ u_int64_t EIO_DBN_GET(struct cache_c *dmc, u_int64_t index)
{
	if (EIO_MD8(dmc))
		return dmc->cache_md8[index].md8_md & EIO_MD8_DBN_MASK;

	return eio_expand_dbn(dmc, index);
}

static __inline__ void
EIO_CACHE_STATE_SET(struct cache_c *dmc, u_int64_t index, u_int8_t cache_state)
{
	if (EIO_MD8(dmc))
		EIO_MD8_CACHE_STATE(dmc, index) = cache_state;
	else
		EIO_MD4_CACHE_STATE(dmc, index) = cache_state;
}

static __inline__ u_int8_t
EIO_CACHE_STATE_GET(struct cache_c *dmc, u_int64_t index)
{
	u_int8_t cache_state;

	if (EIO_MD8(dmc))
		cache_state = EIO_MD8_CACHE_STATE(dmc, index);
	else
		cache_state = EIO_MD4_CACHE_STATE(dmc, index);
	return cache_state;
}

static __inline__ void
EIO_CACHE_STATE_OFF(struct cache_c *dmc, index_t index, u_int8_t bitmask)
{
	u_int8_t cache_state = EIO_CACHE_STATE_GET(dmc, index);

	cache_state &= ~bitmask;
	EIO_CACHE_STATE_SET(dmc, index, cache_state);
}

static __inline__ void
EIO_CACHE_STATE_ON(struct cache_c *dmc, index_t index, u_int8_t bitmask)
{
	u_int8_t cache_state = EIO_CACHE_STATE_GET(dmc, index);

	cache_state |= bitmask;
	EIO_CACHE_STATE_SET(dmc, index, cache_state);
}

void eio_set_warm_boot(void);
#endif                          /* defined(__KERNEL__) */

#include "eio_ioctl.h"

/* resolve conflict with scsi/scsi_device.h */
#ifdef __KERNEL__

#ifdef EIO_ASSERT 
#undef EIO_ASSERT
#endif
/*Always compiled in*/
#define EIO_ASSERT(x)    BUG_ON(unlikely(!(x)))

extern sector_t eio_get_device_size(struct eio_bdev *);
extern sector_t eio_get_device_start_sect(struct eio_bdev *);
#endif                          /* __KERNEL__ */

#define EIO_INIT_EVENT(ev)						\
	do {						\
		(ev)->process = NULL;			\
	} while (0)

/*Assumes that the macro gets called under the same spinlock as in wait event*/
#define EIO_SET_EVENT_AND_UNLOCK(ev, sl, flags)					\
	do {							\
		struct task_struct      *p = NULL;		\
		if ((ev)->process) {				\
			(p) = (ev)->process;			\
			(ev)->process = NULL;			\
		}						\
		spin_unlock_irqrestore((sl), flags);		\
		if (p) {					\
			(void)wake_up_process(p);		\
		}						\
	} while (0)

/*Assumes that the spin lock sl is taken while calling this macro*/
#define EIO_WAIT_EVENT(ev, sl, flags)						\
	do {							\
		(ev)->process = current;			\
		set_current_state(TASK_INTERRUPTIBLE);		\
		spin_unlock_irqrestore((sl), flags);		\
		(void)schedule_timeout(10 * HZ);		\
		spin_lock_irqsave((sl), flags);			\
		(ev)->process = NULL;				\
	} while (0)

#define EIO_CLEAR_EVENT(ev)							\
	do {							\
		(ev)->process = NULL;				\
	} while (0)

#include "eio_setlru.h"
#include "eio_policy.h"
#define EIO_CACHE(dmc)          (EIO_MD8(dmc) ? (void *)dmc->cache_md8 : (void *)dmc->cache)

#endif                          /* !EIO_INC_H */
