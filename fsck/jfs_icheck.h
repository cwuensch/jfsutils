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
 *   FUNCTION: Alter inodes in an mounted filesystem
 */

#include <stdio.h>

#include "jfs_types.h"
#include "jfs_dinode.h"
#include "jfs_imap.h"
#include "jfs_superblock.h"

/* Global Data */
extern unsigned type_jfs;
extern int bsize;
extern FILE *fp;
extern short l2bsize;
extern int64_t AIT_2nd_offset; /* Used by find_iag routines    */

/* Global Functions */
int jfs_icheck(char *device, char *filenames[], int NrFiles, int UseInodeNums, int DoFix);
int CheckInodeByName(char *device, char *filename, int64_t RealBlocks, int DoFix);
int CheckInodeByNr(char *device, unsigned InodeNr, int64_t RealBlocks, int DoFix);
