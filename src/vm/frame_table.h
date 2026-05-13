#ifndef FRAME_TABLE_H
#define FRAME_TABLE_H

#include <stdint.h>
#include "kernel/list.h"
#include "threads/synch.h"

extern struct list frame_table;
extern struct lock frame_table_lock;

struct frame_table_entry {
    uint32_t *frame;
    struct thread *owner;
    struct list_elem elem;
};

void frame_table_init (void);
void frame_table_destroy (void);
#endif
