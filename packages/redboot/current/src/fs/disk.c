//==========================================================================
//
//      disk.c
//
//      RedBoot disk support
//
//==========================================================================
//####COPYRIGHTBEGIN####
//                                                                          
// -------------------------------------------                              
// The contents of this file are subject to the Red Hat eCos Public License 
// Version 1.1 (the "License"); you may not use this file except in         
// compliance with the License.  You may obtain a copy of the License at    
// http://www.redhat.com/                                                   
//                                                                          
// Software distributed under the License is distributed on an "AS IS"      
// basis, WITHOUT WARRANTY OF ANY KIND, either express or implied.  See the 
// License for the specific language governing rights and limitations under 
// the License.                                                             
//                                                                          
// The Original Code is eCos - Embedded Configurable Operating System,      
// released September 30, 1998.                                             
//                                                                          
// The Initial Developer of the Original Code is Red Hat.                   
// Portions created by Red Hat are                                          
// Copyright (C) 1998, 1999, 2000, 2001 Red Hat, Inc.
// All Rights Reserved.                                                     
// -------------------------------------------                              
//                                                                          
//####COPYRIGHTEND####
//==========================================================================
//#####DESCRIPTIONBEGIN####
//
// Author(s):    msalter
// Contributors: msalter
// Date:         2001-07-14
// Purpose:      
// Description:  
//              
// This code is part of RedBoot (tm).
//
//####DESCRIPTIONEND####
//
//==========================================================================

#include <redboot.h>
#include <fs/disk.h>

#ifdef CYGSEM_REDBOOT_DISK_EXT2FS
#include <fs/e2fs.h>
#endif
#ifdef CYGSEM_REDBOOT_DISK_ISO9660
#include <fs/iso9660fs.h>
#endif

static void do_disks(int argc, char *argv[]);

RedBoot_cmd("disks", 
            "Display disks/partitions.",
            "",
            do_disks
    );

static disk_t disk_table[CYGNUM_REDBOOT_MAX_DISKS];
static int    disk_count = 0;

static int
find_dos_partitions(disk_t *d, cyg_uint8 *mbr)
{
    cyg_uint32 s, n, tmp;
    struct mbr_partition *p;
    int i, found = 0;

    p = (struct mbr_partition *)(mbr + MBR_PTABLE_OFFSET);

    // Look for primary partitions
    for (i = 0; i < 4 && i < CYGNUM_REDBOOT_MAX_PARTITIONS; i++) {

	// Have to use memcpy because of alignment
	memcpy(&tmp, p->start_sect, 4);
	s = SWAB_LE32(tmp);
	memcpy(&tmp, p->nr_sects, 4);
	n = SWAB_LE32(tmp);

	if (s && n) {
	    ++found;
	    d->partitions[i].disk = d;
	    d->partitions[i].start_sector = s;
	    d->partitions[i].nr_sectors = n;
	    d->partitions[i].systype = p->sys_ind;
	    d->partitions[i].bootflag = p->boot_ind;
	}
	p++;
    }

#if CYGNUM_REDBOOT_MAX_PARTITIONS > 4
    {
	cyg_uint32 buf[SECTOR_SIZE/sizeof(cyg_uint32)], xoffset;
	cyg_uint16 magic;
	int nextp;

	// Go back through and find extended partitions
	for (i = 0, nextp = 4; i < 4 && nextp < CYGNUM_REDBOOT_MAX_PARTITIONS; i++) {
	    if (d->partitions[i].systype == SYSTYPE_EXTENDED) {
		// sector offsets in partition tables are relative to start
		// of extended partition.
		xoffset = d->partitions[i].start_sector;
		for ( ; nextp < CYGNUM_REDBOOT_MAX_PARTITIONS; ++nextp) {

		    // read partition boot record (same format as mbr except
		    // there should only be 2 entries max: a normal partition
		    // and another extended partition
		    if (DISK_READ(d, xoffset, buf, 1) <= 0)
			break;

		    magic = *(cyg_uint16 *)((char *)buf + MBR_MAGIC_OFFSET);
		    if (SWAB_LE16(magic) != MBR_MAGIC)
			break;

		    p = (struct mbr_partition *)((char *)buf + MBR_PTABLE_OFFSET);

		    // Have to use memcpy because of alignment
		    memcpy(&tmp, p->start_sect, 4);
		    s = SWAB_LE32(tmp);
		    memcpy(&tmp, p->nr_sects, 4);
		    n = SWAB_LE32(tmp);

		    if (s && n) {
			++found;
			d->partitions[nextp].disk = d;
			d->partitions[nextp].start_sector = s + xoffset;
			d->partitions[nextp].nr_sectors = n;
			d->partitions[nextp].systype = p->sys_ind;
			d->partitions[nextp].bootflag = p->boot_ind;
		    }
		    ++p;

		    memcpy(&tmp, p->start_sect, 4);
		    s = SWAB_LE32(tmp);
		    memcpy(&tmp, p->nr_sects, 4);
		    n = SWAB_LE32(tmp);

		    // more extended partitions?
		    if (p->sys_ind != SYSTYPE_EXTENDED || !s || !n)
			break;

		    xoffset += s;
		}
	    }
	}
    }
#endif
    return found;
}


// Find partitions on given disk.
// Return number of partitions found
static int
find_partitions(disk_t *d)
{
    cyg_uint32 buf[SECTOR_SIZE/sizeof(cyg_uint32)];
    cyg_uint16 magic;
    partition_t *p;
    int i, found = 0;


    if (d->kind == DISK_IDE_CDROM) {
	// no partition table, so fake it
	p = d->partitions;
	p->disk = d;
	p->start_sector = 0;
	p->nr_sectors = d->nr_sectors;
#ifdef CYGSEM_REDBOOT_DISK_ISO9660
	p->funs = &redboot_iso9660fs_funs;
#endif
	return 1;
    }

    // read Master Boot Record
    if (DISK_READ(d, 0, buf, 1) <= 0)
	return 0;

    // Check for DOS MBR
    magic = *(cyg_uint16 *)((char *)buf + MBR_MAGIC_OFFSET);
    if (SWAB_LE16(magic) == MBR_MAGIC) {
	found = find_dos_partitions(d, (cyg_uint8 *)buf);
    } else {
	// Might want to handle other MBR types, here...
    }

    // Now go through all partitions and install the correct
    // funcs for supported filesystems.
    for (i = 0, p = d->partitions; i < CYGNUM_REDBOOT_MAX_PARTITIONS; i++, p++) {
	switch (p->systype) {
#ifdef CYGSEM_REDBOOT_DISK_EXT2FS
	  case SYSTYPE_LINUX:
	    p->funs = &redboot_e2fs_funs;
	    break;
#endif
#ifdef CYGSEM_REDBOOT_DISK_FAT16
	  case SYSTYPE_FAT16:
	    p->funs = &redboot_fat16_funs;
	    break;
#endif
#ifdef CYGSEM_REDBOOT_DISK_FAT32
	  case SYSTYPE_FAT32:
	    p->funs = &redboot_fat32_funs;
	    break;
#endif
	  default:
	    break;  // ignore unsupported filesystems
	}
    }

    return found;
}

// Add a disk to the disk table.
// Return zero if no more room in table.
externC int
disk_register(disk_t *d)
{
    int i;

    // make sure we have room for it
    if (disk_count >= CYGNUM_REDBOOT_MAX_DISKS)
	return 0;

    // Set the index
    d->index = 0;
    for (i = 0; i < disk_count; i++)
	if (disk_table[i].kind == d->kind)
	    d->index++;

    // put it in the table
    disk_table[disk_count] = *d;

    // fill in partition info
    find_partitions(&disk_table[disk_count++]);

    return 1;
}

// Convert a filename in the form <partition_name>:<filename> into
// a partition and path.
//
static int
disk_parse_filename(const char *name, partition_t **part, const char **path)
{
    int i, kind, index, pindex;

    kind = index = pindex = 0;

    if (name[0] == 'h' && name[1] == 'd') {
	// IDE hard drives
	kind = DISK_IDE_HD;
	if (name[2] < 'a' || name[2] > 'z')
	    return 0;
	index = name[2] - 'a';
	if (name[3] < '1' || name[3] >= ('1' + CYGNUM_REDBOOT_MAX_PARTITIONS))
	    return 0;
	pindex = name[3] - '1';
	if (name[4] != ':')
	    return 0;
	*path = &name[5];
    }

    if (kind) {
	for (i = 0; i < CYGNUM_REDBOOT_MAX_DISKS; i++) {
	    if (disk_table[i].kind == kind && disk_table[i].index == index) {
		*part = &disk_table[i].partitions[pindex];
		return 1;
	    }
	}
    }
    return 0;
}

static const struct {
    int        kind;
    const char *str;
} systype_names[] = {
    { SYSTYPE_FAT12,      "FAT12" },
    { SYSTYPE_FAT16_32M,  "FAT16 <32M" },
    { SYSTYPE_FAT16,      "FAT16" },
    { SYSTYPE_EXTENDED,   "Extended" },
    { SYSTYPE_LINUX_SWAP, "Linux Swap" },
    { SYSTYPE_LINUX,      "Linux" }
};

static const char *
systype_name(int systype)
{
    int i;
    
    for (i = 0; i < sizeof(systype_names)/sizeof(systype_names[0]); i++)
	if (systype_names[i].kind == systype)
	    return systype_names[i].str;
    return "Unknown";
}

// List disk partitions
static void
do_disks(int argc, char *argv[])
{
    int i, j;
    disk_t *d;
    partition_t *p;
    char name[16];

    for (i = 0, d = disk_table;  i < disk_count;  i++, d++) {
        switch (d->kind) {
          case DISK_IDE_HD:
            for (j = 0, p = d->partitions;
                 j < CYGNUM_REDBOOT_MAX_PARTITIONS;
                 j++, p++) {
                if (p->systype) {
                    diag_sprintf(name, "hd%c%d", 'a' + d->index, j+1);
                    diag_printf("%-8s %s\n", name, systype_name(p->systype));
                }
            }
            break;
          case DISK_IDE_CDROM:
            diag_sprintf(name, "cd%d", d->index);
            diag_printf("%-8s ISO9660\n", name);
            break;
        }
    }
}

static void *fileptr;
static partition_t *file_part;

externC int 
disk_stream_open(char *filename, int *err)
{
    const char *filepath;

    // The filename is in <disk>:<path> format.
    // Convert to a partition and path.
    if (!disk_parse_filename(filename, &file_part, &filepath)) {
	*err = diskerr_badname;
	return -1;
    }

    if (file_part->systype == 0 || file_part->funs == (fs_funs_t *)0) {
	*err = diskerr_partition;
	return -1;
    }

    fileptr = (file_part->funs->open)(file_part, filepath);
    if (fileptr == NULL) {
	*err = diskerr_open;
	return -1;
    }
    return 0;
}

externC int 
disk_stream_read(char *buf, int size, int *err)
{
    int nread;

    if ((nread = (file_part->funs->read)(fileptr, buf, size)) != size) {
	*err = diskerr_read;
	return -1;
    }
    return nread;
}

externC void
disk_stream_close(int *err)
{
    fileptr = NULL;
}

externC char *
disk_error(int err)
{
    switch (err) {
    case diskerr_badname:
        return "Bad filename";
        break;
    case diskerr_partition:
        return "Unsupported filesystem";
        break;
    case diskerr_open:
        return "Can't open file";
        break;
    case diskerr_read:
        return "Can't read file";
        break;
    default:
        return "Unknown error";
        break;
    }
}

