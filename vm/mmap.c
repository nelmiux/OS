#include "vm/mmap.h"
#include <hash.h>
#include <list.h>
#include "kernel/malloc.h"
#include "kernel/thread.h"
#include "kernel/synch.h"

static struct lock lock_mfile;
static struct hash mfiles;
static bool mfile_less (const struct hash_elem *, const struct hash_elem *, void *);
static unsigned mfile_hash (const struct hash_elem *, void *);

void mfile_init (void)
	{
		lock_init (&lock_mfile);
		hash_init (&mfiles, mfile_hash, mfile_less, NULL);
	}

static unsigned mfile_hash (const struct hash_elem *mfi, void *aux UNUSED)
	{
		const struct mfile *mf = hash_entry (mfi, struct mfile, hash_elem);
		return hash_int ((unsigned)mf->mapid);
	}

struct mfile *mfile_lookup (mapid_t mapid)
	{
		struct mfile mf;
		struct hash_elem *e;

		mf.mapid = mapid;
		e = hash_find (&mfiles, &mf.hash_elem);
		return e != NULL ? hash_entry (e, struct mfile, hash_elem) : NULL;
	}

bool mfile_rem (mapid_t mapid)
	{
		struct mfile *mf = mfile_lookup (mapid);
		if (mf == NULL)
		  return false;
		lock_acquire (&lock_mfile);
		hash_delete (&mfiles, &mf->hash_elem);
		list_remove (&mf->thread_elem);
		free (mf);
		lock_release (&lock_mfile);
		return true; 
	}

void mfile_add (mapid_t mapid, int fid, void *addr_init, void *addr_fin)
	{
		struct mfile *mf = (struct mfile *) malloc (sizeof (struct mfile));
		mf->fid = fid;
		mf->mapid = mapid;
		mf->addr_init = addr_init;
		mf->addr_fin = addr_fin;
		lock_acquire (&lock_mfile);
		list_push_back (&thread_current ()->mfiles, &mf->thread_elem);
		hash_insert (&mfiles, &mf->hash_elem);
		lock_release (&lock_mfile);
	}

static bool mfile_less (const struct hash_elem *am, const struct hash_elem *bm, void *aux UNUSED)
	{
		const struct mfile *a = hash_entry (am, struct mfile, hash_elem);
		const struct mfile *b = hash_entry (bm, struct mfile, hash_elem);

		return a->mapid < b->mapid;
	}

