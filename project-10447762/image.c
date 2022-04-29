/*
 * file:        image.c
 * description: skeleton code for CS492
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
	char *path; // path to device file
	int   fd; // file descriptor of open file
	int   nblks; // number of blocks in device
};

/**
 * To count the number of blocks on the device
 * @param dev: the block device
 * @return: the number of blocks in the block device
*/
static int image_num_blocks(struct blkdev *dev)
{
	//CS492: your code here
	return -1;
}


/**
 * To read blocks from block device starting at give block index
 * @param dev: the block device
 * @param first_blk: index of the block to start reading from
 * @param nblks: number of blocks to read from the device
 * @param buf: buffer to store the data
 * @return: SUCCESS if successful, E_UNAVAIL if device unavailable
*/
static int image_read(struct blkdev *dev, int first_blk, int nblks, void *buf)
{
	struct image_dev *im = dev->private;

	/* Check whether the disk is unavailable */
	if (im->fd == -1) {
		return E_UNAVAIL;
	}

	assert(first_blk >= 0 && first_blk+nblks <= im->nblks);

	int result = pread(im->fd, buf, nblks*BLOCK_SIZE, first_blk*BLOCK_SIZE);

	/* Since we already checked the address, this shouldn't
	 * happen very often.
	 */
	if (result < 0) {
		fprintf(stderr, "read error on %s: %s\n", im->path, strerror(errno));
		assert(0);
	}
	if (result != nblks*BLOCK_SIZE) {
		fprintf(stderr, "short read on %s: %s\n", im->path, strerror(errno));
		assert(0);
	}
	return SUCCESS;
}

/**
 * To write bytes to block device starting at give block index
 * @param dev: the block device
 * @param first_blk: index of the block to start writing to
 * @param nblks: number of blocks to write to the device
 * @param buf: buffer where data comes from
 * @return SUCCESS if successful, E_UNAVAIL if device unavailable
 *
 * Note: add in your code a test to print a warning message when
 * writing to the superblock (block 0).  The write should then still
 * happen.
*/
static int image_write(struct blkdev * dev, int first_blk, int nblks, void *buf)
{
	//CS492: your code here
	return -1;
}

/**
 * Flush the block device.
 * @param dev: the block device
 * @aparam first_blk: index of the block to start flushing 
 * @param nblks: number of blocks to flush
 * @return SUCCESS if successful, E_UNAVAIL if device unavailable
 *
 * Note: this function does not actually flush anything, it just returns
 * one or the other of the twp possible return values.
*/
static int image_flush(struct blkdev * dev, int first_blk, int nblks)
{
	//CS492: your code here
	return -1;
}

/**
 * Close the block device (if it's available).
 * @param dev: the block device
 *
 * Note: this is the opposite of image_create and, in addition to
 * closing the device, should also free all allocated memory.
*/
static void image_close(struct blkdev *dev)
{
	//CS492: your code here
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
 * Create an image block device by reading from a specified image file.
 *
 * @param path: the path to the image file
 * @return the block device or NULL if cannot open or read image file
 */
struct blkdev *image_create(char *path)
{
	struct blkdev *dev = malloc(sizeof(*dev));
	struct image_dev *im = malloc(sizeof(*im));

	if (dev == NULL || im == NULL)
		return NULL;

	im->path = strdup(path); /* save a copy for error reporting */
	
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
