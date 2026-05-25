#ifndef VM_MEMORY_MAPPING_H
#define VM_MEMORY_MAPPING_H

#include "filesys/file.h"
#include "list.h"
#include <stdbool.h>
#include <stddef.h>

typedef int mapid_t;

struct memory_mapping
{
  struct file *file;
  void *user_vaddr;
  size_t length;
  mapid_t mapid;
  struct list_elem mapping_elem;
};

struct thread;

void memory_mapping_table_init (struct list *mmap_table);
struct memory_mapping *memory_mapping_create (struct file *file,
                                              void *user_vaddr,
                                              size_t length,
                                              mapid_t mapid);
void memory_mapping_destroy (struct memory_mapping *mapping);
void memory_mapping_table_push (struct list *mmap_table,
                                struct memory_mapping *mapping);
struct memory_mapping *memory_mapping_find (struct list *mmap_table,
                                            mapid_t mapid);
void memory_mapping_table_remove (struct memory_mapping *mapping);
void memory_mapping_unmap (struct thread *t, struct memory_mapping *mapping);
void memory_mapping_table_unmap_all (struct thread *t, struct list *mmap_table);

#endif /* vm/memory_mapping.h */
