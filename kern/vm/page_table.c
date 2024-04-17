
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
    } else {
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

#if OPT_COW
/*
 * Copy the given page that handles READONLY
 */
PTE *
pte_copy_on_write(PTE *pte) {
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
#endif
PTE *
pte_copy(PTE *pte) {
    KASSERT(pte != NULL);
    KASSERT(pte->lock != NULL);

    lock_acquire(pte->lock);

    PTE *new = new_pte();
    if (new == NULL) {
        lock_release(pte->lock);
        return NULL;
    }

    // copy the contents of the old frame to the new frame
    memmove((void *)(PADDR_TO_KVADDR(new->frame) & PAGE_FRAME), (void *)(PADDR_TO_KVADDR(pte->frame) & PAGE_FRAME), PAGE_SIZE);

    // copy offset bits
    new->frame |= pte->frame & ~PAGE_FRAME;

    lock_release(pte->lock);

    return new;
}

PageTable *
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

void
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
PageTable *
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
                    PTE *existing = old->tables[i]->entries[j];
                    if (existing->shared) {
                        pte_inc_ref(old->tables[i]->entries[j]);
                        new->tables[i]->entries[j] = old->tables[i]->entries[j];
                    } else {
                        PTE *new_entry = pte_copy(old->tables[i]->entries[j]);
                        if (new_entry == NULL) {
                            lock_release(old->lock);
                            page_table_destroy(new);
                            return NULL;
                        }
                        new->tables[i]->entries[j] = new_entry;
                    }
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
