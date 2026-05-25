#include "vm.h"
#include "frame_table.h"
#include "swap.h"

void
vm_init (void) {
  frame_table_init ();
  swap_init ();
}
