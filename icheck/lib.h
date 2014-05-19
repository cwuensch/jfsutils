
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
 *   FUNCTION: common data & function prototypes
 */

#include <config.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "jfs_icheck.h"

#include "jfs_endian.h"
#include "jfs_filsys.h"
#include "jfs_dinode.h"
#include "inode.h"
#include "devices.h"

int xRead(int64_t address, unsigned count, char *buffer);
int xWrite(int64_t address, unsigned count, char *buffer);
int find_inode(unsigned inum, unsigned which_table, int64_t * address);
int find_iag(unsigned iagnum, unsigned which_table, int64_t * address);
