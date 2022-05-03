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

#include "fsx492.h"
#include "blkdev.h"

/*
 * disk access - the global variable 'disk' points to a blkdev
 * structure which has been initialized to access the image file.
 *
 * NOTE - blkdev access is in terms of 1024-byte blocks
 */
extern struct blkdev *disk; //see main.c

/* by defining bitmaps as 'fd_set' pointers, you can use existing
 * macros to handle them.
 *   FD_ISSET(##, inode_map);
 *   FD_CLR(##, block_map);
 *   FD_SET(##, block_map);
 */

/** pointer to inode bitmap to determine free inodes */
static fd_set *inode_map;
static int     inode_map_base;

/** pointer to inode blocks */
static struct fs_inode *inodes;
/** number of inodes from superblock */
static int   n_inodes;
/** number of first inode block */
static int   inode_base;

/** pointer to block bitmap to determine free blocks */
fd_set *block_map;
/** number of first data block */
static int     block_map_base;

/** number of available blocks from superblock */
static int   n_blocks;

/** number of root inode from superblock */
static int   root_inode;

/** array of dirty metadata blocks to write  -- optional */
static void **dirty;

/** length of dirty array -- optional */
static int    dirty_len;

/** total size of direct blocks */
static int DIR_SIZE = BLOCK_SIZE * N_DIRECT;
static int INDIR1_SIZE = (BLOCK_SIZE / sizeof(uint32_t)) * BLOCK_SIZE;
static int INDIR2_SIZE = (BLOCK_SIZE / sizeof(uint32_t)) * (BLOCK_SIZE / sizeof(uint32_t)) * BLOCK_SIZE;

/* Suggested functions to implement -- you are free to ignore these
 * and implement your own instead
 */

/**
 * Find inode for existing directory entry.
 *
 * @param fs_dirent: pointer to first dirent in directory
 * @param name: the name of the directory entry
 * @return the entry inode, or 0 if not found.
 */
static int find_in_dir(struct fs_dirent *de, char *name)
{
	for (int i = 0; i < DIRENTS_PER_BLK; i++) {
		//found, return its inode
		if (de[i].valid && strcmp(de[i].name, name) == 0) {
			return de[i].inode;
		}
	}
	return 0;
}

/**
 * Look up a single directory entry in a directory.
 *
 * Errors
 *   -EIO     - error reading block
 *   -ENOENT  - a component of the path is not present.
 *   -ENOTDIR - intermediate component of path not a directory
 *
 */
static int lookup(int inum, char *name)
{
	//get corresponding directory
	struct fs_inode cur_dir = inodes[inum];
	//init buff entries
	struct fs_dirent entries[DIRENTS_PER_BLK];
	memset(entries, 0, DIRENTS_PER_BLK * sizeof(struct fs_dirent));
	if (disk->ops->read(disk, cur_dir.direct[0], 1, &entries) < 0) exit(1);
	int inode = find_in_dir(entries, name);
	return inode == 0 ? -ENOENT : inode;
}

/**
 * Parse path name into tokens at most nnames tokens after
 * normalizing paths by removing '.' and '..' elements.
 *
 * If names is NULL, path is not altered and function  returns
 * the path count. Otherwise, path is altered by strtok() and
 * function returns names in the names array, which point to
 * elements of path string.
 *
 * @param path: the directory path
 * @param names: the argument token array or NULL
 * @param nnames: the maximum number of names, 0 = unlimited
 * @return the number of path name tokens
 */
static int parse(char *path, char *names[], int nnames)
{
	char *_path = strdup(path);
	int count = 0;
	char *token = strtok(_path, "/");
	while (token != NULL) {
		int len = strlen(token);
		if (len > FS_FILENAME_SIZE - 1) return -EINVAL;
		if (strcmp(token, "..") == 0 && count > 0) count--;
		else if (strcmp(token, ".") != 0) {
			if (names != NULL && count < nnames) {
				names[count] = (char*)malloc(len+1);
				memset(names[count], 0, len+1);
				strcpy(names[count], token);
			}
			count++;
		}
		token = strtok(NULL, "/");
	}
	//if the number of names in the path exceed the maximum
	if (nnames != 0 && count > nnames) return -1;
	return count;
}

/**
 * free allocated char pointer array
 * @param arr: array to be freed
 */
static void free_char_ptr_array(char *arr[], int len) {
	for (int i = 0; i < len; i++) {
		free(arr[i]);
	}
}

/**
 * Return inode number for specified file or
 * directory.
 *
 * Errors
 *   -ENOENT  - a component of the path is not present.
 *   -ENOTDIR - an intermediate component of path not a directory
 *
 * @param path: the file path
 * @return inode of path node or error
 */
static int translate(char *path)
{
	if (strcmp(path, "/") == 0 || strlen(path) == 0) {
		return root_inode;
	}
	int inode_idx = root_inode;
	//get number of names
	int num_names = parse(path, NULL, 0);
	//if the number of names in the path exceed the maximum, return an error, error type to be fixed if necessary
	if (num_names < 0) {
		return -ENOTDIR;
	}
	if (num_names == 0) return root_inode;
	//copy all the names
	char *names[num_names];
	parse(path, names, num_names);
	//lookup inode

	for (int i = 0; i < num_names; i++) {
		//if token is not a directory return error
		if (!S_ISDIR(inodes[inode_idx].mode)) {
			free_char_ptr_array(names, num_names);
			return -ENOTDIR;
		}
		//lookup and record inode
		inode_idx = lookup(inode_idx, names[i]);
		if (inode_idx < 0) {
			free_char_ptr_array(names, num_names);
			return -ENOENT;
		}
	}
	free_char_ptr_array(names, num_names);
	return inode_idx;
}

/**
 *  Return inode number for path to specified file
 *  or directory, and a leaf name that may not yet
 *  exist.
 *
 * Errors
 *   -ENOENT  - a component of the path is not present.
 *   -ENOTDIR - an intermediate component of path not a directory
 *
 * @param path: the file path
 * @param leaf: pointer to space for FS_FILENAME_SIZE leaf name
 * @return inode of path node or error
 */
static int translate_1(char *path, char *leaf)
{
	if (strcmp(path, "/") == 0 || strlen(path) == 0) return root_inode;
	int inode_idx = root_inode;
	//get number of names
	int num_names = parse(path, NULL, 0);
	//if the number of names in the path exceed the maximum, return an error, error type to be fixed if necessary
	if (num_names < 0) {
		return -ENOTDIR;
	}
	if (num_names == 0) return root_inode;
	//copy all the names
	char *names[num_names];
	parse(path, names, num_names);
	//lookup inode

	for (int i = 0; i < num_names - 1; i++) {
		//if token is not a directory return error
		if (!S_ISDIR(inodes[inode_idx].mode)) {
			free_char_ptr_array(names, num_names);
			return -ENOTDIR;
		}
		//lookup and record inode
		inode_idx = lookup(inode_idx, names[i]);
		if (inode_idx < 0) {
			free_char_ptr_array(names, num_names);
			return -ENOENT;
		}
	}
	strcpy(leaf, names[num_names - 1]);
	free_char_ptr_array(names, num_names);
	return inode_idx;
}

/**
 * Flush dirty metadata blocks to disk.
 */
void flush_metadata(void)
{
	int i;
	for (i = 0; i < dirty_len; i++) {
		if (dirty[i]) {
			disk->ops->write(disk, i, 1, dirty[i]);
			dirty[i] = NULL;
		}
	}
}

/**
 * Count number of free blocks
 * @return number of free blocks
 */
int num_free_blk() {
	int count = 0;
	for (int i = 0; i < n_blocks; i++) {
		if (!FD_ISSET(i, block_map)) {
			count++;
		}
	}
	return count;
}

/**
 * Returns a free block number or -ENOSPC if none available.
 *
 * @return free block number or -ENOSPC if none available
 */
static int get_free_blk(void)
{
	for (int i = 0; i < n_blocks; i++) {
		if (!FD_ISSET(i, block_map)) {
			char buff[BLOCK_SIZE];
			memset(buff, 0, BLOCK_SIZE);
			if (disk->ops->write(disk, i, 1, buff) < 0) exit(1);
			FD_SET(i, block_map);
			return i;
		}
	}
	return -ENOSPC;
}

/**
 * Return a block to the free list
 *
 * @param  blkno the block number
 */
static void return_blk(int blkno)
{
	FD_CLR(blkno, block_map);
}

static void update_blk(void)
{
	if (disk->ops->write(disk, block_map_base, inode_base - block_map_base, block_map) < 0)
		exit(1);
}

/**
 * Returns a free inode number
 *
 * @return a free inode number or -ENOSPC if none available
 */
static int get_free_inode(void)
{
	for (int i = 2; i < n_inodes; i++) {
		if (!FD_ISSET(i, inode_map)) {
			FD_SET(i, inode_map);
			return i;
		}
	}
	return -ENOSPC;
}

/**
 * Return an inode to the free list.
 *
 * @param  inum the inode number
 */
static void return_inode(int inum)
{
	FD_CLR(inum, inode_map);
}

static void update_inode(int inum)
{
	if (disk->ops->write(disk, inode_base + inum / INODES_PER_BLK, 1, &inodes[inum - (inum % INODES_PER_BLK)]) < 0)
		exit(1);
	if (disk->ops->write(disk, inode_map_base, block_map_base - inode_map_base, inode_map) < 0)
		exit(1);
}

/**
 * Find free directory entry.
 *
 * @return index of directory free entry or -ENOSPC
 *   if no space for new entry in directory
 */
static int find_free_dir(struct fs_dirent *de)
{
	for (int i = 0; i < DIRENTS_PER_BLK; i++) {
		if (!de[i].valid) {
			return i;
		}
	}
	return -ENOSPC;
}

/**
 * Determines whether directory is empty.
 *
 * @param de ptr to first entry in directory
 * @return 1 if empty 0 if has entries
 */
static int is_empty_dir(struct fs_dirent *de)
{
	for (int i = 0; i < DIRENTS_PER_BLK; i++) {
		if (de[i].valid) {
			return 0;
		}
	}
	return 1;
}

/**
 * Copy stat from inode to sb
 * @param inode inode to be copied from
 * @param sb holder to hold copied stat
 * @param inode_idx inode_idx
 */
static void cpy_stat(struct fs_inode *inode, struct stat *sb) {
	memset(sb, 0, sizeof(*sb));
	sb->st_uid = inode->uid;
	sb->st_gid = inode->gid;
	sb->st_mode = (mode_t) inode->mode;
	sb->st_atime = inode->mtime;
	sb->st_ctime = inode->ctime;
	sb->st_mtime = inode->mtime;
	sb->st_size = inode->size;
	sb->st_blksize = FS_BLOCK_SIZE;
	sb->st_nlink = 1;
	sb->st_blocks = (inode->size + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
}

/* Extra Global Variables */
int inode_map_sz;		// inode map size
int inode_region_sz;	// inode region size 
int block_map_sz;		// block map size 
static struct fs_super sb;

/* Helper Functions */

/** COPIED FROM main.c
 * Split string into array of at most n tokens.
 *
 * If toks is NULL, p is not altered and function returns
 * the token count. Otherwise, p is altered by strtok()
 * and function returns tokens in the toks array, which
 * point to elements of p string.
 *
 * @param p the character string
 * @param toks token array
 * @param n max number of tokens to retrieve, 0 = unlimited
 * @param delim the delimiter string between tokens
 */
static int split(char *p, char *toks[], int n, char *delim)
{
	if (n == 0) {
		n = INT_MAX;
	}
	if (toks == NULL) {
		// do not alter p if not returning names
		p = strdup(p);
	}
	char *str;
	char *lasts = NULL;
	int i;
	for (i = 0; i < n && (str = strtok_r(p, delim, &lasts)) != NULL; i++) {
		p = NULL;
		if (toks != NULL) {
			toks[i] = str;
		}
	}
	if (toks == NULL) {
		free(p);
	}
	return i;
}

static void unset_block_bit(int block_num, char *block_bitmap){
	if (block_num != 0){
		block_bitmap[(block_num) / 8] &= ~(1 << (block_num % 8));
	}
}

//checks the bitmap to see if a given inode number is valid
//could return -1 if reading from the disk fails
static int inode_is_used(int inode_num){
	char *inode_bitmap = malloc(FS_BLOCK_SIZE * sb.inode_map_sz);
	if (inode_bitmap == NULL){
		return -1;
	}
	if (disk->ops->read(disk, 1, sb.inode_map_sz, inode_bitmap) != SUCCESS){
		free(inode_bitmap);
		return -1;
	}
	int result = inode_bitmap[inode_num / 8] & (1 << (inode_num % 8));
	free(inode_bitmap);
	return result;
}

//before calling this function, make sure that inode_num is used by calling inode_is_used
static int read_inode(int inode_num, struct fs_inode* buf){
	char temp_block[FS_BLOCK_SIZE];
	int block_number = 1 + sb.inode_map_sz + sb.block_map_sz + inode_num / INODES_PER_BLK;
	if(disk->ops->read(disk, block_number, 1, temp_block) != SUCCESS){
		return -1;
	}
	int offset_in_block = inode_num % INODES_PER_BLK;
	memcpy(buf, temp_block + offset_in_block * sizeof(struct fs_inode), sizeof(struct fs_inode));
	return 0;
}

enum {MAX_PATH = 4096 }; //Copied from main.c

//reads a block (which is assumed to be a directory data block), and looks for an
//entry that contains filename
//returns 0 if not found, returns -1 on error
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
			//file found
			return entries[i].inode;
		}
	}
	//scanned entire block's worth of entries, not found
	return 0;
}

//Given a path, returns the inode number of that file
//Might return -ENOENT or -ENOTDIR, or -1 if reading from the disk failed
static int inode_from_full_path(const char *path){
	//fprintf(stderr, "now getting inode of '%s'\n", path);
	if(path[0] != '/'){
		fprintf(stderr, "cannot get inode from relative path\n");
		return -ENOENT;
	}
	char temp_path[MAX_PATH];
	strcpy(temp_path, path);
	int number_of_path_components = split(temp_path, NULL, 0, "/");
	if (number_of_path_components == 0){
		//path is just "/", so return inode of root directory
		return sb.root_inode;
	}
	char** path_components = malloc(number_of_path_components * sizeof(char*));
	split(temp_path, path_components, number_of_path_components, "/");
	//path_components is now an array of strings
	for (int i = 0; i < number_of_path_components; i++){
		//fprintf(stderr, "component %d = '%s'\n", i, path_components[i]);
	}

	//directories only ever use one block, which simplifies this a lot
	int inode = sb.root_inode; //start at the root directory, should be inode 1
	struct fs_inode current_inode;
	for (int i = 0; i < number_of_path_components; i++){
		int inode_is_used_result = inode_is_used(inode);
		if (inode_is_used_result == -1){
			fprintf(stderr, "error reading from disk on line %d\n", __LINE__);
			free(path_components);
			return -1;
		}
		if (inode_is_used_result == 0 || inode == 0){
			fprintf(stderr,
				"could not get inode from full path: inode %u not used (or 0)\n"
				"when trying to find the inode of file '%s'\n"
				, inode, path);
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
		//only need to scan the first block
		int scan_result = scan_dir_block(current_inode.direct[0], path_components[i]);
		if (scan_result == -1){
			free(path_components);
			return -1;
		}
		if (scan_result > 0){
			inode = scan_result;
		} else {
			//this component could not be found
			free(path_components);
			return -ENOENT;
		}

	}
	free(path_components);
	return inode;
}

//either returns 0 or -ENAMETOOLONG
int split_path(const char* path, char *temp_path, char *new_file_name){
	strcpy(temp_path, path);
	//remove the trailing / in temp_path, if it has one
	int len = strlen(temp_path);
	if (temp_path[len-1] == '/'){
		temp_path[len-1] = '\0';
		len--;
	}
	//separate temp_path into 2 parts: before and after the last /
	//the / character will be kept with the 'before' part, in temp_path
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

//returns either a physical block number (> 0), -EIO on error, or 0 if the file is not long enough
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

//returns 0 on success, -1 if file does not have enough blocks, and -EIO on error
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

//returns 0 on success, -1 if file does not have enough blocks, and -EIO on error
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

//returns a block number on success, or -ENOSPC or -EIO on failure
static int allocate_zeroed_block(){
	char *block_bitmap = malloc(FS_BLOCK_SIZE * sb.block_map_sz);
	if (block_bitmap == NULL){
		return -EIO;
	}
	if (disk->ops->read(disk, 1 + sb.inode_map_sz, sb.block_map_sz, block_bitmap) != SUCCESS){
		free(block_bitmap);
		return -EIO;
	}
	int new_block_num = -1;
	for (int i = 0; i < sb.num_blocks; i++){
		if (!(block_bitmap[i/8] & (1 << (i % 8)))){
			new_block_num = i;
			break;
		}
	}
	if (new_block_num == -1){
		free(block_bitmap);
		return -ENOSPC;
	}
	//zero out the new block
	char zeros[FS_BLOCK_SIZE];
	memset(zeros, 0, FS_BLOCK_SIZE);
	if (disk->ops->write(disk, new_block_num, 1, zeros) != SUCCESS){
		free(block_bitmap);
		return -EIO;
	}
	block_bitmap[new_block_num / 8] |= 1 << (new_block_num % 8);
	if (disk->ops->write(disk, 1 + sb.inode_map_sz, sb.block_map_sz, block_bitmap) != SUCCESS){
		free(block_bitmap);
		return -EIO;
	}
	//if the rest of the write fails after this, then an inode is permanently unusable. Too bad.
	free(block_bitmap);
	return new_block_num;
}

//returns 0 on success, or -EIO or -ENOSPC on error
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
				/* 
				if a block is allocated, then allocating a new indir_1 block fails,
				the first block will be marked as used even though it isn't
				*/
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
		//double indirect block
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
 * CS492: FUSE functions to implement are below.
*/

/**
 * init - this is called once by the FUSE framework at startup.
 *
 * This is a good place to read in the super-block and set up any
 * global variables you need. You don't need to worry about the
 * argument or the return value.
 *
 * @param conn: fuse connection information - unused
 * @return: unused - returns NULL
 *
 * Note: if any block read operation fails, just exit(1) immediately.
*/
void* fs_init(struct fuse_conn_info *conn)
{
	// read the superblock
	//CS492: your code below

	// Read superblock from disk and store in global variable
	int retval = disk->ops->read(disk, 0, 1, &sb);
	if(retval != SUCCESS){
		fprintf(stderr, "Error: Return value of %d when reading the superblock\n", retval);
		abort();
	}

	// Check that the magic number is correct
	if (sb.magic != FS_MAGIC){
		fprintf(stderr, "Error: Superblocks contains wrong magic number, possibly corrupt\n");
	}

	// Check that the number of blocks matches what is stored in disk
	if (disk->ops->num_blocks(disk) != sb.num_blocks){
		fprintf(stderr, "Error: Superblock contains wrong number of blocks, possibly corrupt\n");
	}

	// number of blocks on device
	n_blocks = sb.num_blocks;

	// dirty metadata blocks
	dirty_len = inode_base + sb.inode_region_sz;
	dirty = calloc(dirty_len*sizeof(void*), 1);

	return NULL;
}

/* Note on path translation errors:
 * In addition to the method-specific errors listed below, almost
 * every method can return one of the following errors if it fails to
 * locate a file or directory corresponding to a specified path.
 *
 * ENOENT - a component of the path is not present.
 * ENOTDIR - an intermediate component of the path (e.g. 'b' in
 *           /a/b/c) is not a directory
 */

/* note on splitting the 'path' variable:
 * the value passed in by the FUSE framework is declared as 'const',
 * which means you can't modify it. The standard mechanisms for
 * splitting strings in C (strtok, strsep) modify the string in place,
 * so you have to copy the string and then free the copy when you're
 * done. One way of doing this:
 *
 *    char *_path = strdup(path);
 *    int inum = translate(_path);
 *    free(_path);
 */

/**
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
	fs_init(NULL);

	char *_path = strdup(path);
	int inode_idx = translate(_path);
	if (inode_idx < 0) return inode_idx;
	struct fs_inode* inode = &inodes[inode_idx];
	cpy_stat(inode, sb);
	return SUCCESS;
}

/**
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
	char *_path = strdup(path);
	int inode_idx = translate(_path);
	if (inode_idx < 0) return inode_idx;
	if (!S_ISDIR(inodes[inode_idx].mode)) return -ENOTDIR;
	fi->fh = (uint64_t) inode_idx;
	return SUCCESS;
}

/**
 * readdir - get directory contents
 *
 * For each entry in the directory, invoke the 'filler' function,
 * which is passed as a function pointer, as follows:
 * filler(buf, <name>, <statbuf>, 0)
 * where <statbuf> is a struct stat, just like in getattr.
 *
 * @param path: the directory path
 * @param ptr: filler buf pointer
 * @param filler filler function to call for each entry
 * @param offset: the file offset -- unused
 * @param fi: the fuse file information -- you do not have to use it
 *
 * @return: 0 if successful, or -error number
 * 	-ENOENT  - a component of the path is not present
 * 	-ENOTDIR - an intermediate component of path not a directory
*/
static int fs_readdir(const char *path, void *ptr, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi)
{
	char *_path = strdup(path);
	int inode_idx = translate(_path);
	if (inode_idx < 0) return inode_idx;
	struct fs_inode *inode = &inodes[inode_idx];
	if (!S_ISDIR(inode->mode)) {
		return -ENOTDIR;
	}
	struct fs_dirent entries[DIRENTS_PER_BLK];
	memset(entries, 0, DIRENTS_PER_BLK * sizeof(struct fs_dirent));
	struct stat sb;
	if (disk->ops->read(disk, inode->direct[0], 1, entries) < 0) exit(1);
	for (int i = 0; i < DIRENTS_PER_BLK; i++) {
		if (entries[i].valid) {
			cpy_stat(&inodes[entries[i].inode], &sb);
			filler(ptr, entries[i].name, &sb, 0);
		}
	}
	return SUCCESS;
}

/**
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
	char *_path = strdup(path);
	int inode_idx = translate(_path);
	if (inode_idx < 0) return inode_idx;
	if (!S_ISDIR(inodes[inode_idx].mode)) {
		return -ENOTDIR;
	}
	fi->fh = (uint64_t) -1;
	return SUCCESS;
}

static int set_attributes_and_update(struct fs_dirent *de, char *name, mode_t mode, bool isDir)
{
	//get free directory and inode
	int freed = find_free_dir(de);
	int freei = get_free_inode();
	int freeb = isDir ? get_free_blk() : 0;
	if (freed < 0 || freei < 0 || freeb < 0) return -ENOSPC;
	struct fs_dirent *dir = &de[freed];
	struct fs_inode *inode = &inodes[freei];
	strcpy(dir->name, name);
	dir->inode = freei;
	dir->valid = true;
	inode->uid = getuid();
	inode->gid = getgid();
	inode->mode = mode;
	inode->ctime = inode->mtime = time(NULL);
	inode->size = 0;
	inode->direct[0] = freeb;
	//update map and inode
	update_inode(freei);
	update_blk();
	return SUCCESS;
}

/**
 * mknod - create a new regular file with permissions (mode & 01777).
 * Behavior undefined when mode bits other than the low 9 bits are used.
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
	//get current and parent inodes
	mode |= S_IFREG;
	if (!S_ISREG(mode) || strcmp(path, "/") == 0) return -EINVAL;
	char *_path = strdup(path);
	char name[FS_FILENAME_SIZE];
	int inode_idx = translate(_path);
	int parent_inode_idx = translate_1(_path, name);
	if (inode_idx >= 0) return -EEXIST;
	if (parent_inode_idx < 0) return parent_inode_idx;
	//read parent info
	struct fs_inode *parent_inode = &inodes[parent_inode_idx];
	if (!S_ISDIR(parent_inode->mode)) {
		return -ENOTDIR;
	}

	struct fs_dirent entries[DIRENTS_PER_BLK];
	memset(entries, 0, DIRENTS_PER_BLK * sizeof(struct fs_dirent));
	if (disk->ops->read(disk, parent_inode->direct[0], 1, entries) < 0)
		exit(1);
	//assign inode and directory and update
	int res = set_attributes_and_update(entries, name, mode, false);
	if (res < 0) return res;

	//write entries buffer into disk
	if (disk->ops->write(disk, parent_inode->direct[0], 1, entries) < 0)
		exit(1);
	return SUCCESS;
}

/**
 * mkdir - create a directory with the given mode. Behavior undefined
 * when mode bits other than the low 9 bits are used.
 *
 * @param path: path to directory
 * @param mode: the mode for the new directory
 *
 * @return: 0 if successful, or -error number
 * 	-ENOTDIR  - component of path not a directory
 * 	-EEXIST   - file already exists
 * 	-ENOSPC   - free inode not available
 * 	-ENOSPC   - results in >32 entries in directory
 *
 * Note: fs_mkdir is the same as fs_mknod except that fs_mknod creates
 * a regular file while fs_mkdir creates a directory.  See also the
 * last argument of set_attributes_and_update.
*/
static int fs_mkdir(const char *path, mode_t mode)
{
	//CS492: your code here
	if (path[0] == '\0') {
		//if the path is "" somehow, then just return an error
		return -EINVAL;
	}
	if (!strcmp(path, "/")) {
		return -EEXIST;
	}
	char temp_path[MAX_PATH];
	char new_dir_name[FS_FILENAME_SIZE];
	if (split_path(path, temp_path, new_dir_name) == -ENAMETOOLONG){
		return -ENAMETOOLONG;
	}
	//fprintf(stderr, "temp_path = '%s'\nnew_dir_name = '%s'\n", temp_path, new_dir_name);

	//find inode of directory that should contain the new dir
	int inode_num_of_containing_dir = inode_from_full_path(temp_path);
	if (inode_num_of_containing_dir == -1) {
		return -EIO;
	}
	if (inode_num_of_containing_dir < 0) {
		return inode_num_of_containing_dir; //either -ENOENT or -ENOTDIR
	}
	struct fs_inode containing_dir_inode;
	if (read_inode(inode_num_of_containing_dir, &containing_dir_inode) != 0){
		return -EIO;
	}
	//make sure temp_path is a directory
	if (!S_ISDIR(containing_dir_inode.mode)){
		return -ENOTDIR;
	}
	struct fs_dirent entries[DIRENTS_PER_BLK];
	if (disk->ops->read(disk, containing_dir_inode.direct[0], 1, entries) != SUCCESS){
		return -EIO;
	}
	//make sure no file with this name already exists in the containing directory
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
	char *inode_bitmap = malloc(FS_BLOCK_SIZE * sb.inode_map_sz);
	if (inode_bitmap == NULL){
		return -EIO;
	}
	if (disk->ops->read(disk, 1, sb.inode_map_sz, inode_bitmap) != SUCCESS){
		free(inode_bitmap);
		return -EIO;
	}
	for (int i = 0; i < INODES_PER_BLK * sb.inode_region_sz; i++){
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
		//main.c calls this function with a mode that does not have the dir bit set, so I'm setting
		//it manually here.
		.mode = (mode & 01777 & ~(fuse_get_context()->umask)) | S_IFDIR,
		.ctime = time(NULL),
		.mtime = time(NULL),
		.size = 0, // /dir1, /dir2, and /dir3 all have size 0, so I'm assuming the size of a directory is always 0
		.indir_1 = 0,
		.indir_2 = 0,
	};
	//set all direct blocks other than the first to 0
	for (int i = 1; i < N_DIRECT; i++){
		new_inode.direct[i] = 0;
	}
	//find a new block for this dir's entries
	int new_block_num = -1;
	char *block_bitmap = malloc(FS_BLOCK_SIZE * sb.block_map_sz);
	if (block_bitmap == NULL){
		free(inode_bitmap);
		return -EIO;
	}
	if (disk->ops->read(disk, 1 + sb.inode_map_sz, sb.block_map_sz, block_bitmap) != SUCCESS){
		free(block_bitmap);
		free(inode_bitmap);
		return -EIO;
	}
	for (int i = 0; i < sb.num_blocks; i++){
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
	//now mark that block as used in the bitmap
	block_bitmap[new_block_num / 8] |= 1 << (new_block_num % 8);
	if (disk->ops->write(disk, 1 + sb.inode_map_sz, sb.block_map_sz, block_bitmap) != SUCCESS){
		free(block_bitmap);
		free(inode_bitmap);
		return -EIO;
	}
	//If this function returns an error after this point, the block has been marked as used in
	//the bitmap, but the dir was not created successfully. This means the block will
	//never be usable again
	free(block_bitmap);

	//now mark new_inode_num as used in the bitmap
	inode_bitmap[new_inode_num / 8] |= 1 << (new_inode_num % 8);
	if (disk->ops->write(disk, 1, sb.inode_map_sz, inode_bitmap) != SUCCESS){
		free(inode_bitmap);
		return -EIO;
	}
	//If this function returns an error after this point, the inode has been marked as used in
	//the bitmap, but the dir was not created successfully. This means the inode will
	//never be usable again
	free(inode_bitmap);
	
	//zero out the new data block
	struct fs_dirent zeros[FS_BLOCK_SIZE];
	memset(zeros, 0, FS_BLOCK_SIZE);
	if (disk->ops->write(disk, new_block_num, 1, zeros) != SUCCESS){
		return -EIO;
	}
	new_inode.direct[0] = new_block_num;

	//read the block that contains the new inode slot, modify the part of it that corresponds to
	//this new inode, then write it back to the disk
	int block_number_that_contains_new_inode =
		1 + sb.inode_map_sz + sb.block_map_sz
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
	//now, update the directory containing this file to have an entry for the new file
	entries[entry_index].valid = 1;
	entries[entry_index].inode = new_inode_num;
	strcpy(entries[entry_index].name, new_dir_name);
	if (disk->ops->write(disk, containing_dir_inode.direct[0], 1, entries) != SUCCESS){
		fprintf(stderr, "Error updating directory %s to contain new directory %s, after creating the inode for it. Disk is probably corrupt.\n", temp_path, new_dir_name);
		return -EIO;
	}
	return 0;
}

static void fs_truncate_dir(uint32_t *de) {
	for (int i = 0; i < N_DIRECT; i++) {
		if (de[i]) return_blk(de[i]);
		de[i] = 0;
	}
}

static void fs_truncate_indir1(int blk_num) {
	uint32_t entries[PTRS_PER_BLK];
	memset(entries, 0, PTRS_PER_BLK * sizeof(uint32_t));
	if (disk->ops->read(disk, blk_num, 1, entries) < 0)
		exit(1);
	//clear each blk and wipe from blk_map
	for (int i = 0; i < PTRS_PER_BLK; i++) {
		if (entries[i]) return_blk(entries[i]);
		entries[i] = 0;
	}
}

static void fs_truncate_indir2(int blk_num) {
	uint32_t entries[PTRS_PER_BLK];
	memset(entries, 0, PTRS_PER_BLK * sizeof(uint32_t));
	if (disk->ops->read(disk, blk_num, 1, entries) < 0)
		exit(1);
	//clear each double link
	for (int i = 0; i < PTRS_PER_BLK; i++) {
		if (entries[i]) fs_truncate_indir1(entries[i]);
		entries[i] = 0;
	}
}

/**
 * truncate - truncate file to exactly 'len' bytes.
 *
 * Errors:
 *   ENOENT  - file does not exist
 *   ENOTDIR - component of path not a directory
 *   EINVAL  - length invalid (only supports 0)
 *   EISDIR	 - path is a directory (only files)
 *
 * @param path the file path
 * @param len the length
 * @return 0 if successful, or error value
 */
static int fs_truncate(const char *path, off_t len)
{
	if (len != 0) return -EINVAL; /* invalid argument */

	//get inode
	char *_path = strdup(path);
	int inode_idx = translate(_path);
	if (inode_idx < 0) return inode_idx;
	struct fs_inode *inode = &inodes[inode_idx];
	if (S_ISDIR(inode->mode)) return -EISDIR;

	//clear direct
	fs_truncate_dir(inode->direct);

	//clear indirect1
	if (inode->indir_1) {
		fs_truncate_indir1(inode->indir_1);
		return_blk(inode->indir_1);
	}
	inode->indir_1 = 0;

	//clear indirect2
	if (inode->indir_2) {
		fs_truncate_indir2(inode->indir_2);
		return_blk(inode->indir_2);
	}
	inode->indir_2 = 0;

	inode->size = 0;

	//update at the end for efficiency
	update_inode(inode_idx);
	update_blk();

	return SUCCESS;
}

/**
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
	//truncate first
	int res = fs_truncate(path, 0);
	if (res < 0) return res;

	//get inodes and check
	char *_path = strdup(path);
	char name[FS_FILENAME_SIZE];
	int inode_idx = translate(_path);
	int parent_inode_idx = translate_1(_path, name);
	struct fs_inode *inode = &inodes[inode_idx];
	struct fs_inode *parent_inode = &inodes[parent_inode_idx];
	if (inode_idx < 0 || parent_inode_idx < 0) return -ENOENT;
	if (S_ISDIR(inode->mode)) return -EISDIR;
	if (!S_ISDIR(parent_inode->mode)) {
		return -ENOTDIR;
	}

	//remove entire entry from parent dir
	struct fs_dirent entries[DIRENTS_PER_BLK];
	memset(entries, 0, DIRENTS_PER_BLK * sizeof(struct fs_dirent));
	if (disk->ops->read(disk, parent_inode->direct[0], 1, entries) < 0)
		exit(1);
	for (int i = 0; i < DIRENTS_PER_BLK; i++) {
		if (entries[i].valid && strcmp(entries[i].name, name) == 0) {
			memset(&entries[i], 0, sizeof(struct fs_dirent));
		}
	}
	if (disk->ops->write(disk, parent_inode->direct[0], 1, entries) < 0)
		exit(1);

	//clear inode
	memset(inode, 0, sizeof(struct fs_inode));
	return_inode(inode_idx);

	//update
	update_inode(inode_idx);
	update_blk();

	return SUCCESS;
}

/**
 * rmdir - remove a directory
 *
 * @param path: the path of the directory
 *
 * @return: 0 if successful, or -error number
 * 	-ENOENT   - file does not exist
 *  	-ENOTDIR  - component of path not a directory
 *  	-ENOTDIR  - path not a directory
 *  	-ENOEMPTY - directory not empty
 *
 * Note: this is similar to deleting a file, except that we delete
 * a directory.
*/
static int fs_rmdir(const char *path)
{
	//can not remove root
	if (strcmp(path, "/") == 0) return -EINVAL;

	//get inodes and check
	//CS492: your code below
	if (path[0] == '\0'){
		//if the path is "" somehow, then just return an error
		return -EINVAL;
	}
	if (!strcmp(path, "/")){
		//TODO: what to do when the user tries to remove the root?
		return -ENOTEMPTY;
	}
	char temp_path[MAX_PATH];
	char dir_name[FS_FILENAME_SIZE];
	if (split_path(path, temp_path, dir_name) == -ENAMETOOLONG){
		return -ENOENT; //if the filename is too long, then it can't exist
	}

	int inode_num_of_containing_dir = inode_from_full_path(temp_path);
	if (inode_num_of_containing_dir == -1){
		return -EIO;
	}
	if (inode_num_of_containing_dir < 0){
		return inode_num_of_containing_dir; //either -ENOENT or -ENOTDIR
	}
	struct fs_inode containing_dir_inode;
	if (read_inode(inode_num_of_containing_dir, &containing_dir_inode) != 0){
		return -EIO;
	}
	//make sure temp_path is a directory
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
	//scan the directory to make sure it is empty
	struct fs_dirent entries_of_dir_to_be_removed[DIRENTS_PER_BLK];
	if (disk->ops->read(disk, inode_of_dir_to_be_removed.direct[0], 1, entries_of_dir_to_be_removed) != SUCCESS){
		return -EIO;
	}
	for (int i = 0; i < DIRENTS_PER_BLK; i++){
		if (entries_of_dir_to_be_removed[i].valid){
			return -ENOTEMPTY;
		}
	}
	//dir to remove is empty

	//now mark the data block as unused
	char *block_bitmap = malloc(FS_BLOCK_SIZE * sb.block_map_sz);
	if (block_bitmap == NULL){
		return -EIO;
	}
	if (disk->ops->read(disk, 1 + sb.inode_map_sz, sb.block_map_sz, block_bitmap) != 0){
		free(block_bitmap);
		return -EIO;
	}
	unset_block_bit(inode_of_dir_to_be_removed.direct[0], block_bitmap);
	
	char *inode_bitmap = malloc(FS_BLOCK_SIZE * sb.inode_map_sz);
	if (inode_bitmap == NULL){
		free(block_bitmap);
		return -EIO;
	}
	if (disk->ops->read(disk, 1, sb.inode_map_sz, inode_bitmap) != SUCCESS){
		free(inode_bitmap);
		free(block_bitmap);
		return -EIO;
	}
	inode_bitmap[entries[entry_index].inode / 8] &= ~(1 << (entries[entry_index].inode % 8));
	entries[entry_index].valid = 0;

	//write the block bitmap back to the disk
	if (disk->ops->write(disk, 1 + sb.inode_map_sz, sb.block_map_sz, block_bitmap) != SUCCESS){
		free(block_bitmap);
		free(inode_bitmap);
		return -EIO;
	}
	free(block_bitmap);
	//if this function returns an error beyond this point, only part of the updated data
	//will have been written to the disk, and the file system will likely be corrupt.

	//write the inode bitmap back to the disk
	if (disk->ops->write(disk, 1, sb.inode_map_sz, inode_bitmap) != SUCCESS){
		free(inode_bitmap);
		fprintf(stderr, "Error updating inode bitmap when deleting directory '%s'. Disk is probably corrupt.\n", path);
		return -EIO;
	}
	free(inode_bitmap);

	//update the containing dir
	if (disk->ops->write(disk, containing_dir_inode.direct[0], 1, entries) != SUCCESS){
		fprintf(stderr, "Error updating contents of directory '%s' when deleting '%s'. This directory is now corrupt.\n", temp_path, dir_name);
		return -EIO;
	}

	// //return blk and clear inode
	// return_blk(inode->direct[0]);
	// memset(inode, 0, sizeof(struct fs_inode));
	// return_inode(inode_idx);

	// //update
	// update_inode(inode_idx);
	// update_blk();

	return SUCCESS;
}

/**
 * rename - rename a file or directory. You can assume the destination
 * and the source share the same path-prefix so the renaming changes
 * the name without moving the file or directory into another file or
 * directory.
 *
 * Note that this is a simplified version of the UNIX rename
 * functionality - see 'man 2 rename' for full semantics. In
 * particular, the full version can move across directories, replace a
 * destination file, and replace an empty directory with a full one.
 *
 * @param src_path: the source path
 * @param dst_path: the destination path
 *
 * @return: 0 if successful, or -error number
 * 	-ENOENT   - source file or directory does not exist
 * 	-ENOTDIR  - component of source or target path not a directory
 * 	-EEXIST   - destination already exists
 * 	-EINVAL   - source and destination not in the same directory
*/
static int fs_rename(const char *src_path, const char *dst_path)
{
	//deep copy both path
	char *_src_path = strdup(src_path);
	char *_dst_path = strdup(dst_path);
	//get inodes
	int src_inode_idx = translate(_src_path);
	int dst_inode_idx = translate(_dst_path);
	//if src inode does not exist return error
	if (src_inode_idx < 0) return src_inode_idx;
	//if dst already exist return error
	if (dst_inode_idx >= 0) return -EEXIST;

	//get parent directory inode
	char src_name[FS_FILENAME_SIZE];
	char dst_name[FS_FILENAME_SIZE];
	int src_parent_inode_idx = translate_1(_src_path, src_name);
	int dst_parent_inode_idx = translate_1(_dst_path, dst_name);
	//src and dst should be in the same directory (same parent)
	if (src_parent_inode_idx != dst_parent_inode_idx) return -EINVAL;
	int parent_inode_idx = src_parent_inode_idx;
	if (parent_inode_idx < 0) return parent_inode_idx;

	//read parent dir inode
	struct fs_inode *parent_inode = &inodes[parent_inode_idx];
	if (!S_ISDIR(parent_inode->mode)) {
		return -ENOTDIR;
	}

	struct fs_dirent entries[DIRENTS_PER_BLK];
	memset(entries, 0, DIRENTS_PER_BLK * sizeof(struct fs_dirent));
	if (disk->ops->read(disk, parent_inode->direct[0], 1, entries) < 0) exit(1);

	//make change to buff
	for (int i = 0; i < DIRENTS_PER_BLK; i++) {
		if (entries[i].valid && strcmp(entries[i].name, src_name) == 0) {
			memset(entries[i].name, 0, sizeof(entries[i].name));
			strcpy(entries[i].name, dst_name);
		}
	}

	//write buff to inode
	if (disk->ops->write(disk, parent_inode->direct[0], 1, entries)) exit(1);
	return SUCCESS;
}

/**
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
	char* _path = strdup(path);
	int inode_idx = translate(_path);
	if (inode_idx < 0) return inode_idx;
	struct fs_inode *inode = &inodes[inode_idx];
	//protect system from other modes
	mode |= S_ISDIR(inode->mode) ? S_IFDIR : S_IFREG;
	//change through reference
	inode->mode = mode;
	update_inode(inode_idx);
 	return SUCCESS;
}

/**
 * utime - change modification time.
 *
 * @param path the file or directory path.
 * @param ut utimbuf - see man 'utime' for description.
 * @return 0 if successful, or error value
 *   -ENOENT   - file does not exist
 *   -ENOTDIR  - component of path not a directory
 */
int fs_utime(const char *path, struct utimbuf *ut)
{
	//CS492: your code here
	return -ENOSYS;
}

/**
 * Open a filesystem file or directory path.
 *
 * @param path: the path
 * @param fuse: file info data
 *
 * @return: 0 if successful, or -error number
 *	-ENOENT   - file does not exist
 *	-ENOTDIR  - component of path not a directory
*/
static int fs_open(const char *path, struct fuse_file_info *fi)
{
	char *_path = strdup(path);
	int inode_idx = translate(_path);
	if (inode_idx < 0) return inode_idx;
	if (S_ISDIR(inodes[inode_idx].mode)) return -EISDIR;
	fi->fh = (uint64_t) inode_idx;
	return SUCCESS;
}

static void fs_read_blk(int blk_num, char *buf, size_t len, size_t offset) {
	//CS492: your code here
	char entries[BLOCK_SIZE];
	memset(entries, 0, BLOCK_SIZE);
	if (disk->ops->read(disk, blk_num, 1, entries) < 0) exit(1);
}

static size_t fs_read_dir(size_t inode_idx, char *buf, size_t len, size_t offset) {
	struct fs_inode *inode = &inodes[inode_idx];
	size_t blk_num = offset / BLOCK_SIZE;
	size_t blk_offset = offset % BLOCK_SIZE;
	size_t len_to_read = len;
	while (blk_num < N_DIRECT && len_to_read > 0) {
		size_t cur_len_to_read = len_to_read > BLOCK_SIZE ? (size_t) BLOCK_SIZE - blk_offset : len_to_read;
		size_t temp = blk_offset + cur_len_to_read;

		if (!inode->direct[blk_num]) {
			return len - len_to_read;
		}

		fs_read_blk(inode->direct[blk_num], buf, temp, blk_offset);

		buf += temp;
		len_to_read -= temp;
		blk_num++;
		blk_offset = 0;
	}
	return len - len_to_read;
}

static size_t fs_read_indir1(size_t blk, char *buf, size_t len, size_t offset) {
	uint32_t blk_indices[PTRS_PER_BLK];
	memset(blk_indices, 0, PTRS_PER_BLK * sizeof(uint32_t));
	if (disk->ops->read(disk, (int) blk, 1, blk_indices) < 0) exit(1);

	size_t blk_num = offset / BLOCK_SIZE;
	size_t blk_offset = offset % BLOCK_SIZE;
	size_t len_to_read = len;
	while (blk_num < PTRS_PER_BLK && len_to_read > 0) {
		size_t cur_len_to_read = len_to_read > BLOCK_SIZE ? (size_t) BLOCK_SIZE - blk_offset : len_to_read;
		size_t temp = blk_offset + cur_len_to_read;

		if (!blk_indices[blk_num]) {
			return len - len_to_read;
		}

		fs_read_blk(blk_indices[blk_num], buf, temp, blk_offset);

		buf += temp;
		len_to_read -= temp;
		blk_num++;
		blk_offset = 0;
	}
	return len - len_to_read;
}

static size_t fs_read_indir2(size_t blk, char *buf, size_t len, size_t offset) {
	uint32_t blk_indices[PTRS_PER_BLK];
	memset(blk_indices, 0, PTRS_PER_BLK * sizeof(uint32_t));
	if (disk->ops->read(disk, (int) blk, 1, blk_indices) < 0) return 0;

	size_t blk_num = offset / INDIR1_SIZE;
	size_t blk_offset = offset % INDIR1_SIZE;
	size_t len_to_read = len;
	while (blk_num < PTRS_PER_BLK && len_to_read > 0) {
		size_t cur_len_to_read = len_to_read > INDIR1_SIZE ? (size_t) INDIR1_SIZE - blk_offset : len_to_read;
		size_t temp = blk_offset + cur_len_to_read;

		if (!blk_indices[blk_num]) {
			return len - len_to_read;
		}

		temp = fs_read_indir1(blk_indices[blk_num], buf, temp, blk_offset);

		buf += temp;
		len_to_read -= temp;
		blk_num++;
		blk_offset = 0;
	}
	return len - len_to_read;
}

/**
 * read - read data from an open file.
 *
 * @param path: the path to the file
 * @param buf: the buffer to keep the data
 * @param len: the number of bytes to read
 * @param offset: the location to start reading at
 * @param fi: fuse file info
 *
 * @return: return exactly the number of bytes requested, except:
 * - if offset >= file len, return 0
 * - if offset+len > file len, return bytes from offset to EOF
 * - on error, return <0
 * 	-ENOENT  - file does not exist
 * 	-EISDIR  - file is in fact a directory
 * 	-ENOTDIR - component of path not a directory
 * 	-EIO     - error reading block
 *
 * Note: similar to fs_write, except that:
 * 1) we cannot read past the end of the file (so you need to add a test
 *    for that, that limits the len to be read in this case)
 * 2) there's no need to allocate or update anything since we are only
 *    reading the file.
*/
static int fs_read(const char *path, char *buf, size_t len, off_t offset,
		    struct fuse_file_info *fi)
{
	//CS492: your code here
	int inode_num = inode_from_full_path(path);
	if (inode_num == -1){
		return -EIO;
	}
	if (inode_num < 0){
		return inode_num; //either -ENOENT or -ENOTDIR
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
	//copy all the blocks into one contiguous chunk, then copy from there to buf
	char *all_blocks = malloc((last_block_num - first_block_num + 1) * FS_BLOCK_SIZE);
	if (all_blocks == NULL){
		return -EIO;
	}
	int current_block_num = 0;
	for (int i = first_block_num; i <= last_block_num; i++){
		//figure out where block i is, then copy it to all_blocks
		if (read_block_of_file(i, &inode, all_blocks + (current_block_num * FS_BLOCK_SIZE)) == -EIO){
			free(all_blocks);
			return -EIO;
		}
		current_block_num++;
	}
	//all_blocks now holds all the blocks that contain the requested data, now just copy
	//the right amount from the right offset
	memcpy(buf, all_blocks + (offset % FS_BLOCK_SIZE), len);
	free(all_blocks);
	return len;
}

static void fs_write_blk(int blk_num, const char *buf, size_t len, size_t offset) {
	char entries[BLOCK_SIZE];
	memset(entries, 0, BLOCK_SIZE);
	if (disk->ops->read(disk, blk_num, 1, entries) < 0) exit(1);
	memcpy(entries + offset, buf, len);
	if (disk->ops->write(disk, blk_num, 1, entries) < 0) exit(1);
}

static size_t fs_write_dir(size_t inode_idx, const char *buf, size_t len, size_t offset) {
	struct fs_inode *inode = &inodes[inode_idx];
	size_t blk_num = offset / BLOCK_SIZE;
	size_t blk_offset = offset % BLOCK_SIZE;
	size_t len_to_write = len;
	while (blk_num < N_DIRECT && len_to_write > 0) {
		size_t cur_len_to_write = len_to_write > BLOCK_SIZE ? (size_t) BLOCK_SIZE - blk_offset : len_to_write;
		//size_t temp = blk_offset + cur_len_to_write;
		size_t temp = cur_len_to_write;

		if (!inode->direct[blk_num]) {
			int freeb = get_free_blk();
			if (freeb < 0) return len - len_to_write;
			inode->direct[blk_num] = freeb;
			update_inode(inode_idx);
		}

		fs_write_blk(inode->direct[blk_num], buf, temp, blk_offset);

		buf += temp;
		len_to_write -= temp;
		blk_num++;
		blk_offset = 0;
	}
	return len - len_to_write;
}

static size_t fs_write_indir1(size_t blk, const char *buf, size_t len, size_t offset) {
	uint32_t blk_indices[PTRS_PER_BLK];
	memset(blk_indices, 0, PTRS_PER_BLK * sizeof(uint32_t));
	if (disk->ops->read(disk, (int) blk, 1, blk_indices) < 0) exit(1);

	size_t blk_num = offset / BLOCK_SIZE;
	size_t blk_offset = offset % BLOCK_SIZE;
	size_t len_to_write = len;
	while (blk_num < PTRS_PER_BLK && len_to_write > 0) {
		size_t cur_len_to_write = len_to_write > BLOCK_SIZE ? (size_t) BLOCK_SIZE - blk_offset : len_to_write;
		size_t temp = blk_offset + cur_len_to_write;

		if (!blk_indices[blk_num]) {
			int freeb = get_free_blk();
			if (freeb < 0) return len - len_to_write;
			blk_indices[blk_num] = freeb;
			//write back
			if (disk->ops->write(disk, blk, 1, blk_indices) < 0)
				exit(1);
		}

		fs_write_blk(blk_indices[blk_num], buf, temp, blk_offset);

		buf += temp;
		len_to_write -= temp;
		blk_num++;
		blk_offset = 0;
	}
	return len - len_to_write;
}

static size_t fs_write_indir2(size_t blk, const char *buf, size_t len, size_t offset) {
	uint32_t blk_indices[PTRS_PER_BLK];
	memset(blk_indices, 0, PTRS_PER_BLK * sizeof(uint32_t));
	if (disk->ops->read(disk, (int) blk, 1, blk_indices) < 0) return 0;

	size_t blk_num = offset / INDIR1_SIZE;
	size_t blk_offset = offset % INDIR1_SIZE;
	size_t len_to_write = len;
	while (blk_num < PTRS_PER_BLK && len_to_write > 0) {
		size_t cur_len_to_write = len_to_write > INDIR1_SIZE ? (size_t) INDIR1_SIZE - blk_offset : len_to_write;
		size_t temp = blk_offset + cur_len_to_write;
		len_to_write -= temp;
		if (!blk_indices[blk_num]) {
			int freeb = get_free_blk();
			if (freeb < 0) return len - len_to_write;
			blk_indices[blk_num] = freeb;
			//write back
			if (disk->ops->write(disk, blk, 1, blk_indices) < 0)
				exit(1);
		}

		temp = fs_write_indir1(blk_indices[blk_num], buf, temp, blk_offset);
		if (temp == 0) return len - len_to_write;
		buf += temp;
		blk_num++;
		blk_offset = 0;
	}
	return len - len_to_write;
}

/**
 * write - write data to a file
 *
 * @param path: the file path
 * @param buf: the buffer to write
 * @param len: the number of bytes to write
 * @param offset: the offset to starting writing at
 * @param fi: the Fuse file info for writing
 *
 * @return: It should return exactly the number of bytes requested, except on error.
 *
 * 	-ENOENT  - file does not exist
 * 	-EISDIR  - file is in fact a directory
 *	-ENOTDIR - component of path not a directory
 *	-EINVAL  - if 'offset' is greater than current file length.
 *  			(POSIX semantics support the creation of files with
 *  			"holes" in them, but we don't)
*/
static int fs_write(const char *path, const char *buf, size_t len,
		     off_t offset, struct fuse_file_info *fi)
{
	char *_path = strdup(path);
	int inode_idx = translate(_path);
	if (inode_idx < 0) return inode_idx;
	struct fs_inode *inode = &inodes[inode_idx];
	if (S_ISDIR(inode->mode)) return -EISDIR;
	if (offset > inode->size) return 0;

	//len need to write
	size_t len_to_write = len;

	//write direct blocks
	if (len_to_write > 0 && offset < DIR_SIZE) {
		//len finished write
		size_t temp = fs_write_dir(inode_idx, buf, len_to_write, (size_t) offset);
		len_to_write -= temp;
		offset += temp;
		buf += temp;
	}

	//write indirect 1 blocks
	if (len_to_write > 0 && offset < DIR_SIZE + INDIR1_SIZE) {
		//need to allocate indir_1
		if (!inode->indir_1) {
			int freeb = get_free_blk();
			if (freeb < 0) return len - len_to_write;
			inode->indir_1 = freeb;
			update_inode(inode_idx);
		}
		size_t temp = fs_write_indir1(inode->indir_1, buf, len_to_write, (size_t) offset - DIR_SIZE);
		len_to_write -= temp;
		offset += temp;
		buf += temp;
	}

	//write indirect 2 blocks
	if (len_to_write > 0 && offset < DIR_SIZE + INDIR1_SIZE + INDIR2_SIZE) {
		//need to allocate indir_2
		if (!inode->indir_2) {
			int freeb = get_free_blk();
			if (freeb < 0) return len - len_to_write;
			inode->indir_2 = freeb;
			update_inode(inode_idx);
		}
		//len finshed write
		size_t temp = fs_write_indir2(inode->indir_2, buf, len_to_write, (size_t) offset - DIR_SIZE - INDIR1_SIZE);
		len_to_write -= temp;
		offset += len_to_write;
	}

	if (offset > inode->size) inode->size = offset;

	//update inode and blk
	update_inode(inode_idx);
	update_blk();

	return (int) (len - len_to_write);
}

/**
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
	char *_path = strdup(path);
	int inode_idx = translate(_path);
	if (inode_idx < 0) return inode_idx;
	if (S_ISDIR(inodes[inode_idx].mode)) return -EISDIR;
	fi->fh = (uint64_t) -1;
	return SUCCESS;
}

/**
 * statfs - get file system statistics. See 'man 2 statfs' for
 * description of 'struct statvfs'.
 *
 * @param path: the path to the file (ignored)
 * @param st: pointer to the destination statvfs struct
 *
 * @return: 0 if successful
 *
 * Errors
 *   none -  Needs to work
*/
static int fs_statfs(const char *path, struct statvfs *st)
{
	/* needs to return the following fields (set others to zero):
	 *   f_bsize = BLOCK_SIZE
	 *   f_blocks = total image - metadata
	 *   f_bfree = f_blocks - blocks used
	 *   f_bavail = f_bfree
	 *   f_namelen = <whatever your max namelength is>
	 */

	//clear original stats
	memset(st, 0, sizeof(*st));
	st->f_bsize = FS_BLOCK_SIZE;
	st->f_blocks = (fsblkcnt_t) (n_blocks - root_inode - inode_base);
	st->f_bfree = (fsblkcnt_t) num_free_blk();
	st->f_bavail = st->f_bfree;
	st->f_namemax = FS_FILENAME_SIZE - 1;

	return 0;
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
	.utime = fs_utime,
	.truncate = fs_truncate,
	.open = fs_open,
	.read = fs_read,
	.write = fs_write,
	.release = fs_release,
	.statfs = fs_statfs,
};

/*#pragma clang diagnostic pop*/
