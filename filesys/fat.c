#include "filesys/fat.h"

#include <stdio.h>
#include <string.h>

#include "devices/disk.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Should be less than DISK_SECTOR_SIZE */
struct fat_boot {
  unsigned int magic;
  unsigned int sectors_per_cluster; /* Fixed to 1 */
  unsigned int total_sectors;
  unsigned int fat_start;
  unsigned int fat_sectors; /* Size of FAT in sectors. */
  unsigned int root_dir_cluster;
  unsigned int free_head;
};

/* FAT FS */
struct fat_fs {
  struct fat_boot bs;
  unsigned int *fat;
  unsigned int fat_length;
  disk_sector_t data_start;
  cluster_t last_clst;
  struct lock write_lock;
  unsigned int free_head;
};

static struct fat_fs *fat_fs;

void fat_boot_create(void);
void fat_fs_init(void);
void fat_link_init(void);
cluster_t fat_get_free(void);

void fat_init(void) {
  fat_fs = calloc(1, sizeof(struct fat_fs));
  if (fat_fs == NULL) PANIC("FAT init failed");

  // Read boot sector from the disk
  unsigned int *bounce = malloc(DISK_SECTOR_SIZE);
  if (bounce == NULL) PANIC("FAT init failed");
  disk_read(filesys_disk, FAT_BOOT_SECTOR, bounce);
  memcpy(&fat_fs->bs, bounce, sizeof(fat_fs->bs));
  free(bounce);

  // Extract FAT info
  if (fat_fs->bs.magic != FAT_MAGIC) fat_boot_create();
  fat_fs_init();
}

void fat_open(void) {
  fat_fs->fat = calloc(fat_fs->fat_length, sizeof(cluster_t));
  if (fat_fs->fat == NULL) PANIC("FAT load failed");

  // Load FAT directly from the disk
  uint8_t *buffer = (uint8_t *)fat_fs->fat;
  off_t bytes_read = 0;
  off_t bytes_left = sizeof(fat_fs->fat);
  const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof(cluster_t);
  for (unsigned i = 1; i < fat_fs->bs.fat_sectors; i++) {
    bytes_left = fat_size_in_bytes - bytes_read;
    if (bytes_left >= DISK_SECTOR_SIZE) {
      disk_read(filesys_disk, fat_fs->bs.fat_start + i, buffer + bytes_read);
      bytes_read += DISK_SECTOR_SIZE;
    } else {
      uint8_t *bounce = malloc(DISK_SECTOR_SIZE);
      if (bounce == NULL) PANIC("FAT load failed");
      disk_read(filesys_disk, fat_fs->bs.fat_start + i, bounce);
      memcpy(buffer + bytes_read, bounce, bytes_left);
      bytes_read += bytes_left;
      free(bounce);
    }
  }
}

void fat_close(void) {
  // Write FAT boot sector
  uint8_t *bounce = calloc(1, DISK_SECTOR_SIZE);
  if (bounce == NULL) PANIC("FAT close failed");
  fat_fs->bs.free_head = fat_fs->free_head;
  memcpy(bounce, &fat_fs->bs, sizeof(fat_fs->bs));
  disk_write(filesys_disk, FAT_BOOT_SECTOR, bounce);
  free(bounce);

  // Write FAT directly to the disk
  uint8_t *buffer = (uint8_t *)fat_fs->fat;
  off_t bytes_wrote = 0;
  off_t bytes_left = sizeof(fat_fs->fat);
  const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof(cluster_t);
  for (unsigned i = 1; i < fat_fs->bs.fat_sectors; i++) {
    bytes_left = fat_size_in_bytes - bytes_wrote;
    if (bytes_left >= DISK_SECTOR_SIZE) {
      disk_write(filesys_disk, fat_fs->bs.fat_start + i, buffer + bytes_wrote);
      bytes_wrote += DISK_SECTOR_SIZE;
    } else {
      bounce = calloc(1, DISK_SECTOR_SIZE);
      if (bounce == NULL) PANIC("FAT close failed");
      memcpy(bounce, buffer + bytes_wrote, bytes_left);
      disk_write(filesys_disk, fat_fs->bs.fat_start + i, bounce);
      bytes_wrote += bytes_left;
      free(bounce);
    }
  }
}

void fat_create(void) {
  // Create FAT boot
  fat_boot_create();
  fat_fs_init();

  // Create FAT table
  fat_fs->fat = calloc(fat_fs->fat_length, sizeof(cluster_t));
  if (fat_fs->fat == NULL) PANIC("FAT creation failed");

  // Set up ROOT_DIR_CLST
  fat_put(ROOT_DIR_CLUSTER, EOChain);

  // Fill up ROOT_DIR_CLUSTER region with 0
  uint8_t *buf = calloc(1, DISK_SECTOR_SIZE);
  if (buf == NULL) PANIC("FAT create failed due to OOM");
  disk_write(filesys_disk, cluster_to_sector(ROOT_DIR_CLUSTER), buf);
  free(buf);
  fat_link_init();
}

void fat_boot_create(void) {
  unsigned int fat_sectors =
      (disk_size(filesys_disk) - 1) /
          (DISK_SECTOR_SIZE / sizeof(cluster_t) * SECTORS_PER_CLUSTER + 1) +
      1;
  fat_fs->bs = (struct fat_boot){
      .magic = FAT_MAGIC,
      .sectors_per_cluster = SECTORS_PER_CLUSTER,
      .total_sectors = disk_size(filesys_disk),
      .fat_start = 1,
      .fat_sectors = fat_sectors,
      .root_dir_cluster = ROOT_DIR_CLUSTER,
  };
}

void fat_fs_init(void) {
  fat_fs->fat_length = sector_to_cluster(fat_fs->bs.total_sectors);
  fat_fs->data_start =
      sector_to_cluster(fat_fs->bs.fat_start + fat_fs->bs.fat_sectors);
  fat_fs->last_clst =
      sector_to_cluster(fat_fs->bs.total_sectors - SECTORS_PER_CLUSTER);
  fat_fs->free_head = fat_fs->bs.free_head;
  lock_init(&fat_fs->write_lock);
}

void fat_link_init(void) {
  cluster_t tail;

  fat_fs->free_head = fat_fs->data_start;
  tail = fat_fs->data_start;
  for (cluster_t i = fat_fs->data_start + 1; i <= fat_fs->last_clst; ++i) {
    fat_put(tail, i);
    tail = i;
  }
  if (tail) {
    fat_put(tail, EOChain);
  }
}

/*----------------------------------------------------------------------------*/
/* FAT handling                                                               */
/*----------------------------------------------------------------------------*/

cluster_t fat_get_free(void) {
  cluster_t ret;
  if (fat_fs->free_head == EOChain) {
    return 0;
  }
  ret = fat_fs->free_head;
  fat_fs->free_head = fat_get(fat_fs->free_head);
  fat_put(ret, EOChain);
  return ret;
}

/* Add a cluster to the chain.
 * If CLST is 0, start a new chain.
 * Returns 0 if fails to allocate a new cluster. */
cluster_t fat_create_chain(cluster_t clst) {
  cluster_t new;

  lock_acquire(&fat_fs->write_lock);
  new = fat_get_free();
  if (new == 0) {
    lock_release(&fat_fs->write_lock);
    return 0;
  }

  if (clst == 0) {
    fat_put(new, EOChain);
  } else if (fat_get(clst) == 0) {
    fat_put(new, EOChain);
    fat_put(clst, new);
  } else {
    fat_put(new, fat_get(clst));
    fat_put(clst, new);
  }
  lock_release(&fat_fs->write_lock);
  return new;
}

/* Add COUNT clusters to the chain.
 * If CLST is 0, start a new chain.
 * Returns 0 if fails to allocate COUNT clusters. */
cluster_t fat_create_chain_multiple(cluster_t clst, unsigned count) {
  cluster_t start = 0;
  cluster_t cur = 0;

  cur = fat_create_chain(clst);
  if (cur == 0) {
    fat_remove_chain(clst, 0);
    return 0;
  }

  start = cur;
  for (cluster_t i = 1; i < count; ++i) {
    cur = fat_create_chain(cur);
    if (cur == 0) {
      fat_remove_chain(clst, i);
      return 0;
    }
  }
  return start;
}

/* Remove the chain of clusters starting from CLST.
 * If PCLST is 0, assume CLST as the start of the chain. */
void fat_remove_chain(cluster_t clst, cluster_t pclst) {
  cluster_t cur = clst;
  cluster_t next;

  lock_acquire(&fat_fs->write_lock);
  while (cur != EOChain) {
    next = fat_get(cur);
    fat_put(cur, fat_fs->free_head);
    fat_fs->free_head = cur;
    cur = next;
  }

  if (pclst != 0) {
    fat_put(pclst, fat_fs->free_head);
    fat_fs->free_head = pclst;
  }
  lock_release(&fat_fs->write_lock);
}

cluster_t fat_end_of_chain(cluster_t clst) {
  cluster_t curr = clst, nxt;

  while (curr != EOChain) {
    nxt = fat_get(curr);
    if (nxt == EOChain || nxt == 0) break;
    curr = nxt;
  }
  return curr;
}

/* Update a value in the FAT table. */
void fat_put(cluster_t clst, cluster_t val) { fat_fs->fat[clst] = val; }

/* Fetch a value in the FAT table. */
cluster_t fat_get(cluster_t clst) { return fat_fs->fat[clst]; }

/* Convert a cluster number to a sector number. */
disk_sector_t cluster_to_sector(cluster_t clst) {
  return (disk_sector_t)clst * SECTORS_PER_CLUSTER;
}

/* Convert a sector number to a cluster number. */
cluster_t sector_to_cluster(disk_sector_t sector) {
  return (cluster_t)sector / SECTORS_PER_CLUSTER +
         (sector % SECTORS_PER_CLUSTER);
}
