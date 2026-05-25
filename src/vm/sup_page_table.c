#include "sup_page_table.h"
#include "bitmap.h"
#include "hash.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include <stdint.h>
#include <stdlib.h>

unsigned
sup_page_hash (const struct hash_elem *elem, void *aux UNUSED)
{
    const struct sup_page_table_entry *spte
        = hash_entry (elem, struct sup_page_table_entry, hash_elem);
    return hash_bytes (&spte->user_vaddr, sizeof spte->user_vaddr);
}

bool
sup_page_less (const struct hash_elem *a, const struct hash_elem *b,
               void *aux UNUSED)
{
    const struct sup_page_table_entry *sptea
        = hash_entry (a, struct sup_page_table_entry, hash_elem);
    const struct sup_page_table_entry *spteb
        = hash_entry (b, struct sup_page_table_entry, hash_elem);
    return sptea->user_vaddr < spteb->user_vaddr;
}

struct sup_page_table_entry *
sup_page_table_find (void *fault_addr)
{
    struct sup_page_table_entry aux;

    struct thread *t = thread_current ();

    struct hash_elem *elem;

    void *upage = pg_round_down (fault_addr);

    aux.user_vaddr = upage;
    elem = hash_find (&t->sup_page_table, &aux.hash_elem);

    if (!elem)
        {
            return NULL;
        }
    return hash_entry (elem, struct sup_page_table_entry, hash_elem);
}

struct sup_page_table_entry *
sup_page_table_add (void *fault_addr)
{
    struct sup_page_table_entry *spte;
    struct sup_page_table_entry aux;
    struct hash_elem *elem;

    struct thread *t = thread_current ();

    void *upage = pg_round_down (fault_addr);

    aux.user_vaddr = upage;
    elem = hash_find (&t->sup_page_table, &aux.hash_elem);
    if (elem != NULL)
        return hash_entry (elem, struct sup_page_table_entry, hash_elem);

    spte = (struct sup_page_table_entry *)malloc (
        sizeof (struct sup_page_table_entry));
    if (spte == NULL)
        return NULL;

    spte->user_vaddr = upage;
    spte->access_time = 0;
    spte->is_loaded = true;
    spte->source = ZERO;
    spte->swap_slot_index = BITMAP_ERROR;
    spte->file = NULL;
    spte->ofs = 0;
    spte->read_bytes = 0;
    spte->zero_bytes = 0;
    spte->writable = true;
    spte->mapping = NULL;

    if (hash_insert (&t->sup_page_table, &spte->hash_elem) != NULL)
        {
            free (spte);
            elem = hash_find (&t->sup_page_table, &aux.hash_elem);
            if (elem == NULL)
                return NULL;
            return hash_entry (elem, struct sup_page_table_entry, hash_elem);
        }

    return spte;
}

struct sup_page_table_entry *
sup_page_table_add_file (void *upage, struct file *file, off_t ofs,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable)
{
    struct sup_page_table_entry aux;
    struct sup_page_table_entry *spte;
    struct thread *t = thread_current ();
    struct hash_elem *elem;

    upage = pg_round_down (upage);
    aux.user_vaddr = upage;
    elem = hash_find (&t->sup_page_table, &aux.hash_elem);
    if (elem != NULL)
        return hash_entry (elem, struct sup_page_table_entry, hash_elem);

    spte = (struct sup_page_table_entry *)malloc (sizeof *spte);
    if (spte == NULL)
        return NULL;

    spte->user_vaddr = upage;
    spte->access_time = 0;
    spte->is_loaded = false;
    spte->source = FILE_BACKED;
    spte->swap_slot_index = BITMAP_ERROR;
    spte->file = file;
    spte->ofs = ofs;
    spte->read_bytes = read_bytes;
    spte->zero_bytes = zero_bytes;
    spte->writable = writable;
    spte->mapping = NULL;

    hash_insert (&t->sup_page_table, &spte->hash_elem);
    return spte;
}

void
sup_page_table_remove (struct sup_page_table_entry *spte)
{
    hash_delete (&thread_current ()->sup_page_table, &spte->hash_elem);
    free (spte);
}

static void
sup_page_free (struct hash_elem *elem, void *aux UNUSED)
{
    struct sup_page_table_entry *entry
        = hash_entry (elem, struct sup_page_table_entry, hash_elem);
    free (entry);
}
void
sup_page_table_destroy (void)
{
    struct hash *sup_page_table = &thread_current ()->sup_page_table;
    hash_destroy (sup_page_table, sup_page_free);
}
