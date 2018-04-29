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
char arr[8] = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 };
// #define BITSET(bitmapblocks, blockaddr) (*(bitmapblocks + blockaddr / 8) & (0x80 >> (blockaddr % 8))) 
#define BITSET(bitmapblocks, blockaddr) ((*(bitmapblocks + blockaddr / 8)) & (arr[blockaddr % 8])) 
#define PBITSET(bitmapblocks, blockaddr) printf("blockaddr %u, value %u\n", blockaddr, *(bitmapblocks))
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

int check_inode_type(struct dinode *inode) {
    switch(inode->type) {
        case T_DIR:
        case T_FILE:
        case T_DEV:
            break;
        default:
            return 1;
    }

    return 0;
}

int check_inode_direct_blocks(image_t *image, struct dinode *inode) {
    int i;
    uint blockaddr;
    for (i = 0; i < NDIRECT; i++) {
        blockaddr = inode->addrs[i];
        if (blockaddr == 0)
            continue;
        
        if (blockaddr < 0 || blockaddr >= image->sb->size || !BITSET(image->bitmapblocks, blockaddr)) {
            return 1;
        }
    }
    return 0;
}

int check_inode_indirect_blocks(image_t *image, struct dinode *inode) {
    uint blockaddr;
    blockaddr = inode->addrs[NDIRECT];
    uint *indirectblk;
    int i;

    if (blockaddr == 0)
        return 0;

    if (blockaddr < 0 || blockaddr >= image->sb->size || !(BITSET(image->bitmapblocks, blockaddr))) {
        return 1;
    }

    indirectblk = (uint *) (image->datablocks + blockaddr * BSIZE);
    for (i = 0; i < NINDIRECT; i++, indirectblk++) {
        blockaddr = *(indirectblk);
        if (blockaddr == 0)
            continue;
        
        PBITSET(image->bitmapblocks, blockaddr);
        printf("bit set = %d\n", BITSET(image->bitmapblocks, blockaddr));
        if (blockaddr < 0 || blockaddr >= image->sb->size || !BITSET(image->bitmapblocks, blockaddr)) {
            return 1;
        }
    }
    return 0;
}

int inode_test(image_t *image) {
    struct dinode *inode;
    int i;
    int count_not_allocated = 0;
    //char arr[sizeof(struct dinode)];

    inode = (struct dinode *)(image->inodeblocks);
    //memset(arr, 0, sizeof(struct dinode));
    
    for(i = 0; i < image->sb->ninodes; i++, inode++) {
        /*if (memcmp(inode, arr, sizeof(struct dinode)) == 0) {
            count_not_allocated++;
            continue;
        }*/
        if (inode->type == 0) {
            count_not_allocated++;
            continue;
        }

        if (check_inode_type(inode) != 0) {
            PERROR("ERROR: bad inode.\n");
            return 1;
        }

        if (check_inode_direct_blocks(image, inode) != 0) {
            PERROR("ERROR: bad direct address in inode.\n");
            return 1;
        }

        if (check_inode_indirect_blocks(image, inode) != 0) {
            PERROR("ERROR: bad indirect address in inode.\n");
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
    image.numbitmapblocks =  (image.sb->nblocks / BPB) + 1;
    
    image.inodeblocks = (char *) (mmapimage + BSIZE * 2);
    image.bitmapblocks = (char *) (image.inodeblocks + BSIZE * image.numinodeblocks);
    image.datablocks = (char *) (image.bitmapblocks + BSIZE * image.numbitmapblocks);
    
    rv = inode_test(&image); 
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
