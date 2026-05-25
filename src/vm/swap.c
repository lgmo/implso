#include "vm/swap.h"
#include "bitmap.h"
#include "devices/block.h"
#include "filesys/file.h"
#include "list.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "vm/frame_table.h"
#include "vm/sup_page_table.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static struct block *global_swap_block;
static struct bitmap *swap_slots_bitmap;
static struct lock swap_slots_bitmap_lock;

void
swap_init (void)
{
    size_t num_slots;

    global_swap_block = block_get_role (BLOCK_SWAP);
    if (global_swap_block == NULL)
        {
            swap_slots_bitmap = NULL;
            return;
        }

    lock_init (&swap_slots_bitmap_lock);
    num_slots = block_size (global_swap_block) / 8;
    swap_slots_bitmap = bitmap_create (num_slots);
}

static void
read_from_block (uint8_t *frame, int index)
{
    for (int i = 0; i < 8; i++)
        {
            block_read (global_swap_block, index * 8 + i,
                        frame + (i * BLOCK_SECTOR_SIZE));
        }
}

static void
write_to_block (uint8_t *frame, int index)
{
    for (int i = 0; i < 8; i++)
        {
            block_write (global_swap_block, index * 8 + i,
                         frame + (i * BLOCK_SECTOR_SIZE));
        }
}

void
swap_free_slot (size_t index)
{
    if (swap_slots_bitmap == NULL || index == BITMAP_ERROR
        || index >= bitmap_size (swap_slots_bitmap))
        return;
    lock_acquire (&swap_slots_bitmap_lock);
    bitmap_reset (swap_slots_bitmap, index);
    lock_release (&swap_slots_bitmap_lock);
}

bool
swap_read_slot (size_t index, uint8_t *dst)
{
    if (swap_slots_bitmap == NULL || dst == NULL || index == BITMAP_ERROR
        || index >= bitmap_size (swap_slots_bitmap))
        return false;

    lock_acquire (&swap_slots_bitmap_lock);
    read_from_block (dst, index);
    lock_release (&swap_slots_bitmap_lock);
    return true;
}

void *
swap_evict_frame (void)
{
    struct frame_table_entry *fte = frame_table_pop_lru ();
    struct sup_page_table_entry *spte;

    if (!fte)
        return NULL;

    spte = fte->aux;
    if (spte == NULL || fte->owner == NULL || fte->owner->pagedir == NULL)
        {
            free (fte);
            return NULL;
        }

    bool dirty = pagedir_is_dirty (fte->owner->pagedir, spte->user_vaddr);

    if (spte->source == FILE_BACKED)
        {
            void *res = fte->frame;
            bool use_swap = (spte->mapping == NULL && spte->writable);

            if (spte->mapping != NULL && dirty)
                {
                    filesys_lock_acquire ();
                    if (file_write_at (spte->file, fte->frame, spte->read_bytes,
                                       spte->ofs)
                        != (int)spte->read_bytes)
                        {
                            filesys_lock_release ();
                            frame_table_insert (fte);
                            return NULL;
                        }
                    filesys_lock_release ();
                }

            if (!use_swap)
                {
                    pagedir_clear_page (fte->owner->pagedir, spte->user_vaddr);
                    spte->is_loaded = false;
                    spte->source = FILE_BACKED;
                    spte->swap_slot_index = BITMAP_ERROR;
                    free (fte);
                    return res;
                }
        }

    size_t index;
    void *res = fte->frame;
    lock_acquire (&swap_slots_bitmap_lock);
    index = bitmap_scan_and_flip (swap_slots_bitmap, 0, 1, false);
    lock_release (&swap_slots_bitmap_lock);
    if (index == BITMAP_ERROR)
        {
            frame_table_insert (fte);
            return NULL;
        }

    write_to_block ((uint8_t *)fte->frame, index);
    spte->swap_slot_index = index;
    spte->is_loaded = false;
    spte->source = SWAP;
    pagedir_clear_page (fte->owner->pagedir, spte->user_vaddr);
    free (fte);

    return res;
}

bool
swap_reclaim_page (struct sup_page_table_entry *spte,
                   struct frame_table_entry *fte)
{
    if (spte == NULL || fte == NULL)
        return false;
    if (spte->is_loaded || spte->source != SWAP
        || spte->swap_slot_index == BITMAP_ERROR
        || swap_slots_bitmap == NULL
        || spte->swap_slot_index >= bitmap_size (swap_slots_bitmap))
        return false;

    lock_acquire (&swap_slots_bitmap_lock);
    read_from_block ((uint8_t *)fte->frame, spte->swap_slot_index);
    lock_release (&swap_slots_bitmap_lock);
    return true;
}
