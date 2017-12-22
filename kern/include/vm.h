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

#ifndef _VM_H_
#define _VM_H_

/*
 * VM system-related definitions.
 *
 * You'll probably want to add stuff here.
 */


#include <machine/vm.h>
#include <spinlock.h>
/* Fault-type arguments to vm_fault() */
#define VM_FAULT_READ        0    /* A read was attempted */
#define VM_FAULT_WRITE       1    /* A write was attempted */
#define VM_FAULT_READONLY    2    /* A write to a readonly page was attempted*/

/* The maximum size of user stack can be 2M. */
#define USERSTACK_SIZE 2 * 1024 * 1024
/* The lowest possible address of the stack. */
#define USERSTACK_BASE USERSTACK - STACK_SIZE

struct coremapentry {
 struct addrspace *cme_as;  /* Address space this page belongs to. */
 vaddr_t cme_vaddr;  /* Virtual address of this page. */

 /*
  * Info uses some bitwise arithmetic to encode more information using less
  * memory. Use the provided macros to decode or encode. An info field is
  * laid out as(1st bit is the least significant bit, and so on):
  *   1st bit - Whether the page is currently allocated or not.
  *   2nd bit - Whether the page is a part of a contiguous allocation or not
  *             A contiguous allocation is usually made by alloc_kpages so we
  *             need to keep track of this information so we can free the
  *             correct pages when free_kpages is called. All pages that are
  *             allocated alone have this bit clear. Only pages that are
  *             allocated in a batch have this bit set(except the starting page
  *             of that allocation). So, when freeing pages, as soon as we hit
  *             a page after the start page that has this bit clear, all pages
  *             of that allocation are freed.
  *   3rd bit - Write permission. A page is always readable, but not always
  *             writeable.
  *   Top 20 bits - Physical address of the page. Top 20 bits of a vaddr are the
  *                 start address of the page, rest 12 bits are offset into the
  *                 page.
  *   Remaining 10 bits - Currently unused.
  */
 int cme_info;
};

/*
 * The coremap, which is an array of coremapentry structures. Initialized by
 * coremap_bootstrap(). It does not map pages it uses itself.
 */
struct coremap {
  struct coremapentry *map;
  unsigned int cm_npages;  /* Number of free pages after coremap initialization. */
  unsigned int cm_nfreepages;  /* Number of pages currently not allocated. */
  paddr_t cm_firstpaddr; /* Address of the first usable page mapped by the coremap. */
  paddr_t cm_lastpaddr;  /* Last possible address in the RAM. */
  struct spinlock cm_lock; /* Spinlock for synchronized operations. */
};

extern struct coremap *kcoremap;

/* Coremap entry information encoding. x has to be 0 or 1. */
#define _MKINFW(x)      ((x)<<2) /* Encode whether the page is writeable or not. */
#define _MKINFCONTIG(x) ((x)<<1)  /* Encode whether the page is a part of a contiguous allocation or not. */
#define _MKINFALLOC(x) (x)  /*Encode whether page is currently allocated or not. */
#define _MKINFPNUM(p)  ((p)<<12) /* Encode the page number of the page. */

/*
 * Set the properties of the info field. The physical page number always remains
 * the same, so just zero out the rest of the bits and set the new properties.
 * Usage: info = CME_SETINF(info, 1, 0, 1);
 * This sets the page as writeable, allocated and not a part of a contiguous
 * allocation. Here, info is the old info of this page.
 */
#define CME_SETINF(info, alloc, contig, w) (((info)&PAGE_FRAME) | \
              _MKINFALLOC(alloc) | _MKINFCONTIG(contig) | _MKINFW(w))
/*
 * Set individual properties. The following code is very hacky, but to save
 * memory, it has to be done. Bitwise arithmetic is always ugly. To set
 * individual properties, we call CME_SETINF with all of the previous values
 * except the one we want to change.
 */

/* Set allocated. */
#define CME_SETINFALLOC(info, alloc)   (CME_SETINF((info), alloc, CME_ISCONTIG(info), CME_ISWRITE(info)))
/* Set contiguous. */
#define CME_SETINFCONTIG(info, contig) (CME_SETINF((info), CME_ISALLOC(info), (contig), CME_ISWRITE(info)))
/* Set writeable. */
#define CME_SETWRITE(info, w)          (CME_SETINF((info), CME_ISALLOC(info), CME_ISCONTIG(info), w))

/* Decode the coremap entry's info field. */
#define CME_ISALLOC(x)  ((x)&1)  /* Is the coremap entry page allocated? */
#define CME_ISCONTIG(x) ((x)&2) /* Is the coremap entry page a part of a contiguous allocation? */
#define CME_ISWRITE(x)  ((x)&4) /* Is the coremap entry page writeable. */
#define CME_PADDR(x)    ((x)&PAGE_FRAME)  /* Physical address of the page. */
#define CME_PNUM(x)     ((x)>>12)  /* Page number of the page. */

/* Calculate the index into the coremap array from physical address of page. */
#define CMINDEX_FROM_PADDR(paddr) (((paddr) - (kcoremap->cm_firstpaddr))/PAGE_SIZE)

/* Convert a physical address to it's page number. */
#define PADDR_TO_PNUM(x)  ((x)>>12)

/* Allocate physical pages. */
vaddr_t cm_getkpages(unsigned npages);

/* Free up a single physical page. */
void cm_freekpage(unsigned int index);

/*
 * Allocate a userspace page belonging to the address space AS. VADDR is used to
 * store in the coremap entry. Returns the physical address of the page. Returns
 * 0 on error.
 */
paddr_t cm_allocupage(struct addrspace *as, vaddr_t vaddr);

/* Free up a userspace page. */
int cm_freeupage(paddr_t paddr);

/* Copy the contents of the SRC page to DEST page. */
int cm_copypage(paddr_t src, paddr_t dest);

/* Initialization function */
void vm_bootstrap(void);

/* Fault handling function called by trap code */
int vm_fault(int faulttype, vaddr_t faultaddress);

/* Allocate/free kernel heap pages (called by kmalloc/kfree) */
vaddr_t alloc_kpages(unsigned npages);
void free_kpages(vaddr_t addr);

/*
 * Return amount of memory (in bytes) used by allocated coremap pages.  If
 * there are ongoing allocations, this value could change after it is returned
 * to the caller. But it should have been correct at some point in time.
 */
unsigned int coremap_used_bytes(void);

/* TLB shootdown handling called from interprocessor_interrupt */
void vm_tlbshootdown(const struct tlbshootdown *);


#endif /* _VM_H_ */
