/*
 *   Copyright (c) International Business Machines Corp., 2000-2002
 *   Copyright (c) Tino Reichardt, 2014
 *   Copyright (c) Christian Wünsch, 2014
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
 *   FUNCTION: Alter inodes in a mounted filesystem
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

#define MY_VERSION  "0.2"
#define MY_DATE     "2014-06-01"

#ifdef fsck_BUILD
  #define ick_MAINFUNC() icheck_main
#else
  #define ick_MAINFUNC() main
#endif


/* return value is an array of some bits */
#define RV_OKAY                  0x00    /* alle dateien ok */
#define RV_FILE_NOT_FOUND        0x01    /* mind. eine Datei wurde nicht gefunden */
#define RV_FILE_NEEDS_FIX        0x02    /* mind. eine Datei hat fehlerhaften Eintrag */
#define RV_FILE_WAS_FIXED        0x04    /* es gab fehlerhafte Einträge, die wurden aber korrigiert */
#define RV_FILE_CHECKING_FAILED  0x08    /* es gibt Probleme beim Herausfinden der korrekten Blockanzahl... */
#define RV_FILE_FIXING_FAILED    0x10    /* mind. ein fehlerhaften Eintrag konnte nicht korrigiert werden*/
#define RV_DROP_CACHE_FAILED     0x20    /* Cache auf Platte schreiben fehlgeschlagen */
int return_value = RV_OKAY;

/* setting some bit of some byte can be so easy ;) */
#define is_set(x,v)  (((x)&(v)) == (v))
#define set(x,v)     ((x) |= (v))


/* Global Data */
unsigned type_jfs;
FILE *fp = NULL;           /* Used by libfs routines       */
int bsize;                 /* aggregate block size         */
short l2bsize;             /* log2 of aggregate block size */
int64_t AIT_2nd_offset;    /* Used by find_iag routines    */
struct dinode inode;

/* global values for our options */
int opt_useinodenums = 0;
int opt_providerealsize = 0;
int opt_usefibmap = 0;
int opt_fixinode = 0;
int opt_quiet = 0;

/**
 * internal functions
 */


/**
 * show some info how we where called
 */
void usage()
{
  printf("\nUsage: jfs_icheck [options] <device> file1 file2 .. fileN <nblocks>\n"
           " -i        use inode-numbers instead of file-names (default: off)\n"
           " -s        provide the real size (number of blocks) to be used. (Only one file possible!)\n"
           " -c        calculate the real size via FIBMAP (default: off)\n"
           " -f        fix the inode block number value (default: off)\n"
           " -q        enable quiet mode (default: off)\n"
           " -h        show some help about usage\n");
}


/**
 * 1 -> drop pagecache
 * 2 -> drop dentries and inodes
 * 3 -> drop pagecache, dentries and inodes
 */
int drop_caches()
{
  FILE *fp = fopen("/proc/sys/vm/drop_caches", "w");

  /* we are syncing only our wanted disk here... */
  sync();
  sync();

  /* open proc file */
  if (!fp) {
    perror ("fopen /proc/sys/vm/drop_caches failed, can't flush disk!");
    set(return_value, RV_DROP_CACHE_FAILED);
    return 1;
  }

  /* drop all caches */
  if (fprintf(fp, "3\n") == -1) {
    perror("echo 3 > /proc/sys/vm/drop_caches failed!");
    set(return_value, RV_DROP_CACHE_FAILED);
    fclose(fp);
    return 1;
  }

  fclose(fp);
  return 0;
}

void close_device()
{
  ujfs_flush_dev(fp);
  fclose(fp);

  /* @ drop caches also in the end... the system should read our new (correct) data */
  if (opt_fixinode)
    drop_caches();
}

int open_device(char *device)
{
  struct superblock sb;

  /* open the device */
  fp = fopen(device, "r+");
  if (fp == NULL) {
    perror("Cannot open device.");
    return(1);
  }

  /* @ first, drop caches, so we read/write fresh/valid data */
  drop_caches();

  /* Get block size information from the superblock       */
  if (ujfs_get_superblk(fp, &sb, 1)) {
    fprintf(stderr, "error reading primary superblock\n");
    if (ujfs_get_superblk(fp, &sb, 0)) {
      fprintf(stderr, "jfs_debugfs: error reading secondary superblock\n");
      close_device();
      return(1);
    } else {
      printf("jfs_debugfs: using secondary superblock\n");
    }
  }

  type_jfs = sb.s_flag;
  bsize = sb.s_bsize;
  l2bsize = sb.s_l2bsize;
  AIT_2nd_offset = addressPXD(&(sb.s_ait2)) * bsize;
  return(0);
}


/**
 * fix some data on inode
 */
int fix_inode(int64_t used_blks)
{
  int64_t address;

  /* fix nblocks value */
  inode.di_nblocks = used_blks;

  /* swap if on big endian machine */
  ujfs_swap_dinode(&inode, PUT, type_jfs);

  /* write it */
  if (xWrite(address, sizeof(struct dinode), (char *)&inode)) {
    set(return_value, RV_FILE_FIXING_FAILED);
    return 1;
  }

  set(return_value, RV_FILE_WAS_FIXED);
  return 0;
}


int CheckInodeByNr(char *device, unsigned InodeNr, int64_t RealBlocks, int DoFix)
{
  opt_fixinode = DoFix;

  int DeviceOpened = (!fp) ? 1 : 0;
  if (fp || (open_device(device) == 0))
  {
    int opt_tolerance = 0;
    int64_t address;
    int64_t cur_blks;

    if (!InodeNr || find_inode(InodeNr, FILESYSTEM_I, &address)) {
      fprintf(stderr, "Can't find inode %u!\n", InodeNr);
      set(return_value, RV_FILE_NOT_FOUND);
      return(return_value);
    }

    /* read it */
    if (xRead(address, sizeof(struct dinode), (char *)&inode)) {
      fprintf(stderr, "Error reading inode %u\n", InodeNr);
      set(return_value, RV_FILE_NOT_FOUND);
      return(return_value);
    }

    /* swap if on big endian machine */
    ujfs_swap_dinode(&inode, GET, type_jfs);

    cur_blks = inode.di_nblocks;
    if (RealBlocks == 0)
    {
      opt_tolerance = 1;
      RealBlocks = cur_blks;
      if ((cur_blks - RealBlocks > 10) || (RealBlocks - cur_blks > 10))
        RealBlocks = (cur_blks && 0x0FFFFFll);
      if ((cur_blks - RealBlocks > 10) || (RealBlocks - cur_blks > 10))
        RealBlocks = ((inode.di_size + bsize-1) / bsize);
    }

    if (!opt_quiet) {
      if ((cur_blks == RealBlocks) || (opt_tolerance && (cur_blks - RealBlocks <= 10) && (RealBlocks - cur_blks <= 10))) {
        /* good */
        printf("ok: %s[i=%u] size=%llu blocks=%llu\n",
               "Inode", InodeNr, inode.di_size, cur_blks);
        fflush(stdout);
      } else {
        /* wrong */
        printf("??: %s[i=%u] size=%llu blocks=%llu, should be %llu",
               "Inode", InodeNr, inode.di_size, cur_blks, RealBlocks);
        if (DoFix)
          printf(" (will be fixed)");
        printf("\n");
        fflush(stdout);
      }
    }

    /* now the real fixing, if needed */
    if (cur_blks != RealBlocks) {
      set(return_value, RV_FILE_NEEDS_FIX);
      if (DoFix)
        fix_inode(RealBlocks);
    }
    if (DeviceOpened) close_device();
  }
  return(return_value);
}

int CheckInodeByName(char *device, char *filename, int64_t RealBlocks, int DoFix)
{
  opt_fixinode = DoFix;

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
    return(return_value);
  }
  fsync(fd);    /* just again ;) */

  if (fstat(fd, &st) == -1) {
    perror(filename);
    set(return_value, RV_FILE_NOT_FOUND);
    close(fd);
    return(return_value);
  }
  close(fd);

  if (opt_usefibmap && (RealBlocks == 0))
  {
    /* check more blocks, cause JFS can split files across AG's */
    #define SOME_MORE_BLOCKS 1024 * 4

    total_blks = (st.st_size + st.st_blksize - 1) / st.st_blksize + SOME_MORE_BLOCKS;
    for (RealBlocks = 0, blk = 0; blk < total_blks; blk++) {
      blknum = blk;  /* FIBMAP ist nur 32bit ... */
      if (ioctl(fd, FIBMAP, &blknum) == -1) {
        perror("ioctl(FIBMAP)");
        set(return_value, RV_FILE_NOT_FOUND);
        close(fd);
        return(return_value);
      }

      if (blknum != 0)
        RealBlocks++;
    }
  }

  CheckInodeByNr(device, st.st_ino, RealBlocks, DoFix);
  return(return_value);
}


int jfs_icheck(char *device, char *filenames[], int NrFiles, int UseInodeNums, int DoFix)
{
  opt_fixinode = DoFix;

  if (open_device(device) == 0)
  {
    /* for each given file ... */
    int i;
    for (i = 0; i < NrFiles; i++) {
      if(UseInodeNums)
        CheckInodeByNr(device, strtoull(filenames[i], NULL, 10), 0, DoFix);
      else
        CheckInodeByName(device, filenames[i], 0, DoFix);
    }
    close_device();
  }
  return(return_value);
}

int ick_MAINFUNC()(int argc, char *argv[])
{
  int opt;    /* for getopt() */

  printf("jfs_icheck version %s, %s, written by Tino Reichardt\n",
         MY_VERSION, MY_DATE);
  while ((opt = getopt(argc, argv, "iscfqh?")) != -1) {
    switch (opt) {
      case 'i':
        opt_useinodenums = 1;
        break;
      case 's':
        opt_providerealsize = 1;
        break;
      case 'c':
        opt_usefibmap = 1;
        break;
      case 'f':
        opt_fixinode = 1;
        break;
      case 'q':
        opt_quiet = 1;
        break;
      case 'h':
      case '?':
        usage();
        exit(0);
      default:
        usage();
        exit(1);
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
  if ((argc <= optind + 1) || (opt_providerealsize && argc <= optind + 2))
  {
    usage();
    exit(1);
  }

  if(!opt_providerealsize)
    jfs_icheck(argv[optind], &argv[optind+1], argc - optind - 1, opt_useinodenums, opt_fixinode);
  else
  {
    if(opt_useinodenums)
      CheckInodeByNr(argv[optind], strtoul(argv[optind+1], NULL, 10), strtoull(argv[optind+2], NULL, 10), opt_fixinode);
    else
      CheckInodeByName(argv[optind], argv[optind+1], strtoull(argv[optind+2], NULL, 10), opt_fixinode);
  }

  fflush(stdout);
  fflush(stderr);
  exit(return_value);
}
