#ifndef VM_MMAP_H
#define VM_MMAP_H

#include <hash.h>

typedef int mapid_t;

struct mfile
  {
		void *addr_init;
    void *addr_fin;
    mapid_t mapid;
    int fid;
    struct hash_elem hash_elem;
    struct list_elem thread_elem;
  };

void mfile_init (void);
void mfile_add (mapid_t, int, void *, void *);
bool mfile_rem (mapid_t);
struct mfile *mfile_lookup (mapid_t);


#endif /* vm/mmap.h */
