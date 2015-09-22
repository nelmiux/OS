#include "vm/frame.h"
#include <stdio.h>
#include "kernel/syscall.h"
#include "kernel/pagedir.h"
#include "kernel/malloc.h"
#include "kernel/synch.h"

static struct lock lock_frame;
static struct lock lock_evict;
static struct hash frames;
static struct list frames_list;
static struct list_elem *next;
static unsigned frame_hash (const struct hash_elem *, void *);
static bool frame_less (const struct hash_elem *, const struct hash_elem *, void *);
static struct frame *frame_find (void *);
static void frame_remove (struct frame *);
/* eviction helper */
static bool evict_helper (struct frame *);
static void evict (void);
/* clock algorith helper */
static void pointer_rem (struct frame *);
static struct frame *get_next (void);
static void move_next (void);

void frame_init ()
	{
		lock_init (&lock_frame);
		lock_init (&lock_evict);
		hash_init (&frames, frame_hash, frame_less, NULL);
		list_init (&frames_list);
	}

void *frame_new (enum palloc_flags flags)
	{
		void *address = palloc_get_page (flags);
		if (address != NULL) 
			{
		  	struct frame *f;
		  	f = (struct frame *) malloc (sizeof (struct frame));
		  	if (f == NULL)
		    	return false;
		  	list_init (&f->pages);
				lock_init (&f->lock_list);
				f->address = address;
				f->pin = true;
				lock_acquire (&lock_frame);
				list_push_back (&frames_list, &f->list_elem);
				hash_insert (&frames, &f->hash_elem);   
				lock_release (&lock_frame);
			}
		else
			{
				evict ();
				return frame_new (flags);
			}
		return address;
	}

void *frame_lookup (off_t bid)
	{
		void *address = NULL;

		struct hash_iterator iterator;
		lock_acquire (&lock_frame);
		hash_first (&iterator, &frames);
		while (hash_next (&iterator) && address == NULL)
		  {
		    struct frame *f = NULL;
		    f = hash_entry (hash_cur (&iterator), struct frame, hash_elem);
		    lock_acquire (&f->lock_list);    
		    struct list_elem *e = list_begin (&f->pages);
		    struct page *p = list_entry (e, struct page, fr_elem);
		    if (p->type == FILE && p->file_info.bid == bid)
		      {
		        address = f->address;
		        f->pin = true;
		      }

		    lock_release (&f->lock_list);
		  }
		lock_release (&lock_frame);
		
		return address;
	}

bool frame_page (void *fr, struct page *p)
	{
		struct frame *f = frame_find (fr);
		if (f == NULL)
		  return false;
		lock_acquire (&f->lock_list);
		list_push_back (&f->pages, &p->fr_elem);
		lock_release (&f->lock_list);
		return true;
	}

struct page *frame_page_get (void *fr, uint32_t *pagedir)
	{
		struct frame *f = frame_find (fr);
		struct list_elem *e;

		if (f == NULL)
		  return NULL;

		lock_acquire (&f->lock_list);
		for (e = list_begin (&f->pages); e != list_end (&f->pages);
		     e = list_next (e))
		  {
		    struct page *p = list_entry (e, struct page, fr_elem);
		    if (p->pagedir == pagedir)
		      {
		        lock_release (&f->lock_list);
		        return p;
		      }
		  }
		lock_release (&f->lock_list);
		
		return NULL; 
	}

static bool evict_helper (struct frame *f)
	{
		struct list_elem *e;
		for (e = list_begin (&f->pages); e != list_end (&f->pages);
		     e = list_next (e))
		  {
		    struct page *p = list_entry (e, struct page, fr_elem);
		    if (pagedir_is_accessed (p->pagedir, p->address) )
		      {
		        pagedir_set_accessed (p->pagedir, p->address, false);
		        return false;
		      }
		  }
		return true;
	}

static void evict ()
	{
		struct frame *v = NULL;
		lock_acquire (&lock_evict);
		lock_acquire (&lock_frame);
		while (v == NULL)
		  {
				struct frame *f = get_next ();
		    ASSERT (f != NULL);
		    if (f->pin == true || evict_helper(f) == false)
		      {
		        move_next ();
		    	  continue;  
		      }    
		    v = f;
		  }
		lock_release (&lock_frame);
		lock_release (&lock_evict);
		frame_free (v->address, NULL);
	}

static void pointer_rem (struct frame *v)
	{
		if (next == NULL || next == list_end (&frames_list))
			return;
		struct frame *f = list_entry (next, struct frame, list_elem);
		if (f == v)
			move_next ();
	}

static struct frame *get_next (void)
	{
		if (next == NULL || next == list_end (&frames_list))
			next = list_begin (&frames_list);
		if (next != NULL)
		  {
				struct frame *f = list_entry (next, struct frame, list_elem);
		    return f;
		  }  
		NOT_REACHED ();
	}

static void move_next (void)
	{
		if (next == NULL || next == list_end (&frames_list))
		  next = list_begin (&frames_list);
		else
		  next = list_next (next); 
	}

static void frame_remove (struct frame *f)
	{
		lock_acquire (&lock_frame);
		pointer_rem (f);
		hash_delete (&frames, &f->hash_elem);
		list_remove (&f->list_elem);
		free (f);
		lock_release (&lock_frame);
	}

void frame_free (void *address, uint32_t *pagedir)
	{
		lock_acquire (&lock_evict);
		struct frame *f = frame_find (address);  
		struct list_elem *e;
		
		if (f == NULL) 
		  {
		    lock_release (&lock_evict);
		    return; 
		  }
		if (pagedir == NULL)
		  {
		    lock_acquire (&f->lock_list);
		    while (!list_empty (&f->pages) )
		      {
		        e = list_begin (&f->pages);
		        struct page *p = list_entry (e, struct page, fr_elem);
		        list_remove (&p->fr_elem);
		        page_out (p, f->address);
		      }
		    lock_release (&f->lock_list);
		  }
		else
		  {
		    struct page *p = frame_page_get (address, pagedir); 
		    if (p != NULL)
		      {
		        lock_acquire (&f->lock_list);
		        list_remove (&p->fr_elem);
		        lock_release (&f->lock_list);
		        page_out (p, f->address);
		      }
		  }
		if (list_empty (&f->pages))
		{
		  frame_remove (f);
		  palloc_free_page (address);
		}
		lock_release (&lock_evict);
	}

void frame_pin (void *address)
	{
		struct frame *f = frame_find (address);
		if (f != NULL)
		  f->pin = true;
	}

void frame_unpin (void *address)
	{
		struct frame *f = frame_find (address);
		if (f != NULL)
		  f->pin = false;
	}

static unsigned frame_hash (const struct hash_elem *fr, void *aux UNUSED)
	{
		const struct frame *f = hash_entry (fr, struct frame, hash_elem);
		return hash_int ((unsigned)f->address);
	}


static struct frame *frame_find (void *address)
	{
		struct frame f;
		struct hash_elem *e;
		f.address = address;
		lock_acquire (&lock_frame);
		e = hash_find (&frames, &f.hash_elem);
		lock_release (&lock_frame);
		return e != NULL ? hash_entry (e, struct frame, hash_elem) : NULL;
	}

static bool frame_less (const struct hash_elem *af, const struct hash_elem *bf, void *aux UNUSED)
	{
		const struct frame *a = hash_entry (af, struct frame, hash_elem);
		const struct frame *b = hash_entry (bf, struct frame, hash_elem);
		return a->address < b->address;
	}
