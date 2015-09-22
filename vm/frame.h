#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <hash.h>
#include "kernel/thread.h"
#include "kernel/palloc.h"
#include "vm/page.h"

struct frame 
  {
    void *address;                 
    bool pin;
		struct list pages;
    struct hash_elem hash_elem;
    struct lock lock_list;     
	  struct list_elem list_elem; 
  };

void frame_init (void);
void *frame_new (enum palloc_flags flags);
bool frame_page (void *, struct page *);
struct page *frame_page_get (void *, uint32_t *);
void *frame_lookup (off_t);
void frame_free (void *, uint32_t *);
void frame_pin (void *);
void frame_unpin (void *);



#endif /* vm/frame.h */
