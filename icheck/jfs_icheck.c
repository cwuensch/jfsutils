
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

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>

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

/* Global Data */
unsigned type_jfs;
FILE *fp;			/* Used by libfs routines       */
int bsize;			/* aggregate block size         */
short l2bsize;			/* log2 of aggregate block size */
int64_t AIT_2nd_offset;		/* Used by find_iag routines    */

/* if num_errors > 0 in the end, we will exit with error */
int num_errors = 0;

/**
 * internal functions
 */
void usage(void);
void drop_caches(void);
void fix_inode(unsigned inum);

/**
 * show some info how we where called
 */
void usage()
{
	printf("\nUsage:  jfs_icheck [options] <device> file1 file2 .. fileN\n"
	       " -c        only check the gives files (default: on)\n"
	       " -f        fix the inode information (default: off)\n"
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
		num_errors++;
		return;
	}

	/* drop all caches */
	if (fprintf(fp, "3\n") == -1) {
		perror("echo 3 > /proc/sys/vm/drop_caches failed!");
		num_errors++;
		return;
	}

	fclose(fp);
}

/**
 * fix some data on inode
 */
void fix_inode(unsigned inum)
{
	int64_t address;
	struct dinode inode;

	if (find_inode(inum, FILESYSTEM_I, &address)) {
		fprintf(stderr, "Can't find inode %u!\n", inum);
		num_errors++;
		return;
	}

	/* read it */
	if (xRead(address, sizeof(struct dinode), (char *)&inode)) {
		fprintf(stderr, "Error reading inode %u\n", inum);
		num_errors++;
		return;
	}

	/* swap if on big endian machine */
	ujfs_swap_dinode(&inode, GET, type_jfs);

	/* fix nblocks value */
	inode.di_nblocks = (inode.di_size + 4095) / 4096;

	/* swap if on big endian machine */
	ujfs_swap_dinode(&inode, PUT, type_jfs);

	if (xWrite(address, sizeof(struct dinode), (char *)&inode)) {
		fprintf(stderr, "Error writing inode %u\n", inum);
		num_errors++;
	}

	return;
}


int main(int argc, char *argv[])
{
	struct superblock sb;
	char *device;		/* name of partition */
	int opt;		/* for getopt() */

	/* default values for our options */
	int opt_fixinode = 0;
	int opt_quiet = 0;
	int i, rv;

	printf("jfs_icheck version %s, %s, written by Tino Reichardt\n",
	       MY_VERSION, MY_DATE);
	while ((opt = getopt(argc, argv, "h?cfq")) != -1) {
		switch (opt) {
		case 'h':
		case '?':
			usage();
		case 'c':
			opt_fixinode = 0;
			break;
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
		perror("Cannot open device.\n");
		exit(1);
	}

	/* @ first, drop caches, so we read/write fresh/valid data */
	drop_caches();

	/* Get block size information from the superblock       */
	if (ujfs_get_superblk(fp, &sb, 1)) {
		perror("error reading primary superblock");
		if (ujfs_get_superblk(fp, &sb, 0)) {
			perror
			    ("jfs_debugfs: error reading secondary superblock");
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
		struct stat st;
		char *file = argv[i];	/* name of current file */
		long long unsigned blocks, size, blocks_good;
		unsigned ino;

		rv = stat(file, &st);
		if (rv == -1) {
			fprintf(stderr, "Can't find the file \"%s\"\n", file);
			fflush(stderr);
			num_errors++;
			continue;	/* try next file ... */
		}

		ino = (unsigned)st.st_ino;
		size = (long long unsigned)st.st_size;
		blocks = (long long unsigned)st.st_blocks / 8;
		blocks_good = (size + 4095) / 4096;

		if (!opt_quiet) {
			if (blocks == blocks_good) {
				/* good */
				printf("ok: %s[i=%u] size=%llu blocks=%llu\n",
				       file, ino, size, blocks);
				fflush(stdout);
			} else {
				/* wrong */
				printf
				    ("??: %s[i=%u] size=%llu blocks=%llu, but should be %llu",
				     file, ino, size, blocks, blocks_good);
				if (opt_fixinode)
					printf(" (will be fixed)");
				printf("\n");
				fflush(stdout);
			}
		}

		if (opt_fixinode)
			fix_inode(ino);
	}

 errorout:
	ujfs_flush_dev(fp);
	fclose(fp);

	/* @ drop caches also in the end... the system should read our new (correct) data */
	if (opt_fixinode)
		drop_caches();

	fflush(stdout);
	fflush(stderr);
	if (num_errors == 0)
		exit(EXIT_SUCCESS);

	printf("num_errors=%d\n", num_errors);
	fflush(stdout);
	exit(EXIT_FAILURE);
}
