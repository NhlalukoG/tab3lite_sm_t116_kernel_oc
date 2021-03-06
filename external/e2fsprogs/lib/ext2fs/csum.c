/*
 * csum.c --- checksumming of ext3 structures
 *
 * Copyright (C) 2006 Cluster File Systems, Inc.
 * Copyright (C) 2006, 2007 by Andreas Dilger <adilger@clusterfs.com>
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Library
 * General Public License, version 2.
 * %End-Header%
 */

#include "config.h"
#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#include "ext2_fs.h"
#include "ext2fs.h"
#include "crc16.h"
#include <assert.h>

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#ifdef DEBUG
#define STATIC
#else
#define STATIC static
#endif

__u16 ext2fs_group_desc_csum(ext2_filsys fs, dgrp_t group)
{
	__u16 crc = 0;
	struct ext2_group_desc *desc;
	size_t size;

	size = fs->super->s_desc_size;
	if (size < EXT2_MIN_DESC_SIZE)
		size = EXT2_MIN_DESC_SIZE;
	if (size > sizeof(struct ext4_group_desc)) {
		printf("%s: illegal s_desc_size(%zd)\n", __func__, size);
		size = sizeof(struct ext4_group_desc);
	}

	desc = ext2fs_group_desc(fs, fs->group_desc, group);

	if (fs->super->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_GDT_CSUM) {
		size_t offset = offsetof(struct ext2_group_desc, bg_checksum);

#ifdef WORDS_BIGENDIAN
		struct ext4_group_desc swabdesc;

		/* Have to swab back to little-endian to do the checksum */
		memcpy(&swabdesc, desc, size);
		ext2fs_swap_group_desc2(fs,
					(struct ext2_group_desc *) &swabdesc);
		desc = (struct ext2_group_desc *) &swabdesc;

		group = ext2fs_swab32(group);
#endif
		crc = ext2fs_crc16(~0, fs->super->s_uuid,
				   sizeof(fs->super->s_uuid));
		crc = ext2fs_crc16(crc, &group, sizeof(group));
		crc = ext2fs_crc16(crc, desc, offset);
		offset += sizeof(desc->bg_checksum); /* skip checksum */
		/* for checksum of struct ext4_group_desc do the rest...*/
		if (offset < size) {
			crc = ext2fs_crc16(crc, (char *)desc + offset,
					   size - offset);
		}
	}

	return crc;
}

int ext2fs_group_desc_csum_verify(ext2_filsys fs, dgrp_t group)
{
	if (EXT2_HAS_RO_COMPAT_FEATURE(fs->super,
				       EXT4_FEATURE_RO_COMPAT_GDT_CSUM) &&
	    (ext2fs_bg_checksum(fs, group) !=
	     ext2fs_group_desc_csum(fs, group)))
		return 0;

	return 1;
}

void ext2fs_group_desc_csum_set(ext2_filsys fs, dgrp_t group)
{
	if (!EXT2_HAS_RO_COMPAT_FEATURE(fs->super,
					EXT4_FEATURE_RO_COMPAT_GDT_CSUM))
		return;

	/* ext2fs_bg_checksum_set() sets the actual checksum field but
	 * does not calculate the checksum itself. */
	ext2fs_bg_checksum_set(fs, group, ext2fs_group_desc_csum(fs, group));
}

static __u32 find_last_inode_ingrp(ext2fs_inode_bitmap bitmap,
				   __u32 inodes_per_grp, dgrp_t grp_no)
{
	ext2_ino_t i, start_ino, end_ino;

	start_ino = grp_no * inodes_per_grp + 1;
	end_ino = start_ino + inodes_per_grp - 1;

	for (i = end_ino; i >= start_ino; i--) {
		if (ext2fs_fast_test_inode_bitmap2(bitmap, i))
			return i - start_ino + 1;
	}
	return inodes_per_grp;
}

/* update the bitmap flags, set the itable high watermark, and calculate
 * checksums for the group descriptors */
errcode_t ext2fs_set_gdt_csum(ext2_filsys fs)
{
	struct ext2_super_block *sb = fs->super;
	int dirty = 0;
	dgrp_t i;

	if (!fs->inode_map)
		return EXT2_ET_NO_INODE_BITMAP;

	if (!EXT2_HAS_RO_COMPAT_FEATURE(fs->super,
					EXT4_FEATURE_RO_COMPAT_GDT_CSUM))
		return 0;

	for (i = 0; i < fs->group_desc_count; i++) {
		__u32 old_csum = ext2fs_bg_checksum(fs, i);
		__u32 old_unused = ext2fs_bg_itable_unused(fs, i);
		__u32 old_flags = ext2fs_bg_flags(fs, i);
		__u32 old_free_inodes_count = ext2fs_bg_free_inodes_count(fs, i);

		if (old_free_inodes_count == sb->s_inodes_per_group) {
			ext2fs_bg_flags_set(fs, i, EXT2_BG_INODE_UNINIT);
			ext2fs_bg_itable_unused_set(fs, i, sb->s_inodes_per_group);
		} else {
			int unused =
				sb->s_inodes_per_group -
				find_last_inode_ingrp(fs->inode_map,
						      sb->s_inodes_per_group, i);

			ext2fs_bg_flags_clear(fs, i, EXT2_BG_INODE_UNINIT);
			ext2fs_bg_itable_unused_set(fs, i, unused);
		}

		ext2fs_group_desc_csum_set(fs, i);
		if (old_flags != ext2fs_bg_flags(fs, i))
			dirty = 1;
		if (old_unused != ext2fs_bg_itable_unused(fs, i))
			dirty = 1;
		if (old_csum != ext2fs_bg_checksum(fs, i))
			dirty = 1;
	}
	if (dirty)
		ext2fs_mark_super_dirty(fs);
	return 0;
}

#ifdef DEBUG
#include "e2p/e2p.h"

void print_csum(const char *msg, ext2_filsys fs, dgrp_t group)
{
	__u16 crc1, crc2, crc3;
	dgrp_t swabgroup;
 	struct ext2_group_desc *desc = ext2fs_group_desc(fs, fs->group_desc, group);
	size_t size;
	struct ext2_super_block *sb = fs->super;
	int offset = offsetof(struct ext2_group_desc, bg_checksum);
#ifdef WORDS_BIGENDIAN
	struct ext4_group_desc swabdesc;
#endif

	size = fs->super->s_desc_size;
	if (size < EXT2_MIN_DESC_SIZE)
		size = EXT2_MIN_DESC_SIZE;
	if (size > sizeof(struct ext4_group_desc))
		size = sizeof(struct ext4_group_desc);
#ifdef WORDS_BIGENDIAN
	/* Have to swab back to little-endian to do the checksum */
	memcpy(&swabdesc, desc, size);
	ext2fs_swap_group_desc2(fs, (struct ext2_group_desc *) &swabdesc);
	desc = (struct ext2_group_desc *) &swabdesc;

	swabgroup = ext2fs_swab32(group);
#else
	swabgroup = group;
#endif

	crc1 = ext2fs_crc16(~0, sb->s_uuid, sizeof(fs->super->s_uuid));
	crc2 = ext2fs_crc16(crc1, &swabgroup, sizeof(swabgroup));
	crc3 = ext2fs_crc16(crc2, desc, offset);
	offset += sizeof(desc->bg_checksum); /* skip checksum */
	/* for checksum of struct ext4_group_desc do the rest...*/
	if (offset < size)
		crc3 = ext2fs_crc16(crc3, (char *)desc + offset, size - offset);

	printf("%s: UUID %s(%04x), grp %u(%04x): %04x=%04x\n",
	       msg, e2p_uuid2str(sb->s_uuid), crc1, group, crc2, crc3,
	       ext2fs_group_desc_csum(fs, group));
}

unsigned char sb_uuid[16] = { 0x4f, 0x25, 0xe8, 0xcf, 0xe7, 0x97, 0x48, 0x23,
			      0xbe, 0xfa, 0xa7, 0x88, 0x4b, 0xae, 0xec, 0xdb };

int main(int argc, char **argv)
{
	struct ext2_super_block param;
	errcode_t		retval;
	ext2_filsys		fs;
	int			i;
	__u16 csum1, csum2, csum_known = 0xd3a4;

	memset(&param, 0, sizeof(param));
	ext2fs_blocks_count_set(&param, 32768);

	retval = ext2fs_initialize("test fs", EXT2_FLAG_64BITS, &param,
				   test_io_manager, &fs);
	if (retval) {
		com_err("setup", retval,
			"While initializing filesystem");
		exit(1);
	}
	memcpy(fs->super->s_uuid, sb_uuid, 16);
	fs->super->s_feature_ro_compat = EXT4_FEATURE_RO_COMPAT_GDT_CSUM;

	for (i=0; i < fs->group_desc_count; i++) {
		ext2fs_block_bitmap_loc_set(fs, i, 124);
		ext2fs_inode_bitmap_loc_set(fs, i, 125);
		ext2fs_inode_table_loc_set(fs, i, 126);
		ext2fs_bg_free_blocks_count_set(fs, i, 31119);
		ext2fs_bg_free_inodes_count_set(fs, i, 15701);
		ext2fs_bg_used_dirs_count_set(fs, i, 2);
		ext2fs_bg_flags_zap(fs, i);
	};

	csum1 = ext2fs_group_desc_csum(fs, 0);
	print_csum("csum0000", fs, 0);

	if (csum1 != csum_known) {
		printf("checksum for group 0 should be %04x\n", csum_known);
		exit(1);
	}
	csum2 = ext2fs_group_desc_csum(fs, 1);
	print_csum("csum0001", fs, 1);
	if (csum1 == csum2) {
		printf("checksums for different groups shouldn't match\n");
		exit(1);
	}
	csum2 = ext2fs_group_desc_csum(fs, 2);
	print_csum("csumffff", fs, 2);
	if (csum1 == csum2) {
		printf("checksums for different groups shouldn't match\n");
		exit(1);
	}
	ext2fs_bg_checksum_set(fs, 0, csum1);
	csum2 = ext2fs_group_desc_csum(fs, 0);
	print_csum("csum_set", fs, 0);
	if (csum1 != csum2) {
		printf("checksums should not depend on checksum field\n");
		exit(1);
	}
	if (!ext2fs_group_desc_csum_verify(fs, 0)) {
		printf("checksums should verify against gd_checksum\n");
		exit(1);
	}
	memset(fs->super->s_uuid, 0x30, sizeof(fs->super->s_uuid));
	print_csum("new_uuid", fs, 0);
	if (ext2fs_group_desc_csum_verify(fs, 0) != 0) {
		printf("checksums for different filesystems shouldn't match\n");
		exit(1);
	}
	csum1 = ext2fs_group_desc_csum(fs, 0);
	ext2fs_bg_checksum_set(fs, 0, csum1);
	print_csum("csum_new", fs, 0);
	ext2fs_bg_free_blocks_count_set(fs, 0, 1);
	csum2 = ext2fs_group_desc_csum(fs, 0);
	print_csum("csum_blk", fs, 0);
	if (csum1 == csum2) {
		printf("checksums for different data shouldn't match\n");
		exit(1);
	}

	return 0;
}
#endif
