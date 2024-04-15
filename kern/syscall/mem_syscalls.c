
#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/seek.h>
#include <kern/stat.h>
#include <lib.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <synch.h>
#include <copyinout.h>
#include <vfs.h>
#include <vnode.h>
#include <openfile.h>
#include <filetable.h>
#include <syscall.h>
#include <addrspace.h>
#include <mips/tlb.h>
/*
 * sbrk()
 * If successful, sbrk() returns the previous break value.
 * If unsuccessful, sbrk() returns -1 and sets errno to one of the following values:
 */

int
sys_sbrk(ssize_t amount, vaddr_t *retval) {

#if OPT_SBRK
    struct addrspace *as;
    vaddr_t old_heap_end, new_heap_end;

    as = curproc->p_addrspace;
    if (as == NULL) {
        return ENOMEM;
    }

    // sbrk(0) should return end of heap
    if (amount == 0) {
        *retval = as->heap->vtop;
        return 0;
    }

    // Calculate the new heap end, based on weather we are growing or shrinking, round up or down to the nearest page
    old_heap_end = as->heap->vtop;
    // make sure original is page aligned
    KASSERT(old_heap_end % PAGE_SIZE == 0);
    new_heap_end = old_heap_end + amount;
    KASSERT(new_heap_end != old_heap_end);
    if (amount > 0) {
        // round up
        new_heap_end = (new_heap_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    } else {
        // round down
        new_heap_end = new_heap_end & ~(PAGE_SIZE - 1);
    }

    // make sure we did the right rounding
    KASSERT((new_heap_end - old_heap_end) / PAGE_SIZE * PAGE_SIZE == new_heap_end - old_heap_end);

    // make sure the new heap end does not go below the heap start
    if (new_heap_end < as->heap->vbase) {
        return ENOMEM;
    }

    // make sure heap end does not grow into the stack
    if (new_heap_end >= as->stack->vbase) {
        return ENOMEM;
    }

    // // now we have checked range
    // // we need to check if we need to grow or shrink the heap
    // if (amount > 0) {
    //     // grow the heap
    //     struct page_table *page_table = as->page_table;
    //     vaddr_t vaddr = as->heap->vtop;
    //     vaddr_t vaddr_end = new_heap_end;
    //     while (vaddr < vaddr_end) {
    //         struct page_table_entry *pte = page_table_lookup(page_table, vaddr);
    //         if (pte == NULL) {
    //             pte = new_pte();
    //             pte->frame |= TLBLO_DIRTY;
    //             // TODO: COW
    //             page_table_add_entry(page_table, vaddr, pte);
    //         }
    //         vaddr += PAGE_SIZE;
    //     }
    // } else {
    //     // shrink the heap
    //     struct page_table *page_table = as->page_table;
    //     vaddr_t vaddr = new_heap_end;
    //     vaddr_t vaddr_end = as->heap->vtop;
    //     while (vaddr < vaddr_end) {
    //         struct page_table_entry *pte = page_table_lookup(page_table, vaddr);
    //         if (pte != NULL) {
    //             // TODO: COW
    //             PTE *removed = page_table_remove_entry(page_table, vaddr);
    //             KASSERT(removed != NULL);
    //             pte_destroy(removed);
    //         }
    //         vaddr += PAGE_SIZE;
    //     }
    // }

    as->heap->vtop = new_heap_end;
    as->heap->npages = (new_heap_end - as->heap->vbase) / PAGE_SIZE;
    // npages should be evenly divisible by PAGE_SIZE
    KASSERT(as->heap->npages * PAGE_SIZE == new_heap_end - as->heap->vbase);
    *retval = old_heap_end;

    return 0;
#else
    (void)amount;
    (void)retval;
    return ENOSYS;
#endif
}