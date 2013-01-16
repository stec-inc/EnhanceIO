/*
 *  linux_os.h
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
#include <linux/sort.h>		/* required for eio_subr.c */
#include <linux/kthread.h>
#include <linux/jiffies.h>

#include <scsi/scsi_device.h>	/* required for SSD failure handling */
/* resolve conflict with scsi/scsi_device.h */
#ifdef QUEUED
#undef QUEUED
#endif


#include <linux/vmalloc.h>	/* for sysinfo (mem) variables */
#include <linux/mm.h>

struct eio_event {
	struct task_struct *process;	/* handle of the sleeping process */
};

#include "eio.h"
#include "eio_ioctl.h"

#define GET_SYSTIME(ptime)		(*(ptime) = jiffies)
#define CONV_SYSTIME_TO_SECS(time)	((time) / HZ)	

#define MAX_ALLOWED_IO	(256 << 10)	/* Harish: TBD: May need modification */

/* resolve conflict with scsi/scsi_device.h */
#ifdef __KERNEL__
#ifdef VERIFY
#undef VERIFY
#endif
#define ENABLE_VERIFY
#ifdef ENABLE_VERIFY
/* Like ASSERT() but always compiled in */
#define VERIFY(x) do { \
	if (unlikely(!(x))) { \
		dump_stack(); \
		panic("VERIFY: assertion (%s) failed at %s (%d)\n", \
		      #x,  __FILE__ , __LINE__);		    \
	} \
} while(0)
#else /* ENABLE_VERIFY */
#define VERIFY(x) do { } while(0);
#endif /* ENABLE_VERIFY */

extern sector_t eio_get_device_size(struct eio_bdev *);
extern sector_t eio_get_device_start_sect(struct eio_bdev *);
#endif /* __KERNEL__ */

#define EIO_INIT_EVENT(ev)						\
			do {				  		\
				(ev)->process = NULL;	  		\
			} while (0)

//Assumes that the macro gets called under the same spinlock as in wait event
#define EIO_SET_EVENT_AND_UNLOCK(ev, sl, flags)					\
			do {	  						\
				struct task_struct	*p = NULL;		\
				if ((ev)->process) { 				\
					(p) = (ev)->process;			\
					(ev)->process = NULL;			\
				}						\
				spin_unlock_irqrestore((sl), flags); 		\
				if (p) {					\
					(void)wake_up_process(p);		\
				}						\
			} while (0)

//Assumes that the spin lock sl is taken while calling this macro
#define EIO_WAIT_EVENT(ev, sl, flags)						\
			do {							\
				(ev)->process = current;			\
				set_current_state(TASK_INTERRUPTIBLE);		\
				spin_unlock_irqrestore((sl), flags); 		\
				(void)schedule_timeout(10 * HZ);		\
				spin_lock_irqsave((sl), flags);			\
				(ev)->process = NULL;				\
			} while (0)

#define EIO_CLEAR_EVENT(ev)							\
			do {							\
				(ev)->process = NULL;				\
			} while (0)
