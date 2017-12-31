/*
 * Author: Pratyush Yadav
 */

#include <types.h>
#include <limits.h>
#include <vm.h>
#include <spl.h>
#include <lib.h>
#include <spinlock.h>
#include <current.h>
#include <proc.h>
#include <kern/errno.h>
#include <pagetable.h>
#include <addrspace.h>
#include <machine/tlb.h>

struct coremap *kcoremap;

void
vm_bootstrap(void)
{
  paddr_t firstpaddr, lastpaddr;
  vaddr_t currentaddr;
  int ncoremappages, pagesfree;

  lastpaddr = ram_getsize();
  firstpaddr = ram_getfirstfree(); /* To calculate available ram. */

  KASSERT(firstpaddr%PAGE_SIZE == 0); /* firstpaddr must be page-aligned. */

  pagesfree = (lastpaddr - firstpaddr)/PAGE_SIZE; /* Number of pages free. */

  /* Calculate how many pages we need to store our coremap. */
  ncoremappages = ((sizeof(struct coremapentry)*pagesfree) +
                sizeof(struct coremap));

  /*
   * Since allocations need to be page-aligned, if total memory needed does not
   * fit exactly in integral number of pages, we need an extra page to store
   * that remaining data, hence we add one extra page. If it fits exactly, no
   * need to add an extra page.
   */
  if(ncoremappages%PAGE_SIZE != 0) {
    ncoremappages = ncoremappages/PAGE_SIZE + 1;
  }
  else {
    ncoremappages = ncoremappages/PAGE_SIZE;
  }

  currentaddr = PADDR_TO_KVADDR(firstpaddr)
  /* Start coremap at the first free address. */;
  kcoremap = (struct coremap *)currentaddr;
  currentaddr += sizeof(*kcoremap);
  kcoremap->map = (struct coremapentry *)currentaddr;  /* Start of the coremap's array. */
  currentaddr += sizeof(struct coremapentry)*(pagesfree - ncoremappages);

  /* Other coremap initialization. */
  kcoremap->cm_npages = pagesfree - ncoremappages;
  kcoremap->cm_nfreepages = kcoremap->cm_npages;
  kcoremap->cm_firstpaddr = firstpaddr + ncoremappages*PAGE_SIZE;
  kcoremap->cm_lastpaddr = lastpaddr;
  spinlock_init(&kcoremap->cm_lock);

  /* Initialize all coremap entries. */
  int info;
  paddr_t pageaddr;
  for(unsigned int i = 0; i < kcoremap->cm_npages; i++) {
    pageaddr = kcoremap->cm_firstpaddr + i*PAGE_SIZE;

    /*
     * Encodes the page number of the page into info, and sets read, write,
     * execute, alloc to 0.
     */
    info = (int)pageaddr & PAGE_FRAME;

    kcoremap->map[i].cme_as = NULL;
    kcoremap->map[i].cme_vaddr = 0;
    kcoremap->map[i].cme_info = info;
  }
}

/*
 * Allocates contiguous physical pages. If the number of available pages is
 * less than the number of required pages, or if the pages are not available
 * contiguously, return 0.
 */
vaddr_t
cm_getkpages(unsigned npages)
{
  if(npages == 0) {
    return 0;
  }

  spinlock_acquire(&kcoremap->cm_lock);
  if(kcoremap->cm_nfreepages < npages) {
    spinlock_release(&kcoremap->cm_lock);
    return 0;
  }

  unsigned int start = 0;
  unsigned ncontigpages = 0; /* Number of contiguous we found. */
  for(unsigned int i = 0; i < kcoremap->cm_npages; i++) {
    /* If the page is allocated, continue. */
    if(CME_ISALLOC(kcoremap->map[i].cme_info)) {
      ncontigpages = 0;
      continue;
    }

    /*
     * Set the start index. We will return its page address if we find n
     * contiguous pages starting here.
     */
    if(ncontigpages == 0) {
      start = i;
    }
    ncontigpages++;
    if(ncontigpages == npages) {
      break;
    }
  }

  /* Unable to find n contiguous pages. */
  if(ncontigpages != npages) {
    spinlock_release(&kcoremap->cm_lock);
    return 0;
  }

  for(unsigned int i = start; i < start + ncontigpages; i++) {
    /*
     * Set the page as allocated and writeable. If it is the first page of the
     * allocation, clear its contig bit and set that bit for the rest of the
     * pages.
     */
    if(i == start) {
      kcoremap->map[i].cme_info = CME_SETINF(kcoremap->map[i].cme_info, 1, 0, 1);
    }
    else {
      kcoremap->map[i].cme_info = CME_SETINF(kcoremap->map[i].cme_info, 1, 1, 1);
    }
    /* Set the virtual address of this page. */
    kcoremap->map[i].cme_vaddr = PADDR_TO_KVADDR(CME_PADDR(kcoremap->map[i].cme_info));
    kcoremap->map[i].cme_as = NULL; /* Address space of the kernel is NULL. */
  }

  kcoremap->cm_nfreepages -= npages; /* Update the free page count. */
  spinlock_release(&kcoremap->cm_lock);

  /* Convert the physical address of the page to virtual address and return. */
  return PADDR_TO_KVADDR(CME_PADDR(kcoremap->map[start].cme_info));
}

void
cm_freekpage(unsigned int index)
{
  /* Should be a valid index. */
  KASSERT(index < kcoremap->cm_npages);
  KASSERT(index > 0);

  /* Set the page as unallocated. This basically frees it up. */
  kcoremap->map[index].cme_info = CME_SETINFALLOC(kcoremap->map[index].cme_info, 0);
  kcoremap->cm_nfreepages++; /* Update the free page count. */
}

paddr_t
cm_allocupage(struct addrspace *as, vaddr_t vaddr)
{
  /* as must be a valid address space. */
  KASSERT(as != NULL);
  /* vaddr should be a valid page address. */
  KASSERT((vaddr & PAGE_FRAME) == vaddr);

  paddr_t paddr = 0;
  int info;

  spinlock_acquire(&kcoremap->cm_lock);

  /*
   * We currently don't support swapping, so return error when physical
   * memory is full.
   */
  if(kcoremap->cm_nfreepages == 0) {
    spinlock_release(&kcoremap->cm_lock);
    return 0;
  }

  /* Get a free page. */
  for(unsigned i = 0; i < kcoremap->cm_npages; i++) {
    info = kcoremap->map[i].cme_info;
    if(CME_ISALLOC(info)) {
      continue;
    }
    /* Found a free page. Get its physical address. */
    paddr = CME_PADDR(info);

    /* Set up the coremap entry. */
    info = CME_SETINFALLOC(info, 1);
    info = CME_SETINFCONTIG(info, 0);
    info = CME_SETWRITE(info, 1);
    kcoremap->map[i].cme_info = info;

    kcoremap->map[i].cme_as = as;
    kcoremap->map[i].cme_vaddr = vaddr;
    break;
  }

  /* Update the coremap fields. */
  kcoremap->cm_nfreepages--;

  spinlock_release(&kcoremap->cm_lock);

  /*
   * We already checked if a free page is available. If the above loop failed to
   * find a free page, that's some error in the coremap's information. We have
   * nothing to do but panic.
   */
  KASSERT(paddr != 0);
  return paddr;
}

int
cm_freeupage(paddr_t paddr)
{
  /* Get the index into the coremap. */
  unsigned int index = CMINDEX_FROM_PADDR(paddr);
  spinlock_acquire(&kcoremap->cm_lock);

  /* Make sure it's a valid coremap index. */
  if(index >= kcoremap->cm_nfreepages) {
    spinlock_release(&kcoremap->cm_lock);
    return EINVAL;
  }

  /* This page must belong to the process freeing it. */
  if(kcoremap->map[index].cme_as != curproc->p_addrspace) {
    spinlock_release(&kcoremap->cm_lock);
    return EPERM;
  }

  /* Free the page up. */
  kcoremap->map[index].cme_as = NULL;
  kcoremap->map[index].cme_vaddr = 0;

  int info = kcoremap->map[index].cme_info;
  info = CME_SETWRITE(info, 0); /* Mark the page as not writeable. */
  info = CME_SETINFALLOC(info, 0); /* Mark the page as not allocated. */
  /*
   * Mark the page as not a part of a contiguous allocation. Userspace pages
   * never are, but let's just make sure.
   */
  info = CME_SETINFCONTIG(info, 0);
  kcoremap->map[index].cme_info = info;

  /* Update the number of free pages. */
  kcoremap->cm_nfreepages++;

  spinlock_release(&kcoremap->cm_lock);
  return 0;
}

int
cm_copypage(paddr_t src, paddr_t dest)
{
  unsigned srcindex, destindex;
  srcindex = CMINDEX_FROM_PADDR(src);
  destindex = CMINDEX_FROM_PADDR(dest);

  /* Check if src and dest are page-aligned addresses. */
  if(src % PAGE_SIZE != 0 || dest % PAGE_SIZE != 0) {
    return EINVAL;
  }

  /* Check if src and dest both are pages managed by the coremap. */
  if(src < kcoremap->cm_firstpaddr || dest < kcoremap->cm_firstpaddr) {
    return EINVAL;
  }
  if(srcindex >= kcoremap->cm_npages || destindex >= kcoremap->cm_npages) {
    return EINVAL;
  }

  /* Check of dest is allocated. */
  if(!CME_ISALLOC(kcoremap->map[destindex].cme_info)) {
    return EFAULT;
  }

  /* Check if dest is writeable. */
  if(!CME_ISWRITE(kcoremap->map[destindex].cme_info)) {
    return EPERM;
  }

  /* Copy the contents. */
  memcpy((void *)PADDR_TO_KVADDR(dest), (void *)PADDR_TO_KVADDR(src), PAGE_SIZE);
  return 0;
}

vaddr_t
alloc_kpages(unsigned npages)
{
  if(kcoremap->cm_nfreepages < npages) {
    return 0;
  }
  else {
    return cm_getkpages(npages);
  }
}

void
free_kpages(vaddr_t addr)
{
  /* Addr is not the starting address of a page. */
  if(addr%PAGE_SIZE != 0) {
    return;
  }

  spinlock_acquire(&kcoremap->cm_lock);

  /* addr should be an adress mapped by the coremap. */
  if(addr < kcoremap->cm_firstpaddr) {
    spinlock_release(&kcoremap->cm_lock);
    return;
  }

  paddr_t paddr = KVADDR_TO_PADDR(addr);

  unsigned index = CMINDEX_FROM_PADDR(paddr); /* Get the index into coremap array. */

  /* If the page is not allocated, return. */
  if(!CME_ISALLOC(kcoremap->map[index].cme_info)) {
    spinlock_release(&kcoremap->cm_lock);
    return;
  }

  cm_freekpage(index); /* Free up the first page. */
  index++;

  /* Free up the rest of the pages. */
  int info = kcoremap->map[index].cme_info;
  while(CME_ISALLOC(info) && CME_ISCONTIG(info) && index < kcoremap->cm_npages)
  {
    cm_freekpage(index);
    index++;
    info = kcoremap->map[index].cme_info;
  }
  spinlock_release(&kcoremap->cm_lock);
}

unsigned int
coremap_used_bytes(void)
{
  return (kcoremap->cm_npages - kcoremap->cm_nfreepages)*PAGE_SIZE;
}

void
vm_tlbshootdown(const struct tlbshootdown *tsd)
{
  /* Will be implemented later. */
  (void)tsd;
}

/* Load the TLB with the translation of pageaddr. */
static
int
vm_loadtlb(struct addrspace *as, vaddr_t faultaddr)
{
  struct pagetable *pgt;
  struct pagetableentry *pte;
  int spl;
  vaddr_t pageaddr;
  uint32_t ehi, elo;

  if(as == NULL) {
    /* The process is the kernel. But KSEG2 is not used as of now so we panic. */
    panic("vm_loadtlb: kseg2 address used\n");
    return 0;
  }

  pgt = as->as_pgtable;
  KASSERT(pgt != NULL);  /* The process must have a valid page table. */

  /* Get the address of the page where the fault occured. */
  pageaddr = faultaddr & PAGE_FRAME;

  pte = pagetable_getentry(pgt, pageaddr);
  if(pte == NULL) {
    /* The page is not allocated. */
    return EFAULT;
  }

  /* If the page is not allocated on physical memory, allocate it (lazy allocation). */
  if(pte->pte_phyaddr == 0) {
    pte->pte_phyaddr = cm_allocupage(as, pageaddr);
    if(pte->pte_phyaddr == 0) {
      return ENOMEM;
    }
  }

  /* Disable interrupts while handling the TLB. */
  spl = splhigh();

  /* Load the translation into the TLB. */
  ehi = pageaddr & TLBHI_VPAGE;
  elo = (pte->pte_phyaddr & TLBLO_PPAGE) | TLBLO_VALID | TLBLO_DIRTY;
  tlb_random(ehi, elo);

  /* Re-enable interrupts. */
  splx(spl);

  return 0;
}
int
vm_fault(int faulttype, vaddr_t faultaddress)
{
  int result = 0;

  switch(faulttype) {
    case VM_FAULT_READ:
    case VM_FAULT_WRITE:
      result = vm_loadtlb(curproc->p_addrspace, faultaddress);
      break;
    case VM_FAULT_READONLY:
      /* We always create pages read-write, so we can't get this */
      panic("dumbvm: got VM_FAULT_READONLY\n");
      break;
    default:
      return EINVAL;
  }

  if(result) {
    return result;
  }

  return 0;
}
