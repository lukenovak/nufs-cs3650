// implementation of storage.h

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include "slist.h"
#include "pages.h"
#include "directory.h"
#include "inode.h"
#include "bitmap.h"
#include "util.h"

// declaring helpers
static void storage_update_time(inode* dd, time_t newa, time_t newm);
static void get_parent_child(const char* path, char* parent, char* child);

// initializes our file structure
void
storage_init(const char* path) {
    // initialize the pages
    pages_init(path);
    // allocate a page for the inode list
    if (!bitmap_get(get_pages_bitmap(), 1)) {
        for (int i = 0; i < 3; i++) {
            int newpage = alloc_page();
            printf("second inode page allocated at page %d\n", newpage);
        }
    }
    // the remaining pages will be alloced when we put data in them

    // then we initialize the root directory if it isn't allocated
    if (!bitmap_get(get_pages_bitmap(), 4)) {
        printf("initializing root directory");
        directory_init();
    }
}

// check to see if the file is available, if not returns -ENOENT
int
storage_access(const char* path) {

    int rv = tree_lookup(path);
    if (rv >= 0) {
        inode* node = get_inode(rv);
        time_t curtime = time(NULL);
        node->atim = curtime;
        return 0;
    }
    else
        return -ENOENT;
}

// mutates the stat with the inode features at the path
int 
storage_stat(const char* path, struct stat* st) {
    int working_inum = tree_lookup(path);
    if (working_inum > 0) {
        inode* node = get_inode(working_inum);
        st->st_mode = node->mode;
        st->st_size = node->size;
        st->st_atime = node->atim;
        st->st_mtime = node->mtim;
        st->st_ctime = node->ctim;
        st->st_nlink = node->refs;
        return 0;
    }
    return -1;
}

int 
storage_truncate(const char *path, off_t size) {
    int inum = tree_lookup(path);
    inode* node = get_inode(inum);
    if (node->size < size) {
	grow_inode(node, size);
    } else {
	shrink_inode(node, size);
    }
    return 0;
}

int 
storage_write(const char* path, const char* buf, size_t size, off_t offset)
{
    // get the start point with the path
    inode* write_node = get_inode(tree_lookup(path));
    if (write_node->size < size + offset) {
        storage_truncate(path, size + offset);
    }
    int bindex = 0;
    int nindex = offset;
    int rem = size;
    while (rem > 0) {
        char* dest = pages_get_page(inode_get_pnum(write_node, nindex));
        dest += nindex % 4096;
        int cpyamnt = min(rem, 4096 - (nindex % 4096));
        memcpy(dest, buf + bindex, cpyamnt);
        bindex += cpyamnt;
        nindex += cpyamnt;
        rem -= cpyamnt;
    }
    return size;    
}

int
storage_read(const char* path, char* buf, size_t size, off_t offset)
{
    printf("storage_read called, buffer is\n%s\n", buf);
    inode* node = get_inode(tree_lookup(path));
    int bindex = 0;
    int nindex = offset;
    int rem = size;
    while (rem > 0) {
        char* src = pages_get_page(inode_get_pnum(node, nindex));
        src += nindex % 4096;
        int cpyamnt = min(rem, 4096 - (nindex % 4096));
        memcpy(buf + bindex, src, cpyamnt);
        bindex += cpyamnt;
        nindex += cpyamnt;
        rem -= cpyamnt;
    }
    return size;
}

int
storage_mknod(const char* path, int mode) {
    // should add a direntry of the correct mode to the
    // directory at the path
        
    // check to make sure the node doesn't alreay exist
    if (tree_lookup(path) != -1) {
        return -EEXIST;
    }
 
    char* item = malloc(50);
    char* parent = malloc(strlen(path));
    get_parent_child(path, parent, item);

    int    pnodenum = tree_lookup(parent);
    if (pnodenum < 0) {
        free(item);
        free(parent);   
        return -ENOENT;
    }
    
    int new_inode = alloc_inode();
    inode* node = get_inode(new_inode);
    node->mode = mode;
    node->size = 0;
    node->refs = 1;
    inode* parent_dir = get_inode(pnodenum);

    directory_put(parent_dir, item, new_inode);
    free(item);
    free(parent);
    return 0;
}

// this is used for the removal of a link. If refs are 0, then we also
// delete the inode associated with the dirent
int
storage_unlink(const char* path) {
    char* nodename = malloc(50);
    char* parentpath = malloc(strlen(path));
    get_parent_child(path, parentpath, nodename);

    inode* parent = get_inode(tree_lookup(parentpath));
    int rv = directory_delete(parent, nodename);

    free(parentpath);
    free(nodename);

    return rv;
}

int    
storage_link(const char *from, const char *to) {
    int tnum = tree_lookup(to);
    if (tnum < 0) {
        return tnum;
    }

    char* fname = malloc(50);
    char* fparent = malloc(strlen(from));
    get_parent_child(from, fparent, fname);

    inode* pnode = get_inode(tree_lookup(fparent));
    directory_put(pnode, fname, tnum);
    get_inode(tnum)->refs ++;
    
    free(fname);
    free(fparent);
    return 0;
}

int
storage_symlink(const char* to, const char* from) {
    int rv = storage_mknod(from, 0120000);
    if (rv < 0 ) {
        return rv;
    }
    storage_write(from, to, strlen(to), 0);
    return 0;
}

int
storage_readlink(const char* path, char* buf, size_t size) {
    return storage_read(path, buf, size, 0);
}

int    
storage_rename(const char *from, const char *to) {
    storage_link(to, from);
    storage_unlink(from);
    return 0;
}

int    
storage_set_time(const char* path, const struct timespec ts[2])
{
    int nodenum = tree_lookup(path);
    if (nodenum < 0) {
        return -ENOENT;
    }
    inode* node = get_inode(nodenum);
    storage_update_time(node, ts[0].tv_sec, ts[1].tv_sec);
    return 0;
}

void   storage_update_ctime(const char* path) {
    int inum = tree_lookup(path);
    if (inum < 0) {
        return;
    }
    get_inode(inum)->ctim = time(NULL);
}

int    storage_chmod(const char* path, mode_t mode) {
    int inum = tree_lookup(path);
    if (inum < 0) {
        return -ENOENT;
    }
    inode* node = get_inode(inum);
    node->mode = node->mode & ~07777 & mode;
}

static
void storage_update_time(inode* dd, time_t newa, time_t newm)
{
    dd->atim = newa;
    dd->mtim = newm;
}

slist* storage_list(const char* path) {
    return directory_list(path);
}

static void get_parent_child(const char* path, char* parent, char* child) {
    slist* flist = s_split(path, '/');
    slist* fdir = flist;
    parent[0] = 0;
    while (fdir->next != NULL) {
        strncat(parent, "/", 1);
        strncat(parent, fdir->data, 48);
        fdir = fdir->next;
    }
    memcpy(child, fdir->data, strlen(fdir->data));
    child[strlen(fdir->data)] = 0;
    s_free(flist);
}
