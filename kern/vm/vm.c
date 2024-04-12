#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <spl.h>
#include <proc.h>

/* Place your page table functions here */

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
page_table_add_entry(PageTable *page_table, vaddr_t vaddr, paddr_t paddr) {

    PTE *pte = kmalloc(sizeof(*pte));
    if (pte == NULL) {
        return ENOMEM;
    }

    pte->frame = paddr;

    int l1_index = L1_INDEX(vaddr);
    int l2_index = L2_INDEX(vaddr);

    if (page_table->tables[l1_index] == NULL) {
        page_table->tables[l1_index] = kmalloc(sizeof(L2Table));
        if (page_table->tables[l1_index] == NULL) {
            kfree(pte);
            return ENOMEM;
        }
        for (int i = 0; i < 1 << L2_BITS; i++) {
            page_table->tables[l1_index]->entries[i] = NULL;
        }
    }
    // TODO: Can we have a collision here?
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
        return EFAULT;
    default:
        return EINVAL;
    }

    /*
     * At this point, we know that the fault was a read or write fault.
     * Handle this fault. Look up page table first
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
     * If this is a valid translation, we can load TLB
     */
    if (pte) {
        paddr_t paddr = pte->frame;
        load_tlb(faultaddress, paddr, as->force_readwrite);
        return 0;
    }

    /*
     * Otherwise we need to check if this is a valid translation, we need to look up in regions
     */

    struct region *current_region = as->regions;
    while (current_region != NULL) {
        if (current_region->vbase <= faultaddress && faultaddress < current_region->vtop) {
            break;
        }
        current_region = current_region->next;
    }
    if (current_region == NULL) {
        /*
         * Not found in regions, this is an invalid address
         */
        return EFAULT;
    }

    /*
     * At this point we know this is in a valid region, we need to allocate a page and add it to the page table
     */
    vaddr_t vaddr = alloc_kpages(1);
    if (vaddr == 0) {
        return ENOMEM;
    }

    // Zero fill the page
    bzero((void *)vaddr, PAGE_SIZE);

    /*
     * Now add this to the page table
     */

    // First, we need to get the physical address

    paddr_t paddr = KVADDR_TO_PADDR(vaddr);

    paddr |= TLBLO_VALID;

    // find out if the region is writeable
    if (current_region->writeable) {
        paddr |= TLBLO_DIRTY;
    }

    // Add the new page table entry to the page table
    int result = page_table_add_entry(pt, faultaddress, paddr);
    if (result) {
        return result;
    }

    /*
     * Now we can load the TLB
     */

    load_tlb(faultaddress, paddr, as->force_readwrite);

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
