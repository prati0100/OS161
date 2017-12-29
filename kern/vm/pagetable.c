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

void
pagetable_destroy(struct pagetable *pgt)
{
  KASSERT(pgt != NULL);
  paddr_t paddr;

  /* Free up all the page table entries one by one. */
  for(int i = 0; i < PGT_ENTRIESINALEVEL; i++) {
    /* If the second level table does not exist, skip. */
    if(pgt->pgt_firstlevel[i] == NULL) {
      continue;
    }

    /* Free up each entry in the second level table. */
    for(int j = 0; j < PGT_ENTRIESINALEVEL; j++) {
      if(pgt->pgt_firstlevel[i][j] == NULL) {
        continue;
      }

      paddr = pgt->pgt_firstlevel[i][j]->pte_phyaddr;
      cm_freeupage(paddr);
      kfree(pgt->pgt_firstlevel[i][j]);
      pgt->pgt_nallocpages--;
    }
  }

  /* All pages must have been freed by now. */
  KASSERT(pgt->pgt_nallocpages == 0);

  spinlock_cleanup(&pgt->pgt_spinlock);
  kfree(pgt);
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
  pgt->pgt_nallocpages++;  /* Update the number of allocated pages. */
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
  pgt->pgt_firstlevel[firstlvlindex][secondlvlindex] = NULL;

  /* Make sure the page table entry is not corrupted in some weird way. */
  KASSERT(pte->pte_pageaddr == addr);
  paddr_t paddr = pte->pte_phyaddr;
  pgt->pgt_nallocpages--;  /* Update the number of allocated pages. */
  kfree(pte);
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

int
pagetable_copy(struct pagetable *old, struct addrspace *newas, struct pagetable
                **ret)
{
  KASSERT(old != NULL);
  KASSERT(newas != NULL);
  /* A single process can not have two page tables. */
  KASSERT(newas != curproc->p_addrspace);

  struct pagetable *new = pagetable_create();
  if(new == NULL) {
    return ENOMEM;
  }

  struct pagetableentry *temp;

  /* The lock to makes sure no one modifies the page table while we copy it. */
  spinlock_acquire(&old->pgt_spinlock);

  /* Copy all the pagetable entries one by one. */
  for(int i = 0; i < PGT_ENTRIESINALEVEL; i++) {
    /* If the second level array was not created, skip. */
    if(old->pgt_firstlevel[i] == NULL) {
      continue;
    }

    /* Create the second level array for the new page table. */
    pagetable_createsecondlvl(new, i);

    /* Copy each entry of the old second level array into the new one. */
    for(int j = 0; j < PGT_ENTRIESINALEVEL; j++) {
      if(old->pgt_firstlevel[i][j] != NULL) {
        temp = kmalloc(sizeof(struct pagetableentry *));
        /* If the allocation fails, clean up. */
        if(temp == NULL) {
          spinlock_release(&old->pgt_spinlock);
          pagetable_destroy(new);
          return ENOMEM;
        }
        temp->pte_pageaddr = old->pgt_firstlevel[i][j]->pte_pageaddr;
        temp->pte_phyaddr = cm_allocupage(newas, temp->pte_pageaddr);
        if(temp->pte_phyaddr == 0) {
          spinlock_release(&old->pgt_spinlock);
          pagetable_destroy(new);
          kfree(temp);
          return ENOMEM;
        }

        /* Copy the contents of the old page into the new one. */
        cm_copypage(old->pgt_firstlevel[i][j]->pte_phyaddr, temp->pte_phyaddr);
        new->pgt_firstlevel[i][j] = temp;
      }
    }
  }

  new->pgt_nallocpages = old->pgt_nallocpages;
  spinlock_release(&old->pgt_spinlock);

  *ret = new;
  return 0;
}

struct pagetableentry *
pagetable_getentry(struct pagetable *pgt, vaddr_t addr)
{
  /* Index into the first level array. */
  unsigned int firstlvlindex = addr & PGT_FIRSTLEVELMASK;
  /* Index into the second level array. */
  unsigned int secondlvlindex = addr & PGT_SECONDLEVELMASK;

  KASSERT(pgt != NULL);

  /* Check if the entry exists or not. */
  if(pgt->pgt_firstlevel[firstlvlindex] == NULL) {
    return NULL;
  }

  return pgt->pgt_firstlevel[firstlvlindex][secondlvlindex];
}
