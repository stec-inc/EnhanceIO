/*
 *  eio_mem.c
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

#include "eio.h"

#define SECTORS_PER_SET         (dmc->assoc * dmc->block_size)
#define SECTORS_PER_SET_SHIFT   (dmc->consecutive_shift + dmc->block_shift)
#define SECTORS_PER_SET_MASK    (SECTORS_PER_SET - 1)

#define EIO_DBN_TO_SET(dmc, dbn, set_number, wrapped)   do {		\
		u_int64_t value;						\
		u_int64_t mid_i;						\
		value = (dbn) >> SECTORS_PER_SET_SHIFT;				\
		mid_i = (value) & (dmc)->num_sets_mask;				\
		if (mid_i >= (dmc)->num_sets) {					\
			(wrapped) = 1;						\
			(set_number) = mid_i - (dmc)->num_sets;			\
		} else {							\
			(wrapped) = 0;						\
			(set_number) = mid_i;					\
		}								\
} while (0)

/*
 * eio_mem_init
 */
int eio_mem_init(struct cache_c *dmc)
{
	u_int32_t lsb_bits;
	u_int32_t msb_bits_24;  /* most significant bits in shrunk dbn */
	u_int64_t max_dbn;
	u_int64_t num_sets_64;

	/*
	 * Sanity check the number of sets.
	 */
	num_sets_64 = EIO_DIV(dmc->size, dmc->assoc);
	if (num_sets_64 > UINT_MAX) {
		pr_err("Number of cache sets (%lu) greater than maximum"
		       "allowed (%u)",
		       (long unsigned int)num_sets_64, UINT_MAX);
		return -1;
	}

	/*
	 * Find the number of bits required to encode the set number and
	 * its corresponding mask value.
	 */
	dmc->num_sets = (u_int32_t)num_sets_64;
	for (dmc->num_sets_bits = 0; (dmc->num_sets >> dmc->num_sets_bits) != 0;
	     dmc->num_sets_bits++) ;
	dmc->num_sets_mask = ULLONG_MAX >> (64 - dmc->num_sets_bits);

	/*
	 * If we don't have at least 16 bits to save,
	 * we can't use small metadata.
	 */
	if (dmc->num_sets_bits < 16) {
		dmc->cache_flags |= CACHE_FLAGS_MD8;
		pr_info("Not enough sets to use small metadata");
		return 1;
	}

	/*
	 * Now compute the largest sector number that we can shrink; then see
	 * if the source volume is smaller.
	 */
	lsb_bits = dmc->consecutive_shift + dmc->block_shift;
	msb_bits_24 = 24 - 1 - lsb_bits;        /* 1 for wrapped bit */
	max_dbn =
		((u_int64_t)1) << (msb_bits_24 + dmc->num_sets_bits + lsb_bits);
	if (to_sector(eio_get_device_size(dmc->disk_dev)) > max_dbn) {
		dmc->cache_flags |= CACHE_FLAGS_MD8;
		pr_info("Source volume too big to use small metadata");
		return 1;
	}

	return 0;
}

/*
 * eio_hash_block
 */
u_int32_t eio_hash_block(struct cache_c *dmc, sector_t dbn)
{
	int wrapped;
	u_int64_t set_number;

	EIO_DBN_TO_SET(dmc, dbn, set_number, wrapped);
	EIO_ASSERT(set_number < dmc->num_sets);

	return (u_int32_t)set_number;
}

/*
 * eio_shrink_dbn
 *
 * Shrink a 5-byte "dbn" into a 3-byte "dbn" by eliminating 16 lower bits
 * of the set number this "dbn" belongs to.
 */
unsigned int eio_shrink_dbn(struct cache_c *dmc, sector_t dbn)
{
	u_int32_t dbn_24;
	sector_t lsb;
	sector_t wrapped;
	sector_t msb;
	sector_t set_number;

	EIO_ASSERT(!EIO_MD8(dmc));
	if (unlikely(dbn == 0))
		return 0;

	lsb = dbn & SECTORS_PER_SET_MASK;
	EIO_DBN_TO_SET(dmc, dbn, set_number, wrapped);
	msb = dbn >> (dmc->num_sets_bits + SECTORS_PER_SET_SHIFT);
	dbn_24 =
		(unsigned int)(lsb | (wrapped << SECTORS_PER_SET_SHIFT) |
			       (msb << (SECTORS_PER_SET_SHIFT + 1)));

	return dbn_24;
}

/*
 * eio_expand_dbn
 *
 * Expand a 3-byte "dbn" into a 5-byte "dbn" by adding 16 lower bits
 * of the set number this "dbn" belongs to.
 */
sector_t eio_expand_dbn(struct cache_c *dmc, u_int64_t index)
{
	u_int32_t dbn_24;
	u_int64_t set_number;
	sector_t lsb;
	sector_t msb;
	sector_t dbn_40;

	EIO_ASSERT(!EIO_MD8(dmc));
	/*
	 * Expanding "dbn" zero?
	 */
	if (index == dmc->index_zero &&
	    dmc->index_zero < (u_int64_t)dmc->assoc)
		return 0;

	dbn_24 = dmc->cache[index].md4_md & EIO_MD4_DBN_MASK;
	if (dbn_24 == 0 && EIO_CACHE_STATE_GET(dmc, index) == INVALID)
		return (sector_t)0;

	set_number = EIO_DIV(index, dmc->assoc);
	lsb = dbn_24 & SECTORS_PER_SET_MASK;
	msb = dbn_24 >> (SECTORS_PER_SET_SHIFT + 1);    /* 1 for wrapped */
	/* had we wrapped? */
	if ((dbn_24 & SECTORS_PER_SET) != 0) {
		dbn_40 = msb << (dmc->num_sets_bits + SECTORS_PER_SET_SHIFT);
		dbn_40 |= (set_number + dmc->num_sets) << SECTORS_PER_SET_SHIFT;
		dbn_40 |= lsb;
	} else {
		dbn_40 = msb << (dmc->num_sets_bits + SECTORS_PER_SET_SHIFT);
		dbn_40 |= set_number << SECTORS_PER_SET_SHIFT;
		dbn_40 |= lsb;
	}
	EIO_ASSERT(unlikely(dbn_40 < EIO_MAX_SECTOR));

	return (sector_t)dbn_40;
}
EXPORT_SYMBOL(eio_expand_dbn);

/*
 * eio_invalidate_md
 */
void eio_invalidate_md(struct cache_c *dmc, u_int64_t index)
{

	if (EIO_MD8(dmc))
		dmc->cache_md8[index].md8_md = EIO_MD8_INVALID;
	else
		dmc->cache[index].md4_md = EIO_MD4_INVALID;
}

/*
 * eio_md4_dbn_set
 */
void eio_md4_dbn_set(struct cache_c *dmc, u_int64_t index, u_int32_t dbn_24)
{

	EIO_ASSERT((dbn_24 & ~EIO_MD4_DBN_MASK) == 0);

	/* retain "cache_state" */
	dmc->cache[index].md4_md &= ~EIO_MD4_DBN_MASK;
	dmc->cache[index].md4_md |= dbn_24;

	/* XXX excessive debugging */
	if (dmc->index_zero < (u_int64_t)dmc->assoc &&  /* sector 0 cached */
	    index == dmc->index_zero &&                 /* we're accessing sector 0 */
	    dbn_24 != 0)                                /* we're replacing sector 0 */
		dmc->index_zero = dmc->assoc;
}

/*
 * eio_md8_dbn_set
 */
void eio_md8_dbn_set(struct cache_c *dmc, u_int64_t index, sector_t dbn)
{

	EIO_ASSERT((dbn & ~EIO_MD8_DBN_MASK) == 0);

	/* retain "cache_state" */
	dmc->cache_md8[index].md8_md &= ~EIO_MD8_DBN_MASK;
	dmc->cache_md8[index].md8_md |= dbn;

	/* XXX excessive debugging */
	if (dmc->index_zero < (u_int64_t)dmc->assoc &&  /* sector 0 cached */
	    index == dmc->index_zero &&                 /* we're accessing sector 0 */
	    dbn != 0)                                   /* we're replacing sector 0 */
		dmc->index_zero = dmc->assoc;
}
