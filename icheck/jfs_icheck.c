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

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <locale.h>

#include <linux/types.h>
#include <linux/fs.h>

#include "jfs_endian.h"
#include "jfs_logmgr.h"
#include "jfs_version.h"
#include "devices.h"
#include "inode.h"
#include "super.h"
#include "message.h"

#include "jfs_dinode.h"
#include "jfs_imap.h"
#include "jfs_superblock.h"

#include "lib.h"
#include "jfs_icheck.h"

#define MY_VERSION  "0.3"
#define MY_DATE     "2014-06-14"

#ifdef fsck_BUILD
  #define ick_MAINFUNC() icheck_main
#else
  #define ick_MAINFUNC() main
#endif

#define setReturnVal(x)  if (return_value <= x) return_value = x


/* Global Data */
unsigned int            type_jfs;
FILE                   *fp = NULL;           /* Used by libfs routines       */
int                     bsize;               /* aggregate block size         */
short                   l2bsize;             /* log2 of aggregate block size */
int64_t                 AIT_2nd_offset;      /* Used by find_iag routines    */
struct                  dinode cur_inode;
int64_t                 cur_address;

/* global values for our options */
bool opt_tolerance   =  TRUE;
bool opt_usefibmap   =  FALSE;
bool opt_quiet       =  FALSE;


/**
 * internal functions
 */
time_t GetBootTime(void)
{
  char                  Buffer[BUFSIZ];
  static time_t         btime = 0;

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
time_t GetUpTime(void)
{
  return (time(NULL) - GetBootTime());
}

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
           " -h        show some help about usage\n\n");
}


/**
 * 1 -> drop pagecache
 * 2 -> drop dentries and inodes
 * 3 -> drop pagecache, dentries and inodes
 */
bool drop_caches()
{
  FILE *fp = fopen("/proc/sys/vm/drop_caches", "w");

  /* we are syncing only our wanted disk here... */
  sync();
  sync();

  /* open proc file */
  if (fp)
  {
    /* drop all caches */
    if (fprintf(fp, "3\n") > 0)
    {
      fclose(fp);
      return TRUE;
    }
    else
      perror("echo 3 > /proc/sys/vm/drop_caches failed!");
    fclose(fp);
  }
  else
    perror ("fopen /proc/sys/vm/drop_caches failed, can't flush disk!");

  return FALSE;
}

void close_device(bool flush_cache)
{
  ujfs_flush_dev(fp);
  fclose(fp);

  /* @ drop caches also in the end... the system should read our new (correct) data */
  if (flush_cache)
    drop_caches();
}

bool open_device(char *device)
{
  struct superblock sb;

  if (Is_Device_Mounted(device) == MSG_JFS_NOT_JFS)
  {
    printf("\n%s is mounted and the file system is not type JFS.\n\n"
		    "Check aborted.\n", device);
    return FALSE;
  }

  /* open the device */
  fp = fopen(device, "r+");
  if (fp != NULL)
  {
    /* @ first, drop caches, so we read/write fresh/valid data */
    if (!drop_caches())
    {
//      setReturnVal(rc_ERRDROPCACHE);
      fclose(fp);
      return(FALSE);
    }

    /* Get block size information from the superblock */
    if (ujfs_get_superblk(fp, &sb, 1))
    {
      fprintf(stderr, "error reading primary superblock\n");
      if (ujfs_get_superblk(fp, &sb, 0))
      {
        fprintf(stderr, "jfs_debugfs: error reading secondary superblock\n");
        fclose(fp);
//        setReturnVal(rc_ERRDEVICEOPEN);
        return(FALSE);
      }
      else
        printf("jfs_debugfs: using secondary superblock\n");
    }

    type_jfs = sb.s_flag;
    bsize = sb.s_bsize;
    l2bsize = sb.s_l2bsize;
    AIT_2nd_offset = addressPXD(&(sb.s_ait2)) * bsize;
    return(TRUE);
  }
  else
    perror("Cannot open device");

//  setReturnVal(rc_ERRDEVICEOPEN);
  return(FALSE);
}


/**
 * read some inode
 */
bool read_inode(unsigned int InodeNr)
{
  if (!InodeNr || find_inode(InodeNr, FILESYSTEM_I, &cur_address))
    return(FALSE);

  /* read it */
  if (xRead(cur_address, sizeof(struct dinode), (char *)&cur_inode) != 0)
  {
    fprintf(stderr, "Error reading inode %u!\n", InodeNr);
    return(FALSE);
  }

  /* swap if on big endian machine */
  ujfs_swap_dinode(&cur_inode, GET, type_jfs);
  return TRUE;
}

/**
 * fix some data on current inode
 */
bool fix_inode(int64_t used_blks)
{
  /* fix nblocks value */
  cur_inode.di_nblocks = used_blks;

  /* swap if on big endian machine */
  ujfs_swap_dinode(&cur_inode, PUT, type_jfs);

  /* write it */
  if (xWrite(cur_address, sizeof(struct dinode), (char *)&cur_inode) == 0)
    return TRUE;
  else
  {
    fprintf(stderr, "Error writing data to inode.\n");
    return FALSE;
  }
}

int64_t calc_realblocks()
{
  int64_t CurrentBlocks, ExpectedBlocks, RealBlocks;

  // RealBlocks berechnen
  CurrentBlocks = cur_inode.di_nblocks;
  ExpectedBlocks = ((cur_inode.di_size + bsize-1) / bsize);

  // bei zu starker Abweichung, probiere +/- 1048576
  if (CurrentBlocks < ExpectedBlocks)
  {
    RealBlocks = CurrentBlocks + 1048576;
  }
  else if (CurrentBlocks >= ExpectedBlocks + 1048576)
  {
    RealBlocks = CurrentBlocks - 1048576;
    if (RealBlocks > ExpectedBlocks + 1024)
      RealBlocks = (CurrentBlocks + 1048576) & 0x0FFFFFFFll;
  }

  // wenn immernoch starke Abweichung, lieber wieder den "Original" berechneten Wert nehmen
  if ((RealBlocks < ExpectedBlocks) || (RealBlocks > ExpectedBlocks + 1024))
    RealBlocks = ExpectedBlocks;
  return RealBlocks;
}


int CheckInodeByNr(char *device, unsigned int InodeNr, int64_t RealBlocks, int64_t SizeOfFile, bool DoFix)
{
  bool                  tolerance_mode = FALSE;
  tReturnCode           ret = rc_UNKNOWN;

  bool DeviceOpened = (!fp) ? TRUE : FALSE;
  if (fp || open_device(device))
  {
    // Inode einlesen
    if (read_inode(InodeNr))
    {
      // Dateigröße mit der übergebenen vergleichen
      if ((SizeOfFile == 0) || (cur_inode.di_size == SizeOfFile))
      {
        // tatsächliche Blockanzahl berechnen
        if (RealBlocks == 0)
        {
          if (opt_tolerance) tolerance_mode = TRUE;
          RealBlocks = calc_realblocks();
        }

        // mit der eingetragenen Blockzahl vergleichen und Ergebnis ausgeben
        int64_t cur_blks = cur_inode.di_nblocks;

        if (cur_blks == RealBlocks)
        {
          /* good */
          ret = rc_ALLFILESOKAY;
          if (!opt_quiet)
            printf("ok: %s[i=%u] size=%llu blocks=%llu\n", "Inode", InodeNr, cur_inode.di_size, cur_blks);  // good
        }
        else if (tolerance_mode && (cur_blks >= RealBlocks) && (cur_blks <= RealBlocks + 100))
        {
          /* tolerance */
          ret = rc_ALLFILESOKAY;
          if (!opt_quiet)
            printf("ok?: %s[i=%u] size=%llu blocks=%llu, should be %llu (tolerated)\n", "Inode", InodeNr, cur_inode.di_size, cur_blks, RealBlocks);
        }
        else
        {
          /* wrong */
          if (!opt_quiet)
            printf("??: %s[i=%u] size=%llu blocks=%llu, should be %llu", "Inode", InodeNr, cur_inode.di_size, cur_blks, RealBlocks);

          // now the real fixing, if needed
          if (DoFix && fix_inode(RealBlocks))
          {
            ret = rc_ALLFILESFIXED;
            if (!opt_quiet) printf(" (has been fixed)");
          }
          else
          {
            ret = rc_SOMENOTFIXED;
            if (!opt_quiet)
              printf((DoFix) ? " (Error! NOT fixed)" : " (not fixed)");
          }
          if(!opt_quiet) printf("\n");
        }
      }
      else
      {
        if (!opt_quiet)
          printf("--: %s[i=%u] size=%llu, expected %llu (skipping inode)\n", "Inode", InodeNr, cur_inode.di_size, SizeOfFile);
        ret = rc_NOFILEFOUND;
      }
    }
    else
    {
      printf("Inode not found: %u\n", InodeNr);
      ret = rc_NOFILEFOUND;
    }
    if (DeviceOpened) close_device(DoFix);
  }
  else
    ret = rc_ERRDEVICEOPEN;
  return(ret);
}

int CheckInodeByName(char *device, char *filename, int64_t RealBlocks, bool DoFix)
{
  struct stat           st;
  int                   fd;

  /* open the file */
  fd = open(filename, O_RDONLY);
  if (fd != -1)
  {
    fsync(fd);    /* just again ;) */

    if (fstat(fd, &st) != -1)
    {
      // ggf. Blockzahl durch FIBMAP ermitteln
      if (opt_usefibmap && (RealBlocks == 0))
      {
        /* check more blocks, cause JFS can split files across AG's */
        uint64_t total_blks, blk;
        unsigned int blknum;
        #define SOME_MORE_BLOCKS 1024 * 4

        total_blks = (st.st_size + st.st_blksize - 1) / st.st_blksize + SOME_MORE_BLOCKS;
        for (RealBlocks = 0, blk = 0; blk < total_blks; blk++)
        {
          blknum = blk;  /* FIBMAP ist nur 32bit ... */
          if (ioctl(fd, FIBMAP, &blknum) == -1)
          {
            perror("ioctl(FIBMAP)");
            RealBlocks = 0;
            break;
          }
          if(blknum != 0) RealBlocks++;
        }
      }
      close(fd);

      return (CheckInodeByNr(device, st.st_ino, RealBlocks, 0, DoFix));
    }
    else
    {
      close(fd);
      fprintf(stderr, "Error reading file: \"%s\"\n", filename);
      return rc_SOMENOTFIXED;
    }
  }

  printf("File not found: \"%s\"\n", filename);
  return rc_NOFILEFOUND;
}

int CheckInodeList(char *device, tInodeData InodeList[], int *NrInodes, bool DoFix, bool DeleteOldEntries)
{
  int                   NrGiven = *NrInodes, NrFound = 0, NrOk = 0, NrOkSinceBoot = 0, NrFixed = 0;
  int                   ret, return_value = rc_UNKNOWN;
  int                   i, j;

  if (InodeList && (*NrInodes > 0))
  {
    if (open_device(device))
    {
      for (i = 0; i < *NrInodes; i++)
      {
printf("%d. InodeNr=%u, LastFixTime=%lu, UpTime=%lu\n", i, InodeList[i].InodeNr, InodeList[i].LastFixTime, GetUpTime());
        ret = CheckInodeByNr(device, InodeList[i].InodeNr, InodeList[i].nblocks_real, InodeList[i].di_size, DoFix);
        setReturnVal(ret);
        if (ret == rc_NOFILEFOUND)
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
        else if (ret == rc_ALLFILESOKAY)
        {
          // Datei ist okay
          NrFound++; NrOk++;

          if((InodeList[i].LastFixTime > 0) && (GetUpTime() < InodeList[i].LastFixTime))
          {
            // Datei wurde VOR dem letzten Neustart korrigiert -> lösche aus der Liste
            NrOkSinceBoot++;
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
          if (ret == rc_ALLFILESFIXED) NrFixed++;
        }
        InodeList[i].LastFixTime = GetUpTime();
      }
      close_device(DoFix);

      printf("%u files given, %u found. %u of them ok (%u ok since last reboot), %u were fixed.\n", NrGiven, NrFound, NrOk, NrOkSinceBoot, NrFixed);
printf("NrInodes = %u\n", *NrInodes);
      return return_value;
    }
    else
      return rc_ERRDEVICEOPEN;
  }
  else
  {
    printf("No files specified!\n");
    return rc_ALLFILESOKAY;
  }
}

int CheckInodeListFile(char *device, char *ListFileName, bool DoFix, bool DeleteOldEntries)
{
  tInodeListHeader      InodeListHeader;
  tInodeData           *InodeList  = NULL;
  FILE                 *fInodeList = NULL;
  long                  fs = 0;
  int                   NrInodes = 0;
  int                   return_value = rc_UNKNOWN;

  fInodeList = fopen(ListFileName, "rb");
  if(fInodeList)
  {
    // Dateigröße bestimmen um Puffer zu allozieren
    fseek(fInodeList, 0, SEEK_END);
    long fs = ftell(fInodeList);
    rewind(fInodeList);

    // Header prüfen
    if ( (fread(&InodeListHeader, sizeof(tInodeListHeader), 1, fInodeList))
      && (strncmp(InodeListHeader.Magic, "TFinos", 6) == 0)
      && (InodeListHeader.Version == 1)
      && (InodeListHeader.FileSize == fs)
      && (InodeListHeader.NrEntries * sizeof(tInodeData) + sizeof(tInodeListHeader) == fs))
    {
      InodeList = (tInodeData*) malloc(InodeListHeader.NrEntries * sizeof(tInodeData));
      if (InodeList)
      {
        NrInodes = fread(InodeList, sizeof(tInodeData), InodeListHeader.NrEntries, fInodeList);
        fclose(fInodeList);

        if (NrInodes == InodeListHeader.NrEntries)
          return_value = CheckInodeList(device, InodeList, &NrInodes, DoFix, DeleteOldEntries);
        else
        {
          fprintf(stderr, "Error! List file could not be fully loaded.\n");
          fclose(fInodeList);
          free(InodeList);
          return rc_ERRLISTFILEOPEN;
        }
      }
      else
      {
        fclose(fInodeList);
        fprintf(stderr, "Error! Not enough memory to load the file.\n");
        return rc_ERRLISTFILEOPEN;
      }
    }
    else
    {
      fclose(fInodeList);
      printf("Invalid list file header: \"%s\".\n", ListFileName);
      return rc_ERRLISTFILEOPEN;
    }
  }
  else
  {
    printf("List file not found: \"%s\".\n", ListFileName);
    return rc_ERRLISTFILEOPEN;
  }

  if(InodeList && (NrInodes > 0) /*&& (DoFix || DeleteOldEntries)*/)
  {
    fInodeList = fopen(ListFileName, "wb");
    if(fInodeList)
    {
      InodeListHeader.NrEntries = NrInodes;
      InodeListHeader.FileSize  = (NrInodes * sizeof(tInodeData)) + sizeof(tInodeListHeader);
      if (!fwrite(&InodeListHeader, sizeof(tInodeListHeader), 1, fInodeList))
      {
        fprintf(stderr, "Error writing the updated inode list to file!\n");
        setReturnVal(rc_ERRLISTFILEWRT);
      }
      if (fwrite(InodeList, sizeof(tInodeData), NrInodes, fInodeList) != NrInodes)
      {
        fprintf(stderr, "Error writing the updated inode list to file!\n");
        setReturnVal(rc_ERRLISTFILEWRT);
      }
      fclose(fInodeList);
    }
    else
    {
      fprintf(stderr, "Error writing the updated inode list to file!\n");
      setReturnVal(rc_ERRLISTFILEWRT);
    }
  }
  else if ((NrInodes == 0) && DeleteOldEntries)
    remove(ListFileName);
  free(InodeList);
  
  return return_value;
}


int jfs_icheck(char *device, char *filenames[], int NrFiles, int64_t RealBlocks, bool UseInodeNums, bool DoFix)
{
  int ret, return_value = rc_UNKNOWN;

  if (open_device(device))
  {
    /* for each given file ... */
    int i;
    for (i = 0; i < NrFiles; i++) {
      if(UseInodeNums)
        ret = CheckInodeByNr(device, strtoul(filenames[i], NULL, 10), RealBlocks, FALSE, DoFix);
      else
        ret = CheckInodeByName(device, filenames[i], RealBlocks, DoFix);
      setReturnVal(ret);
    }
    close_device(DoFix);
  }
  else
    return rc_ERRDEVICEOPEN;

  return return_value;
}

int ick_MAINFUNC()(int argc, char *argv[])
{
  int                   opt;    /* for getopt() */
  bool                  opt_useinodenums     = FALSE;
  bool                  opt_fixinode         = FALSE;
  bool                  opt_realsize         = FALSE;
  char                  opt_listfile[512];     opt_listfile[0] = '\0';
  bool                  opt_deleteoldentries = FALSE;
  int                   return_value         = rc_UNKNOWN;

  while ((opt = getopt(argc, argv, "ib:l:L:tcfqh?")) != -1) {
    switch (opt) {
      case 'i':
        opt_useinodenums = TRUE;
        break;
      case 'b':
        opt_realsize = strtoul(optarg, NULL, 0);
        break;
      case 'L':
        opt_deleteoldentries = TRUE;
      case 'l':
        strncpy(opt_listfile, optarg, sizeof(opt_listfile));
        opt_listfile[sizeof(opt_listfile)-1] = '\0';
        break;
/*      case 't':
        opt_tolerance = TRUE;
        break;  */
      case 'c':
        opt_usefibmap = TRUE;
        break;
      case 'f':
        opt_fixinode = TRUE;
        break;
      case 'q':
        opt_quiet = TRUE;
        break;
      case 'h':
      case '?':
      default:
        printf("jfs_icheck version %s, %s, written by T. Reichardt & C. Wuensch\n", MY_VERSION, MY_DATE);
        usage();
        return rc_NOFILEFOUND;
    }
  }

  if (!opt_quiet)
    printf("jfs_icheck version %s, %s, written by T. Reichardt & C. Wuensch\n", MY_VERSION, MY_DATE);

  /**
   * not only options, we need argc = optind+2
   * argv[0] = ./jfs_tune
   * argv[1] = -i
   * argv[2] = partition (=> optind)
   * argv[3] = file1
   * argv[4] = file2
   */
  if ((argc <= optind) || ((argc <= optind + 1) && !opt_listfile[0]))
  {
    printf("\n");
    if (argc <= optind)
      printf("No partition specified!\n");
    printf("No files specified!\n");
    usage();
    return rc_NOFILEFOUND;
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
    return_value = CheckInodeListFile(argv[optind], opt_listfile, opt_fixinode, opt_deleteoldentries);
  else /* if(!opt_realsize) */
    return_value = jfs_icheck(argv[optind], &argv[optind+1], argc - optind - 1, opt_realsize, opt_useinodenums, opt_fixinode);
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

  return return_value;
}
