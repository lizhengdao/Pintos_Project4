#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* A directory. */
struct dir 
{
  struct inode *inode;                /* Backing store. */
  off_t pos;                          /* Current position. */
};

/* A single directory entry. */
struct dir_entry 
{
  disk_sector_t inode_sector;         /* Sector number of header. */
  char name[NAME_MAX + 1];            /* Null terminated file name. */
  bool in_use;                        /* In use or free? */
};

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (disk_sector_t sector, size_t entry_cnt) 
{
  return inode_create (sector, entry_cnt * sizeof (struct dir_entry), true);
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL && inode_is_dir(inode))
  {
    dir->inode = inode;
    dir->pos = 0;
    return dir;
  }
  else
  {
    inode_close (inode);
    free (dir);
    return NULL; 
  }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
  {
    inode_close (dir->inode);
    free (dir);
  }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
    struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
      ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
    {
      if (ep != NULL)
	*ep = e;
      if (ofsp != NULL)
	*ofsp = ofs;
      return true;
    }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
    struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (lookup (dir, name, &e, NULL))
  {
    *inode = inode_open (e.inode_sector);
    set_parentdir (*inode, inode_reopen (dir_get_inode (dir)));
  }
  else
    *inode = NULL;

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, disk_sector_t inode_sector) 
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.

     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
      ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* If deleting process dir, check dir_removed to true */
  if (inode_get_inumber (thread_current ()->dir->inode) == inode_get_inumber (inode))
  {
      thread_current ()->dir_removed = true;
  }

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

done:
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;
  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
  {
    dir->pos += sizeof e;
    if (e.in_use)
    {
      strlcpy (name, e.name, NAME_MAX + 1);
      return true;
    } 
  }
  return false;
}

/* Get directory structure DIR that DIRFILE is located at 
 * if DIRFILE is /a/b/c, return directory structure of /a/b
 * if DIRFILE is /, return root
 * (char ptr input DIRFILE unmodified in this function)*/
struct dir *
get_dir (const char *dirfile)
{
  if (!dirfile || strlen (dirfile) == 0)
    return NULL;

  /* set directory structures */
  struct dir *dir = thread_current()->dir;
  struct inode *inode = NULL;

  /* set strings */
  int dirfilelen = strlen(dirfile) + 1;
  char copy[dirfilelen], *sub, *subnext, *save_ptr;
  memcpy(copy, dirfile, dirfilelen);
  sub = (char*)calloc(1, NAME_MAX + 1);
  if (!sub)
    return NULL;

  /* obtain topmost directory */
  if ((dir) && (copy[0] != '/'))
  {
      if (thread_current ()->dir_removed)
	  return NULL;

      dir = dir_reopen (dir);
  }
  else
  {
    dir = dir_open_root();
  }
 
  /* go to directories */
  subnext = strtok_r(copy, "/", &save_ptr);
  if (!subnext) // result of parsing is NULL
      return dir;
  
  memcpy(sub, subnext, NAME_MAX + 1);
  while ((subnext = strtok_r (NULL, "/", &save_ptr)) != NULL)
  {
    if (!strcmp(sub, ".")); //stay at current directory
    else if (!strcmp(sub, "..")) //go to parent directory
    {
      dir = dir_open (inode_reopen (get_parentdir (dir->inode)));
    }
    else{ //lookup directory, get inode for child, open child directory
	if (!dir_lookup(dir, sub, &inode)){
	    dir_close(dir);
	    free(sub);
	    return NULL;
	}
	dir_close(dir); //close current directory
	dir = dir_open(inode);
    }

    memcpy(sub, subnext, NAME_MAX + 1);
  }

  free(sub);
  return dir;
}
