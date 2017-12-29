/*
 * Author: Pratyush Yadav
 */

#ifndef _PAGETABLE_H_
#define _PAGETABLE_H_

struct spinlock;

struct pagetableentry {
  /* Add stuff here. */
  vaddr_t pte_pageaddr;  /* The virtual address of the page. */
  paddr_t pte_phyaddr;  /* The physical address of the page. */
};

/* The page table structure. */
struct pagetable {
  /*
   * Pointer to the first level array. Each element in first level array points
   * to an array of pagetableentry pointers. That array is the second level
   * of the page table.
   */
  struct pagetableentry ***pgt_firstlevel;
  unsigned int pgt_nallocpages;  /* Number of allocated pages. */
  struct spinlock pgt_spinlock;
};

/*
 * Number of entries in a level of the multi-level array. Since the page number
 * is a 20-bit number and we are using 2 level arrays, top 10 bits of the page
 * number are index in the first level array and next 10 bits in second level
 * array. So, each array will be of the size of 2^10 = 0x400.
 */
#define PGT_ENTRIESINALEVEL 0x00000400

/* Get the index into the first level of the page table. */
#define PGT_FIRSTLEVELMASK 0xFFC00000 /* The top 10 bits are 1s, rest 0s. */
/* Get the index into second level of the page table. */
#define PGT_SECONDLEVELMASK 0x003FF000 /* Bits 11-20 are 1s, rest are 0s. */

/*
 * Use these macros to get the index into the first or second level of the
 * page table. X is the virtual address of the page to be referenced.
 */
#define PGT_GETFIRSTLVLINDEX(x)  (((x)&PGT_FIRSTLEVELMASK) >> 22)
#define PGT_GETSECONDLVLINDEX(x)  (((x)&PGT_SECONDLEVELMASK) >> 12)

/* Create a new pagetable. */
struct pagetable * pagetable_create(void);

/* Destroy a page table. All the pages must be free before calling this. */
void pagetable_destroy(struct pagetable *);

/* Allocate a page starting at addr. addr must be page-aligned. */
int pagetable_allocpage(vaddr_t addr);

/* Free the page at addr, if allocated. addr must be page-aligned. */
int pagetable_freepage(vaddr_t addr);

/*
 * Get the page table entry corresponding to ADDR of the given address
 * space. Returns NULL when the page is not allocated.
 */
struct pagetableentry * pagetable_getentry(struct pagetable *, vaddr_t addr);

/*
 * Copy the OLD page table into RET. The new page table's pages are assigned to
 * the address space NEWAS.
 */
int pagetable_copy(struct pagetable *old, struct addrspace *newas,
                struct pagetable **ret);

#endif  /* _PAGETABLE_H_ */
