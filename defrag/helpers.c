/*
 *   Copyright (c) International Business Machines Corp., 2000-2002
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
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jfs_endian.h"
#include "jfs_types.h"
#include "jfs_superblock.h"
#include "jfs_filsys.h"
#include "jfs_dinode.h"
#include "jfs_dmap.h"
#include "jfs_imap.h"

#include "devices.h"
#include "debug.h"

#include "helpers.h"
#include "utilsubs.h"
#include "message.h"

/* forward references */
static void bAllocate(void);

/* Global Data */

int32_t Fail_Quietly = 0;	/* If set, don't report errors */

int LVHandle;
LV_t LVMount;			/* logical volume descriptor */
LV_t *lvMount = &LVMount;

struct dinode DIBMap;		/* block allocation map inode */
struct dinode *diBMap = &DIBMap;

struct dinode DIIMap;		/* inode allocation map inode */
struct dinode *diIMap = &DIIMap;

int32_t IAGNumber = 0;

/*
 *	file system under defragfs
 */
FS_t FSMount;			/* file system descriptor */
FS_t *fsMount = &FSMount;

/* imap xtree sequential read control */
int64_t IMapXtreeLMLeaf;	/* imap xtree leftmost leaf */
xtpage_t IMapXtreeLeaf;		/* imap xtree leaf buffer */
IMapXtree_t IMapXtree = { &IMapXtreeLeaf, XTENTRYSTART, 1 };

/*
 *	openLV()
 */
int openLV(char *LVName)
{
	int rc = 0;
	int64_t LVSize;

	if ((rc = Is_Device_Mounted(LVName)) == MSG_JFS_VOLUME_IS_MOUNTED) {	/* is mounted */
		printf("\n%s is mounted.\n", LVName);
	} else if (rc == MSG_JFS_VOLUME_IS_MOUNTED_RO) {	/* is mounted read only */
		printf("\n%s is mounted 'read only'.\n", LVName);
	} else if (rc == MSG_JFS_MNT_LIST_ERROR) {	/* setmntent failed */
		printf("\nCannot detect if %s is mounted.\n", LVName);
		return rc;
	} else if (rc == MSG_JFS_NOT_JFS) {	/* mounted, but not jfs */
		printf("\nDevice mounted, but not type jfs!\n");
		return rc;
	} else {		/* not mounted */
		printf("\n%s is NOT mounted.\n", LVName);
		return -1;
	}

	LVHandle = open(LVName, O_RDONLY, 0);
	if (LVHandle < 0) {
		if (!Fail_Quietly)
			message_user(MSG_OSO_CANT_OPEN, NULL, 0, OSO_MSG);
		return rc;
	}

	LVMount.pbsize = PBSIZE;
	LVMount.l2pbsize = log2shift(LVMount.pbsize);

	/* get logical volume size */
	if ((rc = ujfs_get_dev_size(LVHandle, &LVSize))) {
		message_user(MSG_OSO_ERR_ACCESSING_DISK, NULL, 0, OSO_MSG);
		close(LVHandle);
		return rc;
	}

	LVMount.LVSize = LVSize >> LVMount.l2pbsize;

	return rc;
}

/*
 *	openFS()
 */
int openFS(void)
{
	int rc;
	struct superblock *sb;
	buf_t *bp;

	/*
	 *      validate and retrieve fs parameters from superblock
	 */
	/* try to read the primary superblock */
	rc = bRawRead(LVHandle, (int64_t) SUPER1_OFF, (int32_t) PAGESIZE, &bp);
	if (rc != 0) {
		/* try to read the secondary superblock */
		rc = bRawRead(LVHandle, (int64_t) SUPER2_OFF, (int32_t) PAGESIZE, &bp);
		if (rc != 0) {
			return rc;
		}
	}

	sb = (struct superblock *) bp->b_data;

	/* check magic/version number */
	if (strncmp(sb->s_magic, JFS_MAGIC, (unsigned) strlen(JFS_MAGIC))
	    || (sb->s_version > JFS_VERSION)) {
		message_user(MSG_JFS_BAD_SUPERBLOCK, NULL, 0, JFS_MSG);
		return -1;
	}

	if (sb->s_state & FM_DIRTY) {
		message_user(MSG_JFS_DIRTY, NULL, 0, JFS_MSG);
		return -1;
	}

	/* assert(lvMount->pbsize == sb->s_pbsize); */
	/* assert(lvMount->l2pbsize == sb->s_l2pbsize); */

	fsMount->bsize = sb->s_bsize;
	fsMount->l2bsize = sb->s_l2bsize;
	fsMount->l2bfactor = sb->s_l2bfactor;
	fsMount->nbperpage = PAGESIZE >> fsMount->l2bsize;
	fsMount->l2nbperpage = log2shift(fsMount->nbperpage);
	fsMount->FSSize = sb->s_size >> sb->s_l2bfactor;
	fsMount->AGSize = sb->s_agsize;

	bRelease(bp);

	return rc;
}

/*
 *	closeFS()
 */
void closeFS(void)
{
	close(LVHandle);
}

/*
 * NAME:	readBMapGCP()
 *
 * FUNCTION:	read the bmap global control page and
 *		the initial level L2, L1.0 and L0.0 control pages;
 *
 * note: bmap file structure is satic during defragfs()
 * (defragfs() and extendfs() are mutually exclusive operation);
 */
int readBMapGCP(BMap_t * bMap)
{
	int rc;
	int64_t xaddr;
	int32_t xlen;
	struct dbmap *bcp;

	/* first 4 pages are made contiguous by mkfs() */
	xlen = 4 << fsMount->l2nbperpage;
	rc = xtLookup(diBMap, (int64_t) 0, &xaddr, &xlen, 0);
	if (rc)
		return rc;

	rc = pRead(fsMount, xaddr, xlen, (void *) bMap);
	if (rc) {
		return rc;
	}

	bcp = (struct dbmap *) bMap;

	/* swap if on big endian machine */
	ujfs_swap_dbmap(bcp);

	fsMount->nAG = bcp->dn_numag;

	return 0;
}

/*
 * NAME:	readBMapLCP()
 *
 * FUNCTION:  	read level control page of given level for given block number;
 */
int readBMapLCP(int64_t bn, int32_t level, struct dmapctl *lcp)
{
	int rc = 0;
	int64_t xoff = 0, xaddr;
	int32_t xlen;

	switch (level) {
	case 0:
		xoff = BLKTOL0(bn, fsMount->l2nbperpage);
		break;
	case 1:
		xoff = BLKTOL1(bn, fsMount->l2nbperpage);
		break;
	case 2:
		xoff = fsMount->nbperpage;
		break;
	}

	xlen = fsMount->nbperpage;
	rc = xtLookup(diBMap, xoff, &xaddr, &xlen, 0);
	if (rc != 0)
		return rc;

	rc = pRead(fsMount, xaddr, xlen, (void *) lcp);
	if (rc) {
		return rc;
	}

	/* swap if on big endian machine */
	ujfs_swap_dmapctl(lcp);

	return 0;
}

/*
 * NAME:        readBMap()
 *
 * FUNCTION:	Read ndmaps dmap pages starting from the start_dmap.
 *
 * note: bmap file structure is satic during defragfs()
 * (defragfs() and extendfs() are mutually exclusive operation);
 */
int readBMap(int64_t start_dmap,	/* the dmap number to start read */
	     int32_t ndmaps,	/* number of dmap pages read */
	     struct dmap * dmap)
{				/* buffer pointer */
	int rc;
	int64_t xaddr;
	int64_t xoff;
	int32_t xlen0, xlen;

	/* convert dmap number to xoffset */
	xaddr = start_dmap << L2BPERDMAP;
	xoff = BLKTODMAP(xaddr, fsMount->l2nbperpage);

	xlen0 = ndmaps << fsMount->l2nbperpage;
	xlen = fsMount->nbperpage;
	for (; xlen0 > 0; xlen0 -= xlen) {
		rc = xtLookup(diBMap, xoff, &xaddr, &xlen, 0);
		if (rc)
			return rc;

		rc = pRead(fsMount, xaddr, xlen, (void *) dmap);
		if (rc) {
			return rc;
		}

		/* swap if on big endian machine */
		ujfs_swap_dmap(dmap);

		xoff += xlen;
		dmap = (struct dmap *) ((char *) dmap + PAGESIZE);
	}

	return 0;
}

/*
 * NAME:	readIMapGCPSequential()
 *
 * FUNCTION:	read the imap global control page.
 */
int readIMapGCPSequential(IMap_t * iMap, struct iag * iag)
{
	int rc;
	xtpage_t *p;
	int64_t xaddr;

	/* read in leftmost leaft xtpage */
	rc = xtLMLeaf(diIMap, &IMapXtreeLeaf);
	if (rc != 0)
		return rc;

	p = &IMapXtreeLeaf;
	IMapXtreeLMLeaf = addressPXD(&p->header.self);

	/* read IMap control page */
	xaddr = addressXAD(&p->xad[XTENTRYSTART]);
	rc = pRead(fsMount, xaddr, fsMount->nbperpage, (void *) iMap);

	/* swap if on big endian machine */
	ujfs_swap_dinomap(&(iMap->ctl));

	/* init for start of bitmap page read */
	IAGNumber = 0;
	IMapXtree.index = XTENTRYSTART;
	IMapXtree.page = 1;	/* skip for global control page */
	iag->iagnum = -1;

	return rc;
}

/*
 * NAME:	readIMapSequential()
 *
 * FUNCTION:	read one iag page at a time.
 *
 * state variable:
 *	IMapXtree.leaf - current imap leaf xtpage under scan;
 *	IMapXtree.index - current xad entry index in iMapXtreeLeaf;
 *	IMapXtree.page - iag number to read within current imap extent;
 *
 * note: IMap pages may grow (but NOT freed) dynamically;
 */
int readIMapSequential(struct iag * iag)
{
	int rc;
	xtpage_t *p;
	int64_t xaddr;
	int32_t xlen, delta;

	p = &IMapXtreeLeaf;

	while (1) {
		/* continue with current leaf xtpage ? */
		if (IMapXtree.index < p->header.nextindex) {
			/* determine iag page extent */
			xaddr = addressXAD(&p->xad[IMapXtree.index]);
			xlen = lengthXAD(&p->xad[IMapXtree.index]);

			if (IMapXtree.page) {
				/* compute offset within current extent */
				delta = IMapXtree.page << fsMount->l2nbperpage;
				xaddr += delta;
				xlen -= delta;
			}

			/* read a iag page */
			rc = pRead(fsMount, xaddr, fsMount->nbperpage, (void *) iag);

			/* swap if on big endian machine */
			ujfs_swap_iag(iag);

			if (rc != 0) {
				return rc;
			}

			/*current extent has more iag */
			if (xlen > fsMount->nbperpage) {
				/* continue to read from current extent */
				IMapXtree.page++;
			} else {
				IMapXtree.index++;
				IMapXtree.page = 0;
			}

			return 0;
		}

		/* read next/right sibling leaf xtpage */
		if (p->header.next != 0) {
			xaddr = p->header.next;

			/* init for next leaf xtpage */
			IMapXtree.index = XTENTRYSTART;
			IMapXtree.page = 0;
		} else {
			/* init for start of bitmap page read */
			IAGNumber = 0;
			IMapXtree.index = XTENTRYSTART;
			IMapXtree.page = 1;	/* skip for global control page */

			if (p->header.prev != 0)
				xaddr = IMapXtreeLMLeaf;
			/* a single leaf xtree */
			else
				continue;
		}

		/* read next xtree leaf page */
		rc = pRead(fsMount, xaddr, fsMount->nbperpage, (void *) &IMapXtreeLeaf);

		/* swap if on big endian machine */
		ujfs_swap_xtpage_t(&IMapXtreeLeaf);

		if (rc != 0) {
			return rc;
		}
	}
}

/*
 * NAME:        xtLMLeaf()
 *
 * FUNCTION:    read in leftmost leaf page of the xtree
 *              by traversing down leftmost path of xtree;
 */
int xtLMLeaf(struct dinode * dip,	/* disk inode */
	     xtpage_t * pp)
{				/* pointer to leftmost leaf xtpage */
	int rc;
	xtpage_t *p;
	int64_t xaddr;

	/* start from root in dinode */
	p = (xtpage_t *) & dip->di_btroot;
	/* is this page leaf ? */
	if (p->header.flag & BT_LEAF) {
		p->header.next = p->header.prev = 0;
		memcpy(pp, p, DISIZE);
		return 0;
	}

	/*
	 * traverse down leftmost child node to the leftmost leaf of xtree
	 */
	while (1) {
		/* read in the leftmost child page */
		xaddr = addressXAD(&p->xad[XTENTRYSTART]);
		rc = pRead(fsMount, xaddr, fsMount->nbperpage, (void *) pp);
		if (rc) {
			return rc;
		}

		/* swap if on big endian machine */
		ujfs_swap_xtpage_t(pp);

		/* is this page leaf ? */
		if (pp->header.flag & BT_LEAF)
			return 0;
		else
			p = pp;
	}
}

/*
 *	 xtree key/entry comparison: extent offset
 *
 * return:
 *	-1: k < start of extent
 *	 0: start_of_extent <= k <= end_of_extent
 *	 1: k > end_of_extent
 */
#define XT_CMP(CMP, K, X, OFFSET64)\
{\
	OFFSET64 = offsetXAD(X);\
	(CMP) = ((K) >= OFFSET64 + lengthXAD(X)) ? 1 :\
	      ((K) < OFFSET64) ? -1 : 0;\
}

/*
 *	xtLookup()
 *
 * function:	search for the xad entry covering specified offset.
 *
 * parameters:
 *	ip	    - file object;
 *	xoff	- extent offset;
 *	cmpp	- comparison result:
 *	btstack	- traverse stack;
 *	flag	- search process flag (XT_INSERT);
 *
 * returns:
 *	btstack contains (bn, index) of search path traversed to the entry.
 *	*cmpp is set to result of comparison with the entry returned.
 *	the page containing the entry is pinned at exit.
 */
int xtLookup(struct dinode * dip, int64_t xoff,	/* offset of extent */
	     int64_t * xaddr, int32_t * xlen, uint32_t flag)
{
	int rc = 0;
	buf_t *bp = NULL;	/* page buffer */
	xtpage_t *p;		/* page */
	xad_t *xad;
	int64_t t64;
	int32_t cmp = 1, base, index, lim;
	int32_t t32;

	/*
	 *      search down tree from root:
	 *
	 * between two consecutive entries of <Ki, Pi> and <Kj, Pj> of
	 * internal page, child page Pi contains entry with k, Ki <= K < Kj.
	 *
	 * if entry with search key K is not found
	 * internal page search find the entry with largest key Ki
	 * less than K which point to the child page to search;
	 * leaf page search find the entry with smallest key Kj
	 * greater than K so that the returned index is the position of
	 * the entry to be shifted right for insertion of new entry.
	 * for empty tree, search key is greater than any key of the tree.
	 */

	/* start with inline root */
	p = (xtpage_t *) & dip->di_btroot;

      xtSearchPage:
	lim = p->header.nextindex - XTENTRYSTART;

	/*
	 * binary search with search key K on the current page
	 */
	for (base = XTENTRYSTART; lim; lim >>= 1) {
		index = base + (lim >> 1);

		XT_CMP(cmp, xoff, &p->xad[index], t64);
		if (cmp == 0) {
			/*
			 *      search hit
			 */
			/* search hit - leaf page:
			 * return the entry found
			 */
			if (p->header.flag & BT_LEAF) {
				/* save search result */
				xad = &p->xad[index];
				t32 = xoff - offsetXAD(xad);
				*xaddr = addressXAD(xad) + t32;
				*xlen = MIN(*xlen, lengthXAD(xad) - t32);

				rc = cmp;
				goto out;
			}

			/* search hit - internal page:
			 * descend/search its child page
			 */
			goto descend;
		}

		if (cmp > 0) {
			base = index + 1;
			--lim;
		}
	}

	/*
	 *      search miss
	 *
	 * base is the smallest index with key (Kj) greater than
	 * search key (K) and may be zero or maxentry index.
	 */
	/*
	 * search miss - leaf page:
	 *
	 * return location of entry (base) where new entry with
	 * search key K is to be inserted.
	 */
	if (p->header.flag & BT_LEAF) {
		rc = cmp;
		goto out;
	}

	/*
	 * search miss - non-leaf page:
	 *
	 * if base is non-zero, decrement base by one to get the parent
	 * entry of the child page to search.
	 */
	index = base ? base - 1 : base;

	/*
	 * go down to child page
	 */
      descend:
	/* get the child page block number */
	t64 = addressXAD(&p->xad[index]);

	/* release parent page */
	if (bp)
		bRelease(bp);

	/* read child page */
	bRead(fsMount, t64, fsMount->nbperpage, &bp);

	p = (xtpage_t *) bp->b_data;
	goto xtSearchPage;

      out:
	/* release current page */
	if (bp)
		bRelease(bp);

	return rc;
}

/*
 *	pRead()
 *
 * read into specified buffer;
 */
int pRead(FS_t * fsMount, int64_t xaddr,	/* in bsize */
	  int32_t xlen,	/* in bsize */
	  void *p)
{				/* buffer */
	int rc;
	int64_t off;		/* offset in byte */
	int32_t len;		/* length in byte */

	/* read in from disk */
	off = xaddr << fsMount->l2bsize;
	len = xlen << fsMount->l2bsize;
	if ((rc = ujfs_rw_diskblocks(LVHandle, off, len, p, GET))) {
		message_user(MSG_OSO_READ_ERROR, NULL, 0, OSO_MSG);
	}

	return rc;
}

/*
 *	buffer pool
 *	===========
 */
int32_t NBUFFER = 8;
int32_t nBuffer = 0;
buf_t *bCachelist = NULL;

static void bAllocate(void)
{
	buf_t *bp;
	char *buffer;
	int i;

	/* allocate buffer header */
	if ((bp = malloc(NBUFFER * sizeof (buf_t))) == NULL) {
		message_user(MSG_OSO_NOT_ENOUGH_MEMORY, NULL, 0, OSO_MSG);
		exit(ERROR_DISK_FULL);
	}

	/* allocate buffer pages */
	if ((buffer = malloc(NBUFFER * PAGESIZE)) == NULL) {
		message_user(MSG_OSO_NOT_ENOUGH_MEMORY, NULL, 0, OSO_MSG);
		exit(ERROR_DISK_FULL);
	}

	/* insert buffer headers in lru/freelist */
	for (i = 0; i < NBUFFER; i++, bp++, buffer += PAGESIZE) {
		bp->b_data = buffer;
		bp->b_next = bCachelist;
		bCachelist = bp;
	}

	nBuffer += NBUFFER;
}

/*
 *	bRead()
 *
 * aloocate, read and return a buffer;
 */
int bRead(FS_t * fsMount, int64_t xaddr,	/* in bsize */
	  int32_t xlen,	/* in bsize */
	  buf_t ** bpp)
{
	int rc;
	int64_t off;		/* offset in byte */
	int32_t len;		/* length in byte */
	buf_t *bp;

	/* allocate buffer */
	if (bCachelist == NULL)
		bAllocate();
	bp = bCachelist;
	bCachelist = bp->b_next;

	/* read in from disk */
	off = xaddr << fsMount->l2bsize;
	len = xlen << fsMount->l2bsize;
	rc = ujfs_rw_diskblocks(LVHandle, off, len, (void *) bp->b_data, GET);
	if (rc == 0)
		*bpp = bp;
	else {
		message_user(MSG_OSO_READ_ERROR, NULL, 0, OSO_MSG);
		bRelease(bp);
	}

	return rc;
}

int bRawRead(uint32_t LVHandle, int64_t off,	/* in byte */
	     int32_t len,	/* in byte */
	     buf_t ** bpp)
{
	int rc;
	buf_t *bp;

	/* allocate buffer */
	if (bCachelist == NULL)
		bAllocate();
	bp = bCachelist;
	bCachelist = bp->b_next;

	/* read in from disk */
	rc = ujfs_rw_diskblocks(LVHandle, off, len, (void *) bp->b_data, GET);
	if (rc == 0)
		*bpp = bp;
	else {
		message_user(MSG_OSO_READ_ERROR, NULL, 0, OSO_MSG);
		bRelease(bp);
	}

	return rc;
}

void bRelease(buf_t * bp)
{
	/* release buffer */
	bp->b_next = bCachelist;
	bCachelist = bp;
}
