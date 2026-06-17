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
    uint32_t *user_vaddr; //chave
    uint64_t access_time;
    bool dirty;
    bool accessed;
    bool is_loaded; // A página está fisicamente na RAM agora?
    enum page_source source;//De onde recuperar a página quando ela não está na RAM
    size_t swap_slot_index; // se source == SWAP, este é o index no bitmap
    struct file *file;
    off_t ofs; //offset no arquivo onde está a página
    uint32_t read_bytes; // quantos bytes estão no arquivo
    uint32_t zero_bytes; // quantos bytes de zeros tem no final
    bool writable; //se é writable
    struct memory_mapping *mapping; // se é uma memória compartilhada
    struct hash_elem hash_elem;
};

unsigned sup_page_hash (const struct hash_elem *elem, void *aux UNUSED);

bool sup_page_less (const struct hash_elem *a, const struct hash_elem *b,
                    void *aux UNUSED);
struct sup_page_table_entry *add_sup_page (void *fault_addr);
struct sup_page_table_entry *add_file_sup_page (void *upage, struct file *file,
                                                off_t ofs, uint32_t read_bytes,
                                                uint32_t zero_bytes,
                                                bool writable);
void remove_sup_page (struct sup_page_table_entry *spte);
void sup_page_table_destroy (void);
struct sup_page_table_entry *find_page (void *fault_addr);
#endif
