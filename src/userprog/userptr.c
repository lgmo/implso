#include "userprog/userptr.h"
#include <debug.h>
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"

char *
copy_in_string (const char *ustr)
{
  struct thread *cur = thread_current ();
  if (ustr == NULL)
    return NULL;

  char *kpage = palloc_get_page (0);
  if (kpage == NULL)
    return NULL;

  unsigned i = 0;
  while (i < PGSIZE)
    {
      if (!is_user_vaddr (ustr) || pagedir_get_page (cur->pagedir, (void *) ustr) == NULL)
        {
          palloc_free_page (kpage);
          return NULL;
        }

      kpage[i] = *ustr;
      if (kpage[i] == '\0')
        return kpage;
      ++ustr;
      ++i;
    }

  palloc_free_page (kpage);
  return NULL;
}
