#ifndef SUP_PAGE_TABLE_H
#define SUP_PAGE_TABLE_H

#include "debug.h"
#include "hash.h"
#include "stdbool.h"
#include "filesys/off_t.h"
#include "vm/memory_mapping.h"
#include <stdint.h>

struct file;

enum page_source
{
    ZERO,
    SWAP,
    FILE_BACKED,
};

struct sup_page_table_entry
{
    uint32_t *user_vaddr;
    uint64_t access_time;
    bool is_loaded;
    enum page_source source;
    size_t swap_slot_index;
    struct file *file;
    off_t ofs;
    uint32_t read_bytes;
    uint32_t zero_bytes;
    bool writable;
    struct memory_mapping *mapping;
    struct hash_elem hash_elem;
};

unsigned sup_page_hash (const struct hash_elem *elem, void *aux UNUSED);

bool sup_page_less (const struct hash_elem *a, const struct hash_elem *b,
                    void *aux UNUSED);
struct sup_page_table_entry *sup_page_table_add (void *fault_addr);
struct sup_page_table_entry *sup_page_table_add_file (void *upage,
                                                      struct file *file,
                                                      off_t ofs,
                                                      uint32_t read_bytes,
                                                      uint32_t zero_bytes,
                                                      bool writable);
void sup_page_table_remove (struct sup_page_table_entry *spte);
void sup_page_table_destroy (void);
struct sup_page_table_entry *sup_page_table_find (void *fault_addr);
#endif
#endif
