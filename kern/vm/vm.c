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
#include <vfs.h>
#include <vnode.h>
#include <openfile.h>
#include <current.h>
#include <filetable.h>
#include <uio.h>

/* Place your page table functions here */

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

#if OPT_COW
        // we need to figure out if we need to do copy on write
        // so first thing we need to check is if we already have a page table entry
        // if we do, we need to check the reference count
        // if the reference count is greater than 1, we need to copy the page
        // and update the page table
        break;
#else
        return EFAULT;
#endif

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

    /*
     * we need to look up this address in regions
     */

    struct region *current_region = find_region(as->all_regions, faultaddress);

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

    if (faulttype == VM_FAULT_READONLY && !current_region->writeable && !as->force_readwrite) {
        // this falls under a readonly region, and you are trying to write to it?
        return EFAULT;
    }

    /* Find the page table entry */
    PTE *pte = page_table_lookup(pt, faultaddress);

    /*
     * If this is a valid translation, we need to check if we need to do copy on write
     */
    if (pte) {
        paddr_t paddr = pte->frame;
#if OPT_COW
        if (faulttype == VM_FAULT_READONLY) {

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
#endif
        load_tlb(faultaddress, paddr, as->force_readwrite);
        return 0;
    }

    /*
     * At this point we know this is in a valid region, we need to allocate a page and add it to the page table
     */
    PTE *new_entry = new_pte();

    if (new_entry == NULL) {
        return ENOMEM;
    }

    // modify the paddr to include control bits
    paddr_t paddr = new_entry->frame;

    paddr |= TLBLO_VALID;

    // find out if the region is writeable
    if (current_region->writeable) {
        paddr |= TLBLO_DIRTY;
    }
    // commit the changes
    new_entry->frame = paddr;

    switch (current_region->type) {
    case UNNAMED_REGION:
    case HEAP_REGION:
    case FILE_REGION:
#if OPT_COW
        new_entry->shared = 1;
#endif
        break;
    case STACK_REGION:
#if OPT_COW
        new_entry->shared = 0;
#endif
        break;
    default:
        // unimplemented
        return ENOSYS;
    }

#if OPT_MMAP
    if (current_region->type == FILE_REGION) {
        // if we reach here, it means that the page table does not have a pte
        // and we are in a file region. If this is a read fault, we need to read from the file
        // for write fault, we need to write to the file and make sure the page is also updated in the page table

        // read the file
        int fd = current_region->fd;
        off_t offset = current_region->offset;

        // get the open file from the file table
        struct openfile *file;
        int result = filetable_get(curproc->p_filetable, fd, &file);
        if (result) {
            return result;
        }
        // make sure the file is open
        if (file == NULL) {
            return EBADF;
        }
        // load the 4kb range requested by this fault address from this file using uio
        struct iovec iov;
        struct uio u;

        // now figure out this is a read or write fault
        if (faulttype == VM_FAULT_READ) {
            uio_kinit(&iov, &u, (void *)PADDR_TO_KVADDR(paddr), PAGE_SIZE, offset, UIO_READ);
            result = VOP_READ(file->of_vnode, &u);
        } else if (faulttype == VM_FAULT_WRITE) {
            // TODO: how to handle write fault in a file region?
            uio_kinit(&iov, &u, (void *)PADDR_TO_KVADDR(paddr), PAGE_SIZE, offset, UIO_WRITE);
            result = VOP_WRITE(file->of_vnode, &u);
        }

        if (result) {
            return result;
        }
    }

#endif

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
