#include "vm/memory_mapping.h"
#include "bitmap.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "vm/frame_table.h"
#include "vm/sup_page_table.h"
#include "vm/swap.h"

void
memory_mapping_table_init (struct list *mmap_table)
{
  list_init (mmap_table);
}

struct memory_mapping *
memory_mapping_create (struct file *file, void *user_vaddr, size_t length,
                       mapid_t mapid)
{
  struct memory_mapping *mapping = malloc (sizeof *mapping);
  if (mapping == NULL)
    return NULL;

  mapping->file = file;
  mapping->user_vaddr = user_vaddr;
  mapping->length = length;
  mapping->mapid = mapid;
  return mapping;
}

void
memory_mapping_destroy (struct memory_mapping *mapping)
{
  free (mapping);
}

void
memory_mapping_table_push (struct list *mmap_table,
                           struct memory_mapping *mapping)
{
  list_push_back (mmap_table, &mapping->mapping_elem);
}

struct memory_mapping *
memory_mapping_find (struct list *mmap_table, mapid_t mapid)
{
  struct list_elem *e;

  for (e = list_begin (mmap_table); e != list_end (mmap_table);
       e = list_next (e))
    {
      struct memory_mapping *mapping
          = list_entry (e, struct memory_mapping, mapping_elem);
      if (mapping->mapid == mapid)
        return mapping;
    }

  return NULL;
}

void
memory_mapping_table_remove (struct memory_mapping *mapping)
{
  list_remove (&mapping->mapping_elem);
}

void
memory_mapping_unmap (struct thread *t, struct memory_mapping *mapping)
{
  uint8_t *upage;
  off_t ofs;

  if (t == NULL || mapping == NULL)
    return;

  upage = (uint8_t *) mapping->user_vaddr;
  ofs = 0;
  while (ofs < (off_t) mapping->length)
    {
      uint32_t page_read_bytes = (mapping->length - (size_t) ofs) < PGSIZE
                                     ? (uint32_t) (mapping->length - (size_t) ofs)
                                     : PGSIZE;
      struct sup_page_table_entry *spte = sup_page_table_find (upage);

      if (spte != NULL && spte->mapping == mapping)
        {
          if (spte->is_loaded)
            {
              bool dirty = pagedir_is_dirty (t->pagedir, upage);
              void *kpage = pagedir_get_page (t->pagedir, upage);
              if (dirty && kpage != NULL)
                {
                  filesys_lock_acquire ();
                  file_write_at (mapping->file, kpage, page_read_bytes,
                                 spte->ofs);
                  filesys_lock_release ();
                }
              if (kpage != NULL)
                {
                  void *frame = pg_round_down (kpage);
                  if (frame_table_remove_by_kpage (frame, t))
                    palloc_free_page (frame);
                }
              pagedir_clear_page (t->pagedir, upage);
            }
          else if (spte->source == SWAP
                   && spte->swap_slot_index != BITMAP_ERROR)
            {
              uint8_t *tmp = palloc_get_page (0);
              if (tmp != NULL)
                {
                  if (swap_read_slot (spte->swap_slot_index, tmp))
                    {
                      filesys_lock_acquire ();
                      file_write_at (mapping->file, tmp, page_read_bytes,
                                     spte->ofs);
                      filesys_lock_release ();
                    }
                  palloc_free_page (tmp);
                }
            }

          if (spte->source == SWAP && spte->swap_slot_index != BITMAP_ERROR)
            swap_free_slot (spte->swap_slot_index);

          sup_page_table_remove (spte);
        }

      ofs += page_read_bytes;
      upage += PGSIZE;
    }

  filesys_lock_acquire ();
  file_close (mapping->file);
  filesys_lock_release ();
  memory_mapping_destroy (mapping);
}

void
memory_mapping_table_unmap_all (struct thread *t, struct list *mmap_table)
{
  while (!list_empty (mmap_table))
    {
      struct list_elem *e = list_pop_front (mmap_table);
      struct memory_mapping *mapping
          = list_entry (e, struct memory_mapping, mapping_elem);
      memory_mapping_unmap (t, mapping);
    }
}
