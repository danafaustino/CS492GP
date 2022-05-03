/*
 * file:        image.c
 * description: skeleton code for CS 7600 / CS5600 Homework 3
 *
 * Peter Desnoyers, Northeastern Computer Science, 2011
 * Philip Gust, Northeastern Computer Science, 2019
 */

#define _XOPEN_SOURCE 500

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "blkdev.h"


// should be defined in "string.h" but is not on macos
extern char* strdup(const char *);


/** definition of image block device */
struct image_dev {
    char *path;		// path to device file
    int   fd;		// file descriptor of open file
    int   nblks;	// number of blocks in device
};

/*
The only time the 'private' field of a struct blkdev is ever set is in the image_create
function in this file, and it is set to point to a struct image_dev
I am assuming that the 'private' field of every struct blkdev always points
to a struct image_dev, even though it is really a void*
--Bobby, 4/27/2021
*/

/*
 * 	count the number of blocks on the device
 * 	@param dev: the block device
 * 	@return: the number of blocks in the block device
*/
static int image_num_blocks(struct blkdev *dev)
{
	struct image_dev* temp = dev->private;
	return temp->nblks;
}


/*
 *	read blocks from block device starting at give block index
 * 	@param dev: the block device
 * 	@param first_blk: index of the block to start reading from
 * 	@param nblks: number of blocks to read from the device
 * 	@param buf: buffer to store the data
 * 	@return: return status of the operation (success or error reason -- please refer to blkdev.h for error reasons)
*/
static int image_read(struct blkdev *dev, int first_blk, int nblks, void *buf)
{
	struct image_dev* image_device = dev->private;
	if (image_device->fd == -1){
		//this image device is not open
		return E_UNAVAIL;
	}
	//image_device->fd is the file descriptor of the actual file that stores all the blocks
	//seek to the right point in it (first_blk * BLOCK_SIZE), then read the specified amount
	//from it (nblks * BLOCK_SIZE), and store it in buf
	off_t seek_result = lseek(image_device->fd, first_blk * BLOCK_SIZE, SEEK_SET);
	if (seek_result == -1){
		//seek failed, return one of the errors specified in blkdev.h
		//I'll go with E_SIZE for now
		fprintf(stderr, "image_read: seek failed\n");
		return E_SIZE;
	}
	int amount_to_try_to_read = nblks * BLOCK_SIZE;
	ssize_t amount_actually_read = read(image_device->fd, buf, amount_to_try_to_read);
	if (amount_actually_read == -1){
		//read failed, return one of the errors specified in blkdev.h
		//I'll go with E_BADADDR for now
		return E_BADADDR;
	}
	if (amount_actually_read < amount_to_try_to_read){
		//could not read all the data specified, return one of the error codes in blkdev.h
		//I'll go with E_SIZE for now
		fprintf(stderr, "image_read: could not read all %d bytes, only read %zd bytes\n", amount_to_try_to_read, amount_actually_read);
		return E_SIZE;
	}
	return SUCCESS;
}


/*
 * write bytes to block device starting at give block index
 * @param dev: the block device
 * @param first_blk: index of the block to start writing to
 * @param nblks: number of blocks to write to the device
 * @param buf: buffer where data comes from
 * @return: return status of the operation (success or error reason -- please refer to blkdev.h for error reasons)
*/
static int image_write(struct blkdev * dev, int first_blk, int nblks, void *buf)
{
	struct image_dev* image_device = dev->private;
	if (image_device->fd == -1){
		//this image device is not open
		return E_UNAVAIL;
	}
	if (lseek(image_device->fd, first_blk * BLOCK_SIZE, SEEK_SET) == -1){
		fprintf(stderr, "image_write: seek failed\n");
		return E_SIZE;
	}
	int amount_to_try_to_write = nblks * BLOCK_SIZE;
	ssize_t amount_actually_written = write(image_device->fd, buf, amount_to_try_to_write);
	if (amount_actually_written == -1){
		return E_BADADDR;
	}
	if (amount_actually_written < amount_to_try_to_write){
		fprintf(stderr, "image_read: could not write all %d bytes, only wrote %zd bytes\n", amount_to_try_to_write, amount_actually_written);
		return E_SIZE;
	}
	return SUCCESS;
}


/*
 * Flush the block device.
 * @param dev: the block device
 * @aparam first_blk: index of the block to start flushing 
 * @param nblks: number of blocks to flush
 * @return: return status of the operation (success or error reason -- please refer to blkdev.h for error reasons)
*/
static int image_flush(struct blkdev * dev, int first_blk, int nblks)
{
	//The linux fsync function just flushes the entire file, so I won't
	//do anything with first_blk and nblks
	struct image_dev* image_device = dev->private;
	if (image_device->fd == -1){
		//this image device is not open
		return E_UNAVAIL;
	}
	if (fsync(image_device->fd) == -1){
		//Not quite sure which error to return here
		return E_UNAVAIL;
	}
	return SUCCESS;
}


/* 
 * close the device. After this, access to device will return errors
 * @param dev: the block device
*/
static void image_close(struct blkdev *dev)
{
	struct image_dev *image_device = dev->private;
	if (image_device->fd == -1){
		//this image device is not open
		return;
	}
	if (close(image_device->fd) == -1){
		return;
	}
	//apparently, this function doesn't need to return success/failure
	return;
}


/** Operations on this block device */
static struct blkdev_ops image_ops = {
    .num_blocks = image_num_blocks,
    .read = image_read,
    .write = image_write,
    .flush = image_flush,
    .close = image_close
};

/**
 * Create an image block device reading from a specified image file.
 *
 * @param path the path to the image file
 * @return the block device or NULL if cannot open or read image file
 */
struct blkdev *image_create(char *path)
{
    struct blkdev *dev = malloc(sizeof(*dev));
    struct image_dev *im = malloc(sizeof(*im));

    if (dev == NULL || im == NULL)
        return NULL;

    im->path = strdup(path);    /* save a copy for error reporting */
    
    /* open image device */
    im->fd = open(path, O_RDWR);
    if (im->fd < 0) {
        fprintf(stderr, "can't open image %s: %s\n", path, strerror(errno));
        return NULL;
    }

    /* access image device */
    struct stat sb;
    if (fstat(im->fd, &sb) < 0) {
        fprintf(stderr, "can't access image %s: %s\n", path, strerror(errno));
        return NULL;
    }

    /* print a warning if file is not a multiple of the block size -
     * this isn't a fatal error, as extra bytes beyond the last full
     * block will be ignored by read and write.
     */
    if (sb.st_size % BLOCK_SIZE != 0) {
        fprintf(stderr, "warning: file %s not a multiple of %d bytes\n",
                path, BLOCK_SIZE);
    }
    im->nblks = sb.st_size / BLOCK_SIZE;
    dev->private = im;
    dev->ops = &image_ops;

    return dev;
}

/**
 * Force an image blkdev into failure. After this any
 * further access to that device will return E_UNAVAIL.
 */
void image_fail(struct blkdev *dev)
{
    struct image_dev *im = dev->private;

    if (im->fd != -1) {
        close(im->fd);
    }
    im->fd = -1;
}
