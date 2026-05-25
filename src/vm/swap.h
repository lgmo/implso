#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "vm/frame_table.h"
#include "vm/sup_page_table.h"

void swap_init (void);
bool swap_read_slot (size_t index, uint8_t *dst);
void swap_free_slot (size_t index);
void *swap_evict_frame (void);
bool swap_reclaim_page (struct sup_page_table_entry *spte,
                        struct frame_table_entry *fte);
#endif /* vm/swap.h */
