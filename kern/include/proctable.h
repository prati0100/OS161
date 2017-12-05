#ifndef _PROCTABLE_H
#define _PROCTABLE_H

#include <types.h>
#include <spinlock.h>
#include <proc.h>

/*
 * The process table to be maintained by the kernel.
 *
 * Note: The process table functions just return the pid, that pid should
 * be assigned to the proc structure by the calling function. They do not set
 * those pid values themselves. Also, we cannot account for the changes to pid
 * values of the proc structures by other functions. It is suggested to not
 * change pid values directly without making sure no pid collisions occur
 */
struct proctable {
  struct proc **table;   /*The list of proc structures*/
  struct spinlock pt_spinlock;
};

extern struct proctable *kproctable; /*The global proctable structure*/

/*Set up the process table, called once during boot*/
void proctable_bootstrap(void);

/*
 * Create a new proctable
 */
struct proctable * ptable_create(void);

/*
 * Clean up the proctable.
 */
void ptable_destroy(struct proctable *pt);

/*
 * Insert a new process to the table. Returns the new pid of the process, but
 * does not assign that pid to the process. This must be done by the calling
 * function.
 */
int ptable_insert(struct proc *p, pid_t *retval);

/*
 * Remove a process from the table. Returns the process removed, but does not
 * clear up the pid in the proc. This must be done by the calling function.
 * If the pid does not exist, set RETVAL to NULL, do nothing else, and return.
 */
int ptable_remove(pid_t pid, struct proc **retval);

/*
 * Get the process associated with the pid, and set it to RETVAL. If the pid
 * does not exist, set RETVAl to NULL, and return error code
 */
int ptable_get(pid_t pid, struct proc **retval);
#endif
