#include "frame_table.h"
#include "list.h"
#include "threads/malloc.h"

struct list frame_table;
struct lock frame_table_lock;

void frame_table_init (void) {
    list_init(&frame_table);
    lock_init(&frame_table_lock);
}

void frame_table_destroy (void) {
  lock_acquire(&frame_table_lock);
  while (!list_empty(&frame_table)) {
    struct frame_table_entry *fte = list_entry(list_pop_front(&frame_table), struct frame_table_entry, elem);
    free(fte);
  }
  lock_release(&frame_table_lock);
}
