/*
 * Author: Pratyush Yadav
 */

#include <types.h>
#include <limits.h>
#include <synch.h>
#include <current.h>
#include <syscall.h>
#include <proc.h>
#include <mips/trapframe.h>
#include <kern/errno.h>
#include <kern/wait.h>
#include <proctable.h>
#include <addrspace.h>
#include <copyinout.h>
#include <thread.h>
#include <filetable.h>

int sys_getpid(int32_t *retval)
{
  *retval = (int32_t) curproc->p_pid;
  return 0;
}

static
void
entrypoint(void *data1, unsigned long data2)
{
  struct trapframe newtf;
  struct trapframe *tf = data1;
  (void) data2; /* To avoid the compiler warning. Nothing we want to do with data2 */


  /* Set up the trapframe for the new process */
  newtf = *tf;
  kfree(tf); /* We allocated tf in sys_fork(). Free it, because we are now done with it */

  newtf.tf_v0 = 0; /* Return value of fork(). For the child it is 0 */
  newtf.tf_a3 = 0; /* Signal no error */
  newtf.tf_epc += 4; /* Advance the program counter to avoid restarting the syscall again and again */

  as_activate();

  mips_usermode(&newtf); /* Enter user mode */
}

int sys_fork(struct trapframe *tf, int32_t *retval)
{
  struct proc *childproc;
  int result;
  pid_t childpid;

  childproc = proc_create_runprogram("child");
  if(childproc == NULL)
  {
    return ENOMEM;
  }

  result = as_copy(curproc->p_addrspace, &childproc->p_addrspace);
  if(result)
  {
    return result;
  }

  /* Set child's parent pid to current process's pid */
  childproc->p_ppid = curproc->p_pid;

  /* Add newproc to the process table and set its pid */
  result = ptable_insert(childproc, &childpid);
  if(result)
  {
    return result;
  }

  childproc->p_pid = childpid;

  /*Copy the file table*/
	struct filetable *ft = ftable_create("name");
	if(ft == NULL)
  {
    return ENOMEM;
  }

  /*
   * ftable_create allocates 3 file handles, for stdin, stdout, stderr. We don't
   * need them here because we will copy those from the parent's file table
   */
  fhandle_destroy(ft->table[0]);
  fhandle_destroy(ft->table[1]);
  fhandle_destroy(ft->table[2]);

  /* Copy all the file handles of the parent, and increase their refcount */
  for(int i = 0; i < OPEN_MAX; i++)
  {
    ft->table[i] = curproc->p_ftable->table[i];
    if(ft->table[i] != NULL)
    {
      ft->table[i]->fh_refcount++;
    }
  }
  childproc->p_ftable = ft;

  struct trapframe *newtf = kmalloc(sizeof(*newtf));
  *newtf = *tf; /* Copy the data of our trapframe to the new trapframe */

  /* Fork a thread for this new process */
  result = thread_fork(childproc->p_name, childproc, entrypoint, (void *)newtf, 0);
  if(result)
  {
    return result;
  }

  *retval = childpid;
  return 0;
}

int
sys__exit(int exitcode)
{
  /*
   * If the parent process has already exited or is not available in the process
   * table (which also means that it has exited), then simply destroy the process
   * because nobody else is there to collect the exit status. In unix systems
   * orphans are usually assigned to init or an equivalent process, but there
   * is no such mechanism in OS161.
   */
  struct proc *parent;
  int result;
  /* We don't check the result here because we know that only two cases are
   * possible: either the parent does not exist, this case is checked by
   * the if. Or, the parent pid is out of range. This will not happen
   * because the invalid pid won't be assigned as ppid in the first place.
   */
  result = ptable_get(curproc->p_ppid, &parent);
  if(parent == NULL || parent->p_exited == true)
  {
    struct proc *p;
    /* Since there is no parent to call waitpid(), we must remove the current
     * process from the process table */
    result = ptable_remove(curproc->p_pid, &p);
    KASSERT(result == 0); /* Removal of current process HAS to succeed */
    proc_remthread(curthread);
    proc_destroy(curproc);
  }
  /* Otherwise store the exit code and then exit, without destroying the process */
  else
  {
    /* Set up the exit status for this process. For details about this macro, check kern/wait.h */
    int status = _MKWAIT_EXIT(exitcode);
    spinlock_acquire(&curproc->p_lock);
    curproc->p_exitstatus = status;
    curproc->p_exited = true;
    lock_acquire(curproc->p_waitlock);
    cv_broadcast(curproc->p_waitcv, curproc->p_waitlock); /* Wake up all processes waiting for us to exit */
    lock_release(curproc->p_waitlock);
    spinlock_release(&curproc->p_lock);
  }

  thread_exit(); /* Kill the current thread and end the process */
  return 0;
}

int
sys_waitpid(pid_t pid, userptr_t status, int options, int32_t *retval)
{
  int result, exitstatus;
  struct proc *target;

  /* Check if the options provided are invalid, even though we won't use them */
  if(!(options == 0 || options == 1 || options == 2))
  {
    return EINVAL;
  }

  result = ptable_get(pid, &target);
  if(result)
  {
    return result;
  }
  KASSERT(target != NULL); /* The process we get must not be NULL. */

  /* If the target is not our child, return an error. */
  if(target->p_ppid != curproc->p_pid)
  {
    return ECHILD;
  }

  /* If the target has not already exited, go to sleep and wait for it to exit */
  while(target->p_exited == false)
  {
    lock_acquire(target->p_waitlock);
    cv_wait(target->p_waitcv, target->p_waitlock);
    lock_release(target->p_waitlock);
  }

  exitstatus = target->p_exitstatus;
  if(status != NULL)
  {
    result = copyout(&exitstatus, status, sizeof(exitstatus));
    if(result)
    {
      return result;
    }
  }

  /*
   * Reclaim the pid. Value of target should not be affected by this call
   * because it will return the process with the specified pid, which is target
   * itself. Also, we don't need to check the value returned by ptable_remove,
   * because any possible errors are already handled above. The pid is guaranteed
   * to be valid.
   */
  ptable_remove(pid, &target);
  proc_destroy(target); /* Clean up the target process because we don't need it anymore */

  *retval = pid;
  return 0;
}

int
sys_execv(const_userptr_t program, userptr_t args)
{
  int result;

}
