#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* Number of direct block pointers in an inode. */
#define DIRECT_BLOCKS 123

/* Number of block_sector_t entries that fit in one disk sector. */
#define PTRS_PER_SECTOR (BLOCK_SECTOR_SIZE / sizeof (block_sector_t))

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long.
   Layout: length(4) + magic(4) + direct[123](492) + indirect(4)
           + doubly_indirect(4) + unused(4) = 512 bytes. */
struct inode_disk
  {
    off_t length;                             /* File size in bytes. */
    unsigned magic;                           /* Magic number. */
    block_sector_t direct[DIRECT_BLOCKS];     /* Direct block pointers. */
    block_sector_t indirect;                  /* Indirect block pointer. */
    block_sector_t doubly_indirect;           /* Doubly indirect block pointer. */
    uint32_t is_dir;                          /* 1 if directory, 0 if file. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos >= inode->data.length)
    return (block_sector_t) -1;

  off_t idx = pos / BLOCK_SECTOR_SIZE;

  /* Direct blocks. */
  if (idx < DIRECT_BLOCKS)
    return inode->data.direct[idx];

  idx -= DIRECT_BLOCKS;

  /* Indirect block. */
  if (idx < (off_t) PTRS_PER_SECTOR)
    {
      block_sector_t buffer[PTRS_PER_SECTOR];
      block_read (fs_device, inode->data.indirect, buffer);
      return buffer[idx];
    }

  idx -= PTRS_PER_SECTOR;

  /* Doubly indirect block. */
  {
    block_sector_t outer[PTRS_PER_SECTOR];
    block_sector_t inner[PTRS_PER_SECTOR];
    block_read (fs_device, inode->data.doubly_indirect, outer);
    block_read (fs_device, outer[idx / PTRS_PER_SECTOR], inner);
    return inner[idx % PTRS_PER_SECTOR];
  }
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Extends DISK_INODE to cover at least NEW_LENGTH bytes by
   allocating new sectors one at a time.
   Returns true if successful, false on allocation failure. */
static bool
inode_extend (struct inode_disk *disk_inode, off_t new_length)
{
  static char zeros[BLOCK_SECTOR_SIZE];
  size_t old_sectors = bytes_to_sectors (disk_inode->length);
  size_t new_sectors = bytes_to_sectors (new_length);
  size_t i;

  /* If file size is 0 but new_length is also 0 (or shrinking), nothing to do
     except update length. */
  if (new_sectors <= old_sectors)
    {
      disk_inode->length = new_length;
      return true;
    }

  for (i = old_sectors; i < new_sectors; i++)
    {
      /* Allocate one data sector. */
      block_sector_t new_sector;
      if (!free_map_allocate (1, &new_sector))
        return false;
      block_write (fs_device, new_sector, zeros);

      if (i < DIRECT_BLOCKS)
        {
          /* Direct block slot. */
          disk_inode->direct[i] = new_sector;
        }
      else if (i < DIRECT_BLOCKS + PTRS_PER_SECTOR)
        {
          /* Indirect block slot. */
          size_t ind_idx = i - DIRECT_BLOCKS;
          if (ind_idx == 0)
            {
              /* First entry — allocate the indirect block itself. */
              if (!free_map_allocate (1, &disk_inode->indirect))
                {
                  free_map_release (new_sector, 1);
                  return false;
                }
              block_write (fs_device, disk_inode->indirect, zeros);
            }
          block_sector_t buf[PTRS_PER_SECTOR];
          block_read (fs_device, disk_inode->indirect, buf);
          buf[ind_idx] = new_sector;
          block_write (fs_device, disk_inode->indirect, buf);
        }
      else
        {
          /* Doubly indirect block slot. */
          size_t di_off = i - DIRECT_BLOCKS - PTRS_PER_SECTOR;
          size_t outer_idx = di_off / PTRS_PER_SECTOR;
          size_t inner_idx = di_off % PTRS_PER_SECTOR;

          if (di_off == 0)
            {
              /* First entry — allocate the doubly-indirect block. */
              if (!free_map_allocate (1, &disk_inode->doubly_indirect))
                {
                  free_map_release (new_sector, 1);
                  return false;
                }
              block_write (fs_device, disk_inode->doubly_indirect, zeros);
            }

          block_sector_t outer[PTRS_PER_SECTOR];
          block_read (fs_device, disk_inode->doubly_indirect, outer);

          if (inner_idx == 0)
            {
              /* First entry in this 2nd-level block — allocate it. */
              block_sector_t new_ind;
              if (!free_map_allocate (1, &new_ind))
                {
                  free_map_release (new_sector, 1);
                  return false;
                }
              block_write (fs_device, new_ind, zeros);
              outer[outer_idx] = new_ind;
              block_write (fs_device, disk_inode->doubly_indirect, outer);
            }

          block_sector_t inner[PTRS_PER_SECTOR];
          block_read (fs_device, outer[outer_idx], inner);
          inner[inner_idx] = new_sector;
          block_write (fs_device, outer[outer_idx], inner);
        }
    }

  disk_inode->length = new_length;
  return true;
}

/* Releases all data sectors (and index blocks) owned by
   DISK_INODE. */
static void
inode_deallocate (struct inode_disk *disk_inode)
{
  size_t sectors = bytes_to_sectors (disk_inode->length);
  size_t i, j;

  if (sectors == 0)
    return;

  /* Free direct blocks. */
  size_t direct_cnt = sectors < DIRECT_BLOCKS ? sectors : DIRECT_BLOCKS;
  for (i = 0; i < direct_cnt; i++)
    free_map_release (disk_inode->direct[i], 1);

  if (sectors <= DIRECT_BLOCKS)
    return;

  /* Free indirect block entries, then the indirect block itself. */
  {
    block_sector_t buf[PTRS_PER_SECTOR];
    block_read (fs_device, disk_inode->indirect, buf);
    size_t ind_cnt = sectors - DIRECT_BLOCKS;
    if (ind_cnt > PTRS_PER_SECTOR)
      ind_cnt = PTRS_PER_SECTOR;
    for (i = 0; i < ind_cnt; i++)
      free_map_release (buf[i], 1);
    free_map_release (disk_inode->indirect, 1);
  }

  if (sectors <= DIRECT_BLOCKS + PTRS_PER_SECTOR)
    return;

  /* Free doubly indirect block entries. */
  {
    block_sector_t outer[PTRS_PER_SECTOR];
    block_read (fs_device, disk_inode->doubly_indirect, outer);
    size_t remaining = sectors - DIRECT_BLOCKS - PTRS_PER_SECTOR;
    size_t outer_cnt = DIV_ROUND_UP (remaining, PTRS_PER_SECTOR);
    for (i = 0; i < outer_cnt; i++)
      {
        block_sector_t inner[PTRS_PER_SECTOR];
        block_read (fs_device, outer[i], inner);
        size_t inner_cnt = remaining < PTRS_PER_SECTOR
                             ? remaining : PTRS_PER_SECTOR;
        for (j = 0; j < inner_cnt; j++)
          free_map_release (inner[j], 1);
        free_map_release (outer[i], 1);
        remaining -= inner_cnt;
      }
    free_map_release (disk_inode->doubly_indirect, 1);
  }
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = 0;
      disk_inode->magic = INODE_MAGIC;

      if (length > 0)
        {
          if (!inode_extend (disk_inode, length))
            {
              free (disk_inode);
              return false;
            }
        }

      block_write (fs_device, sector, disk_inode);
      success = true;
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  block_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1);
          inode_deallocate (&inode->data);
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else 
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if an error occurs.
   A write past end-of-file extends the inode. */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  /* Extend the file if writing past current end-of-file. */
  if (offset + size > inode_length (inode))
    {
      if (!inode_extend (&inode->data, offset + size))
        return 0;
      /* Persist the updated inode to disk. */
      block_write (fs_device, inode->sector, &inode->data);
    }

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      else 
        {
          /* We need a bounce buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left) 
            block_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_write (fs_device, sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}

/* Returns true if INODE represents a directory. */
bool
inode_is_dir (const struct inode *inode)
{
  return inode->data.is_dir != 0;
}

/* Marks INODE as a directory (IS_DIR true) or regular file (false)
   and persists the change to disk. */
void
inode_set_dir (struct inode *inode, bool is_dir)
{
  inode->data.is_dir = is_dir ? 1 : 0;
  block_write (fs_device, inode->sector, &inode->data);
}

/* Returns true if INODE has been marked for removal. */
bool
inode_is_removed (const struct inode *inode)
{
  return inode->removed;
}
