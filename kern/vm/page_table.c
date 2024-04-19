
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

void
pte_destroy(PTE *pte) {
    KASSERT(pte != NULL);
    KASSERT(pte->lock != NULL);

#if OPT_COW & !OPT_MMAP
    KASSERT(pte->ref_count == 1);
#endif
    struct lock *lock = pte->lock;
    lock_acquire(lock);
    paddr_t frame = pte->frame;
    vaddr_t page = PADDR_TO_KVADDR(frame & PAGE_FRAME);
    bzero((void *)page, PAGE_SIZE);
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

static L2Table *
l2_table_init(void) {
    L2Table *l2_table = kmalloc(sizeof(L2Table));
    if (l2_table == NULL) {
        return NULL;
    }

    l2_table->count = 0;
    for (int i = 0; i < 1 << L2_BITS; i++) {
        l2_table->entries[i] = NULL;
    }

    return l2_table;
}

static void
l2_table_destroy(L2Table *l2_table) {
    KASSERT(l2_table != NULL);

    for (int i = 0; i < 1 << L2_BITS; i++) {
        if (l2_table->entries[i] != NULL) {
#if OPT_COW
            // decrement ref_count and free the frame if ref_count is 1
            PTE *pte = l2_table->entries[i];
            KASSERT(pte->lock != NULL);
            lock_acquire(pte->lock);
            KASSERT(pte->ref_count > 0);
            if (pte->ref_count > 1) {
                pte->ref_count--;
                lock_release(pte->lock);
            } else {
                lock_release(pte->lock);
                pte_destroy(pte);
            }
#else
            pte_destroy(l2_table->entries[i]);
#endif
            l2_table->entries[i] = NULL;
            l2_table->count--;
        }
    }
    KASSERT(l2_table->count == 0);
    kfree(l2_table);
}

// L2Table copy
static L2Table *
l2_table_copy(L2Table *old) {

    KASSERT(old != NULL);

    L2Table *new = l2_table_init();
    if (new == NULL) {
        return NULL;
    }

    for (int i = 0; i < 1 << L2_BITS; i++) {
        if (old->entries[i] != NULL) {
#if OPT_COW
            PTE *existing = old->entries[i];
            if (existing->shared) {
                pte_inc_ref(old->entries[i]);
                new->entries[i] = old->entries[i];
            } else {
                PTE *new_entry = pte_copy(old->entries[i]);
                if (new_entry == NULL) {
                    l2_table_destroy(new);
                    return NULL;
                }
                new->entries[i] = new_entry;
            }
#else
            new->entries[i] = pte_copy(old->entries[i]);
            if (new->entries[i] == NULL) {
                l2_table_destroy(new);
                return NULL;
            }
#endif
            new->count++;
        }
    }

    KASSERT(new->count == old->count);
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
            l2_table_destroy(page_table->tables[i]);
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
            L2Table *l2_table = l2_table_copy(old->tables[i]);
            if (l2_table == NULL) {
                page_table_destroy(new);
                lock_release(old->lock);
                return NULL;
            }
            new->tables[i] = l2_table;
        }
    }
    lock_release(old->lock);

    return new;
}

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
        page_table->tables[l1_index] = l2_table_init();
        if (page_table->tables[l1_index] == NULL) {
            lock_release(page_table->lock);
            return ENOMEM;
        }
    }

    // Potentially here we can already have a page table entry
    // if that is the case then it means we are doing a copy on write
    if (page_table->tables[l1_index]->entries[l2_index] == NULL) {
        page_table->tables[l1_index]->count++;
    }
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
    page_table->tables[l1_index]->count--;

    if (page_table->tables[l1_index]->count == 0) {
        kfree(page_table->tables[l1_index]);
        page_table->tables[l1_index] = NULL;
    }

    lock_release(page_table->lock);

    return entry;
}
