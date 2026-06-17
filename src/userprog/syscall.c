#include "userprog/syscall.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/malloc.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "userprog/userptr.h"
#include "vm/memory_mapping.h"
#include "vm/sup_page_table.h"
#include "filesys/directory.h"
#include "filesys/inode.h"

static void syscall_handler (struct intr_frame *);
static struct lock filesys_lock;

void
syscall_init (void) 
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

void
filesys_lock_acquire (void)
{
    lock_acquire (&filesys_lock);
}

void
filesys_lock_release (void)
{
    lock_release (&filesys_lock);
}

void
halt (void) {
    shutdown_power_off();
}

void
exit (int status)
{
    struct thread *cur = thread_current();
    printf("%s: exit(%d)\n", cur->name, status);
    if (cur->exit_state != NULL) {
        cur->exit_state->exit_status = status;
        cur->exit_state->exited = true;
        if (cur->exit_state->waiting)
            sema_up(&cur->exit_state->exit_wait);
    }
    thread_exit();
}

pid_t
exec(const char *cmd_line) {
    return process_execute(cmd_line);
}

int
wait(pid_t pid) {
    return process_wait(pid);
}

static bool check_user_buffer (const void *buffer, unsigned size);
static bool check_user_buffer_writable (const void *buffer, unsigned size);

void validate_pointers(void *base_ptr, unsigned int count) {
    if (count == 0)
        return;
    if (!check_user_buffer((char *) base_ptr + 4, count * 4))
        exit(-1);
}

static struct fd_entry *fd_alloc (struct file *);
static struct fd_entry *fd_lookup (int fd);
static void fd_close (struct fd_entry *);
static int read_from_fd (int fd, void *buffer, unsigned size);
static int write_to_fd (int fd, const void *buffer, unsigned size);
static mapid_t mmap_file (int fd, void *addr);
static void munmap_file (mapid_t mapid);

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
    if (!check_user_buffer (f->esp, sizeof (int)))
        exit (-1);
    switch (*(int*)(f->esp)) {
        case SYS_HALT:
            halt();
            break;
        case SYS_EXIT:
            validate_pointers(f->esp, 1);
            exit(*(int*)(f->esp+4));
            break;
        case SYS_EXEC:
            validate_pointers(f->esp, 1);
            char *fn = copy_in_string(*(const char **)(f->esp+4));
            if (fn == NULL)
                exit(-1);
            f->eax = exec(fn);
            palloc_free_page(fn);
            break;
        case SYS_WAIT:
            validate_pointers(f->esp, 1);
            f->eax = wait(*(unsigned int*)(f->esp+4)); 
            break;
        case SYS_CREATE:
        {
            validate_pointers(f->esp, 2);
            char *fn = copy_in_string(*(const char **)(f->esp+4));
            if (fn == NULL)
                exit(-1);
            int initial_size = *(int*)(f->esp+8);
            filesys_lock_acquire ();
            f->eax = filesys_create(fn, initial_size);
            filesys_lock_release ();
            palloc_free_page(fn);
            break;
        }
        case SYS_REMOVE:
        {
            validate_pointers(f->esp, 1);
            char *fn = copy_in_string(*(const char **)(f->esp+4));
            if (fn == NULL)
                exit(-1);
            filesys_lock_acquire ();
            f->eax = filesys_remove(fn);
            filesys_lock_release ();
            palloc_free_page(fn);
            break;
        }
        case SYS_OPEN:
        {
            validate_pointers(f->esp, 1);
            char *fn = copy_in_string(*(const char **)(f->esp+4));
            if (fn == NULL)
                exit(-1);
            filesys_lock_acquire ();
            struct inode *inode = filesys_open_inode (fn);
            f->eax = -1;
            if (inode != NULL) {
                if (inode_is_dir (inode)) {
                    struct dir *dir = dir_open (inode);
                    if (dir != NULL) {
                        struct fd_entry *entry = fd_alloc (NULL);
                        if (entry != NULL) {
                            entry->dir = dir;
                            f->eax = entry->fd;
                        } else
                            dir_close (dir);
                    }
                } else {
                    struct file *file = file_open (inode);
                    if (file != NULL) {
                        struct fd_entry *entry = fd_alloc (file);
                        if (entry != NULL)
                            f->eax = entry->fd;
                        else
                            file_close (file);
                    }
                }
            }
            filesys_lock_release ();
            palloc_free_page(fn);
            break;
        }
        case SYS_CLOSE:
        {
            validate_pointers(f->esp, 1);
            struct fd_entry *entry = fd_lookup(*(int*)(f->esp+4));
            if (entry != NULL) {
                filesys_lock_acquire ();
                fd_close(entry);
                filesys_lock_release ();
                f->eax = 0;
            } else {
                f->eax = -1;
            }
            break;
        }
        case SYS_READ:
        {
            validate_pointers(f->esp, 3);
            f->eax = read_from_fd(*(int*)(f->esp+4), *(void**)(f->esp+8), *(unsigned int*)(f->esp+12));
            break;
        }
        case SYS_FILESIZE:
        {
            validate_pointers(f->esp, 1);
            struct fd_entry *entry = fd_lookup(*(int*)(f->esp+4));
            if (entry != NULL)
                {
                    filesys_lock_acquire ();
                    f->eax = file_length(entry->file);
                    filesys_lock_release ();
                }
            else
                f->eax = -1;
            break;
        }
        case SYS_WRITE:
            validate_pointers(f->esp, 3);
            f->eax = write_to_fd(*(int*)(f->esp+4), *(const void**)(f->esp+8), *(unsigned int*)(f->esp+12));
            break;
        case SYS_SEEK:
        {
            validate_pointers(f->esp, 2);
            int fd = *(int*)(f->esp+4);
            unsigned position = *(unsigned*)(f->esp+8);
            struct fd_entry *entry = fd_lookup(fd);
            if (entry != NULL)
                {
                    filesys_lock_acquire ();
                    file_seek(entry->file, position);
                    filesys_lock_release ();
                }
            break;
        }
        case SYS_TELL:
        {
            validate_pointers(f->esp, 1);
            struct fd_entry *entry = fd_lookup(*(int*)(f->esp+4));
            if (entry != NULL)
                {
                    filesys_lock_acquire ();
                    f->eax = file_tell(entry->file);
                    filesys_lock_release ();
                }
            else
                f->eax = -1;
            break;
        }
        case SYS_MMAP:
        {
            validate_pointers (f->esp, 2);
            f->eax = mmap_file (*(int *)(f->esp + 4), *(void **)(f->esp + 8));
            break;
        }
        case SYS_MUNMAP:
        {
            validate_pointers (f->esp, 1);
            munmap_file (*(int *)(f->esp + 4));
            break;
        }
        case SYS_CHDIR:
        {
            validate_pointers(f->esp, 1);
            char *fn = copy_in_string(*(const char **)(f->esp+4));
            if (fn == NULL)
                exit(-1);
            filesys_lock_acquire ();
            f->eax = filesys_chdir (fn);
            filesys_lock_release ();
            palloc_free_page(fn);
            break;
        }
        case SYS_MKDIR:
        {
            validate_pointers(f->esp, 1);
            char *fn = copy_in_string(*(const char **)(f->esp+4));
            if (fn == NULL)
                exit(-1);
            filesys_lock_acquire ();
            f->eax = filesys_mkdir (fn);
            filesys_lock_release ();
            palloc_free_page(fn);
            break;
        }
        case SYS_READDIR:
        {
            validate_pointers(f->esp, 2);
            int fd_val = *(int*)(f->esp+4);
            char *name_buf = *(char **)(f->esp+8);
            if (!check_user_buffer (name_buf, NAME_MAX + 1))
                exit(-1);
            struct fd_entry *entry = fd_lookup(fd_val);
            if (entry == NULL || entry->dir == NULL)
                f->eax = false;
            else {
                filesys_lock_acquire ();
                f->eax = dir_readdir (entry->dir, name_buf);
                filesys_lock_release ();
            }
            break;
        }
        case SYS_ISDIR:
        {
            validate_pointers(f->esp, 1);
            struct fd_entry *entry = fd_lookup(*(int*)(f->esp+4));
            f->eax = (entry != NULL && entry->dir != NULL);
            break;
        }
        case SYS_INUMBER:
        {
            validate_pointers(f->esp, 1);
            struct fd_entry *entry = fd_lookup(*(int*)(f->esp+4));
            if (entry == NULL)
                f->eax = -1;
            else if (entry->dir != NULL) {
                filesys_lock_acquire ();
                f->eax = (int) inode_get_inumber (dir_get_inode (entry->dir));
                filesys_lock_release ();
            } else {
                filesys_lock_acquire ();
                f->eax = (int) inode_get_inumber (file_get_inode (entry->file));
                filesys_lock_release ();
            }
            break;
        }
        default:
            break;
  }
}

static void
munmap_file (mapid_t mapid)
{
    struct thread *cur = thread_current ();
    struct memory_mapping *mapping = memory_mapping_find (&cur->mmap_table, mapid);

    if (mapping == NULL)
        return;

    memory_mapping_table_remove (mapping);
    memory_mapping_unmap (cur, mapping);
}

static mapid_t
mmap_file (int fd, void *addr)
{
    struct thread *cur = thread_current ();
    struct fd_entry *entry;
    struct file *mapping_file;
    struct memory_mapping *mapping;
    off_t length;
    uint8_t *upage;
    off_t ofs;
    bool ok = true;

    if (fd <= 1 || addr == NULL || pg_ofs (addr) != 0)
        return -1;

    entry = fd_lookup (fd);
    if (entry == NULL)
        return -1;

    filesys_lock_acquire ();
    length = file_length (entry->file);
    filesys_lock_release ();
    if (length <= 0)
        return -1;

    filesys_lock_acquire ();
    mapping_file = file_reopen (entry->file);
    filesys_lock_release ();
    if (mapping_file == NULL)
        return -1;

    mapping = memory_mapping_create (mapping_file, addr, (size_t) length,
                                     cur->next_mapid++);
    if (mapping == NULL)
        {
            filesys_lock_acquire ();
            file_close (mapping_file);
            filesys_lock_release ();
            return -1;
        }

    upage = addr;
    ofs = 0;
    while (ofs < length)
        {
            uint32_t page_read_bytes = (length - ofs) < PGSIZE
                                           ? (uint32_t)(length - ofs)
                                           : PGSIZE;
            uint32_t page_zero_bytes = PGSIZE - page_read_bytes;
            struct sup_page_table_entry *spte;

            if (sup_page_table_find (upage) != NULL)
                {
                    ok = false;
                    break;
                }

            spte = sup_page_table_add_file (upage, mapping_file, ofs,
                                            page_read_bytes, page_zero_bytes,
                                            true);
            if (spte == NULL)
                {
                    ok = false;
                    break;
                }
            spte->mapping = mapping;

            ofs += page_read_bytes;
            upage += PGSIZE;
        }

    if (!ok)
        {
            upage = addr;
            ofs = 0;
            while (ofs < length)
                {
                    struct sup_page_table_entry *spte = sup_page_table_find (upage);
                    uint32_t page_read_bytes = (length - ofs) < PGSIZE
                                                   ? (uint32_t)(length - ofs)
                                                   : PGSIZE;
                    if (spte != NULL && spte->mapping == mapping)
                        sup_page_table_remove (spte);
                    ofs += page_read_bytes;
                    upage += PGSIZE;
                }

            filesys_lock_acquire ();
            file_close (mapping_file);
            filesys_lock_release ();
            memory_mapping_destroy (mapping);
            return -1;
        }

    memory_mapping_table_push (&cur->mmap_table, mapping);
    return mapping->mapid;
}

static struct fd_entry *
fd_alloc (struct file *file)
{
    struct thread *cur = thread_current ();
    struct fd_entry *entry = malloc (sizeof *entry);
    if (entry == NULL)
        return NULL;
    entry->fd = cur->next_fd++;
    entry->file = file;
    entry->dir = NULL;
    list_push_back (&cur->fd_table, &entry->elem);
    return entry;
}

static struct fd_entry *
fd_lookup (int fd)
{
    struct thread *cur = thread_current ();
    struct list_elem *e;
    for (e = list_begin (&cur->fd_table); e != list_end (&cur->fd_table); e = list_next (e))
      {
        struct fd_entry *entry = list_entry (e, struct fd_entry, elem);
        if (entry->fd == fd)
          return entry;
      }
    return NULL;
}

static void
fd_close (struct fd_entry *entry)
{
    if (entry == NULL)
        return;
    list_remove (&entry->elem);
    if (entry->dir != NULL)
        dir_close (entry->dir);
    else if (entry->file != NULL)
        file_close (entry->file);
    free (entry);
}

static bool
check_user_buffer (const void *buffer, unsigned size)
{
    if (size == 0)
        return true;

    struct thread *cur = thread_current ();
    const char *ptr = buffer;
    unsigned i;
    for (i = 0; i < size; ++i, ++ptr) {
        if (ptr == NULL || !is_user_vaddr (ptr))
          return false;
    }
    return true;
}

static bool
check_user_buffer_writable (const void *buffer, unsigned size)
{
    if (size == 0)
        return true;

    struct thread *cur = thread_current ();
    const char *ptr = buffer;
    unsigned i;
    for (i = 0; i < size; ++i, ++ptr) {
        if (ptr == NULL || !is_user_vaddr (ptr))
          return false;

        void *kaddr = pagedir_get_page (cur->pagedir, (void *) ptr);
        if (kaddr != NULL && !pagedir_is_writable (cur->pagedir, (void *) ptr))
            return false;
    }
    return true;
}

static int
read_from_fd (int fd, void *buffer, unsigned size)
{
    if (size == 0)
        return 0;
    if (!check_user_buffer_writable (buffer, size))
        exit(-1);
    if (fd == 0) {
        unsigned count;
        for (count = 0; count < size; ++count)
            ((char *) buffer)[count] = input_getc();
        return count;
    }
    struct fd_entry *entry = fd_lookup (fd);
    if (entry == NULL)
        return -1;
    if (entry->dir != NULL)
        return -1;
    uint8_t *bounce = malloc (PGSIZE);
    if (bounce == NULL)
        exit (-1);
    unsigned total = 0;
    while (total < size) {
        unsigned chunk_size = size - total;
        if (chunk_size > PGSIZE)
            chunk_size = PGSIZE;
        filesys_lock_acquire ();
        int chunk = file_read (entry->file, bounce, chunk_size);
        filesys_lock_release ();
        if (chunk <= 0)
            break;
        memcpy ((uint8_t *) buffer + total, bounce, chunk);
        total += chunk;
        if ((unsigned) chunk < chunk_size)
            break;
    }
    free (bounce);
    if (total == 0)
        return 0;
    return total;
}

static int
write_to_fd (int fd, const void *buffer, unsigned size)
{
    if (size == 0)
        return 0;
    if (!check_user_buffer (buffer, size))
        exit(-1);
    if (fd == 1) {
        putbuf (buffer, size);
        return size;
    }
    struct fd_entry *entry = fd_lookup (fd);
    if (entry == NULL)
        return -1;
    if (entry->dir != NULL)
        return -1;
    uint8_t *bounce = malloc (PGSIZE);
    if (bounce == NULL)
        exit (-1);
    unsigned total = 0;
    while (total < size) {
        unsigned chunk_size = size - total;
        if (chunk_size > PGSIZE)
            chunk_size = PGSIZE;
        memcpy (bounce, (const uint8_t *) buffer + total, chunk_size);
        filesys_lock_acquire ();
        int chunk = file_write (entry->file, bounce, chunk_size);
        filesys_lock_release ();
        if (chunk <= 0)
            break;
        total += chunk;
        if ((unsigned) chunk < chunk_size)
            break;
    }
    free (bounce);
    return total;
}
