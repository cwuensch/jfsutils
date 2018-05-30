/*
 *   Copyright (c) International Business Machines Corp., 2000-2002
 *   Copyright (c) Tino Reichardt, 2014
 *   Copyright (c) Christian Wünsch, 2015
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

#define _LARGEFILE_SOURCE   1
#define _LARGEFILE64_SOURCE 1
#define _FILE_OFFSET_BITS  64
#ifdef _MSC_VER
  #define __const const
#endif

#include "lib.h"
#include "jfs_icheck.h"

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

#define MY_VERSION  "0.5"
#define MY_DATE     "2018-05-31"

#define setReturnVal(x)  if (return_value <= x) return_value = x


/* Global Data */
FILE                   *fp = NULL;           /* Used by libfs routines       */
int                     bsize;               /* aggregate block size         */
short                   l2bsize;             /* log2 of aggregate block size */
int64_t                 AIT_2nd_offset;      /* Used by find_iag routines    */
static unsigned int     type_jfs;

/* global variables for this file only */
static struct dinode    cur_inode;
static int64_t          cur_address;

static tInodeData      *InodeLog = NULL;
static int              NrLogEntries = 0;
static unsigned long    curUpTime = 0;

/* global values for our options */
static bool opt_tolerance   =  TRUE;
static bool opt_clearcache  =  TRUE;
static bool opt_usefibmap   =  FALSE;
static bool opt_quiet       =  FALSE;


/**
 * internal functions
 */
static unsigned long GetBootTime(void)
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
  }
  return 0;
}
static unsigned long GetUpTime(void)
{
  return (time(NULL) - GetBootTime());
}

/**
 * show some info how we where called
 */
static void usage()
{
  printf("\nUsage: jfs_icheck [options] <device> file1 [file2 .. fileN]\n"
           " -i        use inode-numbers instead of file-names (default: off)\n"
           " -b <nr>   provide the real block number to be used (for all files!)\n"
           " -l <fn>   provide a file with list of inodes to be checked (default: off)\n"
           " -L <fn>   same as -l, but delete non-existent or already fixed entries\n"
           "           (if files to check are specified, <fn> will only by used as Log)\n"
//           " -t        use tolerance mode when calculating size (default when not -b or -c)\n"
           " -c        calculate the real size via FIBMAP (not with -i, default: off)\n"
           " -f        fix the inode block number and tree errors (default: off)\n"
           " -n        do not clear the inode cache before fixing (default: off)\n"
           " -q        enable quiet mode (default: off)\n"
           " -h        show some help about usage\n\n");
}


/**
 * 1 -> drop pagecache
 * 2 -> drop dentries and inodes
 * 3 -> drop pagecache, dentries and inodes
 */
static bool drop_caches()
{
  /* we are syncing only our wanted disk here... */
  sync();
  sync();

  if (!opt_clearcache)
    return TRUE;

  /* open proc file */
  int fp = open("/proc/sys/vm/drop_caches", O_WRONLY | O_TRUNC | O_CREAT | O_APPEND | O_SYNC, 0666);
  if (fp >= 0)
  {
    /* drop all caches */
    if (write(fp, "3\n", 2) == 2)
      if (close(fp) == 0)
        return TRUE;

    perror("echo 3 > /proc/sys/vm/drop_caches failed!");
    close(fp);
  }
  else
    perror ("fopen /proc/sys/vm/drop_caches failed, can't flush disk!");

  return FALSE;
}

static void close_device(bool FlushCache)
{
  ujfs_flush_dev(fp);
  fclose(fp);

  /* @ drop caches also in the end... the system should read our new (correct) data */
  if (FlushCache)
    drop_caches();
}

static bool open_device(char *device, bool FlushCache)
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
    if(FlushCache)
      if (!drop_caches())
      {
//        setReturnVal(rc_ERRDROPCACHE);
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
static bool read_inode(unsigned int InodeNr)
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
static bool fix_inode(int64_t used_blks)
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

static int64_t calc_realblocks(unsigned int NrXADBlocks)
{
  int64_t               CurrentBlocks, ExpectedBlocks, RealBlocks;
  int                   i;

  // RealBlocks berechnen
  CurrentBlocks = cur_inode.di_nblocks;
  ExpectedBlocks = ((cur_inode.di_size + bsize-1) / bsize) + NrXADBlocks;
  RealBlocks = CurrentBlocks;

  // bei zu starker Abweichung, probiere +/- 1048576 (und Vielfache)
  if ((RealBlocks < ExpectedBlocks) || ((RealBlocks > ExpectedBlocks /*+ 1024*/) && (RealBlocks >= 0x010000000)))
    for (i = 1; ((RealBlocks < ExpectedBlocks) || (RealBlocks >= 0x010000000)) && (i <= 10); i++)
    {
      RealBlocks = RealBlocks + 1048576;
      if (RealBlocks / 0x010000000 > CurrentBlocks / 0x010000000)
        RealBlocks = RealBlocks & 0x0FFFFFFFll;
    }
  else if (RealBlocks >= ExpectedBlocks + 1048576)
  {
    for (i = 1; (RealBlocks >= ExpectedBlocks + 1048576) && (i <= 10); i++)
      RealBlocks = RealBlocks - 1048576;
  }

  // wenn immernoch starke Abweichung, lieber wieder den "Original" berechneten Wert nehmen
  if ((RealBlocks < ExpectedBlocks) || (RealBlocks > ExpectedBlocks /*+ 1024*/))
  {
    if(!opt_quiet) printf("~");
    RealBlocks = ExpectedBlocks;
  }
  return RealBlocks;
}


static bool CheckInodeXTree(unsigned int InodeNr, unsigned int *NrXADBlocks)
{
  xtpage_t              xtree_area2;
  xtpage_t             *xtree, *xtree2 = &xtree_area2;
  bool                  ret = TRUE;
  int                   i, xads = 0;

  if ((cur_inode.di_mode & IFMT) != IFDIR)
  {
    xtree = (xtpage_t *) & (cur_inode.di_btroot);

    if ((xtree->header.flag & BT_LEAF) == 0)
    {
      for (i = 2; i < xtree->header.nextindex; i++)
      {
        int64_t xtpage_address2 = addressXAD(&(xtree->xad[i])) * bsize;

        if (xRead(xtpage_address2, sizeof (xtpage_t), (char *) xtree2) == 0)
        {
          /* swap if on big endian machine */
          ujfs_swap_xtpage_t(xtree2);

          if ((xtree->xad[i].off1 != xtree2->xad[2].off1) || (xtree->xad[i].off2 != xtree2->xad[2].off2))
          {
            printf("??: Inode[%u]: Tree Error! Internal XAD[%d], offset=%llu, should be %llu", InodeNr, i, offsetXAD(&(xtree->xad[i])), offsetXAD(&(xtree2->xad[2])));
            xtree->xad[i].off1 = xtree2->xad[2].off1;
            xtree->xad[i].off2 = xtree2->xad[2].off2;
            ret = FALSE;
          }
        }
        else
          fputs("xtree: error reading xtpage\n\n", stderr);
        xads++;
      }
    }
  }
  if (NrXADBlocks) *NrXADBlocks = xads;
  return ret;
}

tReturnCode CheckInodeByNr(char *device, unsigned int InodeNr, int64_t RealBlocks, int64_t *SizeOfFile, bool DoFix)
{
  bool                  RealBlocksProvided = (RealBlocks > 0);
  bool                  TreeOK = TRUE;
  unsigned int          NrXADBlocks = 0;
  tReturnCode           ret = rc_UNKNOWN;

  bool DeviceOpened = (!fp) ? TRUE : FALSE;
  if (fp || open_device(device, DoFix))
  {
    // Inode einlesen
    if (read_inode(InodeNr))
    {
      // Dateigröße mit der übergebenen vergleichen
      if ((SizeOfFile == NULL) || (*SizeOfFile == 0) || (cur_inode.di_size == *SizeOfFile))
      {
        if ((SizeOfFile) && (*SizeOfFile == 0)) *SizeOfFile = cur_inode.di_size;

        TreeOK = CheckInodeXTree(InodeNr, &NrXADBlocks);

        // tatsächliche Blockanzahl berechnen
        if (!RealBlocksProvided)
          RealBlocks = calc_realblocks(NrXADBlocks);

        // mit der eingetragenen Blockzahl vergleichen und Ergebnis ausgeben
        int64_t cur_blks = cur_inode.di_nblocks;

        if (cur_blks == RealBlocks)
        {
          /* good */
          ret = rc_ALLFILESOKAY;
          if (!opt_quiet && TreeOK)
            printf("ok: Inode[%u]: size=%llu blocks=%llu\n", InodeNr, cur_inode.di_size, cur_blks);  // good
        }
        else if ((opt_tolerance && !RealBlocksProvided) && (cur_blks >= RealBlocks) && (cur_blks <= RealBlocks + 100))
        {
          /* tolerance */
          ret = rc_ALLFILESOKAY;
          if (!opt_quiet && TreeOK)
            printf("ok?: Inode[%u]: size=%llu blocks=%llu, should be %llu (tolerated)\n", InodeNr, cur_inode.di_size, cur_blks, RealBlocks);
          RealBlocks = cur_blks;
        }
        if (cur_blks != RealBlocks || !TreeOK)
        {
          /* wrong */
          if (!opt_quiet && cur_blks != RealBlocks)
            printf("??: Inode[%u]: size=%llu blocks=%llu, should be %llu", InodeNr, cur_inode.di_size, cur_blks, RealBlocks);

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

          // add inode to LogList
          if (InodeLog)
          {
            int i;
            for (i = 0; i < NrLogEntries; i++)
              if (InodeLog[i].InodeNr == InodeNr) break;

            InodeLog[i].InodeNr       = InodeNr;
            if (i == NrLogEntries || InodeLog[i].di_size != cur_inode.di_size) 
              memset(InodeLog[i].FileName, '\0', sizeof(InodeLog[i].FileName));
            InodeLog[i].di_size       = cur_inode.di_size;
            InodeLog[i].nblocks_real  = (RealBlocksProvided) ? RealBlocks : 0;
            InodeLog[i].nblocks_wrong = cur_blks;
            InodeLog[i].LastFixTime   = (DoFix) ? curUpTime : 0;
            if (i == NrLogEntries) NrLogEntries++;
          }
        }
      }
      else
      {
        ret = rc_NOFILEFOUND;
        if (!opt_quiet)
          printf("--: Inode[%u]: size=%llu, expected %llu (skipping inode)\n", InodeNr, cur_inode.di_size, *SizeOfFile);
      }
    }
    else
    {
      printf("Inode not found: %u\n", InodeNr);
      ret = rc_NOFILEFOUND;
    }
    fflush(stdout);
    if (DeviceOpened) close_device(DoFix);
  }
  else
    ret = rc_ERRDEVICEOPEN;
  return(ret);
}

tReturnCode CheckInodeByName(char *device, char *filename, int64_t RealBlocks, bool DoFix)
{
  struct stat           st;
  int                   fd;
  tReturnCode           ret = rc_UNKNOWN;

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

      ret = CheckInodeByNr(device, st.st_ino, RealBlocks, NULL, DoFix);

      // add filename to LogList
      if (InodeLog)
      {
        int i;
        for (i = 0; i < NrLogEntries; i++)
          if (InodeLog[i].InodeNr == st.st_ino)
          {
            char *p = NULL;
            if (strlen(filename) >= sizeof(InodeLog[i].FileName))
              p = strrchr(filename, '/');
            strncpy(InodeLog[i].FileName, ((p && p[1]) ? (p+1) : filename), sizeof(InodeLog[i].FileName) - 1);
            InodeLog[i].FileName[sizeof(InodeLog[i].FileName)-1] = '\0';
            break;
          }
      }
      return ret;
    }
    else
    {
      close(fd);
      fprintf(stderr, "Error reading file: \"%s\"\n", filename);
      return rc_SOMENOTFIXED;
    }
  }

  printf("File not found: \"%s\"\n", filename);
  fflush(stdout);
  return rc_NOFILEFOUND;
}


tInodeData* ReadListFileAlloc(const char *AbsListFileName, int *OutNrInodes, int AddEntries)
{
  tInodeListHeader      InodeListHeader;
  tInodeData           *InodeList  = NULL;
  FILE                 *fInodeList = NULL;
  int                   NrInodes = 0;
  unsigned long         fs = 0;

  InodeListHeader.NrEntries = 0;
  
  fInodeList = fopen(AbsListFileName, "rb");
  if(fInodeList)
  {
    // Dateigröße bestimmen um Puffer zu allozieren
    struct stat statbuf;
    if (fstat(fileno(fInodeList), &statbuf) == 0)
      fs = statbuf.st_size;

    // Header prüfen
    if (!( (fread(&InodeListHeader, sizeof(tInodeListHeader), 1, fInodeList))
        && (strncmp(InodeListHeader.Magic, "TFinos", 6) == 0)
        && (InodeListHeader.Version == 1)
        && (InodeListHeader.FileSize == fs)
        && (InodeListHeader.NrEntries * sizeof(tInodeData) + sizeof(tInodeListHeader) == fs) ))
    {
      fclose(fInodeList);
      fprintf(stderr, "Invalid list file header!\n");
      return NULL;
    }
  }
  if(OutNrInodes) *OutNrInodes = InodeListHeader.NrEntries;

  // Puffer allozieren
  if (InodeListHeader.NrEntries + AddEntries > 0)
  {
//printf("ReadListFileAlloc(): Buffer allocated (%d entries, %d Bytes)\n", InodeListHeader.NrEntries+AddEntries, (InodeListHeader.NrEntries+AddEntries)*sizeof(tInodeData));
    InodeList = (tInodeData*) malloc((InodeListHeader.NrEntries + AddEntries) * sizeof(tInodeData));
    if(InodeList)
    {
      memset(InodeList, '\0', (InodeListHeader.NrEntries + AddEntries) * sizeof(tInodeData));
      if (fInodeList)
      {
        NrInodes = fread(InodeList, sizeof(tInodeData), InodeListHeader.NrEntries, fInodeList);
        if (NrInodes != InodeListHeader.NrEntries)
        {
          fclose(fInodeList);
          free(InodeList);
          fprintf(stderr, "Error! Unexpected end of list file.\n");
          return NULL;
        }
      }
    }
    else
      fprintf(stderr, "Error! Not enough memory to store the list file.\n");
  }
  if(fInodeList) fclose(fInodeList);
  return InodeList;
}

bool WriteListFile(const char *AbsListFileName, const tInodeData InodeList[], const int NrInodes)
{
  tInodeListHeader      InodeListHeader;
  FILE                 *fInodeList = NULL;
  bool                  ret = FALSE;

  fInodeList = fopen(AbsListFileName, "wb");
  if(fInodeList)
  {
    strncpy(InodeListHeader.Magic, "TFinos", 6);
    InodeListHeader.Version   = 1;
    InodeListHeader.NrEntries = NrInodes;
    InodeListHeader.FileSize  = (NrInodes * sizeof(tInodeData)) + sizeof(tInodeListHeader);

    if (fwrite(&InodeListHeader, sizeof(tInodeListHeader), 1, fInodeList))
    {
      if (NrInodes == 0)
        ret = TRUE;
      else if (InodeList && (fwrite(InodeList, sizeof(tInodeData), NrInodes, fInodeList) == (size_t)NrInodes))
        ret = TRUE;
    }
//    ret = (fflush(fInodeList) == 0) && ret;
    ret = (fclose(fInodeList) == 0) && ret;
  }
  return ret;
}


tReturnCode CheckInodeList(char *device, tInodeData InodeList[], int *NrInodes, bool DoFix, bool DeleteOldEntries)
{
  unsigned long         curUpTime;
  int                   NrGiven = *NrInodes, NrFound = 0, NrOk = 0, NrOkSinceBoot = 0, NrFixed = 0;
  tReturnCode           ret, return_value = rc_UNKNOWN;
  int                   i, j;

  curUpTime = GetUpTime();

  if (InodeList && (*NrInodes > 0))
  {
    if (open_device(device, DoFix))
    {
      for (i = 0; i < *NrInodes; i++)
      {
//printf("%2d. InodeNr=%u, LastFixTime=%lu, UpTime=%lu\n", i+1, InodeList[i].InodeNr, InodeList[i].LastFixTime, curUpTime);
        ret = CheckInodeByNr(device, InodeList[i].InodeNr, InodeList[i].nblocks_real, &InodeList[i].di_size, DoFix);
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

          if((InodeList[i].LastFixTime > 0) && (curUpTime < InodeList[i].LastFixTime))
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
        if(DoFix)
          InodeList[i].LastFixTime = curUpTime;
      }
      close_device(DoFix);

      printf("%d files given, %d found. %d of them ok (%d ok since last reboot), %d were fixed.\n", NrGiven, NrFound, NrOk, NrOkSinceBoot, NrFixed);
      fflush(stdout);
//printf("NrInodes = %d\n", *NrInodes);
      return return_value;
    }
    else
      return rc_ERRDEVICEOPEN;
  }
  else
  {
    printf("No files specified!\n");
    fflush(stdout);
    return rc_ALLFILESOKAY;
  }
}


tReturnCode CheckInodeListFile(char *device, char *ListFileName, bool DoFix, bool DeleteOldEntries)
{
  tInodeData           *InodeList  = NULL;
  int                   NrInodes = 0;
  tReturnCode           return_value = rc_UNKNOWN;

  InodeList = ReadListFileAlloc(ListFileName, &NrInodes, 0);
  if(!InodeList)
  {
    printf("List file could not be loaded: \"%s\".\n", ListFileName);
    fflush(stdout);
    return rc_ERRLISTFILEOPEN;
  }
 
  return_value = CheckInodeList(device, InodeList, &NrInodes, DoFix, DeleteOldEntries);

  if(InodeList && (NrInodes > 0) /*&& (DoFix || DeleteOldEntries)*/)
  {
    if (!WriteListFile(ListFileName, InodeList, NrInodes))
    {
      fprintf(stderr, "Error writing the inode list data to file!\n");
      setReturnVal(rc_ERRLISTFILEWRT);
    }
  }
  else if ((NrInodes == 0) && DeleteOldEntries)
    remove(ListFileName);
  free(InodeList);
  
  return return_value;
}


tReturnCode jfs_icheck(char *device, char *filenames[], int NrFiles, int64_t RealBlocks, bool UseInodeNums, bool DoFix, char *LogFileName)
{
  int                   NrFound = 0, NrOk = 0, NrFixed = 0;
  tReturnCode           ret, return_value = rc_UNKNOWN;

  InodeLog = NULL;

  // bisheriges Logfile einlesen
  if (LogFileName && LogFileName[0])
  {
    curUpTime = GetUpTime();
    InodeLog = ReadListFileAlloc(LogFileName, &NrLogEntries, NrFiles);
    if(!InodeLog)
      return rc_ERRLISTFILEOPEN;
    if(NrLogEntries > 0)
      printf("Additional files specified - contents of Listfile will not be processed!\n");
  }

  // eigentlicher Check
  if (open_device(device, DoFix))
  {
    /* for each given file ... */
    int i;
    for (i = 0; i < NrFiles; i++)
    {
      if(UseInodeNums)
        ret = CheckInodeByNr(device, strtoul(filenames[i], NULL, 10), RealBlocks, NULL, DoFix);
      else
        ret = CheckInodeByName(device, filenames[i], RealBlocks, DoFix);
      setReturnVal(ret);

      if (ret != rc_NOFILEFOUND)
      {
        NrFound++;
        if (ret == rc_ALLFILESOKAY)       NrOk++;
        else if (ret == rc_ALLFILESFIXED) NrFixed++;
      }
    }
    close_device(DoFix);
    printf("%d files given, %d found. %d of them ok, %d were fixed.\n", NrFiles, NrFound, NrOk, NrFixed);
  }
  else
    return rc_ERRDEVICEOPEN;

  // neues Logfile ausgeben
  if (LogFileName && LogFileName[0] && InodeLog && (NrLogEntries > 0))
  {
    if(!WriteListFile(LogFileName, InodeLog, NrLogEntries))
      fprintf(stderr, "Error writing the log data to file!\n");
  }

  free(InodeLog);
  return return_value;
}

int ick_MAINFUNC(int argc, char *argv[])
{
  int                   opt;    /* for getopt() */
  bool                  opt_useinodenums     = FALSE;
  bool                  opt_fixinode         = FALSE;
  int64_t               opt_realsize         = 0;
  char                  opt_listfile[512];     opt_listfile[0] = '\0';
  bool                  opt_deleteoldentries = FALSE;
  tReturnCode           return_value         = rc_UNKNOWN;

  setvbuf(stdout, NULL, _IOLBF, 4096);  // zeilenweises Buffering, auch bei Ausgabe in Datei

  while ((opt = getopt(argc, argv, "ib:l:L:tcfnqh?")) != -1) {
    switch (opt) {
      case 'i':
        opt_useinodenums = TRUE;
        break;
      case 'b':
        opt_realsize = strtoull(optarg, NULL, 0);
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
      case 'n':
        opt_clearcache = FALSE;
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

  if (opt_usefibmap && opt_useinodenums)
  {
    printf("Options -i and -c cannot be combined!\n");
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


  if (opt_listfile[0] && (argc == optind + 1))
    return_value = CheckInodeListFile(argv[optind], opt_listfile, opt_fixinode, opt_deleteoldentries);
  else
    return_value = jfs_icheck(argv[optind], &argv[optind+1], (argc - optind - 1), opt_realsize, opt_useinodenums, opt_fixinode, opt_listfile);

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
