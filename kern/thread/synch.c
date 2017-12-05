/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Synchronization primitives.
 * The specifications of the functions are in synch.h.
 */

#include <types.h>
#include <lib.h>
#include <spinlock.h>
#include <wchan.h>
#include <thread.h>
#include <current.h>
#include <synch.h>

////////////////////////////////////////////////////////////
//
// Semaphore.

struct semaphore *
sem_create(const char *name, unsigned initial_count)
{
	struct semaphore *sem;

	sem = kmalloc(sizeof(*sem));
	if (sem == NULL) {
		return NULL;
	}

	sem->sem_name = kstrdup(name);
	if (sem->sem_name == NULL) {
		kfree(sem);
		return NULL;
	}

	sem->sem_wchan = wchan_create(sem->sem_name);
	if (sem->sem_wchan == NULL) {
		kfree(sem->sem_name);
		kfree(sem);
		return NULL;
	}

	spinlock_init(&sem->sem_lock);
	sem->sem_count = initial_count;

	return sem;
}

void
sem_destroy(struct semaphore *sem)
{
	KASSERT(sem != NULL);

	/* wchan_cleanup will assert if anyone's waiting on it */
	spinlock_cleanup(&sem->sem_lock);
	wchan_destroy(sem->sem_wchan);
	kfree(sem->sem_name);
	kfree(sem);
}

void
P(struct semaphore *sem)
{
	KASSERT(sem != NULL);

	/*
	 * May not block in an interrupt handler.
	 *
	 * For robustness, always check, even if we can actually
	 * complete the P without blocking.
	 */
	KASSERT(curthread->t_in_interrupt == false);

	/* Use the semaphore spinlock to protect the wchan as well. */
	spinlock_acquire(&sem->sem_lock);
	while (sem->sem_count == 0) {
		/*
		 *
		 * Note that we don't maintain strict FIFO ordering of
		 * threads going through the semaphore; that is, we
		 * might "get" it on the first try even if other
		 * threads are waiting. Apparently according to some
		 * textbooks semaphores must for some reason have
		 * strict ordering. Too bad. :-)
		 *
		 * Exercise: how would you implement strict FIFO
		 * ordering?
		 */
		wchan_sleep(sem->sem_wchan, &sem->sem_lock);
	}
	KASSERT(sem->sem_count > 0);
	sem->sem_count--;
	spinlock_release(&sem->sem_lock);
}

void
V(struct semaphore *sem)
{
	KASSERT(sem != NULL);

	spinlock_acquire(&sem->sem_lock);

	sem->sem_count++;
	KASSERT(sem->sem_count > 0);
	wchan_wakeone(sem->sem_wchan, &sem->sem_lock);

	spinlock_release(&sem->sem_lock);
}

////////////////////////////////////////////////////////////
//
// Lock.

struct lock *
lock_create(const char *name)
{
	struct lock *lock;

	lock = kmalloc(sizeof(*lock));
	if (lock == NULL) {
		return NULL;
	}

	lock->lk_name = kstrdup(name);
	if (lock->lk_name == NULL) {
		kfree(lock);
		return NULL;
	}

	HANGMAN_LOCKABLEINIT(&lock->lk_hangman, lock->lk_name);

	// add stuff here as needed
	lock->is_held = false;
	lock->lk_holder = NULL;
	spinlock_init(&lock->lk_spinlock);
	lock->lk_wchan = wchan_create(lock->lk_name);
	return lock;
}

void
lock_destroy(struct lock *lock)
{
	KASSERT(lock != NULL);

	// add stuff here as needed
	if(lock->is_held)
	{
		panic("lock is held, can't destroy");
	}

	spinlock_cleanup(&lock->lk_spinlock);
	kfree(lock->lk_name);
	wchan_destroy(lock->lk_wchan);
	kfree(lock);
}

void
lock_acquire(struct lock *lock)
{
	KASSERT(lock != NULL);

	KASSERT(curthread->t_in_interrupt == false);


	spinlock_acquire(&lock->lk_spinlock);
	HANGMAN_WAIT(&curthread->t_hangman, &lock->lk_hangman);

	if(lock->lk_holder == curthread)
	{
		return;
	}

	//Block until lock is not held
	while(lock->is_held == true)
	{
		wchan_sleep(lock->lk_wchan, &lock->lk_spinlock);
	}

	lock->is_held = true;
	lock->lk_holder = curthread;
	/* Call this (atomically) once the lock is acquired */
	HANGMAN_ACQUIRE(&curthread->t_hangman, &lock->lk_hangman);
	spinlock_release(&lock->lk_spinlock);
}

void
lock_release(struct lock *lock)
{
	KASSERT(lock != NULL);

	spinlock_acquire(&lock->lk_spinlock);

	//If the thread is not the holder, it can't release the lock
	KASSERT(lock->lk_holder == curthread);

	//If the lock is not held, nothing to do
	if(!lock->is_held)
	{
		spinlock_release(&lock->lk_spinlock);
		return;
	}

	lock->is_held = false;
	lock->lk_holder = NULL;
	wchan_wakeone(lock->lk_wchan, &lock->lk_spinlock);
	/* Call this (atomically) when the lock is released */
	HANGMAN_RELEASE(&curthread->t_hangman, &lock->lk_hangman);
	spinlock_release(&lock->lk_spinlock);
}

bool
lock_do_i_hold(struct lock *lock)
{
	KASSERT(curthread != NULL);
	return (lock->lk_holder == curthread);
}

////////////////////////////////////////////////////////////
//
// CV


struct cv *
cv_create(const char *name)
{
	struct cv *cv;

	cv = kmalloc(sizeof(*cv));
	if (cv == NULL) {
		return NULL;
	}

	cv->cv_name = kstrdup(name);
	if (cv->cv_name==NULL) {
		kfree(cv);
		return NULL;
	}

	// add stuff here as needed

	cv->cv_wchan = wchan_create(name);
	spinlock_init(&cv->cv_spinlock);

	return cv;
}

void
cv_destroy(struct cv *cv)
{
	KASSERT(cv != NULL);

	// add stuff here as needed

	kfree(cv->cv_name);
	wchan_destroy(cv->cv_wchan);
	spinlock_cleanup(&cv->cv_spinlock);
	kfree(cv);
}

void
cv_wait(struct cv *cv, struct lock *lock)
{
	// Write this
	KASSERT(lock_do_i_hold(lock));
	spinlock_acquire(&cv->cv_spinlock);

	lock_release(lock);
	wchan_sleep(cv->cv_wchan, &cv->cv_spinlock);

	spinlock_release(&cv->cv_spinlock);
	lock_acquire(lock);
}

void
cv_signal(struct cv *cv, struct lock *lock)
{
	// Write this
	KASSERT(lock_do_i_hold(lock));
	spinlock_acquire(&cv->cv_spinlock);
	wchan_wakeone(cv->cv_wchan, &cv->cv_spinlock);
	spinlock_release(&cv->cv_spinlock);
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
	// Write this
	KASSERT(lock_do_i_hold(lock));
	spinlock_acquire(&cv->cv_spinlock);
	wchan_wakeall(cv->cv_wchan, &cv->cv_spinlock);
	spinlock_release(&cv->cv_spinlock);
}

struct rwlock *
rwlock_create(const char *name)
{
	struct rwlock *rwlock = kmalloc(sizeof(*rwlock));
	if(rwlock == NULL)
	{
		return NULL;
	}

	rwlock->rwlock_name = kstrdup(name);
	if(rwlock->rwlock_name == NULL)
	{
		kfree(rwlock);
		return NULL;
	}

	rwlock->reader_wchan = wchan_create(name);
	if(rwlock->reader_wchan == NULL)
	{
		kfree(rwlock->rwlock_name);
		kfree(rwlock);
		return NULL;
	}

	rwlock->writer_wchan = wchan_create(name);
	if(rwlock->writer_wchan == NULL)
	{
		kfree(rwlock->rwlock_name);
		wchan_destroy(rwlock->reader_wchan);
		kfree(rwlock);
		return NULL;
	}

	spinlock_init(&rwlock->rwlock_spinlock);
	rwlock->rwlock_writer_thrd = NULL;
	rwlock->reader_count = 0;
	rwlock->writer_count = 0;
	return rwlock;
}

void
rwlock_destroy(struct rwlock *rwlock)
{
	KASSERT(rwlock != NULL);
	KASSERT(rwlock->reader_count == 0 && rwlock->writer_count == 0);

	kfree(rwlock->rwlock_name);
	spinlock_cleanup(&rwlock->rwlock_spinlock);
	wchan_destroy(rwlock->reader_wchan);
	wchan_destroy(rwlock->writer_wchan);
	kfree(rwlock);
}

void
rwlock_acquire_read(struct rwlock *rwlock)
{
	KASSERT(rwlock!= NULL);

	spinlock_acquire(&rwlock->rwlock_spinlock);
	while(rwlock->writer_count != 0)
	{
		//If a writer is waiting, no reader can acquire this lock
		wchan_sleep(rwlock->reader_wchan, &rwlock->rwlock_spinlock);
	}

	rwlock->reader_count++;
	spinlock_release(&rwlock->rwlock_spinlock);
}

void
rwlock_release_read(struct rwlock *rwlock)
{
	KASSERT(rwlock != NULL);

	spinlock_acquire(&rwlock->rwlock_spinlock);
	rwlock->reader_count--;

	//If any writers are waiting, wake one
	if(rwlock->reader_count == 0)
	{
		wchan_wakeone(rwlock->writer_wchan, &rwlock->rwlock_spinlock);
	}
	spinlock_release(&rwlock->rwlock_spinlock);
}

void
rwlock_acquire_write(struct rwlock *rwlock)
{
	KASSERT(rwlock != NULL);

	spinlock_acquire(&rwlock->rwlock_spinlock);

	//If other writers are holding the lock, sleep
	while(rwlock->writer_count != 0)
	{
		wchan_sleep(rwlock->writer_wchan, &rwlock->rwlock_spinlock);
	}

	//If readers are holding the lock, sleep
	while(rwlock->reader_count != 0)
	{
		wchan_sleep(rwlock->writer_wchan, &rwlock->rwlock_spinlock);
	}

	rwlock->writer_count++;
	rwlock->rwlock_writer_thrd = curthread;
	spinlock_release(&rwlock->rwlock_spinlock);
}

void
rwlock_release_write(struct rwlock *rwlock)
{
	KASSERT(rwlock != NULL);

	spinlock_acquire(&rwlock->rwlock_spinlock);
	KASSERT(rwlock->rwlock_writer_thrd == curthread);

	rwlock->writer_count--;

	//If writers are waiting, wake one
	if(rwlock->writer_count > 0)
	{
		wchan_wakeone(rwlock->writer_wchan, &rwlock->rwlock_spinlock);
		spinlock_release(&rwlock->rwlock_spinlock);
		return;
	}

	//Here no writers should be waiting, but still making sure
	//Wake all readers if no writers left
	if(rwlock->reader_count > 0 && rwlock->writer_count == 0)
	{
		wchan_wakeall(rwlock->reader_wchan, &rwlock->rwlock_spinlock);
		spinlock_release(&rwlock->rwlock_spinlock);
		return;
	}

	spinlock_release(&rwlock->rwlock_spinlock);
}
