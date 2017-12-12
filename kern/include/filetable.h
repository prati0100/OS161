/*
 * Author: Pratyush Yadav
 */

#ifndef _FILETABLE_H_
#define _FILETABLE_H_

#include <types.h>
#include <vnode.h>
#include <spinlock.h>
#include <synch.h>
#include <limits.h>

/*The file handle object that describes each file opened*/
struct filehandle {
  char *name;
  struct vnode *fh_vn;     /*The file object*/
  off_t offset;      /*The current seek position. It is initialized to 0 and
                        *changed when a read/write is done*/
  struct lock *fh_lock;    /*Lock for read/write operations*/
  int fh_refcount;    /*initialized to 1 when file handle is created*/
  int flags;     /*The flags with which the file was opened*/
};

/*The file table associated with each process.
 *The first 3 entries in open_files are stdin, stdout and stderr respectively*/
struct filetable {
  char *name;
  struct spinlock ft_lock;     /*In case atomic operations on filetable are needed*/
  struct filehandle *table[OPEN_MAX];
};

/*Make sure all the open files are closed when using ftable_destroy*/
struct filetable * ftable_create(const char *name);
void ftable_destroy(struct filetable *);

/*fhandle_destroy does not free the vnode object, if you malloc'd it, you have
 *to free it*/
struct filehandle * fhandle_create(const char* name, struct vnode *, int flags);
void fhandle_destroy(struct filehandle *);

/*Operations:
 *    ftable_add         -    Add the given vnode to the file table, set the index
 *                            at which the vnode is added to the value of RET.
 *                            The return value if for error codes
 *    ftable_get         -    Get the vnode at the given index. Set the vnode pointer in ret
 *    ftable_remove      -    Remove the filetable entry at INDEX
 */
int ftable_add(struct filetable *, struct filehandle *, int *ret);
int ftable_get(struct filetable *, int index, struct filehandle **ret);
int ftable_remove(struct filetable *, int index);
#endif
