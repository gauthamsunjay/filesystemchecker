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
char bitarr[8] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
#define BITSET(bitmapblocks, blockaddr) ((*(bitmapblocks + blockaddr / 8)) & (bitarr[blockaddr % 8])) 

typedef struct _image_t {
    uint numinodeblocks;
    uint numbitmapblocks;
    uint firstdatablock;
    struct superblock *sb;
    char *inodeblocks;
    char *bitmapblocks;
    char *datablocks;
    char *mmapimage;
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
        
        if (blockaddr < 0 || blockaddr >= image->sb->size) {
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

    if (blockaddr < 0 || blockaddr >= image->sb->size) {
        return 1;
    }

    indirectblk = (uint *) (image->mmapimage + blockaddr * BSIZE);
    for (i = 0; i < NINDIRECT; i++, indirectblk++) {
        blockaddr = *(indirectblk);
        if (blockaddr == 0)
            continue;
        
        if (blockaddr < 0 || blockaddr >= image->sb->size) {
            return 1;
        }
    }
    return 0;
}

int check_dir(image_t *image, struct dinode *inode, int inum) {
    int i, j, pfound, cfound;
    uint blockaddr;
    struct dirent *de;
    pfound = cfound = 0;
    
    for (i = 0; i < NDIRECT; i++) {
        blockaddr = inode->addrs[i];
        if (blockaddr == 0)
            continue;

        de = (struct dirent *) (image->mmapimage + blockaddr * BSIZE);
        for (j = 0; j < DPB; j++, de++) {
            if (!cfound && strcmp(".", de->name) == 0) {
                cfound = 1;
                if (de->inum != inum)
                    return 1;
            }

            if (!pfound && strcmp("..", de->name) == 0) {
                pfound = 1;
                if (inum != 1 && de->inum == inum)
                    return 1;

                if (inum == 1 && de->inum != inum)
                    return 1;
            }

            if (pfound && cfound)
                break;
        }

        if (pfound && cfound)
            break;
    }

    if (!pfound || !cfound)
        return 1;

    return 0;
}

int check_bitmap_addr(image_t *image, struct dinode *inode) {
    int i, j;
    uint blockaddr;
    uint *indirect;

    for (i = 0; i < (NDIRECT + 1); i++) {
        blockaddr = inode->addrs[i];
        if (blockaddr == 0)
            continue;

        if (!BITSET(image->bitmapblocks, blockaddr))
            return 1;
        
        if (i == NDIRECT) {
            indirect = (uint *) (image->mmapimage + blockaddr * BSIZE);
            for (j = 0; j < NINDIRECT; j++, indirect++) {
                blockaddr = *(indirect);
                if (blockaddr == 0)
                    continue;

                if (!BITSET(image->bitmapblocks, blockaddr))
                    return 1;
            }
        }
    }
    return 0;
}

int inode_test(image_t *image) {
    struct dinode *inode;
    int i;
    int count_not_allocated = 0;

    inode = (struct dinode *)(image->inodeblocks);
    
    for(i = 0; i < image->sb->ninodes; i++, inode++) {
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
        
        if (i == 1 && (inode->type != T_DIR || check_dir(image, inode, i) != 0)) {
            PERROR("ERROR: root directory does not exist.\n");
            return 1;
        }

        if (inode->type == T_DIR && check_dir(image, inode, i) != 0) {
            PERROR("ERROR: directory not properly formatted.\n");
            return 1;
        }

        if (check_bitmap_addr(image, inode) != 0) {
            PERROR("ERROR: address used by inode but marked free in bitmap.\n");
            return 1;
        }

    }

    return 0;
}

void get_used_dbs(image_t *image, struct dinode *inode, int *used_dbs) {
    int i, j;
    uint blockaddr;
    uint *indirect;

    for (i = 0; i < (NDIRECT + 1); i++) {
        blockaddr = inode->addrs[i];
        if (blockaddr == 0)
            continue;

        used_dbs[blockaddr - image->firstdatablock] = 1;
        
        if (i == NDIRECT) {
            indirect = (uint *) (image->mmapimage + blockaddr * BSIZE);
            for (j = 0; j < NINDIRECT; j++, indirect++) {
                blockaddr = *(indirect);
                if (blockaddr == 0)
                    continue;

                used_dbs[blockaddr - image->firstdatablock] = 1;
            }
        }
    }
}

int bitmap_test(image_t *image) {    
    struct dinode *inode;
    int i;
    int used_dbs[image->sb->nblocks];
    uint blockaddr;
    memset(used_dbs, 0, image->sb->nblocks * sizeof(int));

    inode = (struct dinode *)(image->inodeblocks);
    for(i = 0; i < image->sb->ninodes; i++, inode++) {
        if (inode->type == 0)
            continue;

        get_used_dbs(image, inode, used_dbs);
    }
    
    for (i = 0; i < image->sb->nblocks; i++) {
        blockaddr = (uint) (i + image->firstdatablock);
        if (used_dbs[i] == 0 && BITSET(image->bitmapblocks, blockaddr)) {
            PERROR("ERROR: bitmap marks block in use but it is not in use.\n");
            return 1;
        }
    }

    return 0;
}

void fill_duaddrs(image_t *image, struct dinode *inode, uint *duaddrs) {
    int i;
    uint blockaddr;

    for (i = 0; i < NDIRECT; i++) {
        blockaddr = inode->addrs[i];
        if (blockaddr == 0)
            continue;

        duaddrs[blockaddr - image->firstdatablock]++;
    }
}

void fill_iuaddrs(image_t *image, struct dinode *inode, uint *iuaddrs) {
    int i;
    uint *indirect;
    uint blockaddr = inode->addrs[NDIRECT];
    
    indirect = (uint *) (image->mmapimage + blockaddr * BSIZE);
    for (i = 0; i < NINDIRECT; i++, indirect++) {
        blockaddr = *(indirect);
        if (blockaddr == 0)
            continue;

        iuaddrs[blockaddr - image->firstdatablock]++;
    }
}

int blockaddrs_test(image_t *image) {
    struct dinode *inode;
    int i;
    uint duaddrs[image->sb->nblocks];
    memset(duaddrs, 0, sizeof(uint) * image->sb->nblocks);

    uint iuaddrs[image->sb->nblocks];
    memset(iuaddrs, 0, sizeof(uint) * image->sb->nblocks);

    inode = (struct dinode *)(image->inodeblocks);
    
    for(i = 0; i < image->sb->ninodes; i++, inode++) {
        if (inode->type == 0)
            continue;

        fill_duaddrs(image, inode, duaddrs);
        fill_iuaddrs(image, inode, iuaddrs);
    }

    for (i = 0; i < image->sb->nblocks; i++) {
        if (duaddrs[i] > 1) {
            PERROR("ERROR: direct address used more than once.\n");
            return 1;
        }

        if (iuaddrs[i] > 1) {
            PERROR("ERROR: indirect address used more than once.\n");
            return 1;
        }
    }

    return 0;
}

void traverse_dirs(image_t *image, struct dinode *rootinode, int *inodemap) {
    int i, j;
    uint blockaddr;
    uint *indirect;
    struct dinode *inode;
    struct dirent *dir;
    
    if (rootinode->type == T_DIR) {
        for (i = 0; i < NDIRECT; i++) {
            blockaddr = rootinode->addrs[i];
            if (blockaddr == 0)
                continue;

            dir = (struct dirent *) (image->mmapimage + blockaddr * BSIZE);
            for (j = 0; j < DPB; j++, dir++) {
                if (dir->inum != 0 && strcmp(dir->name, ".") != 0 && strcmp(dir->name, "..") != 0) {
                    inode = ((struct dinode *) (image->inodeblocks)) + dir->inum;
                    inodemap[dir->inum]++;
                    traverse_dirs(image, inode, inodemap);
                }
            }
        }

        blockaddr = rootinode->addrs[NDIRECT];
        if (blockaddr != 0) {
            indirect = (uint *) (image->mmapimage + blockaddr * BSIZE);
            for (i = 0; i < NINDIRECT; i++, indirect++) {
                blockaddr = *(indirect);
                if (blockaddr == 0)
                    continue;

                dir = (struct dirent *) (image->mmapimage + blockaddr * BSIZE);

                for (j = 0; j < DPB; j++, dir++) {
                    if (dir->inum != 0 && strcmp(dir->name, ".") != 0 && strcmp(dir->name, "..") != 0) {
                        inode = ((struct dinode *) (image->inodeblocks)) + dir->inum;
                        inodemap[dir->inum]++;
                        traverse_dirs(image, inode, inodemap);
                    }
                }
            }
        }
    }
}

int directory_check(image_t *image) {
    int i;
    int inodemap[image->sb->ninodes];
    memset(inodemap, 0, sizeof(int) * image->sb->ninodes);
    struct dinode *inode, *rootinode;

    inode = (struct dinode *) (image->inodeblocks);
    rootinode = ++inode;
    
    inodemap[0]++;
    inodemap[1]++;
    
    traverse_dirs(image, rootinode, inodemap);
    
    inode++;
    for (i = 2; i < image->sb->ninodes; i++, inode++) {
        if (inode->type != 0 && inodemap[i] == 0) {
            PERROR("ERROR: inode marked use but not found in a directory.\n");
            return 1;
        }

        if (inodemap[i] > 0 && inode->type == 0) {
            PERROR("ERROR: inode referred to in directory but marked free.\n");
            return 1;
        }

        if (inode->type == T_FILE && inode->nlink != inodemap[i]) {
            PERROR("ERROR: bad reference count for file.\n");
            return 1;
        }
    
        if (inode->type == T_DIR && inodemap[i] > 1) {
            PERROR("ERROR: directory appears more than once in file system.\n");
            return 1;
        }
    }
    return 0;
}

int fsck(char *filename) {
	int fd = open(filename, O_RDONLY, 0);
    image_t image;
	struct stat st;
	char *mmapimage;
    int rv = 0;
    
    if (fd == -1) {
		PERROR("image not found.\n");
        return 1;
    }

    if (fstat(fd, &st) != 0) {
        PERROR("failed to fstat file %s\n", filename);
        return 1;
    }
	
	mmapimage = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
    image.mmapimage = mmapimage;
	image.sb = (struct superblock *) (mmapimage + BSIZE);

    image.numinodeblocks = (image.sb->ninodes / (IPB)) + 1;
    image.numbitmapblocks =  (image.sb->size / (BPB)) + 1;
    
    image.inodeblocks = (char *) (mmapimage + BSIZE * 2);
    image.bitmapblocks = (char *) (image.inodeblocks + BSIZE * image.numinodeblocks);
    image.datablocks = (char *) (image.bitmapblocks + BSIZE * image.numbitmapblocks);
    
    image.firstdatablock = image.numinodeblocks + image.numbitmapblocks + 2;
    

    rv = inode_test(&image); 
    if (rv != 0) 
        goto cleanup;

    rv = bitmap_test(&image);
    if (rv != 0)
        goto cleanup;
    
    rv = blockaddrs_test(&image);
    if (rv != 0)
        goto cleanup;
    
    rv = directory_check(&image);
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
        PERROR("Usage: xcheck <file_system_image>\n");
        exit(1);
    }

    return fsck(argv[1]);
}   
