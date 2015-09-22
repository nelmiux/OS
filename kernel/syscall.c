#include "kernel/syscall.h"
#include "kernel/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "devices/shutdown.h"
#include "devices/input.h"
#include "kernel/interrupt.h"
#include "kernel/thread.h"
#include "kernel/synch.h"
#include "kernel/vaddr.h"
#include "kernel/palloc.h"
#include "kernel/malloc.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "vm/page.h"
#include "vm/mmap.h"
#include "vm/frame.h"
#include <inttypes.h>
#include <list.h>

/* Process identifier. */
typedef int pid_t;
/* File identifier. */
typedef int fid_t;

static void syscall_handler (struct intr_frame *);

static void      halt (void);
static void      exit (int status);
static pid_t     exec (const char *cmdline);
static int       wait (pid_t pid);
static bool      create (const char *file, unsigned initial_size);
static bool      remove (const char *file);
static int       open (const char *file);
static int       filesize (int fd);
static int       read (int fd, void *buffer, unsigned size);
static int       write (int fd, const void *buffer, unsigned size);
static void      seek (int fd, unsigned position);
static unsigned  tell (int fd);
static void      close (int fd);
static mapid_t   mmap (int fd, void *addr);
static void      munmap (mapid_t mapid);

static struct ufile *file_by_fid (fid_t);
static fid_t allocate_fid (void);
static mapid_t allocate_mapid (void);

static struct list list_file;

typedef int (*handler) (uint32_t, uint32_t, uint32_t);
static handler syscall_map[32];

static void *param_esp;

struct ufile
  {
    struct file *file;                 /* Pointer to the actual file */
    fid_t fid;                         /* File identifier */
    struct list_elem thread_elem;      /* List elem for a thread's file list */
  };

/* Initialization of syscall handlers */
void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");

  syscall_map[SYS_HALT]     = (handler)halt;
  syscall_map[SYS_EXIT]     = (handler)exit;
  syscall_map[SYS_EXEC]     = (handler)exec;
  syscall_map[SYS_WAIT]     = (handler)wait;
  syscall_map[SYS_CREATE]   = (handler)create;
  syscall_map[SYS_REMOVE]   = (handler)remove;
  syscall_map[SYS_OPEN]     = (handler)open;
  syscall_map[SYS_FILESIZE] = (handler)filesize;
  syscall_map[SYS_READ]     = (handler)read;
  syscall_map[SYS_WRITE]    = (handler)write;
  syscall_map[SYS_SEEK]     = (handler)seek;
  syscall_map[SYS_TELL]     = (handler)tell;
  syscall_map[SYS_CLOSE]    = (handler)close;
  syscall_map[SYS_MMAP]     = (handler)mmap;
  syscall_map[SYS_MUNMAP]   = (handler)munmap;
  list_init (&list_file);
}

/* Syscall handler calls the appropriate function. */
static void
syscall_handler (struct intr_frame *f)
{
  handler function;
  int *param = f->esp, ret;

  if ( !is_user_vaddr(param) )
    thread_exit ();

  if (!( is_user_vaddr (param + 1) && is_user_vaddr (param + 2) && is_user_vaddr (param + 3)))
    thread_exit ();

  if (*param < SYS_HALT || *param > SYS_INUMBER)
    thread_exit ();

  function = syscall_map[*param];

  param_esp = f->esp;
  ret = function (*(param + 1), *(param + 2), *(param + 3));
  f->eax = ret;
  return;
}

/* Halt the operating system. */
static void
halt (void)
{
  shutdown_power_off ();
}

/* Terminate this process. */
void
exit (int status)
{
  struct thread *t = thread_current ();

  t->exit_status = status;
  thread_exit ();
}

/* Start another process. */
static pid_t
exec (const char *cmdline)
{
  return process_execute (cmdline);
}

/* Wait for a child process to die. */
static int
wait (pid_t pid)
{
  return process_wait (pid);
}

/* Create a file. */
static bool
create (const char *file, unsigned initial_size)
{
  if (file == NULL)
    thread_exit ();

  lock_acquire (&thread_filesys_lock);
  int ret = filesys_create (file, initial_size);
  lock_release (&thread_filesys_lock);
  return ret;
}

/* Delete a file. */
static bool
remove (const char *file)
{
   if (file == NULL)
     thread_exit ();
  
  lock_acquire (&thread_filesys_lock);
  bool ret = filesys_remove (file);
  lock_release (&thread_filesys_lock);
  return ret;
}

/* Open a file. */
static int
open (const char *file)
{
  struct file *sfile;
  struct ufile *f;

  if (file == NULL)
    return -1;

  lock_acquire (&thread_filesys_lock);
  sfile = filesys_open (file);
  lock_release (&thread_filesys_lock);
  if (sfile == NULL)
    return -1;

  f = (struct ufile *) malloc (sizeof (struct ufile));
  if (f == NULL)
    {
      file_close (sfile);
      return -1;
    }

  lock_acquire (&thread_filesys_lock);
  f->file = sfile;
  f->fid = allocate_fid ();
  list_push_back (&thread_current ()->files, &f->thread_elem);
  lock_release (&thread_filesys_lock);
  return f->fid;
}

/* Obtain a file's size. */
static int
filesize (int fd)
{
  struct ufile *f;
  int size = -1;

  f = file_by_fid (fd);
  if (f == NULL)
    return -1;

  lock_acquire (&thread_filesys_lock);
  size = file_length (f->file);
  lock_release (&thread_filesys_lock);

  return size;
}

/* Read from a file. */
static int
read (int fd, void *buffer, unsigned length)
{
  const void *esp = (const void*)param_esp;

  struct ufile *f;
  int ret = -1;

  if (fd == STDIN_FILENO)
    {
      unsigned i;
      for (i = 0; i < length; ++i)
        *(uint8_t *)(buffer + i) = input_getc ();
      ret = length;
    }
  else if (fd == STDOUT_FILENO)
    ret = -1;
  else if ( !is_user_vaddr (buffer) || !is_user_vaddr (buffer + length) )
    thread_exit ();
  else
    {
      f = file_by_fid (fd);
      if (f == NULL)
        ret = -1;
      else
        {
          size_t rem = length;
          void *tbuffer = (void *)buffer;
          ret = 0;
          while (rem > 0)
            {
              size_t ofs = tbuffer - pg_round_down (tbuffer);
              struct page *p = page_lookup (tbuffer - ofs);       
              if (p == NULL && need_grow (esp, tbuffer))
                p = stack_grow (tbuffer - ofs, true);   
              else if (p == NULL)
                thread_exit ();
              if (!p->loaded)
                page_in (p, true);
              size_t read_bytes = ofs + rem > PGSIZE ?
                                  rem - (ofs + rem - PGSIZE) : rem;
              lock_acquire (&thread_filesys_lock);
              ASSERT (p->loaded);
              ret += file_read (f->file, tbuffer, read_bytes);
              lock_release (&thread_filesys_lock);              
              rem -= read_bytes;
              tbuffer += read_bytes;
              frame_unpin (p->kpage);
            }
        }
    }
  return ret;
}

/* Write to a file. */
static int
write (int fd, const void *buffer, unsigned length)
{
  const void *esp = (const void*)param_esp;

  struct ufile *f;
  int ret = -1;
  if (fd == STDIN_FILENO)
    ret = -1;
  else if (fd == STDOUT_FILENO)
    {
      putbuf (buffer, length);
      ret = length;
    }
  else if (!is_user_vaddr (buffer) || !is_user_vaddr (buffer + length) )
    thread_exit ();
  else
    {
      f = file_by_fid (fd);
      if (f == NULL)
        ret = -1;
      else
        {
          size_t rem = length;
          void *tbuffer = (void *)buffer;
          ret = 0;
          while (rem > 0)
            {
              size_t ofs = tbuffer - pg_round_down (tbuffer);
              struct page *p = page_lookup (tbuffer - ofs);
              if (p == NULL && need_grow (esp, tbuffer) )
                p = stack_grow (tbuffer - ofs, true);   
              else if (p == NULL)
                thread_exit ();
              if (!p->loaded)
                page_in (p, true);
              size_t write_bytes = ofs + rem > PGSIZE ? 
                                   rem - (ofs + rem - PGSIZE) : rem;
              lock_acquire (&thread_filesys_lock);
              ASSERT (p->loaded);
              ret += file_write (f->file, tbuffer, write_bytes);
              lock_release (&thread_filesys_lock);              
              rem -= write_bytes;
              tbuffer += write_bytes;
              frame_unpin (p->kpage);
            }
        }
    }
  return ret;
}

/* Change position in a file */
static void
seek (int fd, unsigned position)
{
  struct ufile *f;
  f = file_by_fid (fd);
  if (!f)
    thread_exit ();
  lock_acquire (&thread_filesys_lock);
  file_seek (f->file, position);
  lock_release (&thread_filesys_lock);
}

/* Report current position in a file */
static unsigned
tell (int fd)
{
  struct ufile *f;
  unsigned status;
  f = file_by_fid (fd);
  if (!f)
    thread_exit ();
  lock_acquire (&thread_filesys_lock);
  status = file_tell (f->file);
  lock_release (&thread_filesys_lock);
  return status;
}

/* Close a file. */
static void
close (int fd)
{
  struct ufile *f;
  f = file_by_fid (fd);
  if (f == NULL)
    thread_exit ();
  lock_acquire (&thread_filesys_lock);
  list_remove (&f->thread_elem);
  file_close (f->file);
  free (f);
  lock_release (&thread_filesys_lock);
}

/* Creates a memory mapped file from the given file. */
mapid_t
mmap (int fd, void *address)
{
  size_t size;
  struct file *file;
  size = filesize(fd);
  lock_acquire (&thread_filesys_lock);
  file = file_reopen (file_by_fid (fd)->file);
  lock_release (&thread_filesys_lock);  
  if (size <= 0 || file == NULL)
    return -1;
  if (address == NULL || address == 0x0 || pg_ofs (address) != 0)
    return -1;
  if (fd == STDIN_FILENO || fd == STDOUT_FILENO)
    return -1;
  size_t ofs = 0;
  void *addr = address;
  while (size > 0)
    {
      size_t read_bytes;
      size_t zero_bytes;
      
      if (size >= PGSIZE)
        {
          read_bytes = PGSIZE;
          zero_bytes = 0;
        }
      else
        {
          read_bytes = size;
          zero_bytes = PGSIZE - size;
        }
      if (page_lookup (addr) != NULL)
          return -1;
      page_file (addr, file, ofs, read_bytes, zero_bytes, true, -1);
      ofs += PGSIZE;
      size -= read_bytes;
      addr += PGSIZE;
    }
  mapid_t mapid = allocate_mapid();
  mfile_add (mapid, fd, address, addr);
  return mapid;
}

/* unmaps a files */
void
munmap (mapid_t mapid)
{
  struct mfile *mf = mfile_lookup (mapid);
  if (mf == NULL)
    thread_exit ();

  void *address = mf->addr_init;

  for (;address < mf->addr_fin; address += PGSIZE)
    {
      struct page *p = NULL;
      p = page_lookup (address);
      if (p == NULL)
        continue;
      if (p->loaded == true)
        {
          page_pin (p);
          ASSERT (p->loaded && p->kpage != NULL);
          frame_free (p->kpage, p->pagedir);
        } 
      page_free (p);
    }
  mfile_rem (mapid);
}

/* Allocate a new fid for a file */
static fid_t
allocate_fid (void)
{
  static fid_t next_fid = 2;
  return next_fid++;
}

/* Allocate a new mapid for a file */
static mapid_t
allocate_mapid (void)
{
  static mapid_t next_mapid = 0;
  return next_mapid++;
}

/* Returns the file with the given fid from the current thread's files */
static struct ufile *
file_by_fid (int fid)
{
  struct list_elem *e;
  struct thread *t;

  t = thread_current();
  for (e = list_begin (&t->files); e != list_end (&t->files);
       e = list_next (e))
    {
      struct ufile *f = list_entry (e, struct ufile, thread_elem);
      if (f->fid == fid)
        return f;
    }

  return NULL;
}

/* Extern function for sys_exit */
void 
syscall_exit (void)
{
  struct list_elem *e;
	struct thread *t = thread_current ();

  if (lock_held_by_current_thread (&thread_filesys_lock))
    lock_release (&thread_filesys_lock);

	/* close all opened files of the thread */
	while (!list_empty (&t->files) )
		{
			e = list_begin (&t->files);
			close (list_entry (e, struct ufile, thread_elem)->fid);
		}
	/* unmap all memory mapped files of the thread */
	while (!list_empty (&t->mfiles) )
		{
			e = list_begin (&t->mfiles);
			munmap (list_entry (e, struct mfile, thread_elem)->mapid );
		};
}

