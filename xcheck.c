#include<stdio.h>
#include<stdlib.h>
#include<sys/mman.h>
#include<sys/types.h>
#include<sys/stat.h>
#include<unistd.h>
#include<fcntl.h>
#include<string.h>
#include "fs.h"

#define PERROR(msg...) fprintf(stderr, msg)
#define ENABLE_LOGS 0

#define INFO(msg...) if (ENABLE_LOGS) printf(msg)

typedef struct _image_t {
    uint numinodeblocks;
    uint numbitmapblocks;
    struct superblock *sb;
    char *inodeblocks;
    char *bitmapblocks;
    char *datablocks;
} image_t;

int inode_type_test(image_t *image) {
    struct dinode *inode;
    int i;
    char arr[sizeof(struct dinode)];
    memset(arr, 0, sizeof(struct dinode));

    inode = (struct dinode *)(image->inodeblocks);
    int count_not_allocated = 0;
    
    for(i = 0; i < image->sb->ninodes; i++, inode++) {
        if (memcmp(inode, arr, sizeof(struct dinode)) == 0) {
            count_not_allocated++;
            continue;
        }
        
        switch(inode->type) {
            case T_DIR:
            case T_FILE:
            case T_DEV:
                break;
            default:
                PERROR("ERROR: bad inode.\n");
                return 1;
        }

    }

    INFO("Num allocated = %d\n", image->sb->ninodes - count_not_allocated);
    INFO("Num not allocated = %d\n", count_not_allocated);
    return 0;
}




int fsck(char *filename) {
	int fd = open(filename, O_RDONLY, 0);
    image_t image;
	struct stat st;
	char *mmapimage;
    int rv = 0;

    if (fd == -1) {
		PERROR("image not found\n");
        return 1;
    }

    if (fstat(fd, &st) != 0) {
        PERROR("failed to fstat file %s\n", filename);
        return 1;
    }
	
	mmapimage = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
	image.sb = (struct superblock *) (mmapimage + BSIZE);
	INFO("nblocks = %u, ninodes = %u, size = %u\n", image.sb->nblocks, image.sb->ninodes, image.sb->size);

    image.numinodeblocks = image.sb->ninodes / IPB + 1;
    image.numbitmapblocks =  (image.sb->nblocks / BSIZE) + 1;
    
    image.inodeblocks = (char *) (mmapimage + BSIZE * 2);
    image.bitmapblocks = (char *) (image.inodeblocks + BSIZE * image.numinodeblocks);
    image.datablocks = (char *) (image.bitmapblocks + BSIZE * image.numbitmapblocks);
    
    rv = inode_type_test(&image); 
    if (rv != 0) 
        goto cleanup;


    // Free
cleanup:
    munmap(mmapimage, st.st_size);
	close(fd);

	return rv;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: xcheck <file_system_image>\n");
        exit(1);
    }

    return fsck(argv[1]);
}   
