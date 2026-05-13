#include "vm.h"
#include "frame_table.h"

void
vm_init (void) {
  frame_table_init ();
}

void
vm_destroy (void) {
  frame_table_destroy ();
}
