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

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static PageTable *
page_table_init(void) {
    PageTable *page_table = kmalloc(sizeof(PageTable));
    if (page_table == NULL) {
        return NULL;
    }

    for (int i = 0; i < 1 << L1_BITS; i++) {
        page_table->tables[i] = NULL;
    }

    return page_table;
}

static void
page_table_destroy(PageTable *page_table) {
    for (int i = 0; i < 1 << L1_BITS; i++) {
        if (page_table->tables[i] != NULL) {
            for (int j = 0; j < 1 << L2_BITS; j++) {
                if (page_table->tables[i]->entries[j] != NULL) {
                    // Free the frame
                    paddr_t frame = page_table->tables[i]->entries[j]->frame;
                    vaddr_t page = PADDR_TO_KVADDR(frame & PAGE_FRAME);
                    free_kpages(page);
                    kfree(page_table->tables[i]->entries[j]);
                    page_table->tables[i]->entries[j] = NULL;
                }
            }
            kfree(page_table->tables[i]);
            page_table->tables[i] = NULL;
        }
    }
    kfree(page_table);
}

// for fork, copy page table and alloc new frames
static PageTable *
page_table_copy(PageTable *old) {
    PageTable *new = page_table_init();
    if (new == NULL) {
        return NULL;
    }

    for (int i = 0; i < 1 << L1_BITS; i++) {
        if (old->tables[i] != NULL) {
            new->tables[i] = kmalloc(sizeof(*new->tables[i]));
            if (new->tables[i] == NULL) {
                page_table_destroy(new);
                return NULL;
            }

            for (int j = 0; j < 1 << L2_BITS; j++) {
                if (old->tables[i]->entries[j] != NULL) {
                    new->tables[i]->entries[j] = kmalloc(sizeof(*new->tables[i]->entries[j]));
                    if (new->tables[i]->entries[j] == NULL) {
                        page_table_destroy(new);
                        return NULL;
                    }

                    vaddr_t new_page = alloc_kpages(1);
                    if (new_page == 0) {
                        page_table_destroy(new);
                        return NULL;
                    }
                    // copy the contents of the old frame to the new frame
                    memcpy((void *)new_page, (void *)PADDR_TO_KVADDR(old->tables[i]->entries[j]->frame & PAGE_FRAME), PAGE_SIZE);

                    // we have the offset of the old frame, we need to copy offset bits to combine with the new frame address bits
                    paddr_t new_paddr = KVADDR_TO_PADDR(new_page);
                    paddr_t old_paddr = old->tables[i]->entries[j]->frame;

                    // copy offset bits
                    new->tables[i]->entries[j]->frame = new_paddr | (old_paddr & (~PAGE_FRAME));

                } else {
                    new->tables[i]->entries[j] = NULL;
                }
            }
        } else {
            new->tables[i] = NULL;
        }
    }

    return new;
}

static int
page_table_identical(PageTable *pt1, PageTable *pt2) {
    for (int i = 0; i < 1 << L1_BITS; i++) {
        if (pt1->tables[i] == NULL && pt2->tables[i] == NULL) {
            continue;
        }

        if (pt1->tables[i] == NULL || pt2->tables[i] == NULL) {
            return 0;
        }

        for (int j = 0; j < 1 << L2_BITS; j++) {
            if (pt1->tables[i]->entries[j] == NULL && pt2->tables[i]->entries[j] == NULL) {
                continue;
            }

            if (pt1->tables[i]->entries[j] == NULL || pt2->tables[i]->entries[j] == NULL) {
                return 0;
            }

            if (pt1->tables[i]->entries[j]->frame != pt2->tables[i]->entries[j]->frame) {
                return 0;
            }
        }
    }

    return 1;
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
    KASSERT(!page_table_identical(old->page_table, newas->page_table));

    newas->force_readwrite = old->force_readwrite;

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

    // ELF does not contain a stack region because initially stack is empty
    // and it grows downwards
    // We need to define the stack region here

    // Stack region is read/write and not executable
    as_define_region(as, USERSTACK - YANG_VM_STACKPAGES * PAGE_SIZE, YANG_VM_STACKPAGES * PAGE_SIZE, PF_R, PF_W, 0);

    return 0;
}
