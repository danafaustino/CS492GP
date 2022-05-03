CC=gcc
#I added the -g flag here to make debugging in gdb easier
CFLAGS=-D_FILE_OFFSET_BITS=64 -Wall -g
LIBS=-lfuse

all:
	$(CC) $(CFLAGS) *.c -o fsx492 $(LIBS)

clean:
	rm -f fsx492

#to reset the image, just do make reset_img
reset_img:
	git checkout -- test/fsx492.img

#to unmount the image, just do make unmount
unmount:
	fusermount -u /tmp/use
