#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <spl.h>
#include <proc.h>
#include <synch.h>

/* Place your page table functions here */

static PTE *
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
    pte->ref_count = 1;
    pte->lock = lock_create("PTE lock");
    if (pte->lock == NULL) {
        kfree(pte);
        return NULL;
    }

    return pte;
}

/*
 * Copy the given page that handles READONLY
 */
static PTE *
pte_copy_on_write(PTE *pte) {
    if (pte == NULL) {
        return NULL;
    }
    if (pte->ref_count == 1) {
        // we can just mark this page as writeable
        pte->frame |= TLBLO_DIRTY;
        return pte;
    }

    PTE *new = new_pte();
    if (new == NULL) {
        return NULL;
    }

    // copy the contents of the old frame to the new frame
    memcpy((void *)(PADDR_TO_KVADDR(new->frame) & PAGE_FRAME), (void *)(PADDR_TO_KVADDR(pte->frame) & PAGE_FRAME), PAGE_SIZE);

    // copy offset bits
    new->frame |= pte->frame & ~PAGE_FRAME;

    // mark it as writeable
    new->frame |= TLBLO_DIRTY;

    // now we need to update the old page reference count
    pte_dec_ref(pte);

    return new;
}

static PTE *
page_table_lookup(PageTable *page_table, vaddr_t vaddr) {
    int l1_index = L1_INDEX(vaddr);
    int l2_index = L2_INDEX(vaddr);

    if (page_table->tables[l1_index] == NULL) {
        return NULL;
    }

    PTE *entry = page_table->tables[l1_index]->entries[l2_index];
    return entry;
}

static int
page_table_add_entry(PageTable *page_table, vaddr_t vaddr, PTE *pte) {

    KASSERT(pte != NULL);

    int l1_index = L1_INDEX(vaddr);
    int l2_index = L2_INDEX(vaddr);

    if (page_table->tables[l1_index] == NULL) {
        page_table->tables[l1_index] = kmalloc(sizeof(L2Table));
        if (page_table->tables[l1_index] == NULL) {
            return ENOMEM;
        }
        for (int i = 0; i < 1 << L2_BITS; i++) {
            page_table->tables[l1_index]->entries[i] = NULL;
        }
    }
    // Potentially we can have a reference here to some PTE in other page tables
    // so here we might replace it with a real page with ref count 1
    page_table->tables[l1_index]->entries[l2_index] = pte;

    return 0;
}

static void
load_tlb(vaddr_t vaddr, paddr_t paddr, bool force_rw) {
    uint32_t ehi, elo;
    int spl;

    spl = splhigh();

    if (force_rw) {
        paddr |= TLBLO_DIRTY;
    }

    ehi = vaddr & TLBHI_VPAGE;
    elo = paddr | TLBLO_VALID;

    int result = tlb_probe(ehi, 0);
    if (result >= 0) {
        tlb_write(ehi, elo, result);
        splx(spl);
        return;
    }

    tlb_random(ehi, elo);

    splx(spl);
}

static struct region *
find_region(struct addrspace *as, vaddr_t vaddr) {
    struct region *current_region = as->regions;
    while (current_region != NULL) {
        if (current_region->vbase <= vaddr && vaddr < current_region->vtop) {
            return current_region;
        }
        current_region = current_region->next;
    }
    return NULL;
}

void
vm_bootstrap(void) {
    /* Initialise any global components of your VM sub-system here.
     *
     * You may or may not need to add anything here depending what's
     * provided or required by the assignment spec.
     */
}

int
vm_fault(int faulttype, vaddr_t faultaddress) {
    (void)faulttype;
    (void)faultaddress;

    /*
     * Check fault type
     */
    switch (faulttype) {
    case VM_FAULT_READ:
        break;
    case VM_FAULT_WRITE:
        break;
    case VM_FAULT_READONLY:
        // we need to figure out if we need to do copy on write
        // so first thing we need to check is if we already have a page table entry
        // if we do, we need to check the reference count
        // if the reference count is greater than 1, we need to copy the page
        // and update the page table
        break;
    default:
        return EINVAL;
    }

    /*
     * Get the current address space
     */

    struct addrspace *as;
    as = proc_getas();
    if (as == NULL) {
        return EFAULT;
    }

    PageTable *pt = as->page_table;
    if (pt == NULL) {
        return EFAULT;
    }

    /* Find the page table entry */
    PTE *pte = page_table_lookup(pt, faultaddress);

    /*
     * If this is a valid translation, we need to check if we need to do copy on write
     */
    if (pte) {
        paddr_t paddr = pte->frame;

        if (faulttype == VM_FAULT_READONLY) {

            struct region *current_region = find_region(as, faultaddress);

            if (!current_region->writeable) {
                // this falls under a readonly region, are you are trying to write to it?
                return EFAULT;
            }

            // now we know it is made readonly for copy on write

            PTE *new_entry = pte_copy_on_write(pte);
            KASSERT(new_entry != NULL);
            KASSERT(new_entry->ref_count == 1);
            paddr = new_entry->frame;
            int result = page_table_add_entry(pt, faultaddress, new_entry);
            if (result) {
                return result;
            }
        }

        load_tlb(faultaddress, paddr, as->force_readwrite);
        return 0;
    }

    /*
     * Otherwise we need to check if this is a valid translation, we need to look up in regions
     */

    struct region *current_region = find_region(as, faultaddress);

    if (current_region == NULL) {
        /*
         * Not found in regions, this is an invalid address
         */
        return EFAULT;
    }

    /* now check if we receive a read fault on a non-readable page */
    if (!current_region->readable && faulttype == VM_FAULT_READ) {
        return EFAULT;
    }

    /* now check if we receive a write fault on a read-only page, with forced write off */
    if (!current_region->writeable && !as->force_readwrite && faulttype == VM_FAULT_WRITE) {
        return EFAULT;
    }

    /*
     * At this point we know this is in a valid region, we need to allocate a page and add it to the page table
     */
    PTE *new_entry = new_pte();

    // modify the paddr to include control bits
    paddr_t paddr = new_entry->frame;

    paddr |= TLBLO_VALID;

    // find out if the region is writeable
    if (current_region->writeable) {
        paddr |= TLBLO_DIRTY;
    }
    // commit the changes
    new_entry->frame = paddr;

    // Add the new page table entry to the page table
    int result = page_table_add_entry(pt, faultaddress, new_entry);
    if (result) {
        return result;
    }

    /*
     * Now we can load the TLB
     */

    load_tlb(faultaddress, new_entry->frame, as->force_readwrite);

    return 0;
}

/*
 * SMP-specific functions.  Unused in our UNSW configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts) {
    (void)ts;
    panic("vm tried to do tlb shootdown?!\n");
}
