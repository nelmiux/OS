#include "vm/swap.h"
#include <stdio.h>
#include <string.h>
#include <bitmap.h>
#include "devices/block.h"
#include "kernel/synch.h"
#include "kernel/palloc.h"
#include "kernel/vaddr.h"

#define BPP (PGSIZE / BLOCK_SECTOR_SIZE)

static unsigned ssize;
static struct block *sb;
static struct lock lock_swap;
static struct bitmap *sm;

void swap_init ()
	{
		sb = block_get_role (BLOCK_SWAP);
		lock_init (&lock_swap);  
		ssize = block_size (sb); 
		sm = bitmap_create (ssize);
	}

void swap_in (size_t idx, void *address)
	{
		lock_acquire (&lock_swap);
		size_t ofs; 
		for (ofs = 0; ofs < BPP; ++ofs)
		  {
		    ASSERT (idx < ssize);
		    ASSERT ( bitmap_test (sm, idx) );

		    block_read (sb, idx, address + ofs * BPP);
		    ++idx;
		  }
		lock_release (&lock_swap); 
	}

size_t swap_save (void *address)
	{
		lock_acquire (&lock_swap);
		size_t idx = bitmap_scan_and_flip (sm, 0, BPP, false);

		ASSERT (idx != BITMAP_ERROR);

		size_t ofs, ix = idx;
		for (ofs = 0; ofs < BPP; ++ofs)
		  {
		    ASSERT (idx < ssize);
		    ASSERT (bitmap_test (sm, ix));

		    block_write (sb, ix, address + ofs * BPP);
		    ++ix;
		  }
		lock_release (&lock_swap);

		return idx;
	} 

void swap_free (size_t idx)
	{
		lock_acquire (&lock_swap);
		size_t ofs;
		for (ofs = 0; ofs < BPP; ++ofs)
		  {
		    ASSERT (idx < ssize);
		    ASSERT ( bitmap_test (sm, idx) );

		    bitmap_reset (sm, idx);
		    ++idx;
		  }
		lock_release (&lock_swap);
	}
