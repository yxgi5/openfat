/*
 * This file is part of the openfat project.
 *
 * Copyright (C) 2011  Department of Physics, University of Otago
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* FAT Filesystem write support implementation
 */
#include <string.h>

#include "openfat.h"

#include "fat_core.h"

uint32_t fat_find_free_cluster(const struct fat_vol_handle *h)
{
	for(uint32_t i = 2; i < h->cluster_count; i++) {
		/* FIXME: Read sectors here to save reads */
		if(fat_get_next_cluster(h, i) == 0)
			return i;
	}

	return 0; /* No free clusters */
}

static int 
fat32_set_next_cluster(const struct fat_vol_handle *h, 
			uint32_t cluster, uint32_t next)
{
	uint32_t offset = cluster * 4;
	uint32_t sector;

	sector = h->reserved_sector_count + (offset / h->bytes_per_sector);
	offset %= h->bytes_per_sector;

	block_read_sectors(h->dev, sector, 1, sector_buf); 

	/* Preserve high nybble */
	next &= 0x0FFFFFFF;
	next |= __get_le32((uint32_t*)(sector_buf + offset)) & 0xF0000000;
	__put_le32((uint32_t*)(sector_buf + offset), next);

	block_write_sectors(h->dev, sector, 1, sector_buf); 

	return 0;
}

static int 
fat16_set_next_cluster(const struct fat_vol_handle *h, 
			uint16_t cluster, uint16_t next)
{
	uint32_t offset = cluster * 2;
	uint32_t sector;

	sector = h->reserved_sector_count + (offset / h->bytes_per_sector);
	offset %= h->bytes_per_sector;

	block_read_sectors(h->dev, sector, 1, sector_buf); 
	__put_le16((uint16_t*)(sector_buf + offset), next);
	block_write_sectors(h->dev, sector, 1, sector_buf); 

	return 0;
}

static int 
fat12_set_next_cluster(const struct fat_vol_handle *h, 
			uint16_t cluster, uint16_t next)
{
	uint32_t offset = cluster + (cluster / 2);
	uint32_t sector;
	sector = h->reserved_sector_count + (offset / h->bytes_per_sector);
	offset %= h->bytes_per_sector;

	block_read_sectors(h->dev, sector, 1, sector_buf); 
	if(offset == (uint32_t)h->bytes_per_sector - 1) {
		if(cluster & 1) {
			next <<= 4;
			sector_buf[offset] &= 0x0F;
			sector_buf[offset] |= next & 0xF0;
			block_write_sectors(h->dev, sector, 1, sector_buf); 
			block_read_sectors(h->dev, sector + 1, 1, sector_buf); 
			sector_buf[0] = next >> 8;
		} else {
			sector_buf[offset] = next & 0xFF;
			block_write_sectors(h->dev, sector, 1, sector_buf); 
			block_read_sectors(h->dev, sector + 1, 1, sector_buf); 
			sector_buf[0] &= 0xF0;
			sector_buf[0] |= (next >> 8) & 0x0F;
		}
	} else {
		if(cluster & 1) {
			next <<= 4;
			next |= __get_le16((uint16_t*)(sector_buf + offset)) & 0xF;
		} else {
			next &= 0x0FFF;
			next |= __get_le16((uint16_t*)(sector_buf + offset)) & 0xF000;
		}
		__put_le16((uint16_t*)(sector_buf + offset), next);
	}
	block_write_sectors(h->dev, sector, 1, sector_buf); 

	return 0;
}

static int 
fat_set_next_cluster(const struct fat_vol_handle *h, 
			uint32_t cluster, uint32_t next)
{
	switch(h->type) {
	case FAT_TYPE_FAT12:
		return fat12_set_next_cluster(h, cluster, next);
	case FAT_TYPE_FAT16:
		return fat16_set_next_cluster(h, cluster, next);
	case FAT_TYPE_FAT32:
		return fat32_set_next_cluster(h, cluster, next);
	}
	return -1;
}

uint32_t fat_alloc_next_cluster(const struct fat_vol_handle *h, 
				uint32_t cluster)
{
	uint32_t next = fat_find_free_cluster(h);

	if(!next) 
		return 0;

	/* Write end of chain marker in new cluster */
	fat_set_next_cluster(h, next, fat_eoc(h));
	/* Add new cluster to chain */
	fat_set_next_cluster(h, cluster, next);

	return next;
}

#define MIN(x, y) (((x) < (y))?(x):(y))
int fat_file_write(struct fat_file_handle *h, const void *buf, int size)
{
	/* FIXME: don't write past end of FAT12/FAT16 root directory! */
	int i;
	uint32_t sector;
	uint16_t offset;

	sector = fat_first_sector_of_cluster(h->fat, h->cur_cluster);
	sector += (h->position / h->fat->bytes_per_sector) % h->fat->sectors_per_cluster;
	offset = h->position % h->fat->bytes_per_sector;

	for(i = 0; i < size; ) {
		uint16_t chunk = MIN(h->fat->bytes_per_sector - offset, size - i);
		if(chunk < h->fat->bytes_per_sector)
			block_read_sectors(h->fat->dev, sector, 1, sector_buf); 

		memcpy(sector_buf + offset, buf + i, chunk);
		block_write_sectors(h->fat->dev, sector, 1, sector_buf); 
		h->position += chunk;
		i += chunk;
		if((h->position % h->fat->bytes_per_sector) != 0) 
			/* we didn't write until the end of the sector... */
			break;
		offset = 0;
		sector++;
		if(h->root_flag) /* FAT12/16 isn't a cluster chain */
			continue;
		if((sector % h->fat->sectors_per_cluster) == 0) {
			/* Go to next cluster... */
			uint32_t next_cluster = fat_get_next_cluster(h->fat, 
						h->cur_cluster);
			if(next_cluster == fat_eoc(h->fat)) {
				next_cluster = fat_alloc_next_cluster(h->fat,
						h->cur_cluster);
				if(!next_cluster)
					return i;
			}
			h->cur_cluster = next_cluster;
			sector = fat_first_sector_of_cluster(h->fat, 
						h->cur_cluster);
		}
	}

	if(h->dirent_sector && (h->position > h->size)) {
		/* Update directory entry with new size */
		h->size = h->position;
		block_read_sectors(h->fat->dev, h->dirent_sector, 1, sector_buf); 
		struct fat_sdirent *dirent = (void*)&sector_buf[h->dirent_offset];
		__put_le32(&dirent->size, h->size);
		block_write_sectors(h->fat->dev, h->dirent_sector, 1, sector_buf); 
	}

	return i;
}