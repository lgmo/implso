#include "frame_table.h"
#include "list.h"
#include "stdbool.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "devices/timer.h"
#include "userprog/pagedir.h"
#include <stdint.h>

struct list frame_table;
struct lock frame_table_lock;

void
frame_table_init (void)
{
    list_init (&frame_table);
    lock_init (&frame_table_lock);
}

void
frame_table_destroy (void)
{
    lock_acquire (&frame_table_lock);
    while (!list_empty (&frame_table))
        {
            struct frame_table_entry *fte = list_entry (
                list_pop_front (&frame_table), struct frame_table_entry, elem);
            free (fte);
        }
    lock_release (&frame_table_lock);
}

void
insert_in_frame_table (struct frame_table_entry *fte)
{
    lock_acquire (&frame_table_lock);
    list_push_back (&frame_table, &(fte->elem));
    lock_release (&frame_table_lock);
}

struct frame_table_entry *
add_frame (uint32_t *frame)
{
    struct frame_table_entry *fte
        = (struct frame_table_entry *)malloc (sizeof *fte);
    if (fte == NULL)
        return NULL;
    fte->frame = frame;
    fte->owner = thread_current ();
    fte->aux = NULL;
    insert_in_frame_table (fte);
    return fte;
}

void
remove_frame (struct frame_table_entry *fte)
{
    lock_acquire (&frame_table_lock);
    list_remove (&fte->elem);
    lock_release (&frame_table_lock);
    free (fte);
}

struct frame_table_entry *
find_frame (void *frame, struct thread *owner)
{
    struct list_elem *e;

    lock_acquire (&frame_table_lock);
    for (e = list_begin (&frame_table); e != list_end (&frame_table);
         e = list_next (e))
        {
            struct frame_table_entry *fte
                = list_entry (e, struct frame_table_entry, elem);
            if (fte->frame == frame && fte->owner == owner)
                {
                    lock_release (&frame_table_lock);
                    return fte;
                }
        }
    lock_release (&frame_table_lock);
    return NULL;
}

bool
remove_frame_by_kpage (void *frame, struct thread *owner)
{
    struct list_elem *e;

    lock_acquire (&frame_table_lock);
    for (e = list_begin (&frame_table); e != list_end (&frame_table);
         e = list_next (e))
        {
            struct frame_table_entry *fte
                = list_entry (e, struct frame_table_entry, elem);
            if (fte->frame == frame && fte->owner == owner)
                {
                    list_remove (&fte->elem);
                    lock_release (&frame_table_lock);
                    free (fte);
                    return true;
                }
        }
    lock_release (&frame_table_lock);
    return false;
}

struct frame_table_entry *
pop_lru_frame (void)
{
    struct list_elem *elem;
    struct frame_table_entry *fte = NULL;
    struct frame_table_entry *aux;
    uint64_t now = timer_ticks ();
    int pass;

    lock_acquire (&frame_table_lock);
    for (pass = 0; pass < 2 && fte == NULL; pass++)
        {
            for (elem = list_begin (&frame_table); elem != list_end (&frame_table);
                 elem = list_next (elem))
                {
                    aux = list_entry (elem, struct frame_table_entry, elem);
                    if (aux->aux == NULL || !aux->aux->is_loaded) // n da pra ser evictado
                        continue;

                    if (aux->owner == NULL || aux->owner->pagedir == NULL) // nda pra ser evictado
                        continue;

                    if (pagedir_is_accessed (aux->owner->pagedir,
                                             aux->aux->user_vaddr)) //se a página foi acessada
                        {
                            pagedir_set_accessed (aux->owner->pagedir,
                                                  aux->aux->user_vaddr, false); // zera o bit de acesso
                            aux->aux->accessed = true;
                            aux->aux->access_time = now;
                            continue;
                        }

                    if (!fte || aux->aux->access_time < fte->aux->access_time) //se não tem fte ou tem e a página atual foi acessada a menos tempo
                        fte = aux; // fte recebe a página atual
                }
        }

    if (fte)
        {
            list_remove (&fte->elem);
        }
    lock_release (&frame_table_lock);

    return fte;
}

void
remove_owner_frames (struct thread *owner)// remove as páginas do thread que vai ser deletado
{
    struct list_elem *e;
    struct list_elem *next;

    if (owner == NULL)
        return;

    lock_acquire (&frame_table_lock);
    for (e = list_begin (&frame_table); e != list_end (&frame_table); e = next)
        {
            struct frame_table_entry *fte;
            next = list_next (e);
            fte = list_entry (e, struct frame_table_entry, elem);
            if (fte->owner == owner)
                {
                    list_remove (&fte->elem);
                    free (fte);
                }
        }
    lock_release (&frame_table_lock);
}
