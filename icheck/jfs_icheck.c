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
#include <locale.h>

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

#define MY_VERSION  "0.3"
#define MY_DATE     "2014-06-15"

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
int opt_tolerance = 1;
int opt_usefibmap = 0;
int opt_quiet = 0;

/**
 * internal functions
 */
unsigned long GetBootTime(void)
{
  char                  Buffer[BUFSIZ];
  static unsigned long  btime = 0;

  if (btime > 0) return btime;
  FILE *fp = fopen("/proc/stat", "r");
  if (fp)
  {
    while(fgets(Buffer, BUFSIZ, fp))
    {
      if(sscanf(Buffer, "btime %ld", &btime) == 1)
      {
        fclose(fp);
        return btime;
      }
    }
    fclose(fp);
    return 0;
  }
}
unsigned long GetUpTime(void)
{
  return (time(NULL) - GetBootTime());
}
/* unsigned int GetUpTime2(void)
{
  unsigned int          uptime = 0;
  FILE                 *fp;
  double                upsecs;

  fp = fopen("/proc/uptime", "r");
  if(fp != NULL)
  {
    char                buf[BUFSIZ];
    int                 res;
    char               *b;

    b = fgets(buf, BUFSIZ, fp);
    if(b == buf)
    {
      /* The following sscanf must use the C locale.  *
      setlocale(LC_NUMERIC, "C");
      res = sscanf(buf, "%lf", &upsecs);
      setlocale(LC_NUMERIC, "");
      if(res == 1) uptime = (unsigned int)(upsecs);
    }
    fclose(fp);
  }
  return uptime;
}  */


/**
 * show some info how we where called
 */
void usage()
{
  printf("\nUsage: jfs_icheck [options] <device> file1 [file2 .. fileN]\n"
           " -i        use inode-numbers instead of file-names (default: off)\n"
           " -b <nr>   provide the real block number to be used (for all files!)\n"
           " -l <fn>   provide a file with list of inodes to be checked (default: off)\n"
           " -L <fn>   same as -l, but delete non-existent or already fixed entries\n"
//           " -t        use tolerance mode when calculating size (default when not -b or -c)\n"
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
    int tolerance_mode = 0;
    int64_t cur_blks, ExpectedBlocks;

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
      printf("??: %s[i=%u] size=%llu, expected %llu (skipping inode)\n",
             "Inode", InodeNr, cur_inode.di_size, SizeOfFile);
//      fflush(stdout);
      set(return_value, RV_FILE_HAS_WRONG_SIZE);
      return(RV_FILE_HAS_WRONG_SIZE);
    }

    cur_blks = cur_inode.di_nblocks;
    if (RealBlocks == 0)
    {
      // RealBlocks berechnen
      if (opt_tolerance) tolerance_mode = 1;
      ExpectedBlocks = ((cur_inode.di_size + bsize-1) / bsize);

      // bei zu starker Abweichung, probiere +/- 1048576
      if (cur_blks < ExpectedBlocks)
      {
        RealBlocks = cur_blks + 1048576;
fprintf(stdout, "Probiere RealBlocks=%lld\n", RealBlocks);
      }
      else if (cur_blks >= ExpectedBlocks + 1048576)
      {
        RealBlocks = cur_blks - 1048576;
fprintf(stdout, "Probiere RealBlocks=%lld\n", RealBlocks);
        if (RealBlocks > ExpectedBlocks + 1024)
          RealBlocks = (cur_blks + 1048576) & 0x0FFFFFFFll;
fprintf(stdout, "Probiere RealBlocks=%lld\n", RealBlocks);
      }

      // wenn immernoch starke Abweichung, lieber wieder den "Original" berechneten Wert nehmen
      if ((RealBlocks < ExpectedBlocks) || (RealBlocks > ExpectedBlocks + 1024))
        RealBlocks = ExpectedBlocks;
    }

    if (!opt_quiet) {
      if (cur_blks == RealBlocks) {
        /* good */
        printf("ok: %s[i=%u] size=%llu blocks=%llu\n",
               "Inode", InodeNr, cur_inode.di_size, cur_blks);
      } else if (tolerance_mode && (cur_blks >= RealBlocks) && (cur_blks <= RealBlocks + 100)) {
        /* okay?, tolerance */
        printf("ok?: %s[i=%u] size=%llu blocks=%llu, should be %llu (tolerated)\n",
               "Inode", InodeNr, cur_inode.di_size, cur_blks, RealBlocks);
      } else {
        /* wrong */
        printf("??: %s[i=%u] size=%llu blocks=%llu, should be %llu",
               "Inode", InodeNr, cur_inode.di_size, cur_blks, RealBlocks);
        if (DoFix)
          printf(" (will be fixed)");
        printf("\n");
      }
//      fflush(stdout);
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

int CheckInodeList(char *device, tInodeData InodeList[], int *NrInodes, int DoFix, int DeleteOldEntries)
{
  char                  CommandLine[512];
  int                   NrGiven = *NrInodes, NrFound = 0, NrOk = 0, NrOkOk = 0, NrFixed = 0;
  int                   i, j, ret = 0;

  if (InodeList && (*NrInodes > 0))
  {
    if (open_device(device) == 0)
    {
      for (i = 0; i < *NrInodes; i++)
      {
//fprintf(stdout, "%d. InodeNr=%lu, LastFixTime=%lu, UpTime=%lu\n", i, InodeList[i].InodeNr, InodeList[i].LastFixTime, GetUpTime());
        ret = CheckInodeByNr(device, InodeList[i].InodeNr, InodeList[i].nblocks_real, InodeList[i].di_size, DoFix);
        if ((ret < 0) || is_set(ret, RV_FILE_NOT_FOUND) || is_set(ret, RV_FILE_HAS_WRONG_SIZE))
        {
          // Datei existiert nicht -> lösche aus der Liste
          if (DeleteOldEntries)
          {
            (*NrInodes)--;
            for (j = i; j < *NrInodes; j++)
              InodeList[j] = InodeList[j+1];
            i--;
          }
          continue;
        }
        else if (ret == 0)
        {
          // Datei ist okay
          NrFound++; NrOk++;

          if((InodeList[i].LastFixTime > 0) && (GetUpTime() < InodeList[i].LastFixTime))
          {
            // Datei wurde VOR dem letzten Neustart korrigiert -> lösche aus der Liste
            NrOkOk++;
            if (DeleteOldEntries)
            {
              (*NrInodes)--;
              for (j = i; j < *NrInodes; j++)
                InodeList[j] = InodeList[j+1];
              i--;
            }
            continue;
          }
        }
        else
        {
          // Datei wurde (erfolgreich?) korrigiert
          NrFound++;
          if (is_set(ret, RV_FILE_WAS_FIXED)) NrFixed++;
        }
        /*if(DoFix)*/ InodeList[i].LastFixTime = GetUpTime();
      }
      close_device(DoFix);
    }
  }

  printf("%d files given, %d found. %d of them ok (%d ok since last reboot), %d were fixed.\n",
         NrGiven, NrFound, NrOk, NrOkOk, NrFixed);
  printf("NrInodes = %lu\n", *NrInodes);
  return return_value;
}

int CheckInodeListFile(char *device, char *ListFileName, int DoFix, int DeleteOldEntries)
{
  tInodeData           *InodeList  = NULL;
  FILE                 *fInodeList = NULL;
  int                   NrInodes = 0;
  int                   ret = 0;

  fInodeList = fopen(ListFileName, "rb");
  if(fInodeList)
  {
    // Dateigröße bestimmen um Puffer zu allozieren
    fseek(fInodeList, 0, SEEK_END);
    long fs = ftell(fInodeList);
    rewind(fInodeList);

    tInodeData *InodeList = (tInodeData*) malloc((fs + sizeof(tInodeData)-1) / sizeof(tInodeData) * sizeof(tInodeData));
    if (InodeList)
    {
      while (!feof(fInodeList))
      {
        ret = fread(&InodeList[NrInodes], sizeof(tInodeData), 10, fInodeList);
        NrInodes += ret;
      }
      fclose(fInodeList);
    }
    else
    {
      fprintf(stderr, "Error! Not enough memory to load the file.\n");
      return -1;
    }

    ret = CheckInodeList(device, InodeList, &NrInodes, DoFix, DeleteOldEntries);

    if((NrInodes > 0) /*&& (DoFix || DeleteOldEntries)*/)
    {
      fInodeList = fopen(ListFileName, "wb");
      if(fInodeList)
      {
        if (fwrite(InodeList, sizeof(tInodeData), NrInodes, fInodeList) != NrInodes)
          fprintf(stdout, "Error writing the updated inode list to file!\n");
        fclose(fInodeList);
      }
      else
        fprintf(stderr, "Error writing the updated inode list to file!\n");
    }
    else if ((NrInodes == 0) && DeleteOldEntries)
      remove(ListFileName);
    free(InodeList);
  }
  else
    fprintf(stdout, "List file not found: '%s'.\n", ListFileName);
  
  return ret;
}


int jfs_icheck(char *device, char *filenames[], int NrFiles, int64_t RealBlocks, int UseInodeNums, int DoFix)
{
  if (open_device(device) == 0)
  {
    /* for each given file ... */
    int i;
    for (i = 0; i < NrFiles; i++) {
      if(UseInodeNums)
        CheckInodeByNr(device, strtoull(filenames[i], NULL, 10), RealBlocks, 0, DoFix);
      else
        CheckInodeByName(device, filenames[i], RealBlocks, DoFix);
    }
    close_device(DoFix);
  }
  return(return_value);
}

int ick_MAINFUNC()(int argc, char *argv[])
{
  int opt;    /* for getopt() */
  int opt_useinodenums = 0;
  int opt_fixinode = 0;
  int opt_realsize = 0;
  char opt_listfile[512]; opt_listfile[0] = '\0';
  int opt_deleteoldentries = 0;

  printf("jfs_icheck version %s, %s, written by T. Reichardt & C. Wuensch\n",
         MY_VERSION, MY_DATE);
  while ((opt = getopt(argc, argv, "ib:l:L:tcfqh?")) != -1) {
    switch (opt) {
      case 'i':
        opt_useinodenums = 1;
        break;
      case 'b':
        opt_realsize = strtoul(optarg, NULL, 0);
        break;
      case 'L':
        opt_deleteoldentries = 1;
      case 'l':
        strncpy(opt_listfile, optarg, sizeof(opt_listfile));
        opt_listfile[sizeof(opt_listfile)-1] = '\0';
        break;
/*      case 't':
        opt_tolerance = 1;
        break;  */
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
  if ((argc <= optind) || (argc <= optind + 1 && !opt_listfile[0]))
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


  if(opt_listfile[0])
    CheckInodeListFile(argv[optind], opt_listfile, opt_fixinode, opt_deleteoldentries);
  else /* if(!opt_realsize) */
    jfs_icheck(argv[optind], &argv[optind+1], argc - optind - 1, opt_realsize, opt_useinodenums, opt_fixinode);
/*  else
  {
    if(opt_useinodenums)
      CheckInodeByNr(argv[optind], strtoul(argv[optind+1], NULL, 10), opt_realsize, 0, opt_fixinode);
    else
      CheckInodeByName(argv[optind], argv[optind+1], opt_realsize, opt_fixinode);
  } */

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
