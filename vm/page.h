#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <list.h>
#include <stdbool.h>
#include <stddef.h>
#include "filesys/file.h"
#include "filesys/inode.h"

enum page_t { SWAP, FILE, ZERO };

struct page
{
  enum page_t type;      					/* Page types */
  bool writable;         					/* writable? */
	bool loaded;
	uint32_t *pagedir;
  struct list_elem fr_elem;
  void *address;
  void *kpage;
  struct        
  {
    struct file *file;
    off_t ofs;
		off_t bid;
    size_t zero_bytes;
    size_t read_bytes;

  } file_info;

  struct
  {
    size_t idx;
  } swap_info;
};

void page_init (void);

struct page *page_zero (void *, bool);
struct page *page_file (void *, struct file *, off_t, 												uint32_t, uint32_t, bool, off_t);

void page_pin (struct page *);
void page_unpin (struct page *);

struct page *page_lookup (void *);
void page_free (struct page *);

bool page_in (struct page *, bool);
void page_out (struct page *, void *);

bool need_grow (const void *, void *);
struct page *stack_grow (void *, bool);

#endif /* vm/page.h */
