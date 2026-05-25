#ifndef FRAME_TABLE_H
#define FRAME_TABLE_H

#include "kernel/list.h"
#include "threads/synch.h"
#include "vm/sup_page_table.h"
#include <stdint.h>

extern struct list frame_table;
extern struct lock frame_table_lock;

struct frame_table_entry
{
    uint32_t *frame;
    struct thread *owner;
    struct list_elem elem;
    struct sup_page_table_entry *aux;
};

void frame_table_init (void);
struct frame_table_entry *frame_table_add (uint32_t *frame);
void frame_table_insert (struct frame_table_entry *fte);
void frame_table_remove (struct frame_table_entry *fte);
struct frame_table_entry *frame_table_pop_lru (void);
bool frame_table_remove_by_kpage (void *frame, struct thread *owner);
void frame_table_remove_owner_frames (struct thread *owner);
#endif
