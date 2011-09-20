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

/* Example to list directory structure.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <assert.h>

#include <sys/stat.h>

#include "openfat.h"

/* Prototypes for blockdev_file.c functions */
extern struct block_device * 
block_device_file_new(const char *filename, const char *mode);
extern void block_device_file_destroy(struct block_device *bldev);

void print_tree(struct fat_vol_handle *vol, struct fat_file_handle *dir, 
		const char *path)
{
	struct dirent ent;
	char tmppath[1024];
	struct fat_file_handle subdir;

	while(fat_readdir(dir, &ent)) {
		if((strcmp(ent.d_name, ".") == 0) || 
		   (strcmp(ent.d_name, "..") == 0))
			continue;
		sprintf(tmppath, "%s/%s", path, ent.d_name);
		puts(tmppath);
		
		if(ent.fat_attr == FAT_ATTR_DIRECTORY) {
			fat_chdir(vol, ent.d_name);
			fat_open(vol, ".", 0, &subdir);
			print_tree(vol, &subdir, tmppath);
			fat_chdir(vol, "..");
		}
	}

}

int main(int argc, char *argv[])
{
	struct block_device *bldev;
	struct fat_vol_handle fat;
	struct fat_file_handle root;
	struct stat st;
	char *rootpath = argc > 2 ? argv[2] : "/";

	bldev = block_device_file_new(argc > 1 ? argv[1] : "fat32.img", "r");
	assert(bldev != NULL);

	fat_vol_init(bldev, &fat);
	fprintf(stderr, "Fat type is FAT%d\n", fat.type);

	if(!fat_path_open(&fat, rootpath, &root)) {
		fprintf(stderr, "Failed to find file: %s\n", rootpath);
		return -1;
	}

	fat_file_stat(&root, &st);
	if(S_ISDIR(st.st_mode)) {
		print_tree(&fat, &root, rootpath[0] == '/' ? rootpath + 1 : rootpath);
	} else {
		char *buf = malloc(st.st_size);
		fat_read(&root, buf, st.st_size);
		fwrite(buf, st.st_size, 1, stdout);
		fflush(stdout);
	}

	block_device_file_destroy(bldev);
}

