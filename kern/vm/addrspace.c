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

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <pagetable.h>
#include <spl.h>
#include <machine/tlb.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 */

struct segment *
seg_create(vaddr_t start, size_t npages)
{
	struct segment *seg = kmalloc(sizeof(*seg));
	if(seg == NULL) {
		return NULL;
	}

	seg->seg_start = start;
	seg->seg_npages = npages;
	return seg;
}

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	/*
	 * Initialize as needed.
	 */

	as->as_pgtable = pagetable_create();
	if(as->as_pgtable == NULL) {
		kfree(as);
		return NULL;
	}

	/*
	 * Initialize the segment array. Initially it can store 4 segments (text,
	 * global, stack, heap). It can be resized when more segments are needed.
	 */
	segmentarray_init(&as->as_segarray);
	segmentarray_setsize(&as->as_segarray, 4);
	for(unsigned i = 0; i < segmentarray_num(&as->as_segarray); i++) {
		segmentarray_set(&as->as_segarray, i, NULL);
	}

	as->as_stack = NULL;
	as->as_heap = NULL;
	return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;
	struct segment *tempseg;
	int result;

	newas = as_create();
	if (newas==NULL) {
		return ENOMEM;
	}

	/* Copy the page table. */
	result = pagetable_copy(old->as_pgtable, newas, &newas->as_pgtable);
	if(result) {
		as_destroy(newas);
		return result;
	}

	/* Copy the segments. */
	segmentarray_setsize(&newas->as_segarray, segmentarray_num(&old->as_segarray));
	for(unsigned i = 0; i < segmentarray_num(&old->as_segarray); i++) {
		tempseg = kmalloc(sizeof(*tempseg));
		if(tempseg == NULL) {
			/* as_destroy() will clean up the previously allocated segments. */
			as_destroy(newas);
			return ENOMEM;
		}
		*tempseg = *(segmentarray_get(&old->as_segarray, i));
		segmentarray_set(&newas->as_segarray, i, tempseg);
	}

	/* Copy the heap and stack segments. */
	newas->as_heap = kmalloc(sizeof(struct segment));
	if(newas->as_heap == NULL) {
		as_destroy(newas);
		return ENOMEM;
	}
	*newas->as_heap = *old->as_heap;
	newas->as_stack = kmalloc(sizeof(struct segment));
	if(newas->as_stack == NULL) {
		as_destroy(newas);
		return ENOMEM;
	}
	*newas->as_stack = *old->as_stack;

	*ret = newas;
	return 0;
}

void
as_destroy(struct addrspace *as)
{
	KASSERT(as != NULL);

	/* Clean up the page table. This also frees up the pages allocated. */
	pagetable_destroy(as->as_pgtable);

	/*
	 * Clean up the segments. The pages were already freed from physical memory by
	 * pagetable_destroy(). Now just free up the segment structures. The stack and
	 * heap segments are also stored in the segment array. So they will be freen
	 * by this loop, no need for freeing them specifically.
	 */
	for(unsigned i = 0; i < segmentarray_num(&as->as_segarray); i++) {
		kfree(segmentarray_get(&as->as_segarray, i));
	}
	kfree(as);
}

void
as_activate(void)
{
	struct addrspace *as;
	int i, spl;

	as = proc_getas();
	if (as == NULL) {
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	/* Disable the interrupts while flushing the TLB. */
	spl = splhigh();

	/* Replace all TLB entries with invalid entries. */
	for(i = 0; i < NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl); /* Re-enable interrupts. */
}

void
as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
		 int readable, int writeable, int executable)
{
	/* TODO: Implement permissions. */
	(void)readable;
	(void)writeable;
	(void)executable;

	int segindex = -1, seg_npages;
	struct segment *seg;
	int result;

	KASSERT(as != NULL);

	if(vaddr >= USERSPACETOP) {
		return EFAULT;
	}

	/* Calculate the number of pages this segment needs. */
	seg_npages = ROUNDUP(memsize, PAGE_SIZE);

	/* Create and initialize the segment. */
	seg = seg_create(vaddr, seg_npages);
	if(seg == NULL) {
		return ENOMEM;
	}

	/* Check if there is an available entry in the segment array. */
	for(unsigned i = 0; i < segmentarray_num(&as->as_segarray); i++) {
		if(segmentarray_get(&as->as_segarray, i) == NULL) {
			segindex = i;
			break;
		}
	}

	/* If there was no free entry, extend the segment array. */
	if(segindex == -1) {
		result = segmentarray_setsize(&as->as_segarray,
			            segmentarray_num(&as->as_segarray)+1);
		if(result) {
			kfree(seg);
			return result;
		}
		segindex = segmentarray_num(&as->as_segarray) - 1;
	}

	/* Put the segment into the segment array. */
	segmentarray_set(&as->as_segarray, segindex, seg);

	/* Allocate the pages the segment spans. */
	for(int i = 0; i < seg_npages; i++) {
		pagetable_allocpage(vaddr + i*PAGE_SIZE);
	}
	return 0;
}

int
as_prepare_load(struct addrspace *as)
{
	/*
	 * This function is to do with permissions. Permissions haven't been
	 * implemented yet, so it is useless as of now.
	 */

	(void)as;
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	/*
	 * This function is to do with permissions. Permissions haven't been
	 * implemented yet, so it is useless as of now.
	 */

	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	struct segment *stackseg;
	int result;
	size_t stack_npages;

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	/* Calculate the maximum number of pages the stack can use. */
	stack_npages = USERSTACK_SIZE/PAGE_SIZE;

	/* Create the stack segment. */
	stackseg = seg_create(USERSTACK_BASE, stack_npages);
	if(stackseg == NULL) {
		return ENOMEM;
	}

	as->as_stack = stackseg;

	/* Add the stack segment to the segment array. */
	int segindex = -1;

	/* Check if there is an available entry in the segment array. */
	for(unsigned i = 0; i < segmentarray_num(&as->as_segarray); i++) {
		if(segmentarray_get(&as->as_segarray, i) == NULL) {
			segindex = i;
			break;
		}
	}

	/* If there was no free entry, extend the segment array. */
	if(segindex == -1) {
		result = segmentarray_setsize(&as->as_segarray,
			            segmentarray_num(&as->as_segarray)+1);
		if(result) {
			kfree(stackseg);
			return result;
		}
		segindex = segmentarray_num(&as->as_segarray) - 1;
	}

	/* Insert the segment into the array. */
	segmentarray_set(&as->as_segarray, segindex, stackseg);

	/* Allocate the pages the segment spans. */
	for(unsigned i = 0; i < stack_npages; i++) {
		pagetable_allocpage(USERSTACK_BASE + i*PAGE_SIZE);
	}
	return 0;
}
