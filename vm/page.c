#include "vm/page.h"
#include <stdio.h>
#include <string.h>
#include "kernel/pagedir.h"
#include "kernel/syscall.h"
#include "kernel/malloc.h"
#include "kernel/palloc.h"
#include "kernel/thread.h"
#include "kernel/synch.h"
#include "kernel/vaddr.h"
#include "kernel/thread.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "filesys/file.h"
#include "filesys/inode.h"

static int c = 0;
static void zero_in_page (uint8_t *kpage);
static void swap_in_page (uint8_t *kpage, struct page *p);
static bool file_in (uint8_t *kpage, struct page *p);
static void add_page (struct page *p);

/* in out lock*/
static struct lock lock_in;
static struct lock lock_out;
void page_init (void)
	{
		lock_init (&lock_in);
		lock_init (&lock_out);
	}

struct page* page_file (void *address, struct file *file, off_t ofs, size_t read_bytes, size_t zero_bytes, bool writable, off_t bid)
	{
		struct page *p = (struct page*) malloc (sizeof (struct page));
		if (p == NULL)
		  return NULL;		
		p->loaded = false;
		p->kpage = NULL;
		p->type = FILE;
		p->address = address;
		p->writable = writable;
		p->file_info.file = file;
		p->file_info.ofs = ofs;
		p->file_info.read_bytes = read_bytes;
		p->file_info.zero_bytes = zero_bytes;
		p->file_info.bid = bid;
		p->pagedir = thread_current ()->pagedir;
		add_page (p);
		return p; 
	}

struct page* page_zero (void *address, bool writable)
	{
		struct page *p = (struct page*) malloc (sizeof (struct page));
		if (p == NULL)
		  return NULL; 
		p->loaded = false;
		p->kpage = NULL;  
		p->type = ZERO;
		p->address = address;
		p->writable = writable;
		p->pagedir = thread_current ()->pagedir; 
		add_page (p);
		return p;
	}

bool page_in (struct page *p, bool pin)
	{
		lock_acquire (&lock_in);
		
		if (p->type == FILE && p->file_info.bid != -1)
		  p->kpage = frame_lookup (p->file_info.bid);
		
		if (p->kpage == NULL)
		  p->kpage = frame_new (PAL_USER);

		lock_release (&lock_in);
		frame_page (p->kpage, p);

		bool ok = true;
		if (p->type == FILE)
		  ok = file_in (p->kpage, p);
		else if (p->type == ZERO)
		  zero_in_page (p->kpage);
		else
		  swap_in_page (p->kpage, p);

		if (!ok)
		  {
		    frame_unpin (p->kpage);
		    return false;
		  }

		pagedir_clear_page (p->pagedir, p->address);
		if (!pagedir_set_page (p->pagedir, p->address, p->kpage, p->writable))
		  {
		    ASSERT (false);
		    frame_unpin (p->kpage);
		    return false;
		  }

		pagedir_set_dirty (p->pagedir, p->address, false);
		pagedir_set_accessed (p->pagedir, p->address, true);

		p->loaded = true;
		
		if (!pin)
		  frame_unpin (p->kpage);
		return true;
	}

void
page_out (struct page *p, void *kpage)
	{
		lock_acquire (&lock_out);
		if (p->type == FILE && pagedir_is_dirty (p->pagedir, p->address) && file_writable (p->file_info.file) == false)
		  {
		    /* page back to file */
		    frame_pin (kpage);
		    lock_acquire (&thread_filesys_lock);

		    file_seek (p->file_info.file, p->file_info.ofs);
		    file_write (p->file_info.file, kpage, p->file_info.read_bytes);
		    lock_release (&thread_filesys_lock);
		    frame_unpin (kpage);
		  }
		else if (p->type == SWAP || pagedir_is_dirty (p->pagedir, p->address))
		  {
		    /* save to swap. */
		    p->type = SWAP;
		    p->swap_info.idx = swap_save (kpage);
		  }
		lock_release (&lock_out);
		pagedir_clear_page (p->pagedir, p->address);
		pagedir_add_page (p->pagedir, p->address, (void *)p);
		p->loaded = false;
		p->kpage = NULL;
	}

static bool file_in (uint8_t *kpage, struct page *p)
	{
		/* reading the page from file. */

		lock_acquire (&thread_filesys_lock);
		file_seek (p->file_info.file, p->file_info.ofs);
		size_t temp = file_read (p->file_info.file, kpage, 
		                        p->file_info.read_bytes);
		lock_release (&thread_filesys_lock);
		 
		if (temp != p->file_info.read_bytes)
		  {
		    frame_free (kpage, p->pagedir);
		    return false;
		  }
		
		/* end of the page to zeroes. */
		memset (kpage + p->file_info.read_bytes, 0, p->file_info.zero_bytes);
		return true;
	}

static void zero_in_page (uint8_t *kpage)
	{
		memset (kpage, 0, PGSIZE);
	}

static void
swap_in_page (uint8_t *kpage, struct page *p)
	{
		swap_in (p->swap_info.idx, kpage);
		swap_free (p->swap_info.idx);
	}

/* saves a pointer to page in the page table */
static void add_page (struct page *p)
	{
		pagedir_add_page (p->pagedir, p->address, (void *)p);
	}

struct page *page_lookup (void *address)
	{
		uint32_t *pagedir = thread_current ()->pagedir;
		struct page *p = NULL;
		p = (struct page *) pagedir_find_page (pagedir, (const void *)address);
		return p;
	}

/* free page and swap */
void page_free (struct page *p)
	{
		if (p == NULL)
		  return;
		
		if (p->type == SWAP && p->loaded == false)
		  swap_free (p->swap_info.idx);

		/* clear mapping */
		pagedir_clear_page (p->pagedir, p->address);
		free (p);
		--c;
	}

void page_pin (struct page *p)
	{
		if (p->kpage != NULL)
		  return;
		frame_pin (p->kpage);
	}

void page_unpin (struct page *p)
	{
		if (p->kpage != NULL)
		  return;
		frame_unpin (p->kpage);
	}

bool need_grow (const void *esp, void *address)
	{
		return (uint32_t)address > 0 && address >= (esp - 32) &&
		   (PHYS_BASE - pg_round_down (address)) <= (1<<23);
	}

struct page *stack_grow (void *user_vaddr, bool pin)
	{
		struct page *p = page_zero (user_vaddr, true);
		if (!page_in (p, pin))
		  return NULL;
		return p;
	}


