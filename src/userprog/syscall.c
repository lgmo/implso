#include "userprog/syscall.h"
#include <debug.h>
#include <stdio.h>
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

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
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
            f->eax = filesys_create(fn, initial_size);
            palloc_free_page(fn);
            break;
        }
        case SYS_OPEN:
        {
            validate_pointers(f->esp, 1);
            char *fn = copy_in_string(*(const char **)(f->esp+4));
            if (fn == NULL)
                exit(-1);
            struct file *file = filesys_open(fn);
            f->eax = -1;
            if (file != NULL) {
                struct fd_entry *entry = fd_alloc(file);
                if (entry != NULL)
                    f->eax = entry->fd;
                else
                    file_close(file);
            }
            palloc_free_page(fn);
            break;
        }
        case SYS_CLOSE:
        {
            validate_pointers(f->esp, 1);
            struct fd_entry *entry = fd_lookup(*(int*)(f->esp+4));
            if (entry != NULL) {
                fd_close(entry);
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
            f->eax = entry != NULL ? file_length(entry->file) : -1;
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
                file_seek(entry->file, position);
            break;
        }
        case SYS_TELL:
        {
            validate_pointers(f->esp, 1);
            struct fd_entry *entry = fd_lookup(*(int*)(f->esp+4));
            f->eax = entry != NULL ? file_tell(entry->file) : -1;
            break;
        }
        default:
            break;
  }
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
        if (ptr == NULL || !is_user_vaddr (ptr) ||
            pagedir_get_page (cur->pagedir, (void *) ptr) == NULL)
          return false;
    }
    return true;
}

static int
read_from_fd (int fd, void *buffer, unsigned size)
{
    if (size == 0)
        return 0;
    if (!check_user_buffer (buffer, size))
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
    unsigned total = 0;
    while (total < size) {
        int chunk = file_read (entry->file, (char *)buffer + total, size - total);
        if (chunk <= 0)
            return total > 0 ? total : chunk;
        total += chunk;
    }
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
    unsigned total = 0;
    while (total < size) {
        int chunk = file_write (entry->file, (const char *)buffer + total, size - total);
        if (chunk <= 0)
            return total > 0 ? total : chunk;
        total += chunk;
    }
    return total;
}
