# Project 2 Report

## Overview
This solution finishes the Project 2 goals by implementing a user-level syscall interface, per-process file handling, clean process exit/wait semantics, and correct stack layout when starting programs. Each added struct and function addresses a specific missing component from the base Pintos kernel, allowing user programs to execute, call syscalls, and coordinate without corrupting the kernel state.

## Data structures and thread state
- `struct exit_state` lives on each parent to track a child's lifecycle. It bundles the child's `tid`, the semaphores used by `process_wait` and the loader, the final exit status, and booleans that indicate whether the child has finished loading/exiting or whether the parent is already waiting. Recording this state per child avoids races between `exec`, `wait`, and `exit`, because the parent blocks on the child’s semaphores instead of busy-waiting or re-scanning the thread list.
- `struct fd_entry` pairs an integer descriptor with an open `struct file *`. The descriptor is assigned sequentially from `thread->next_fd`. Each thread keeps a `fd_table` list of its own `fd_entry`s, so file descriptors are isolated per-process and can be closed without touching global locks.
- `struct thread` now stores `children_exit_state` (for a parent’s list of child states), a pointer to the thread’s own `exit_state`, and the per-thread `fd_table`/`next_fd`. These additions enable syscall/exit handling code to look up the relevant data without traversing global structures and keep per-process resources tied to the owning thread.

## Process creation, exit, and waiting
- `process_execute` allocates an `exit_state` before creating the child. It pushes that `exit_state` onto the parent’s `children_exit_state` list so `wait` can later find it, then blocks on `child_exit_state->load_wait`; if loading or thread creation fails, it removes/frees the state and returns `TID_ERROR`. This sequence guarantees the parent does not continue until the child has attempted to load, preventing a race where the parent returns success while the child still aborts.
- `start_process` receives both the command line and pointer to the child’s `exit_state`. It sets `thread_current()->exit_state` so `syscall_exit()` and exception handlers can find it, calls `load`, signals `exit_state->load_wait` with the result, and exits immediately if the load fails. This ensures the parent unblocks with accurate success/failure information and also avoids leaking the state when the child never reaches user mode.
- `process_wait` scans `thread_current()->children_exit_state` under the assumption that only the parent manipulates the list. Once it finds the matching `tid`, it marks `waiting = true`, removes the entry, and blocks on `exit_wait` if the child has not exited yet. The semaphore guarantees the parent is woken only once the child has completed, preventing busy waiting and ensuring `exit_status` is read after the child has fully torn down.
- `process_exit` closes the executable file (`thread->exe`) by re-allowing writes and shutting it down, drains the per-thread `fd_table`, and frees any remaining descriptors. Because each thread uses its own list, there are no shared locks and thus no race between processes closing each other’s descriptors.
- `exit(int status)` (called via `SYS_EXIT`) stores the status in the current thread’s `exit_state`, marks `exited = true`, and `sema_up(&exit_wait)` if the parent is already waiting. This handshake combined with `process_wait`’s semaphore prevents races where the parent might miss the child’s exit unless the parent was already blocked.

## Stack setup and argument passing
- `setup_stack` now records the command-line string and argument pointers while building the stack. It copies the command-line bytes from the end toward `PHYS_BASE`, inserts null terminators between tokens, then pushes the argv array, the pointer to argv, argc, and a fake return address in the correct order. By calculating each token’s start (after copying) before inserting the pointer into the helper array, it avoids dangling pointers that previously pointed into regions overwritten by later bytes. This ensures user programs receive valid `argc`/`argv` values regardless of spaces or extra whitespace.

## Syscall interface and helpers
- `syscall_init` registers `syscall_handler` at interrupt vector 0x30. The handler first validates the syscall number and `esp` pointer using `validate_pointers`, which ensures the kernel never dereferences an invalid user pointer; if validation fails, it calls `exit(-1)`.
- For each syscall (halt, exit, exec, wait, create, open, close, read, filesize, write, seek, tell), the handler copies necessary strings using `copy_in_string`, validates buffers with `check_user_buffer`, and dispatches to helpers that encapsulate the logic.
- `fd_alloc`, `fd_lookup`, and `fd_close` manage the per-thread descriptor table. No global file-locking is needed because each descriptor list is protected implicitly by the fact that only the owning thread touches it; the code still calls `file_close` to release the inode.
- `read_from_fd` and `write_to_fd` ensure buffer validation, support standard input/output (fd 0 and 1), and loop to read/write until either the request is satisfied or the file returns zero. They return the total bytes transferred so user programs can rely on POSIX semantics.
- `validate_pointers` and `check_user_buffer` walk each byte of the supplied buffer, using `is_user_vaddr` and `pagedir_get_page` to prevent kernel accesses to invalid or unmapped addresses. The code uses the current thread’s `pagedir`, so it fails fast if the buffer crosses into kernel space or unmapped pages.

## User pointer helpers and exception handling
- `copy_in_string` (in `userprog/userptr.c`) copies strings from user space to kernel space one page at a time, checking `is_user_vaddr` and `pagedir_get_page`. It now includes `userprog/pagedir.h` to avoid implicit declarations, ensuring the loader/syscall code compiles cleanly.
- `kill()` in `userprog/exception.c` now reports the exit status to the child’s `exit_state` (if present) before calling `exit(-1)`. This provides deterministic exit reporting for exceptions and avoids leaving a parent waiting forever when the child dies due to a fault.

## Thread scheduler updates
- The scheduler now initializes `children_exit_state`, `fd_table`, and `next_fd` inside `init_thread`, ensuring every thread starts with clean structures; new lists avoid stale data from reused pages.
- In `thread_create`, after inserting the new thread into the ready list, the code yields if the new thread has higher priority (and only when not in interrupt context) so the scheduler can immediately run the child thread if necessary. `thread_cmp` and `thread_donor_cmp` are defined to compare priorities for both ready and donor lists.
- `sema_up` now checks `intr_context()` before calling `thread_yield`, preventing preemption while in interrupt context and ensuring higher-priority threads still run promptly.

## Synchronization/race condition handling
- The parent/child handshake uses two semaphores per child (`load_wait` and `exit_wait`). The loader signals `load_wait` after `load()` returns so the parent knows whether the child was able to load; `exit_wait` is signaled by `exit()`/`kill()` when the child finishes. These semaphores prevent races that would otherwise arise from the parent calling `wait()` before or while the child is still transitioning out of `process_execute`.
- Per-thread lists (`children_exit_state`, `fd_table`) avoid the need for global locking. Each thread manipulates its own lists only from kernel mode, so there is no deadlock between threads trying to close each other’s descriptors.
- `fd_alloc`/`fd_close` are only called by the owning thread, and `thread->fd_table` is drained before the thread exits, ensuring there are no dangling files left behind.

## Summary
Every added function, struct, and field ties directly to one of the missing behaviors from the Pintos base kernel. Per-child tracking, syscall validation, stack layout, and file descriptor management now behave like a simplified UNIX process model. The synchronization primitives already provided by Pintos (semaphores, lists, and per-thread structures) are reused so no new global locks are needed, which keeps the solution lightweight and deterministic.
