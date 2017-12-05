/*
 * Author: Pratyush Yadav
 */
 
#include <types.h>
#include <proc.h>
#include <limits.h>
#include <proctable.h>
#include <lib.h>
#include <kern/errno.h>
#include <spinlock.h>

/*The process table for our kernel, created at boot*/
struct proctable *kproctable;

void proctable_bootstrap()
{
  kproctable = ptable_create();
  if(kproctable == NULL)
  {
    panic("proctable_create failed for kproctable\n");
  }

  kproctable->table[0] = kproc;
}

struct proctable *
ptable_create()
{
  struct proctable *pt = kmalloc(sizeof(*pt));
  if(pt == NULL)
  {
    return NULL;
  }

  spinlock_init(&pt->pt_spinlock);
  pt->table = kmalloc(sizeof(*pt->table)*PID_MAX);
  if(pt->table == NULL)
  {
    kfree(pt);
    return NULL;
  }

  for(pid_t i = 0; i < PID_MAX; i++)
  {
    pt->table[i] = NULL;
  }

  return pt;
}

void ptable_destroy(struct proctable *pt)
{
  KASSERT(pt != NULL);

  //Maybe check if the process table is empty? I'm not sure
  kfree(pt->table);
  spinlock_cleanup(&pt->pt_spinlock);
  kfree(pt);
}

int
ptable_insert(struct proc *p, pid_t *retval)
{
  struct proctable *pt = kproctable;
  KASSERT(pt != NULL);
  KASSERT(p != NULL);

  pid_t pid = 0;

  spinlock_acquire(&pt->pt_spinlock);

  pid_t i;
  for(i = PID_MIN; i < PID_MAX; i++)
  {
    if(pt->table[i] != NULL)
    {
      continue;
    }

    //We found the first NULL table entry. The index will pe the process's pid
    pid = i;
    pt->table[i] = p;
    break;
  }

  if(i == PID_MAX)
  {
    spinlock_release(&pt->pt_spinlock);
    *retval = 0;
    return EMPROC;
  }
  *retval = pid;
  spinlock_release(&pt->pt_spinlock);
  return 0;
}

int
ptable_remove(pid_t pid, struct proc **retval)
{
  struct proctable *pt = kproctable;
  KASSERT(pt != NULL);

  if(pid < 0 || pid >= PID_MAX)
  {
    *retval = NULL;
    return ESRCH;
  }

  spinlock_acquire(&pt->pt_spinlock);

  *retval = pt->table[pid];
  pt->table[pid] = NULL;
  spinlock_release(&pt->pt_spinlock);
  return 0;
}

int
ptable_get(pid_t pid, struct proc **retval)
{
  struct proctable *pt = kproctable;
  KASSERT(pt != NULL);

  if(pid < 0 || pid >= PID_MAX)
  {
    *retval = NULL;
    return ESRCH;
  }

  spinlock_acquire(&pt->pt_spinlock);
  *retval = pt->table[pid];
  spinlock_release(&pt->pt_spinlock);

  /* If the process does not exist, return ESRCH (no such process) */
  if(*retval == NULL)
  {
    return ESRCH;
  }
  return 0;
}
