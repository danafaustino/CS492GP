/*
 * file:        homework.c
 * description: skeleton file for CS492 file system
 *
 * Credit:
 * 	Peter Desnoyers, November 2016
 * 	Philip Gust, March 2019
 */

#define FUSE_USE_VERSION 27

#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <fuse.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdbool.h>
#include <limits.h>

#include "fsx492.h"
#include "blkdev.h"

/* 
 * disk access - the global variable 'disk' points to a blkdev
 * structure which has been initialized to access the image file.
 *
 * NOTE - blkdev access is in terms of 1024-byte blocks
 */
extern struct blkdev *disk; //see main.c


/*
 * CS492: feel free to add any data structures, external variables, functions you want to add here
*/

static int split(char *p, char *toks[], int n, char *delim)
{
    if (n == 0){
        n = INT_MAX;
    }
    if (toks == NULL){
        // do not alter p if not returning names
        p = strdup(p);
    }
    char *str;
    char *lasts = NULL;
    int i;
    for (i = 0; i < n && (str = strtok_r(p, delim, &lasts)) != NULL; i++){
        p = NULL;
        if (toks != NULL){
            toks[i] = str;
        }
    }
    if (toks == NULL){
        free(p);
    }
    return i;
}
static struct fs_super superblock;
static int inode_used(int inode_num){
	char *inode_bitmap = malloc(FS_BLOCK_SIZE * superblock.inode_map_sz);
	if (inode_bitmap == NULL){
		return -1;
	}
	if (disk->ops->read(disk, 1, superblock.inode_map_sz, inode_bitmap) != SUCCESS){
		free(inode_bitmap);
		return -1;
	}
	int result = inode_bitmap[inode_num / 8] & (1 << (inode_num % 8));
	free(inode_bitmap);
	return result;
}

static int read_inode(int inode_num, struct fs_inode* buf){
	char temp_block[FS_BLOCK_SIZE];
	int block_number = 1 + superblock.inode_map_sz + superblock.block_map_sz + inode_num / INODES_PER_BLK;
	if(disk->ops->read(disk, block_number, 1, temp_block) != SUCCESS){
		return -1;
	}
	int offset_in_block = inode_num % INODES_PER_BLK;
	memcpy(buf, temp_block + offset_in_block * sizeof(struct fs_inode), sizeof(struct fs_inode));
	return 0;
}

enum {MAX_PATH = 4096 };

static int scan_dir_block(int block_number, char *filename){
	struct fs_dirent entries[DIRENTS_PER_BLK];
	if(disk->ops->read(disk, block_number, 1, entries) != SUCCESS){
		return -1;
	}
	for (int i = 0; i < DIRENTS_PER_BLK; i++){
		if (!entries[i].valid){
			continue;
		}
		if (!strcmp(entries[i].name, filename)){
			return entries[i].inode;
		}
	}
	return 0;
}

static int inode_from_full_path(const char *path){
	if(path[0] != '/'){
		fprintf(stderr, "cannot get inode from relative path\n");
		return -ENOENT;
	}
	char temp_path[MAX_PATH];
	strcpy(temp_path, path);
	int number_of_path_components = split(temp_path, NULL, 0, "/");
	if (number_of_path_components == 0){
		return superblock.root_inode;
	}
	char** path_components = malloc(number_of_path_components * sizeof(char*));
	split(temp_path, path_components, number_of_path_components, "/");
	int inode = superblock.root_inode;
	struct fs_inode current_inode;
	for (int i = 0; i < number_of_path_components; i++){
		int inode_used_result = inode_used(inode);
		if (inode_used_result == -1){
			fprintf(stderr, "error reading from disk on line %d\n", __LINE__);
			free(path_components);
			return -1;
		}
		if (inode_used_result == 0 || inode == 0){
			fprintf(stderr, "could not get inode from full path: inode %u not used (or 0)\nwhen trying to find the inode of file '%s'\n", inode, path);
			free(path_components);
			return -ENOENT;
		}
		if (read_inode(inode, &current_inode) != 0){
			free(path_components);
			return -1;
		}
		if (!S_ISDIR(current_inode.mode)){
			free(path_components);
			return -ENOTDIR;
		}
		int scan_result = scan_dir_block(current_inode.direct[0], path_components[i]);
		if (scan_result == -1){
			free(path_components);
			return -1;
		}
		if (scan_result > 0){
			inode = scan_result;
		} else {
			free(path_components);
			return -ENOENT;
		}

	}
	free(path_components);
	return inode;
}

int split_path(const char* path, char *temp_path, char *new_file_name){
	strcpy(temp_path, path);
	int len = strlen(temp_path);
	if (temp_path[len-1] == '/'){
		temp_path[len-1] = '\0';
		len--;
	}
	int i;
	for (i = len-1; i >= 0; i--){
		if (temp_path[i] == '/'){
			i++;
			break;
		}
	}
	if (len - i > FS_FILENAME_SIZE){
		return -ENAMETOOLONG;
	}
	strcpy(new_file_name, temp_path + i);
	temp_path[i] = '\0';
	return 0;
}

static int logical_to_physical(struct fs_inode *inode, int logical){
	if (logical < N_DIRECT){
		return inode->direct[logical];
	} else if (logical - N_DIRECT < PTRS_PER_BLK){
		if (inode->indir_1 == 0){
			return 0;
		}
		uint32_t indir_1_block[PTRS_PER_BLK];
		if (disk->ops->read(disk, inode->indir_1, 1, indir_1_block) != SUCCESS){
			return -EIO;
		}
		return indir_1_block[logical - N_DIRECT];
	} else {
		if (inode->indir_2 == 0){
			return 0;
		}
		uint32_t indir_2_block[PTRS_PER_BLK];
		if (disk->ops->read(disk, inode->indir_2, 1, indir_2_block) != SUCCESS){
			return -EIO;
		}
		if (indir_2_block[(logical - N_DIRECT - PTRS_PER_BLK) / PTRS_PER_BLK] == 0){
			return 0;
		}
		uint32_t second_indir[PTRS_PER_BLK];
		if (disk->ops->read(disk, indir_2_block[(logical - N_DIRECT - PTRS_PER_BLK) / PTRS_PER_BLK], 1, second_indir) != SUCCESS){
			return -EIO;
		}
		return second_indir[(logical - N_DIRECT - PTRS_PER_BLK) % PTRS_PER_BLK];
	}
}

int read_block_of_file(uint32_t logical_block_number, struct fs_inode *inode, void *buf){
	int physical_block_number = logical_to_physical(inode, logical_block_number);
	if (physical_block_number < 0){
		return physical_block_number;
	}
	if (physical_block_number == 0){
		return -1;
	}
	if (disk->ops->read(disk, physical_block_number, 1, buf) != SUCCESS){
		return -EIO;
	}
	return 0;
}

int write_block_to_file(uint32_t block_number, struct fs_inode *inode, void *buf){
	int physical_block_number = logical_to_physical(inode, block_number);
	if (physical_block_number < 0){
		return physical_block_number;
	}
	if (physical_block_number == 0){
		return -1;
	}
	if (disk->ops->write(disk, physical_block_number, 1, buf) != SUCCESS){
		return -EIO;
	}
	return 0;
}

static int allocate_zeroed_block(){
	char *block_bitmap = malloc(FS_BLOCK_SIZE * superblock.block_map_sz);
	if (block_bitmap == NULL){
		return -EIO;
	}
	if (disk->ops->read(disk, 1 + superblock.inode_map_sz, superblock.block_map_sz, block_bitmap) != SUCCESS){
		free(block_bitmap);
		return -EIO;
	}
	int new_block_num = -1;
	for (int i = 0; i < superblock.num_blocks; i++){
		if (!(block_bitmap[i/8] & (1 << (i % 8)))){
			new_block_num = i;
			break;
		}
	}
	if (new_block_num == -1){
		free(block_bitmap);
		return -ENOSPC;
	}
	char zeros[FS_BLOCK_SIZE];
	memset(zeros, 0, FS_BLOCK_SIZE);
	if (disk->ops->write(disk, new_block_num, 1, zeros) != SUCCESS){
		free(block_bitmap);
		return -EIO;
	}
	block_bitmap[new_block_num / 8] |= 1 << (new_block_num % 8);
	if (disk->ops->write(disk, 1 + superblock.inode_map_sz, superblock.block_map_sz, block_bitmap) != SUCCESS){
		free(block_bitmap);
		return -EIO;
	}
	free(block_bitmap);
	return new_block_num;
}

static int put_block_in_file(struct fs_inode *inode, int logical_block, void *buf){
	if (logical_block < N_DIRECT){
		if (inode->direct[logical_block] == 0){
			int temp = allocate_zeroed_block();
			if (temp < 0){
				return temp;
			}
			inode->direct[logical_block] = temp;
		}
		if (disk->ops->write(disk, inode->direct[logical_block], 1, buf) != SUCCESS){
			return -EIO;
		}
	} else if (logical_block - N_DIRECT < PTRS_PER_BLK){
		if (inode->indir_1 == 0){
			int temp = allocate_zeroed_block();
			if (temp < 0){
				return temp;
			}
			inode->indir_1 = temp;
		}
		uint32_t indir_1_block[PTRS_PER_BLK];
		if (disk->ops->read(disk, inode->indir_1, 1, indir_1_block) != SUCCESS){
			return -EIO;
		}
		if (indir_1_block[logical_block - N_DIRECT] == 0){
			int temp = allocate_zeroed_block();
			if (temp < 0){
				return temp;
			}
			indir_1_block[logical_block - N_DIRECT] = temp;
		}
		if (disk->ops->write(disk, indir_1_block[logical_block - N_DIRECT], 1, buf) != SUCCESS){
			return -EIO;
		}
		if (disk->ops->write(disk, inode->indir_1, 1, indir_1_block) != SUCCESS){
			return -EIO;
		}
	} else {
		if (inode->indir_2 == 0){
			int temp = allocate_zeroed_block();
			if (temp < 0){
				return temp;
			}
			inode->indir_2 = temp;
		}
		uint32_t indir_2_block[PTRS_PER_BLK];
		if (disk->ops->read(disk, inode->indir_2, 1, indir_2_block) != SUCCESS){
			return -EIO;
		}
		const uint32_t index_in_indir_2 = (logical_block - N_DIRECT - PTRS_PER_BLK) / PTRS_PER_BLK;
		if (indir_2_block[index_in_indir_2] == 0){
			int temp = allocate_zeroed_block();
			if (temp < 0){
				return temp;
			}
			indir_2_block[index_in_indir_2] = temp;
		}
		uint32_t second_indir[PTRS_PER_BLK];
		if (disk->ops->read(disk, indir_2_block[index_in_indir_2], 1, second_indir) != SUCCESS){
			return -EIO;
		}
		const uint32_t index_in_second_indir = (logical_block - N_DIRECT - PTRS_PER_BLK) % PTRS_PER_BLK;
		if (second_indir[index_in_second_indir] == 0){
			int temp = allocate_zeroed_block();
			if (temp < 0){
				return temp;
			}
			second_indir[index_in_indir_2] = temp;
		}
		if (disk->ops->write(disk, second_indir[index_in_second_indir], 1, buf) != SUCCESS){
			return -EIO;
		}
		if (disk->ops->write(disk, indir_2_block[index_in_indir_2], 1, second_indir) != SUCCESS){
			return -EIO;
		}
		if (disk->ops->write(disk, inode->indir_2, 1, indir_2_block) != SUCCESS){
			return -EIO;
		}
	}
	return 0;
}


/* 
 * CS492: FUSE functions
*/


/*
 * init - this is called once by the FUSE framework at startup.
 *
 * This is a good place to read in the super-block and set up any
 * global variables you need. You don't need to worry about the
 * argument or the return value.
 *
 * @param conn: fuse connection information - unused
 * @return: unused - returns NULL
*/

void* fs_init(struct fuse_conn_info *conn)
{
	int retval = disk->ops->read(disk, 0, 1, &superblock);
	if(retval != SUCCESS){
		fprintf(stderr, "fs_init: got return value of %d when reading the superblock\n", retval);
		abort();
	}
	if (superblock.magic != FS_MAGIC){
		fprintf(stderr, "fs_init: superblocks contains wrong magic number, probably corrupt\n");
	}
	if (disk->ops->num_blocks(disk) != superblock.num_blocks){
		fprintf(stderr, "fs_init: superblock contains wrong number of blocks, probably corrupt\n");
	}
	return NULL;
}


/*
 * getattr - get file or directory attributes. For a description of
 * the fields in 'struct stat', see 'man lstat'.
 *
 * Note: you can handle some fields as follows:
 * st_nlink: always set to 1
 * st_atime, st_ctime: set to same value as st_mtime
 *
 * @param path: the file path
 * @param sb: pointer to stat struct
 *
 * @return: 0 if successful, or -error number
 * 	-ENOENT  - a component of the path is not present
 *	-ENOTDIR - an intermediate component of path not a directory
*/
static int fs_getattr(const char *path, struct stat *sb)
{
	if (path[0] != '/'){
		fprintf(stderr, "Error: stat must be used with an absolute path\n");
		return -EINVAL;
	}
	int inode_number_of_file = inode_from_full_path(path);
	if (inode_number_of_file == -1){
		return -EIO;
	}
	if (inode_number_of_file < 0){
		return inode_number_of_file;
	}
	struct fs_inode inode_of_file;
	if (read_inode(inode_number_of_file, &inode_of_file) != 0){
		return -EIO;
	}
	sb->st_dev = 0;
	sb->st_ino = inode_number_of_file;
	sb->st_mode = inode_of_file.mode;
	sb->st_nlink = 1;
	sb->st_uid = inode_of_file.uid;
	sb->st_gid = inode_of_file.gid;
	sb->st_rdev = 0;
	sb->st_size = inode_of_file.size;
	sb->st_blksize = FS_BLOCK_SIZE;
	sb->st_blocks = inode_of_file.size / 512 + (inode_of_file.size % 512 != 0);
	sb->st_ctime = inode_of_file.ctime;
	sb->st_mtime = inode_of_file.mtime;
	sb->st_atime = inode_of_file.mtime;
	return 0;
}

/*
 * opendir - open file directory
 *
 * You can save information about the open directory in fi->fh. 
 * If you allocate memory, free it in fs_releasedir.
 *
 * @param path: the file path
 * @param fi: fuse file system information
 *
 * @return: 0 if successful, or -error number
 *	-ENOENT  - a component of the path is not present
 *	-ENOTDIR - an intermediate component of path not a directory
*/

static int fs_opendir(const char *path, struct fuse_file_info *fi)
{
	int inode_number = inode_from_full_path(path);
	if (inode_number == -1){
		return -EIO;
	}
	if (inode_number < 0){
		return inode_number;
	}
	struct fs_inode inode;
	if (read_inode(inode_number, &inode) != 0){
		return -EIO;
	}
	if (!S_ISDIR(inode.mode)){
		return -ENOTDIR; 
	}
	return 0;
} 


/*
    readdir - get directory contents

    For each entry in the directory, invoke the 'filler' function, 
	which is passed as a function pointer, as follows:
    filler(buf, <name>, <statbuf>, 0) 
	where <statbuf> is a struct stat, just like in getattr.

    @param path: the directory path
    @param ptr: filler buf pointer
    @param filler filler function to call for each entry
    @param offset: the file offset -- unused
    @param fi: the fuse file information -- you do not have to use it

    @return: 0 if successful, or -error number
    	-ENOENT  - a component of the path is not present
    	-ENOTDIR - an intermediate component of path not a directory
*/
static int fs_readdir(const char *path, void *ptr, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
	int inode_number = inode_from_full_path(path);
	if (inode_number == -1){
		return -EIO;
	}
	if (inode_number < 0){
		return inode_number;
	}
	struct fs_inode inode;
	if (read_inode(inode_number, &inode) != 0){
		return -EIO;
	}
	if (!S_ISDIR(inode.mode)){
		return -ENOTDIR; 
	}
	struct fs_dirent entries[DIRENTS_PER_BLK];
	if (disk->ops->read(disk, inode.direct[0], 1, entries) != SUCCESS){
		return -EIO;
	}
	for (int i = 0; i < DIRENTS_PER_BLK; i++){
		if (!entries[i].valid){
			continue;
		}
		struct fs_inode inode_of_entry;
		if (read_inode(entries[i].inode, &inode_of_entry) != 0){
			return -EIO;
		}
		struct stat sb;
		sb.st_dev = 0; 
		sb.st_ino = entries[i].inode;
		sb.st_mode = inode_of_entry.mode;
		sb.st_nlink = 1;
		sb.st_uid = inode_of_entry.uid;
		sb.st_gid = inode_of_entry.gid;
		sb.st_rdev = 0;
		sb.st_size = inode_of_entry.size;
		sb.st_blksize = FS_BLOCK_SIZE;
		sb.st_blocks = inode_of_entry.size / 512 + (inode_of_entry.size % 512 != 0); 
		sb.st_ctime = inode_of_entry.ctime;
		sb.st_mtime = inode_of_entry.mtime;
		sb.st_atime = inode_of_entry.mtime;
		filler(ptr, entries[i].name, &sb, 0);
	}
	return 0;
}

/*
 * Release resources when directory is closed.
 * If you allocate memory in fs_opendir, free it here.
 *
 * @param path: the directory path
 * @param fi: fuse file system information -- you do not have to use it
 *
 * @return: 0 if successful, or -error number
 *	-ENOENT  - a component of the path is not present
 *	-ENOTDIR - an intermediate component of path not a directory
*/
static int fs_releasedir(const char *path, struct fuse_file_info *fi)
{
	int inode_number = inode_from_full_path(path);
	if (inode_number == -1){
		return -EIO;
	}
	if (inode_number < 0){
		return inode_number; 
	}
	struct fs_inode inode;
	if (read_inode(inode_number, &inode) != 0){
		return -EIO;
	}
	if (!S_ISDIR(inode.mode)){
		return -ENOTDIR;
	}
	return 0;
}

/*
 * mknod - create a new file with permissions (mode & 01777). Behavior undefined when mode bits other than the low 9 bits are used.
 * 	
 * @param path: the file path
 * @param mode: indicating block or character-special file
 * @param dev: the character or block I/O device specification - you do not have to use it
 * 			 
 * @return: 0 if successful, or -error number
 * 	-ENOTDIR  - component of path not a directory
 * 	-EEXIST   - file already exists
 * 	-ENOSPC   - free inode not available
 * 	-ENOSPC   - results in >32 entries in directory
*/
static int fs_mknod(const char *path, mode_t mode, dev_t dev)
{
	if (path[0] == '\0'){
		return -EINVAL;
	}
	if (!strcmp(path, "/")){
		return -EEXIST;
	}
	char temp_path[MAX_PATH];
	char new_file_name[FS_FILENAME_SIZE];
	if (split_path(path, temp_path, new_file_name) == -ENAMETOOLONG){
		return -ENAMETOOLONG;
	}
	int inode_num_of_dir = inode_from_full_path(temp_path);
	if (inode_num_of_dir == -1){
		return -EIO;
	}
	if (inode_num_of_dir < 0){
		return inode_num_of_dir;
	}
	struct fs_inode dir_inode;
	if (read_inode(inode_num_of_dir, &dir_inode) != 0){
		return -EIO;
	}
	if (!S_ISDIR(dir_inode.mode)){
		return -ENOTDIR;
	}
	struct fs_dirent entries[DIRENTS_PER_BLK];
	if (disk->ops->read(disk, dir_inode.direct[0], 1, entries) != SUCCESS){
		return -EIO;
	}
	bool dir_has_space = false;
	int entry_index = -1;
	for (int i = 0; i < DIRENTS_PER_BLK; i++){
		if (!entries[i].valid){
			if (!dir_has_space){
				dir_has_space = true;
				entry_index = i;
			}
			continue;
		}
		if (!strcmp(entries[i].name, new_file_name)){
			return -EEXIST;
		}
	}
	if (!dir_has_space){
		return -ENOSPC;
	}
	int new_inode_num = -1;
	char *inode_bitmap = malloc(FS_BLOCK_SIZE * superblock.inode_map_sz);
	if (inode_bitmap == NULL){
		return -EIO;
	}
	if (disk->ops->read(disk, 1, superblock.inode_map_sz, inode_bitmap) != SUCCESS){
		free(inode_bitmap);
		return -EIO;
	}
	for (int i = 0; i < INODES_PER_BLK * superblock.inode_region_sz; i++){
		if (!(inode_bitmap[i / 8] & (1 << (i % 8)))){
			new_inode_num = i;
			break;
		}
	}
	if (new_inode_num == -1){
		free(inode_bitmap);
		return -ENOSPC;
	}
	inode_bitmap[new_inode_num / 8] |= 1 << (new_inode_num % 8);
	if (disk->ops->write(disk, 1, superblock.inode_map_sz, inode_bitmap) != SUCCESS){
		free(inode_bitmap);
		return -EIO;
	}
	free(inode_bitmap);
	struct fs_inode new_inode = {
		.uid = fuse_get_context()->uid,
		.gid = fuse_get_context()->gid,
		.mode = (mode & 01777 & ~(fuse_get_context()->umask)) | S_IFREG,
		.ctime = time(NULL),
		.mtime = time(NULL),
		.size = 0,
		.indir_1 = 0,
		.indir_2 = 0,
	};
	for (int i = 0; i < N_DIRECT; i++){
		new_inode.direct[i] = 0;
	}
	int block_number_that_contains_new_inode =
		1 + superblock.inode_map_sz + superblock.block_map_sz
		+ (new_inode_num / INODES_PER_BLK);
	struct fs_inode block_containing_new_inode[INODES_PER_BLK];
	if (disk->ops->read(disk, block_number_that_contains_new_inode, 1, block_containing_new_inode) != SUCCESS){
		return -EIO;
	}
	memcpy(&block_containing_new_inode[new_inode_num % INODES_PER_BLK],
		&new_inode, sizeof(struct fs_inode));
	if (disk->ops->write(disk, block_number_that_contains_new_inode, 1, block_containing_new_inode) != SUCCESS){
		return -EIO;
	}
	entries[entry_index].valid = 1;
	entries[entry_index].isDir = 0;
	entries[entry_index].inode = new_inode_num;
	strcpy(entries[entry_index].name, new_file_name);
	if (disk->ops->write(disk, dir_inode.direct[0], 1, entries) != SUCCESS){
		fprintf(stderr, "Error updating directory %s to contain new file %s, after creating the inode for it. Disk is probably corrupt.\n", temp_path, new_file_name);
		return -EIO;
	}
	return 0;
}

/*
 * 	mkdir - create a directory with the given mode. Behavior undefined when mode bits other than the low 9 bits are used.
 *
 * 	@param path: path to directory
 * 	@param mode: the mode for the new directory
 *
 * 	@return: 0 if successful, or -error number
 * 		-ENOTDIR  - component of path not a directory
 * 		-EEXIST   - file already exists
 * 		-ENOSPC   - free inode not available
 * 		-ENOSPC   - results in >32 entries in directory
*/
static int fs_mkdir(const char *path, mode_t mode)
{
	if (path[0] == '\0'){
		return -EINVAL;
	}
	if (!strcmp(path, "/")){
		return -EEXIST;
	}
	char temp_path[MAX_PATH];
	char new_dir_name[FS_FILENAME_SIZE];
	if (split_path(path, temp_path, new_dir_name) == -ENAMETOOLONG){
		return -ENAMETOOLONG;
	}
	int inode_num_of_containing_dir = inode_from_full_path(temp_path);
	if (inode_num_of_containing_dir == -1){
		return -EIO;
	}
	if (inode_num_of_containing_dir < 0){
		return inode_num_of_containing_dir;
	}
	struct fs_inode containing_dir_inode;
	if (read_inode(inode_num_of_containing_dir, &containing_dir_inode) != 0){
		return -EIO;
	}
	if (!S_ISDIR(containing_dir_inode.mode)){
		return -ENOTDIR;
	}
	struct fs_dirent entries[DIRENTS_PER_BLK];
	if (disk->ops->read(disk, containing_dir_inode.direct[0], 1, entries) != SUCCESS){
		return -EIO;
	}
	bool dir_has_space = false;
	int entry_index = -1;
	for (int i = 0; i < DIRENTS_PER_BLK; i++){
		if (!entries[i].valid){
			if (!dir_has_space){
				dir_has_space = true;
				entry_index = i;
			}
			continue;
		}
		if (!strcmp(entries[i].name, new_dir_name)){
			return -EEXIST;
		}
	}
	if (!dir_has_space){
		return -ENOSPC;
	}
	int new_inode_num = -1;
	char *inode_bitmap = malloc(FS_BLOCK_SIZE * superblock.inode_map_sz);
	if (inode_bitmap == NULL){
		return -EIO;
	}
	if (disk->ops->read(disk, 1, superblock.inode_map_sz, inode_bitmap) != SUCCESS){
		free(inode_bitmap);
		return -EIO;
	}
	for (int i = 0; i < INODES_PER_BLK * superblock.inode_region_sz; i++){
		if (!(inode_bitmap[i / 8] & (1 << (i % 8)))){
			new_inode_num = i;
			break;
		}
	}
	if (new_inode_num == -1){
		free(inode_bitmap);
		return -ENOSPC;
	}
	struct fs_inode new_inode = {
		.uid = fuse_get_context()->uid,
		.gid = fuse_get_context()->gid,
		.mode = (mode & 01777 & ~(fuse_get_context()->umask)) | S_IFDIR,
		.ctime = time(NULL),
		.mtime = time(NULL),
		.size = 0,
		.indir_1 = 0,
		.indir_2 = 0,
	};
	for (int i = 1; i < N_DIRECT; i++){
		new_inode.direct[i] = 0;
	}
	int new_block_num = -1;
	char *block_bitmap = malloc(FS_BLOCK_SIZE * superblock.block_map_sz);
	if (block_bitmap == NULL){
		free(inode_bitmap);
		return -EIO;
	}
	if (disk->ops->read(disk, 1 + superblock.inode_map_sz, superblock.block_map_sz, block_bitmap) != SUCCESS){
		free(block_bitmap);
		free(inode_bitmap);
		return -EIO;
	}
	for (int i = 0; i < superblock.num_blocks; i++){
		if (!(block_bitmap[i / 8] & (1 << (i % 8)))){
			new_block_num = i;
			break;
		}
	}
	if (new_block_num == -1){
		free(block_bitmap);
		free(inode_bitmap);
		return -ENOSPC;
	}
	block_bitmap[new_block_num / 8] |= 1 << (new_block_num % 8);
	if (disk->ops->write(disk, 1 + superblock.inode_map_sz, superblock.block_map_sz, block_bitmap) != SUCCESS){
		free(block_bitmap);
		free(inode_bitmap);
		return -EIO;
	}
	free(block_bitmap);
	inode_bitmap[new_inode_num / 8] |= 1 << (new_inode_num % 8);
	if (disk->ops->write(disk, 1, superblock.inode_map_sz, inode_bitmap) != SUCCESS){
		free(inode_bitmap);
		return -EIO;
	}
	free(inode_bitmap);
	struct fs_dirent zeros[FS_BLOCK_SIZE];
	memset(zeros, 0, FS_BLOCK_SIZE);
	if (disk->ops->write(disk, new_block_num, 1, zeros) != SUCCESS){
		return -EIO;
	}
	new_inode.direct[0] = new_block_num;
	int block_number_that_contains_new_inode =
		1 + superblock.inode_map_sz + superblock.block_map_sz
		+ (new_inode_num / INODES_PER_BLK);
	struct fs_inode block_containing_new_inode[INODES_PER_BLK];
	if (disk->ops->read(disk, block_number_that_contains_new_inode, 1, block_containing_new_inode) != SUCCESS){
		return -EIO;
	}
	memcpy(&block_containing_new_inode[new_inode_num % INODES_PER_BLK],
		&new_inode, sizeof(struct fs_inode));
	if (disk->ops->write(disk, block_number_that_contains_new_inode, 1, block_containing_new_inode) != SUCCESS){
		return -EIO;
	}
	entries[entry_index].valid = 1;
	entries[entry_index].isDir = 1;
	entries[entry_index].inode = new_inode_num;
	strcpy(entries[entry_index].name, new_dir_name);
	if (disk->ops->write(disk, containing_dir_inode.direct[0], 1, entries) != SUCCESS){
		fprintf(stderr, "Error updating directory %s to contain new directory %s, after creating the inode for it. Disk is probably corrupt.\n", temp_path, new_dir_name);
		return -EIO;
	}
	return 0;
}


static void unset_block_bit(int block_num, char *block_bitmap){
	if (block_num != 0){
		block_bitmap[(block_num) / 8] &= ~(1 << (block_num % 8));
	}
}

static int unset_bits(struct fs_inode *inode, char *block_bitmap){
	for (int i = 0; i < N_DIRECT; i++){
		if (inode->direct[i] == 0){
			return 0;
		}
		unset_block_bit(inode->direct[i], block_bitmap);
	}

	if (inode->indir_1 == 0){
		return 0;
	}
	uint32_t indir_1_block[PTRS_PER_BLK];
	if (disk->ops->read(disk, inode->indir_1, 1, indir_1_block) != SUCCESS){
		return -EIO;
	}
	for (int i = 0; i < PTRS_PER_BLK; i++){
		if (indir_1_block[i] == 0){
			return 0;
		}
		unset_block_bit(indir_1_block[i], block_bitmap);
	}
	unset_block_bit(inode->indir_1, block_bitmap);

	if (inode->indir_2 == 0){
		return 0;
	}
	uint32_t indir_2_block[PTRS_PER_BLK];
	if (disk->ops->read(disk, inode->indir_2, 1, indir_2_block) != SUCCESS){
		return -EIO;
	}
	for (int i = 0; i < PTRS_PER_BLK; i++){
		if (indir_2_block[i] == 0){
			return 0;
		}
		uint32_t second_level_block[PTRS_PER_BLK];
		if (disk->ops->read(disk, indir_2_block[i], 1, second_level_block) != SUCCESS){
			return -EIO;
		}
		for (int j = 0; j < PTRS_PER_BLK; j++){
			if (second_level_block[j] == 0){
				return 0;
			}
			unset_block_bit(second_level_block[j], block_bitmap);
		}
		unset_block_bit(indir_2_block[i], block_bitmap);
	}
	unset_block_bit(inode->indir_2, block_bitmap);
	return 0;
}

/*
 * unlink - delete a file
 *
 * @param path: path to file
 *
 * @return 0 if successful, or error value
 *	-ENOENT   - file does not exist
 * 	-ENOTDIR  - component of path not a directory
 * 	-EISDIR   - cannot unlink a directory
*/
static int fs_unlink(const char *path)
{
	if (path[0] == '\0'){
		return -EINVAL;
	}
	if (!strcmp(path, "/")){
		return -EISDIR;
	}
	char temp_path[MAX_PATH];
	char new_file_name[FS_FILENAME_SIZE];
	if (split_path(path, temp_path, new_file_name) == -ENAMETOOLONG){
		return -ENOENT; 
	}
	int inode_num_of_dir = inode_from_full_path(temp_path);
	if (inode_num_of_dir == -1){
		return -EIO;
	}
	if (inode_num_of_dir < 0){
		return inode_num_of_dir;
	}
	struct fs_inode dir_inode;
	if (read_inode(inode_num_of_dir, &dir_inode) != 0){
		return -EIO;
	}
	if (!S_ISDIR(dir_inode.mode)){
		return -ENOTDIR;
	}
	struct fs_dirent entries[DIRENTS_PER_BLK];
	if (disk->ops->read(disk, dir_inode.direct[0], 1, entries) != SUCCESS){
		return -EIO;
	}
	int entry_index = -1;
	for (int i = 0; i < DIRENTS_PER_BLK; i++){
		if (entries[i].valid && !strcmp(entries[i].name, new_file_name)){
			entry_index = i;
			break;
		}
	}
	if (entry_index == -1){
		return -ENOENT;
	}

	struct fs_inode inode_of_file_to_be_removed;
	if (read_inode(entries[entry_index].inode, &inode_of_file_to_be_removed) != 0){
		return -EIO;
	}
	if (S_ISDIR(inode_of_file_to_be_removed.mode)){
		return -EISDIR;
	}
	char *block_bitmap = malloc(FS_BLOCK_SIZE * superblock.block_map_sz);
	if (block_bitmap == NULL){
		return -EIO;
	}
	if (disk->ops->read(disk, 1 + superblock.inode_map_sz, superblock.block_map_sz, block_bitmap) != SUCCESS){
		free(block_bitmap);
		return -EIO;
	}
	if (unset_bits(&inode_of_file_to_be_removed, block_bitmap) != 0){
		free(block_bitmap);
		return -EIO;
	}
	char *inode_bitmap = malloc(FS_BLOCK_SIZE * superblock.inode_map_sz);
	if (inode_bitmap == NULL){
		free(block_bitmap);
		return -EIO;
	}
	if (disk->ops->read(disk, 1, superblock.inode_map_sz, inode_bitmap) != SUCCESS){
		free(inode_bitmap);
		free(block_bitmap);
		return -EIO;
	}
	inode_bitmap[entries[entry_index].inode / 8] &= ~(1 << (entries[entry_index].inode % 8));
	entries[entry_index].valid = 0;
	if (disk->ops->write(disk, 1 + superblock.inode_map_sz, superblock.block_map_sz, block_bitmap) != SUCCESS){
		free(block_bitmap);
		free(inode_bitmap);
		return -EIO;
	}
	free(block_bitmap);
	if (disk->ops->write(disk, 1, superblock.inode_map_sz, inode_bitmap) != SUCCESS){
		free(inode_bitmap);
		fprintf(stderr, "Error updating inode bitmap when deleting file '%s'. Disk is probably corrupt.\n", path);
		return -EIO;
	}
	free(inode_bitmap);
	if (disk->ops->write(disk, dir_inode.direct[0], 1, entries) != SUCCESS){
		fprintf(stderr, "Error updating contents of directory '%s' when deleting '%s'. This directory is now corrupt.\n", temp_path, new_file_name);
		return -EIO;
	}
	return 0;
}

/*
 * rmdir - remove a directory
 *
 * @param path: the path of the directory
 *
 * @return: 0 if successful, or -error number
 * 	-ENOENT   - file does not exist
 *  	-ENOTDIR  - component of path not a directory
 *  	-ENOTDIR  - path not a directory
 *  	-ENOEMPTY - directory not empty
*/

static int fs_rmdir(const char *path)
{
	if (path[0] == '\0'){
		return -EINVAL;
	}
	if (!strcmp(path, "/")){
		return -ENOTEMPTY;
	}
	char temp_path[MAX_PATH];
	char dir_name[FS_FILENAME_SIZE];
	if (split_path(path, temp_path, dir_name) == -ENAMETOOLONG){
		return -ENOENT;
	}

	int inode_num_of_containing_dir = inode_from_full_path(temp_path);
	if (inode_num_of_containing_dir == -1){
		return -EIO;
	}
	if (inode_num_of_containing_dir < 0){
		return inode_num_of_containing_dir;
	}
	struct fs_inode containing_dir_inode;
	if (read_inode(inode_num_of_containing_dir, &containing_dir_inode) != 0){
		return -EIO;
	}
	if (!S_ISDIR(containing_dir_inode.mode)){
		return -ENOTDIR;
	}
	struct fs_dirent entries[DIRENTS_PER_BLK];
	if (disk->ops->read(disk, containing_dir_inode.direct[0], 1, entries) != SUCCESS){
		return -EIO;
	}
	int entry_index = -1;
	for (int i = 0; i < DIRENTS_PER_BLK; i++){
		if (entries[i].valid && !strcmp(entries[i].name, dir_name)){
			entry_index = i;
			break;
		}
	}
	if (entry_index == -1){
		return -ENOENT;
	}
	
	struct fs_inode inode_of_dir_to_be_removed;
	if (read_inode(entries[entry_index].inode, &inode_of_dir_to_be_removed) != 0){
		return -EIO;
	}
	if (!S_ISDIR(inode_of_dir_to_be_removed.mode)){
		return -ENOTDIR;
	}
	struct fs_dirent entries_of_dir_to_be_removed[DIRENTS_PER_BLK];
	if (disk->ops->read(disk, inode_of_dir_to_be_removed.direct[0], 1, entries_of_dir_to_be_removed) != SUCCESS){
		return -EIO;
	}
	for (int i = 0; i < DIRENTS_PER_BLK; i++){
		if (entries_of_dir_to_be_removed[i].valid){
			return -ENOTEMPTY;
		}
	}
	char *block_bitmap = malloc(FS_BLOCK_SIZE * superblock.block_map_sz);
	if (block_bitmap == NULL){
		return -EIO;
	}
	if (disk->ops->read(disk, 1 + superblock.inode_map_sz, superblock.block_map_sz, block_bitmap) != 0){
		free(block_bitmap);
		return -EIO;
	}
	unset_block_bit(inode_of_dir_to_be_removed.direct[0], block_bitmap);
	
	char *inode_bitmap = malloc(FS_BLOCK_SIZE * superblock.inode_map_sz);
	if (inode_bitmap == NULL){
		free(block_bitmap);
		return -EIO;
	}
	if (disk->ops->read(disk, 1, superblock.inode_map_sz, inode_bitmap) != SUCCESS){
		free(inode_bitmap);
		free(block_bitmap);
		return -EIO;
	}
	inode_bitmap[entries[entry_index].inode / 8] &= ~(1 << (entries[entry_index].inode % 8));
	entries[entry_index].valid = 0;
	if (disk->ops->write(disk, 1 + superblock.inode_map_sz, superblock.block_map_sz, block_bitmap) != SUCCESS){
		free(block_bitmap);
		free(inode_bitmap);
		return -EIO;
	}
	free(block_bitmap);
	if (disk->ops->write(disk, 1, superblock.inode_map_sz, inode_bitmap) != SUCCESS){
		free(inode_bitmap);
		fprintf(stderr, "Error updating inode bitmap when deleting directory '%s'. Disk is probably corrupt.\n", path);
		return -EIO;
	}
	free(inode_bitmap);
	if (disk->ops->write(disk, containing_dir_inode.direct[0], 1, entries) != SUCCESS){
		fprintf(stderr, "Error updating contents of directory '%s' when deleting '%s'. This directory is now corrupt.\n", temp_path, dir_name);
		return -EIO;
	}
	return 0;
}

/*
 * rename - rename a file or directory. You can assume the destination 
 * and the source share the same path-prefix so the renaming changes
 * the name without moving the file or directory into another file or
 * directory.
 *
 *  
 *  @param src_path: the source path
 *  @param dst_path: the destination path
 *
 *  @return: 0 if successful, or -error number
 *  -ENOENT - source file or directory does not exist
 *  -ENOTDIR - component of source or target path not a directory
 *  -EEXIST - destination already exists
 *  -EINVAL - source and destination not in the same directory
*/
static int fs_rename(const char *src_path, const char *dst_path)
{
	if (src_path[0] == '\0' || dst_path[0] == '\0'){
		return -EINVAL;
	}
	if (!strcmp(src_path, "/") || !strcmp(dst_path, "/")){
		return -EINVAL;
	}
	char src_prefix[MAX_PATH];
	char src_suffix[FS_FILENAME_SIZE];
	if (split_path(src_path, src_prefix, src_suffix) == -ENAMETOOLONG){
		return -ENOENT; 
	}
	char dest_prefix[MAX_PATH];
	char dest_suffix[FS_FILENAME_SIZE];
	if (split_path(dst_path, dest_prefix, dest_suffix) == -ENAMETOOLONG){
		return -ENAMETOOLONG;
	}
	if (strcmp(src_prefix, dest_prefix)){
		return -EINVAL; 
	}
	int inode_num_of_containing_dir = inode_from_full_path(src_prefix);
	if (inode_num_of_containing_dir == -1){
		return -EIO;
	}
	if (inode_num_of_containing_dir < 0){
		return inode_num_of_containing_dir;
	}
	struct fs_inode containing_dir_inode;
	if (read_inode(inode_num_of_containing_dir, &containing_dir_inode) != 0){
		return -EIO;
	}
	if (!S_ISDIR(containing_dir_inode.mode)){
		return -ENOTDIR;
	}
	struct fs_dirent entries[DIRENTS_PER_BLK];
	if (disk->ops->read(disk, containing_dir_inode.direct[0], 1, entries) != SUCCESS){
		return -EIO;
	}
	int entry_index = -1;
	for (int i = 0; i < DIRENTS_PER_BLK; i++){
		if (!entries[i].valid){
			continue;
		}
		if (!strcmp(entries[i].name, dest_suffix)){
			return -EEXIST;
		}
		if (entry_index == -1 && !strcmp(entries[i].name, src_suffix)){
			entry_index = i;
		}
	}
	if (entry_index == -1){
		return -ENOENT;
	}
	strcpy(entries[entry_index].name, dest_suffix);
	if (disk->ops->write(disk, containing_dir_inode.direct[0], 1, entries) != SUCCESS){
		return -EIO;
	}
	return 0;
}

/*
 * chmod - change file permissions
 *
 * @param path: the file or directory path
 * @param mode: the mode_t mode value -- see man 'chmod'
 * for description
 *
 *  @return: 0 if successful, or -error number
 *  	-ENOENT   - file does not exist
 *  	-ENOTDIR  - component of path not a directory
*/
static int fs_chmod(const char *path, mode_t mode)
{
	if (path[0] == '\0'){
		return -EINVAL;
	}
	int inode_num = inode_from_full_path(path);
	if (inode_num == -1){
		return -EIO;
	}
	if (inode_num < 0){
		return inode_num;
	}
	int block_number_that_contains_inode = 1 + superblock.inode_map_sz + superblock.block_map_sz + (inode_num / INODES_PER_BLK);
	struct fs_inode block_containing_inode[INODES_PER_BLK];
	if (disk->ops->read(disk, block_number_that_contains_inode, 1, block_containing_inode) != SUCCESS){
		return -EIO;
	}
	block_containing_inode[inode_num % INODES_PER_BLK].mode =(block_containing_inode[inode_num % INODES_PER_BLK].mode & ~0777) | (mode & 0777);
	if (disk->ops->write(disk, block_number_that_contains_inode, 1, block_containing_inode) != SUCCESS){
		return -EIO;
	}
	return 0;
}

/*
 * Open a filesystem file or directory path.
 *
 * @param path: the path
 * @param fuse: file info data
 *
 * @return: 0 if successful, or -error number
 *	-ENOENT - file does not exist
 *	-ENOTDIR - component of path not a directory
*/
static int fs_open(const char *path, struct fuse_file_info *fi)
{
	int inode_num = inode_from_full_path(path);
	if (inode_num == -1){
		return -EIO;
	}
	if (inode_num < 0){
		return inode_num;
	}
	struct fs_inode inode;
	if (read_inode(inode_num, &inode) != 0){
		return -EIO;
	}
	if (S_ISDIR(inode.mode)){
		return -EISDIR;
	}
	return 0;
}

/*
 * read - read data from an open file.
 *
 * 	@param path: the path to the file
 * 	@param buf: the buffer to keep the data
 * 	@param len: the number of bytes to read
 * 	@param offset: the location to start reading at
 * 	@param fi: fuse file info
 *
 * 	@return: return exactly the number of bytes requested, except:
 * 	- if offset >= file len, return 0
 * 	- if offset+len > file len, return bytes from offset to EOF
 * 	- on error, return <0
 * 		-ENOENT  - file does not exist
 * 		-ENOTDIR - component of path not a directory
 * 		-EIO     - error reading block
*/
static int fs_read(const char *path, char *buf, size_t len, off_t offset,
		    struct fuse_file_info *fi)
{
	int inode_num = inode_from_full_path(path);
	if (inode_num == -1){
		return -EIO;
	}
	if (inode_num < 0){
		return inode_num;
	}
	struct fs_inode inode;
	if (read_inode(inode_num, &inode) != 0){
		return -EIO;
	}
	if (S_ISDIR(inode.mode)){
		return -EISDIR;
	}
	int32_t file_size = inode.size;
	if (offset >= file_size){
		return 0;
	}
	if (offset + len > file_size){
		len = file_size - offset;
	}
	size_t first_block_num = offset / FS_BLOCK_SIZE;
	size_t last_block_num = (offset + len) / FS_BLOCK_SIZE;
	char *all_blocks = malloc((last_block_num - first_block_num + 1) * FS_BLOCK_SIZE);
	if (all_blocks == NULL){
		return -EIO;
	}
	int current_block_num = 0;
	for (int i = first_block_num; i <= last_block_num; i++){
		if (read_block_of_file(i, &inode, all_blocks + (current_block_num * FS_BLOCK_SIZE)) == -EIO){
			free(all_blocks);
			return -EIO;
		}
		current_block_num++;
	}
	memcpy(buf, all_blocks + (offset % FS_BLOCK_SIZE), len);
	free(all_blocks);
	return len;
}

/*
 * write - write data to a file
 *
 * @param path: the file path
 * @param buf: the buffer to write
 * @param len: the number of bytes to write
 * @param offset: the offset to starting writing at
 * @param fi: the Fuse file info for writing
 *
 * @return: It should return exactly the number of bytes requested, except on error:
 * 	-ENOENT  - file does not exist
 *	-ENOTDIR - component of path not a directory
 *	-EINVAL  - if 'offset' is greater than current file length. (POSIX semantics support the creation of files with "holes" in them, but we don't)
*/
static int fs_write(const char *path, const char *buf, size_t len, off_t offset, struct fuse_file_info *fi) {
	int inode_num = inode_from_full_path(path);
	if (inode_num == -1){
		return -EIO;
	}
	if (inode_num < 0){
		return inode_num;
	}
	struct fs_inode inode;
	if (read_inode(inode_num, &inode) != 0){
		return -EIO;
	}
	if (S_ISDIR(inode.mode)){
		return -EISDIR;
	}
	if (offset > inode.size){
		return -EINVAL;
	}
	if (len == 0){
		return 0;
	}
	const int MAX_FILE_SIZE = FS_BLOCK_SIZE * N_DIRECT + PTRS_PER_BLK * FS_BLOCK_SIZE + PTRS_PER_BLK * PTRS_PER_BLK * FS_BLOCK_SIZE;
	if (offset == MAX_FILE_SIZE){
		if (len == 0){
			return 0;
		} else {
			return -EFBIG;
		}
	}
	if (offset + len >= MAX_FILE_SIZE){
		len = MAX_FILE_SIZE - offset;
	}
	uint32_t first_logical_block_num = offset / FS_BLOCK_SIZE;
	uint32_t last_logical_block_num = (offset + len - 1) / FS_BLOCK_SIZE;
	if (last_logical_block_num > N_DIRECT + PTRS_PER_BLK + PTRS_PER_BLK * PTRS_PER_BLK){
		last_logical_block_num = N_DIRECT + PTRS_PER_BLK + PTRS_PER_BLK * PTRS_PER_BLK;
		len = MAX_FILE_SIZE - offset;
	}
	char first_block[FS_BLOCK_SIZE];
	switch(read_block_of_file(first_logical_block_num, &inode, first_block)){
	case -EIO:
		return -EIO;
	case -1:
		;
		char empty_block[FS_BLOCK_SIZE];
		memset(empty_block, 0, FS_BLOCK_SIZE);
		int temp = put_block_in_file(&inode, first_logical_block_num, empty_block);
		if (temp < 0){
			return temp;
		}
		memset(first_block, 0, FS_BLOCK_SIZE);
		break;
	case 0:
		;
	}
	memcpy(first_block + offset % FS_BLOCK_SIZE, buf, (len <= FS_BLOCK_SIZE - offset % FS_BLOCK_SIZE) ? len : FS_BLOCK_SIZE - offset % FS_BLOCK_SIZE);
	switch(put_block_in_file(&inode, first_logical_block_num, first_block)){
	case -EIO:
		return -EIO;
	case -ENOSPC:
		return -ENOSPC;
	}
	if (first_logical_block_num != last_logical_block_num){
		size_t offset_in_buf = FS_BLOCK_SIZE - offset % FS_BLOCK_SIZE; //amount written to the first block
		for (int log_block = first_logical_block_num + 1; log_block <= last_logical_block_num - 1; log_block++){
			int temp = put_block_in_file(&inode, log_block, (void*)(buf + offset_in_buf));
			if (temp < 0){
				return temp;
			}
			offset_in_buf += FS_BLOCK_SIZE;
		}
		char last_block[FS_BLOCK_SIZE];
		switch(read_block_of_file(last_logical_block_num, &inode, last_block)){
		case -EIO:
			return -EIO;
		case -1:
			;
			char empty_block[FS_BLOCK_SIZE];
			memset(empty_block, 0, FS_BLOCK_SIZE);
			int temp = put_block_in_file(&inode, last_logical_block_num, empty_block);
			if (temp < 0){
				return temp;
			}
			memset(last_block, 0, FS_BLOCK_SIZE);
			break;
		}
		memcpy(last_block, buf + offset_in_buf, len - offset_in_buf);
		switch(put_block_in_file(&inode, last_logical_block_num, last_block)){
		case -EIO:
			return -EIO;
		case -ENOSPC:
			return -ENOSPC;
		}
	}
	uint32_t temp = offset + len;
	if (temp > inode.size){
		inode.size = temp;
	}
	int block_number_that_contains_inode = 1 + superblock.inode_map_sz + superblock.block_map_sz + (inode_num / INODES_PER_BLK);
	struct fs_inode block_containing_inode[INODES_PER_BLK];
	if (disk->ops->read(disk, block_number_that_contains_inode, 1, block_containing_inode) != SUCCESS){
		return -EIO;
	}
	memcpy(&block_containing_inode[inode_num % INODES_PER_BLK],
			&inode, sizeof(struct fs_inode));
	if (disk->ops->write(disk, block_number_that_contains_inode, 1, block_containing_inode) != SUCCESS){
		return -EIO;
	}
	return len;
}


/* 
 * Release resources created by pending open call.
 *
 * @param path: path to the file
 * @param fi: the fuse file info
 *
 * @return: 0 if successful, or -error number
 *	-ENOENT   - file does not exist
 *	-ENOTDIR  - component of path not a directory
*/
static int fs_release(const char *path, struct fuse_file_info *fi)
{	
	return fs_open(path, fi);
}


/*
 * statfs - get file system statistics. See 'man 2 statfs' for 
 * description of 'struct statvfs'.
 *
 * @param path: the path to the file
 * @param st: pointer to the destination statvfs struct
 *
 * @return: 0 if successful, or -error number
 * 	-ENOENT  - a component of the path is not present
 *	-ENOTDIR - an intermediate component of path not a directory
*/
static int fs_statfs(const char *path, struct statvfs *st)
{
	char *block_bitmap = malloc(FS_BLOCK_SIZE * superblock.block_map_sz);
	if (block_bitmap == NULL){
		return -EIO;
	}
	if (disk->ops->read(disk, 1 + superblock.inode_map_sz, superblock.block_map_sz, block_bitmap) != SUCCESS){
		free(block_bitmap);
		return -EIO;
	}
	long available_blocks = 0;
	for (long i = 1 + superblock.inode_map_sz + superblock.block_map_sz + superblock.inode_region_sz; i < superblock.num_blocks; i++){
		if ( !(block_bitmap[i / 8] & (1 << (i % 8))) ){
			available_blocks += 1;
		}
	}
	free(block_bitmap);

	char *inode_bitmap = malloc(FS_BLOCK_SIZE * superblock.inode_map_sz);
	if (inode_bitmap == NULL){
		return -EIO;
	}
	if (disk->ops->read(disk, 1, superblock.inode_map_sz, inode_bitmap) != SUCCESS){
		free(inode_bitmap);
		return -EIO;
	}
	long available_inodes = 0;
	for (long i = 0; i < superblock.inode_region_sz * INODES_PER_BLK; i++){
		if ( !(inode_bitmap[i / 8] & (1 << (i % 8)))){
			available_inodes += 1;
		}
	}
	free(inode_bitmap);

	st->f_bsize = FS_BLOCK_SIZE;
	st->f_blocks = superblock.num_blocks - 1 - superblock.inode_map_sz - superblock.block_map_sz - superblock.inode_region_sz;
	st->f_bfree = available_blocks;
	st->f_bavail = available_blocks;
	st->f_files = superblock.inode_region_sz * INODES_PER_BLK;
	st->f_ffree = available_inodes;
	st->f_namemax = FS_FILENAME_SIZE;
	st->f_fsid = 0;
	st->f_frsize = 0;
	st->f_flag = 0;
	return 0;
}

static int fs_utime(const char *path, struct utimbuf *timebuf){
	struct file *f;
	f = find_file(path);
	if (!f){
		return -ENOENT;
	}
	return 0;
}

static int fs_truncate(const char *path, off_t offset){
	return -ENOSYS;
}

/**
 * Operations vector. Please don't rename it, as the
 * skeleton code in main.c assumes it is named 'fs_ops'.
 */
struct fuse_operations fs_ops = {
    .init = fs_init,
    .getattr = fs_getattr,
    .opendir = fs_opendir,
    .readdir = fs_readdir,
    .releasedir = fs_releasedir,
    .mknod = fs_mknod,
    .mkdir = fs_mkdir,
    .unlink = fs_unlink,
    .rmdir = fs_rmdir,
    .rename = fs_rename,
    .chmod = fs_chmod,
    .open = fs_open,
    .read = fs_read,
    .write = fs_write,
    .release = fs_release,
    .statfs = fs_statfs,
	.utime = fs_utime, 
	.truncate = fs_truncate,
};
