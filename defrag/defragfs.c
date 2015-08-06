/*
 *   Copyright (c) International Business Machines  Corp., 2000
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
 *
 *
 *
 *	defragfs.c: defragment file system
 *
 * limitation:
 * this version implements heuristics of simple compaction
 * per allocation group or subset of it when it is large.
 * little of reports of pre- and post-defragmentation
 * are available.
 * (refer to design notes in defragfs.note for comments, etc.)
 *
 * further experiments for heuristics/policy of defragmentation
 * may be necessary, especially if file system/allocation group
 * size is large: i think most of building block services
 * required/reusable to implement variations of defragmentation
 * heuristics/policy in application level and ifs level are made
 * available now;
 *
 * restriction: defragmentation of file system is mutually
 * exclusive of extension of file system concurrently.
 */

#include <config.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "jfs_byteorder.h"
#include "jfs_types.h"
#include "jfs_superblock.h"
#include "jfs_filsys.h"
#include "jfs_dinode.h"
#include "jfs_dmap.h"
#include "jfs_imap.h"
#include "jfs_xtree.h"
#include "jfs_dtree.h"
#include "jfs_defragfs.h"

#include "devices.h"
#include "debug.h"

#include "helpers.h"
#include "utilsubs.h"
#include "message.h"

/*
 *	debug control
 */
#ifdef _JFS_DEBUG
/* information message: e.g., configuration, major event */
#define jFYI(button,prspec) \
     do { if (button) printk prspec; } while (0)
/* debug event message: */
#define jEVENT(button,prspec) \
     do { if (button) printk prspec; } while (0)
/* error event message: e.g., i/o error */
#define jERROR(button,prspec) \
     do { if (button) printk prspec; } while (0)
#else				/* _JFS_DEBUG */
#define jEVENT(button,prspec)
#define jFYI(button,prspec)
#define jERROR(button,prspec)
#endif				/* _JFS_DEBUG */

/* command control */
#define DEFRAG           1
#define NODEFRAG_FREE    2
#define NODEFRAG_FULL    3
#define NODEFRAG_LONGRUN 4

bool DoIt = true;		/* go ahead. */

/* policy control */
#define XMOVESIZE	  64	/* max size of extent to move + 1 */
#define BMAPSET		  16	/* max # of bitmaps to be processed at a time */
#define	MIN_FREERATE   3	/* min free block % of AG to defragment */

/* return code control */
#define DEFRAG_OK	   0	/* generic RC - completed successfully */
#define DEFRAG_FAILED -1	/* generic RC - completed unsuccessfully */

/*
 *	device under defragfs
 */
char *LVName;			// device name

/*
 *	block allocation map
 */
BMap_t BMap;			// block allocation map control page
BMap_t *bMap = &BMap;

/* bitmap page control */
int32_t BMapPageNumber;
struct dmap BMapSet[BMAPSET];

/*
 *	inode allocation map
 */
IMap_t IMap;			// inode allocation map control page
IMap_t *iMap = &IMap;

/* bitmap page control */
struct iag iagbuf;

/* inode extent buffer */
int32_t IXNumber = 0;
int32_t IXINumber = 0;		/* next inode index of ixbuf */
struct dinode ixbuf[INOSPEREXT];

/*
 *	relocation control
 */
int32_t AGNumber;
int32_t nBMapPerAG;		/* number of dmaps per AG  */
int32_t nAGPerLCP;		/* number of ags covered by aglevel lcp */

int64_t AGStart;		/* start aggr. block number of current AG  */
int64_t AGEnd;			/* end aggr. blk number of current AG */
int32_t AGBMapStart;		/* start bMap page number of current AG */
int32_t AGBMapEnd;		/* end bMap page number of current AG */

int32_t nBMapSet;		/* number of dmaps in a processing dmap set */
int64_t BMapSetStart;		/* start block number of current BMapSet of AG */
int64_t BMapSetEnd;		/* end block number of current BMapSet of AG */

int64_t barrierStart;		/* start blk number of region in BMapSet to compact */
int64_t barrierEnd;		/* end blk number of region in BMapSet to compact */
int64_t barrier;		/* current barrier blk number in region in BMapSet */

/*
 *	inode-xad table
 */
/*
 *	ixad_t: movable extent allocation descriptor
 *
 *  movable extent descriptor specifying the object and xad.
 *  the length of the extent is specified by the ixadTbl list it
 *  is enqueued which is indexed by the extent length.
 *  for a regular file, the offset is used as search key.
 *  for a directory, the addr of left most dtpage of the level
 *  is used to specify search domain.
 *
 *  When moving an extent, we must consider not to move
 *  extents that belong to one inode far apart.
 */
#define  INODE_TYPE	0x0000F000	/* IFREG or IFDIR */

typedef struct ixad {
	struct ixad *next;	/* 4: next pointer */
	uint32_t flag;		/* 4: INDISEAG, OUTSIDEAG, IFREG, IFDIR */
	int32_t fileset;	/* 4: fileset number */
	uint32_t inostamp;	/* 4: fileset timestamp */
	uint32_t inumber;	/* 4: inode number */
	uint32_t gen;		/* 4: inode generation number */
	union {
		int64_t xoff;	/* @8: extent offset */
		int64_t lmxaddr;	/* @8: extent address of leftmost dtpage
					   * of the level whose idtentry covers
					   * the dtpage to be moved.
					   * zero if dtpage is dtroot.
					 */
	} xkey;
	int64_t xaddr;		/* @8: extent address */
} ixad_t;			/* (40) */

#define	f_xoff		xkey.xoff
#define	d_lmxaddr	xkey.lmxaddr

typedef struct ixadlist {
	ixad_t *head;		/* 4: anchor of the list */
	int32_t count;		/* 4: number of entry in the list */
} ixadlist_t;

ixadlist_t ixadTbl[XMOVESIZE];
int32_t nIxad = 0;
int32_t nIxadPool = 0;
ixad_t *ixadPool;		/* pointer to array (table) of ixad */
ixad_t *ixadHWM;
ixad_t *ixadFreelist;		/* head of free list */

uint32_t inoext_vector[2];

/*
 *	 inode traversal
 */
/* regular file */
int32_t xtype;
int64_t lmxaddr;
xtpage_t next_xtpg;		/* next xtpage to work on for the current inode */
xtpage_t *pnext_xtpg = NULL;
int32_t nxtpg_inx = 0;		/* the next index in next_xtpg to work on */

/* directory */
int64_t lmpg_addr;
int32_t lmxlen;
dtpage_t next_lmdtpg;		/*next left-most dtpage to work on for the current inode */
xtpage_t *pnext_lmdtpg = NULL;
dtpage_t next_dtpg;		/* next dtpage to work on for the current inode */
dtpage_t *pnext_dtpg = NULL;
int32_t ndtpg_inx = 0;		/* the next index in next_dtpg to work on */

/*
 *	statistics
 */
/* current AG working statistics */
int32_t AGFreeRun[XMOVESIZE] = { 0 };
int32_t AGAllocRun[XMOVESIZE] = { 0 };

/* FS cumulative summary */
int32_t FSFreeRun[XMOVESIZE] = { 0 };
int32_t FSAllocRun[XMOVESIZE] = { 0 };

/* Actions taken on AG's */
int32_t AG_DEFRAG;
int32_t AG_SKIP_FREE;
int32_t AG_SKIP_FULL;
int32_t AG_SKIP_LONG_RUN;

/* Count of free runs */
uint32_t total_free_runs;

/*
 *	Parameter array for messages
 */
#define MAXPARMS	2
#define MAXSTR		80
char *msg_parms[MAXPARMS];
char msgstr[MAXSTR];

/*
 *	handy macro
 */
#define UZBIT	(0x80000000)
#define BUDSIZE(s,m)  (1 << ((s) -(m)))

/*
 * forward references
 */
static int32_t defragfs(void);
static int32_t examineAG(int32_t agno, struct dmapctl *lcp, int32_t * AGBMapStart, int32_t * AGBMapEnd);
static int32_t compactAG(int32_t agno, int32_t AGBMapStart, int32_t AGBMapEnd, int32_t nBMapSet);
static int32_t buildIxadTbl(int32_t agno, int64_t barrierStart, int64_t barrierEnd);
static int32_t addXtree(struct dinode * dip, int64_t barrierStart, int64_t barrierEnd);
static int32_t addDtree(struct dinode * dip, int64_t barrierStart, int64_t barrierEnd);
static ixad_t *allocIxad(int32_t * rc);
static void insertIxad(int32_t nblks, ixad_t * ixad);

#ifdef _JFS_OS2
static void washIxadTbl(int64_t barrier);
#endif

static void purgeIxadTbl(void);
static int32_t compactBMap(int32_t BmapSetBMapStart, int32_t bmx);
static int32_t moveExtent(int32_t BMapSetBMapStart, int64_t fxaddr, int32_t fxlen);
static int32_t fscntlMove(int64_t nxaddr, int32_t xlen, ixad_t * ixad);
static void dbFreeBMap(int32_t BmapSetBMapStart, int64_t blkno, int32_t nblocks);
static int32_t preamble(void);
static int32_t getProvisions(void);
static int32_t whatDoYouWant(int32_t argc, char **argv);

/*
 * NAME:	main()
 *
 * FUNCTION:	Main routine of defragfs utility.
 */
int main(int argc, char **argv)
{
	int rc;

	/* parse command line arguments */
	if ((rc = whatDoYouWant(argc, argv))) {
		message_user(MSG_JFS_DEFRAGFS_USAGE, NULL, 0, JFS_MSG);
		goto out;
	}

	/* validate and open LV */
	if ((rc = openLV(LVName)))
		goto out;

	/* open FS */
	if ((rc = openFS()))
		goto out;

	/* alloc/init resources */
	if ((rc = getProvisions()))
		goto out;

	/* init defragfs */
      mainidea:
	if ((rc = preamble()))
		goto out;

	if (DoIt == true) {
		msg_parms[0] = LVName;
		message_user(MSG_JFS_DEFRAGMENTING, msg_parms, 1, JFS_MSG);
	}
	/*
	 * report pre-defragmentation statistics and
	 * defragment the file system
	 */
	if ((rc = defragfs()))
		goto out;

	/* report post-defragmentation statistics */
	if (DoIt == true) {
		message_user(MSG_JFS_DEFRAGFS_COMPLETE, NULL, 0, JFS_MSG);
		DoIt = false;
		goto mainidea;
	}

	/* close FS */
	closeFS();

      out:
	return rc;
}

/*
 *	defragfs()
 */
static int defragfs(void)
{
	int rc = 0;
	struct dbmap *gcp;		/* bmap global control page */
	struct dmapctl *lcp = NULL;	/* bmap level control page */
	int32_t nBMap;
	int32_t nAG;
	int32_t nFullAG;
	int64_t partialAG;
	int32_t nodeperlcp;
	int32_t lcpn;
	int32_t i;

	gcp = (struct dbmap *) & BMap.ctl;

	/* number of full AGs */
	nFullAG = gcp->dn_mapsize >> gcp->dn_agl2size;

	/* number of blocks in partial AG */
	partialAG = fsMount->FSSize & (gcp->dn_agsize - 1);

	/* Total AG's */
	nAG = nFullAG + (partialAG ? 1 : 0);

	/* number of nodes at agheigth level of summary tree */
	nodeperlcp = 1 << (L2LPERCTL - (gcp->dn_agheigth << 1));

	/* number of AGs covered at agheight level of summary tree */
	nAGPerLCP = nodeperlcp / gcp->dn_agwidth;

	/*
	 * init for lcp scan at aglevel
	 */
	/* lcp at ag level */
	switch (gcp->dn_aglevel) {
	case 0:
		lcp = (struct dmapctl *) & BMap.l0;
		break;
	case 1:
		lcp = (struct dmapctl *) & BMap.l1;
		break;
	case 2:
		lcp = (struct dmapctl *) & BMap.l2;
		break;
	}

	/* current level control page number at aglevel */
	lcpn = 0;

	jEVENT(0, ("defragfs: nFullAG:%d partialAG:%lld nAGPerLCP:%d\n",
		   nFullAG, partialAG, nAGPerLCP));

	/*
	 *      process each full AG
	 */
	for (AGNumber = 0; AGNumber < nFullAG; AGNumber++) {
		/* start and end block address of AG */
		AGStart = AGNumber << gcp->dn_agl2size;
		AGEnd = AGStart + gcp->dn_agsize - 1;

		/* start and end bitmap page number of AG */
		AGBMapStart = AGNumber * nBMapPerAG;
		AGBMapEnd = AGBMapStart + nBMapPerAG - 1;

		/* does current lcp cover current AG ? */
		if (AGNumber >= ((lcpn + 1) * nAGPerLCP)) {
			/* readin lcp covering i-th agwidth */
			rc = readBMapLCP(AGStart, gcp->dn_aglevel, lcp);
			if (rc)
				return rc;	/* i/o error */

			lcpn++;
		}

		jFYI(1, ("\t--- AG:%d AGStart:%lld AGEnd:%lld ---\n", AGNumber, AGStart, AGEnd));

		/*
		 *      determine if this AG needs defragmentation
		 */
		rc = examineAG(AGNumber, lcp, &AGBMapStart, &AGBMapEnd);
		if (rc == NODEFRAG_FREE) {
			AG_SKIP_FREE++;
			continue;
		} else if (rc == NODEFRAG_FULL) {
			AG_SKIP_FULL++;
			continue;
		} else if (rc == NODEFRAG_LONGRUN) {
			AG_SKIP_LONG_RUN++;
			continue;
		} else if (rc != DEFRAG)	/* i/o error */
			return rc;
		AG_DEFRAG++;

		/*
		 *      compact the AG
		 */
		nBMapSet = (nBMapPerAG <= BMAPSET) ? nBMapPerAG : BMAPSET;
		rc = compactAG(AGNumber, AGBMapStart, AGBMapEnd, nBMapSet);
		if (rc) {
			return rc;	/* i/o error */
		}

		/* display AG statistics */
		jFYI(1, ("\tRunLength   AllocatedRun   FreeRun\n", AGNumber));
		for (i = 1; i < XMOVESIZE; i++) {
			if ((AGAllocRun[i] + AGFreeRun[i]) != 0) {
				jFYI(1, ("\t%9d   %12d   %7d\n", i, AGAllocRun[i], AGFreeRun[i]));
				FSAllocRun[i] += AGAllocRun[i];
				AGAllocRun[i] = 0;
				total_free_runs += AGFreeRun[i];
				FSFreeRun[i] += AGFreeRun[i];
				AGFreeRun[i] = 0;
			}
		}
		jFYI(1, ("\t%8d+   %12d   %7d\n", i, AGAllocRun[0], AGFreeRun[0]));
		FSAllocRun[0] += AGAllocRun[0];
		AGAllocRun[0] = 0;
		total_free_runs += AGFreeRun[0];
		FSFreeRun[0] += AGFreeRun[0];
		AGFreeRun[0] = 0;
	}			/* end of for-loop numags  */

	/*
	 *      partial last AG
	 *
	 * process the last AG, which may include virtual dmaps
	 * (all marked as allocated) padding to make it full size
	 * of AG, s.t. it is not very useful to examine level control
	 * pages to determine whether to defragment it or not:
	 * just go ahead and defragment it.
	 */
	if (partialAG) {
		AG_DEFRAG++;

		/* start and end block address of AG */
		AGStart = AGNumber << gcp->dn_agl2size;
		AGEnd = AGStart + partialAG - 1;

		/* note: partialAG may not be a multiple of BPERDMAP,
		 * the last dmap may be partially belonged to fssize,
		 * in which case, the part of the last dmap that
		 * is beyond fssize is marked as allocated in mkfs time.
		 */
		nBMap = (partialAG + BPERDMAP - 1) >> L2BPERDMAP;
		AGBMapStart = nFullAG * nBMapPerAG;
		AGBMapEnd = AGBMapStart + nBMap - 1;

		jFYI(1, ("\t--- partial AG:%d AGStart:%lld AGEnd:%lld ---\n",
			 AGNumber, AGStart, AGEnd));

		/*
		 *      compact the AG
		 */
		nBMapSet = (nBMap <= BMAPSET) ? nBMap : BMAPSET;
		rc = compactAG(AGNumber, AGBMapStart, AGBMapEnd, nBMapSet);
		if (rc) {
			return rc;	/* i/o error */
		}

		/* display AG statistics */
		jFYI(1, ("\tRunLength   AllocatedRun   FreeRun\n", AGNumber));
		for (i = 1; i < XMOVESIZE; i++) {
			if ((AGAllocRun[i] + AGFreeRun[i]) != 0) {
				jFYI(1, ("\t%9d   %12d   %7d\n", i, AGAllocRun[i], AGFreeRun[i]));
				FSAllocRun[i] += AGAllocRun[i];
				total_free_runs += AGFreeRun[i];
				FSFreeRun[i] += AGFreeRun[i];
			}
		}

		jFYI(1, ("\t%8d+   %12d   %7d\n", i, AGAllocRun[0], AGFreeRun[0]));
		FSAllocRun[0] += AGAllocRun[0];
		total_free_runs += AGFreeRun[0];
		FSFreeRun[0] += AGFreeRun[0];
	}

	/* display FS statistics */
	jFYI(1, ("\t=== FS summary ===\n"));
	jFYI(1, ("\tRunLength   AllocatedRun   FreeRun\n"));
	for (i = 1; i < XMOVESIZE; i++) {
		if ((FSAllocRun[i] + FSFreeRun[i]) != 0)
			jFYI(1, ("\t%9d   %12d   %7d\n", i, FSAllocRun[i], FSFreeRun[i]));
	}

	jFYI(1, ("\t%8d+   %12d   %7d\n", i, FSAllocRun[0], FSFreeRun[0]));

#ifdef _JFS_OS2
	_itoa(nAG, msgstr, 10);
	msg_parms[0] = msgstr;
	message_user(MSG_JFS_TOTAL_AGS, msg_parms, 1, JFS_MSG);
#endif

	if (AG_SKIP_FREE) {

#ifdef _JFS_OS2
		_itoa(AG_SKIP_FREE, msgstr, 10);
		msg_parms[0] = msgstr;
		message_user(MSG_JFS_SKIPPED_FREE, msg_parms, 1, JFS_MSG);
#endif
	}

	if (AG_SKIP_FULL) {
#ifdef _JFS_OS2
		_itoa(AG_SKIP_FULL, msgstr, 10);
		msg_parms[0] = msgstr;
		message_user(MSG_JFS_SKIPPED_FULL, msg_parms, 1, JFS_MSG);
#endif
	}

	if (AG_SKIP_LONG_RUN) {

#ifdef _JFS_OS2
		_itoa(AG_SKIP_LONG_RUN, msgstr, 10);
		msg_parms[0] = msgstr;
		message_user(MSG_JFS_SKIPPED_CONTIG, msg_parms, 1, JFS_MSG);
#endif
	}
#ifdef _JFS_OS2
	_itoa(AG_DEFRAG, msgstr, 10);
	msg_parms[0] = msgstr;
#endif

	if (DoIt)
		message_user(MSG_JFS_NUM_DEFRAGED, msg_parms, 1, JFS_MSG);
	else {
		message_user(MSG_JFS_NUM_CANDIDATE, msg_parms, 1, JFS_MSG);
		if (AG_DEFRAG) {

#ifdef _JFS_OS2
			_itoa(total_free_runs / AG_DEFRAG, msgstr, 10);
#endif
			msg_parms[0] = msgstr;
			message_user(MSG_JFS_AVG_FREE_RUNS, msg_parms, 1, JFS_MSG);
		}
	}

	return rc;
}

/*
 * NAME:        examineAG()
 *
 * FUNCTION:    heuristics to determine whether to defragment
 *		the specified allocation group;
 */
static int examineAG(int32_t agno,	/* AG number */
		     struct dmapctl *lcp,	/* level control page covering AG */
		     int32_t * AGBMapStart,	/* start bitmap apge number of AG */
		     int32_t * AGBMapEnd)
{				/* end bitmap page number of AG */
	struct dbmap *gcp;
	int32_t agstart;
	int32_t freerate, f, r;
	int32_t f0, f1, f2;

	jEVENT(0, ("examineAG: agno:%d blockRange:%lld:%lld bmapRange:%d:%d\n",
		   agno, AGStart, AGEnd, *AGBMapStart, *AGBMapEnd));

	gcp = (struct dbmap *) & BMap.ctl;

	/* start node index of agwidth in dcp */
	agstart = gcp->dn_agstart + gcp->dn_agwidth * (agno & (nAGPerLCP - 1));

	/* whole AG is free! */
	if (lcp->stree[agstart] >= gcp->dn_agl2size) {
		return NODEFRAG_FREE;
	}

	if (lcp->stree[agstart] == -1) {
		/* This is either entirely free or entirely allocated */
		if (gcp->dn_agfree[agno])
			return NODEFRAG_FREE;
		else
			return NODEFRAG_FULL;
	}

	/* compute % of free blocks */
	freerate = (gcp->dn_agfree[agno] * 100) / gcp->dn_agsize;

	jEVENT(0, ("examineAG: nfree:%lld freerate:%d\n", gcp->dn_agfree[agno], freerate));
	jEVENT(0, ("examineAG: lcp node:%d maxfreebuddy:%d\n", agstart, lcp->stree[agstart]));

	/* too few free blocks */
	if (freerate < MIN_FREERATE) {
		return NODEFRAG_FULL;
	}

	/*
	 * determine max (budmax) and its next two successive split
	 * buddy size (budmin1 and budmin2) for the given free rate;
	 * skip defragment operation if the free blocks of AG are
	 * covered by sufficiently large contiguous extents;
	 *  free rate(%)        f0(%)   f1(%)   f2(%)
	 *  50 - 99             50      25      12
	 *  25 - 49             25      12      6
	 *  12 - 24             12      6       3
	 *  6  - 12             6       3       1
	 *  3  - 6              3       1       0
	 */
	f0 = gcp->dn_agl2size;
	r = 100;
	f = freerate;
	while (f < r) {
		r /= 2;
		f0--;
	}
	f -= r;

	r /= 2;
	if (f >= r) {
		f -= r;
		f1 = f0 - 1;
	} else
		f1 = -1;

	r /= 2;
	if (f >= r) {
		f -= r;
		f2 = f0 - 2;
	} else
		f2 = -1;

	if (f1 == -1 && f2 != -1) {
		f1 = f2;
		f2 = -1;
	}

	if (gcp->dn_agwidth == 2)
		goto agw2;

	/*
	 *      gcp->dn_agwidth == 1
	 */

	if (lcp->stree[agstart] < f0)
		return DEFRAG;

	/* lcp->stree[agstart] == f0 */

	/* are majority of free blocks contiguous ? */
	if (f1 == -1 && f2 == -1) {
		return NODEFRAG_LONGRUN;
	}

	/* f1 != -1 && (f2 != -1 || f2 == -1) */

	/* check stree whether f2, f1 and f0 are contiguous */
	return DEFRAG;

	/*
	 *      gcp->dn_agwidth == 2
	 */
      agw2:
	if ((lcp->stree[agstart] < f0 - 1) && (lcp->stree[agstart + 1] < f0 - 1))
		return DEFRAG;

	/* lcp->stree[agstart] >= f0-1 || lcp->stree[agstart+1] >= f0-1 */

	/* are majority of free blocks contiguous ? */
	if (f1 == -1 && f2 == -1) {
		return NODEFRAG_LONGRUN;
	}

	return DEFRAG;
}

/*
 * NAME:	compactAG()
 *
 * FUNCTION:	compact allocated extents per BMapSet/AG;
 *		for each BMapSet, scan IMAP for IAGs associated
 *		with the current AG and glean allocated extents
 *		residing in the BMapSet to build ixadTbl set, and
 *		for each ixadTbl set, scan BMapSet for free extents
 *		to compact from the allocated extents in ixadTbl set;
 */
static int compactAG(int32_t agno, int32_t AGBMapStart, int32_t AGBMapEnd, int32_t nBMapSet)
{
	int rc, rcx = 0;
	int32_t i, j, k, n;
	struct dmap *bmp;

	BMapPageNumber = AGBMapStart;
	jEVENT(0, ("compactAG: agno:%d bmapRange:%d:%d\n", agno, AGBMapStart, AGBMapEnd));

	/*
	 * process bitmaps of AG in set of nBMapSet at a time
	 */
	for (i = AGBMapStart; i <= AGBMapEnd; i += n) {
		/* read nBMapSet number of dmaps into buffer */
		n = MIN(AGBMapEnd - i + 1, nBMapSet);
		if ((rc = readBMap(i, n, BMapSet))) {
			return rc;	/* i/o error */
		}

		/* at the beginning of this BMapSet,
		 * skip bitmaps if it is all allocated or all free
		 */
		for (j = 0; j < n; j++, BMapPageNumber++) {
			bmp = (struct dmap *) & BMapSet[j];
			jEVENT(0, ("compactAG: bmap:%d nfree:%d maxfreebuddy:%d\n",
				   BMapPageNumber, bmp->nfree, bmp->tree.stree[0]));

			/* if the dmap is all allocated, skip it */
			if (bmp->nfree == 0)
				continue;
			/* if the dmap is all free, skip it */
			else if (bmp->nfree == BPERDMAP)
				continue;
			else
				break;
		}
		/* any bitmap left to compact ? */
		if (j == n)
			continue;

		/*
		 * build movable extent table (ixadTbl)
		 *
		 * select movable extents of objects (inodes)
		 * from iag's associated with current AG,
		 * which resides in the region covered by
		 * the bitmaps in BMapSet;
		 */
		barrierStart = (i + j) << L2BPERDMAP;
		barrierEnd = ((i + n) << L2BPERDMAP) - 1;
	      build_ixadtbl:
		if (DoIt == false)
			goto scan_bmap;

		rc = buildIxadTbl(agno, barrierStart, barrierEnd);
		if (rc != 0) {
			/* suspended from ixadTbl full ? */
			if (rc == ERROR_NOT_ENOUGH_MEMORY)
				/* remember to resume */
				rcx = rc;
			else {
				return rc;	/* i/o error */
			}
		}

		/*
		 * reorganize each dmap in current dmap set
		 */
	      scan_bmap:
		for (k = j; k < n; k++) {
			bmp = (struct dmap *) & BMapSet[k];
			BMapPageNumber = i + k;
			jEVENT(0, ("compactAG: bmap:%d nfree:%d maxfreebuddy:%d\n",
				   BMapPageNumber, bmp->nfree, bmp->tree.stree[0]));

			/* if the dmap is all allocated, skip it */
			if (bmp->nfree == 0)
				continue;
			/* if the dmap is all free, skip it */
			else if (bmp->nfree == BPERDMAP)
				continue;

			/*
			 * compact region covered by dmap
			 */
			if ((rc = compactBMap(i, k))) {
				if (rc == ERROR_FILE_NOT_FOUND)
					break;	/* ixadTbl empty */
				else
					return rc;	/* i/o error */
			}
		}

		/* more iags/objects to scan in AG ? */
		if (rcx == ERROR_NOT_ENOUGH_MEMORY) {
			rcx = 0;
			goto build_ixadtbl;
		}

		/* purge ixadTbl */
		purgeIxadTbl();
	}			/* end of for loop of BMapSet of AG */

	return 0;
}

/*
 * NAME:        buildIxadTbl()
 *
 * FUNCTION:    start/resume to build ixad table with size of XMOVESIZE.
 *		The index of the table is the size of extent starting
 *		from one aggr. blk up to XMOVESIZE aggr. blks.
 *		extents have size bigger than XMOVESIZE aggr. blks
 *		will not be moved.
 *		The table is built up by all the data extents address
 *		whose inode is belonged to the current dmap process group
 *		which starts from start_dmap ends with end_dmap.
 *
 * note: buildIxadTbl() and its subroutines acts like co-routine
 * with compactAG(), i.e., they may suspend and resume with persistent
 * state variables cross function calls;
 */
static int buildIxadTbl(int32_t agno, int64_t barrierStart,	/* start blk number of region */
			int64_t barrierEnd)
{				/* end blk number of region */
	int rc;
	struct dbmap *gcp;
	struct dinomap *icp;
	struct dinode *dip;
	int64_t xaddr;
	int32_t xlen;
	int32_t i, j;

	jEVENT(0, ("buildIxadTbl: agno:%d region:%lld:%lld\n", agno, barrierStart, barrierEnd));

	gcp = (struct dbmap *) & BMap.ctl;

	/*
	 * scan each IAG in the file set for IAG that belongs to AG
	 *
	 * for each call, resume from previous suspension point controlled by:
	 * IAGNumber, IXNumber, IXINumber (at the start of AG, all set at 0);
	 */
	icp = (struct dinomap *) & IMap.ctl;
	for (; IAGNumber < icp->in_nextiag; IAGNumber++) {
		/* If in the iagbuf still has some iag's that left over,
		 * then iagbuf.iagnum should be the same as IAGNumber
		 */
		if (iagbuf.iagnum != IAGNumber) {
			rc = readIMapSequential(&iagbuf);
			if (rc)
				return rc;	/* i/o error */

			/* for new IAG, reset IXNumber and IXINumber */
			IXNumber = IXINumber = 0;
		}

		jEVENT(0, ("buildIxadTbl: iagno:%d agstart:%lld nfree:%d\n",
			   iagbuf.iagnum, iagbuf.agstart, iagbuf.nfreeinos));

		/* is this IAG associated the current AG ? */
		if ((iagbuf.agstart >> gcp->dn_agl2size) != agno)
			continue;

		/*
		 * scan each inode extent covered by the IAG
		 */
		for (i = IXNumber; i < EXTSPERIAG; i++, IXNumber++) {
			/* if there is leftover inodes, read them first */
			if (IXINumber > 0)
				goto read_inoext;

			if (iagbuf.wmap[i] == 0)
				continue;

			/* read in inode extent (4 pages) */
			xaddr = addressPXD(&iagbuf.inoext[i]);
			xlen = lengthPXD(&iagbuf.inoext[i]);
			jEVENT(0,
			       ("buildIxadTbl: ixn:%d xaddr:%lld xlen:%d\n", IXNumber, xaddr,
				xlen));
			rc = pRead(fsMount, xaddr, xlen, (void *) ixbuf);
			if (rc != 0) {
				jERROR(1, ("buildIXadTbl: inode extent i/o error rc=%d\n", rc));
				return rc;	/* i/o error */
			}

			/*
			 * scan each inode in inode extent
			 */
		      read_inoext:
			for (j = IXINumber; j < INOSPEREXT; j++, IXINumber++) {
				if (ixbuf[j].di_nlink == 0 ||
				    ixbuf[j].di_inostamp != DIIMap.di_inostamp)
					continue;

				dip = &ixbuf[j];
				switch (dip->di_mode & IFMT) {
				case IFREG:
					rc = addXtree(dip, barrierStart, barrierEnd);
					break;
				case IFDIR:
					rc = addDtree(dip, barrierStart, barrierEnd);
					break;
				default:
					continue;
				}
				if (rc != 0) {
					/* either ENOMEM or EIO */
					return rc;
				}
			}	/* end-for inode extent scan */

			/* all inodes in this inoext have been processed */
			IXINumber = 0;
		}		/* end-for an IAG scan */

		/* all inode extents in this IAG have been processed */
		IXNumber = 0;
	}			/* end-for IMap scan */

	IAGNumber = 0;

	return 0;
}

/*
 * NAME:	addXtree()
 *
 * FUNCTION:	For a given inode of regular file,
 *		scan its xtree and enter its allocated extents
 *		into inode xad table;
 *		(xtree is scanned via its next sibling pointer);
 *
 * NOTE:	partial xtree may have been read previously:
 *		start from the previous stop point indicated by
 *		xtype, lmxaddr, pnext_xtpg and nxtpg_inx.
 *
 * RETURN:	0 -- ok
 *		ENOMEM -- No pre-allocated mem left.
 */
static int addXtree(struct dinode * dip, int64_t barrierStart, int64_t barrierEnd)
{
	int rc;
	xtpage_t *p;
	int32_t xlen, i;
	int64_t xaddr, xoff;
	ixad_t *ixad;

	/* start with root ? */
	if (pnext_xtpg == NULL) {
		pnext_xtpg = p = (xtpage_t *) & dip->di_btroot;
		if (p->header.flag & BT_LEAF) {
			xtype = DATAEXT;
		} else {
			xtype = XTPAGE;
			p->header.next = (int64_t) NULL;
			/* save leftmost child xtpage xaddr */
			lmxaddr = addressXAD(&p->xad[XTENTRYSTART]);
		}

		nxtpg_inx = XTENTRYSTART;
		jEVENT(0, ("addXtree: inumber:%d root xtype:%d n:%d\n",
			   dip->di_number, xtype, p->header.nextindex));
	} else {
		p = pnext_xtpg;
		/* xtype, lmxaddr, pnext_xtpg, nxtpg_inx */
		jEVENT(0, ("addXtree: inumber:%d xtype:%d\n", dip->di_number));
	}

	/*
	 * scan each level of xtree
	 */
	while (1) {
		/*
		 * scan each xtpage of current level of xtree
		 */
		while (1) {
			/*
			 * scan each xad in current xtpage
			 */
			for (i = nxtpg_inx; i < p->header.nextindex; i++) {
				/* test if extent is of interest */
				xoff = offsetXAD(&p->xad[i]);
				xaddr = addressXAD(&p->xad[i]);
				xlen = lengthXAD(&p->xad[i]);
				jEVENT(0, ("addXtree: inumber:%d xoff:%lld xaddr:%lld xlen:%d\n",
					   dip->di_number, xoff, xaddr, xlen));
				if (xlen >= XMOVESIZE)
					continue;

				if (xaddr < barrierStart)
					continue;
				if (xaddr + xlen > barrierEnd)
					continue;

				if ((ixad = allocIxad(&rc))) {
					ixad->flag = IFREG | xtype;
					ixad->fileset = dip->di_fileset;
					ixad->inostamp = dip->di_inostamp;
					ixad->inumber = dip->di_number;
					ixad->gen = dip->di_gen;
					ixad->f_xoff = xoff;
					ixad->xaddr = xaddr;
					insertIxad(xlen, ixad);
				} else {
					/* ixadTbl full */
					nxtpg_inx = i;
					return rc;	/* ENOMEM */
				}
			}	/* end for current xtpage scan */

			/* read in next/right sibling xtpage */
			if (p->header.next != (int64_t) NULL) {
				xaddr = p->header.next;
				rc = pRead(fsMount, xaddr, fsMount->nbperpage, &next_xtpg);
				if (rc != 0) {
					jERROR(1, ("addXtree: i/o error\n"));
					return rc;	/* i/o error */
				}

				pnext_xtpg = p = &next_xtpg;
				nxtpg_inx = XTENTRYSTART;
			} else
				break;
		}		/* end while current level scan */

		/*
		 * descend: read leftmost xtpage of next lower level of xtree
		 */
		if (xtype == XTPAGE) {
			/* get the leftmost child page  */
			rc = pRead(fsMount, lmxaddr, fsMount->nbperpage, &next_xtpg);
			if (rc != 0) {
				jERROR(1, ("addXtree: i/o error\n"));
				return rc;	/* i/o error */
			}

			pnext_xtpg = p = &next_xtpg;
			nxtpg_inx = XTENTRYSTART;
			if (p->header.flag & BT_LEAF)
				xtype = DATAEXT;
			else {
				xtype = XTPAGE;
				/* save leftmost child xtpage xaddr */
				lmxaddr = addressXAD(&p->xad[XTENTRYSTART]);
			}
		} else
			break;
	}			/* end while level scan */

	/* this inode is done: reset variables */
	pnext_xtpg = NULL;

	return 0;
}

/*
 * NAME:	addDtree()
 *
 * FUNCTION:	For a given inode of directory,
 *		go down to the dtree. insert the internal
 *		and leaf nodes into the ixadTbl.
 *
 * NOTE:	partial dtree may have been read previously. We have
 *		to start from the previous stop point indicated by
 *		next_dtpg and ndtpg_inx.
 */
static int addDtree(struct dinode * dip, int64_t barrierStart, int64_t barrierEnd)
{
	int rc = 0;
	dtpage_t *p;
	int8_t *stbl;
	int32_t i;
	pxd_t *pxd;
	int64_t xaddr, lmxaddr = 0;
	int32_t xlen;
	ixad_t *ixad;

	/* start with root ? */
	if (pnext_dtpg == NULL) {
		pnext_dtpg = p = (dtpage_t *) & dip->di_btroot;

		/* is it leaf, i.e., inode inline data ? */
		if (p->header.flag & BT_LEAF) {
			jEVENT(0, ("addDtree: inumber:%d root leaf\n", dip->di_number));
			goto out;
		}

		p->header.next = (int64_t) NULL;
		/* save leftmost dtpage xaddr */
		lmpg_addr = 0;

		stbl = DT_GETSTBL(p);
		pxd = (pxd_t *) & p->slot[stbl[0]];
		/* save leftmost child dtpage extent */
		lmxaddr = addressPXD(pxd);	/* leftmost child xaddr */
		lmxlen = lengthPXD(pxd);
		ndtpg_inx = 0;
		jEVENT(0, ("addDtree: inumber:%d root lmxaddr:%lld lmxlen:%d\n",
			   dip->di_number, lmxaddr, lmxlen));
	} else {
		p = pnext_dtpg;
		/* lmpg_addr, lmxaddr, pnext_dtpg, ndtpg_inx */
		jEVENT(0, ("addDtree: inumber:%d\n", dip->di_number));
	}

	/*
	 * scan each level of dtree
	 */
	while (1) {
		/*
		 * scan each dtpage of current level of dtree
		 */
		while (1) {
			stbl = DT_GETSTBL(p);

			/*
			 * scan each idtentry in current dtpage
			 */
			for (i = ndtpg_inx; i < p->header.nextindex; i++) {
				pxd = (pxd_t *) & p->slot[stbl[i]];

				/* test if extent is of interest */
				xaddr = addressPXD(pxd);
				xlen = lengthPXD(pxd);
				jEVENT(0, ("addDtree: inumber:%d xaddr:%lld xlen:%d\n",
					   dip->di_number, xaddr, xlen));
				if (xaddr < barrierStart)
					continue;
				if (xaddr + xlen > barrierEnd)
					continue;

				if ((ixad = allocIxad(&rc))) {
					ixad->flag = IFDIR | DTPAGE;
					ixad->fileset = dip->di_fileset;
					ixad->inostamp = dip->di_inostamp;
					ixad->inumber = dip->di_number;
					ixad->gen = dip->di_gen;
					ixad->d_lmxaddr = lmpg_addr;
					ixad->xaddr = xaddr;
					insertIxad(xlen, ixad);
				} else {
					/* ixadTbl full */
					ndtpg_inx = i;
					return rc;	/* ENOMEM */
				}
			}	/* end for loop */

			/* read in next/right sibling dtpage */
			if (p->header.next != (int64_t) NULL) {
				xaddr = p->header.next;
				rc = pRead(fsMount, xaddr, fsMount->nbperpage, &next_dtpg);
				if (rc != 0) {
					jERROR(1, ("addDtree: i/o error.\n"));
					return rc;	/* i/o error */
				}

				pnext_dtpg = p = &next_dtpg;
				ndtpg_inx = 0;
			} else
				break;
		}		/* end while current level scan */

		/*
		 * descend: read leftmost dtpage of next lower level of dtree
		 */
		/* the first child of the dtroot split may not have PSIZE */
		rc = pRead(fsMount, lmxaddr, lmxlen, &next_dtpg);
		if (rc != 0) {
			jERROR(1, ("addDtree: i/o error.\n"));
			return rc;	/* i/o error */
		}

		pnext_dtpg = p = &next_dtpg;

		/* for dir, the leaf contains data, its pxd info
		 * has been reported by the parent page. so we stop here
		 */
		if (p->header.flag & BT_LEAF)
			break;

		/* save leftmost dtpage xaddr */
		lmpg_addr = lmxaddr;

		stbl = DT_GETSTBL(p);
		pxd = (pxd_t *) & p->slot[stbl[0]];
		/* save leftmost child dtpage extent */
		lmxaddr = addressPXD(pxd);	/* leftmost child xaddr */
		lmxlen = lengthPXD(pxd);
		ndtpg_inx = 0;
	}

	/* reset global state variable for the inode */
      out:
	pnext_dtpg = NULL;

	return rc;
}

/*
 * NAME:	allocIxad()
 *
 * FUNCTION:	allocate a free inodexaddr structure from the pre-allocated
 *		pool.
 *
 * RETURN:	rc = 0 -- ok
 *               ENOMEM --  the pool is empty.
 *               other error -- system error
 */
static ixad_t *allocIxad(int *rc)
{
	ixad_t *ixad;
	int32_t n;

	if (ixadFreelist == NULL) {
		n = MIN(nIxadPool, 1024);
		if (n == 0) {
			*rc = ERROR_NOT_ENOUGH_MEMORY;
			return NULL;
		} else
			nIxadPool -= n;

		ixadFreelist = ixadHWM;

		/* init next 1024 entry */
		for (ixad = ixadFreelist; ixad < ixadFreelist + n - 1; ixad++)
			ixad->next = ixad + 1;
		ixad->next = NULL;

		ixadHWM = ++ixad;
	}

	ixad = ixadFreelist;
	ixadFreelist = ixadFreelist->next;
	nIxad++;

	*rc = 0;
	return ixad;
}

#define freeIxad(PTR)\
{\
	(PTR)->next = ixadFreelist;\
	ixadFreelist = (PTR);\
	nIxad--;\
}

/*
 * NAME:	insertIxad()
 *
 * FUNCTION:	insert ixad entry into the ixad table
 *		in ascending order of ixad xaddr.
 *
 * todo: may need do better than simple insertion sort;
 */
static void insertIxad(int32_t nblks,	/* number of aggr. blks for the target extent */
		       ixad_t * ixad)
{				/* addr of pointer to inodexaddr */
	ixad_t *tmp, *prev;

	/* empty list ? */
	if (ixadTbl[nblks].head == NULL) {
		ixadTbl[nblks].head = ixad;
		ixad->next = NULL;
		if (nblks <= 32)
			inoext_vector[0] |= UZBIT >> (nblks - 1);
		else
			inoext_vector[1] |= UZBIT >> ((nblks & (0x1F)) - 1);
	} else {
		prev = (ixad_t *) & ixadTbl[nblks].head;
		tmp = ixadTbl[nblks].head;
		while (tmp) {
			if (tmp->xaddr > ixad->xaddr) {
				ixad->next = tmp;
				prev->next = ixad;
				break;
			} else {
				prev = tmp;
				tmp = tmp->next;
			}
		}

		/* end of list ? */
		if (!tmp) {
			prev->next = ixad;
			ixad->next = NULL;
		}
	}

	ixadTbl[nblks].count++;
}

/*
 *	washIxadTbl()
 */

#ifdef _JFS_OS2
static void washIxadTbl(int64_t barrier)
{
	ixad_t *ixad, *head;
	int32_t i;

	for (i = 1; i < XMOVESIZE; i++) {
		if (ixadTbl[i].count == 0)
			continue;

		ixad = ixadTbl[i].head;

		/* free ixads below watermark */
		while (ixad != NULL && ixad->xaddr < barrier) {
			head = ixad;
			ixad = ixad->next;
			ixadTbl[i].count--;
			freeIxad(head);
		}

		ixadTbl[i].head = ixad;
	}
}
#endif

/*
 *	purgeIxadTbl()
 */
static void purgeIxadTbl(void)
{
	ixad_t *ixad, *head;
	int i;

	for (i = 1; i < XMOVESIZE; i++) {
		if (ixadTbl[i].count == 0)
			continue;

		ixad = ixadTbl[i].head;

		/* free ixads below watermark */
		while (ixad != NULL) {
			head = ixad;
			ixad = ixad->next;
			freeIxad(head);
		}

		ixadTbl[i].count = 0;
		ixadTbl[i].head = NULL;
	}

	nIxad = 0;
}

/*
 *	compactBMap()
 */
static int compactBMap(int32_t BMapSetBMapStart,	/* current bitmap # in BMapSet[0] */
		       int32_t bmx)
{				/* current bitmap index in BMapSet */
	int rc = 0;
	uint32_t x32, *wmap;
	int32_t run, w, b0, b, n, i;
	uint32_t mask;
	int64_t xaddr0, xaddr;
	struct dmap *bmp;
	int32_t bmn, agn;

	agn = BMapSetBMapStart >> nBMapPerAG;

	bmn = BMapSetBMapStart + bmx;
	bmp = &BMapSet[bmx];
	jEVENT(0, ("compactBMap: bmap:%d\n", bmn));

	xaddr0 = bmn << L2BPERDMAP;
	wmap = bmp->wmap;

	b0 = 0;			/* start address of run in the dmap */
	b = b0;			/* address cursor */
	n = 0;			/* length of current run */

	/* determinr start alloc/free run */
	if (*wmap & 0x80000000)
		run = 1;	/* alloc */
	else
		run = 0;	/* free */

	/* scan each bit map word left to right */
	for (w = 0; w < LPERDMAP; w++, wmap++) {
		x32 = *wmap;
		i = 32;		/* bit in word counter */
		mask = 0x80000000;	/* bit mask cursor */
		if (run == 0)
			goto free;

		/*
		 * scan alloc run
		 */
	      alloc:
		if (x32 == 0xffffffff) {
			b += 32;
			n += 32;
			continue;
		}

		while (i > 0 && (x32 & mask)) {
			mask >>= 1;
			b++;
			n++;
			i--;
		}

		if (i) {
			if (n < XMOVESIZE)
				AGAllocRun[n]++;
			else
				AGAllocRun[0]++;

			/* alloc run stopped - switch to free run */
			run = 0;	/* free */
			b0 = b;
			n = 0;
			goto free;
		} else
			continue;

		/*
		 * scan free run
		 */
	      free:
		if (x32 == 0) {
			b += 32;
			n += 32;
			continue;
		}

		while (i > 0 && !(x32 & mask)) {
			mask >>= 1;
			b++;
			n++;
			i--;
		}

		if (i) {
			if (n < XMOVESIZE)
				AGFreeRun[n]++;
			else
				AGFreeRun[0]++;

			if (DoIt == true) {
				/* free run stopped - try to move extent into it */
				xaddr = xaddr0 + b0;
				rc = moveExtent(BMapSetBMapStart, xaddr, n);
				if (rc) {
					return rc;	/* ixadTbl empty or i/o error */
				}
#ifdef	_JFS_DEBUG
#ifdef	_DEFRAGFS_DEBUG
				if (!more())
					return rc;
#endif				/* _DEFRAGFS_DEBUG */
#endif				/* _JFS_DEBUG */
			}

			/* switch to alloc run */
			run = 1;	/* alloc */
			n = 0;
			goto alloc;
		} else
			continue;
	}

	/* process last run */
	if (run == 1) {
		if (n < XMOVESIZE)
			AGAllocRun[n]++;
		else
			AGAllocRun[0]++;
	} else {
		if (n < XMOVESIZE)
			AGFreeRun[n]++;
		else
			AGFreeRun[0]++;

		/* try to move extent into it */
		if (DoIt == true) {
			xaddr = xaddr0 + b0;
			rc = moveExtent(BMapSetBMapStart, xaddr, n);
		}
	}

	return rc;
}

/*
 * NAME:	moveExtent()
 *
 * FUNCTION:	Move extent(s) into the specified free extent
 *		(blkno, len), and
 *		update the wmap in the dmap set for the moved extent
 *		if this extent is within the current dmap set
 *
 * TODO: error handling
 */
static int moveExtent(int32_t BMapSetBMapStart,	/* the dmap number in the BMapSet[0]  */
		      int64_t fxaddr,	/* the beginning blkno of the free space */
		      int32_t fxlen)
{				/* the number of blks of the free space */
	int rc = 0;
	ixad_t *ixad = NULL, *head;
	int64_t xaddr;
	int32_t i;

	jEVENT(0, ("moveExtent: xaddr:%lld xlen:%d\n", fxaddr, fxlen));

	/* update the global blk address Barrier:
	 * extents before it should not be moved.
	 */
	barrier = fxaddr;

	/*
	 * move allocated extent(s) to the free extent;
	 */
	while (fxlen) {
		/* select best-fit ixad to move */
		i = MIN(fxlen, XMOVESIZE - 1);
		for (; i > 0; i--)
			if (ixadTbl[i].head != NULL)
				break;
		if (i > 0)
			ixad = ixadTbl[i].head;
		else if (nIxad == 0)
			return ERROR_FILE_NOT_FOUND;	/* ixadTbl empty */
		else
			return 0;	/* no movalble extent */

		/* free ixads below watermark */
		while (ixad != NULL && ixad->xaddr < barrier) {
			/* free ixad */
			head = ixad;
			ixad = ixad->next;
			ixadTbl[i].head = ixad;
			ixadTbl[i].count--;
			freeIxad(head);
		}
		if (ixad == NULL)
			continue;
		else {
			/* request fs to relocate the extent */
			rc = fscntlMove(fxaddr, i, ixad);
			if (rc == 0) {
				/* update the wmap in the dmap set for
				 * the moved extent IF this extent is
				 * within the current dmap set;
				 */
				xaddr = ixad->xaddr;
/*
				if ((xaddr + i -1 ) <= BMapSetEnd)
					dbFreeBMap(BMapSetBMapStart, xaddr, i);
				else if (xaddr <= BMapSetEnd)
					 dbFreeBMap(BMapSetBMapStart, xaddr, BMapSetEnd - xaddr + 1);
*/
				dbFreeBMap(BMapSetBMapStart, xaddr, i);

				/* remove ixad */
				head = ixad;
				ixad = ixad->next;
				ixadTbl[i].head = ixad;
				ixadTbl[i].count--;
				freeIxad(head);

				/* advance Barrier */
				barrier += i;
				fxaddr = barrier;
				fxlen -= i;
			} else {
				/* stale source extent ? */
				if ((rc = ERROR_WRITE_FAULT)) {
					/* remove ixad */
					head = ixad;
					ixad = ixad->next;
					ixadTbl[i].head = ixad;
					ixadTbl[i].count--;
					freeIxad(head);
				}
				/* stale destination extent ? */
				else if ((rc = ERROR_DISK_FULL)) {
					/* advance Barrier */
					barrier += i;
					fxaddr = barrier;
					fxlen -= i;
				} else {	/* i/o error */

					return rc;	/* i/o error */
				}
			}
		}
	}			/* end while */

	return 0;
}

/*
 * NAME:	fscntlMove()
 *
 * FUNCTION:  attempts to move the number of len logical blks
 *    specified by ixad to the extent starting at blkno.
 */
static int fscntlMove(int64_t nxaddr, int32_t xlen, ixad_t * ixad)
{
	int rc = 0;

#ifdef _JFS_OS2
	int32_t pList;
	unsigned long pListLen = 0;
#endif

	defragfs_t pData;
	unsigned long pDataLen = 0;

	pDataLen = sizeof (defragfs_t);

	/*
	 * move extent
	 */
	pData.flag = ixad->flag | DEFRAGFS_RELOCATE;
	pData.dev = lvMount->LVNumber;
	pData.fileset = ixad->fileset;
	pData.inostamp = ixad->inostamp;
	pData.ino = ixad->inumber;
	pData.gen = ixad->gen;
	pData.xoff = ixad->f_xoff;
	pData.old_xaddr = ixad->xaddr;
	pData.new_xaddr = nxaddr;
	pData.xlen = xlen;
	jEVENT(0, ("defragfs i:%d xoff:%lld xlen:%d xaddr:%lld:%lld\n",
		   pData.ino, pData.xoff, pData.xlen, pData.old_xaddr, pData.new_xaddr));

#ifdef _JFS_OS2
	rc = fscntl(JFSCTL_DEFRAGFS, (void *) &pList, &pListLen, (void *) &pData, &pDataLen);
#endif

	if (rc != 0 && (rc != ERROR_WRITE_FAULT ||	/* source extent stale */
			rc != ERROR_DISK_FULL))	/* destination extent stale */
		rc = ERROR_READ_FAULT;	/* i/o error */

	return rc;
}

/*
 * NAME:	dbFreeBMap()
 *
 * FUNCTION:	update the wmap in the current dmap set.
 *		starting from blkno in the wmap, mark nblocks as free .
 */
static void dbFreeBMap(int32_t BMapSetBMapStart,	/* the dmap number in the BMapSet[0] */
		       int64_t blkno,	/* starting block number to mark as free */
		       int32_t nblocks)
{				/* number of bits marked as free */
	struct dmap *bmp;		/* bitmap page */
	dmtree_t *tp;
	int32_t bmn, i;
	int32_t lzero, tzero;
	int32_t bit, word, rembits, nb, nwords, wbitno, nw;
	int8_t size, maxsize, budsize;
	uint32_t j;

	bmn = blkno >> L2BPERDMAP;
	i = bmn - BMapSetBMapStart;
	bmp = (struct dmap *) & BMapSet[i];
	tp = (dmtree_t *) & bmp->tree;

	/* determine the bit number and word within the dmap of the
	 * starting block.
	 */
	bit = blkno & (BPERDMAP - 1);
	word = bit >> L2DBWORD;

	/* block range better be within the dmap */
	assert(bit + nblocks <= BPERDMAP);

	/* free the bits of the dmaps words corresponding to the block range.
	 * if not all bits of the first and last words may be contained within
	 * the block range, work those words (i.e. partial first and/or last)
	 * on an individual basis (a single pass), freeing the bits of interest
	 * by hand and updating the leaf corresponding to the dmap word.
	 * a single pass will be used for all dmap words fully contained within
	 * the specified range. within this pass, the bits of all fully
	 * contained dmap words will be marked as free in a single shot and
	 * the leaves will be updated.
	 * a single leaf may describe the free space of multiple dmap words,
	 * so we may update only a subset of the actual leaves corresponding
	 * to the dmap words of the block range.
	 */
	for (rembits = nblocks; rembits > 0; rembits -= nb, bit += nb) {
		/* determine the bit number within the word and
		 * the number of bits within the word.
		 */
		wbitno = bit & (DBWORD - 1);
		nb = MIN(rembits, DBWORD - wbitno);

		/* is only part of a word to be freed ? */
		if (nb < DBWORD) {
			/* free (zero) the appropriate bits within this
			 * dmap word.
			 */
			bmp->wmap[word] &= ~(ONES << (DBWORD - nb) >> wbitno);
			word += 1;
		} else {
			/* one or more dmap words are fully contained
			 * within the block range.  determine number of
			 * words to be freed and free (zero) those words.
			 */
			nwords = rembits >> L2DBWORD;
			memset(&bmp->wmap[word], 0, nwords * 4);

			/* determine number of bits freed */
			nb = nwords << L2DBWORD;

			/* update the leaves to reflect the freed words */
			for (; nwords > 0; nwords -= nw) {
				/* determine the new value of leaf covering
				 * words freed from word as minimum of
				 * the l2 number of buddy size of bits being
				 * freed and
				 * the l2 (max) number of bits that can be
				 * described by this leaf.
				 */
				for (i = 0, j = 0x00000001; !(word & j) && i < 32; i++, j <<= 1) ;
				tzero = i;
				maxsize = ((word == 0) ? L2LPERDMAP : tzero) + BUDMIN;
				for (i = 0, j = 0x80000000; !(nwords & j) && i < 32; i++, j >>= 1) ;
				lzero = i;
				budsize = 31 - lzero + BUDMIN;
				size = MIN(maxsize, budsize);

				/* get the number of dmap words handled */
				nw = BUDSIZE(size, BUDMIN);
				word += nw;
			}
		}
	}

	/* update the free count for this dmap */
	bmp->nfree += nblocks;
}

/*
 *	preamble()
 */
static int preamble(void)
{
	int rc = 0;

#ifdef _JFS_OS2
	int32_t pList;
	unsigned long pListLen = 0;
#endif
	defragfs_t pData;
	unsigned long pDataLen = 0;
	buf_t *bp;
	struct dinode *dip;
	int32_t i;

	/*
	 * sync fs meta-data
	 */
	jEVENT(0, ("preamble: sync.\n"));
	pDataLen = sizeof (defragfs_t);
	pData.flag = DEFRAGFS_SYNC;
	pData.dev = lvMount->LVNumber;

#ifdef _JFS_OS2
	rc = fscntl(JFSCTL_DEFRAGFS, (void *) &pList, &pListLen, (void *) &pData, &pDataLen);
#endif

	if (rc) {
		msg_parms[0] = "Sync Failure";
		message_user(MSG_JFS_UNEXPECTED_ERROR, msg_parms, 1, JFS_MSG);
		return rc;
	}

	/*
	 * read in the block allocation map inode (i_number = 2)
	 */
	if ((rc = bRawRead(LVHandle, (int64_t) AITBL_OFF, PAGESIZE, &bp))) {
		jERROR(1, ("preamble: i/o error: rc=%d\n", rc));
		return rc;
	}

	/* locate the bmap inode in the buffer page */
	dip = (struct dinode *) bp->b_data;
	dip += BMAP_I;
	memcpy(diBMap, dip, DISIZE);

	bRelease(bp);

	/*
	 * read bmap control page(s)
	 */
	rc = readBMapGCP(bMap);
	if (rc != 0)
		return rc;

	/*
	 * read in the fileset inode allocation map inode (i_number = 16)
	 */
	i = FILESYSTEM_I / INOSPERPAGE;
	if ((rc = bRawRead(LVHandle, (int64_t) (AITBL_OFF + PAGESIZE * i), PAGESIZE, &bp))) {
		jERROR(1, ("preamble: i/o error: rc=%d\n", rc));
		return rc;
	}

	/* locate the inode in the buffer page */
	dip = (struct dinode *) bp->b_data;
	dip += FILESYSTEM_I & (INOSPERPAGE - 1);
	memcpy(diIMap, dip, DISIZE);

	bRelease(bp);

	/*
	 * read imap global control page
	 */
	rc = readIMapGCPSequential(iMap, &iagbuf);
	if (rc != 0)
		return rc;

	/*
	 * init statistics
	 */
	for (i = 0; i < XMOVESIZE; i++) {
		AGAllocRun[i] = 0;
		AGFreeRun[i] = 0;

		FSAllocRun[i] = 0;
		FSFreeRun[i] = 0;
	}

	AG_DEFRAG = 0;
	AG_SKIP_FREE = 0;
	AG_SKIP_FULL = 0;
	AG_SKIP_LONG_RUN = 0;
	total_free_runs = 0;

	return rc;
}

/*
 *	getProvisions()
 *
 * allocate resources
 */
static int getProvisions(void)
{
	int32_t nbytes, i;

	/*
	 * alloc/init the movable/relocatable extent table
	 */
	/* init list anchor table */
	for (i = 0; i < XMOVESIZE; i++) {
		ixadTbl[i].count = 0;
		ixadTbl[i].head = NULL;
	}

	/* allocate movable extent entry pool:
	 *
	 * each dmap monitors BPERDMAP blks.
	 * Assume one ixad_t entry per two blks.
	 */
	nBMapPerAG = fsMount->AGSize >> L2BPERDMAP;
	nBMapSet = (nBMapPerAG <= BMAPSET) ? nBMapPerAG : BMAPSET;

	nbytes = (nBMapSet << (L2BPERDMAP - 1)) * sizeof (ixad_t);
	nbytes = (nbytes + PAGESIZE - 1) & ~(PAGESIZE - 1);

	if ((ixadPool = (ixad_t *) malloc(nbytes)) == NULL) {
		message_user(MSG_OSO_NOT_ENOUGH_MEMORY, NULL, 0, JFS_MSG);
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	nIxadPool = nbytes / sizeof (ixad_t);
	ixadHWM = ixadPool;
	ixadFreelist = NULL;

	return 0;
}

/*
 *	whatDoYouWant()
 *
 * defrag  [-q] {device_name}
 *
 *  option:
 *	-q -	query. report current file system status.
 *		only the status is reported, nothing is done.
 *
 *	no option - do the defragmentation and generate the result report.
 *
 *  device_name - device under defragmentation.
 */
static int whatDoYouWant(int32_t argc, char **argv)
{
	int rc = DEFRAG_OK;
	int32_t i;
	char *argp;
	char *lvolume = NULL;
	FILE *file_p = NULL;

#ifdef NO_DRIVE_NEEDED
	char cwd[80];

	/* initialize the disk name to be the current drive */
	getcwd(cwd, 80);
	LVName[0] = cwd[0];
	LVName[1] = cwd[1];
	LVName[2] = '\0';
#endif

#ifdef _JFS_OS2
	if (argc < 2)
		return -1;

	/* parse each of the command line parameters */
	for (i = 1; i < argc && rc == 0; i++) {
		argp = argv[i];

		jEVENT(0, ("whatDoYouWant: arg:%s\n", argp));

		/* a switch ? */
		if (*argp == '/' || *argp == '-') {
			argp++;	/* increment past / or - */

			while (rc == 0 && *argp) {
				switch (toupper(*argp)) {
				case 'Q':	/* Query fs status only */
					DoIt = false;
					argp++;
					break;
				default:	/* unknown switch */
					rc = -1;
					goto out;
				}
			}

			if (*argp) {
				rc = -1;
				goto out;
			}

			continue;
		}

		/* a drive letter */
		if (isalpha(*argp) && argp[1] == ':' && !argp[2]) {
			/* only allow one drive letter to be specified */
			if (devFound) {
				rc = -1;
				break;
			} else {
				strcpy(LVName, argp);
				devFound = TRUE;
			}

			continue;
		}

		rc = -1;
		break;
	}			/* end for */

	if (!devFound)
		rc = -1;

      out:
	jEVENT(0, ("whatDoYouWant: rc=%d\n", rc));
	return (rc);
#endif

	for (i = 1; i < argc && rc == 0; i++) {	/* for all parms on command line */
		argp = argv[i];
		if (*argp == '-') {	/* leading - */
			argp++;
			if ((*argp == 'q') || (*argp == 'Q')) {	/* query fs status only */
				DoIt = false;
				argp++;
			}
			/* end query */
			else {	/* unrecognized parm */
				rc = DEFRAG_FAILED;
				printf("Unrecognized parameter %s\n", argp);
				return (DEFRAG_FAILED);
			}	/* end unrecognized parm */
		}
		/* end leading / or - */
		else if (*argp == '/') {	/* This should be the device to check */
			lvolume = argp;
			file_p = fopen(lvolume, "r");
			if (file_p) {
				fclose(file_p);
			} else {
				printf("Bad device name, or device can't be opened.\n");
				return (DEFRAG_FAILED);
			}

		}
		/* end device (possibly) given */
	}			/* end for (loop) all parms on command line */

	if (lvolume == NULL) {	/* no device specified */
		printf("No device specified.\n");
		return (DEFRAG_FAILED);

	} /* end no device specified */
	else {			/* got a device */
		LVName = lvolume;
	}			/* end got a device */

	return (rc);

}
