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
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <elf.h>
#include <synch.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

Regions *
regions_create(void) {
    Regions *regions = kmalloc(sizeof(Regions));
    if (regions == NULL) {
        return NULL;
    }

    regions->head = NULL;
    regions->tail = NULL;

    return regions;
}

void
region_destroy(struct region *region) {
    KASSERT(region != NULL);
    region->next = NULL;
    region->prev = NULL;
    region->vbase = 0;
    region->npages = 0;
    region->vtop = 0;
    region->readable = 0;
    region->writeable = 0;
    region->executable = 0;
    region->type = UNNAMED_REGION;
#if OPT_MMAP
    region->fd = -1;
    region->offset = 0;
#endif
    kfree(region);
}

void
regions_destroy(Regions *regions) {
    KASSERT(regions != NULL);
    struct region *current = regions->head;
    while (current != NULL) {
        struct region *next = current->next;
        region_destroy(current);
        current = next;
    }
    regions->head = NULL;
    regions->tail = NULL;
    kfree(regions);
}

void
regions_insert(Regions *regions, struct region *region) {
    KASSERT(regions != NULL);
    KASSERT(region != NULL);

    if (regions->head == NULL) {
        KASSERT(regions->tail == NULL);
        regions->head = region;
        regions->tail = region;
    } else {
        KASSERT(regions->tail != NULL);
        regions->tail->next = region;
        region->prev = regions->tail;
        regions->tail = region;
    }
    KASSERT(regions->head != NULL);
    KASSERT(regions->tail != NULL);
}

void
regions_remove_region(Regions *regions, struct region *region) {
    KASSERT(regions != NULL);
    KASSERT(region != NULL);
    // make sure the region is in the list
    KASSERT(regions->head != NULL);
    KASSERT(regions->tail != NULL);

    // region's prev next must point to this region
    KASSERT(region->prev->next == region);
    // region's next prev must point to this region
    KASSERT(region->next->prev == region);

    if (region->prev != NULL) {
        region->prev->next = region->next;
    }
    if (region->next != NULL) {
        region->next->prev = region->prev;
    }

    if (regions->head == region) {
        regions->head = region->next;
    }
    if (regions->tail == region) {
        regions->tail = region->prev;
    }
}

static struct region *
region_copy(struct region *old) {
    struct region *new = kmalloc(sizeof(struct region));
    if (new == NULL) {
        return NULL;
    }

    new->vbase = old->vbase;
    new->npages = old->npages;
    new->vtop = old->vtop;
    new->readable = old->readable;
    new->writeable = old->writeable;
    new->executable = old->executable;

    if (old->next != NULL) {
        new->next = region_copy(old->next);
        if (new->next == NULL) {
            kfree(new);
            return NULL;
        }
    } else {
        new->next = NULL;
    }

    return new;
}

Regions *
regions_copy(Regions *old) {
    Regions *new = regions_create();
    if (new == NULL) {
        return NULL;
    }

    new->head = region_copy(old->head);
    if (new->head == NULL) {
        kfree(new);
        return NULL;
    }

    struct region *current = new->head;
    while (current->next != NULL) {
        current = current->next;
    }
    new->tail = current;

    return new;
}

struct region *
find_region(Regions *regions, vaddr_t vaddr) {
    struct region *current_region = regions->head;
    while (current_region != NULL) {
        if (current_region->vbase <= vaddr && vaddr < current_region->vtop) {
            return current_region;
        }
        current_region = current_region->next;
    }
    return NULL;
}

struct region *
find_region_by_vbase(Regions *regions, vaddr_t vbase) {
    struct region *current_region = regions->head;
    while (current_region != NULL) {
        if (current_region->vbase == vbase) {
            return current_region;
        }
        current_region = current_region->next;
    }
    return NULL;
}

#if OPT_MMAP
struct region *
alloc_file_region(struct addrspace *as, size_t memsize, int readable, int writeable, int executable) {
    size_t npages;

    // calcuate how many pages we need
    npages = (memsize + PAGE_SIZE - 1) / PAGE_SIZE;

    // since this is for file map, we are going to place this right under the first free space under the stack but above the heap

    // find heap first
    struct region *heap = find_region_by_vbase(as->all_regions, as->heap_start);
    KASSERT(heap != NULL);

    // start from the stack, searching backwards
    struct region *target = find_region_by_vbase(as->all_regions, as->stack_start);
    KASSERT(target != NULL);

    // now we have the stack, find the last region that is above the heap but is below the stack
    while (target != NULL) {
        if (target->vbase < heap->vtop) {
            break;
        }
        target = target->prev;
    }

    // now we have the region that is above the heap but below the stack
    target = target->prev;
    KASSERT(target != NULL);

    vaddr_t free_space_start = heap->vtop;
    vaddr_t free_space_end = target->vbase;

    // double check start and end are page aligned
    KASSERT(free_space_start % PAGE_SIZE == 0);
    KASSERT(free_space_end % PAGE_SIZE == 0);

    // how many pages, do they fit?
    if (npages * PAGE_SIZE > free_space_end - free_space_start) {
        return NULL;
    }

    // if we reach here, they fit, so we alloc the region below the target

    struct region *new_region = kmalloc(sizeof(struct region));
    if (new_region == NULL) {
        return NULL;
    }

    new_region->vbase = free_space_end - npages * PAGE_SIZE; // vbase is inclusive
    new_region->npages = npages;
    new_region->vtop = free_space_end; // vtop is exclusive
    new_region->next = NULL;
    new_region->prev = NULL;
    new_region->readable = readable;
    new_region->writeable = writeable;
    new_region->executable = executable;
    new_region->type = FILE_REGION;

    // insert the new region, before the target
    // | --- heap --- | --- new region --- | --- target --- | --- stack --- |
    new_region->next = target;
    new_region->prev = target->prev;
    target->prev->next = new_region;
    target->prev = new_region;

    return new_region;
}
#endif

static int
region_identical(struct region *r1, struct region *r2) {
    if (r1 == NULL && r2 == NULL) {
        return 1;
    }

    if (r1 == NULL || r2 == NULL) {
        return 0;
    }

    if (r1->vbase != r2->vbase || r1->npages != r2->npages || r1->vtop != r2->vtop || r1->readable != r2->readable || r1->writeable != r2->writeable || r1->executable != r2->executable) {
        return 0;
    }

    return region_identical(r1->next, r2->next);
}

static int
regions_identical(Regions *r1, Regions *r2) {
    return region_identical(r1->head, r2->head);
}

// sort the regions by vbase
static void
sort_region(struct region *region) {
    struct region *current = region;
    struct region *next = NULL;
    while (current != NULL) {
        next = current->next;
        while (next != NULL) {
            if (current->vbase > next->vbase) {
                // swap
                vaddr_t vbase = current->vbase;
                vaddr_t vtop = current->vtop;
                size_t npages = current->npages;
                int readable = current->readable;
                int writeable = current->writeable;
                int executable = current->executable;

                current->vbase = next->vbase;
                current->vtop = next->vtop;
                current->npages = next->npages;
                current->readable = next->readable;
                current->writeable = next->writeable;
                current->executable = next->executable;

                next->vbase = vbase;
                next->vtop = vtop;
                next->npages = npages;
                next->readable = readable;
                next->writeable = writeable;
                next->executable = executable;
            }
            next = next->next;
        }
        current = current->next;
    }
}

static void
sort_regions(Regions *regions) {
    KASSERT(regions != NULL);
    sort_region(regions->head);
}

// given a sorted regions list, check if there is any overlap
static int
regions_have_overlap(struct region *regions) {
    struct region *current = regions;
    while (current->next != NULL) {
        if (MAX(current->vbase, current->next->vbase) < MIN(current->vtop, current->next->vtop)) {
            return 1;
        }
        current = current->next;
    }
    return 0;
}

void
flush_tlb(void) {
    int spl = splhigh();

    for (int i = 0; i < NUM_TLB; i++) {
        tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
    }

    splx(spl);
}

struct addrspace *
as_create(void) {
    struct addrspace *as;
    Regions *regions;

    as = kmalloc(sizeof(struct addrspace));
    if (as == NULL) {
        return NULL;
    }

    /*
     * Initialize as needed.
     */

    regions = kmalloc(sizeof(*regions));
    if (regions == NULL) {
        kfree(as);
        return NULL;
    }

    regions->head = NULL;
    regions->tail = NULL;
    as->page_table = page_table_init();
    as->force_readwrite = 0;
    as->heap_start = 0;
    as->stack_start = 0;
    as->all_regions = regions;

    return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret) {
    struct addrspace *newas;

    newas = as_create();
    if (newas == NULL) {
        return ENOMEM;
    }

    (void)old;

    newas->all_regions = regions_copy(old->all_regions);
    if (newas->all_regions == NULL) {
        as_destroy(newas);
        return ENOMEM;
    }
    KASSERT(regions_identical(old->all_regions, newas->all_regions));

    newas->page_table = page_table_copy(old->page_table);
    if (newas->page_table == NULL) {
        as_destroy(newas);
        return ENOMEM;
    }

    newas->force_readwrite = old->force_readwrite;

    // assign the heap and stack regions
    newas->heap_start = old->heap_start;
    newas->stack_start = old->stack_start;

    *ret = newas;
    return 0;
}

void
as_destroy(struct addrspace *as) {
    /*
     * Clean up as needed.
     */

    regions_destroy(as->all_regions);
    as->all_regions = NULL;
    page_table_destroy(as->page_table);
    as->page_table = NULL;
    as->stack_start = 0;
    as->heap_start = 0;
    kfree(as);
    as = NULL;
}

void
as_activate(void) {
    struct addrspace *as;

    as = proc_getas();
    if (as == NULL) {
        /*
         * Kernel thread without an address space; leave the
         * prior address space in place.
         */
        return;
    }

    /*
     * Flush the TLB. Or set the ASID.
     */

    flush_tlb();
}

void
as_deactivate(void) {
    /*
     * Write this. For many designs it won't need to actually do
     * anything. See proc.c for an explanation of why it (might)
     * be needed.
     */

    /*
     * Flush the TLB. Or flush the TLB for this specific ASID.
     */
    flush_tlb();
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
                 int readable, int writeable, int executable) {
    size_t npages;

    /*
     * Align the region. First, the base...
     */
    memsize += vaddr & ~(vaddr_t)PAGE_FRAME;
    vaddr &= PAGE_FRAME;

    /*
     * ...and now the length.
     */
    npages = (memsize + PAGE_SIZE - 1) / PAGE_SIZE;

    /*
     * When this function is called, it means we have a region, can be data, text, stack etc.
     * We need to create a new region and add it to the regions list of the address space.
     */

    struct region *new_region = kmalloc(sizeof(*new_region));
    if (new_region == NULL) {
        return ENOMEM;
    }

    new_region->vbase = vaddr; // vbase is inclusive
    new_region->npages = npages;
    new_region->vtop = vaddr + npages * PAGE_SIZE; // vtop is exclusive
    new_region->next = NULL;
    new_region->prev = NULL;
    new_region->readable = readable == PF_R;
    new_region->writeable = writeable == PF_W;
    new_region->executable = executable == PF_X;
    new_region->type = UNNAMED_REGION;

    regions_insert(as->all_regions, new_region);

    return 0;
}

int
as_prepare_load(struct addrspace *as) {
    /*
     * make READONLY regions READWRITE for loading purposes
     */

    KASSERT(as != NULL);
    KASSERT(as->all_regions != NULL);

    as->force_readwrite = 1;

    return 0;
}

int
as_complete_load(struct addrspace *as) {
    /*
     * enforce READONLY again
     */

    KASSERT(as != NULL);
    KASSERT(as->all_regions != NULL);
    KASSERT(as->all_regions->head != NULL);

    as->force_readwrite = 0;

    return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr) {

    (void)as;

    /* Initial user-level stack pointer */
    *stackptr = USERSTACK;

    // given that we are defining stack, we can also define heap here as well
    // Heap region is read/write and not executable
    // we find the top of the top most region and add heap region after that and just below the stack

    struct region *current = as->all_regions->head;

    struct region *topmost = current;
    while (current != NULL) {
        if (current->vtop > topmost->vtop) {
            topmost = current;
        }
        current = current->next;
    }

    vaddr_t heap_base = topmost->vtop;
    // we allocate 1 page for heap just for easy management

    as_define_region(as, heap_base, 1 * PAGE_SIZE, PF_R, PF_W, 0);

    // ELF does not contain a stack region because initially stack is empty
    // and it grows downwards
    // We need to define the stack region here

    // Stack region is read/write and not executable
    as_define_region(as, USERSTACK - YANG_VM_STACKPAGES * PAGE_SIZE, YANG_VM_STACKPAGES * PAGE_SIZE, PF_R, PF_W, 0);

    sort_regions(as->all_regions);
    // check that there is no overlap
    KASSERT(!regions_have_overlap(as->all_regions->head));

    // now keep a reference to the heap region for convenience
    as->heap_start = heap_base;
    struct region *heap = find_region_by_vbase(as->all_regions, heap_base);
    heap->type = HEAP_REGION;
    KASSERT(heap != NULL);
    KASSERT(heap->vbase == as->heap_start);

    // keep a reference to the stack region for convenience
    as->stack_start = USERSTACK - YANG_VM_STACKPAGES * PAGE_SIZE;
    struct region *stack = find_region_by_vbase(as->all_regions, USERSTACK - YANG_VM_STACKPAGES * PAGE_SIZE);
    stack->type = STACK_REGION;
    KASSERT(stack != NULL);
    KASSERT(stack->vbase == as->stack_start);

    return 0;
}
