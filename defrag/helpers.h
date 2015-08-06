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
#ifndef _H_UJFS_FSSUBS
#define _H_UJFS_FSSUBS

#define	PAGESIZE	4096
#define RC_OK       0

/*
 * logical volume parameter
 */
extern int LVHandle;
typedef struct {
	int64_t LVSize;		/* LV size in pbsize        */
	int32_t pbsize;		/* device block/sector size */
	int32_t l2pbsize;	/* log2 of pbsize           */
	uint8_t LVNumber;	/* vpfsi.vpi_drive          */
} LV_t;

extern LV_t LVMount;
extern LV_t *lvMount;

/*
 * file system parameter
 */
typedef struct {
	int64_t FSSize;		/* FS size in bsize              */

	int32_t bsize;		/* FS logical block size in byte */
	int32_t l2bsize;

	int32_t l2bfactor;	/* log2 (bsize/pbsize)           */

	int32_t nbperpage;
	int32_t l2nbperpage;

	int32_t AGSize;		/* FS AG size in bsize           */
	int32_t nAG;		/* number of AGs                 */

	int64_t LOGSize;	/* log size in bsize             */
} FS_t;

extern FS_t FSMount;
extern FS_t *fsMount;

/*
 * block allocation map
 */
extern struct dinode DIBMap;
extern struct dinode *diBMap;

typedef struct {
	struct dbmap ctl;
	struct dmapctl l2;
	struct dmapctl l1;
	struct dmapctl l0;
} BMap_t;
extern BMap_t BMap;
extern BMap_t *bMap;

/*
 * inode allocation map
 */
extern struct dinode DIIMap;
extern struct dinode *diIMap;

typedef struct {
	struct dinomap ctl;
} IMap_t;
extern IMap_t IMap;
extern IMap_t *iMap;

/* imap xtree sequential read control */
extern int32_t IAGNumber;
extern int64_t IMapXtreeLMLeaf;	/* imap xtree leftmost leaf xaddr */
extern xtpage_t IMapXtreeLeaf;	/* imap xtree leaf buffer */
typedef struct {
	xtpage_t *leaf;		/* current imap leaf xtpage under scan */
	int32_t index;		/* current xad entry index in iMapXtreeLeaf */
	int32_t page;		/* iag number to read within current imap extent */
} IMapXtree_t;
extern IMapXtree_t IMapXtree;

/*
 *	buffer pool
 */
typedef struct {
	char page[PAGESIZE];
} page_t;

typedef struct bufhdr {
	struct bufhdr *b_next;	/* 4: */
	char *b_data;		/* 4: */
} buf_t;			/* (8) */

/*
 * function prototypes
 */
int32_t openLV(char *LVName);
int32_t openFS(void);
void closeFS(void);
int32_t readBMapGCP(BMap_t * bMap);
int32_t readBMapLCP(int64_t bn, int32_t level, struct dmapctl *lcp);
int32_t readBMap(int64_t start_dmap, int32_t ndmaps, struct dmap * dmap);
int32_t readIMapGCPSequential(IMap_t * iMap, struct iag * iag);
int32_t readIMapSequential(struct iag * iag);
int32_t xtLookup(struct dinode * dip, int64_t xoff, int64_t * xaddr, int32_t * xlen, uint32_t flag);
int32_t xtLMLeaf(struct dinode * dip, xtpage_t * xpp);
int32_t fscntl(uint32_t cmd, void *pList, unsigned long *pListLen,
	       void *pData, unsigned long *pDataLen);
int32_t pRead(FS_t * fsMount, int64_t xaddr, int32_t xlen, void *p);
int32_t bRead(FS_t * fsMount, int64_t xaddr, int32_t xlen, buf_t ** bpp);
int32_t bRawRead(uint32_t LVHandle, int64_t off, int32_t len, buf_t ** bpp);
void bRelease(buf_t * bp);

#endif				/* _H_UJFS_FSSUBS */
