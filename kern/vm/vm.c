/*
 * Author: Pratyush Yadav
 */

#include <types.h>
#include <limits.h>
#include <vm.h>
#include <spl.h>
#include <lib.h>
#include <spinlock.h>

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

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
  /* Will implement this with page tables and address spaces. Not needed now. */
  (void) faulttype;
  (void) faultaddress;
  return 0;
}
