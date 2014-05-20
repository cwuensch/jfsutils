
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

#define _FILE_OFFSET_BITS 64
#define __USE_FILE_OFFSET64

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>

#include <linux/types.h>
#include <linux/fs.h>

#include "jfs_endian.h"
#include "jfs_logmgr.h"
#include "jfs_version.h"
#include "devices.h"
#include "inode.h"
#include "super.h"

#include "lib.h"
#include "jfs_icheck.h"

#define MY_VERSION  "0.1"
#define MY_DATE     "2014-05-16"

/* setting some bit of some byte can be so easy ;) */
#define is_set(x,v)	(((x)&(v)) == (v))
#define set(x,v)	((x) |=  (v))

/* Global Data */
unsigned type_jfs;
FILE *fp;			/* Used by libfs routines       */
int bsize;			/* aggregate block size         */
short l2bsize;			/* log2 of aggregate block size */
int64_t AIT_2nd_offset;		/* Used by find_iag routines    */

/* default values for our options */
int opt_fixinode = 0;
int opt_quiet = 0;

/* return value is an array of some bits */
#define RV_OKAY                  0x00	/* alle dateien ok */
#define RV_FILE_NOT_FOUND        0x01	/* mind. eine Datei wurde nicht gefunden */
#define RV_FILE_NEEDS_FIX        0x02	/* mind. eine Datei hat fehlerhaften Eintrag */
#define RV_FILE_WAS_FIXED        0x04	/* es gab fehlerhafte Einträge, die wurden aber korrigiert */
#define RV_FILE_CHECKING_FAILED  0x08	/* es gibt probleme beim herusfinden der block anzahl... */
#define RV_FILE_FIXING_FAILED    0x10	/* mind. eine Datei hat fehlerhaften Eintrag */
#define RV_DROP_CACHE_FAILED     0x20	/* cache auf pladde schreiben fehlgeschlagen */
int return_value = RV_OKAY;

/**
 * internal functions
 */
void usage(void);
void drop_caches(void);
void fix_inode(unsigned inum, unsigned used_blks);
void check_file(char *filename);

/**
 * show some info how we where called
 */
void usage()
{
	printf("\nUsage: jfs_icheck [options] <device> file1 file2 .. fileN\n"
	       " -f        fix the inode block information (default: off)\n"
	       " -q        enable quiet mode (default: off)\n"
	       " -h        show some help about usage\n");
	exit(0);
}

/**
 * 1 -> drop pagecache
 * 2 -> drop dentries and inodes
 * 3 -> drop pagecache, dentries and inodes
 */
void drop_caches()
{
	FILE *fp = fopen("/proc/sys/vm/drop_caches", "w");

	/* we are syncing only our wanted disk here... */
	sync();
	sync();

	/* open proc file */
	if (!fp) {
		perror
		    ("fopen /proc/sys/vm/drop_caches failed, can't flush disk!");
		set(return_value, RV_DROP_CACHE_FAILED);
		return;
	}

	/* drop all caches */
	if (fprintf(fp, "3\n") == -1) {
		perror("echo 3 > /proc/sys/vm/drop_caches failed!");
		set(return_value, RV_DROP_CACHE_FAILED);
		return;
	}

	fclose(fp);
}

/**
 * fix some data on inode
 */
void fix_inode(unsigned inum, unsigned used_blks)
{
	int64_t address;
	struct dinode inode;

	if (find_inode(inum, FILESYSTEM_I, &address)) {
		fprintf(stderr, "Can't find inode %u!\n", inum);
		set(return_value, RV_FILE_FIXING_FAILED);
		return;
	}

	/* read it */
	if (xRead(address, sizeof(struct dinode), (char *)&inode)) {
		fprintf(stderr, "Error reading inode %u\n", inum);
		set(return_value, RV_FILE_FIXING_FAILED);
		return;
	}

	/* swap if on big endian machine */
	ujfs_swap_dinode(&inode, GET, type_jfs);

	/* fix nblocks value */
	inode.di_nblocks = used_blks;

	/* swap if on big endian machine */
	ujfs_swap_dinode(&inode, PUT, type_jfs);

	/* write it */
	if (xWrite(address, sizeof(struct dinode), (char *)&inode)) {
		set(return_value, RV_FILE_FIXING_FAILED);
		return;
	}

	set(return_value, RV_FILE_WAS_FIXED);
	return;
}

/**
 * check some file
 * 1) get valid fileblocks via FIBMAP
 * 2) check if the value in the inode matches
 */
void check_file(char *filename)
{
	struct stat st;
	long long unsigned size, cur_blks, used_blks, total_blks, blk;
	unsigned ino, blknum;
	int fd;

	/* open the file */
	fd = open(filename, O_RDONLY);
	if (fd == -1) {
		fprintf(stderr, "File not found: \"%s\"\n", filename);
		fflush(stderr);
		set(return_value, RV_FILE_NOT_FOUND);
		return;
	}
	fsync(fd);		/* just again ;) */

	if (fstat(fd, &st) == -1) {
		perror(filename);
		set(return_value, RV_FILE_NOT_FOUND);
		goto out;
	}

	/* check more blocks, cause JFS can split files across AG's */
	#define SOME_MORE_BLOCKS 1024 * 4

	total_blks = (st.st_size + st.st_blksize - 1) / st.st_blksize + SOME_MORE_BLOCKS;
	for (used_blks = 0, blk = 0; blk < total_blks; blk++) {
		blknum = blk;	/* FIBMAP ist nur 32bit ... */
		if (ioctl(fd, FIBMAP, &blknum) == -1) {
			perror("ioctl(FIBMAP)");
			set(return_value, RV_FILE_NOT_FOUND);
			goto out;
		}

		if (blknum != 0)
			used_blks++;
	}

	ino = (unsigned)st.st_ino;
	size = (long long unsigned)st.st_size;
	cur_blks = (long long unsigned)st.st_blocks / 8;

	if (!opt_quiet) {
		if (cur_blks == used_blks) {
			/* good */
			printf("ok: %s[i=%u] size=%llu blocks=%llu\n",
			       filename, ino, size, cur_blks);
			fflush(stdout);
		} else {
			/* wrong */
			printf
			    ("??: %s[i=%u] size=%llu blocks=%llu, should be %llu",
			     filename, ino, size, cur_blks, used_blks);
			if (opt_fixinode)
				printf(" (will be fixed)");
			printf("\n");
			fflush(stdout);
		}
	}

	/* now the real fixing, if needed */
	if (cur_blks != used_blks) {
		set(return_value, RV_FILE_NEEDS_FIX);
		if (opt_fixinode)
			fix_inode(ino, used_blks);
	}
 out:
	close(fd);
	return;
}

int main(int argc, char *argv[])
{
	struct superblock sb;
	char *device;		/* name of partition */
	int opt;		/* for getopt() */
	int i;

	printf("jfs_icheck version %s, %s, written by Tino Reichardt\n",
	       MY_VERSION, MY_DATE);
	while ((opt = getopt(argc, argv, "h?fq")) != -1) {
		switch (opt) {
		case 'h':
		case '?':
			usage();
		case 'f':
			opt_fixinode = 1;
			break;
		case 'q':
			opt_quiet = 1;
			break;
		default:
			usage();
		}
	}

	/**
	 * not only options, we need argc = optind+2
	 * argv[0] = ./jfs_tune
	 * argv[1] = -f
	 * argv[2] = -c
	 * argv[3] = -q
	 * argv[4] = partition (=> optind)
	 * argv[5] = file1
	 * argv[6] = file2
	 */
	if (argc < optind + 1)
		usage();

	/* open the device */
	device = argv[optind];
	fp = fopen(device, "r+");
	if (fp == NULL) {
		perror("Cannot open device.");
		exit(1);
	}

	/* @ first, drop caches, so we read/write fresh/valid data */
	drop_caches();

	/* Get block size information from the superblock       */
	if (ujfs_get_superblk(fp, &sb, 1)) {
		fprintf(stderr, "error reading primary superblock\n");
		if (ujfs_get_superblk(fp, &sb, 0)) {
			fprintf(stderr,
				"jfs_debugfs: error reading secondary superblock\n");
			goto errorout;
		} else {
			printf("jfs_debugfs: using secondary superblock\n");
		}
	}

	type_jfs = sb.s_flag;
	bsize = sb.s_bsize;
	l2bsize = sb.s_l2bsize;
	AIT_2nd_offset = addressPXD(&(sb.s_ait2)) * bsize;

	/* for each given file ... */
	for (i = optind + 1; i < argc; i++) {
		check_file(argv[i]);
	}

 errorout:
	ujfs_flush_dev(fp);
	fclose(fp);

	/* @ drop caches also in the end... the system should read our new (correct) data */
	if (opt_fixinode)
		drop_caches();

	fflush(stdout);
	fflush(stderr);
	exit(return_value);
}
