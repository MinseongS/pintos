#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>

#include "filesys/off_t.h"

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0 /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1 /* Root directory file inode sector. */

/* Disk used for file system. */
extern struct disk *filesys_disk;
struct dir;

void filesys_init(bool format);
void filesys_done(void);
bool filesys_create(const char *name, off_t initial_size);
bool filesys_create_dir(const char *name);
bool filesys_change_dir(const char *name);
struct file *filesys_open(const char *name);
bool filesys_remove(const char *name);
int filesys_create_symlink(const char *target, const char *linkpath);

bool open_parent_dir(const char *path, struct dir *cur_dir,
                     struct dir **parent_dir);
char *get_filename(const char *path);

#endif /* filesys/filesys.h */
