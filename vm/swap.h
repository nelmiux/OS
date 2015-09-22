#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stdbool.h>
#include <stddef.h>

void swap_init (void);
size_t swap_save (void *);
void swap_free (size_t);
void swap_in (size_t, void *);

#endif /* vm/swap.h */
