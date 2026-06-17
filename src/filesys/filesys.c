#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
}

/* Resolves PATH into a parent directory and a final file name
   component.  Stores the final component in NAME (up to
   NAME_MAX characters).  Returns the opened parent directory,
   or NULL on failure.  Caller must close the returned directory. */
static struct dir *
resolve_path (const char *path, char name[NAME_MAX + 1])
{
  struct dir *dir;
  char *copy, *token, *save_ptr, *prev;

  if (path == NULL || *path == '\0')
    return NULL;

  copy = malloc (strlen (path) + 1);
  if (copy == NULL)
    return NULL;
  strlcpy (copy, path, strlen (path) + 1);

  /* Start from root or CWD. */
  if (path[0] == '/')
    dir = dir_open_root ();
  else
    {
      struct thread *cur = thread_current ();
      if (cur->cwd != NULL)
        dir = dir_reopen (cur->cwd);
      else
        dir = dir_open_root ();
    }

  if (dir == NULL)
    {
      free (copy);
      return NULL;
    }

  /* Tokenize and walk the path. */
  prev = NULL;
  for (token = strtok_r (copy, "/", &save_ptr);
       token != NULL;
       token = strtok_r (NULL, "/", &save_ptr))
    {
      if (prev != NULL)
        {
          /* Traverse into the intermediate directory PREV. */
          struct inode *inode;
          if (!dir_lookup (dir, prev, &inode))
            {
              dir_close (dir);
              free (copy);
              return NULL;
            }
          if (!inode_is_dir (inode))
            {
              inode_close (inode);
              dir_close (dir);
              free (copy);
              return NULL;
            }
          dir_close (dir);
          dir = dir_open (inode);
          if (dir == NULL)
            {
              free (copy);
              return NULL;
            }
        }
      prev = token;
    }

  /* Copy the final component name. */
  if (prev == NULL)
    strlcpy (name, ".", NAME_MAX + 1);
  else
    strlcpy (name, prev, NAME_MAX + 1);

  free (copy);

  /* Fail if the resolved directory has been removed. */
  if (inode_is_removed (dir_get_inode (dir)))
    {
      dir_close (dir);
      return NULL;
    }

  return dir;
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  char file_name[NAME_MAX + 1];
  struct dir *dir = resolve_path (name, file_name);
  if (dir == NULL)
    return false;

  block_sector_t inode_sector = 0;
  bool success = (free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size)
                  && dir_add (dir, file_name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  char file_name[NAME_MAX + 1];
  struct dir *dir = resolve_path (name, file_name);
  struct inode *inode = NULL;

  if (dir != NULL)
    dir_lookup (dir, file_name, &inode);
  dir_close (dir);

  return file_open (inode);
}

/* Opens the inode at PATH.  Returns the inode if successful,
   NULL otherwise.  Caller must close the returned inode. */
struct inode *
filesys_open_inode (const char *path)
{
  char file_name[NAME_MAX + 1];
  struct dir *dir = resolve_path (path, file_name);
  struct inode *inode = NULL;

  if (dir != NULL)
    dir_lookup (dir, file_name, &inode);
  dir_close (dir);

  /* Refuse to open a removed inode. */
  if (inode != NULL && inode_is_removed (inode))
    {
      inode_close (inode);
      return NULL;
    }

  return inode;
}

/* Deletes the file or empty directory named NAME.
   Returns true if successful, false on failure. */
bool
filesys_remove (const char *name) 
{
  char file_name[NAME_MAX + 1];
  struct dir *dir = resolve_path (name, file_name);
  if (dir == NULL)
    return false;

  /* Look up the target to check directory constraints. */
  struct inode *inode;
  if (!dir_lookup (dir, file_name, &inode))
    {
      dir_close (dir);
      return false;
    }

  if (inode_is_dir (inode))
    {
      /* Cannot remove root. */
      if (inode_get_inumber (inode) == ROOT_DIR_SECTOR)
        {
          inode_close (inode);
          dir_close (dir);
          return false;
        }
      /* Directory must be empty. */
      struct dir *target = dir_open (inode);   /* takes ownership */
      if (target == NULL)
        {
          dir_close (dir);
          return false;
        }
      bool empty = dir_is_empty (target);
      dir_close (target);
      if (!empty)
        {
          dir_close (dir);
          return false;
        }
    }
  else
    inode_close (inode);

  bool success = dir_remove (dir, file_name);
  dir_close (dir); 

  return success;
}

/* Creates a directory at PATH.  Returns true if successful. */
bool
filesys_mkdir (const char *path)
{
  char dir_name[NAME_MAX + 1];
  struct dir *parent = resolve_path (path, dir_name);
  if (parent == NULL)
    return false;

  block_sector_t sector = 0;
  block_sector_t parent_sector = inode_get_inumber (dir_get_inode (parent));

  bool success = (free_map_allocate (1, &sector)
                  && dir_create (sector, parent_sector)
                  && dir_add (parent, dir_name, sector));
  if (!success && sector != 0)
    free_map_release (sector, 1);
  dir_close (parent);
  return success;
}

/* Changes the current thread's working directory to PATH.
   Returns true if successful. */
bool
filesys_chdir (const char *path)
{
  char dir_name[NAME_MAX + 1];
  struct dir *parent = resolve_path (path, dir_name);
  if (parent == NULL)
    return false;

  struct inode *inode;
  if (!dir_lookup (parent, dir_name, &inode))
    {
      dir_close (parent);
      return false;
    }
  dir_close (parent);

  if (!inode_is_dir (inode))
    {
      inode_close (inode);
      return false;
    }

  struct dir *new_cwd = dir_open (inode);
  if (new_cwd == NULL)
    return false;

  struct thread *cur = thread_current ();
  if (cur->cwd != NULL)
    dir_close (cur->cwd);
  cur->cwd = new_cwd;
  return true;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, ROOT_DIR_SECTOR))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
