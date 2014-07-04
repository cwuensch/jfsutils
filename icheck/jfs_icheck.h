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

#ifndef H_JFS_ICHECK
#define H_JFS_ICHECK

#ifndef __MOVIECUTTERLIB__
  #include "jfs_types.h"
  #ifndef TRUE
  #define TRUE          true
  #endif
  #ifndef FALSE
  #define FALSE         false
  #endif
#endif

#define UNIXTIME2010    1262304000   // = 2010-01-01 0:00

/*
 * Return Values
 * (the return value can ONLY be increased during the programme!)
 */
typedef enum tReturnCode
{
  rc_UNKNOWN         = -1,
  rc_NOFILEFOUND     =  0,    // es wurde KEINE der übergebenen Dateien gefunden
  rc_ALLFILESOKAY    =  1,    // alle gefundenen Dateien sind ok
  rc_ALLFILESFIXED   =  2,    // alle gefundenen Dateien sind ok oder wurden (erfolgreich) korrigiert
  rc_SOMENOTFIXED    =  3,    // es gibt Dateien, die nicht ok sind und nicht korrigiert wurden
  rc_ERRDEVICEOPEN   =  4,    // Fehler der Prerequisites
  rc_ERRLISTFILEOPEN =  5,    // Fehler beim Öffnen des ListFiles
  rc_ERRLISTFILEWRT  =  6     // Fehler beim Schreiben des ListFiles
} tReturnCode;

typedef struct
{
  char Magic            [6];  // TFinos
  short                 Version;
  int                   NrEntries;
  unsigned long         FileSize;
} tInodeListHeader;

typedef struct
{
  unsigned int          InodeNr;
  unsigned long         LastFixTime;
  int64_t               di_size;
  int64_t               nblocks_real;
  int64_t               nblocks_wrong;
  char                  FileName[64];
} tInodeData;


#ifndef __MOVIECUTTERLIB__
  /* Global Data */
  extern unsigned type_jfs;
  extern int bsize;
  extern FILE *fp;
  extern short l2bsize;
  extern int64_t AIT_2nd_offset;   /* Used by find_iag routines */

  /* Global Functions */
  tReturnCode jfs_icheck(char *device, char *filenames[], int NrFiles, int64_t RealBlocks, bool UseInodeNums, bool DoFix, char *LogFileName);
  tReturnCode CheckInodeByName(char *device, char *filename, int64_t RealBlocks, bool DoFix);
  tReturnCode CheckInodeByNr(char *device, unsigned int InodeNr, int64_t RealBlocks, int64_t *SizeOfFile, bool DoFix);
  tReturnCode CheckInodeList(char *device, tInodeData InodeList[], int *NrInodes, bool DoFix, bool DeleteOldEntries);
  tReturnCode CheckInodeListFile(char *device, char *ListFileName, bool DoFix, bool DeleteOldEntries);
  tInodeData* ReadListFileAlloc(const char *ListFileName, int *OutNrInodes, int AddEntries);
  bool        WriteListFile(const char *ListFileName, const tInodeData InodeList[], const int NrInodes);
#endif

#endif
