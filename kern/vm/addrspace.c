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

PTE *
page_table_lookup(PageTable *page_table, vaddr_t vaddr) {

    KASSERT(page_table != NULL);
    KASSERT(page_table->lock != NULL);

    int l1_index = L1_INDEX(vaddr);
    int l2_index = L2_INDEX(vaddr);

    lock_acquire(page_table->lock);

    if (page_table->tables[l1_index] == NULL) {
        lock_release(page_table->lock);
        return NULL;
    }

    PTE *entry = page_table->tables[l1_index]->entries[l2_index];

    lock_release(page_table->lock);
    return entry;
}

int
page_table_add_entry(PageTable *page_table, vaddr_t vaddr, PTE *pte) {

    KASSERT(pte != NULL);
    KASSERT(page_table != NULL);
    KASSERT(page_table->lock != NULL);

    int l1_index = L1_INDEX(vaddr);
    int l2_index = L2_INDEX(vaddr);

    lock_acquire(page_table->lock);

    if (page_table->tables[l1_index] == NULL) {
        page_table->tables[l1_index] = kmalloc(sizeof(L2Table));
        if (page_table->tables[l1_index] == NULL) {
            lock_release(page_table->lock);
            return ENOMEM;
        }
        for (int i = 0; i < 1 << L2_BITS; i++) {
            page_table->tables[l1_index]->entries[i] = NULL;
        }
    }
    // Potentially we can have a reference here to some PTE in other page tables
    // so here we might replace it with a real page with ref count 1
    page_table->tables[l1_index]->entries[l2_index] = pte;

    lock_release(page_table->lock);

    return 0;
}

PTE *
page_table_remove_entry(PageTable *page_table, vaddr_t vaddr) {
    int l1_index = L1_INDEX(vaddr);
    int l2_index = L2_INDEX(vaddr);

    lock_acquire(page_table->lock);

    if (page_table->tables[l1_index] == NULL) {
        lock_release(page_table->lock);
        return NULL;
    }

    PTE *entry = page_table->tables[l1_index]->entries[l2_index];
    page_table->tables[l1_index]->entries[l2_index] = NULL;

    lock_release(page_table->lock);

    return entry;
}

void
pte_destroy(PTE *pte) {
    KASSERT(pte != NULL);
    KASSERT(pte->lock != NULL);

#if OPT_COW
    KASSERT(pte->ref_count == 1);
#endif
    // keep a reference to the lock
    struct lock *lock = pte->lock;
    lock_acquire(lock);
    paddr_t frame = pte->frame;
    vaddr_t page = PADDR_TO_KVADDR(frame & PAGE_FRAME);
    free_kpages(page);
    pte->frame = 0;
#if OPT_COW
    pte->ref_count = 0;
#endif
    pte->lock = NULL;
    kfree(pte);
    lock_release(lock);
    lock_destroy(lock);
}

#if OPT_COW
void
pte_dec_ref(PTE *pte) {
    KASSERT(pte != NULL);
    KASSERT(pte->lock != NULL);
    lock_acquire(pte->lock);
    KASSERT(pte->ref_count > 0);
    if (pte->ref_count > 1) {
        pte->ref_count--;
    }

    // if ref_count is 1, we can free the frame
    if (pte->ref_count == 1) {
        lock_release(pte->lock);
        pte_destroy(pte);
        return;
    }

    lock_release(pte->lock);
}
void
pte_inc_ref(PTE *pte) {
    KASSERT(pte != NULL);
    KASSERT(pte->lock != NULL);
    lock_acquire(pte->lock);
    KASSERT(pte->ref_count > 0);
    pte->ref_count++;
    pte->frame &= ~TLBLO_DIRTY; // mark the page as unwriteable, so it triggers a page fault on write
    lock_release(pte->lock);
}
#endif

PTE *
new_pte() {
    PTE *pte = kmalloc(sizeof(*pte));
    if (pte == NULL) {
        return NULL;
    }

    vaddr_t vaddr = alloc_kpages(1);
    if (vaddr == 0) {
        return NULL;
    }

    // Zero fill the page
    bzero((void *)vaddr, PAGE_SIZE);

    paddr_t paddr = KVADDR_TO_PADDR(vaddr);
    KASSERT((paddr & PAGE_FRAME) == paddr);

    pte->frame = paddr;
    pte->lock = lock_create("PTE lock");
    if (pte->lock == NULL) {
        kfree(pte);
        return NULL;
    }

#if OPT_COW
    pte->ref_count = 1;
#endif

    return pte;
}

/*
 * Copy the given page that handles READONLY
 */
PTE *
pte_copy(PTE *pte) {
    KASSERT(pte != NULL);
    KASSERT(pte->lock != NULL);

    lock_acquire(pte->lock);

#if OPT_COW
    if (pte->ref_count == 1) {
        // we can just mark this page as writeable
        pte->frame |= TLBLO_DIRTY;
        lock_release(pte->lock);
        return pte;
    }
#endif

    PTE *new = new_pte();
    if (new == NULL) {
        lock_release(pte->lock);
        return NULL;
    }

    // copy the contents of the old frame to the new frame
    memmove((void *)(PADDR_TO_KVADDR(new->frame) & PAGE_FRAME), (void *)(PADDR_TO_KVADDR(pte->frame) & PAGE_FRAME), PAGE_SIZE);

    // copy offset bits
    new->frame |= pte->frame & ~PAGE_FRAME;

    // mark it as writeable
    new->frame |= TLBLO_DIRTY;

#if OPT_COW
    // now we need to update the old page reference count
    pte->ref_count--;

    KASSERT(pte->ref_count > 0);
#endif
    lock_release(pte->lock);

    return new;
}

static PageTable *
page_table_init(void) {
    PageTable *page_table = kmalloc(sizeof(PageTable));
    if (page_table == NULL) {
        return NULL;
    }

    for (int i = 0; i < 1 << L1_BITS; i++) {
        page_table->tables[i] = NULL;
    }

    struct lock *lock = lock_create("Page table lock");
    if (lock == NULL) {
        kfree(page_table);
        return NULL;
    }

    page_table->lock = lock;

    return page_table;
}

static void
page_table_destroy(PageTable *page_table) {

    KASSERT(page_table != NULL);

    // keep a reference to the lock
    struct lock *lock = page_table->lock;
    lock_acquire(lock);

    for (int i = 0; i < 1 << L1_BITS; i++) {
        if (page_table->tables[i] != NULL) {
            for (int j = 0; j < 1 << L2_BITS; j++) {
                if (page_table->tables[i]->entries[j] != NULL) {
#if OPT_COW
                    // decrement ref_count and free the frame if ref_count is 1
                    pte_dec_ref(page_table->tables[i]->entries[j]);
#else
                    pte_destroy(page_table->tables[i]->entries[j]);
#endif
                }
            }
            kfree(page_table->tables[i]);
            page_table->tables[i] = NULL;
        }
    }
    page_table->lock = NULL;
    kfree(page_table);
    lock_release(lock);
    lock_destroy(lock);
}

// for fork, copy page table and alloc new frames
static PageTable *
page_table_copy(PageTable *old) {
    PageTable *new = page_table_init();
    if (new == NULL) {
        return NULL;
    }

    lock_acquire(old->lock);
    for (int i = 0; i < 1 << L1_BITS; i++) {
        if (old->tables[i] != NULL) {
            new->tables[i] = kmalloc(sizeof(*new->tables[i]));
            if (new->tables[i] == NULL) {
                lock_release(old->lock);
                page_table_destroy(new);
                return NULL;
            }

            for (int j = 0; j < 1 << L2_BITS; j++) {
                if (old->tables[i]->entries[j] != NULL) {
#if OPT_COW
                    // only copy reference here, we have copy-on-write in vm
                    pte_inc_ref(old->tables[i]->entries[j]);
                    new->tables[i]->entries[j] = old->tables[i]->entries[j];
#else
                    new->tables[i]->entries[j] = pte_copy(old->tables[i]->entries[j]);
                    if (new->tables[i]->entries[j] == NULL) {
                        lock_release(old->lock);
                        page_table_destroy(new);
                        return NULL;
                    }
#endif
                } else {
                    new->tables[i]->entries[j] = NULL;
                }
            }
        }
    }
    lock_release(old->lock);

    return new;
}

static struct region *
regions_copy(struct region *old) {
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
        new->next = regions_copy(old->next);
        if (new->next == NULL) {
            kfree(new);
            return NULL;
        }
    } else {
        new->next = NULL;
    }

    return new;
}

static int
regions_identical(struct region *r1, struct region *r2) {
    if (r1 == NULL && r2 == NULL) {
        return 1;
    }

    if (r1 == NULL || r2 == NULL) {
        return 0;
    }

    if (r1->vbase != r2->vbase || r1->npages != r2->npages || r1->vtop != r2->vtop || r1->readable != r2->readable || r1->writeable != r2->writeable || r1->executable != r2->executable) {
        return 0;
    }

    return regions_identical(r1->next, r2->next);
}

static void
free_region(struct region *region) {
    if (region->next != NULL) {
        free_region(region->next);
    }
    kfree(region);
}

static void
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

    as = kmalloc(sizeof(struct addrspace));
    if (as == NULL) {
        return NULL;
    }

    /*
     * Initialize as needed.
     */

    as->regions = NULL;
    as->page_table = page_table_init();
    as->force_readwrite = 0;
    as->heap = NULL;
    as->stack = NULL;

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

    newas->regions = regions_copy(old->regions);
    if (newas->regions == NULL) {
        as_destroy(newas);
        return ENOMEM;
    }
    KASSERT(regions_identical(old->regions, newas->regions));

    newas->page_table = page_table_copy(old->page_table);
    if (newas->page_table == NULL) {
        as_destroy(newas);
        return ENOMEM;
    }

    newas->force_readwrite = old->force_readwrite;

#if OPT_SBRK
    // assign the heap and stack regions
    // stack is the last region and heap is the second last region
    struct region *current = newas->regions;
    while (current->next != NULL) {
        current = current->next;
    }
    newas->stack = current;
    current = newas->regions;
    while (current->next != newas->stack) {
        current = current->next;
    }
    newas->heap = current;

#endif

    *ret = newas;
    return 0;
}

void
as_destroy(struct addrspace *as) {
    /*
     * Clean up as needed.
     */

    free_region(as->regions);
    as->regions = NULL;
    page_table_destroy(as->page_table);
    as->page_table = NULL;
    as->stack = NULL;
    as->heap = NULL;
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

    struct region *new_region = kmalloc(sizeof(struct region));
    if (new_region == NULL) {
        return ENOMEM;
    }

    new_region->vbase = vaddr; // vbase is inclusive
    new_region->npages = npages;
    new_region->vtop = vaddr + npages * PAGE_SIZE; // vtop is exclusive
    new_region->next = NULL;
    new_region->readable = readable == PF_R;
    new_region->writeable = writeable == PF_W;
    new_region->executable = executable == PF_X;

    if (as->regions == NULL) {
        as->regions = new_region;
    } else {
        struct region *current = as->regions;
        while (current->next != NULL) {
            // meanwhile we would like to sanity check if the regions overlap
            if (MAX(current->vbase, new_region->vbase) < MIN(current->vtop, new_region->vtop)) {
                // regions overlap, bad ELF region definitions
                kfree(new_region);
                return EINVAL;
            }
            current = current->next;
        }
        current->next = new_region;
    }

    return 0;
}

int
as_prepare_load(struct addrspace *as) {
    /*
     * make READONLY regions READWRITE for loading purposes
     */

    KASSERT(as != NULL);
    KASSERT(as->regions != NULL);

    as->force_readwrite = 1;

    return 0;
}

int
as_complete_load(struct addrspace *as) {
    /*
     * enforce READONLY again
     */

    KASSERT(as != NULL);
    KASSERT(as->regions != NULL);

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

    struct region *current = as->regions;

    struct region *topmost = current;
    while (current != NULL) {
        if (current->vtop > topmost->vtop) {
            topmost = current;
        }
        current = current->next;
    }

    vaddr_t heap_base = topmost->vtop;
    // heap initially does not have any pages

    as_define_region(as, heap_base, 0, PF_R, PF_W, 0);

    // now keep a reference to the heap region for convenience, it should be the last item in the linked list
    current = as->regions;
    while (current->next != NULL) {
        current = current->next;
    }
    as->heap = current;
    KASSERT(as->heap->vbase == heap_base);
    KASSERT(as->heap->npages == 0);

    // ELF does not contain a stack region because initially stack is empty
    // and it grows downwards
    // We need to define the stack region here

    // Stack region is read/write and not executable
    as_define_region(as, USERSTACK - YANG_VM_STACKPAGES * PAGE_SIZE, YANG_VM_STACKPAGES * PAGE_SIZE, PF_R, PF_W, 0);
    // keep a reference to the stack region for convenience
    current = as->regions;
    while (current->next != NULL) {
        current = current->next;
    }
    as->stack = current;
    KASSERT(as->stack->vbase == USERSTACK - YANG_VM_STACKPAGES * PAGE_SIZE);
    KASSERT(as->stack->npages == YANG_VM_STACKPAGES);

    return 0;
}
