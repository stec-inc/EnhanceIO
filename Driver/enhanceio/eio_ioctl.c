/*
 *  eio_ioctl.c
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

#include "os.h"
#include "eio_ttc.h"

long
eio_ioctl(struct file *filp, unsigned cmd, unsigned long arg)
{
	int			error = 0;
	cache_rec_short_t	*cache;
	uint64_t		ncaches;
	dev_notifier_t 		note;
	int			do_delete = 0;

	switch(cmd) {
		case EIO_IOC_CREATE:
		case EIO_IOC_ENABLE:

			cache = vmalloc(sizeof (cache_rec_short_t));
			if (!cache) {
				return -ENOMEM;
			}
			if (copy_from_user(cache, (cache_rec_short_t *)arg,
					   sizeof (cache_rec_short_t))) {
				vfree(cache);
				return -EFAULT;
			}
			error = eio_cache_create(cache);
			vfree(cache);
			break;

		case EIO_IOC_DELETE:

			do_delete = 1;

		case EIO_IOC_DISABLE:

			cache = vmalloc(sizeof (cache_rec_short_t));
			if (!cache) {
				return -ENOMEM;
			}

			if (copy_from_user(cache, (cache_rec_short_t *)arg,
					   sizeof (cache_rec_short_t))) {
				vfree(cache);
				return -EFAULT;
			}
			error = eio_cache_delete(cache->cr_name, do_delete);
			vfree(cache);
			break;

		case EIO_IOC_EDIT:

			cache = vmalloc(sizeof (cache_rec_short_t));
			if (!cache) {
				return -ENOMEM;
			}

			if (copy_from_user(cache, (cache_rec_short_t *)arg,
					   sizeof (cache_rec_short_t))) {
				vfree(cache);
				return -EFAULT;
			}
			error = eio_cache_edit(cache->cr_name,
						(u_int32_t)cache->cr_mode,
						(u_int32_t)cache->cr_policy);
			vfree(cache);
			break;

		case EIO_IOC_NCACHES:
			ncaches = eio_get_cache_count();
			if (copy_to_user((uint64_t *)arg, &ncaches,
					 sizeof (uint64_t))) {
				return -EFAULT;
			}
			break;

		case EIO_IOC_CACHE_LIST:
			error = eio_get_cache_list((unsigned long *)arg);
			break;

		case EIO_IOC_SET_WARM_BOOT:
			eio_set_warm_boot();
			break;

		case EIO_IOC_SSD_ADD:
			cache = vmalloc(sizeof (cache_rec_short_t));
			if (!cache)
				return -ENOMEM;

			if (copy_from_user(cache, (cache_rec_short_t *)arg,
					   sizeof (cache_rec_short_t))) {
				vfree(cache);
				return -EFAULT;
			}
			note = NOTIFY_SSD_ADD;
			error = eio_handle_ssd_message(cache->cr_name, cache->cr_ssd_devname, note);
			vfree(cache);

			break;

		case EIO_IOC_SSD_REMOVE:
			cache = vmalloc(sizeof (cache_rec_short_t));
			if (!cache)
				return -ENOMEM;

			if (copy_from_user(cache, (cache_rec_short_t *)arg,
					   sizeof (cache_rec_short_t))) {
				vfree(cache);
				return -EFAULT;
			}
			note = NOTIFY_SSD_REMOVED;
			error = eio_handle_ssd_message(cache->cr_name, cache->cr_ssd_devname, note);
			vfree(cache);
			break;

		case EIO_IOC_SRC_ADD:
			printk("Hello EIO_IOC_SRC_ADD called\n");
			break;

		case EIO_IOC_NOTIFY_REBOOT:
			eio_reboot_handling();
			break;

		default:
			error = EINVAL;
	}
	return error;
}

long
eio_compact_ioctl(struct file *filp, unsigned cmd, unsigned long arg)
{
	return eio_ioctl(filp, cmd, arg);
}

