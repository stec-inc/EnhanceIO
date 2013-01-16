/*
 *  os.h
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

/*
 * Since the use of "spin_lock_irq()" and "spin_unlock_irq()" is
 * risky in itself *and* the original code interchangably uses these
 * functions with the safer varianats "spin_lock_irqsave()" and
 * "spin_unlock_irqrestore()" * variants, we force using the safer
 * version for all cases.
 *
 * Note that because sometimes locking is done in one function and
 * unlocking in another function without passing "flags" from the
 * locking function to the unlocking function, we have to store "flags"
 * in "<lock-name>_flags" for the unlocking function.  For example,
 * consider the following call sequence:
 *
 *   eio_do_pending_noerror()	uses spin_lock_irqsave
 *     eio_inval_blocks()
 *       eio_inval_block_set()	uses spin_unlock_irq
 */
#define SPIN_LOCK_INIT			spin_lock_init
#define SPIN_LOCK_IRQSAVE(l, f)		spin_lock_irqsave(l, f)
#define SPIN_UNLOCK_IRQRESTORE(l, f)	spin_unlock_irqrestore(l, f)
#define SPIN_LOCK_IRQSAVE_FLAGS(l)	do { long unsigned int f; spin_lock_irqsave(l, f); *(l##_flags) = f; } while (0)
#define SPIN_UNLOCK_IRQRESTORE_FLAGS(l)	do { long unsigned int f = *(l##_flags); spin_unlock_irqrestore(l, f); } while (0)

#define EIO_DBN_SET(dmc, index, dbn)		ssdcache_dbn_set(dmc, index, dbn)
#define EIO_DBN_GET(dmc, index)			ssdcache_dbn_get(dmc, index)
#define EIO_CACHE_STATE_SET(dmc, index, state)	ssdcache_cache_state_set(dmc, index, state)
#define EIO_CACHE_STATE_GET(dmc, index)		ssdcache_cache_state_get(dmc, index)
#define EIO_CACHE_STATE_OFF(dmc, index, bitmask)	ssdcache_cache_state_off(dmc, index, bitmask)
#define EIO_CACHE_STATE_ON(dmc, index, bitmask)	ssdcache_cache_state_on(dmc, index, bitmask)

/* Bit offsets for wait_on_bit_lock() */
#define EIO_UPDATE_LIST		0
#define EIO_HANDLE_REBOOT	1

struct eio_control_s {
	volatile unsigned long synch_flags;
};

int eio_wait_schedule(void *unused);


#include "linux_os.h"

#include "eio_setlru.h"
#include "eio_policy.h"
#define	EIO_CACHE(dmc)		(EIO_MD8(dmc) ? (void *)dmc->cache_md8 : (void *)dmc->cache)


