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
#include <kern/fcntl.h>
#include <proctable.h>
#include <addrspace.h>
#include <copyinout.h>
#include <thread.h>
#include <filetable.h>
#include <vnode.h>
#include <vfs.h>

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

/*
 * Given the ARGS buffer, extract all the arg strings into ARGBUF making sure
 * no invalid memory operations are made. Helper for sys_execv().
 */
static
int
extract_args(userptr_t *args, char **argbuf, int *argcount)
{
  int result, argc;
  size_t length;
  /* Get the number of arguments being passed. args is NULL terminated. */
  argc = 0;
  int total_size = 0; /* The total combined size of args (should be less than ARG_MAX). */
  char *temp = kmalloc(sizeof(char)*ARG_MAX);
  if(temp == NULL)
  {
    return ENOMEM;
  }

  /* Copy the actual argument strings one by one, keeping a check for invalid
   * addresses. We use a dummy for copyin to make sure no invalid address is in args. */
  char *dummy;
  result = copyin((const_userptr_t)&args[argc], &dummy, sizeof(char *));
  if(result)
  {
    kfree(temp);
    return result;
  }

  while(dummy != NULL)
  {
    result = copyinstr((const_userptr_t)args[argc], temp, ARG_MAX, &length);
    if(result)
    {
      for(int j = 0; j < argc; j++)
      {
        kfree(argbuf[j]);
      }
      kfree(temp);
      return result;
    }
    total_size += length;
    /* If the total size of args exceeds ARG_MAX, return error. */
    if(total_size > ARG_MAX)
    {
      for(int j = 0; j < argc; j++)
      {
        kfree(argbuf[j]);
      }
      kfree(temp);
      return E2BIG;
    }

    argbuf[argc] = kmalloc(sizeof(char) * length);
    strcpy(argbuf[argc], temp);
    argc++;

    /*
     * This condition might become true when args is not NULL terminated.
     * Here we are checking if argbuf[argc] goes out of bounds of the allocated
     * memory. There should be a more elegant way to check for this but I can't
     * be bothered to try.
     */
    if(&argbuf[argc] > (argbuf + ARG_MAX))
    {
      for(int j = 0; j < argc; j++)
      {
        kfree(argbuf[j]);
      }
      kfree(temp);
      return E2BIG;
    }

    /* Copyin the next arg to make sure it is a valid pointer. */
    result = copyin((const_userptr_t)&args[argc], &dummy, sizeof(char *));
    if(result)
    {
      for(int j = 0; j < argc; j++)
      {
        kfree(argbuf[j]);
      }
      kfree(temp);
      return result;
    }
  }

  kfree(temp);
  argbuf[argc] = NULL;
  *argcount = argc;
  return 0;
}

int
sys_execv(const_userptr_t program, userptr_t *args)
{
  int result, argc;
  size_t length;
  char pathname[PATH_MAX];
  struct vnode *vn;
  vaddr_t startpoint, stackptr;
  struct addrspace *oldas, *as;
  userptr_t *uargs;
  char **argbuf; /* Buffer to temporarily store args. */

  argbuf = kmalloc(sizeof(char*)*ARG_MAX); /* Max total size of args is ARG_MAX. */
  if(argbuf == NULL)
  {
    return ENOMEM;
  }

  result = extract_args(args, argbuf, &argc);
  if(result)
  {
    kfree(argbuf);
    return result;
  }

  /* Copy the program name from userspace buffer to kernel-space buffer. */
  result = copyinstr(program, pathname, PATH_MAX, &length);
  if(result)
  {
    for(int i = 0; i < argc; i++)
    {
      kfree(argbuf[i]);
    }
    kfree(argbuf);
    return result;
  }

  /* Open the file. */
  result = vfs_open(pathname, O_RDONLY, 0, &vn);
  if(result)
  {
    for(int i = 0; i < argc; i++)
    {
      kfree(argbuf[i]);
    }
    kfree(argbuf);
    return result;
  }

  /* Remove the old address space, but don't get rid of it just yet, in case exec fails somewhere ahead. */
  oldas = proc_getas();
  as_deactivate();

  /* Create a new address space. */
	as = as_create();
	if (as == NULL) {
		vfs_close(vn);
    for(int i = 0; i < argc; i++)
    {
      kfree(argbuf[i]);
    }
    kfree(argbuf);
		return ENOMEM;
	}

  /* Switch to the new address space and activate it. */
	proc_setas(as);
	as_activate();

  /* Load the ELF file. */
  result = load_elf(vn, &startpoint);
  if(result)
  {
    for(int i = 0; i < argc; i++)
    {
      kfree(argbuf[i]);
    }
    kfree(argbuf);
    vfs_close(vn);

    /* Get rid of the new address space */
    as_deactivate();
    as_destroy(as);

    /* Restore the old address space. */
    proc_setas(oldas);
    as_activate();
    return result;
  }

  vfs_close(vn); /* We are done with the file. */

  result = as_define_stack(as, &stackptr);
  if(result)
  {
    for(int i = 0; i < argc; i++)
    {
      kfree(argbuf[i]);
    }
    kfree(argbuf);

    /* Get rid of the new address space. */
    as_deactivate();
    as_destroy(as);

    /* Restore the old address space. */
    proc_setas(oldas);
    as_activate();
    return result;
  }

  /* Setting up args in new process's userspace. */
  stackptr -= (argc + 1) * sizeof(char *); /* Create space for all string (including NULL terminator) pointers on stack */
  uargs = (userptr_t*)stackptr;
  for(int i = 0; i < argc; i++)
  {
    stackptr -= strlen(argbuf[i]) + 1;
    uargs[i] = (userptr_t)stackptr;
    result = copyout(argbuf[i], uargs[i], strlen(argbuf[i]) + 1);
    if(result)
    {
      /* Should we panic here or just return an error? I'm not sure. */
      panic("copyout failed!"); /* Possible errors in args should be already checked for */
    }
  }
  uargs[argc] = NULL;

  /* Everything done. Time for cleanup. */
  as_destroy(oldas);
  for(int i = 0; i < argc; i++)
  {
    kfree(argbuf[i]);
  }
  kfree(argbuf);

  enter_new_process(argc, (userptr_t)uargs /* Userspace address of argv */,
        NULL /* Userspace address of env */, stackptr, startpoint);

  /* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}
