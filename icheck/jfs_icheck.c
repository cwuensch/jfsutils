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
#ifdef _MSC_VER
  #define __const const
#endif

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
#define RV_FILE_WAS_FIXED        0x04    /* es gab fehlerhafte Einträge, die wurden aber alle(!) korrigiert */
#define RV_FILE_CHECKING_FAILED  0x08    /* es gibt Probleme beim Herausfinden der korrekten Blockanzahl... */
#define RV_FILE_FIXING_FAILED    0x10    /* mind. ein fehlerhaften Eintrag konnte nicht korrigiert werden */
#define RV_FILE_HAS_WRONG_SIZE   0x20    /* eine Datei wurde nicht korrigiert, da übergebene Dateigröße falsch */
#define RV_DROP_CACHE_FAILED     0x30    /* Cache auf Platte schreiben fehlgeschlagen */
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
struct dinode cur_inode;
int64_t cur_address;

/* global values for our options */
int opt_usefibmap = 0;
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
           " -b        provide the real block number to be used (only 1 file possible!)\n"
           " -l        provide a file with list of inodes to be checked (default: off)\n"
           " -c        calculate the real size via FIBMAP (not with -i, default: off)\n"
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

void close_device(int flush_cache)
{
  ujfs_flush_dev(fp);
  fclose(fp);

  /* @ drop caches also in the end... the system should read our new (correct) data */
  if (flush_cache)
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
      close_device(0);
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
  /* fix nblocks value */
  cur_inode.di_nblocks = used_blks;

  /* swap if on big endian machine */
  ujfs_swap_dinode(&cur_inode, PUT, type_jfs);

  /* write it */
  if (xWrite(cur_address, sizeof(struct dinode), (char *)&cur_inode)) {
    return_value = return_value & (!RV_FILE_WAS_FIXED);
    set(return_value, RV_FILE_FIXING_FAILED);
    return 1;
  }

  if(!is_set(return_value, RV_FILE_FIXING_FAILED))
    set(return_value, RV_FILE_WAS_FIXED);
  return 0;
}


int CheckInodeByNr(char *device, unsigned InodeNr, int64_t RealBlocks, int64_t SizeOfFile, int DoFix)
{
  int ret = 0;
  int DeviceOpened = (!fp) ? 1 : 0;
  if (fp || (open_device(device) == 0))
  {
    int opt_tolerance = 0;
    int64_t cur_blks;

    if (!InodeNr || find_inode(InodeNr, FILESYSTEM_I, &cur_address)) {
      fprintf(stderr, "Can't find inode %u!\n", InodeNr);
      set(return_value, RV_FILE_NOT_FOUND);
      return(RV_FILE_NOT_FOUND);
    }

    /* read it */
    if (xRead(cur_address, sizeof(struct dinode), (char *)&cur_inode)) {
      fprintf(stderr, "Error reading inode %u\n", InodeNr);
      set(return_value, RV_FILE_NOT_FOUND);
      return(RV_FILE_NOT_FOUND);
    }

    /* swap if on big endian machine */
    ujfs_swap_dinode(&cur_inode, GET, type_jfs);

    if ((SizeOfFile != 0) && (cur_inode.di_size != SizeOfFile))
    {
      printf("??: %s[i=%u] size=%llu, should be %llu\n",
             "Inode", InodeNr, cur_inode.di_size, SizeOfFile);
//      fflush(stdout);
//      set(return_value, RV_FILE_HAS_WRONG_SIZE);
      return(RV_FILE_HAS_WRONG_SIZE);
    }

    cur_blks = cur_inode.di_nblocks;
    if (RealBlocks == 0)
    {
      opt_tolerance = 1;
      RealBlocks = ((cur_inode.di_size + bsize-1) / bsize);
      if ((cur_blks - RealBlocks > 10) || (RealBlocks - cur_blks > 10))
        RealBlocks = (cur_blks && 0x0FFFFFll);
      if ((cur_blks - RealBlocks > 10) || (RealBlocks - cur_blks > 10))
        RealBlocks = ((cur_inode.di_size + bsize-1) / bsize);
    }

    if (!opt_quiet) {
      if ((cur_blks == RealBlocks) || (opt_tolerance && (cur_blks - RealBlocks <= 10) && (RealBlocks - cur_blks <= 10))) {
        /* good */
        printf("ok: %s[i=%u] size=%llu blocks=%llu\n",
               "Inode", InodeNr, cur_inode.di_size, cur_blks);
//        fflush(stdout);
      } else {
        /* wrong */
        printf("??: %s[i=%u] size=%llu blocks=%llu, should be %llu",
               "Inode", InodeNr, cur_inode.di_size, cur_blks, RealBlocks);
        if (DoFix)
          printf(" (will be fixed)");
        printf("\n");
//        fflush(stdout);
      }
    }

    /* now the real fixing, if needed */
    if (cur_blks != RealBlocks) {
      set(ret, RV_FILE_NEEDS_FIX);
      set(return_value, RV_FILE_NEEDS_FIX);
      if (DoFix)
        if (fix_inode(RealBlocks) == 0)
          set(ret, RV_FILE_WAS_FIXED);
    }
    if (DeviceOpened) close_device(DoFix);
  }
  return(ret);
}

int CheckInodeByName(char *device, char *filename, int64_t RealBlocks, int DoFix)
{
  struct stat st;
  long long unsigned size, cur_blks, used_blks, total_blks, blk;
  unsigned ino, blknum;
  int fd;

  /* open the file */
  fd = open(filename, O_RDONLY);
  if (fd == -1) {
    fprintf(stderr, "File not found: \"%s\"\n", filename);
//    fflush(stderr);
    set(return_value, RV_FILE_NOT_FOUND);
    return(RV_FILE_NOT_FOUND);
  }
  fsync(fd);    /* just again ;) */

  if (fstat(fd, &st) == -1) {
    perror(filename);
    set(return_value, RV_FILE_NOT_FOUND);
    close(fd);
    return(RV_FILE_NOT_FOUND);
  }

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
        return(RV_FILE_NOT_FOUND);
      }

      if (blknum != 0)
        RealBlocks++;
    }
  }
  close(fd);

  CheckInodeByNr(device, st.st_ino, RealBlocks, 0, DoFix);
}

int CheckInodeList(char *device, char *ListFileName, int DoFix)
{
  FILE                 *fInodeList = NULL, *fNewList = NULL;
  tInodeData            curInode;
  char                  CommandLine[512];
  int                   NrFiles = 0, NrFound = 0, NrOk = 0, NrFixed = 0;
  int                   ret = 0;

  fInodeList = fopen(ListFileName, "rb");
  if(fInodeList)
  {
    if (open_device(device) == 0)
    {
      remove("/tmp/FixInodes.new");
      fNewList = fopen("/tmp/FixInodes.new", "wb");
      while(fread(&curInode, sizeof(tInodeData), 1, fInodeList))
      {
        NrFiles++;
        ret = CheckInodeByNr(device, curInode.InodeNr, curInode.nblocks_real, curInode.di_size, DoFix);

        if ((ret <= 0) || is_set(ret, RV_FILE_NOT_FOUND) || is_set(ret, RV_FILE_HAS_WRONG_SIZE))
        {
          if(ret == 0) {NrFound++; NrOk++;}
          // lösche aus der Liste
        }
        else
        {
          NrFound++;
          if (is_set(ret, RV_FILE_WAS_FIXED)) NrFixed++;
          // behalte in der Liste
          if(fNewList)
            fwrite(&curInode, sizeof(curInode), 1, fNewList);
        }
      }
      if(fNewList) fclose(fNewList);
      close_device(DoFix);
    }
    fclose(fInodeList);

    if(NrFound > NrOk)
    {
      snprintf(CommandLine, sizeof(CommandLine), "cp /tmp/FixInodes.new \"%s\"", ListFileName);
      system(CommandLine);
    }
    else
      remove(ListFileName);
  }

  printf("%d files given, %d found. %d of them ok, %d were fixed.",
         NrFiles, NrFound, NrOk, NrFixed);
}


int jfs_icheck(char *device, char *filenames[], int NrFiles, int UseInodeNums, int DoFix)
{
  if (open_device(device) == 0)
  {
    /* for each given file ... */
    int i;
    for (i = 0; i < NrFiles; i++) {
      if(UseInodeNums)
        CheckInodeByNr(device, strtoull(filenames[i], NULL, 10), 0, 0, DoFix);
      else
        CheckInodeByName(device, filenames[i], 0, DoFix);
    }
    close_device(DoFix);
  }
  return(return_value);
}

int ick_MAINFUNC()(int argc, char *argv[])
{
  int opt;    /* for getopt() */
  int opt_useinodenums = 0;
  int opt_providerealsize = 0;
  int opt_fixinode = 0;
  int opt_checkfilelist = 0;

  printf("jfs_icheck version %s, %s, written by Tino Reichardt\n",
         MY_VERSION, MY_DATE);
  while ((opt = getopt(argc, argv, "iblcfqh?")) != -1) {
    switch (opt) {
      case 'i':
        opt_useinodenums = 1;
        break;
      case 'b':
        opt_providerealsize = 1;
        break;
      case 'l':
        opt_checkfilelist = 1;
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
        return(0);
      default:
        usage();
        return(1);
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
    return(1);
  }


// *** kaputtmachen ***
// VOR dem icheck war die Datei auf jeden Fall geöffnet, und wurde bearbeitet
// (da sie geschnitten wurde). Das wird hier simuliert.
/*FILE *f;
f = fopen(argv[optind+1], "r+");
if (f)
{
  uint8_t b = 0xFF;
  fseek(f, 5, SEEK_SET);
  fwrite(&b, 1, 1, f);
  fclose(f);
}*/


  if(opt_checkfilelist)
    CheckInodeList(argv[optind], argv[optind + 1], opt_fixinode);
  else if(!opt_providerealsize)
    jfs_icheck(argv[optind], &argv[optind+1], argc - optind - 1, opt_useinodenums, opt_fixinode);
  else
  {
    if(opt_useinodenums)
      CheckInodeByNr(argv[optind], strtoul(argv[optind+1], NULL, 10), strtoull(argv[optind+2], NULL, 10), 0, opt_fixinode);
    else
      CheckInodeByName(argv[optind], argv[optind+1], strtoull(argv[optind+2], NULL, 10), opt_fixinode);
  }

//  fflush(stdout);
//  fflush(stderr);


// *** kaputtmachen - Teil 2 ***
// NACH dem icheck wird die Datei wieder geöffnet, da das geschnittene Video abgespielt wird.
// Das wird hier simuliert.
/*f = fopen(argv[optind+1], "r+");
if (f)
{
  uint8_t b = 0xAB;
  fseek(f, 5, SEEK_SET);
  fwrite(&b, 1, 1, f);
  fclose(f);
}*/


  return(return_value);
}
