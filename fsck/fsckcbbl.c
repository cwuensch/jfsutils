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
/* defines and includes common among the fsck.jfs modules */
#include "xfsckint.h"

/* + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + +
 *
 * superblock buffer pointer
 *
 *      defined in xchkdsk.c
 */
extern struct superblock *sb_ptr;

/* + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + +
 *
 * fsck aggregate info structure pointer
 *
 *      defined in xchkdsk.c
 */
extern struct fsck_agg_record *agg_recptr;

/* + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + + +
 *
 * For message processing
 *
 *      defined in xchkdsk.c
 */
extern char *Vol_Label;

/* VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV
 *
 * The following are internal to this file
 *
 */

int exec_clrbdblks(void);

int examine_LVM_BadBlockLists(int32_t, int32_t *);

int get_LVM_BadBlockList_count(int32_t *);

/* VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV */

/*****************************************************************************
 * NAME: check_BdBlkLst_FillLevel
 *
 * FUNCTION:	If any of the LVM Bad Block Tables composing the LVM Bad Block
 *		List for this partition is more than 50% full, issue a message.
 *
 * PARAMETERS:  none
 *
 * NOTES:	This routine is called only during autocheck processing.  fsck
 *		    cannot perform /B processing at that time since the file
 *		    system is not yet fully initialized.
 *
 * RETURNS:
 *      success: FSCK_OK
 *      failure: something else
 */
int check_BdBlkLst_FillLevel()
{
	int ccbblfl_rc = FSCK_OK;
	int intermed_rc = 0;
	int32_t num_tables = 0;
	int32_t highest_percent_full = 0;

	intermed_rc = get_LVM_BadBlockList_count(&num_tables);

	if (intermed_rc == FSCK_OK) {
		intermed_rc =
		    examine_LVM_BadBlockLists(num_tables,
					      &highest_percent_full);
		if (intermed_rc == FSCK_OK) {
			if (highest_percent_full > 50) {
				fsck_send_msg(fsck_LVMFOUNDBDBLKS);
			}
		}
	}

	return (ccbblfl_rc);
}

/*****************************************************************************
 * NAME: ClrBdBlkLst_processing
 *
 * FUNCTION:  Invoke the JFS processing to clear the LVM's bad block list.
 *
 * PARAMETERS:  none
 *
 * NOTES:	Starts a child process for the utility.
 *
 * RETURNS:
 *      success: FSCK_OK
 *      failure: something else
 */
int ClrBdBlkLst_processing()
{
	int kcbbl_rc = FSCK_OK;
	int intermed_rc = FSCK_OK;
	struct fsckcbbl_record *cbblrecptr;
	int I_am_logredo = 0;

	cbblrecptr = &(agg_recptr->blkmp_ctlptr->cbblrec);
	if (cbblrecptr == NULL) {
		if (agg_recptr->blkmp_ctlptr == NULL) {
			intermed_rc =
			    alloc_wrksp(sizeof (struct fsck_blk_map_hdr),
					dynstg_blkmap_hdr, I_am_logredo,
					(void **) &(agg_recptr->blkmp_ctlptr)
			    );
			if (intermed_rc == FSCK_OK) {
				/* fill eyecatcher */
				strncpy(agg_recptr->blkmp_ctlptr->hdr.
					eyecatcher, fbmh_eyecatcher_string,
					strlen(fbmh_eyecatcher_string));
			}
		}
		intermed_rc = blkmap_get_ctl_page(agg_recptr->blkmp_ctlptr);
		if (intermed_rc == FSCK_OK) {
			cbblrecptr = &(agg_recptr->blkmp_ctlptr->cbblrec);
		}
	}
	if (cbblrecptr != NULL) {
		memcpy((void *) &(cbblrecptr->eyecatcher), (void *) "*unset**",
		       8);
		intermed_rc = blkmap_put_ctl_page(agg_recptr->blkmp_ctlptr);
	}

	fsck_send_msg(fsck_LVMFSNOWAVAIL);
	fsck_send_msg(fsck_LVMTRNSBBLKSTOJFS);

	/*
	 * close the file system so clrbblks can get it
	 */
	close_volume();

	kcbbl_rc = exec_clrbdblks();

	/*
	 * open the file system and get the clrbblks
	 * communication area
	 */
	open_volume(Vol_Label);
	intermed_rc = blkmap_get_ctl_page(agg_recptr->blkmp_ctlptr);
	if (intermed_rc == FSCK_OK) {
		if (memcmp
		    ((void *) &(cbblrecptr->eyecatcher), (void *) "*unset**",
		     8) != 0) {
			/*
			 * The eyecatcher field was reset.  there is good
			 * reason to believe that clrbblks processing did
			 * actually write to the record.
			 */
			fsck_send_msg(fsck_CLRBBLKSRC, cbblrecptr->cbbl_retcode);

			fsck_send_msg(fsck_CLRBBLVMNUMLISTS, cbblrecptr->LVM_lists);

			fsck_send_msg(fsck_CLRBBRANGE, (cbblrecptr->fs_last_metablk + 1),
				      (cbblrecptr->fs_first_wspblk - 1));

			fsck_send_msg(fsck_CLRBBACTIVITY, cbblrecptr->reloc_extents,
				     cbblrecptr->reloc_blocks);

			fsck_send_msg(fsck_CLRBBRESULTS, cbblrecptr->total_bad_blocks,
				      cbblrecptr->resolved_blocks);
		}
	}

	return (kcbbl_rc);
}
