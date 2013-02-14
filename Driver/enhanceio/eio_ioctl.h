/*
 *  eio_ioctl.h
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
 ****************************************************************************/

#ifndef EIO_IOCTL_H
#define EIO_IOCTL_H

#define EIO_DEVPATH     "/dev/eiodev"
#define MISC_DEVICE     "eiodev"

#define CACHE_NAME_LEN          31
#define CACHE_NAME_SZ           CACHE_NAME_LEN + 1

#define NAME_LEN                127
#define NAME_SZ                 NAME_LEN + 1

#define EIO_IOC         ('E' << 8)

enum eio_ioc {
	EIO_IOC_CREATE = EIO_IOC,
	EIO_IOC_DELETE,
	EIO_IOC_ENABLE,
	EIO_IOC_DISABLE,
	EIO_IOC_EDIT,
	EIO_IOC_NCACHES,
	EIO_IOC_CACHE_LIST,
	EIO_IOC_SSD_ADD,
	EIO_IOC_SSD_REMOVE,
	EIO_IOC_SRC_ADD,
	EIO_IOC_SRC_REMOVE,
	EIO_IOC_NOTIFY_REBOOT,
	EIO_IOC_SET_WARM_BOOT,
	EIO_IOC_UNUSED
};

struct cache_rec_short {
	char cr_name[CACHE_NAME_SZ];
	char cr_src_devname[NAME_SZ];
	char cr_ssd_devname[NAME_SZ];
	char cr_ssd_uuid[NAME_SZ];
	uint64_t cr_src_dev_size;
	uint64_t cr_ssd_dev_size;
	uint32_t cr_src_sector_size;
	uint32_t cr_ssd_sector_size;
	uint32_t cr_flags;      /* CACHE_FLAGS_INV* etc. */
	char cr_policy;
	char cr_mode;
	char cr_persistence;
	char cr_cold_boot;
	uint64_t cr_blksize;
	uint64_t cr_assoc;
};

struct cache_list {
	uint64_t ncaches;
	struct cache_rec_short *cachelist;
};

#ifdef __KERNEL__
long eio_ioctl(struct file *filp, unsigned cmd, unsigned long arg);
long eio_compact_ioctl(struct file *filp, unsigned cmd, unsigned long arg);
#endif                          /* __KERNEL__ */

#endif                          /* !EIO_IOCTL_H */
