/*
 *   Copyright (c) International Business Machines Corp., 2000-2002
 *   Copyright (c) Tino Reichardt, 2014
 *
 *   This program is free software;  you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY;  without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program;  if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/*
 *   FUNCTION: common data & function prototypes
 */

#define _LARGEFILE64_SOURCE
#define __USE_LARGEFILE64  1
#define _FILE_OFFSET_BITS  64
#ifdef _MSC_VER
  #define __const const
#endif

#include "lib.h"

/* Defines */
#define AGGREGATE_2ND_I -1

int xRead(int64_t address, unsigned count, char *buffer)
{
	int64_t block_address;
	char *block_buffer;
	int64_t length;
	unsigned offset;

	offset = address & (bsize - 1);
	length = (offset + count + bsize - 1) & ~(bsize - 1);

	if ((offset == 0) & (length == count))
		return ujfs_rw_diskblocks(fp, address, count, buffer, GET);

	block_address = address - offset;
	block_buffer = (char *) malloc(length);
	if (block_buffer == 0)
		return 1;

	if (ujfs_rw_diskblocks(fp, block_address, length, block_buffer, GET)) {
		free(block_buffer);
		return 1;
	}
	memcpy(buffer, block_buffer + offset, count);
	free(block_buffer);
	return 0;
}

int xWrite(int64_t address, unsigned count, char *buffer)
{
	int64_t block_address;
	char *block_buffer;
	int64_t length;
	unsigned offset;

	offset = address & (bsize - 1);
	length = (offset + count + bsize - 1) & ~(bsize - 1);

	if ((offset == 0) & (length == count))
		return ujfs_rw_diskblocks(fp, address, count, buffer, PUT);

	block_address = address - offset;
	block_buffer = (char *) malloc(length);
	if (block_buffer == 0)
		return 1;

	if (ujfs_rw_diskblocks(fp, block_address, length, block_buffer, GET)) {
		free(block_buffer);
		return 1;
	}
	memcpy(block_buffer + offset, buffer, count);
	if (ujfs_rw_diskblocks(fp, block_address, length, block_buffer, PUT)) {
		free(block_buffer);
		return 1;
	}
	free(block_buffer);
	return 0;
}

int find_inode(unsigned inum, unsigned which_table, int64_t * address)
{
	int extnum;
	struct iag iag;
	int iagnum;
	int64_t map_address;
	int rc;

	iagnum = INOTOIAG(inum);
	extnum = (inum & (INOSPERIAG - 1)) >> L2INOSPEREXT;
	if (find_iag(iagnum, which_table, &map_address) == 1)
		return 1;

	rc = ujfs_rw_diskblocks(fp, map_address, sizeof (struct iag), &iag, GET);
	if (rc) {
		fputs("find_inode: Error reading iag\n", stderr);
		return 1;
	}

	/* swap if on big endian machine */
	ujfs_swap_iag(&iag);

	if (iag.inoext[extnum].len == 0)
		return 1;

	*address = (addressPXD(&iag.inoext[extnum]) << l2bsize) +
	    ((inum & (INOSPEREXT - 1)) * sizeof (struct dinode));
	return 0;
}

#define XT_CMP(CMP, K, X) \
{ \
	int64_t offset64 = offsetXAD(X); \
	(CMP) = ((K) >= offset64 + lengthXAD(X)) ? 1 : \
		((K) < offset64) ? -1 : 0 ; \
}

int find_iag(unsigned iagnum, unsigned which_table, int64_t * address)
{
	int base;
	char buffer[PSIZE];
	int cmp;
	struct dinode fileset_inode;
	int64_t fileset_inode_address;
	int64_t iagblock;
	int index;
	int lim;
	xtpage_t *page;
	int rc;

	if (which_table != FILESYSTEM_I &&
	    which_table != (unsigned int)AGGREGATE_I && which_table != (unsigned int)AGGREGATE_2ND_I) {
		fprintf(stderr, "find_iag: Invalid fileset, %d\n", which_table);
		return 1;
	}
	iagblock = IAGTOLBLK(iagnum, L2PSIZE - l2bsize);

	if (which_table == (unsigned int)AGGREGATE_2ND_I) {
		fileset_inode_address = AIT_2nd_offset + sizeof (struct dinode);
	} else {
		fileset_inode_address = AGGR_INODE_TABLE_START + (which_table * sizeof (struct dinode));
	}
	rc = xRead(fileset_inode_address, sizeof (struct dinode), (char *) &fileset_inode);
	if (rc) {
		fputs("find_inode: Error reading fileset inode\n", stderr);
		return 1;
	}

	page = (xtpage_t *) & (fileset_inode.di_btroot);

      descend:
	/* Binary search */
	for (base = XTENTRYSTART,
	     lim = __le16_to_cpu(page->header.nextindex) - XTENTRYSTART; lim; lim >>= 1) {
		index = base + (lim >> 1);
		XT_CMP(cmp, iagblock, &(page->xad[index]));
		if (cmp == 0) {
			/* HIT! */
			if (page->header.flag & BT_LEAF) {
				*address = (addressXAD(&(page->xad[index]))
					    + (iagblock - offsetXAD(&(page->xad[index]))))
				    << l2bsize;
				return 0;
			} else {
				rc = xRead(addressXAD(&(page->xad[index])) << l2bsize,
					   PSIZE, buffer);
				if (rc) {
					fputs("find_iag: Error reading btree node\n", stderr);
					return 1;
				}
				page = (xtpage_t *) buffer;

				goto descend;
			}
		} else if (cmp > 0) {
			base = index + 1;
			--lim;
		}
	}

	if (page->header.flag & BT_INTERNAL) {
		/* Traverse internal page, it might hit down there
		 * If base is non-zero, decrement base by one to get the parent
		 * entry of the child page to search.
		 */
		index = base ? base - 1 : base;

		rc = xRead(addressXAD(&(page->xad[index])) << l2bsize, PSIZE, buffer);
		if (rc) {
			fputs("find_iag: Error reading btree node\n", stderr);
			return 1;
		}
		page = (xtpage_t *) buffer;

		goto descend;
	}

	/* Not found! */
	fprintf(stderr, "find_iag:  IAG %d not found!\n", iagnum);
	return 1;
}

#undef XT_CMP
