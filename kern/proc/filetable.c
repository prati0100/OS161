#include <types.h>
#include <lib.h>
#include <kern/unistd.h>
#include <kern/errno.h>
#include <limits.h>
#include <spinlock.h>
#include <vfs.h>
#include <synch.h>
#include <kern/fcntl.h>
#include <filetable.h>

struct filehandle *
fhandle_create(const char *name, struct vnode *vn, int flags)
{
  struct filehandle *fh = kmalloc(sizeof(*fh));
  if(fh == NULL)
  {
    return NULL;
  }

  fh->name = kstrdup(name);
  if(fh->name == NULL)
  {
    kfree(fh);
    return NULL;
  }

  fh->fh_vn = vn;
  fh->fh_lock = lock_create(name);
  fh->offset = 0;
  fh->flags = flags;
  fh->fh_refcount = 1;
  return fh;
}

void
fhandle_destroy(struct filehandle *fh)
{
  KASSERT(fh != NULL);

  kfree(fh->name);
  lock_destroy(fh->fh_lock);
  VOP_DECREF(fh->fh_vn);
  kfree(fh);
}

struct filetable *
ftable_create(const char *name)
{
  struct filetable *ft = kmalloc(sizeof(*ft));
  struct vnode *stdin = NULL;
  struct vnode *stdout = NULL;
  struct vnode *stderr = NULL;
  char *in = kstrdup("con:");
  char *out = kstrdup("con:");
  char *err = kstrdup("con:");
  int result;
  int flags;
  int i;

  if(ft == NULL)
  {
    return NULL;
  }

  ft->name = kstrdup(name);

  spinlock_init(&ft->ft_lock);
  for(i = 0; i < OPEN_MAX; i++)
  {
    ft->table[i] = NULL;
  }

  /*Setting up stdin*/
  flags = O_RDONLY;
  result = vfs_open(in, flags, 0, &stdin);
  if(result)
  {
    goto fail;
  }
  KASSERT(stdin != NULL);
  ft->table[STDIN_FILENO] = fhandle_create(name, stdin, flags);
  kfree(in);

  /*Setting up stdout*/
  flags = O_WRONLY;
  result = vfs_open(out, flags, 0, &stdout);
  if(result)
  {
    goto fail;
  }
  KASSERT(stdout != NULL);
  ft->table[STDOUT_FILENO] = fhandle_create(name, stdout, flags);
  kfree(out);

  /*Setting up stderr*/
  flags = O_WRONLY;
  result = vfs_open(err, flags, 0, &stderr);
  if(result)
  {
    goto fail;
  }
  KASSERT(stderr != NULL);
  ft->table[STDERR_FILENO] = fhandle_create(name, stderr, flags);
  kfree(err);

  return ft;

  fail:
    kfree(stdin);
    kfree(stdout);
    kfree(stderr);
    kfree(in);
    kfree(out);
    kfree(err);
    kfree(ft);
    return NULL;
}

void ftable_destroy(struct filetable *ft)
{
  KASSERT(ft != NULL);

  int i;

  /*Free up the 3 stdin, stdout and stderr vnodes we created when we created the filetable*/
  struct vnode *vn;
  vn = ft->table[STDIN_FILENO]->fh_vn;
  KASSERT(vn->vn_refcount == 1);
  fhandle_destroy(ft->table[STDIN_FILENO]);
  kfree(vn);

  vn = ft->table[STDOUT_FILENO]->fh_vn;
  KASSERT(vn->vn_refcount == 1);
  fhandle_destroy(ft->table[STDOUT_FILENO]);
  kfree(vn);

  vn = ft->table[STDERR_FILENO]->fh_vn;
  KASSERT(vn->vn_refcount == 1);
  fhandle_destroy(ft->table[STDERR_FILENO]);
  kfree(vn);

  /*0,1,2 reserved for stdin, out, err respectively*/
  for(i = 3; i < OPEN_MAX; i++)
  {
    if(ft->table[i] == NULL)
    {
      continue;
    }

    fhandle_destroy(ft->table[i]);
  }

  kfree(ft);
}

int
ftable_add(struct filetable *ft, struct filehandle *fh, int *ret)
{
  KASSERT(ft != NULL);
  KASSERT(fh != NULL);

  spinlock_acquire(&ft->ft_lock);
  int i = 0;
  /*Find the first empty index*/
  while(ft->table[i] != NULL)
  {
    i++;

    if(i == OPEN_MAX) /*No index empty. Too many files open*/
    {
      *ret = -1;
      spinlock_release(&ft->ft_lock);
      return EMFILE;
    }
  }
  ft->table[i] = fh;
  *ret = i;
  spinlock_release(&ft->ft_lock);
  return 0;
}

int
ftable_get(struct filetable *ft, int index, struct filehandle **ret)
{
  KASSERT(ft != NULL);

  spinlock_acquire(&ft->ft_lock);
  if(index < 0 || index >= OPEN_MAX)
  {
    spinlock_release(&ft->ft_lock);
    return EBADF;
  }

  if(ft->table[index] == NULL)
  {
    spinlock_release(&ft->ft_lock);
    return EBADF;
  }

  *ret = ft->table[index];
  spinlock_release(&ft->ft_lock);
  return 0;
}

int
ftable_remove(struct filetable *ft, int index)
{
  KASSERT(ft != NULL);

  spinlock_acquire(&ft->ft_lock);
  if(index < 0 || index >= OPEN_MAX)
  {
    spinlock_release(&ft->ft_lock);
    return EBADF;
  }

  if(ft->table[index] == NULL)
  {
    spinlock_release(&ft->ft_lock);
    return EBADF;
  }

  ft->table[index]->fh_refcount--;
  spinlock_release(&ft->ft_lock);
  if(ft->table[index]->fh_refcount == 0)
  {
    fhandle_destroy(ft->table[index]);
  }


  return 0;
}
