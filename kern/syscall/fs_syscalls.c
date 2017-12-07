/*
 * Author: Pratyush Yadav
 */

#include <types.h>
#include <limits.h>
#include <syscall.h>
#include <copyinout.h>
#include <vfs.h>
#include <kern/fcntl.h>
#include <kern/errno.h>
#include <filetable.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <kern/seek.h>
#include <stat.h>
#include <spinlock.h>

/*Opens a file in the file table of the process*/
int sys_open(const userptr_t filename, int flags, mode_t mode, int32_t *retval)
{
  char buf[PATH_MAX];
  size_t len; //We need for cpoyinstr, don't think there is much need for it. Keep in mind it includes null terminator
  int result;
  int fd; /*The file descriptor, that ftable_add will give us*/
  struct vnode *vn;
  struct filehandle *fh;
  struct filetable *ft = curproc->p_ftable;
  KASSERT(ft != NULL);

  result = copyinstr(filename, buf, PATH_MAX, &len);
  if(result)
  {
    return result;
  }

  result = vfs_open(buf, flags, mode, &vn);
  if(result)
  {
    return result;
  }

  fh = fhandle_create("from sys_open", vn, flags);
  result = ftable_add(ft, fh, &fd);
  if(result)
  {
    return result;
  }

  *retval = fd;
  return 0;
}

int sys_close(int fd)
{
  struct filetable *ft = curproc->p_ftable;
  int result;
  KASSERT(ft != NULL);

  result = ftable_remove(ft, fd);

  if(result)
  {
    return result;
  }

  return 0;
}

int sys_read(int fd, userptr_t buf, size_t buflen, int32_t *retval)
{
  struct filetable *ft = curproc->p_ftable;
  struct filehandle *fh;
  struct vnode *vn;
  struct uio u;
  struct iovec iov;
  int result, bytes_read, flags;
  off_t offset;
  struct lock *lk;

  KASSERT(ft != NULL);

  result = ftable_get(ft, fd, &fh);
  if(result)
  {
    return result;
  }

  lk = fh->fh_lock;
  lock_acquire(lk);

  offset = fh->offset;
  vn = fh->fh_vn;
  flags = fh->flags;

  /*If the access mode for the file is not read only or readwrite, return error*/
  if((flags & O_ACCMODE) == O_WRONLY)
  {
    lock_release(lk);
    *retval = -1;
    return EBADF;
  }

  /*data and length*/
  iov.iov_ubase = buf;
  iov.iov_len = buflen;

  /*flags and references*/
  u.uio_iov = &iov;
  u.uio_iovcnt = 1;
  u.uio_offset = offset;
  u.uio_resid = buflen;
  u.uio_segflg = UIO_USERSPACE;
  u.uio_rw = UIO_READ;
  u.uio_space = curproc->p_addrspace;

  result = VOP_READ(vn, &u);
  if(result)
  {
    lock_release(lk);
    return result;
  }

  bytes_read = u.uio_offset - offset; /*The offset now - the old offset, will give number of bytes read*/

  lock_release(lk);

  *retval = bytes_read;
  return 0;
}

int sys_write(int fd, userptr_t buf, size_t buflen, int32_t *retval)
{
  struct filetable *ft = curproc->p_ftable;
  struct filehandle *fh;
  struct vnode *vn;
  struct uio u;
  struct iovec iov;
  int result, bytes_written, flags;
  off_t offset;
  struct lock *lk;

  KASSERT(ft != NULL);

  result = ftable_get(ft, fd, &fh);
  if(result)
  {
    return result;
  }

  lk = fh->fh_lock;
  lock_acquire(lk);

  offset = fh->offset;
  vn = fh->fh_vn;
  flags = fh->flags;

  /*If the access mode for the file is not write only or readwrite, return error*/
  if((flags & O_ACCMODE) == O_RDONLY)
  {
    lock_release(lk);
    *retval = -1;
    return EBADF;
  }

  /* data and length */
   iov.iov_ubase = buf;
   iov.iov_len = buflen;

   /* flags and references */
   u.uio_iovcnt = 1;
   u.uio_iov = &iov;
   u.uio_segflg = UIO_USERSPACE;
   u.uio_rw = UIO_WRITE;
   u.uio_space = curproc->p_addrspace;
   u.uio_resid = buflen;
   u.uio_offset = offset;

  result = VOP_WRITE(vn, &u);
  if(result)
  {
    lock_release(lk);
    return result;
  }

  bytes_written = u.uio_offset - offset; /*The offset now - the old offset, will give number of bytes written*/

  fh->offset += bytes_written;

  lock_release(lk);

  *retval = bytes_written;
  return 0;
}

/*Need to fix this, there is something wrong somewhere*/
int
sys_lseek(int fd, off_t pos, int whence, int32_t *retval)
{
  struct filetable *ft = curproc->p_ftable;
  struct filehandle *fh;
  struct vnode *vn;
  struct lock *lk;
  struct stat st;
  off_t filesize, offset;
  int result;

  result = ftable_get(ft, fd, &fh);
  if(result)
  {
    return result;
  }

  lk = fh->fh_lock;
  lock_acquire(lk);
  vn = fh->fh_vn;
  offset = fh->offset;

  /*Check if the file is seekable*/
  if(!VOP_ISSEEKABLE(vn))
  {
    lock_release(lk);
    return ESPIPE;
  }

  result = VOP_STAT(vn, &st);
  if(result)
  {
    lock_release(lk);
    return result;
  }

  filesize = st.st_size;
  //DEBUG(DB_MDB, "sys_lseek: filesize is %d\n", (int)filesize);

  //DEBUG(DB_MDB, "sys_lseek: whence is %d\n", whence);
  switch(whence)
  {
    case SEEK_SET:
    {
      if(pos < 0)
      {
        goto err;
      }

      fh->offset = pos;
      break;
    }

    case SEEK_CUR:
    {
      if(offset + pos < 0)
      {
        goto err;
      }

      fh->offset = offset + pos;
      break;
    }

    case SEEK_END:
    {
      if(filesize + pos < 0)
      {
        goto err;
      }

      fh->offset = filesize + pos;
      break;
    }

    default:
      DEBUG(DB_MDB, "sys_lseek: Inside default\n");
      goto err;
  }

  *retval = fh->offset;
  lock_release(lk);
  return 0;

  err:
    DEBUG(DB_MDB, "sys_lseek: inside err\n");
    lock_release(lk);
    *retval = -1;
    return EINVAL;
}

int sys_dup2(int oldfd, int newfd, int32_t *retval)
{
  struct filetable *ft = curproc->p_ftable;
  struct filehandle *oldfh, *newfh;
  int result;

  KASSERT(ft != NULL);

  if(oldfd == newfd)
  {
    *retval = oldfd;
    return 0;
  }

  result = ftable_get(ft, oldfd, &oldfh);
  if(result)
  {
    return result;
  }

  /*Can't use ftable_get for newfh, because it will return the same error for
   *both cases when file handle is NULL, and when newfd is invalid, so we can't
   *differentiate between the two*/

  spinlock_acquire(&ft->ft_lock);
  if(newfd < 0 || newfd >= OPEN_MAX)
  {
    spinlock_release(&ft->ft_lock);
    return EBADF;
  }

  newfh = ft->table[newfd];
  spinlock_release(&ft->ft_lock);

  if(newfh != NULL)
  {
    sys_close(newfd);
  }

  spinlock_acquire(&ft->ft_lock);
  oldfh->fh_refcount++;
  ft->table[newfd] = oldfh;
  spinlock_release(&ft->ft_lock);

  *retval = newfd;
  return 0;
}

int
sys_chdir(const_userptr_t pathname, int32_t *retval)
{
  size_t len;
  int result;

  char *pname = kmalloc(sizeof(char)*PATH_MAX);
  if(pname == NULL)
  {
    return ENOMEM;
  }
  result = copyinstr(pathname, pname, PATH_MAX, &len);
  if(result)
  {
    kfree(pname);
    return result;
  }

  result = vfs_chdir(pname);
  if(result)
  {
    kfree(pname);
    return result;
  }

  kfree(pname);
  *retval = 0;
  return 0;
}

int
sys___getcwd(userptr_t buf, size_t buflen, int32_t *retval)
{
  int result;
  struct uio u;
  struct iovec iov;

  /*Setting up iovec*/
  iov.iov_ubase = buf;
  iov.iov_len = buflen;

  /*Setting up uio*/
  u.uio_iov = &iov;
  u.uio_iovcnt = 1;
  u.uio_offset = 0;
  u.uio_resid = buflen;
  u.uio_segflg = UIO_USERSPACE;
  u.uio_rw = UIO_READ;
  u.uio_space = curproc->p_addrspace;

  result = vfs_getcwd(&u);
  if(result)
  {
    return result;
  }

  *retval = buflen - u.uio_resid;
  return 0;
}
