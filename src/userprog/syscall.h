#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
typedef int pid_t;

#include <stdbool.h>
#include "threads/synch.h"

void syscall_init (void);
void exit (int status);
void filesys_lock_acquire (void);
void filesys_lock_release (void);

#endif /* userprog/syscall.h */
