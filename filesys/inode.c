#include "filesys/inode.h"

#include <debug.h>
#include <list.h>
#include <round.h>
#include <string.h>

#include "filesys/fat.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
 * Must be exactly DISK_SECTOR_SIZE bytes long. */
struct inode_disk {
  disk_sector_t start; /* First data sector. */
  off_t length;        /* File size in bytes. */
  enum file_type_t
      type;             /* 0 is regular file, 1 is directory, 2 is soft link. */
  unsigned magic;       /* Magic number. */
  uint32_t unused[124]; /* Not used. */
};

/* In-memory inode. */
struct inode {
  struct list_elem elem;  /* Element in inode list. */
  disk_sector_t sector;   /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
  off_t file_pos;
};

/* Returns the number of sectors to allocate for an inode SIZE
 * bytes long. */
static inline size_t bytes_to_sectors(off_t size) {
  return DIV_ROUND_UP(size, DISK_SECTOR_SIZE);
}

/* Returns the disk sector that contains byte offset POS within
 * INODE.
 * Returns -1 if INODE does not contain data for a byte at offset
 * POS. */
static disk_sector_t byte_to_sector(const struct inode *inode, off_t pos) {
  ASSERT(inode != NULL);
  if (pos >= inode->data.length) {
    return -1;
  }
#ifdef EFILESYS
  cluster_t cur = sector_to_cluster(inode->data.start);
  for (size_t i = 0; i < pos / DISK_SECTOR_SIZE; ++i) {
    cur = fat_get(cur);
    if (cur == EOChain) {
      return -1;
    }
  }
  return cluster_to_sector(cur);
#else
  return inode->data.start + pos / DISK_SECTOR_SIZE;
#endif
}

/* List of open inodes, so that opening a single inode twice
 * returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init(void) { list_init(&open_inodes); }

/* Initializes an inode with LENGTH bytes of data and
 * writes the new inode to sector SECTOR on the file system
 * disk.
 * Returns true if successful.
 * Returns false if memory or disk allocation fails. */
bool inode_create(disk_sector_t sector, off_t length, enum file_type_t type) {
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT(length >= 0);
  if (sector >= DISK_SECTOR_SIZE * 5) {
    goto done;
  }

  /* If this assertion fails, the inode structure is not exactly
   * one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == DISK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode == NULL) {
    goto done;
  }

  size_t sectors = bytes_to_sectors(length);
  disk_inode->length = length;
  disk_inode->type = type;
  disk_inode->magic = INODE_MAGIC;

#ifdef EFILESYS
  disk_inode->start = fat_create_chain_multiple(0, sectors);
  if (disk_inode->start == 0) {
    goto done;
  }
  disk_write(filesys_disk, sector, disk_inode);
  if (sectors == 0) {
    success = true;
    goto done;
  }
  static char zeros[DISK_SECTOR_SIZE];
  size_t i;
  cluster_t cur = disk_inode->start;

  disk_write(filesys_disk, cluster_to_sector(cur), zeros);
  for (i = 1; i < sectors; ++i) {
    cur = fat_get(cur);
    disk_write(filesys_disk, cluster_to_sector(cur), zeros);
  }

  success = true;
#else
  if (!free_map_allocate(sectors, &disk_inode->start)) {
    goto done;
  }

  disk_write(filesys_disk, sector, disk_inode);
  if (sectors > 0) {
    static char zeros[DISK_SECTOR_SIZE];
    size_t i;

    for (i = 0; i < sectors; i++)
      disk_write(filesys_disk, disk_inode->start + i, zeros);
  }
  success = true;
#endif

done:
  free(disk_inode);
  return success;
}

/* Reads an inode from SECTOR
 * and returns a `struct inode' that contains it.
 * Returns a null pointer if memory allocation fails. */
struct inode *inode_open(disk_sector_t sector) {
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes);
       e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      inode_reopen(inode);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL) return NULL;

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  inode->file_pos = 0;
  disk_read(filesys_disk, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *inode_reopen(struct inode *inode) {
  if (inode != NULL) inode->open_cnt++;
  // if (inode != NULL) inode->file_pos = 0;
  return inode;
}

/* Returns INODE's inode number. */
disk_sector_t inode_get_inumber(const struct inode *inode) {
  ASSERT(inode != NULL);
  return inode->sector;
}

/* Closes INODE and writes it to disk.
 * If this was the last reference to INODE, frees its memory.
 * If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode *inode) {
  /* Ignore null pointer. */
  if (inode == NULL) return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0) {
    /* Remove from inode list and release lock. */
    list_remove(&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed) {
#ifdef EFILESYS
      fat_remove_chain(inode->sector, 0);
      if (inode_file_type(inode) != FILETYPE_SYMLINK) {
        fat_remove_chain(inode->data.start, 0);
      }
#else
      free_map_release(inode->sector, 1);
      free_map_release(inode->data.start, bytes_to_sectors(inode->data.length));
#endif
    }

    free(inode);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
 * has it open. */
void inode_remove(struct inode *inode) {
  ASSERT(inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
 * Returns the number of bytes actually read, which may be less
 * than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size,
                    off_t offset) {
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  ASSERT(inode != NULL);

  while (size > 0) {
    /* Disk sector to read, starting byte offset within sector. */
    disk_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % DISK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = DISK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0) break;

    if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
      /* Read full sector directly into caller's buffer. */
      disk_read(filesys_disk, sector_idx, buffer + bytes_read);
    } else {
      /* Read sector into bounce buffer, then partially copy
       * into caller's buffer. */
      if (bounce == NULL) {
        bounce = malloc(DISK_SECTOR_SIZE);
        if (bounce == NULL) break;
      }
      disk_read(filesys_disk, sector_idx, bounce);
      memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  free(bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
 * Returns the number of bytes actually written, which may be
 * less than SIZE if end of file is reached or an error occurs.
 * (Normally a write at end of file would extend the inode, but
 * growth is not yet implemented.) */
off_t inode_write_at(struct inode *inode, const void *buffer_, off_t size,
                     off_t offset) {
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;
  off_t gap =
      bytes_to_sectors(offset + size) - bytes_to_sectors(inode_length(inode));

  if (inode->deny_write_cnt) return 0;
#ifdef EFILESYS
  if (gap >= 0 && !inode_extend(inode, gap, offset + size)) return 0;
#endif

  while (size > 0) {
    /* Sector to write, starting byte offset within sector. */
    disk_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % DISK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = DISK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0) break;

    if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
      /* Write full sector directly to disk. */
      disk_write(filesys_disk, sector_idx, buffer + bytes_written);
    } else {
      /* We need a bounce buffer. */
      if (bounce == NULL) {
        bounce = malloc(DISK_SECTOR_SIZE);
        if (bounce == NULL) break;
      }

      /* If the sector contains data before or after the chunk
         we're writing, then we need to read in the sector
         first.  Otherwise we start with a sector of all zeros. */
      if (sector_ofs > 0 || chunk_size < sector_left)
        disk_read(filesys_disk, sector_idx, bounce);
      else
        memset(bounce, 0, DISK_SECTOR_SIZE);
      memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
      disk_write(filesys_disk, sector_idx, bounce);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }
  free(bounce);

  if (size + offset > inode_length(inode)) {
    inode->data.length = size + offset;
    disk_write(filesys_disk, inode->sector, &inode->data);
  }

  return bytes_written;
}

bool inode_extend(struct inode *inode, int sectors, int new_size) {
  cluster_t end;
  int result = 0;

  ASSERT(inode != NULL);
  end = fat_end_of_chain(inode->data.start);
  result = fat_create_chain_multiple(end, sectors == 0 ? 1 : sectors);
  if (new_size > inode_length(inode)) {
    inode->data.length = new_size;
    disk_write(filesys_disk, inode->sector, &inode->data);
  }
  return result != 0;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode *inode) {
  ASSERT(inode != NULL);
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
 * Must be called once by each inode opener who has called
 * inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode *inode) {
  ASSERT(inode != NULL);
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode *inode) {
  ASSERT(inode != NULL);
  return inode->data.length;
}

enum file_type_t inode_file_type(struct inode *inode) {
  ASSERT(inode != NULL);
  return inode->data.type;
}

off_t inode_get_file_pos(const struct inode *inode) {
  ASSERT(inode != NULL);
  return inode->file_pos;
}

void inode_file_pos_set(struct inode *inode, off_t pos) {
  ASSERT(inode != NULL);
  inode->file_pos = pos;
}

bool inode_removed(struct inode *inode) {
  ASSERT(inode != NULL);
  return inode->removed;
}
