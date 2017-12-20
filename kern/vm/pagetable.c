/*
 * Author: Pratyush Yadav
 */

#include <types.h>
#include <lib.h>
#include <vm.h>
#include <pagetable.h>
#include <spinlock.h>
#include <current.h>
#include <addrspace.h>
#include <proc.h>
#include <kern/errno.h>

/////////////////////////////////////////////
//  Internal

/* Create the second level array for the given first level index. */
static
int
pagetable_createsecondlvl(struct pagetable *pgt, unsigned firstlvlindex)
{
  /* The second level array must not be already created. */
  KASSERT(pgt->pgt_firstlevel[firstlvlindex] == NULL);

  pgt->pgt_firstlevel[firstlvlindex] = kmalloc(sizeof(struct pagetableentry *)*
                         PGT_ENTRIESINALEVEL);

  if(pgt->pgt_firstlevel[firstlvlindex] == NULL) {
    return ENOMEM;
  }
  for(int i = 0; i < PGT_ENTRIESINALEVEL; i++) {
    pgt->pgt_firstlevel[firstlvlindex][i] = NULL;
  }
  return 0;
}

/////////////////////////////////////////////
//  Public

struct pagetable *
pagetable_create()
{
  struct pagetable *pgt = kmalloc(sizeof(*pgt));
  if(pgt == NULL) {
    return NULL;
  }

  pgt->pgt_nallocpages = 0;
  spinlock_init(&pgt->pgt_spinlock);

  /* Initialize the 2-level array. */
  pgt->pgt_firstlevel = kmalloc(sizeof(struct pagetableentry **)*PGT_ENTRIESINALEVEL);
  if(pgt->pgt_firstlevel == NULL) {
    spinlock_cleanup(&pgt->pgt_spinlock);
    kfree(pgt);
    return NULL;
  }

  for(int i = 0; i < PGT_ENTRIESINALEVEL; i++) {
    /*
     * Each entry is NULL at the start. Once a page is to be allocated, we will
     * allocate the second level array when required.
     */
    pgt->pgt_firstlevel[i] = NULL;
  }

  return pgt;
}

/*
 * The address has to belong to a valid segment. We don't check for that here,
 * that is the job of the address space function that calls this function.
 */
int
pagetable_allocpage(vaddr_t addr)
{
  struct pagetable *pgt = curproc->p_addrspace->as_pgtable;
  KASSERT(pgt != NULL);
  /* Index into the first level array. */
  unsigned int firstlvlindex = addr & PGT_FIRSTLEVELMASK;
  /* Index into the second level array. */
  unsigned int secondlvlindex = addr & PGT_SECONDLEVELMASK;
  int result;

  spinlock_acquire(&pgt->pgt_spinlock);

  /* If the second level table is not yet allocated, allocate it now. */
  if(pgt->pgt_firstlevel[firstlvlindex] == NULL) {
    result = pagetable_createsecondlvl(pgt, firstlvlindex);
    if(result) {
      spinlock_release(&pgt->pgt_spinlock);
      return result;
    }
  }

  /* The page must not be already allocated. */
  if(pgt->pgt_firstlevel[firstlvlindex][secondlvlindex] != NULL) {
    spinlock_release(&pgt->pgt_spinlock);
    return EFAULT;
  }

  /* Create the pagetable entry. */
  struct pagetableentry *pte = kmalloc(sizeof(*pte));
  if(pte == NULL) {
    spinlock_release(&pgt->pgt_spinlock);
    return ENOMEM;
  }

  pte->pte_pageaddr = addr;
  /*
   * Allocate lazily. Unless the page is accessed, don't allocate it on
   * physical memory.
   */
  pte->pte_phyaddr = 0;

  pgt->pgt_firstlevel[firstlvlindex][secondlvlindex] = pte;
  spinlock_release(&pgt->pgt_spinlock);
  return 0;
}

int
pagetable_freepage(vaddr_t addr)
{
  struct pagetable *pgt = curproc->p_addrspace->as_pgtable;
  KASSERT(pgt != NULL);
  /* Index into the first level array. */
  unsigned int firstlvlindex = addr & PGT_FIRSTLEVELMASK;
  /* Index into the second level array. */
  unsigned int secondlvlindex = addr & PGT_SECONDLEVELMASK;
  struct pagetableentry *pte;
  int result;

  spinlock_acquire(&pgt->pgt_spinlock);

  /* If the page has not been allocated, simply return. */
  if(pgt->pgt_firstlevel[firstlvlindex] == NULL) {
    spinlock_release(&pgt->pgt_spinlock);
    return 0;
  }

  if(pgt->pgt_firstlevel[firstlvlindex][secondlvlindex] == NULL) {
    spinlock_release(&pgt->pgt_spinlock);
    return 0;
  }

  /* The page is allocated. Free it. */
  pte = pgt->pgt_firstlevel[firstlvlindex][secondlvlindex];

  /* Make sure the page table entry is not corrupted in some weird way. */
  KASSERT(pte->pte_pageaddr == addr);
  paddr_t paddr = pte->pte_phyaddr;
  spinlock_release(&pgt->pgt_spinlock);

  /* If the page was not allocated on physical memory, nothing else to do. */
  if(paddr == 0) {
    return 0;
  }

  /* Free the page from physical memory. */
  result = cm_freeupage(paddr);
  if(result) {
    return result;
  }
  return 0;
}
